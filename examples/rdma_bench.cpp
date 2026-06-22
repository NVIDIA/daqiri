/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
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

#include <yaml-cpp/yaml.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include "raw_bench_common.h"
#include <daqiri/daqiri.h>

namespace {

volatile std::sig_atomic_t g_stop_requested = 0;

void signal_handler(int signum) {
  if (signum == SIGINT) { g_stop_requested = 1; }
}

struct RdmaBenchConfig {
  bool server = false;
  bool send = false;
  bool receive = false;
  int cpu_core = -1;
  int message_size = 1024;
  int tx_depth = 128;
  int rx_depth = 512;
  std::string server_address = "10.100.1.1";
  std::string client_address = "10.100.4.1";
  uint16_t server_port = 4096;
};

struct RdmaWorkerStats {
  uint64_t send_completions = 0;
  uint64_t recv_completions = 0;
  uint64_t send_bytes = 0;
  uint64_t recv_bytes = 0;
};

RdmaBenchConfig parse_rdma_cfg(const YAML::Node& node) {
  RdmaBenchConfig cfg;
  cfg.server = node["server"].as<bool>(cfg.server);
  cfg.send = node["send"].as<bool>(cfg.send);
  cfg.receive = node["receive"].as<bool>(cfg.receive);
  cfg.cpu_core = node["cpu_core"].as<int>(cfg.cpu_core);
  if (cfg.cpu_core < -1) {
    throw std::runtime_error("rdma_bench cpu_core is out of range");
  }
  cfg.message_size = node["message_size"].as<int>(cfg.message_size);
  cfg.tx_depth = node["tx_depth"].as<int>(cfg.tx_depth);
  cfg.rx_depth = node["rx_depth"].as<int>(cfg.rx_depth);
  if (cfg.tx_depth < 1) { cfg.tx_depth = 1; }
  if (cfg.rx_depth < 1) { cfg.rx_depth = 1; }
  cfg.server_address = node["server_address"].as<std::string>(cfg.server_address);
  cfg.client_address = node["client_address"].as<std::string>(cfg.client_address);
  cfg.server_port = node["server_port"].as<uint16_t>(cfg.server_port);
  return cfg;
}

void rdma_worker(const RdmaBenchConfig& cfg, daqiri::bench::TokenBucketPacer& pacer,
                 std::atomic<bool>& stop, RdmaWorkerStats& stats,
                 daqiri::bench::BenchWorkload workload) {
  const char *thread_name =
      cfg.server ? "rdma_bench_server" : "rdma_bench_client";
  if (!daqiri::bench::set_current_thread_affinity(cfg.cpu_core, thread_name)) {
    stop.store(true);
    return;
  }

  // Representative GPU workload run per received message (no-op unless
  // --workload set). RoCE exposes no payload device pointer, so this runs on
  // its own scratch buffers sized to the message — exactly the drop-in the
  // reusable component is designed for.
  daqiri::bench::GpuWorkload gpu_workload;
  if (!gpu_workload.init(workload,
                         static_cast<size_t>(cfg.message_size)) &&
      workload != daqiri::bench::BenchWorkload::None) {
    std::cerr << "RDMA workload init failed; continuing without GPU workload\n";
  }

  int outstanding_send = 0;
  int outstanding_recv = 0;
  uint64_t send_wr_id = 0x1234;
  uint64_t recv_wr_id = 0x2345;
  uintptr_t conn_id = 0;
  std::string send_mr = cfg.server ? "DATA_TX_GPU_SERVER" : "DATA_TX_GPU_CLIENT";
  std::string recv_mr = cfg.server ? "DATA_RX_GPU_SERVER" : "DATA_RX_GPU_CLIENT";
  bool recv_primed = !cfg.receive;

  // After `stop` is signalled, drain in-flight completions for a short window so
  // pending SENDs land cleanly instead of getting WR_FLUSH_ERR on disconnect.
  auto drain_deadline = std::chrono::steady_clock::time_point::max();
  const auto drain_window = std::chrono::milliseconds(500);

  while (true) {
    const bool stopped = stop.load();
    if (stopped && drain_deadline == std::chrono::steady_clock::time_point::max()) {
      drain_deadline = std::chrono::steady_clock::now() + drain_window;
    }
    if (stopped) {
      if (outstanding_send == 0 && outstanding_recv == 0) { break; }
      if (std::chrono::steady_clock::now() >= drain_deadline) { break; }
    }

    if (conn_id == 0 && stopped) { break; }
    if (conn_id == 0) {
      daqiri::Status s = daqiri::Status::GENERIC_FAILURE;
      if (cfg.server) {
        s = daqiri::rdma_get_server_conn_id(cfg.server_address, cfg.server_port, &conn_id);
      } else {
        s = daqiri::rdma_connect_to_server(cfg.server_address, cfg.server_port, cfg.client_address,
                                           &conn_id);
      }
      if (s != daqiri::Status::SUCCESS) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        continue;
      }
      recv_primed = !cfg.receive;
    }

    auto post_req = [&](int& outstanding, int depth, uint64_t& wr_id, daqiri::RDMAOpCode op,
                        const std::string& mr_name) -> bool {
      if (outstanding >= depth) { return false; }

      auto* msg = daqiri::create_tx_burst_params();
      if (msg == nullptr) { return false; }

      if (daqiri::rdma_set_header(msg, op, conn_id, cfg.server, 1, wr_id, mr_name) !=
          daqiri::Status::SUCCESS) {
        daqiri::free_tx_metadata(msg);
        return false;
      }

      if (!daqiri::is_tx_burst_available(msg)) {
        daqiri::free_tx_metadata(msg);
        return false;
      }

      if (daqiri::get_tx_packet_burst(msg) != daqiri::Status::SUCCESS) {
        daqiri::free_tx_metadata(msg);
        return false;
      }

      if (daqiri::set_packet_lengths(msg, 0, {cfg.message_size}) != daqiri::Status::SUCCESS) {
        daqiri::free_tx_burst(msg);
        return false;
      }
      if (daqiri::send_tx_burst(msg) != daqiri::Status::SUCCESS) { return false; }
      outstanding++;
      wr_id++;
      // Only meter actual byte transmissions (SENDs), not RECEIVE-side posts.
      if (op == daqiri::RDMAOpCode::SEND) {
        pacer.wait_for_bytes(static_cast<size_t>(cfg.message_size), stop);
      }
      return true;
    };

    auto refill_receives = [&]() -> bool {
      bool posted = false;
      while (cfg.receive && outstanding_recv < cfg.rx_depth &&
             post_req(outstanding_recv, cfg.rx_depth, recv_wr_id, daqiri::RDMAOpCode::RECEIVE,
                      recv_mr)) {
        posted = true;
      }
      return posted;
    };

    if (!recv_primed && !stopped) {
      int idle_spins = 0;
      while (!stop.load() && outstanding_recv < cfg.rx_depth) {
        if (refill_receives()) {
          idle_spins = 0;
        } else {
          if (++idle_spins > 100) { break; }
          std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
      }
      recv_primed = !cfg.receive || outstanding_recv > 0;
    }

    bool posted_work = stopped ? false : refill_receives();
    bool got_completion = false;
    while (true) {
      daqiri::BurstParams* completion = nullptr;
      const auto completion_status = daqiri::get_rx_burst(&completion, conn_id, cfg.server);
      if (completion_status != daqiri::Status::SUCCESS || completion == nullptr) { break; }

      got_completion = true;
      const auto opcode = daqiri::rdma_get_opcode(completion);
      if (opcode == daqiri::RDMAOpCode::SEND && outstanding_send > 0) {
        outstanding_send--;
        stats.send_completions++;
        stats.send_bytes += static_cast<uint64_t>(cfg.message_size);
      } else if (opcode == daqiri::RDMAOpCode::RECEIVE && outstanding_recv > 0) {
        outstanding_recv--;
        stats.recv_completions++;
        stats.recv_bytes += static_cast<uint64_t>(cfg.message_size);
        gpu_workload.run();
      }
      daqiri::free_tx_burst(completion);
    }

    if (!stopped) { posted_work = refill_receives() || posted_work; }
    const bool local_receive_window_ready = !cfg.receive || outstanding_recv > 0;
    if (!stopped && cfg.send && local_receive_window_ready) {
      while (outstanding_send < cfg.tx_depth &&
             post_req(outstanding_send, cfg.tx_depth, send_wr_id, daqiri::RDMAOpCode::SEND,
                      send_mr)) {
        posted_work = true;
      }
    }

    if (!got_completion && !posted_work) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  }
  gpu_workload.sync();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0]
              << " <config.yaml> [--seconds N] [--mode server|client|both] "
                 "[--target-gbps G] [--workload none|fft|gemm]\n";
    return 1;
  }

  int run_seconds = 10;
  double target_gbps = 0.0;
  std::string mode = "both";
  for (int i = 2; i + 1 < argc; i += 2) {
    if (std::string(argv[i]) == "--seconds") {
      run_seconds = std::stoi(argv[i + 1]);
    } else if (std::string(argv[i]) == "--mode") {
      mode = argv[i + 1];
    } else if (std::string(argv[i]) == "--target-gbps") {
      target_gbps = std::stod(argv[i + 1]);
    }
  }
  const auto workload = daqiri::bench::parse_workload(argc, argv);

  const auto root = YAML::LoadFile(argv[1]);
  if (daqiri::daqiri_init(argv[1]) != daqiri::Status::SUCCESS) {
    std::cerr << "daqiri_init failed\n";
    return 1;
  }

  std::atomic<bool> stop{false};
  std::thread server_thread;
  std::thread client_thread;
  RdmaWorkerStats server_stats;
  RdmaWorkerStats client_stats;
  daqiri::bench::TokenBucketPacer server_pacer(target_gbps);
  daqiri::bench::TokenBucketPacer client_pacer(target_gbps);
  bool run_server = false;
  bool run_client = false;
  RdmaBenchConfig server_cfg;
  RdmaBenchConfig client_cfg;

  try {
    if ((mode == "server" || mode == "both") && root["rdma_bench_server"]) {
      run_server = true;
      server_cfg = parse_rdma_cfg(root["rdma_bench_server"]);
    }
    if ((mode == "client" || mode == "both") && root["rdma_bench_client"]) {
      run_client = true;
      client_cfg = parse_rdma_cfg(root["rdma_bench_client"]);
    }
  } catch (const std::exception &e) {
    std::cerr << "Invalid benchmark config: " << e.what() << "\n";
    daqiri::shutdown();
    return 1;
  }

  if (run_server) {
    server_thread = std::thread(rdma_worker, server_cfg, std::ref(server_pacer),
                                std::ref(stop), std::ref(server_stats), workload);
  }
  if (run_client) {
    client_thread = std::thread(rdma_worker, client_cfg, std::ref(client_pacer),
                                std::ref(stop), std::ref(client_stats), workload);
  }

  if (!server_thread.joinable() && !client_thread.joinable()) {
    std::cerr << "No rdma_bench_server/rdma_bench_client config selected\n";
    daqiri::shutdown();
    return 1;
  }

  std::signal(SIGINT, signal_handler);
  auto start = std::chrono::steady_clock::now();
  while (!g_stop_requested) {
    if (run_seconds > 0) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now() - start);
      if (elapsed.count() >= run_seconds) { break; }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  stop.store(true);

  if (server_thread.joinable()) { server_thread.join(); }
  if (client_thread.joinable()) { client_thread.join(); }

  const double secs =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();

  if (run_server) {
    std::cout << "Server complete: send_completions=" << server_stats.send_completions
              << " recv_completions=" << server_stats.recv_completions
              << " send_bytes=" << server_stats.send_bytes
              << " recv_bytes=" << server_stats.recv_bytes
              << " seconds=" << secs << '\n';
  }
  if (run_client) {
    std::cout << "Client complete: send_completions=" << client_stats.send_completions
              << " recv_completions=" << client_stats.recv_completions
              << " send_bytes=" << client_stats.send_bytes
              << " recv_bytes=" << client_stats.recv_bytes
              << " seconds=" << secs << '\n';
  }

  daqiri::print_stats();
  daqiri::shutdown();
  return 0;
}
