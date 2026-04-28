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

#include "raw_bench_common.h"

#include <arpa/inet.h>

#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

#include "src/common.h"

namespace daqiri::bench {
namespace {

volatile std::sig_atomic_t g_stop_requested = 0;

}  // namespace

PinnedHostBuffer::PinnedHostBuffer(PinnedHostBuffer&& other) noexcept {
  ptr_ = other.ptr_;
  capacity_ = other.capacity_;
  other.ptr_ = nullptr;
  other.capacity_ = 0;
}

PinnedHostBuffer& PinnedHostBuffer::operator=(PinnedHostBuffer&& other) noexcept {
  if (this != &other) {
    reset();
    ptr_ = other.ptr_;
    capacity_ = other.capacity_;
    other.ptr_ = nullptr;
    other.capacity_ = 0;
  }
  return *this;
}

PinnedHostBuffer::~PinnedHostBuffer() { reset(); }

bool PinnedHostBuffer::resize(size_t size) {
  if (size <= capacity_) { return true; }
  reset();
  if (size == 0) { return true; }
  if (cudaHostAlloc(&ptr_, size, cudaHostAllocDefault) != cudaSuccess) { return false; }
  capacity_ = size;
  return true;
}

void PinnedHostBuffer::reset() {
  if (ptr_ != nullptr) {
    cudaFreeHost(ptr_);
    ptr_ = nullptr;
    capacity_ = 0;
  }
}

uint8_t* PinnedHostBuffer::data() { return static_cast<uint8_t*>(ptr_); }

const uint8_t* PinnedHostBuffer::data() const { return static_cast<const uint8_t*>(ptr_); }

size_t PinnedHostBuffer::capacity() const { return capacity_; }

int parse_run_seconds(int argc, char** argv) {
  int run_seconds = 10;
  for (int i = 2; i + 1 < argc; i += 2) {
    if (std::string(argv[i]) == "--seconds") { run_seconds = std::stoi(argv[i + 1]); }
  }
  return run_seconds;
}

bool has_bench_rx(const YAML::Node& root) {
  return root["bench_rx"] && root["bench_rx"]["interface_name"];
}

bool has_bench_tx(const YAML::Node& root) {
  return root["bench_tx"] && root["bench_tx"]["interface_name"];
}

RawBenchRxConfig parse_rx(const YAML::Node& root) {
  RawBenchRxConfig cfg;
  if (!root["bench_rx"]) { return cfg; }
  const auto rx = root["bench_rx"];
  cfg.interface_name = rx["interface_name"].as<std::string>(cfg.interface_name);
  return cfg;
}

RawBenchTxConfig parse_tx(const YAML::Node& root) {
  RawBenchTxConfig cfg;
  if (!root["bench_tx"]) { return cfg; }
  const auto tx = root["bench_tx"];
  cfg.interface_name = tx["interface_name"].as<std::string>(cfg.interface_name);
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

void populate_udp_ipv4_headers(uint8_t* pkt_data,
                               uint32_t header_size,
                               uint32_t payload_size,
                               const char* eth_dst,
                               uint32_t ip_src_host,
                               uint32_t ip_dst_host,
                               uint16_t src_port,
                               uint16_t dst_port) {
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
  pkt->udp.len = htons(
      static_cast<uint16_t>(payload_size + header_size - (sizeof(ethhdr) + sizeof(iphdr))));
}

cudaError_t memcpy_batch_async(const std::vector<void*>& dsts,
                               const std::vector<const void*>& srcs,
                               const std::vector<size_t>& sizes,
                               cudaStream_t stream) {
  if (dsts.empty()) { return cudaSuccess; }

  cudaMemcpyAttributes attr{};
  attr.srcAccessOrder = cudaMemcpySrcAccessOrderStream;
  attr.flags = cudaMemcpyFlagDefault;
  size_t attr_idx = 0;
  return cudaMemcpyBatchAsync(
      dsts.data(), srcs.data(), sizes.data(), dsts.size(), &attr, &attr_idx, 1, stream);
}

void signal_handler(int signum) {
  if (signum == SIGINT) { g_stop_requested = 1; }
}

void wait_for_stop(int run_seconds, std::atomic<bool>& stop) {
  std::signal(SIGINT, signal_handler);
  auto start = std::chrono::steady_clock::now();
  while (!g_stop_requested) {
    if (run_seconds > 0) {
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start);
      if (elapsed.count() >= run_seconds) { break; }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  stop.store(true);
}

void rx_count_worker(const RawBenchRxConfig& cfg, std::atomic<bool>& stop) {
  const int port_id = daqiri::get_port_id(cfg.interface_name);
  if (port_id < 0) {
    std::cerr << "Invalid RX interface_name: " << cfg.interface_name << "\n";
    stop.store(true);
    return;
  }

  uint64_t pkts = 0;
  uint64_t bytes = 0;
  uint64_t bursts = 0;
  while (!stop.load()) {
    const auto num_rx_queues = static_cast<int>(daqiri::get_num_rx_queues(port_id));
    bool got_any = false;
    for (int q = 0; q < num_rx_queues; ++q) {
      daqiri::BurstParams* burst = nullptr;
      if (daqiri::get_rx_burst(&burst, port_id, q) != daqiri::Status::SUCCESS || burst == nullptr) {
        continue;
      }
      got_any = true;
      pkts += static_cast<uint64_t>(daqiri::get_num_packets(burst));
      bytes += daqiri::get_burst_tot_byte(burst);
      ++bursts;
      daqiri::free_all_packets_and_burst_rx(burst);
    }
    if (!got_any) { std::this_thread::sleep_for(std::chrono::microseconds(100)); }
  }

  std::cout << "RX complete: packets=" << pkts << " bytes=" << bytes << " bursts=" << bursts
            << "\n";
}

}  // namespace daqiri::bench
