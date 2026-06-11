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
#include <cstring>
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

struct SocketBenchConfig {
  bool server = false;
  bool send = false;
  bool receive = false;
  int cpu_core = -1;
  int message_size = 1024;
  int iterations = 1000;
  std::string server_address = "127.0.0.1";
  std::string client_address = "127.0.0.1";
  uint16_t server_port = 5001;
};

struct SocketWorkerStats {
  uint64_t sent_packets = 0;
  uint64_t received_packets = 0;
  uint64_t sent_bytes = 0;
  uint64_t received_bytes = 0;
};

SocketBenchConfig parse_socket_cfg(const YAML::Node& node) {
  SocketBenchConfig cfg;
  cfg.server = node["server"].as<bool>(cfg.server);
  cfg.send = node["send"].as<bool>(cfg.send);
  cfg.receive = node["receive"].as<bool>(cfg.receive);
  cfg.cpu_core = node["cpu_core"].as<int>(cfg.cpu_core);
  if (cfg.cpu_core < -1) {
    throw std::runtime_error("socket_bench cpu_core is out of range");
  }
  cfg.message_size = node["message_size"].as<int>(cfg.message_size);
  cfg.iterations = node["iterations"].as<int>(cfg.iterations);
  cfg.server_address = node["server_address"].as<std::string>(cfg.server_address);
  cfg.client_address = node["client_address"].as<std::string>(cfg.client_address);
  cfg.server_port = node["server_port"].as<uint16_t>(cfg.server_port);
  return cfg;
}

void socket_worker(const SocketBenchConfig& cfg, daqiri::bench::TokenBucketPacer& pacer,
                   std::atomic<bool>& stop, SocketWorkerStats& stats) {
  const char *thread_name =
      cfg.server ? "socket_bench_server" : "socket_bench_client";
  if (!daqiri::bench::set_current_thread_affinity(cfg.cpu_core, thread_name)) {
    stop.store(true);
    return;
  }

  uintptr_t conn_id = 0;
  uint16_t port = 0;
  uint16_t queue = 0;

  while (!stop.load()) {
    if (conn_id == 0) {
      daqiri::Status s = daqiri::Status::GENERIC_FAILURE;
      if (cfg.server) {
        s = daqiri::socket_get_server_conn_id(cfg.server_address, cfg.server_port, &conn_id);
      } else {
        s = daqiri::socket_connect_to_server(
            cfg.server_address, cfg.server_port, cfg.client_address, &conn_id);
      }

      if (s != daqiri::Status::SUCCESS) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }

      if (daqiri::socket_get_port_queue(conn_id, &port, &queue) != daqiri::Status::SUCCESS) {
        conn_id = 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
    }

    // When cfg.iterations <= 0, the loop is time-bounded (driven by stop.load()
    // set by --seconds). Otherwise the iteration cap applies as before.
    const bool send_done = !cfg.send ||
                           (cfg.iterations > 0 &&
                            stats.sent_packets >= static_cast<uint64_t>(cfg.iterations));
    const bool recv_done = !cfg.receive ||
                           (cfg.iterations > 0 &&
                            stats.received_packets >= static_cast<uint64_t>(cfg.iterations));
    if (send_done && recv_done) { break; }

    if (cfg.send && !send_done) {
      auto* msg = daqiri::create_tx_burst_params();
      daqiri::set_header(msg, port, queue, 1, 1);

      if (daqiri::get_tx_packet_burst(msg) == daqiri::Status::SUCCESS) {
        auto* payload = reinterpret_cast<uint8_t*>(daqiri::get_packet_ptr(msg, 0));
        std::memset(payload, static_cast<int>(stats.sent_packets & 0xff), cfg.message_size);
        daqiri::set_packet_lengths(msg, 0, {cfg.message_size});

        daqiri::set_connection_id(msg, conn_id);

        if (daqiri::send_tx_burst(msg) == daqiri::Status::SUCCESS) {
          stats.sent_packets++;
          stats.sent_bytes += static_cast<uint64_t>(cfg.message_size);
          pacer.wait_for_bytes(static_cast<size_t>(cfg.message_size), stop);
        }
      } else {
        daqiri::free_tx_metadata(msg);
      }
    }

    if (cfg.receive && !recv_done) {
      daqiri::BurstParams* burst = nullptr;
      if (daqiri::get_rx_burst(&burst, conn_id, cfg.server) == daqiri::Status::SUCCESS &&
          burst != nullptr) {
        const uint64_t rx_pkts = static_cast<uint64_t>(daqiri::get_num_packets(burst));
        stats.received_packets += rx_pkts;
        stats.received_bytes += daqiri::get_burst_tot_byte(burst);
        daqiri::free_all_packets_and_burst_rx(burst);
      } else {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0]
              << " <config.yaml> [--seconds N] [--mode server|client|both] [--target-gbps G]\n";
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

  const auto root = YAML::LoadFile(argv[1]);
  if (daqiri::daqiri_init(argv[1]) != daqiri::Status::SUCCESS) {
    std::cerr << "daqiri_init failed\n";
    return 1;
  }

  std::atomic<bool> stop{false};
  std::thread server_thread;
  std::thread client_thread;
  SocketWorkerStats server_stats;
  SocketWorkerStats client_stats;
  daqiri::bench::TokenBucketPacer server_pacer(target_gbps);
  daqiri::bench::TokenBucketPacer client_pacer(target_gbps);
  bool run_server = false;
  bool run_client = false;
  SocketBenchConfig server_cfg;
  SocketBenchConfig client_cfg;

  try {
    if ((mode == "server" || mode == "both") && root["socket_bench_server"]) {
      run_server = true;
      server_cfg = parse_socket_cfg(root["socket_bench_server"]);
    }
    if ((mode == "client" || mode == "both") && root["socket_bench_client"]) {
      run_client = true;
      client_cfg = parse_socket_cfg(root["socket_bench_client"]);
    }
  } catch (const std::exception &e) {
    std::cerr << "Invalid benchmark config: " << e.what() << "\n";
    daqiri::shutdown();
    return 1;
  }

  if (run_server) {
    server_thread = std::thread(socket_worker, server_cfg, std::ref(server_pacer),
                                std::ref(stop), std::ref(server_stats));
  }
  if (run_client) {
    client_thread = std::thread(socket_worker, client_cfg, std::ref(client_pacer),
                                std::ref(stop), std::ref(client_stats));
  }

  if (!server_thread.joinable() && !client_thread.joinable()) {
    std::cerr << "No socket_bench_server/socket_bench_client config selected\n";
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
    std::cout << "Server complete: sent_packets=" << server_stats.sent_packets
              << " recv_packets=" << server_stats.received_packets
              << " sent_bytes=" << server_stats.sent_bytes
              << " recv_bytes=" << server_stats.received_bytes
              << " seconds=" << secs << '\n';
  }
  if (run_client) {
    std::cout << "Client complete: sent_packets=" << client_stats.sent_packets
              << " recv_packets=" << client_stats.received_packets
              << " sent_bytes=" << client_stats.sent_bytes
              << " recv_bytes=" << client_stats.received_bytes
              << " seconds=" << secs << '\n';
  }

  daqiri::print_stats();
  daqiri::shutdown();
  return 0;
}
