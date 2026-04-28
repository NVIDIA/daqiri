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
#include <string>
#include <thread>

#include "src/common.h"

namespace {

volatile std::sig_atomic_t g_stop_requested = 0;

void signal_handler(int signum) {
  if (signum == SIGINT) {
    g_stop_requested = 1;
  }
}

struct RdmaBenchConfig {
  bool server = false;
  bool send = false;
  bool receive = false;
  int message_size = 1024;
  std::string server_address = "10.100.1.1";
  std::string client_address = "10.100.4.1";
  uint16_t server_port = 4096;
};

struct RdmaWorkerStats {
  uint64_t send_completions = 0;
  uint64_t recv_completions = 0;
};

RdmaBenchConfig parse_rdma_cfg(const YAML::Node &node) {
  RdmaBenchConfig cfg;
  cfg.server = node["server"].as<bool>(cfg.server);
  cfg.send = node["send"].as<bool>(cfg.send);
  cfg.receive = node["receive"].as<bool>(cfg.receive);
  cfg.message_size = node["message_size"].as<int>(cfg.message_size);
  cfg.server_address =
      node["server_address"].as<std::string>(cfg.server_address);
  cfg.client_address =
      node["client_address"].as<std::string>(cfg.client_address);
  cfg.server_port = node["server_port"].as<uint16_t>(cfg.server_port);
  return cfg;
}

void rdma_worker(const RdmaBenchConfig &cfg, std::atomic<bool> &stop,
                 RdmaWorkerStats &stats) {
  static constexpr int kMaxOutstanding = 5;
  int outstanding_send = 0;
  int outstanding_recv = 0;
  uint64_t send_wr_id = 0x1234;
  uint64_t recv_wr_id = 0x2345;
  uintptr_t conn_id = 0;
  std::string send_mr =
      cfg.server ? "DATA_TX_GPU_SERVER" : "DATA_TX_GPU_CLIENT";
  std::string recv_mr =
      cfg.server ? "DATA_RX_GPU_SERVER" : "DATA_RX_GPU_CLIENT";

  while (!stop.load()) {
    if (conn_id == 0) {
      daqiri::Status s = daqiri::Status::GENERIC_FAILURE;
      if (cfg.server) {
        s = daqiri::rdma_get_server_conn_id(cfg.server_address, cfg.server_port,
                                            &conn_id);
      } else {
        s = daqiri::rdma_connect_to_server(cfg.server_address, cfg.server_port,
                                           cfg.client_address, &conn_id);
      }
      if (s != daqiri::Status::SUCCESS) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        continue;
      }
    }

    auto post_req = [&](int &outstanding, uint64_t &wr_id,
                        daqiri::RDMAOpCode op, const std::string &mr_name) {
      if (outstanding >= kMaxOutstanding) {
        return;
      }

      auto *msg = daqiri::create_burst_params();
      if (daqiri::rdma_set_header(msg, op, conn_id, cfg.server, 1, wr_id,
                                  mr_name) != daqiri::Status::SUCCESS) {
        daqiri::free_tx_burst(msg);
        return;
      }

      while (daqiri::get_tx_packet_burst(msg) != daqiri::Status::SUCCESS &&
             !stop.load()) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
      }
      if (stop.load()) {
        daqiri::free_tx_burst(msg);
        return;
      }

      if (daqiri::set_packet_lengths(msg, 0, {cfg.message_size}) !=
          daqiri::Status::SUCCESS) {
        daqiri::free_tx_burst(msg);
        return;
      }
      if (daqiri::send_tx_burst(msg) != daqiri::Status::SUCCESS) {
        daqiri::free_tx_burst(msg);
        return;
      }
      outstanding++;
      wr_id++;
    };

    if (cfg.send) {
      post_req(outstanding_send, send_wr_id, daqiri::RDMAOpCode::SEND, send_mr);
    }
    if (cfg.receive) {
      post_req(outstanding_recv, recv_wr_id, daqiri::RDMAOpCode::RECEIVE,
               recv_mr);
    }

    daqiri::BurstParams *completion = nullptr;
    if (daqiri::get_rx_burst(&completion, conn_id, cfg.server) ==
            daqiri::Status::SUCCESS &&
        completion != nullptr) {
      if (daqiri::rdma_get_opcode(completion) == daqiri::RDMAOpCode::SEND &&
          outstanding_send > 0) {
        outstanding_send--;
        stats.send_completions++;
      } else if (daqiri::rdma_get_opcode(completion) ==
                     daqiri::RDMAOpCode::RECEIVE &&
                 outstanding_recv > 0) {
        outstanding_recv--;
        stats.recv_completions++;
      }
      daqiri::free_tx_burst(completion);
    } else {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  }
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0]
              << " <config.yaml> [--seconds N] [--mode server|client|both]\n";
    return 1;
  }

  int run_seconds = 10;
  std::string mode = "both";
  for (int i = 2; i + 1 < argc; i += 2) {
    if (std::string(argv[i]) == "--seconds") {
      run_seconds = std::stoi(argv[i + 1]);
    } else if (std::string(argv[i]) == "--mode") {
      mode = argv[i + 1];
    }
  }

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
  bool run_server = false;
  bool run_client = false;

  if ((mode == "server" || mode == "both") && root["rdma_bench_server"]) {
    run_server = true;
    server_thread =
        std::thread(rdma_worker, parse_rdma_cfg(root["rdma_bench_server"]),
                    std::ref(stop), std::ref(server_stats));
  }
  if ((mode == "client" || mode == "both") && root["rdma_bench_client"]) {
    run_client = true;
    client_thread =
        std::thread(rdma_worker, parse_rdma_cfg(root["rdma_bench_client"]),
                    std::ref(stop), std::ref(client_stats));
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
      if (elapsed.count() >= run_seconds) {
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  stop.store(true);

  if (server_thread.joinable()) {
    server_thread.join();
  }
  if (client_thread.joinable()) {
    client_thread.join();
  }

  if (run_server) {
    std::cout << "Server received messages: " << server_stats.recv_completions
              << '\n';
  }
  if (run_client) {
    std::cout << "Client received messages: " << client_stats.recv_completions
              << '\n';
  }

  daqiri::print_stats();
  daqiri::shutdown();
  return 0;
}
