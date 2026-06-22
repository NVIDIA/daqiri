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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "grafana/otel_prometheus.h"
#include "raw_bench_common.h"
#include <daqiri/daqiri.h>

namespace {

void tx_worker(const daqiri::bench::RawBenchTxConfig &cfg,
               daqiri::bench::TokenBucketPacer &pacer,
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
  char eth_src[6] = {0};
  daqiri::format_eth_addr(eth_src, cfg.eth_src_addr);
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

  std::unordered_set<void *> initialized_tx_buffers;
  daqiri::bench::RawBenchQueueStats stats;
  const auto packet_size =
      static_cast<uint64_t>(cfg.header_size) + cfg.payload_size;
  const auto t0 = std::chrono::steady_clock::now();

  while (!stop.load()) {
    auto *msg = daqiri::create_tx_burst_params();
    daqiri::set_header(msg, static_cast<uint16_t>(port_id),
                       static_cast<uint16_t>(cfg.queue_id), cfg.batch_size, 1);

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

      auto *gpu_pkt = daqiri::get_segment_packet_ptr(msg, 0, i);
      if (initialized_tx_buffers.insert(gpu_pkt).second) {
        std::vector<uint8_t> packet_template(
            static_cast<size_t>(cfg.header_size) + cfg.payload_size);
        daqiri::bench::populate_udp_ipv4_headers(
            packet_template.data(), cfg.header_size, cfg.payload_size, eth_src,
            eth_dst, ip_src, ip_dst, src_port, dst_port);
        std::memcpy(packet_template.data() + cfg.header_size,
                    payload_template.data(), cfg.payload_size);
        daqiri::bench::finalize_udp_ipv4_checksums(packet_template.data());
        if (cudaMemcpy(gpu_pkt, packet_template.data(), packet_template.size(),
                       cudaMemcpyHostToDevice) != cudaSuccess) {
          failed = true;
          break;
        }
      }

      if (daqiri::set_packet_lengths(
              msg, i, {static_cast<int>(packet_size)}) !=
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
      const uint64_t burst_bytes = static_cast<uint64_t>(num_pkts) * packet_size;
      stats.bytes += burst_bytes;
      ++stats.bursts;
      pacer.wait_for_bytes(burst_bytes, stop);
    }
  }

  const double secs =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
          .count();
  daqiri::bench::print_queue_stats("TX", cfg.interface_name, cfg.queue_id,
                                   stats, secs);
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0]
              << " <config.yaml> [--seconds N] [--target-gbps G] "
                 "[--workload none|fft|gemm|gemm_fp16]\n";
    return 1;
  }

  const auto prometheus_metrics =
      daqiri::bench::grafana::init_prometheus_metrics_from_env();
  const int run_seconds = daqiri::bench::parse_run_seconds(argc, argv);
  const double target_gbps = daqiri::bench::parse_target_gbps(argc, argv);
  const auto workload = daqiri::bench::parse_workload(argc, argv);
  const auto root = YAML::LoadFile(argv[1]);

  std::vector<daqiri::bench::RawBenchRxConfig> rx_configs;
  std::vector<daqiri::bench::RawBenchTxConfig> tx_configs;
  try {
    rx_configs = daqiri::bench::parse_rx_configs(root);
    tx_configs = daqiri::bench::parse_tx_configs(root);
  } catch (const std::exception &e) {
    std::cerr << "Invalid benchmark config: " << e.what() << "\n";
    return 1;
  }

  if (rx_configs.empty() && tx_configs.empty()) {
    std::cerr << "Config must define at least one of bench_rx or bench_tx\n";
    return 1;
  }

  if (daqiri::daqiri_init(argv[1]) != daqiri::Status::SUCCESS) {
    std::cerr << "daqiri_init failed\n";
    return 1;
  }

  std::atomic<bool> stop{false};
  std::vector<std::thread> tx_threads;
  std::vector<std::thread> rx_threads;
  daqiri::bench::TokenBucketPacer tx_pacer(target_gbps);

  // Size the per-burst GPU workload to the whole burst's data volume
  // (batch x payload), i.e. "process every byte received in the burst", so the
  // GPU load scales with the receive data rate and is actually visible.
  const size_t workload_bytes =
      tx_configs.empty()
          ? 0
          : static_cast<size_t>(tx_configs.front().payload_size) *
                tx_configs.front().batch_size;
  rx_threads.reserve(rx_configs.size());
  for (const auto &cfg : rx_configs) {
    rx_threads.emplace_back(daqiri::bench::rx_count_worker, cfg,
                            std::ref(stop), workload, workload_bytes);
  }
  tx_threads.reserve(tx_configs.size());
  for (const auto &cfg : tx_configs) {
    tx_threads.emplace_back(tx_worker, cfg, std::ref(tx_pacer), std::ref(stop));
  }

  daqiri::bench::wait_for_stop(run_seconds, stop);

  for (auto &thread : tx_threads) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  for (auto &thread : rx_threads) {
    if (thread.joinable()) {
      thread.join();
    }
  }

  daqiri::print_stats();
  daqiri::shutdown();
  if (prometheus_metrics) {
    daqiri::bench::grafana::shutdown_prometheus_metrics();
  }
  return 0;
}
