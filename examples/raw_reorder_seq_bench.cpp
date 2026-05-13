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
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "raw_bench_common.h"
#include <daqiri/daqiri.h>

namespace {

struct SequenceTxConfig {
  daqiri::bench::RawBenchTxConfig packet;
  uint32_t sequence_number_offset = 0;
  uint32_t sequence_number_start = 0;
};

SequenceTxConfig parse_sequence_tx(const YAML::Node& root) {
  SequenceTxConfig cfg;
  cfg.packet = daqiri::bench::parse_tx(root);
  if (!root["bench_tx"]) { return cfg; }
  const auto tx = root["bench_tx"];
  cfg.sequence_number_offset =
      tx["sequence_number_offset"].as<uint32_t>(cfg.sequence_number_offset);
  cfg.sequence_number_start =
      tx["sequence_number_start"].as<uint32_t>(cfg.sequence_number_start);
  return cfg;
}

std::vector<std::pair<std::string, std::string>> parse_gpu_reorder_plans(
    const YAML::Node& root) {
  std::vector<std::pair<std::string, std::string>> plans;
  std::unordered_set<std::string> dedup;

  const auto cfg = root["daqiri"]["cfg"];
  const auto interfaces = cfg["interfaces"];
  if (!interfaces || !interfaces.IsSequence()) { return plans; }

  for (const auto& intf : interfaces) {
    const auto interface_name = intf["name"].as<std::string>("");
    const auto rx_node = intf["rx"];
    if (!rx_node || !rx_node.IsMap()) { continue; }
    const auto reorder_configs = rx_node["reorder_configs"];
    if (interface_name.empty() || !reorder_configs || !reorder_configs.IsSequence()) {
      continue;
    }

    for (const auto& reorder_cfg : reorder_configs) {
      const auto reorder_name = reorder_cfg["name"].as<std::string>("");
      const auto reorder_type = reorder_cfg["reorder_type"].as<std::string>("");
      if (reorder_name.empty() || reorder_type != "gpu") { continue; }

      const std::string key = interface_name + ":" + reorder_name;
      if (dedup.insert(key).second) { plans.emplace_back(interface_name, reorder_name); }
    }
  }

  return plans;
}

void tx_worker(const SequenceTxConfig& cfg, std::atomic<bool>& stop) {
  const int port_id = daqiri::get_port_id(cfg.packet.interface_name);
  if (port_id < 0) {
    std::cerr << "Invalid TX interface_name: " << cfg.packet.interface_name << "\n";
    stop.store(true);
    return;
  }

  if (cfg.sequence_number_offset + sizeof(uint32_t) > cfg.packet.payload_size) {
    std::cerr << "sequence_number_offset out of payload range\n";
    stop.store(true);
    return;
  }

  char eth_dst[6] = {0};
  daqiri::format_eth_addr(eth_dst, cfg.packet.eth_dst_addr);

  uint32_t ip_src = 0;
  uint32_t ip_dst = 0;
  inet_pton(AF_INET, cfg.packet.ip_src_addr.c_str(), &ip_src);
  inet_pton(AF_INET, cfg.packet.ip_dst_addr.c_str(), &ip_dst);
  ip_src = ntohl(ip_src);
  ip_dst = ntohl(ip_dst);

  const auto src_ports = daqiri::bench::parse_udp_ports(cfg.packet.udp_src_port);
  const auto dst_ports = daqiri::bench::parse_udp_ports(cfg.packet.udp_dst_port);
  size_t src_idx = 0;
  size_t dst_idx = 0;
  uint32_t next_sequence = cfg.sequence_number_start;

  while (!stop.load()) {
    auto* msg = daqiri::create_tx_burst_params();
    daqiri::set_header(msg, static_cast<uint16_t>(port_id), 0, cfg.packet.batch_size, 1);

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
              msg,
              i,
              static_cast<int>(cfg.packet.payload_size + cfg.packet.header_size - (14 + 20)),
              17,
              ip_src,
              ip_dst) != daqiri::Status::SUCCESS ||
          daqiri::set_udp_header(
              msg,
              i,
              static_cast<int>(
                  cfg.packet.payload_size + cfg.packet.header_size - (14 + 20 + 8)),
              src_port,
              dst_port) != daqiri::Status::SUCCESS) {
        failed = true;
        break;
      }

      auto* pkt_data = static_cast<uint8_t*>(daqiri::get_segment_packet_ptr(msg, 0, i));
      if (pkt_data == nullptr) {
        failed = true;
        break;
      }
      const uint32_t sequence_network_order = htonl(next_sequence++);
      std::memcpy(pkt_data + cfg.packet.header_size + cfg.sequence_number_offset,
                  &sequence_network_order,
                  sizeof(sequence_network_order));

      if (daqiri::set_packet_lengths(
              msg, i,
              {static_cast<int>(cfg.packet.header_size + cfg.packet.payload_size)}) !=
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

void rx_reorder_worker(const daqiri::bench::RawBenchRxConfig& cfg, std::atomic<bool>& stop) {
  const int port_id = daqiri::get_port_id(cfg.interface_name);
  if (port_id < 0) {
    std::cerr << "Invalid RX interface_name: " << cfg.interface_name << "\n";
    stop.store(true);
    return;
  }

  uint64_t pkts = 0;
  uint64_t bytes = 0;
  uint64_t bursts = 0;
  uint64_t aggregated_batches = 0;
  uint64_t timeout_batches = 0;
  uint64_t aggregated_packets = 0;
  uint64_t passthrough_packets = 0;
  uint64_t reorder_info_success = 0;
  uint64_t reorder_info_not_ready = 0;
  uint64_t reorder_info_errors = 0;
  uint64_t first_batch_id = 0;
  uint64_t last_batch_id = 0;
  bool have_batch_id = false;
  const bool check_reorder_info = std::getenv("DAQIRI_BENCH_CHECK_REORDER_INFO") != nullptr;
  while (!stop.load()) {
    const auto num_rx_queues = static_cast<int>(daqiri::get_num_rx_queues(port_id));
    bool got_any = false;
    for (int q = 0; q < num_rx_queues; ++q) {
      daqiri::BurstParams* burst = nullptr;
      if (daqiri::get_rx_burst(&burst, port_id, q) != daqiri::Status::SUCCESS ||
          burst == nullptr) {
        continue;
      }
      got_any = true;
      const auto burst_size = daqiri::get_num_packets(burst);
      const bool reordered =
          (burst->hdr.hdr.burst_flags & daqiri::DAQIRI_BURST_FLAG_REORDERED) != 0U;
      const bool timeout_flush =
          (burst->hdr.hdr.burst_flags & daqiri::DAQIRI_BURST_FLAG_REORDER_TIMEOUT) != 0U;
      const uint64_t logical_packets =
          reordered ? static_cast<uint64_t>(burst->hdr.hdr.max_pkt)
                    : static_cast<uint64_t>(burst_size);
      ++bursts;
      pkts += logical_packets;
      bytes += daqiri::get_burst_tot_byte(burst);
      if (reordered) {
        ++aggregated_batches;
        if (timeout_flush) { ++timeout_batches; }
        aggregated_packets += logical_packets;
        if (check_reorder_info) {
          if (burst->event != nullptr) { cudaEventSynchronize(burst->event); }
          daqiri::ReorderBurstInfo info{};
          const auto info_status = daqiri::get_reorder_burst_info(burst, &info);
          if (info_status == daqiri::Status::SUCCESS) {
            if (!have_batch_id) {
              first_batch_id = info.batch_id;
              have_batch_id = true;
            }
            last_batch_id = info.batch_id;
            ++reorder_info_success;
          } else if (info_status == daqiri::Status::NOT_READY) {
            ++reorder_info_not_ready;
          } else {
            ++reorder_info_errors;
          }
        }
      } else {
        passthrough_packets += logical_packets;
      }
      daqiri::free_all_packets_and_burst_rx(burst);
    }
    if (!got_any) { std::this_thread::sleep_for(std::chrono::microseconds(100)); }
  }

  std::cout << "RX complete: packets=" << pkts << " bytes=" << bytes << " bursts=" << bursts
            << " aggregated_batches=" << aggregated_batches
            << " timeout_batches=" << timeout_batches
            << " aggregated_packets=" << aggregated_packets
            << " passthrough_packets=" << passthrough_packets;
  if (check_reorder_info) {
    std::cout << " reorder_info_success=" << reorder_info_success
              << " reorder_info_not_ready=" << reorder_info_not_ready
              << " reorder_info_errors=" << reorder_info_errors
              << " first_batch_id=" << first_batch_id << " last_batch_id=" << last_batch_id;
  }
  std::cout << "\n";
}

}  // namespace

int main(int argc, char** argv) {
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

  cudaStream_t reorder_stream = nullptr;
  bool reorder_stream_initialized = false;
  const auto reorder_plans = parse_gpu_reorder_plans(root);
  if (!reorder_plans.empty()) {
    if (cudaStreamCreate(&reorder_stream) != cudaSuccess) {
      std::cerr << "Failed to create CUDA stream for RX reorder configs\n";
      daqiri::shutdown();
      return 1;
    }
    reorder_stream_initialized = true;

    for (const auto& [interface_name, reorder_name] : reorder_plans) {
      const auto st =
          daqiri::set_reorder_cuda_stream(interface_name, reorder_name, reorder_stream);
      if (st != daqiri::Status::SUCCESS) {
        std::cerr << "set_reorder_cuda_stream failed for interface=" << interface_name
                  << " reorder_name=" << reorder_name
                  << " status_code=" << static_cast<int>(st) << "\n";
        cudaStreamDestroy(reorder_stream);
        daqiri::shutdown();
        return 1;
      }
    }
  }

  const bool has_rx = daqiri::bench::has_bench_rx(root);
  const bool has_tx = daqiri::bench::has_bench_tx(root);
  if (!has_rx && !has_tx) {
    std::cerr << "Config must define at least one of bench_rx or bench_tx\n";
    if (reorder_stream_initialized) { cudaStreamDestroy(reorder_stream); }
    daqiri::shutdown();
    return 1;
  }

  std::atomic<bool> stop{false};
  std::thread tx_thread;
  std::thread rx_thread;

  if (has_rx) {
    rx_thread = std::thread(rx_reorder_worker, daqiri::bench::parse_rx(root), std::ref(stop));
  }
  if (has_tx) { tx_thread = std::thread(tx_worker, parse_sequence_tx(root), std::ref(stop)); }

  daqiri::bench::wait_for_stop(run_seconds, stop);

  if (tx_thread.joinable()) { tx_thread.join(); }
  if (rx_thread.joinable()) { rx_thread.join(); }

  daqiri::print_stats();
  if (reorder_stream_initialized) { cudaStreamDestroy(reorder_stream); }
  daqiri::shutdown();
  return 0;
}
