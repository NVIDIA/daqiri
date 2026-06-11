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
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace daqiri::bench {

// Software token-bucket pacer used by the bench TX workers. When
// target_gbps == 0 the wait_for_bytes() call is a no-op early return, so the
// pacer adds no overhead when --target-gbps is unset.
//
// Accuracy: ~5% at high rates due to Linux nanosleep granularity and scheduler
// jitter. Acceptable for drop-curve sweeps; tighter pacing would require
// hardware TX timestamping (DAQIRI's accurate_send YAML flag), deferred.
class TokenBucketPacer {
public:
  TokenBucketPacer() = default;
  explicit TokenBucketPacer(double target_gbps);

  // Call after each TX burst. Sleeps in short slices until the pacer's notion
  // of "time the configured target rate would have taken to send the
  // accumulated bytes" catches up, OR `stop` flips true. Slicing keeps the
  // bench responsive to --seconds expiry / Ctrl-C without truncating the total
  // sleep (which would silently break pacing for low target rates).
  void wait_for_bytes(size_t bytes, std::atomic<bool> &stop);

  bool enabled() const { return target_bps_ > 0.0; }
  double target_gbps() const { return target_bps_ / 1e9; }

private:
  double target_bps_ = 0.0;  // 0 means disabled
  uint64_t total_bytes_ = 0;
  std::chrono::steady_clock::time_point t0_;
  std::mutex mutex_;
};

struct RawBenchTxConfig {
  std::string interface_name = "tx_port";
  int queue_id = 0;
  int cpu_core = -1;
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
  int queue_id = -1;
  int cpu_core = -1;
};

struct RawBenchQueueStats {
  uint64_t packets = 0;
  uint64_t bytes = 0;
  uint64_t bursts = 0;
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
double parse_target_gbps(int argc, char **argv);
bool has_bench_rx(const YAML::Node &root);
bool has_bench_tx(const YAML::Node &root);
RawBenchRxConfig parse_rx(const YAML::Node &root);
RawBenchTxConfig parse_tx(const YAML::Node &root);
std::vector<RawBenchRxConfig> parse_rx_configs(const YAML::Node &root);
std::vector<RawBenchTxConfig> parse_tx_configs(const YAML::Node &root);
std::vector<uint16_t> parse_udp_ports(const std::string &spec);
bool set_current_thread_affinity(int cpu_core, const std::string &thread_name);

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
void print_queue_stats(const char *direction, const std::string &interface_name,
                       int queue_id, const RawBenchQueueStats &stats,
                       double seconds);
void rx_count_worker(const RawBenchRxConfig &cfg, std::atomic<bool> &stop);

} // namespace daqiri::bench
