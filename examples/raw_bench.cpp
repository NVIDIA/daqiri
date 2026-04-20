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
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "src/common.h"

namespace {

volatile std::sig_atomic_t g_stop_requested = 0;

void signal_handler(int signum) {
  if (signum == SIGINT) { g_stop_requested = 1; }
}

struct BenchTxConfig {
  std::string interface_name = "tx_port";
  bool split_boundary = false;
  bool gpu_direct = false;
  uint32_t batch_size = 1024;
  uint32_t payload_size = 1000;
  uint32_t header_size = 64;
  std::string udp_src_port = "4096";
  std::string udp_dst_port = "4096";
  std::string ip_src_addr = "1.2.3.4";
  std::string ip_dst_addr = "5.6.7.8";
  std::string eth_dst_addr = "00:00:00:00:00:00";
};

struct BenchRxConfig {
  std::string interface_name = "rx_port";
  bool split_boundary = false;
  bool gpu_direct = false;
  uint32_t batch_size = 1024;
  uint32_t max_packet_size = 1064;
  uint32_t header_size = 64;
};

std::vector<uint16_t> parse_udp_ports(const std::string& spec) {
  const auto dash = spec.find('-');
  if (dash == std::string::npos) { return {static_cast<uint16_t>(std::stoul(spec))}; }

  const uint16_t begin = static_cast<uint16_t>(std::stoul(spec.substr(0, dash)));
  const uint16_t end = static_cast<uint16_t>(std::stoul(spec.substr(dash + 1)));
  if (begin > end) { throw std::runtime_error("invalid UDP port range"); }

  std::vector<uint16_t> ports;
  ports.reserve(static_cast<size_t>(end - begin + 1));
  for (uint32_t p = begin; p <= end; ++p) { ports.push_back(static_cast<uint16_t>(p)); }
  return ports;
}

BenchTxConfig parse_tx(const YAML::Node& root) {
  BenchTxConfig cfg;
  if (!root["bench_tx"]) { return cfg; }
  const auto tx = root["bench_tx"];
  cfg.interface_name = tx["interface_name"].as<std::string>(cfg.interface_name);
  cfg.split_boundary = tx["split_boundary"].as<bool>(cfg.split_boundary);
  cfg.gpu_direct = tx["gpu_direct"].as<bool>(cfg.gpu_direct);
  cfg.batch_size = tx["batch_size"].as<uint32_t>(cfg.batch_size);
  cfg.payload_size = tx["payload_size"].as<uint32_t>(cfg.payload_size);
  cfg.header_size = tx["header_size"].as<uint32_t>(cfg.header_size);
  cfg.udp_src_port = tx["udp_src_port"].as<std::string>(cfg.udp_src_port);
  cfg.udp_dst_port = tx["udp_dst_port"].as<std::string>(cfg.udp_dst_port);
  cfg.ip_src_addr = tx["ip_src_addr"].as<std::string>(cfg.ip_src_addr);
  cfg.ip_dst_addr = tx["ip_dst_addr"].as<std::string>(cfg.ip_dst_addr);
  cfg.eth_dst_addr = tx["eth_dst_addr"].as<std::string>(cfg.eth_dst_addr);
  return cfg;
}

BenchRxConfig parse_rx(const YAML::Node& root) {
  BenchRxConfig cfg;
  if (!root["bench_rx"]) { return cfg; }
  const auto rx = root["bench_rx"];
  cfg.interface_name = rx["interface_name"].as<std::string>(cfg.interface_name);
  cfg.split_boundary = rx["split_boundary"].as<bool>(cfg.split_boundary);
  cfg.gpu_direct = rx["gpu_direct"].as<bool>(cfg.gpu_direct);
  cfg.batch_size = rx["batch_size"].as<uint32_t>(cfg.batch_size);
  cfg.max_packet_size = rx["max_packet_size"].as<uint32_t>(cfg.max_packet_size);
  cfg.header_size = rx["header_size"].as<uint32_t>(cfg.header_size);
  return cfg;
}

void populate_udp_ipv4_headers(
    uint8_t* pkt_data, uint32_t header_size, uint32_t payload_size, const char* eth_dst,
    uint32_t ip_src_host, uint32_t ip_dst_host, uint16_t src_port, uint16_t dst_port) {
  std::memset(pkt_data, 0, header_size + payload_size);

  auto* pkt = reinterpret_cast<daqiri::UDPIPV4Pkt*>(pkt_data);
  std::memcpy(pkt->eth.h_dest, eth_dst, ETH_ALEN);
  pkt->eth.h_proto = htons(ETH_P_IP);

  pkt->ip.version = 4;
  pkt->ip.ihl = 5;
  pkt->ip.protocol = IPPROTO_UDP;
  pkt->ip.tot_len = htons(static_cast<uint16_t>(payload_size + header_size - sizeof(ethhdr)));
  pkt->ip.saddr = htonl(ip_src_host);
  pkt->ip.daddr = htonl(ip_dst_host);

  pkt->udp.source = htons(src_port);
  pkt->udp.dest = htons(dst_port);
  pkt->udp.check = 0;
  pkt->udp.len = htons(static_cast<uint16_t>(
      payload_size + header_size - (sizeof(ethhdr) + sizeof(iphdr))));
}

void tx_worker(const BenchTxConfig& cfg, std::atomic<bool>& stop) {
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

  const auto src_ports = parse_udp_ports(cfg.udp_src_port);
  const auto dst_ports = parse_udp_ports(cfg.udp_dst_port);
  size_t src_idx = 0;
  size_t dst_idx = 0;

  std::vector<uint8_t> payload_template(cfg.payload_size, 0);
  for (size_t i = 0; i < payload_template.size(); ++i) {
    payload_template[i] = static_cast<uint8_t>(i & 0xff);
  }

  std::unordered_set<void*> initialized_tx_buffers;

  while (!stop.load()) {
    auto* msg = daqiri::create_tx_burst_params();
    daqiri::set_header(msg, static_cast<uint16_t>(port_id), 0, cfg.batch_size,
                       (cfg.gpu_direct && cfg.split_boundary) ? 2 : 1);

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
      const bool headers_on_cpu = !cfg.gpu_direct || cfg.split_boundary;
      void* buffer_key =
          headers_on_cpu ? msg->pkts[0][i] : daqiri::get_segment_packet_ptr(msg, 0, i);
      const bool first_use = initialized_tx_buffers.insert(buffer_key).second;

      if (headers_on_cpu && first_use) {
        if (daqiri::set_eth_header(msg, i, eth_dst) != daqiri::Status::SUCCESS ||
            daqiri::set_ipv4_header(
                msg, i, static_cast<int>(cfg.payload_size + cfg.header_size - (14 + 20)), 17, ip_src,
                ip_dst) != daqiri::Status::SUCCESS ||
            daqiri::set_udp_header(
                msg, i, static_cast<int>(cfg.payload_size + cfg.header_size - (14 + 20 + 8)),
                src_ports[src_idx], dst_ports[dst_idx]) != daqiri::Status::SUCCESS) {
          failed = true;
          break;
        }
      }

      const uint16_t src_port = src_ports[src_idx];
      const uint16_t dst_port = dst_ports[dst_idx];
      src_idx = (src_idx + 1) % src_ports.size();
      dst_idx = (dst_idx + 1) % dst_ports.size();

      if (!cfg.gpu_direct && !cfg.split_boundary && first_use) {
        if (daqiri::set_udp_payload(msg, i, payload_template.data(),
                                    static_cast<int>(cfg.payload_size)) != daqiri::Status::SUCCESS) {
          failed = true;
          break;
        }
      }

      if (cfg.gpu_direct && cfg.split_boundary) {
        if (first_use) {
          auto* gpu_payload = daqiri::get_segment_packet_ptr(msg, 1, i);
          cudaMemcpy(gpu_payload, payload_template.data(), cfg.payload_size, cudaMemcpyHostToDevice);
        }
        if (daqiri::set_packet_lengths(msg, i, {static_cast<int>(cfg.header_size),
                                                static_cast<int>(cfg.payload_size)}) !=
            daqiri::Status::SUCCESS) {
          failed = true;
          break;
        }
      } else if (cfg.gpu_direct) {
        if (first_use) {
          std::vector<uint8_t> packet_template(static_cast<size_t>(cfg.header_size) + cfg.payload_size);
          populate_udp_ipv4_headers(
              packet_template.data(), cfg.header_size, cfg.payload_size, eth_dst, ip_src, ip_dst,
              src_port, dst_port);
          std::memcpy(packet_template.data() + cfg.header_size, payload_template.data(),
                      cfg.payload_size);
          auto* gpu_pkt = daqiri::get_segment_packet_ptr(msg, 0, i);
          cudaMemcpy(
              gpu_pkt, packet_template.data(), packet_template.size(), cudaMemcpyHostToDevice);
        }
        if (daqiri::set_packet_lengths(msg, i,
                                       {static_cast<int>(cfg.header_size + cfg.payload_size)}) !=
            daqiri::Status::SUCCESS) {
          failed = true;
          break;
        }
      } else {
        if (daqiri::set_packet_lengths(msg, i,
                                       {static_cast<int>(cfg.header_size + cfg.payload_size)}) !=
            daqiri::Status::SUCCESS) {
          failed = true;
          break;
        }
      }
    }

    if (failed) {
      daqiri::free_all_packets_and_burst_tx(msg);
      continue;
    }
    daqiri::send_tx_burst(msg);
  }
}

void rx_worker(const BenchRxConfig& cfg, std::atomic<bool>& stop) {
  const int port_id = daqiri::get_port_id(cfg.interface_name);
  if (port_id < 0) {
    std::cerr << "Invalid RX interface_name: " << cfg.interface_name << "\n";
    stop.store(true);
    return;
  }

  uint64_t pkts = 0;
  uint64_t bytes = 0;
  while (!stop.load()) {
    const auto num_rx_queues = static_cast<int>(daqiri::get_num_rx_queues(port_id));
    bool got_any = false;
    for (int q = 0; q < num_rx_queues; ++q) {
      daqiri::BurstParams* burst = nullptr;
      if (daqiri::get_rx_burst(&burst, port_id, q) != daqiri::Status::SUCCESS || burst == nullptr) {
        continue;
      }
      got_any = true;
      const auto burst_size = daqiri::get_num_packets(burst);
      pkts += static_cast<uint64_t>(burst_size);
      bytes += daqiri::get_burst_tot_byte(burst);
      daqiri::free_all_packets_and_burst_rx(burst);
    }
    if (!got_any) { std::this_thread::sleep_for(std::chrono::microseconds(100)); }
  }

  std::cout << "RX complete: packets=" << pkts << " bytes=" << bytes << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <config.yaml> [--seconds N]\n";
    return 1;
  }

  int run_seconds = 10;
  for (int i = 2; i + 1 < argc; i += 2) {
    if (std::string(argv[i]) == "--seconds") { run_seconds = std::stoi(argv[i + 1]); }
  }

  const auto root = YAML::LoadFile(argv[1]);
  if (daqiri::daqiri_init(argv[1]) != daqiri::Status::SUCCESS) {
    std::cerr << "daqiri_init failed\n";
    return 1;
  }

  const bool has_rx = root["bench_rx"] && root["bench_rx"]["interface_name"];
  const bool has_tx = root["bench_tx"] && root["bench_tx"]["interface_name"];
  if (!has_rx && !has_tx) {
    std::cerr << "Config must define at least one of bench_rx or bench_tx\n";
    daqiri::shutdown();
    return 1;
  }

  std::atomic<bool> stop{false};
  std::thread tx_thread;
  std::thread rx_thread;

  if (has_rx) { rx_thread = std::thread(rx_worker, parse_rx(root), std::ref(stop)); }
  if (has_tx) { tx_thread = std::thread(tx_worker, parse_tx(root), std::ref(stop)); }

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

  if (tx_thread.joinable()) { tx_thread.join(); }
  if (rx_thread.joinable()) { rx_thread.join(); }

  daqiri::print_stats();
  daqiri::shutdown();
  return 0;
}
