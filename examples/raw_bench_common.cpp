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

#include "raw_bench_common.h"

#include <arpa/inet.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <daqiri/daqiri.h>

namespace daqiri::bench {
namespace {

volatile std::sig_atomic_t g_stop_requested = 0;
std::mutex g_stats_print_mutex;

bool has_bench_config(const YAML::Node &root, const char *key) {
  const auto node = root[key];
  if (!node) {
    return false;
  }
  if (node.IsSequence()) {
    for (const auto &item : node) {
      if (item && item.IsMap() && item["interface_name"]) {
        return true;
      }
    }
    return false;
  }
  return node.IsMap() && node["interface_name"];
}

std::vector<int> queue_ids_for_interface(const YAML::Node &root,
                                         const std::string &interface_name,
                                         const char *direction) {
  const auto cfg = root["daqiri"]["cfg"];
  const auto interfaces = cfg["interfaces"];
  if (!interfaces || !interfaces.IsSequence()) {
    return {};
  }

  for (const auto &intf : interfaces) {
    if (intf["name"].as<std::string>("") != interface_name) {
      continue;
    }

    std::vector<int> queue_ids;
    const auto queues = intf[direction]["queues"];
    if (!queues || !queues.IsSequence()) {
      return queue_ids;
    }

    queue_ids.reserve(queues.size());
    for (const auto &queue : queues) {
      queue_ids.push_back(queue["id"].as<int>());
    }
    return queue_ids;
  }

  return {};
}

void validate_queue_id(int queue_id, const std::vector<int> &queue_ids,
                       const std::string &bench_key,
                       const std::string &interface_name) {
  if (queue_id < 0 || queue_id > std::numeric_limits<uint16_t>::max()) {
    throw std::runtime_error(bench_key + " queue_id is out of range for " +
                             interface_name);
  }

  if (!queue_ids.empty() && std::find(queue_ids.begin(), queue_ids.end(),
                                      queue_id) == queue_ids.end()) {
    throw std::runtime_error(bench_key + " queue_id " +
                             std::to_string(queue_id) +
                             " does not exist on interface " + interface_name);
  }
}

int assign_queue_id(const YAML::Node &root, const YAML::Node &item,
                    const char *direction, const char *bench_key,
                    std::unordered_map<std::string, size_t> &next_queue_idx,
                    std::unordered_map<std::string, size_t> &entry_counts,
                    std::unordered_map<std::string, std::unordered_set<int>>
                        &assigned_queue_ids) {
  const auto interface_name = item["interface_name"].as<std::string>();
  const auto queue_ids =
      queue_ids_for_interface(root, interface_name, direction);
  if (queue_ids.empty()) {
    throw std::runtime_error(std::string(bench_key) +
                             " references interface '" + interface_name +
                             "' with no DAQIRI " + direction + " queues");
  }

  ++entry_counts[interface_name];

  int queue_id = 0;
  if (item["queue_id"]) {
    queue_id = item["queue_id"].as<int>();
    validate_queue_id(queue_id, queue_ids, bench_key, interface_name);
  } else {
    const size_t idx = next_queue_idx[interface_name]++;
    if (idx >= queue_ids.size()) {
      throw std::runtime_error(
          std::string(bench_key) + " has more entries for interface '" +
          interface_name + "' than DAQIRI " + direction + " queues");
    }
    queue_id = queue_ids[idx];
  }

  if (!assigned_queue_ids[interface_name].insert(queue_id).second) {
    throw std::runtime_error(
        std::string(bench_key) + " queue_id " + std::to_string(queue_id) +
        " is configured more than once for interface '" + interface_name + "'");
  }
  return queue_id;
}

void validate_bench_list_sizes(
    const YAML::Node &root, const char *direction, const char *bench_key,
    const std::unordered_map<std::string, size_t> &entry_counts) {
  for (const auto &[interface_name, count] : entry_counts) {
    const auto queue_ids =
        queue_ids_for_interface(root, interface_name, direction);
    if (!queue_ids.empty() && count != queue_ids.size()) {
      throw std::runtime_error(
          std::string(bench_key) + " entries for interface '" + interface_name +
          "' must match the DAQIRI " + direction + " queue count (" +
          std::to_string(count) + " entries, " +
          std::to_string(queue_ids.size()) + " queues)");
    }
  }
}

RawBenchRxConfig parse_rx_item(const YAML::Node &rx) {
  RawBenchRxConfig cfg;
  cfg.interface_name = rx["interface_name"].as<std::string>(cfg.interface_name);
  cfg.queue_id = rx["queue_id"].as<int>(cfg.queue_id);
  return cfg;
}

RawBenchTxConfig parse_tx_item(const YAML::Node &tx) {
  RawBenchTxConfig cfg;
  cfg.interface_name = tx["interface_name"].as<std::string>(cfg.interface_name);
  cfg.queue_id = tx["queue_id"].as<int>(cfg.queue_id);
  cfg.batch_size = tx["batch_size"].as<uint32_t>(cfg.batch_size);
  cfg.payload_size = tx["payload_size"].as<uint32_t>(cfg.payload_size);
  cfg.header_size = tx["header_size"].as<uint32_t>(cfg.header_size);
  cfg.udp_src_port = tx["udp_src_port"].as<std::string>(cfg.udp_src_port);
  cfg.udp_dst_port = tx["udp_dst_port"].as<std::string>(cfg.udp_dst_port);
  cfg.ip_src_addr = tx["ip_src_addr"].as<std::string>(cfg.ip_src_addr);
  cfg.ip_dst_addr = tx["ip_dst_addr"].as<std::string>(cfg.ip_dst_addr);
  cfg.eth_src_addr = tx["eth_src_addr"].as<std::string>(cfg.eth_src_addr);
  cfg.eth_dst_addr = tx["eth_dst_addr"].as<std::string>(cfg.eth_dst_addr);
  validate_queue_id(cfg.queue_id, {}, "bench_tx", cfg.interface_name);
  return cfg;
}

} // namespace

PinnedHostBuffer::PinnedHostBuffer(PinnedHostBuffer &&other) noexcept {
  ptr_ = other.ptr_;
  capacity_ = other.capacity_;
  other.ptr_ = nullptr;
  other.capacity_ = 0;
}

PinnedHostBuffer &
PinnedHostBuffer::operator=(PinnedHostBuffer &&other) noexcept {
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
  if (size <= capacity_) {
    return true;
  }
  reset();
  if (size == 0) {
    return true;
  }
  // Batched TX copies may launch a CUDA kernel that reads from this staging
  // buffer, so pinned host allocations must be device-mappable.
  if (cudaHostAlloc(&ptr_, size, cudaHostAllocMapped) != cudaSuccess) {
    return false;
  }
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

uint8_t *PinnedHostBuffer::data() { return static_cast<uint8_t *>(ptr_); }

const uint8_t *PinnedHostBuffer::data() const {
  return static_cast<const uint8_t *>(ptr_);
}

size_t PinnedHostBuffer::capacity() const { return capacity_; }

int parse_run_seconds(int argc, char **argv) {
  int run_seconds = 10;
  for (int i = 2; i + 1 < argc; i += 2) {
    if (std::string(argv[i]) == "--seconds") {
      run_seconds = std::stoi(argv[i + 1]);
    }
  }
  return run_seconds;
}

double parse_target_gbps(int argc, char **argv) {
  double target_gbps = 0.0;
  for (int i = 2; i + 1 < argc; i += 2) {
    if (std::string(argv[i]) == "--target-gbps") {
      target_gbps = std::stod(argv[i + 1]);
    }
  }
  return target_gbps;
}

TokenBucketPacer::TokenBucketPacer(double target_gbps)
    : target_bps_(target_gbps > 0.0 ? target_gbps * 1e9 : 0.0),
      t0_(std::chrono::steady_clock::now()) {}

void TokenBucketPacer::wait_for_bytes(size_t bytes, std::atomic<bool> &stop) {
  if (target_bps_ <= 0.0) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  total_bytes_ += bytes;
  const double scheduled_secs = (total_bytes_ * 8.0) / target_bps_;
  const auto scheduled = t0_ + std::chrono::duration_cast<
                                   std::chrono::steady_clock::duration>(
                                   std::chrono::duration<double>(scheduled_secs));
  // Slice the wait into 10 ms chunks so a stop flag (--seconds expiry or
  // Ctrl-C) can break us out promptly. The total slept across the slices
  // accumulates to the scheduled deadline, so pacing remains accurate.
  constexpr auto kSlice = std::chrono::milliseconds(10);
  while (!stop.load()) {
    const auto now = std::chrono::steady_clock::now();
    if (scheduled <= now) {
      return;
    }
    const auto remaining = scheduled - now;
    std::this_thread::sleep_for(
        std::min<std::chrono::steady_clock::duration>(remaining, kSlice));
  }
}

bool has_bench_rx(const YAML::Node &root) {
  return has_bench_config(root, "bench_rx");
}

bool has_bench_tx(const YAML::Node &root) {
  return has_bench_config(root, "bench_tx");
}

RawBenchRxConfig parse_rx(const YAML::Node &root) {
  const auto configs = parse_rx_configs(root);
  if (configs.empty()) {
    return {};
  }
  return configs.front();
}

RawBenchTxConfig parse_tx(const YAML::Node &root) {
  const auto configs = parse_tx_configs(root);
  if (configs.empty()) {
    return {};
  }
  return configs.front();
}

std::vector<RawBenchRxConfig> parse_rx_configs(const YAML::Node &root) {
  std::vector<RawBenchRxConfig> configs;
  const auto bench_rx = root["bench_rx"];
  if (!bench_rx) {
    return configs;
  }

  if (!bench_rx.IsSequence()) {
    configs.push_back(parse_rx_item(bench_rx));
    return configs;
  }

  std::unordered_map<std::string, size_t> next_queue_idx;
  std::unordered_map<std::string, size_t> entry_counts;
  std::unordered_map<std::string, std::unordered_set<int>> assigned_queue_ids;
  configs.reserve(bench_rx.size());
  for (const auto &item : bench_rx) {
    if (!item || !item.IsMap() || !item["interface_name"]) {
      throw std::runtime_error(
          "bench_rx list entries must be maps with interface_name");
    }
    auto cfg = parse_rx_item(item);
    cfg.queue_id = assign_queue_id(root, item, "rx", "bench_rx", next_queue_idx,
                                   entry_counts, assigned_queue_ids);
    configs.push_back(std::move(cfg));
  }
  validate_bench_list_sizes(root, "rx", "bench_rx", entry_counts);
  return configs;
}

std::vector<RawBenchTxConfig> parse_tx_configs(const YAML::Node &root) {
  std::vector<RawBenchTxConfig> configs;
  const auto bench_tx = root["bench_tx"];
  if (!bench_tx) {
    return configs;
  }

  if (!bench_tx.IsSequence()) {
    configs.push_back(parse_tx_item(bench_tx));
    return configs;
  }

  std::unordered_map<std::string, size_t> next_queue_idx;
  std::unordered_map<std::string, size_t> entry_counts;
  std::unordered_map<std::string, std::unordered_set<int>> assigned_queue_ids;
  configs.reserve(bench_tx.size());
  for (const auto &item : bench_tx) {
    if (!item || !item.IsMap() || !item["interface_name"]) {
      throw std::runtime_error(
          "bench_tx list entries must be maps with interface_name");
    }
    auto cfg = parse_tx_item(item);
    cfg.queue_id = assign_queue_id(root, item, "tx", "bench_tx", next_queue_idx,
                                   entry_counts, assigned_queue_ids);
    configs.push_back(std::move(cfg));
  }
  validate_bench_list_sizes(root, "tx", "bench_tx", entry_counts);
  return configs;
}

std::vector<uint16_t> parse_udp_ports(const std::string &spec) {
  const auto dash = spec.find('-');
  if (dash == std::string::npos) {
    return {static_cast<uint16_t>(std::stoul(spec))};
  }

  const uint16_t begin =
      static_cast<uint16_t>(std::stoul(spec.substr(0, dash)));
  const uint16_t end = static_cast<uint16_t>(std::stoul(spec.substr(dash + 1)));
  if (begin > end) {
    throw std::runtime_error("invalid UDP port range");
  }

  std::vector<uint16_t> ports;
  ports.reserve(static_cast<size_t>(end - begin + 1));
  for (uint32_t p = begin; p <= end; ++p) {
    ports.push_back(static_cast<uint16_t>(p));
  }
  return ports;
}

uint32_t add_checksum_bytes(const void *data, size_t len, uint32_t sum) {
  const auto *bytes = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i + 1 < len; i += 2) {
    sum += (static_cast<uint32_t>(bytes[i]) << 8U) |
           static_cast<uint32_t>(bytes[i + 1]);
    while ((sum >> 16U) != 0) {
      sum = (sum & 0xffffU) + (sum >> 16U);
    }
  }
  if ((len & 1U) != 0) {
    sum += static_cast<uint32_t>(bytes[len - 1]) << 8U;
    while ((sum >> 16U) != 0) {
      sum = (sum & 0xffffU) + (sum >> 16U);
    }
  }
  return sum;
}

uint16_t finalize_checksum(uint32_t sum) {
  while ((sum >> 16U) != 0) {
    sum = (sum & 0xffffU) + (sum >> 16U);
  }
  return htons(static_cast<uint16_t>(~sum & 0xffffU));
}

void populate_udp_ipv4_headers(uint8_t *pkt_data, uint32_t header_size,
                               uint32_t payload_size, const char *eth_src,
                               const char *eth_dst, uint32_t ip_src_host,
                               uint32_t ip_dst_host, uint16_t src_port,
                               uint16_t dst_port) {
  std::memset(pkt_data, 0, header_size + payload_size);

  auto *pkt = reinterpret_cast<daqiri::UDPIPV4Pkt *>(pkt_data);
  std::memcpy(pkt->eth.h_source, eth_src, ETH_ALEN);
  std::memcpy(pkt->eth.h_dest, eth_dst, ETH_ALEN);
  pkt->eth.h_proto = htons(ETH_P_IP);

  const auto ip_total_len =
      static_cast<uint16_t>(payload_size + header_size - sizeof(ethhdr));
  const auto udp_len = static_cast<uint16_t>(payload_size + header_size -
                                             (sizeof(ethhdr) + sizeof(iphdr)));

  pkt->ip.version = 4;
  pkt->ip.ihl = 5;
  pkt->ip.ttl = 64;
  pkt->ip.protocol = IPPROTO_UDP;
  pkt->ip.tot_len = htons(ip_total_len);
  pkt->ip.saddr = htonl(ip_src_host);
  pkt->ip.daddr = htonl(ip_dst_host);
  pkt->ip.check = 0;
  pkt->ip.check =
      finalize_checksum(add_checksum_bytes(&pkt->ip, sizeof(iphdr), 0));

  pkt->udp.source = htons(src_port);
  pkt->udp.dest = htons(dst_port);
  pkt->udp.check = 0;
  pkt->udp.len = htons(udp_len);
}

void finalize_udp_ipv4_checksums(uint8_t *pkt_data) {
  auto *pkt = reinterpret_cast<daqiri::UDPIPV4Pkt *>(pkt_data);
  const size_t ip_header_len = static_cast<size_t>(pkt->ip.ihl) * 4U;
  const size_t udp_len = ntohs(pkt->udp.len);

  pkt->ip.check = 0;
  pkt->ip.check =
      finalize_checksum(add_checksum_bytes(&pkt->ip, ip_header_len, 0));

  pkt->udp.check = 0;
  uint32_t sum = 0;
  sum = add_checksum_bytes(&pkt->ip.saddr, sizeof(pkt->ip.saddr), sum);
  sum = add_checksum_bytes(&pkt->ip.daddr, sizeof(pkt->ip.daddr), sum);
  const uint8_t pseudo_tail[] = {0, pkt->ip.protocol,
                                 static_cast<uint8_t>(udp_len >> 8U),
                                 static_cast<uint8_t>(udp_len & 0xffU)};
  sum = add_checksum_bytes(pseudo_tail, sizeof(pseudo_tail), sum);
  sum = add_checksum_bytes(&pkt->udp, udp_len, sum);
  pkt->udp.check = finalize_checksum(sum);
  if (pkt->udp.check == 0) {
    pkt->udp.check = 0xffffU;
  }
}

void signal_handler(int signum) {
  if (signum == SIGINT) {
    g_stop_requested = 1;
  }
}

void wait_for_stop(int run_seconds, std::atomic<bool> &stop) {
  std::signal(SIGINT, signal_handler);
  auto start = std::chrono::steady_clock::now();
  while (!g_stop_requested) {
    if (run_seconds > 0) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now() - start);
      if (elapsed.count() >= run_seconds) {
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  stop.store(true);
}

void print_queue_stats(const char *direction, const std::string &interface_name,
                       int queue_id, const RawBenchQueueStats &stats,
                       double seconds) {
  std::lock_guard<std::mutex> lock(g_stats_print_mutex);
  std::cout << direction << " complete: interface=" << interface_name;
  if (queue_id >= 0) {
    std::cout << " queue=" << queue_id;
  } else {
    std::cout << " queues=all";
  }
  std::cout << " packets=" << stats.packets << " bytes=" << stats.bytes
            << " bursts=" << stats.bursts << " seconds=" << seconds
            << std::endl;
}

void rx_count_worker(const RawBenchRxConfig &cfg, std::atomic<bool> &stop) {
  const int port_id = daqiri::get_port_id(cfg.interface_name);
  if (port_id < 0) {
    std::cerr << "Invalid RX interface_name: " << cfg.interface_name << "\n";
    stop.store(true);
    return;
  }

  std::vector<int> queue_ids;
  const auto num_rx_queues =
      static_cast<int>(daqiri::get_num_rx_queues(port_id));
  if (cfg.queue_id >= 0) {
    if (cfg.queue_id >= num_rx_queues) {
      std::cerr << "Invalid RX queue_id " << cfg.queue_id
                << " for interface_name: " << cfg.interface_name << "\n";
      stop.store(true);
      return;
    }
    queue_ids.push_back(cfg.queue_id);
  } else {
    queue_ids.reserve(num_rx_queues);
    for (int q = 0; q < num_rx_queues; ++q) {
      queue_ids.push_back(q);
    }
  }

  std::vector<RawBenchQueueStats> queue_stats(num_rx_queues);
  const auto t0 = std::chrono::steady_clock::now();
  while (!stop.load()) {
    bool got_any = false;
    for (int q : queue_ids) {
      daqiri::BurstParams *burst = nullptr;
      if (daqiri::get_rx_burst(&burst, port_id, q) != daqiri::Status::SUCCESS ||
          burst == nullptr) {
        continue;
      }
      got_any = true;
      auto &stats = queue_stats[static_cast<size_t>(q)];
      stats.packets += static_cast<uint64_t>(daqiri::get_num_packets(burst));
      stats.bytes += daqiri::get_burst_tot_byte(burst);
      ++stats.bursts;
      daqiri::free_all_packets_and_burst_rx(burst);
    }
    if (!got_any) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  }
  const double secs =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
          .count();

  RawBenchQueueStats total;
  for (int q : queue_ids) {
    const auto &stats = queue_stats[static_cast<size_t>(q)];
    total.packets += stats.packets;
    total.bytes += stats.bytes;
    total.bursts += stats.bursts;
    print_queue_stats("RX", cfg.interface_name, q, stats, secs);
  }

  if (queue_ids.size() > 1) {
    print_queue_stats("RX", cfg.interface_name, -1, total, secs);
  }
}

} // namespace daqiri::bench
