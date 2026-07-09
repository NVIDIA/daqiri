/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cuda_runtime.h>
#include <yaml-cpp/yaml.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include <pthread.h>
#include <sched.h>

#include <daqiri/daqiri.h>

namespace {

volatile std::sig_atomic_t g_stop_requested = 0;

void signal_handler(int signum) {
  if (signum == SIGINT) {
    g_stop_requested = 1;
  }
}

struct BenchConfig {
  std::string interface_name = "fpga0";
  int tx_cpu_core = -1;
  int rx_cpu_core = -1;
  uint32_t batch_size = 32;
  uint32_t payload_size = 4096;
  uint32_t hold_rx_bursts = 4;
  bool verify = true;
  bool mixed_packet_frees = true;
};

struct BenchStats {
  uint64_t packets = 0;
  uint64_t bytes = 0;
  uint64_t bursts = 0;
  uint64_t backpressure = 0;
  uint64_t errors = 0;
};

struct HeldRxBurst {
  daqiri::BurstParams* burst = nullptr;
  std::vector<uint64_t> sequences;
};

bool set_affinity(int cpu_core, const char* name) {
  if (cpu_core < 0) {
    return true;
  }
  if (cpu_core >= CPU_SETSIZE) {
    std::cerr << name << " cpu_core is out of range: " << cpu_core << '\n';
    return false;
  }

  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(cpu_core, &cpuset);
  const int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
  if (rc != 0) {
    std::cerr << "Failed to pin " << name << " to CPU " << cpu_core << ": " << std::strerror(rc)
              << '\n';
    return false;
  }
  return true;
}

BenchConfig parse_bench_config(const YAML::Node& root) {
  const auto node = root["pcie_bench"];
  if (!node || !node.IsMap()) {
    throw std::runtime_error("config must contain a pcie_bench map");
  }

  BenchConfig cfg;
  cfg.interface_name = node["interface_name"].as<std::string>(cfg.interface_name);
  cfg.tx_cpu_core = node["tx_cpu_core"].as<int>(cfg.tx_cpu_core);
  cfg.rx_cpu_core = node["rx_cpu_core"].as<int>(cfg.rx_cpu_core);
  cfg.batch_size = node["batch_size"].as<uint32_t>(cfg.batch_size);
  cfg.payload_size = node["payload_size"].as<uint32_t>(cfg.payload_size);
  cfg.hold_rx_bursts = node["hold_rx_bursts"].as<uint32_t>(cfg.hold_rx_bursts);
  cfg.verify = node["verify"].as<bool>(cfg.verify);
  cfg.mixed_packet_frees = node["mixed_packet_frees"].as<bool>(cfg.mixed_packet_frees);
  if (cfg.batch_size == 0) {
    throw std::runtime_error("pcie_bench.batch_size must be positive");
  }
  if (cfg.payload_size < sizeof(uint64_t)) {
    throw std::runtime_error("pcie_bench.payload_size must be at least 8 bytes");
  }
  return cfg;
}

void validate_bench_geometry(const BenchConfig& bench, const daqiri::NetworkConfig& network) {
  bool found_interface = false;
  for (const auto& interface : network.ifs_) {
    if (interface.name_ != bench.interface_name) {
      continue;
    }
    found_interface = true;
    for (const auto& queue : interface.tx_.queues_) {
      if (bench.batch_size > static_cast<uint32_t>(queue.common_.batch_size_)) {
        throw std::runtime_error("pcie_bench.batch_size exceeds the TX queue batch_size");
      }
      for (const auto& mr_name : queue.common_.mrs_) {
        const auto mr = network.mrs_.find(mr_name);
        if (mr == network.mrs_.end() || bench.payload_size > mr->second.buf_size_) {
          throw std::runtime_error("pcie_bench.payload_size exceeds TX memory-region buf_size");
        }
      }
    }
    for (const auto& queue : interface.rx_.queues_) {
      for (const auto& mr_name : queue.common_.mrs_) {
        const auto mr = network.mrs_.find(mr_name);
        if (mr == network.mrs_.end() || bench.payload_size > mr->second.buf_size_) {
          throw std::runtime_error("pcie_bench.payload_size exceeds RX memory-region buf_size");
        }
      }
    }
  }
  if (!found_interface) {
    throw std::runtime_error("pcie_bench.interface_name does not name a configured interface");
  }
}

uint8_t pattern_byte(uint64_t sequence, size_t offset) {
  return static_cast<uint8_t>((sequence + offset * 17U) & 0xffU);
}

void fill_payload(std::vector<uint8_t>& payload, uint64_t sequence) {
  std::memcpy(payload.data(), &sequence, sizeof(sequence));
  for (size_t i = sizeof(sequence); i < payload.size(); ++i) {
    payload[i] = pattern_byte(sequence, i);
  }
}

bool validate_payload(const std::vector<uint8_t>& payload, uint64_t* sequence) {
  std::memcpy(sequence, payload.data(), sizeof(*sequence));
  for (size_t i = sizeof(*sequence); i < payload.size(); ++i) {
    if (payload[i] != pattern_byte(*sequence, i)) {
      return false;
    }
  }
  return true;
}

void release_rx_burst(daqiri::BurstParams* burst, bool mixed_packet_frees) {
  if (mixed_packet_frees) {
    const auto packet_count = daqiri::get_num_packets(burst);
    for (int64_t i = 0; i < packet_count; i += 2) {
      daqiri::free_packet(burst, static_cast<int>(i));
    }
  }
  // When alternating packets were released above, this deliberately exercises
  // the mixed individual/whole-burst free path and must not return a slot twice.
  daqiri::free_all_packets_and_burst_rx(burst);
}

bool verify_and_release_held_burst(HeldRxBurst held, const BenchConfig& cfg,
                                   std::vector<uint8_t>& payload) {
  bool valid = true;
  if (cfg.verify) {
    const auto packet_count = daqiri::get_num_packets(held.burst);
    if (held.sequences.size() != static_cast<size_t>(packet_count)) {
      valid = false;
    }
    for (int64_t i = 0; valid && i < packet_count; ++i) {
      const auto length = daqiri::get_packet_length(held.burst, static_cast<int>(i));
      const auto cuda_status =
          cudaMemcpy(payload.data(), daqiri::get_packet_ptr(held.burst, static_cast<int>(i)),
                     payload.size(), cudaMemcpyDeviceToHost);
      uint64_t sequence = 0;
      if (length != cfg.payload_size || cuda_status != cudaSuccess ||
          !validate_payload(payload, &sequence) || sequence != held.sequences[i]) {
        std::cerr << "Held RX slot changed while application-owned at packet " << i << '\n';
        valid = false;
      }
    }
  }
  release_rx_burst(held.burst, cfg.mixed_packet_frees);
  return valid;
}

void tx_worker(const BenchConfig& cfg, int port_id, std::atomic<bool>& stop,
               std::atomic<bool>& failed, BenchStats& stats) {
  if (!set_affinity(cfg.tx_cpu_core, "pcie_bench_tx")) {
    failed.store(true);
    stop.store(true);
    return;
  }

  std::vector<uint8_t> payload(cfg.payload_size);
  uint64_t sequence = 0;
  while (!stop.load()) {
    auto* burst = daqiri::create_tx_burst_params();
    if (burst == nullptr) {
      ++stats.backpressure;
      std::this_thread::sleep_for(std::chrono::microseconds(50));
      continue;
    }

    daqiri::set_header(burst, static_cast<uint16_t>(port_id), 0, cfg.batch_size, 1);
    const auto allocation_status = daqiri::get_tx_packet_burst(burst);
    if (allocation_status == daqiri::Status::NO_FREE_BURST_BUFFERS ||
        allocation_status == daqiri::Status::NO_FREE_PACKET_BUFFERS ||
        allocation_status == daqiri::Status::NOT_READY) {
      daqiri::free_tx_metadata(burst);
      ++stats.backpressure;
      std::this_thread::sleep_for(std::chrono::microseconds(50));
      continue;
    }
    if (allocation_status != daqiri::Status::SUCCESS) {
      std::cerr << "TX allocation failed with status " << static_cast<int>(allocation_status)
                << '\n';
      daqiri::free_tx_metadata(burst);
      ++stats.errors;
      failed.store(true);
      stop.store(true);
      break;
    }

    const auto packet_count = daqiri::get_num_packets(burst);
    bool populate_failed = false;
    for (int64_t i = 0; i < packet_count; ++i) {
      fill_payload(payload, sequence++);
      const auto cuda_status = cudaMemcpy(daqiri::get_packet_ptr(burst, static_cast<int>(i)),
                                          payload.data(), payload.size(), cudaMemcpyHostToDevice);
      if (cuda_status != cudaSuccess ||
          daqiri::set_packet_lengths(burst, static_cast<int>(i),
                                     {static_cast<int>(cfg.payload_size)}) !=
              daqiri::Status::SUCCESS) {
        if (cuda_status != cudaSuccess) {
          std::cerr << "TX cudaMemcpy failed: " << cudaGetErrorString(cuda_status) << '\n';
        }
        populate_failed = true;
        break;
      }
    }

    if (populate_failed) {
      daqiri::free_all_packets_and_burst_tx(burst);
      ++stats.errors;
      failed.store(true);
      stop.store(true);
      break;
    }

    // cudaMemcpy above is synchronous: all GPU writes are complete before the
    // FPGA is allowed to read these slots.
    const auto status = daqiri::send_tx_burst(burst);
    if (status == daqiri::Status::SUCCESS) {
      stats.packets += static_cast<uint64_t>(packet_count);
      stats.bytes += static_cast<uint64_t>(packet_count) * cfg.payload_size;
      ++stats.bursts;
    } else if (status == daqiri::Status::NO_SPACE_AVAILABLE) {
      // The API consumes the burst on NO_SPACE_AVAILABLE.
      ++stats.backpressure;
    } else {
      daqiri::free_all_packets_and_burst_tx(burst);
      ++stats.errors;
      failed.store(true);
      stop.store(true);
    }
  }
}

void rx_worker(const BenchConfig& cfg, int port_id, std::atomic<bool>& stop,
               std::atomic<bool>& failed, BenchStats& stats) {
  if (!set_affinity(cfg.rx_cpu_core, "pcie_bench_rx")) {
    failed.store(true);
    stop.store(true);
    return;
  }

  std::vector<uint8_t> payload(cfg.payload_size);
  std::deque<HeldRxBurst> held_bursts;
  while (!stop.load()) {
    daqiri::BurstParams* burst = nullptr;
    const auto receive_status = daqiri::get_rx_burst(&burst, port_id, 0);
    if (receive_status == daqiri::Status::NULL_PTR || receive_status == daqiri::Status::NOT_READY) {
      std::this_thread::sleep_for(std::chrono::microseconds(50));
      continue;
    }
    if (receive_status != daqiri::Status::SUCCESS || burst == nullptr) {
      std::cerr << "RX dequeue failed with status " << static_cast<int>(receive_status) << '\n';
      ++stats.errors;
      failed.store(true);
      stop.store(true);
      break;
    }

    const auto packet_count = daqiri::get_num_packets(burst);
    bool burst_failed = false;
    std::vector<uint64_t> burst_sequences;
    if (cfg.verify) {
      burst_sequences.reserve(static_cast<size_t>(packet_count));
    }
    for (int64_t i = 0; i < packet_count; ++i) {
      const auto length = daqiri::get_packet_length(burst, static_cast<int>(i));
      stats.bytes += length;
      if (!cfg.verify) {
        continue;
      }
      if (length != cfg.payload_size) {
        std::cerr << "RX length mismatch: expected " << cfg.payload_size << ", received " << length
                  << '\n';
        burst_failed = true;
        break;
      }

      // This synchronous copy finishes every GPU read before the RX slot is
      // returned to the FPGA below.
      const auto cuda_status =
          cudaMemcpy(payload.data(), daqiri::get_packet_ptr(burst, static_cast<int>(i)),
                     payload.size(), cudaMemcpyDeviceToHost);
      uint64_t sequence = 0;
      if (cuda_status != cudaSuccess || !validate_payload(payload, &sequence)) {
        if (cuda_status != cudaSuccess) {
          std::cerr << "RX cudaMemcpy failed: " << cudaGetErrorString(cuda_status) << '\n';
        } else {
          std::cerr << "RX payload validation failed for sequence " << sequence << '\n';
        }
        burst_failed = true;
        break;
      }
      burst_sequences.push_back(sequence);
    }

    stats.packets += static_cast<uint64_t>(packet_count);
    ++stats.bursts;

    if (burst_failed) {
      release_rx_burst(burst, cfg.mixed_packet_frees);
      ++stats.errors;
      failed.store(true);
      stop.store(true);
      break;
    }

    // Keep validated bursts application-owned across later completions. This
    // verifies that held slots are not overwritten or credited back early.
    held_bursts.push_back({burst, std::move(burst_sequences)});
    if (held_bursts.size() > cfg.hold_rx_bursts) {
      if (!verify_and_release_held_burst(std::move(held_bursts.front()), cfg, payload)) {
        ++stats.errors;
        failed.store(true);
        stop.store(true);
      }
      held_bursts.pop_front();
    }
  }
  for (auto& held : held_bursts) {
    if (!verify_and_release_held_burst(std::move(held), cfg, payload)) {
      ++stats.errors;
      failed.store(true);
    }
  }
}

void print_stats(const char* direction, const BenchStats& stats, double seconds) {
  const double gbps =
      seconds > 0.0 ? (static_cast<double>(stats.bytes) * 8.0) / seconds / 1.0e9 : 0.0;
  std::cout << direction << " complete: packets=" << stats.packets << " bytes=" << stats.bytes
            << " bursts=" << stats.bursts << " backpressure=" << stats.backpressure
            << " errors=" << stats.errors << " seconds=" << seconds << " Gbps=" << gbps << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <config.yaml> [--seconds N] [--mode tx|rx|both]\n";
    return 1;
  }

  int run_seconds = 10;
  std::string mode = "both";
  try {
    for (int i = 2; i < argc; ++i) {
      const std::string option = argv[i];
      if (i + 1 >= argc) {
        throw std::runtime_error("missing value for " + option);
      }
      if (option == "--seconds") {
        run_seconds = std::stoi(argv[++i]);
      } else if (option == "--mode") {
        mode = argv[++i];
      } else {
        throw std::runtime_error("unknown option " + option);
      }
    }
    if (run_seconds < 0) {
      throw std::runtime_error("--seconds cannot be negative");
    }
    if (mode != "tx" && mode != "rx" && mode != "both") {
      throw std::runtime_error("--mode must be tx, rx, or both");
    }
  } catch (const std::exception& e) {
    std::cerr << "Invalid command line: " << e.what() << '\n';
    return 1;
  }

  BenchConfig cfg;
  try {
    cfg = parse_bench_config(YAML::LoadFile(argv[1]));
  } catch (const std::exception& e) {
    std::cerr << "Invalid benchmark config: " << e.what() << '\n';
    return 1;
  }

  daqiri::NetworkConfig network_config;
  if (daqiri::parse_network_config_from_yaml_file(argv[1], network_config) !=
      daqiri::Status::SUCCESS) {
    std::cerr << "Failed to parse DAQIRI config\n";
    return 1;
  }
  if (mode == "tx") {
    for (auto& interface : network_config.ifs_) {
      interface.rx_.queues_.clear();
    }
    network_config.common_.dir = daqiri::Direction::TX;
  } else if (mode == "rx") {
    for (auto& interface : network_config.ifs_) {
      interface.tx_.queues_.clear();
    }
    network_config.common_.dir = daqiri::Direction::RX;
  } else {
    network_config.common_.dir = daqiri::Direction::TX_RX;
  }

  std::unordered_set<std::string> active_memory_regions;
  for (const auto& interface : network_config.ifs_) {
    for (const auto& queue : interface.rx_.queues_) {
      active_memory_regions.insert(queue.common_.mrs_.begin(), queue.common_.mrs_.end());
    }
    for (const auto& queue : interface.tx_.queues_) {
      active_memory_regions.insert(queue.common_.mrs_.begin(), queue.common_.mrs_.end());
    }
  }
  for (auto it = network_config.mrs_.begin(); it != network_config.mrs_.end();) {
    if (active_memory_regions.count(it->first) == 0) {
      it = network_config.mrs_.erase(it);
    } else {
      ++it;
    }
  }

  try {
    validate_bench_geometry(cfg, network_config);
  } catch (const std::exception& e) {
    std::cerr << "Invalid benchmark geometry: " << e.what() << '\n';
    return 1;
  }

  if (daqiri::daqiri_init(network_config) != daqiri::Status::SUCCESS) {
    std::cerr << "daqiri_init failed\n";
    return 1;
  }
  if (daqiri::get_stream_type() != daqiri::StreamType::PCIE) {
    std::cerr << "daqiri_bench_pcie requires stream_type: pcie\n";
    daqiri::shutdown();
    return 1;
  }

  const int port_id = daqiri::get_port_id(cfg.interface_name);
  if (port_id < 0) {
    std::cerr << "Unknown PCIe interface: " << cfg.interface_name << '\n';
    daqiri::shutdown();
    return 1;
  }

  std::signal(SIGINT, signal_handler);
  std::atomic<bool> stop{false};
  std::atomic<bool> failed{false};
  BenchStats tx_stats;
  BenchStats rx_stats;
  std::thread tx_thread;
  std::thread rx_thread;
  if (mode == "rx" || mode == "both") {
    rx_thread = std::thread(rx_worker, std::cref(cfg), port_id, std::ref(stop), std::ref(failed),
                            std::ref(rx_stats));
  }
  if (mode == "tx" || mode == "both") {
    tx_thread = std::thread(tx_worker, std::cref(cfg), port_id, std::ref(stop), std::ref(failed),
                            std::ref(tx_stats));
  }

  const auto start = std::chrono::steady_clock::now();
  while (!g_stop_requested && !failed.load()) {
    if (run_seconds > 0 &&
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start)
                .count() >= run_seconds) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  stop.store(true);

  if (tx_thread.joinable()) {
    tx_thread.join();
  }
  if (rx_thread.joinable()) {
    rx_thread.join();
  }

  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  if (mode == "tx" || mode == "both") {
    print_stats("TX", tx_stats, seconds);
  }
  if (mode == "rx" || mode == "both") {
    print_stats("RX", rx_stats, seconds);
  }
  daqiri::print_stats();
  daqiri::shutdown();
  return failed.load() ? 1 : 0;
}
