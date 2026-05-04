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
#include <iostream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "raw_bench_common.h"
#include "src/common.h"

namespace {

void tx_worker(const daqiri::bench::RawBenchTxConfig &cfg,
               std::atomic<bool> &stop) {
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

  while (!stop.load()) {
    auto *msg = daqiri::create_tx_burst_params();
    daqiri::set_header(msg, static_cast<uint16_t>(port_id), 0, cfg.batch_size,
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
              msg, i, {static_cast<int>(cfg.header_size + cfg.payload_size)}) !=
          daqiri::Status::SUCCESS) {
        failed = true;
        break;
      }
    }

    if (failed) {
      daqiri::free_all_packets_and_burst_tx(msg);
      continue;
    }
    daqiri::send_tx_burst(msg);
  }
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <config.yaml> [--seconds N]\n";
    return 1;
  }

  const int run_seconds = daqiri::bench::parse_run_seconds(argc, argv);
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
    rx_thread = std::thread(daqiri::bench::rx_count_worker,
                            daqiri::bench::parse_rx(root), std::ref(stop));
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
