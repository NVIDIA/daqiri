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

#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <csignal>
#include <cstdlib>
#include <cstdint>
#include <iostream>
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
  int message_size = 1024;
  std::string server_address = "10.100.1.1";
  std::string client_address = "10.100.4.1";
  uint16_t server_port = 4096;
  int cpu_core = -1;
};

struct RdmaWorkerStats {
  uint64_t send_completions = 0;
  uint64_t recv_completions = 0;
  uint64_t send_bytes = 0;
  uint64_t recv_bytes = 0;
  uint64_t loop_iters = 0;
  uint64_t send_post_attempts = 0;
  uint64_t recv_post_attempts = 0;
  uint64_t send_post_skips = 0;
  uint64_t recv_post_skips = 0;
  uint64_t create_burst_ns = 0;
  uint64_t set_header_ns = 0;
  uint64_t get_tx_burst_ns = 0;
  uint64_t get_tx_burst_waits = 0;
  uint64_t set_lengths_ns = 0;
  uint64_t send_tx_burst_ns = 0;
  uint64_t completion_poll_ns = 0;
  uint64_t completion_polls = 0;
  uint64_t completion_hits = 0;
  uint64_t completion_misses = 0;
  uint64_t empty_completion_sleeps = 0;
  uint64_t max_outstanding_send = 0;
  uint64_t max_outstanding_recv = 0;
};

uint64_t elapsed_ns(std::chrono::steady_clock::time_point start) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - start)
          .count());
}

RdmaBenchConfig parse_rdma_cfg(const YAML::Node& node) {
  RdmaBenchConfig cfg;
  cfg.server = node["server"].as<bool>(cfg.server);
  cfg.send = node["send"].as<bool>(cfg.send);
  cfg.receive = node["receive"].as<bool>(cfg.receive);
  cfg.message_size = node["message_size"].as<int>(cfg.message_size);
  cfg.server_address = node["server_address"].as<std::string>(cfg.server_address);
  cfg.client_address = node["client_address"].as<std::string>(cfg.client_address);
  cfg.server_port = node["server_port"].as<uint16_t>(cfg.server_port);
  cfg.cpu_core = node["cpu_core"].as<int>(cfg.cpu_core);
  return cfg;
}

void set_current_thread_affinity(int cpu_core, const char* name) {
  if (cpu_core < 0) { return; }

  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(cpu_core, &cpuset);
  const int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
  if (rc != 0) {
    std::cerr << name << " failed to set affinity to core " << cpu_core << ": "
              << std::strerror(rc) << '\n';
  }
}

void rdma_worker(const RdmaBenchConfig& cfg, daqiri::bench::TokenBucketPacer& pacer,
                 std::atomic<bool>& stop, RdmaWorkerStats& stats) {
  // Matches the per-MR num_bufs in the YAML configs. Higher values deadlock
  // the bench: post_req blocks in get_tx_packet_burst when the pool is empty,
  // but free_tx_burst (which refills it) only runs later in the same loop
  // iteration via get_rx_burst. Until the loop is refactored to interleave
  // drain with post, this constant must stay <= num_bufs.
  int max_outstanding = 20;
  if (const char* env = std::getenv("DAQIRI_RDMA_MAX_OUTSTANDING")) {
    max_outstanding = std::max(1, std::stoi(env));
  }
  int outstanding_send = 0;
  int outstanding_recv = 0;
  uint64_t send_wr_id = 0x1234;
  uint64_t recv_wr_id = 0x2345;
  uintptr_t conn_id = 0;
  const bool profile = std::getenv("DAQIRI_RDMA_BENCH_PROFILE") != nullptr;
  int empty_poll_sleep_us = 100;
  if (const char* env = std::getenv("DAQIRI_RDMA_EMPTY_POLL_SLEEP_US")) {
    empty_poll_sleep_us = std::max(0, std::stoi(env));
  }
  set_current_thread_affinity(cfg.cpu_core, cfg.server ? "Server app thread" : "Client app thread");
  std::string send_mr = cfg.server ? "DATA_TX_GPU_SERVER" : "DATA_TX_GPU_CLIENT";
  std::string recv_mr = cfg.server ? "DATA_RX_GPU_SERVER" : "DATA_RX_GPU_CLIENT";

  while (!stop.load()) {
    if (profile) { stats.loop_iters++; }
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
    }

    auto post_req = [&](int& outstanding, uint64_t& wr_id, daqiri::RDMAOpCode op,
                        const std::string& mr_name) -> bool {
      if (outstanding >= max_outstanding) {
        if (profile) {
          if (op == daqiri::RDMAOpCode::SEND) {
            stats.send_post_skips++;
          } else {
            stats.recv_post_skips++;
          }
        }
        return false;
      }

      if (profile) {
        if (op == daqiri::RDMAOpCode::SEND) {
          stats.send_post_attempts++;
        } else {
          stats.recv_post_attempts++;
        }
      }

      auto start = std::chrono::steady_clock::now();
      auto* msg = daqiri::create_burst_params();
      if (profile) { stats.create_burst_ns += elapsed_ns(start); }

      start = std::chrono::steady_clock::now();
      if (daqiri::rdma_set_header(msg, op, conn_id, cfg.server, 1, wr_id, mr_name) !=
          daqiri::Status::SUCCESS) {
        if (profile) { stats.set_header_ns += elapsed_ns(start); }
        daqiri::free_tx_burst(msg);
        return false;
      }
      if (profile) { stats.set_header_ns += elapsed_ns(start); }

      while (!stop.load()) {
        start = std::chrono::steady_clock::now();
        const auto status = daqiri::get_tx_packet_burst(msg);
        if (profile) { stats.get_tx_burst_ns += elapsed_ns(start); }
        if (status == daqiri::Status::SUCCESS) { break; }
        if (profile) { stats.get_tx_burst_waits++; }
        std::this_thread::sleep_for(std::chrono::microseconds(50));
      }
      if (stop.load()) {
        daqiri::free_tx_burst(msg);
        return false;
      }

      start = std::chrono::steady_clock::now();
      if (daqiri::set_packet_lengths(msg, 0, {cfg.message_size}) != daqiri::Status::SUCCESS) {
        if (profile) { stats.set_lengths_ns += elapsed_ns(start); }
        daqiri::free_tx_burst(msg);
        return false;
      }
      if (profile) { stats.set_lengths_ns += elapsed_ns(start); }

      start = std::chrono::steady_clock::now();
      if (daqiri::send_tx_burst(msg) != daqiri::Status::SUCCESS) {
        if (profile) { stats.send_tx_burst_ns += elapsed_ns(start); }
        daqiri::free_tx_burst(msg);
        return false;
      }
      if (profile) { stats.send_tx_burst_ns += elapsed_ns(start); }

      outstanding++;
      wr_id++;
      if (profile) {
        if (op == daqiri::RDMAOpCode::SEND) {
          stats.max_outstanding_send = std::max<uint64_t>(
              stats.max_outstanding_send, static_cast<uint64_t>(outstanding));
        } else {
          stats.max_outstanding_recv = std::max<uint64_t>(
              stats.max_outstanding_recv, static_cast<uint64_t>(outstanding));
        }
      }
      // Only meter actual byte transmissions (SENDs), not RECEIVE-side posts.
      if (op == daqiri::RDMAOpCode::SEND) {
        pacer.wait_for_bytes(static_cast<size_t>(cfg.message_size), stop);
      }
      return true;
    };

    bool got_completion = false;
    while (true) {
      daqiri::BurstParams* completion = nullptr;
      auto poll_start = std::chrono::steady_clock::now();
      const auto completion_status = daqiri::get_rx_burst(&completion, conn_id, cfg.server);
      if (profile) {
        stats.completion_poll_ns += elapsed_ns(poll_start);
        stats.completion_polls++;
      }
      if (completion_status != daqiri::Status::SUCCESS || completion == nullptr) {
        if (profile) { stats.completion_misses++; }
        break;
      }

      got_completion = true;
      if (profile) { stats.completion_hits++; }
      if (daqiri::rdma_get_opcode(completion) == daqiri::RDMAOpCode::SEND && outstanding_send > 0) {
        outstanding_send--;
        stats.send_completions++;
        stats.send_bytes += static_cast<uint64_t>(cfg.message_size);
      } else if (daqiri::rdma_get_opcode(completion) == daqiri::RDMAOpCode::RECEIVE &&
                 outstanding_recv > 0) {
        outstanding_recv--;
        stats.recv_completions++;
        stats.recv_bytes += static_cast<uint64_t>(cfg.message_size);
      }
      daqiri::free_tx_burst(completion);
    }

    bool posted_work = false;
    if (cfg.receive) {
      while (outstanding_recv < max_outstanding &&
             post_req(outstanding_recv, recv_wr_id, daqiri::RDMAOpCode::RECEIVE, recv_mr)) {
        posted_work = true;
      }
      if (profile && outstanding_recv >= max_outstanding) { stats.recv_post_skips++; }
    }
    if (cfg.send) {
      while (outstanding_send < max_outstanding &&
             post_req(outstanding_send, send_wr_id, daqiri::RDMAOpCode::SEND, send_mr)) {
        posted_work = true;
      }
      if (profile && outstanding_send >= max_outstanding) { stats.send_post_skips++; }
    }

    if (!got_completion && !posted_work) {
      if (profile) { stats.empty_completion_sleeps++; }
      if (empty_poll_sleep_us > 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(empty_poll_sleep_us));
      } else {
        std::this_thread::yield();
      }
    }
  }
}

void print_worker_profile(const char* name, const RdmaWorkerStats& stats) {
  const auto post_attempts = stats.send_post_attempts + stats.recv_post_attempts;
  std::cout << name << " profile:"
            << " loop_iters=" << stats.loop_iters
            << " send_post_attempts=" << stats.send_post_attempts
            << " recv_post_attempts=" << stats.recv_post_attempts
            << " send_post_skips=" << stats.send_post_skips
            << " recv_post_skips=" << stats.recv_post_skips
            << " empty_poll_sleep_us="
            << (std::getenv("DAQIRI_RDMA_EMPTY_POLL_SLEEP_US") == nullptr
                    ? 100
                    : std::max(0, std::stoi(std::getenv("DAQIRI_RDMA_EMPTY_POLL_SLEEP_US"))))
            << " max_outstanding_limit="
            << (std::getenv("DAQIRI_RDMA_MAX_OUTSTANDING") == nullptr
                    ? 20
                    : std::max(1, std::stoi(std::getenv("DAQIRI_RDMA_MAX_OUTSTANDING"))))
            << " max_outstanding_send=" << stats.max_outstanding_send
            << " max_outstanding_recv=" << stats.max_outstanding_recv
            << " completion_polls=" << stats.completion_polls
            << " completion_hits=" << stats.completion_hits
            << " completion_misses=" << stats.completion_misses
            << " get_tx_burst_waits=" << stats.get_tx_burst_waits
            << " create_burst_avg_ns="
            << (post_attempts == 0 ? 0 : stats.create_burst_ns / post_attempts)
            << " set_header_avg_ns="
            << (post_attempts == 0 ? 0 : stats.set_header_ns / post_attempts)
            << " get_tx_burst_avg_ns="
            << (post_attempts == 0 ? 0 : stats.get_tx_burst_ns / post_attempts)
            << " set_lengths_avg_ns="
            << (post_attempts == 0 ? 0 : stats.set_lengths_ns / post_attempts)
            << " send_tx_burst_avg_ns="
            << (post_attempts == 0 ? 0 : stats.send_tx_burst_ns / post_attempts)
            << " completion_poll_avg_ns="
            << (stats.completion_polls == 0 ? 0 : stats.completion_poll_ns / stats.completion_polls)
            << '\n';
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
  RdmaWorkerStats server_stats;
  RdmaWorkerStats client_stats;
  daqiri::bench::TokenBucketPacer server_pacer(target_gbps);
  daqiri::bench::TokenBucketPacer client_pacer(target_gbps);
  bool run_server = false;
  bool run_client = false;

  if ((mode == "server" || mode == "both") && root["rdma_bench_server"]) {
    run_server = true;
    server_thread = std::thread(
        rdma_worker, parse_rdma_cfg(root["rdma_bench_server"]),
        std::ref(server_pacer), std::ref(stop), std::ref(server_stats));
  }
  if ((mode == "client" || mode == "both") && root["rdma_bench_client"]) {
    run_client = true;
    client_thread = std::thread(
        rdma_worker, parse_rdma_cfg(root["rdma_bench_client"]),
        std::ref(client_pacer), std::ref(stop), std::ref(client_stats));
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
    if (std::getenv("DAQIRI_RDMA_BENCH_PROFILE") != nullptr) {
      print_worker_profile("Server", server_stats);
    }
  }
  if (run_client) {
    std::cout << "Client complete: send_completions=" << client_stats.send_completions
              << " recv_completions=" << client_stats.recv_completions
              << " send_bytes=" << client_stats.send_bytes
              << " recv_bytes=" << client_stats.recv_bytes
              << " seconds=" << secs << '\n';
    if (std::getenv("DAQIRI_RDMA_BENCH_PROFILE") != nullptr) {
      print_worker_profile("Client", client_stats);
    }
  }

  daqiri::print_stats();
  daqiri::shutdown();
  return 0;
}
