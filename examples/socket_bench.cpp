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

#include <arpa/inet.h>

#include <algorithm>
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

// Detect the socket transport from the YAML: TCP is an in-order byte stream
// (gather-only), UDP datagrams can reorder (seq-based reorder). Scans the
// interfaces' socket_config.local_addr scheme.
bool socket_transport_is_tcp(const YAML::Node& root) {
  const auto ifaces = root["interfaces"];
  if (ifaces && ifaces.IsSequence()) {
    for (const auto& iface : ifaces) {
      const auto sc = iface["socket_config"];
      if (!sc) {
        continue;
      }
      const auto addr = sc["local_addr"].as<std::string>("");
      if (addr.rfind("tcp://", 0) == 0) {
        return true;
      }
      if (addr.rfind("udp://", 0) == 0) {
        return false;
      }
    }
  }
  return false;  // default to UDP semantics
}

void socket_worker(const SocketBenchConfig& cfg, daqiri::bench::TokenBucketPacer& pacer,
                   std::atomic<bool>& stop, SocketWorkerStats& stats,
                   daqiri::bench::BenchWorkload workload, bool is_tcp, int workload_gemm_dim,
                   int workload_sync_interval, int workload_fft_len) {
  const char *thread_name =
      cfg.server ? "socket_bench_server" : "socket_bench_client";
  if (!daqiri::bench::set_current_thread_affinity(cfg.cpu_core, thread_name)) {
    stop.store(true);
    return;
  }

  // Representative GPU workload on the REAL received data. Socket data lands in
  // pageable host memory, so the pipeline stages it to the GPU (a host->device
  // copy on the workload stream -- the honest cost of the socket path). UDP can
  // reorder by sequence number; TCP is in-order, so it gathers per chunk.
  const uint32_t msg = static_cast<uint32_t>(cfg.message_size);
  // The UDP reorder window is a fixed ~8 MB of datagrams (matching the RoCE
  // working set); TCP gathers one chunk at a time (one compute per recv). The GEMM
  // reads the first n*n*elem_size bytes of that window (dimension pinned below).
  const uint32_t target_bytes = 8u * 1024u * 1024u;
  const uint32_t packets_per_batch =
      is_tcp ? 1u : std::max(1u, std::min(8192u, target_bytes / std::max(1u, msg)));
  daqiri::bench::GpuWorkload gpu_workload;
  daqiri::bench::ReorderPipeline pipeline;
  if (workload != daqiri::bench::BenchWorkload::None) {
    if (!gpu_workload.init(workload, static_cast<size_t>(packets_per_batch) * msg,
                           workload_sync_interval, workload_gemm_dim, workload_fft_len) ||
        !pipeline.init(is_tcp ? daqiri::bench::ReorderMode::GatherOnly
                              : daqiri::bench::ReorderMode::SeqReorder,
                       packets_per_batch, msg, /*payload_byte_offset=*/0,
                       /*seq_bit_offset=*/0, /*seq_bit_width=*/is_tcp ? 0 : 32,
                       /*staging_needed=*/true, gpu_workload.stream())) {
      std::cerr << "Socket workload init failed; continuing without GPU workload\n";
    }
  }
  const bool run_workload = gpu_workload.enabled() && pipeline.enabled();

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
        // Inject a sequence number (network byte order) at the payload start so
        // the UDP RX-side reorder has a real seq to sort by. Harmless for TCP.
        if (cfg.message_size >= static_cast<int>(sizeof(uint32_t))) {
          const uint32_t seq = htonl(static_cast<uint32_t>(stats.sent_packets));
          std::memcpy(payload, &seq, sizeof(seq));
        }
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
        const int num_pkts = static_cast<int>(daqiri::get_num_packets(burst));
        stats.received_packets += static_cast<uint64_t>(num_pkts);
        stats.received_bytes += daqiri::get_burst_tot_byte(burst);

        if (run_workload) {
          // Stage each received (host) payload to the GPU; the copy persists in
          // the pipeline's device buffer, so a batch can accumulate across bursts
          // (UDP bursts are one datagram). When a batch fills, reorder/gather it
          // and run the compute. TCP (packets_per_batch == 1) flushes per chunk.
          for (int i = 0; i < num_pkts; ++i) {
            const auto len = daqiri::get_packet_length(burst, i);
            void* d = pipeline.stage_host_packet(daqiri::get_packet_ptr(burst, i), len);
            pipeline.add_device_packet(d);
            if (pipeline.collected() == packets_per_batch) {
              gpu_workload.run(pipeline.finish_batch());
              pipeline.reset_batch();
            }
          }
          // Drain so the host->device staging copies (which read this burst's
          // memory) complete before the burst is freed/recycled.
          gpu_workload.sync();
        }
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
              << " <config.yaml> [--seconds N] [--mode server|client|both] "
                 "[--target-gbps G] [--workload none|fft|gemm|gemm_fp16] "
                 "[--workload-gemm-dim N] [--workload-fft-len N] "
                 "[--workload-sync-interval N]\n";
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
  const int workload_gemm_dim = daqiri::bench::parse_workload_gemm_dim(argc, argv);
  const int workload_fft_len = daqiri::bench::parse_workload_fft_len(argc, argv);
  const int workload_sync_interval = daqiri::bench::parse_workload_sync_interval(argc, argv);

  const auto root = YAML::LoadFile(argv[1]);
  const bool is_tcp = socket_transport_is_tcp(root);
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
    server_thread = std::thread(socket_worker, server_cfg, std::ref(server_pacer), std::ref(stop),
                                std::ref(server_stats), workload, is_tcp, workload_gemm_dim,
                                workload_sync_interval, workload_fft_len);
  }
  if (run_client) {
    client_thread = std::thread(socket_worker, client_cfg, std::ref(client_pacer), std::ref(stop),
                                std::ref(client_stats), workload, is_tcp, workload_gemm_dim,
                                workload_sync_interval, workload_fft_len);
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
