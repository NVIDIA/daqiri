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
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "raw_bench_common.h"
#include <daqiri/daqiri.h>

namespace {

struct NamedEndpointTx {
  daqiri::bench::RawBenchTxConfig tx;
  daqiri::EndpointId endpoint_id = daqiri::INVALID_ENDPOINT_ID;
};

bool add_named_endpoint(const daqiri::bench::RawBenchTxConfig& tx, size_t index,
                        daqiri::EndpointId* endpoint_id) {
  const auto src_ports = daqiri::bench::parse_udp_ports(tx.udp_src_port);
  const auto dst_ports = daqiri::bench::parse_udp_ports(tx.udp_dst_port);
  if (src_ports.size() != 1 || dst_ports.size() != 1) {
    std::cerr << "Named endpoints example requires exactly one UDP source and destination port\n";
    return false;
  }

  daqiri::RawUdpEndpointConfig endpoint;
  endpoint.name_ = "example-endpoint-" + std::to_string(index);
  endpoint.interface_ = tx.interface_name;
  endpoint.dst_mac_ = tx.eth_dst_addr;
  endpoint.src_ipv4_ = tx.ip_src_addr;
  endpoint.dst_ipv4_ = tx.ip_dst_addr;
  endpoint.src_port_ = src_ports.front();
  endpoint.dst_port_ = dst_ports.front();
  // Named-endpoint MTU is the complete L2 frame size. The TX packet buffers
  // below still contain only payload bytes.
  endpoint.mtu_ = static_cast<uint32_t>(sizeof(daqiri::UDPIPV4Pkt)) + tx.payload_size;

  const auto status = daqiri::add_endpoint(endpoint, endpoint_id);
  if (status != daqiri::Status::SUCCESS) {
    std::cerr << "add_endpoint failed for " << endpoint.name_ << " (status "
              << static_cast<int>(status) << ")\n";
    return false;
  }

  daqiri::EndpointId resolved = daqiri::INVALID_ENDPOINT_ID;
  if (daqiri::get_endpoint_id(endpoint.name_, &resolved) != daqiri::Status::SUCCESS ||
      resolved != *endpoint_id) {
    std::cerr << "get_endpoint_id failed for " << endpoint.name_ << "\n";
    daqiri::delete_endpoint(*endpoint_id);
    *endpoint_id = daqiri::INVALID_ENDPOINT_ID;
    return false;
  }
  return true;
}

void tx_worker(const NamedEndpointTx& endpoint, daqiri::bench::TokenBucketPacer& pacer,
               std::atomic<bool>& stop) {
  const auto& cfg = endpoint.tx;
  if (!daqiri::bench::set_current_thread_affinity(cfg.cpu_core, "named_endpoint_tx")) {
    stop.store(true);
    return;
  }

  const int port_id = daqiri::get_port_id(cfg.interface_name);
  if (port_id < 0) {
    std::cerr << "Invalid TX interface_name: " << cfg.interface_name << "\n";
    stop.store(true);
    return;
  }

  std::vector<uint8_t> payload_template(cfg.payload_size, 0);
  for (size_t i = 0; i < payload_template.size(); ++i) {
    payload_template[i] = static_cast<uint8_t>(i & 0xff);
  }

  std::unordered_set<void*> initialized_tx_buffers;
  daqiri::bench::RawBenchQueueStats stats;
  const uint64_t wire_packet_size = sizeof(daqiri::UDPIPV4Pkt) + cfg.payload_size;
  const auto t0 = std::chrono::steady_clock::now();

  while (!stop.load()) {
    auto* burst = daqiri::create_tx_burst_params();
    daqiri::set_header(burst, static_cast<uint16_t>(port_id), static_cast<uint16_t>(cfg.queue_id),
                       cfg.batch_size, 1);

    if (!daqiri::is_tx_burst_available(burst)) {
      daqiri::free_tx_metadata(burst);
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      continue;
    }
    if (daqiri::get_tx_packet_burst(burst) != daqiri::Status::SUCCESS) {
      daqiri::free_tx_metadata(burst);
      continue;
    }

    bool failed = false;
    const auto num_pkts = static_cast<int>(daqiri::get_num_packets(burst));
    for (int packet = 0; packet < num_pkts; ++packet) {
      void* payload = daqiri::get_segment_packet_ptr(burst, 0, packet);
      if (initialized_tx_buffers.insert(payload).second) {
        std::vector<uint8_t> initialized_payload = payload_template;
        if (initialized_payload.size() >= sizeof(uint32_t)) {
          const uint32_t sequence = htonl(static_cast<uint32_t>(packet));
          std::memcpy(initialized_payload.data(), &sequence, sizeof(sequence));
        }
        if (cudaMemcpy(payload, initialized_payload.data(), initialized_payload.size(),
                       cudaMemcpyDefault) != cudaSuccess) {
          failed = true;
          break;
        }
      }
      // The inline endpoint header is not part of segment 0 or its packet length.
      if (daqiri::set_packet_lengths(burst, packet, {static_cast<int>(cfg.payload_size)}) !=
          daqiri::Status::SUCCESS) {
        failed = true;
        break;
      }
    }

    if (failed) {
      daqiri::free_all_packets_and_burst_tx(burst);
      stop.store(true);
      break;
    }

    // Queue selection is per submission; the named endpoint contains only the
    // interface and cached destination/header fields.
    const auto status =
        daqiri::send_tx_burst(endpoint.endpoint_id, static_cast<uint16_t>(cfg.queue_id), burst);
    if (status == daqiri::Status::SUCCESS) {
      stats.packets += static_cast<uint64_t>(num_pkts);
      const uint64_t burst_bytes = static_cast<uint64_t>(num_pkts) * wire_packet_size;
      stats.bytes += burst_bytes;
      ++stats.bursts;
      pacer.wait_for_bytes(burst_bytes, stop);
    } else if (status != daqiri::Status::NO_SPACE_AVAILABLE) {
      std::cerr << "named send_tx_burst failed (status " << static_cast<int>(status) << ")\n";
      daqiri::free_all_packets_and_burst_tx(burst);
      stop.store(true);
      break;
    }
  }

  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  daqiri::bench::print_queue_stats("TX", cfg.interface_name, cfg.queue_id, stats, seconds);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0]
              << " <config.yaml> [--seconds N] [--target-gbps G] "
                 "[--workload none|fft|gemm|gemm_fp16] [--workload-gemm-dim N] "
                 "[--workload-fft-len N] [--workload-sync-interval N]\n";
    return 1;
  }

  const int run_seconds = daqiri::bench::parse_run_seconds(argc, argv);
  const double target_gbps = daqiri::bench::parse_target_gbps(argc, argv);
  const auto workload = daqiri::bench::parse_workload(argc, argv);
  const int workload_gemm_dim = daqiri::bench::parse_workload_gemm_dim(argc, argv);
  const int workload_fft_len = daqiri::bench::parse_workload_fft_len(argc, argv);
  const int workload_sync_interval = daqiri::bench::parse_workload_sync_interval(argc, argv);
  const auto root = YAML::LoadFile(argv[1]);

  std::vector<daqiri::bench::RawBenchRxConfig> rx_configs;
  std::vector<daqiri::bench::RawBenchTxConfig> tx_configs;
  try {
    rx_configs = daqiri::bench::parse_rx_configs(root);
    tx_configs = daqiri::bench::parse_tx_configs(root);
  } catch (const std::exception& error) {
    std::cerr << "Invalid example config: " << error.what() << "\n";
    return 1;
  }
  if (tx_configs.empty()) {
    std::cerr << "Config must define at least one bench_tx entry\n";
    return 1;
  }

  if (daqiri::daqiri_init(argv[1]) != daqiri::Status::SUCCESS) {
    std::cerr << "daqiri_init failed\n";
    return 1;
  }

  std::vector<NamedEndpointTx> named_endpoints;
  named_endpoints.reserve(tx_configs.size());
  for (size_t index = 0; index < tx_configs.size(); ++index) {
    NamedEndpointTx endpoint;
    endpoint.tx = tx_configs[index];
    if (!add_named_endpoint(endpoint.tx, index, &endpoint.endpoint_id)) {
      for (const auto& added : named_endpoints) daqiri::delete_endpoint(added.endpoint_id);
      daqiri::shutdown();
      return 1;
    }
    named_endpoints.push_back(std::move(endpoint));
  }

  std::atomic<bool> stop{false};
  std::vector<std::thread> tx_threads;
  std::vector<std::thread> rx_threads;
  daqiri::bench::TokenBucketPacer tx_pacer(target_gbps);

  daqiri::bench::ReorderGeometry geometry;
  geometry.payload_segment = 0;
  geometry.payload_byte_offset = sizeof(daqiri::UDPIPV4Pkt);
  geometry.seq_bit_offset = static_cast<uint16_t>(sizeof(daqiri::UDPIPV4Pkt) * 8);
  geometry.seq_bit_width = 32;
  geometry.out_payload_len = tx_configs.front().payload_size;
  geometry.packets_per_batch = std::min<uint32_t>(1024, tx_configs.front().batch_size);

  rx_threads.reserve(rx_configs.size());
  for (const auto& cfg : rx_configs) {
    rx_threads.emplace_back(daqiri::bench::rx_count_worker, cfg, std::ref(stop), workload, geometry,
                            workload_gemm_dim, workload_sync_interval, workload_fft_len);
  }
  tx_threads.reserve(named_endpoints.size());
  for (const auto& endpoint : named_endpoints) {
    tx_threads.emplace_back(tx_worker, std::cref(endpoint), std::ref(tx_pacer), std::ref(stop));
  }

  daqiri::bench::wait_for_stop(run_seconds, stop);
  for (auto& thread : tx_threads) {
    if (thread.joinable()) thread.join();
  }
  for (auto& thread : rx_threads) {
    if (thread.joinable()) thread.join();
  }

  for (const auto& endpoint : named_endpoints) daqiri::delete_endpoint(endpoint.endpoint_id);
  daqiri::print_stats();
  daqiri::shutdown();
  return 0;
}
