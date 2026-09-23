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

struct NamedSenderTx {
  daqiri::bench::RawBenchTxConfig tx;
  daqiri::SenderId sender_id = daqiri::INVALID_SENDER_ID;
};

bool add_named_sender(const daqiri::bench::RawBenchTxConfig& tx, size_t index,
                      daqiri::SenderId* sender_id) {
  const auto src_ports = daqiri::bench::parse_udp_ports(tx.udp_src_port);
  const auto dst_ports = daqiri::bench::parse_udp_ports(tx.udp_dst_port);
  if (src_ports.size() != 1 || dst_ports.size() != 1) {
    std::cerr << "Named sender example requires exactly one UDP source and destination port\n";
    return false;
  }

  daqiri::RawUdpSenderConfig sender;
  sender.name_ = "example-sender-" + std::to_string(index);
  sender.interface_ = tx.interface_name;
  sender.dst_mac_ = tx.eth_dst_addr;
  sender.src_ipv4_ = tx.ip_src_addr;
  sender.dst_ipv4_ = tx.ip_dst_addr;
  sender.src_port_ = src_ports.front();
  sender.dst_port_ = dst_ports.front();
  // Named-sender MTU is the complete L2 frame size. The TX packet buffers
  // below still contain only payload bytes.
  sender.mtu_ = static_cast<uint32_t>(sizeof(daqiri::UDPIPV4Pkt)) + tx.payload_size;

  const auto status = daqiri::add_sender(sender, sender_id);
  if (status != daqiri::Status::SUCCESS) {
    std::cerr << "add_sender failed for " << sender.name_ << " (status " << static_cast<int>(status)
              << ")\n";
    return false;
  }

  daqiri::SenderId resolved = daqiri::INVALID_SENDER_ID;
  if (daqiri::get_sender_id(sender.name_, &resolved) != daqiri::Status::SUCCESS ||
      resolved != *sender_id) {
    std::cerr << "get_sender_id failed for " << sender.name_ << "\n";
    daqiri::delete_sender(*sender_id);
    *sender_id = daqiri::INVALID_SENDER_ID;
    return false;
  }
  return true;
}

void tx_worker(const NamedSenderTx& named, daqiri::bench::TokenBucketPacer& pacer,
               std::atomic<bool>& stop) {
  const auto& cfg = named.tx;
  if (!daqiri::bench::set_current_thread_affinity(cfg.cpu_core, "named_sender_tx")) {
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
      // The inline sender header is not part of segment 0 or its packet length.
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

    // Queue selection is per submission; the named sender contains only the
    // interface and cached destination/header fields.
    const auto status =
        daqiri::send_tx_burst(named.sender_id, static_cast<uint16_t>(cfg.queue_id), burst);
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

  std::vector<NamedSenderTx> named_senders;
  named_senders.reserve(tx_configs.size());
  for (size_t index = 0; index < tx_configs.size(); ++index) {
    NamedSenderTx named;
    named.tx = tx_configs[index];
    if (!add_named_sender(named.tx, index, &named.sender_id)) {
      for (const auto& added : named_senders) daqiri::delete_sender(added.sender_id);
      daqiri::shutdown();
      return 1;
    }
    named_senders.push_back(std::move(named));
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
  tx_threads.reserve(named_senders.size());
  for (const auto& named : named_senders) {
    tx_threads.emplace_back(tx_worker, std::cref(named), std::ref(tx_pacer), std::ref(stop));
  }

  daqiri::bench::wait_for_stop(run_seconds, stop);
  for (auto& thread : tx_threads) {
    if (thread.joinable()) thread.join();
  }
  for (auto& thread : rx_threads) {
    if (thread.joinable()) thread.join();
  }

  for (const auto& named : named_senders) daqiri::delete_sender(named.sender_id);
  daqiri::print_stats();
  daqiri::shutdown();
  return 0;
}
