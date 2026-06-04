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

#include <arpa/inet.h>
#include <cuda_runtime.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "raw_bench_common.h"
#include <daqiri/daqiri.h>

namespace {

using Clock = std::chrono::steady_clock;

struct Args {
  std::string config_path;
  int drop_ms = 1000;
  int active_ms = 1000;
  int drain_ms = 200;
  double target_gbps = 0.0;
  double tolerance = 0.35;
};

struct RxCounter {
  std::atomic<uint64_t> packets{0};
  std::atomic<uint64_t> bytes{0};
  std::atomic<uint64_t> bursts{0};
};

Args parse_args(int argc, char** argv) {
  if (argc < 2) {
    throw std::runtime_error(
        "Usage: dynamic_rx_flow_example <config.yaml> [--target-gbps G] "
        "[--drop-ms N] [--active-ms N] [--drain-ms N] [--tolerance R]");
  }

  Args args;
  args.config_path = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string opt = argv[i];
    if (i + 1 >= argc) {
      throw std::runtime_error("Missing value for " + opt);
    }
    const std::string value = argv[++i];
    if (opt == "--target-gbps") {
      args.target_gbps = std::stod(value);
    } else if (opt == "--drop-ms") {
      args.drop_ms = std::stoi(value);
    } else if (opt == "--active-ms") {
      args.active_ms = std::stoi(value);
    } else if (opt == "--drain-ms") {
      args.drain_ms = std::stoi(value);
    } else if (opt == "--tolerance") {
      args.tolerance = std::stod(value);
    } else {
      throw std::runtime_error("Unknown option: " + opt);
    }
  }

  if (args.drop_ms < 0 || args.active_ms <= 0 || args.drain_ms < 0) {
    throw std::runtime_error("Timing values must be non-negative, and active-ms must be positive");
  }
  if (args.tolerance < 0.0) {
    throw std::runtime_error("tolerance must be non-negative");
  }
  return args;
}

uint32_t parse_ipv4_host_order(const std::string& addr) {
  uint32_t value = 0;
  if (inet_pton(AF_INET, addr.c_str(), &value) != 1) {
    throw std::runtime_error("Invalid IPv4 address: " + addr);
  }
  return ntohl(value);
}

uint16_t single_udp_port(const std::string& spec, const char* field) {
  const auto ports = daqiri::bench::parse_udp_ports(spec);
  if (ports.size() != 1) {
    throw std::runtime_error(std::string(field) + " must be one UDP port for this example");
  }
  return ports.front();
}

void rx_worker(const daqiri::bench::RawBenchRxConfig& cfg,
               RxCounter& counter,
               std::atomic<bool>& stop) {
  const int port_id = daqiri::get_port_id(cfg.interface_name);
  if (port_id < 0) {
    std::cerr << "Invalid RX interface_name: " << cfg.interface_name << "\n";
    stop.store(true);
    return;
  }

  while (!stop.load()) {
    daqiri::BurstParams* burst = nullptr;
    if (daqiri::get_rx_burst(&burst, port_id, cfg.queue_id) != daqiri::Status::SUCCESS ||
        burst == nullptr) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      continue;
    }

    counter.packets.fetch_add(static_cast<uint64_t>(daqiri::get_num_packets(burst)),
                              std::memory_order_relaxed);
    counter.bytes.fetch_add(daqiri::get_burst_tot_byte(burst), std::memory_order_relaxed);
    counter.bursts.fetch_add(1, std::memory_order_relaxed);
    daqiri::free_all_packets_and_burst_rx(burst);
  }
}

void tx_worker(const daqiri::bench::RawBenchTxConfig& cfg,
               daqiri::bench::TokenBucketPacer& pacer,
               std::atomic<bool>& stop) {
  const int port_id = daqiri::get_port_id(cfg.interface_name);
  if (port_id < 0) {
    std::cerr << "Invalid TX interface_name: " << cfg.interface_name << "\n";
    stop.store(true);
    return;
  }

  char eth_dst[6] = {0};
  char eth_src[6] = {0};
  daqiri::format_eth_addr(eth_src, cfg.eth_src_addr);
  daqiri::format_eth_addr(eth_dst, cfg.eth_dst_addr);

  const uint32_t ip_src = parse_ipv4_host_order(cfg.ip_src_addr);
  const uint32_t ip_dst = parse_ipv4_host_order(cfg.ip_dst_addr);
  const uint16_t udp_src = single_udp_port(cfg.udp_src_port, "bench_tx.udp_src_port");
  const uint16_t udp_dst = single_udp_port(cfg.udp_dst_port, "bench_tx.udp_dst_port");
  const uint64_t packet_size = static_cast<uint64_t>(cfg.header_size) + cfg.payload_size;

  std::vector<uint8_t> packet_template(static_cast<size_t>(packet_size));
  daqiri::bench::populate_udp_ipv4_headers(packet_template.data(),
                                           cfg.header_size,
                                           cfg.payload_size,
                                           eth_src,
                                           eth_dst,
                                           ip_src,
                                           ip_dst,
                                           udp_src,
                                           udp_dst);
  for (uint32_t i = 0; i < cfg.payload_size; ++i) {
    packet_template[static_cast<size_t>(cfg.header_size) + i] = static_cast<uint8_t>(i);
  }
  daqiri::bench::finalize_udp_ipv4_checksums(packet_template.data());

  std::unordered_set<void*> initialized_tx_buffers;
  cudaEvent_t copy_done = nullptr;
  if (cudaEventCreateWithFlags(&copy_done, cudaEventDisableTiming) != cudaSuccess) {
    std::cerr << "Failed to create CUDA copy completion event\n";
    stop.store(true);
    return;
  }

  uint64_t packets = 0;
  uint64_t bursts = 0;
  const auto t0 = Clock::now();

  while (!stop.load()) {
    auto* msg = daqiri::create_tx_burst_params();
    daqiri::set_header(msg,
                       static_cast<uint16_t>(port_id),
                       static_cast<uint16_t>(cfg.queue_id),
                       cfg.batch_size,
                       1);

    if (!daqiri::is_tx_burst_available(msg)) {
      daqiri::free_tx_metadata(msg);
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      continue;
    }

    if (daqiri::get_tx_packet_burst(msg) != daqiri::Status::SUCCESS) {
      daqiri::free_tx_metadata(msg);
      continue;
    }

    bool failed = false;
    const int num_pkts = static_cast<int>(daqiri::get_num_packets(msg));
    for (int i = 0; i < num_pkts; ++i) {
      void* pkt = daqiri::get_segment_packet_ptr(msg, 0, i);
      if (initialized_tx_buffers.insert(pkt).second) {
        if (cudaMemcpyAsync(pkt,
                            packet_template.data(),
                            packet_template.size(),
                            cudaMemcpyHostToDevice,
                            nullptr) != cudaSuccess ||
            cudaEventRecord(copy_done, nullptr) != cudaSuccess ||
            cudaEventSynchronize(copy_done) != cudaSuccess) {
          initialized_tx_buffers.erase(pkt);
          failed = true;
          break;
        }
      }

      if (daqiri::set_packet_lengths(msg, i, {static_cast<int>(packet_size)}) !=
          daqiri::Status::SUCCESS) {
        failed = true;
        break;
      }
    }

    if (failed) {
      daqiri::free_all_packets_and_burst_tx(msg);
      continue;
    }

    if (daqiri::send_tx_burst(msg) == daqiri::Status::SUCCESS) {
      packets += static_cast<uint64_t>(num_pkts);
      ++bursts;
      pacer.wait_for_bytes(static_cast<size_t>(num_pkts) * packet_size, stop);
    }
  }

  const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
  std::cout << "TX complete: interface=" << cfg.interface_name
            << " queue=" << cfg.queue_id
            << " packets=" << packets
            << " bursts=" << bursts
            << " seconds=" << secs
            << "\n";
  cudaEventDestroy(copy_done);
}

daqiri::FlowId wait_for_flow_op(daqiri::FlowOpId op_id,
                                daqiri::FlowOpType expected_type,
                                const std::chrono::seconds timeout) {
  const auto deadline = Clock::now() + timeout;
  while (Clock::now() < deadline) {
    daqiri::FlowOpResult result;
    const auto status = daqiri::poll_flow_op(&result);
    if (status == daqiri::Status::NOT_READY) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    if (status != daqiri::Status::SUCCESS) {
      throw std::runtime_error("poll_flow_op failed");
    }
    if (result.op_id_ != op_id) {
      continue;
    }
    if (result.type_ != expected_type) {
      throw std::runtime_error("flow operation completed with unexpected type");
    }
    if (result.status_ != daqiri::Status::SUCCESS) {
      throw std::runtime_error("flow operation completed with failure status");
    }
    return result.flow_id_;
  }

  throw std::runtime_error("timed out waiting for dynamic flow operation");
}

daqiri::FlowId add_dynamic_udp_flow(int port_id,
                                    uint16_t queue_id,
                                    uint16_t udp_src,
                                    uint16_t udp_dst) {
  daqiri::FlowRuleConfig flow;
  flow.name_ = "dynamic_udp_" + std::to_string(udp_dst) + "_q" + std::to_string(queue_id);
  flow.action_.type_ = daqiri::FlowType::QUEUE;
  flow.action_.id_ = queue_id;
  flow.match_.type_ = daqiri::FlowMatchType::IPV4_UDP;
  flow.match_.udp_src_ = udp_src;
  flow.match_.udp_dst_ = udp_dst;

  daqiri::FlowOpId op_id = 0;
  const auto status = daqiri::add_rx_flow_async(port_id, flow, &op_id);
  if (status != daqiri::Status::SUCCESS) {
    throw std::runtime_error("add_rx_flow_async was not accepted");
  }
  return wait_for_flow_op(op_id, daqiri::FlowOpType::ADD_RX, std::chrono::seconds(5));
}

void delete_dynamic_flow(daqiri::FlowId flow_id) {
  daqiri::FlowOpId op_id = 0;
  const auto status = daqiri::delete_flow_async(flow_id, &op_id);
  if (status != daqiri::Status::SUCCESS) {
    throw std::runtime_error("delete_flow_async was not accepted");
  }
  wait_for_flow_op(op_id, daqiri::FlowOpType::DELETE, std::chrono::seconds(5));
}

uint64_t packets(const RxCounter& counter) {
  return counter.packets.load(std::memory_order_relaxed);
}

void print_rx_counter(const daqiri::bench::RawBenchRxConfig& cfg,
                      const RxCounter& counter) {
  std::cout << "RX complete: interface=" << cfg.interface_name
            << " queue=" << cfg.queue_id
            << " packets=" << counter.packets.load(std::memory_order_relaxed)
            << " bytes=" << counter.bytes.load(std::memory_order_relaxed)
            << " bursts=" << counter.bursts.load(std::memory_order_relaxed)
            << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  bool daqiri_initialized = false;
  std::atomic<bool> stop{false};
  std::thread tx_thread;
  std::vector<std::thread> rx_threads;

  try {
    const Args args = parse_args(argc, argv);
    const YAML::Node root = YAML::LoadFile(args.config_path);
    const auto rx_configs = daqiri::bench::parse_rx_configs(root);
    const auto tx_configs = daqiri::bench::parse_tx_configs(root);
    if (rx_configs.size() != 2 || rx_configs[0].queue_id != 0 || rx_configs[1].queue_id != 1) {
      throw std::runtime_error("Config must define bench_rx entries for queue 0 and queue 1");
    }
    if (tx_configs.size() != 1) {
      throw std::runtime_error("Config must define exactly one bench_tx entry");
    }

    const uint16_t udp_src = single_udp_port(tx_configs[0].udp_src_port, "bench_tx.udp_src_port");
    const uint16_t udp_dst = single_udp_port(tx_configs[0].udp_dst_port, "bench_tx.udp_dst_port");

    if (daqiri::daqiri_init(args.config_path) != daqiri::Status::SUCCESS) {
      throw std::runtime_error("daqiri_init failed");
    }
    daqiri_initialized = true;

    const int rx_port_id = daqiri::get_port_id(rx_configs[0].interface_name);
    if (rx_port_id < 0) {
      throw std::runtime_error("Invalid RX interface_name: " + rx_configs[0].interface_name);
    }

    std::vector<RxCounter> counters(2);
    rx_threads.reserve(rx_configs.size());
    for (size_t i = 0; i < rx_configs.size(); ++i) {
      rx_threads.emplace_back(rx_worker, std::cref(rx_configs[i]), std::ref(counters[i]), std::ref(stop));
    }

    daqiri::bench::TokenBucketPacer pacer(args.target_gbps);
    tx_thread = std::thread(tx_worker, std::cref(tx_configs[0]), std::ref(pacer), std::ref(stop));

    std::cout << "Initial drop window: UDP " << udp_src << " -> " << udp_dst
              << " has no configured flow for " << args.drop_ms << " ms\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(args.drop_ms));
    const uint64_t dropped_window_packets = packets(counters[0]) + packets(counters[1]);
    if (dropped_window_packets != 0) {
      throw std::runtime_error("Packets arrived during the initial flow-isolated drop window");
    }

    std::cout << "Adding dynamic UDP flow to RX queue 0 for " << args.active_ms << " ms\n";
    const daqiri::FlowId q0_flow = add_dynamic_udp_flow(rx_port_id, 0, udp_src, udp_dst);
    std::this_thread::sleep_for(std::chrono::milliseconds(args.active_ms));
    delete_dynamic_flow(q0_flow);
    std::this_thread::sleep_for(std::chrono::milliseconds(args.drain_ms));
    const uint64_t q0_after = packets(counters[0]);
    const uint64_t q1_after_q0 = packets(counters[1]);

    std::cout << "Adding dynamic UDP flow to RX queue 1 for " << args.active_ms << " ms\n";
    const daqiri::FlowId q1_flow = add_dynamic_udp_flow(rx_port_id, 1, udp_src, udp_dst);
    std::this_thread::sleep_for(std::chrono::milliseconds(args.active_ms));
    delete_dynamic_flow(q1_flow);
    std::this_thread::sleep_for(std::chrono::milliseconds(args.drain_ms));

    stop.store(true);
    if (tx_thread.joinable()) {
      tx_thread.join();
    }
    for (auto& thread : rx_threads) {
      if (thread.joinable()) {
        thread.join();
      }
    }

    print_rx_counter(rx_configs[0], counters[0]);
    print_rx_counter(rx_configs[1], counters[1]);

    const uint64_t q0_packets = packets(counters[0]);
    const uint64_t q1_packets = packets(counters[1]);
    if (q0_packets == 0 || q1_packets == 0) {
      throw std::runtime_error("Expected nonzero packet counts on both RX queues");
    }
    if (q1_after_q0 != 0) {
      throw std::runtime_error("Queue 1 received packets while only the queue 0 flow was active");
    }
    if (q0_packets < q0_after) {
      throw std::runtime_error("Queue 0 packet counter regressed");
    }

    const auto min_packets = static_cast<double>(std::min(q0_packets, q1_packets));
    const auto max_packets = static_cast<double>(std::max(q0_packets, q1_packets));
    const double relative_delta = (max_packets - min_packets) / max_packets;
    std::cout << "Queue packet relative delta=" << relative_delta
              << " tolerance=" << args.tolerance << "\n";
    if (relative_delta > args.tolerance) {
      throw std::runtime_error("RX queue packet counts are not similar");
    }

    daqiri::print_stats();
    daqiri::shutdown();
    daqiri_initialized = false;
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "dynamic_rx_flow_example failed: " << e.what() << "\n";
    stop.store(true);
    if (tx_thread.joinable()) {
      tx_thread.join();
    }
    for (auto& thread : rx_threads) {
      if (thread.joinable()) {
        thread.join();
      }
    }
    if (daqiri_initialized) {
      daqiri::shutdown();
    }
    return 1;
  }
}
