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

#include <arpa/inet.h>
#include <cuda_runtime.h>
#include <yaml-cpp/yaml.h>

#include <atomic>
#include <chrono>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "raw_bench_common.h"
#include <daqiri/daqiri.h>

namespace {

void tx_worker(const daqiri::bench::RawBenchTxConfig &cfg,
               std::atomic<bool> &stop) {
  if (!daqiri::bench::set_current_thread_affinity(cfg.cpu_core, "bench_tx")) {
    stop.store(true);
    return;
  }

  const int port_id = daqiri::get_port_id(cfg.interface_name);
  if (port_id < 0) {
    std::cerr << "Invalid TX interface_name: " << cfg.interface_name << "\n";
    stop.store(true);
    return;
  }

  char eth_dst[6] = {0};
  daqiri::format_eth_addr(eth_dst, cfg.eth_dst_addr);

  uint32_t ip_src = 0;
  uint32_t ip_dst = 0;
  inet_pton(AF_INET, cfg.ip_src_addr.c_str(), &ip_src);
  inet_pton(AF_INET, cfg.ip_dst_addr.c_str(), &ip_dst);
  ip_src = ntohl(ip_src);
  ip_dst = ntohl(ip_dst);

  const auto src_ports = daqiri::bench::parse_udp_ports(cfg.udp_src_port);
  const auto dst_ports = daqiri::bench::parse_udp_ports(cfg.udp_dst_port);
  size_t src_idx = 0;
  size_t dst_idx = 0;

  std::vector<uint8_t> payload_template(cfg.payload_size, 0);
  for (size_t i = 0; i < payload_template.size(); ++i) {
    payload_template[i] = static_cast<uint8_t>(i & 0xff);
  }

  std::unordered_set<void *> initialized_payload_buffers;
  daqiri::bench::RawBenchQueueStats stats;
  const auto t0 = std::chrono::steady_clock::now();

  while (!stop.load()) {
    auto *msg = daqiri::create_tx_burst_params();
    daqiri::set_header(msg, static_cast<uint16_t>(port_id),
                       static_cast<uint16_t>(cfg.queue_id), cfg.batch_size, 2);

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
    const auto num_pkts = static_cast<int>(daqiri::get_num_packets(msg));
    for (int i = 0; i < num_pkts; ++i) {
      const uint16_t src_port = src_ports[src_idx];
      const uint16_t dst_port = dst_ports[dst_idx];
      src_idx = (src_idx + 1) % src_ports.size();
      dst_idx = (dst_idx + 1) % dst_ports.size();

      if (daqiri::set_eth_header(msg, i, eth_dst) != daqiri::Status::SUCCESS ||
          daqiri::set_ipv4_header(
              msg, i,
              static_cast<int>(cfg.payload_size + cfg.header_size - (14 + 20)),
              17, ip_src, ip_dst) != daqiri::Status::SUCCESS ||
          daqiri::set_udp_header(
              msg, i,
              static_cast<int>(cfg.payload_size + cfg.header_size -
                               (14 + 20 + 8)),
              src_port, dst_port) != daqiri::Status::SUCCESS) {
        failed = true;
        break;
      }

      auto *gpu_payload = daqiri::get_segment_packet_ptr(msg, 1, i);
      if (initialized_payload_buffers.insert(gpu_payload).second) {
        if (cudaMemcpy(gpu_payload, payload_template.data(), cfg.payload_size,
                       cudaMemcpyHostToDevice) != cudaSuccess) {
          failed = true;
          break;
        }
      }

      if (daqiri::set_packet_lengths(msg, i,
                                     {static_cast<int>(cfg.header_size),
                                      static_cast<int>(cfg.payload_size)}) !=
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
      stats.packets += static_cast<uint64_t>(num_pkts);
      stats.bytes += static_cast<uint64_t>(num_pkts) *
                    static_cast<uint64_t>(cfg.header_size + cfg.payload_size);
      ++stats.bursts;
    }
  }

  const double secs =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
          .count();
  daqiri::bench::print_queue_stats("TX", cfg.interface_name, cfg.queue_id, stats,
                                   secs);
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0]
              << " <config.yaml> [--seconds N] "
                 "[--workload none|fft|gemm|gemm_fp16] [--workload-batch-bytes N]\n";
    return 1;
  }

  const int run_seconds = daqiri::bench::parse_run_seconds(argc, argv);
  const auto workload = daqiri::bench::parse_workload(argc, argv);
  const size_t workload_batch_bytes = daqiri::bench::parse_workload_batch_bytes(argc, argv);
  const int workload_gemm_n = daqiri::bench::parse_workload_gemm_n(argc, argv);
  const int workload_sync_interval = daqiri::bench::parse_workload_sync_interval(argc, argv);
  const auto root = YAML::LoadFile(argv[1]);
  if (daqiri::daqiri_init(argv[1]) != daqiri::Status::SUCCESS) {
    std::cerr << "daqiri_init failed\n";
    return 1;
  }

  const bool has_rx = daqiri::bench::has_bench_rx(root);
  const bool has_tx = daqiri::bench::has_bench_tx(root);
  if (!has_rx && !has_tx) {
    std::cerr << "Config must define at least one of bench_rx or bench_tx\n";
    daqiri::shutdown();
    return 1;
  }

  std::atomic<bool> stop{false};
  std::thread tx_thread;
  std::thread rx_thread;

  if (has_rx) {
    // HDS reorder geometry: the payload lives in segment 1 (GPU memory), so the
    // reorder reads it from offset 0 of that segment. The HDS TX does not inject
    // a per-packet sequence number, so the seq-based reorder still exercises the
    // kernel but mostly collides on slot 0 -- fine for a throughput benchmark
    // (the FLOP/copy volume is unchanged).
    daqiri::bench::ReorderGeometry geom;
    if (has_tx) {
      const auto tx = daqiri::bench::parse_tx(root);
      geom.payload_segment = 1;
      geom.payload_byte_offset = 0;
      geom.seq_bit_offset = 0;
      geom.seq_bit_width = 32;
      geom.out_payload_len = tx.payload_size;
      const uint32_t ppb =
          workload_batch_bytes > 0
              ? std::max<uint32_t>(1, static_cast<uint32_t>(workload_batch_bytes / tx.payload_size))
              : 1024;
      geom.packets_per_batch = std::min<uint32_t>(ppb, tx.batch_size);
    }
    rx_thread =
        std::thread(daqiri::bench::rx_count_worker, daqiri::bench::parse_rx(root), std::ref(stop),
                    workload, geom, workload_gemm_n, workload_sync_interval);
  }
  if (has_tx) {
    tx_thread =
        std::thread(tx_worker, daqiri::bench::parse_tx(root), std::ref(stop));
  }

  daqiri::bench::wait_for_stop(run_seconds, stop);

  if (tx_thread.joinable()) {
    tx_thread.join();
  }
  if (rx_thread.joinable()) {
    rx_thread.join();
  }

  daqiri::print_stats();
  daqiri::shutdown();
  return 0;
}
