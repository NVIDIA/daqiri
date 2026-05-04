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

#pragma once

#include <cuda_runtime.h>
#include <yaml-cpp/yaml.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace daqiri::bench {

struct RawBenchTxConfig {
  std::string interface_name = "tx_port";
  uint32_t batch_size = 1024;
  uint32_t payload_size = 1000;
  uint32_t header_size = 64;
  std::string udp_src_port = "4096";
  std::string udp_dst_port = "4096";
  std::string ip_src_addr = "1.2.3.4";
  std::string ip_dst_addr = "5.6.7.8";
  std::string eth_src_addr = "00:00:00:00:00:00";
  std::string eth_dst_addr = "00:00:00:00:00:00";
};

struct RawBenchRxConfig {
  std::string interface_name = "rx_port";
};

class PinnedHostBuffer {
public:
  PinnedHostBuffer() = default;
  PinnedHostBuffer(const PinnedHostBuffer &) = delete;
  PinnedHostBuffer &operator=(const PinnedHostBuffer &) = delete;

  PinnedHostBuffer(PinnedHostBuffer &&other) noexcept;
  PinnedHostBuffer &operator=(PinnedHostBuffer &&other) noexcept;
  ~PinnedHostBuffer();

  bool resize(size_t size);
  void reset();

  uint8_t *data();
  const uint8_t *data() const;
  size_t capacity() const;

private:
  void *ptr_ = nullptr;
  size_t capacity_ = 0;
};

int parse_run_seconds(int argc, char **argv);
bool has_bench_rx(const YAML::Node &root);
bool has_bench_tx(const YAML::Node &root);
RawBenchRxConfig parse_rx(const YAML::Node &root);
RawBenchTxConfig parse_tx(const YAML::Node &root);
std::vector<uint16_t> parse_udp_ports(const std::string &spec);

void populate_udp_ipv4_headers(uint8_t *pkt_data, uint32_t header_size,
                               uint32_t payload_size, const char *eth_src,
                               const char *eth_dst, uint32_t ip_src_host,
                               uint32_t ip_dst_host, uint16_t src_port,
                               uint16_t dst_port);

void finalize_udp_ipv4_checksums(uint8_t *pkt_data);

cudaError_t memcpy_batch_async(const std::vector<void *> &dsts,
                               const std::vector<const void *> &srcs,
                               const std::vector<size_t> &sizes,
                               cudaStream_t stream);

void signal_handler(int signum);
void wait_for_stop(int run_seconds, std::atomic<bool> &stop);
void rx_count_worker(const RawBenchRxConfig &cfg, std::atomic<bool> &stop);

} // namespace daqiri::bench
