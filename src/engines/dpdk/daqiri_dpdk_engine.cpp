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

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <chrono>
#include <iostream>
#include <cstdio>
#include <limits>
#include <map>
#include <set>
#include <sys/time.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <regex>
#include <arpa/inet.h>
#include <cstring>
#include <memory>
#include <utility>

#include <rte_mbuf_dyn.h>

#include "src/dpdk_log.h"
#include "src/burst_validation.h"
#include "daqiri_dpdk_engine.h"
#include "src/kernels.h"
#include <daqiri/logging.hpp>

using namespace std::chrono;

namespace daqiri {

static bool looks_like_mac_address(const std::string& address) {
  static const std::regex mac_regex(
      "^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$", std::regex::ECMAScript);
  return std::regex_match(address, mac_regex);
}

static bool looks_like_pci_bdf(const std::string& address) {
  static const std::regex pci_bdf_regex(
      "^[0-9A-Fa-f]{4}:[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}\\.[0-7]$", std::regex::ECMAScript);
  return std::regex_match(address, pci_bdf_regex);
}

// --- Local Helper Functions for Port/Queue Key Management ---

/**
 * @brief Generates a unique 32-bit key from a port and queue ID.
 */
static inline uint32_t generate_queue_key(int port_id, int queue_id) {
    return (static_cast<uint32_t>(port_id) << 16) | static_cast<uint32_t>(queue_id);
}

static inline uint32_t flex_item_handle_key(uint16_t port, uint16_t flex_item_id) {
  return (static_cast<uint32_t>(port) << 16) | static_cast<uint32_t>(flex_item_id);
}

static const FlexItemConfig* find_flex_item_config(const NetworkConfig& cfg, int port,
                                                   uint16_t flex_item_id) {
  for (const auto& intf : cfg.ifs_) {
    if (intf.port_id_ != port) { continue; }
    for (const auto& item : intf.rx_.flex_items_) {
      if (item.id_ == flex_item_id) { return &item; }
    }
    return nullptr;
  }
  return nullptr;
}

/**
 * @brief Extracts the port ID from a 32-bit queue key.
 */
static inline int get_port_from_key(uint32_t key) {
    return static_cast<int>((key >> 16) & 0xFFFF);
}

/**
 * @brief Extracts the queue ID from a 32-bit queue key.
 */
static inline int get_queue_from_key(uint32_t key) {
    return static_cast<int>(key & 0xFFFF);
}

static void free_unsubmitted_tx_packets(BurstParams* burst) {
  if (burst == nullptr) { return; }

  // TX worker chains multi-segment mbufs only after enqueue succeeds.
  // Before that point, every allocated segment must be freed directly.
  for (int seg = 0; seg < burst->hdr.hdr.num_segs; seg++) {
    if (burst->pkts[seg] == nullptr) { continue; }

    auto** pkts = reinterpret_cast<rte_mbuf**>(burst->pkts[seg]);
    for (size_t pkt = 0; pkt < burst->hdr.hdr.num_pkts; pkt++) {
      if (pkts[pkt] != nullptr) { rte_pktmbuf_free_seg(pkts[pkt]); }
    }
  }
}

static inline bool is_cuda_accessible_packet_memory(MemoryKind kind) {
  return kind == MemoryKind::DEVICE || kind == MemoryKind::HOST_PINNED;
}

static inline bool is_cpu_accessible_memory(MemoryKind kind) {
  return kind == MemoryKind::HOST || kind == MemoryKind::HOST_PINNED || kind == MemoryKind::HUGE;
}

static inline uint32_t extract_bits_be_host(const void* data,
                                            uint16_t bit_offset,
                                            uint8_t bit_width) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  uint32_t value = 0;
  for (uint8_t i = 0; i < bit_width; ++i) {
    const uint32_t bit_idx = static_cast<uint32_t>(bit_offset) + static_cast<uint32_t>(i);
    const uint8_t byte = bytes[bit_idx / 8U];
    const uint8_t bit_pos = static_cast<uint8_t>(7U - (bit_idx % 8U));
    value = (value << 1U) | static_cast<uint32_t>((byte >> bit_pos) & 0x1U);
  }
  return value;
}

static inline uint16_t get_reorder_seq_bit_offset(const ReorderConfig& cfg) {
  return (cfg.method_ == ReorderMethod::SEQ_BATCH_NUMBER)
             ? cfg.seq_batch_number_.sequence_number_.bit_offset_
             : cfg.seq_packets_per_batch_.sequence_number_.bit_offset_;
}

static inline uint8_t get_reorder_seq_bit_width(const ReorderConfig& cfg) {
  return (cfg.method_ == ReorderMethod::SEQ_BATCH_NUMBER)
             ? cfg.seq_batch_number_.sequence_number_.bit_width_
             : cfg.seq_packets_per_batch_.sequence_number_.bit_width_;
}

static inline uint16_t get_reorder_batch_bit_offset(const ReorderConfig& cfg) {
  return (cfg.method_ == ReorderMethod::SEQ_BATCH_NUMBER)
             ? cfg.seq_batch_number_.batch_number_.bit_offset_
             : 0U;
}

static inline uint8_t get_reorder_batch_bit_width(const ReorderConfig& cfg) {
  return (cfg.method_ == ReorderMethod::SEQ_BATCH_NUMBER)
             ? cfg.seq_batch_number_.batch_number_.bit_width_
             : 0U;
}

static inline uint64_t derive_reorder_batch_id_host(const ReorderConfig& cfg,
                                                    const void* pkt_ptr,
                                                    uint32_t packets_per_batch) {
  if (cfg.method_ == ReorderMethod::SEQ_BATCH_NUMBER) {
    return extract_bits_be_host(pkt_ptr,
                                cfg.seq_batch_number_.batch_number_.bit_offset_,
                                cfg.seq_batch_number_.batch_number_.bit_width_);
  }

  const uint32_t seq = extract_bits_be_host(pkt_ptr,
                                            get_reorder_seq_bit_offset(cfg),
                                            get_reorder_seq_bit_width(cfg));
  return packets_per_batch == 0 ? 0U : static_cast<uint64_t>(seq / packets_per_batch);
}

static inline bool reorder_uses_data_type_conversion(const ReorderConfig& cfg) {
  if (!cfg.data_types_.enabled_) { return false; }
  if (cfg.data_types_.input_type_ != cfg.data_types_.output_type_) { return true; }

  return cfg.data_types_.input_endianness_ == ReorderEndianness::NETWORK
         && reorder_data_type_bit_width(cfg.data_types_.input_type_) > 8
         && (reorder_data_type_bit_width(cfg.data_types_.input_type_) % 8) == 0;
}

static inline bool compute_reorder_output_payload_len(const ReorderConfig& cfg,
                                                      uint32_t input_payload_len,
                                                      uint32_t* output_payload_len) {
  if (output_payload_len == nullptr) { return false; }
  if (!reorder_uses_data_type_conversion(cfg)) {
    *output_payload_len = input_payload_len;
    return true;
  }

  const uint32_t input_bits = reorder_data_type_bit_width(cfg.data_types_.input_type_);
  const uint32_t output_bits = reorder_data_type_bit_width(cfg.data_types_.output_type_);
  if (input_bits == 0 || output_bits == 0) { return false; }

  const uint64_t total_input_bits = static_cast<uint64_t>(input_payload_len) * 8ULL;
  if ((total_input_bits % static_cast<uint64_t>(input_bits)) != 0ULL) { return false; }

  const uint64_t element_count = total_input_bits / static_cast<uint64_t>(input_bits);
  const uint64_t total_output_bits = element_count * static_cast<uint64_t>(output_bits);
  if ((total_output_bits % 8ULL) != 0ULL) { return false; }

  const uint64_t total_output_bytes = total_output_bits / 8ULL;
  if (total_output_bytes > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
    return false;
  }

  *output_payload_len = static_cast<uint32_t>(total_output_bytes);
  return true;
}

static inline bool compute_reorder_max_output_payload_len(const ReorderConfig& cfg,
                                                          uint32_t max_input_payload_len,
                                                          uint32_t* output_payload_len) {
  if (output_payload_len == nullptr) { return false; }
  if (!reorder_uses_data_type_conversion(cfg)) {
    *output_payload_len = max_input_payload_len;
    return true;
  }

  const uint32_t input_bits = reorder_data_type_bit_width(cfg.data_types_.input_type_);
  const uint32_t output_bits = reorder_data_type_bit_width(cfg.data_types_.output_type_);
  if (input_bits == 0 || output_bits == 0) { return false; }

  const uint64_t total_input_bits = static_cast<uint64_t>(max_input_payload_len) * 8ULL;
  const uint64_t element_count = total_input_bits / static_cast<uint64_t>(input_bits);
  const uint64_t total_output_bits = element_count * static_cast<uint64_t>(output_bits);
  if ((total_output_bits % 8ULL) != 0ULL) { return false; }

  const uint64_t total_output_bytes = total_output_bits / 8ULL;
  if (total_output_bytes > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
    return false;
  }

  *output_payload_len = static_cast<uint32_t>(total_output_bytes);
  return true;
}

// --- End Helper Functions ---


std::atomic<bool> force_quit = false;

// Used to signal to RX threads to flush all existing packets.
// Defaults to seq_cst, so no fences needed.
std::atomic<bool> flush_rx_queues = false;

static bool should_log_bounded(uint64_t count) {
  return count <= 8 || (count & (count - 1)) == 0;
}

static std::atomic<uint64_t> rx_burst_array_allocation_failures{0};
static std::atomic<uint64_t> rx_incomplete_split_packet_drops{0};
static std::atomic<uint64_t> rx_controlled_packet_drops{0};
static std::atomic<uint64_t> rx_controlled_packet_drop_events{0};

static void log_rx_burst_array_allocation_failure(int port, int queue, int seg, int num_segs) {
  const uint64_t total =
      rx_burst_array_allocation_failures.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!should_log_bounded(total)) { return; }
  DAQIRI_LOG_WARN(
      "No free RX burst segment arrays for port {} queue {} while allocating segment {}/{} "
      "(total allocation failures: {})",
      port,
      queue,
      seg,
      num_segs,
      total);
}

static void log_rx_incomplete_split_packet_drop(int port,
                                                int queue,
                                                int expected_segs,
                                                int actual_segs) {
  const uint64_t total =
      rx_incomplete_split_packet_drops.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!should_log_bounded(total)) { return; }
  DAQIRI_LOG_WARN(
      "Dropped malformed split RX packet on port {} queue {}: expected {} segment(s), found {} "
      "(total malformed split drops: {})",
      port,
      queue,
      expected_segs,
      actual_segs,
      total);
}

static void log_rx_controlled_packet_drop(int port,
                                          int queue,
                                          const char* reason,
                                          uint64_t dropped) {
  const uint64_t total =
      rx_controlled_packet_drops.fetch_add(dropped, std::memory_order_relaxed) + dropped;
  const uint64_t events =
      rx_controlled_packet_drop_events.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!should_log_bounded(events)) { return; }
  DAQIRI_LOG_WARN("Dropped {} RX packet(s) on port {} queue {}: {} (total controlled drops: {})",
                  dropped,
                  port,
                  queue,
                  reason,
                  total);
}

static bool is_valid_segment_count(int num_segs) {
  return num_segs > 0 && num_segs <= MAX_NUM_SEGS;
}

static void release_rx_burst_segment_arrays(BurstParams* burst,
                                            struct rte_mempool* pool,
                                            int num_segs) {
  if (burst == nullptr || pool == nullptr) { return; }
  for (int seg = 0; seg < std::clamp(num_segs, 0, MAX_NUM_SEGS); ++seg) {
    if (burst->pkts[seg] == nullptr) { continue; }
    rte_mempool_put(pool, static_cast<void*>(burst->pkts[seg]));
    burst->pkts[seg] = nullptr;
  }
}

static bool initialize_rx_burst(BurstParams* burst,
                                int port,
                                int queue,
                                int num_segs,
                                struct rte_mempool* rx_burst_pool) {
  if (burst == nullptr || rx_burst_pool == nullptr || !is_valid_segment_count(num_segs)) {
    DAQIRI_LOG_ERROR("Invalid RX burst metadata for port {} queue {} with {} segment(s)",
                     port,
                     queue,
                     num_segs);
    return false;
  }

  burst->hdr.hdr.q_id = queue;
  burst->hdr.hdr.port_id = port;
  burst->hdr.hdr.num_segs = num_segs;
  burst->hdr.hdr.num_pkts = 0;
  burst->hdr.hdr.nbytes = 0;
  burst->hdr.hdr.burst_flags = 0;
  burst->pkt_extra_info = nullptr;
  burst->event = nullptr;

  for (int seg = 0; seg < MAX_NUM_SEGS; ++seg) {
    burst->pkts[seg] = nullptr;
    burst->pkt_lens[seg] = nullptr;
  }

  for (int seg = 0; seg < num_segs; ++seg) {
    if (rte_mempool_get(rx_burst_pool, reinterpret_cast<void**>(&burst->pkts[seg])) == 0) {
      continue;
    }

    log_rx_burst_array_allocation_failure(port, queue, seg, num_segs);
    release_rx_burst_segment_arrays(burst, rx_burst_pool, seg);
    return false;
  }

  return true;
}

static void drop_pending_rx_mbufs(struct rte_mbuf** mbufs,
                                  int start,
                                  int count,
                                  int port,
                                  int queue,
                                  const char* reason) {
  if (mbufs == nullptr || count <= 0) { return; }

  uint64_t dropped = 0;
  for (int i = 0; i < count; ++i) {
    auto*& mbuf = mbufs[start + i];
    if (mbuf == nullptr) { continue; }
    rte_pktmbuf_free(mbuf);
    mbuf = nullptr;
    ++dropped;
  }
  if (dropped > 0) { log_rx_controlled_packet_drop(port, queue, reason, dropped); }
}

static bool populate_split_packet_segments(BurstParams* burst,
                                           struct rte_mbuf* mbuf,
                                           int pkt_idx,
                                           int expected_segs,
                                           int port,
                                           int queue) {
  burst->pkts[0][pkt_idx] = mbuf;
  for (int seg = 1; seg < expected_segs; ++seg) {
    mbuf = mbuf->next;
    if (mbuf == nullptr) {
      log_rx_incomplete_split_packet_drop(port, queue, expected_segs, seg);
      for (int clear_seg = 0; clear_seg < seg; ++clear_seg) {
        burst->pkts[clear_seg][pkt_idx] = nullptr;
      }
      return false;
    }
    burst->pkts[seg][pkt_idx] = mbuf;
  }
  return true;
}

struct TxWorkerParams {
  int port;
  int queue;
  uint32_t batch_size;
  struct rte_ring* ring;
  struct rte_ring* lb_ring;         // Ring used between TX and RX when in SW loopback mode
  struct rte_mempool* meta_pool;
  struct rte_mempool* burst_pool;
  struct rte_ether_addr mac_addr;
  // Packet pacing (per-queue): when pacing_mbps > 0 and the SEND_ON_TIMESTAMP
  // offload is active (tx_ts_dynfield_offset >= 0), the worker tags the first
  // mbuf of each burst with a scheduled TX time so the NIC meters the queue at
  // (at most) the configured average rate. pace_next_ns is the NIC-clock time
  // the next burst may start; pace_rem carries the sub-ns division remainder so
  // the long-run average is exact. engine is needed to read the NIC clock.
  uint64_t pacing_mbps = 0;
  int tx_ts_dynfield_offset = -1;
  uint64_t tx_ts_dynflag_mask = 0;
  DpdkEngine* engine = nullptr;
  uint64_t pace_next_ns = 0;
  uint64_t pace_rem = 0;
};

struct RxWorkerParams {
  int port;
  int queue;
  int num_segs;
  uint64_t timeout_us;
  uint32_t batch_size;
  int rx_meta_pool_size;
  struct rte_ring* ring;
  struct rte_ring* lb_ring;
  struct rte_mempool* rx_burst_pool;
  struct rte_mempool* tx_burst_pool;
  struct rte_mempool* rx_meta_pool;
  struct rte_mempool* tx_meta_pool;
};

struct RxWorkerMultiQPerQParams {
  int port;
  int queue;
  int num_segs;
  int batch_size;
  uint64_t timeout_us;
  struct rte_ring* ring;
  struct rte_ring* lb_ring;
};

struct RxWorkerMultiQParams {
  std::vector<RxWorkerMultiQPerQParams> q_params;
  struct rte_mempool* rx_burst_pool;  // Pool used to pull out bursts from RX pool
  struct rte_mempool* tx_burst_pool;  // Pool used for loopback mode to return transmitted bursts
  struct rte_mempool* rx_meta_pool;   // Pool used for RX metadata structures
  struct rte_mempool* tx_meta_pool;   // Pool used in loopback for returning transmitted metadata
  int rx_meta_pool_size;
};

/**
 * @brief Generic UDP packet structure
 *
 */
struct UDPPkt {
  struct rte_ether_hdr eth;
  struct rte_ipv4_hdr ip;
  struct rte_udp_hdr udp;
  uint8_t payload[];
} __attribute__((packed));

static size_t mbuf_data_capacity(const rte_mbuf* mbuf) {
  if (mbuf == nullptr || mbuf->buf_len < mbuf->data_off) { return 0; }
  return static_cast<size_t>(mbuf->buf_len - mbuf->data_off);
}

static Status validate_dpdk_first_segment_write(BurstParams* burst,
                                                int idx,
                                                size_t bytes_required,
                                                const char* op_name) {
  const auto limits = burst_validation::header_limits(burst);
  Status status = burst_validation::validate_segment_packet_storage(
      burst, limits, 0, idx, true, false, op_name);
  if (status != Status::SUCCESS) { return status; }
  if (!burst_validation::strict_enabled()) { return Status::SUCCESS; }

  const auto* mbuf = reinterpret_cast<const rte_mbuf*>(burst->pkts[0][idx]);
  const size_t capacity = mbuf_data_capacity(mbuf);
  if (bytes_required > capacity) {
    DAQIRI_LOG_ERROR("{}: write size {} exceeds first-segment backing capacity {}",
                     op_name,
                     bytes_required,
                     capacity);
    return Status::INVALID_PARAMETER;
  }
  return Status::SUCCESS;
}

static Status dpdk_packet_length_capacities(BurstParams* burst,
                                            int idx,
                                            std::array<size_t, MAX_NUM_SEGS>* capacities,
                                            const char* op_name) {
  if (capacities == nullptr) { return Status::NULL_PTR; }
  *capacities = burst_validation::unknown_capacities();
  if (!burst_validation::strict_enabled()) { return Status::SUCCESS; }

  const auto limits = burst_validation::header_limits(burst);
  Status status = burst_validation::validate_packet_index(burst, limits, idx, op_name);
  if (status != Status::SUCCESS) { return status; }
  status = burst_validation::validate_segment_count(burst, limits, op_name);
  if (status != Status::SUCCESS) { return status; }

  for (int seg = 0; seg < limits.num_segs; ++seg) {
    status = burst_validation::validate_segment_packet_storage(
        burst, limits, seg, idx, true, false, op_name);
    if (status != Status::SUCCESS) { return status; }
    (*capacities)[static_cast<size_t>(seg)] =
        mbuf_data_capacity(reinterpret_cast<const rte_mbuf*>(burst->pkts[seg][idx]));
  }
  return Status::SUCCESS;
}

static Status validate_dpdk_packet_lengths(BurstParams* burst,
                                           int idx,
                                           const std::initializer_list<int>& lens,
                                           uint32_t* total_len,
                                           const char* op_name) {
  std::array<size_t, MAX_NUM_SEGS> capacities{};
  Status status = dpdk_packet_length_capacities(burst, idx, &capacities, op_name);
  if (status != Status::SUCCESS) { return status; }

  return burst_validation::validate_packet_lengths(
      burst,
      burst_validation::header_limits(burst),
      idx,
      lens,
      capacities,
      true,
      false,
      std::numeric_limits<uint16_t>::max(),
      op_name,
      total_len);
}

static inline uint64_t convert_rx_timestamp_ticks_to_ns(uint64_t timestamp,
                                                        const RxTimestampConversion& conversion) {
  if (!conversion.valid || conversion.ticks_per_second == 0) { return 0; }
  const auto ns = (static_cast<unsigned __int128>(timestamp) * 1000000000ULL) /
                  conversion.ticks_per_second;
  return static_cast<uint64_t>(ns);
}

static inline bool extract_mbuf_rx_timestamp_ns(struct rte_mbuf* mbuf,
                                                int timestamp_offset,
                                                uint64_t timestamp_mask,
                                                const RxTimestampConversion& conversion,
                                                uint64_t* timestamp_ns) {
  if (mbuf == nullptr || timestamp_ns == nullptr || timestamp_offset < 0 || timestamp_mask == 0) {
    return false;
  }
  if (!conversion.valid || conversion.ticks_per_second == 0) { return false; }
  if ((mbuf->ol_flags & timestamp_mask) == 0) { return false; }
  const uint64_t timestamp_ticks = *RTE_MBUF_DYNFIELD(mbuf, timestamp_offset, uint64_t*);
  *timestamp_ns = convert_rx_timestamp_ticks_to_ns(timestamp_ticks, conversion);
  return true;
}

bool DpdkEngine::init_reorder_queue_state(const InterfaceConfig& intf, const RxQueueConfig& qcfg) {
  if (intf.rx_.reorder_configs_.empty()) { return true; }

  const auto key = generate_queue_key(intf.port_id_, qcfg.common_.id_);
  ReorderQueueState qstate;

  std::unordered_map<FlowId, uint16_t> flow_id_to_queue;
  for (const auto& flow : intf.rx_.flows_) {
    if (flow_id_to_queue.find(flow.id_) != flow_id_to_queue.end()) {
      DAQIRI_LOG_ERROR("Duplicate flow ID {} in interface '{}'", flow.id_, intf.name_);
      return false;
    }
    flow_id_to_queue[flow.id_] = flow.action_.id_;
  }

  for (const auto& reorder_cfg : intf.rx_.reorder_configs_) {
    const bool use_gpu_backend = reorder_cfg.reorder_type_ == "gpu";
    int flow_queue_id = -1;
    std::vector<FlowId> queue_flow_ids;
    queue_flow_ids.reserve(reorder_cfg.flow_ids_.size());

    for (const auto flow_id : reorder_cfg.flow_ids_) {
      const auto flow_it = flow_id_to_queue.find(flow_id);
      if (flow_it == flow_id_to_queue.end()) {
        DAQIRI_LOG_ERROR("Reorder config '{}' references unknown flow ID {} on interface '{}'",
                         reorder_cfg.name_,
                         flow_id,
                         intf.name_);
        return false;
      }

      if (flow_queue_id < 0) {
        flow_queue_id = static_cast<int>(flow_it->second);
      } else if (flow_queue_id != static_cast<int>(flow_it->second)) {
        DAQIRI_LOG_ERROR(
            "Reorder config '{}' has flow IDs mapped to multiple queues on interface '{}'. "
            "Each reorder config must map to a single RX queue",
            reorder_cfg.name_,
            intf.name_);
        return false;
      }

      if (flow_it->second == static_cast<uint16_t>(qcfg.common_.id_)) {
        queue_flow_ids.push_back(flow_id);
      }
    }

    // Reorder config belongs to a different queue on the same interface.
    if (queue_flow_ids.empty()) { continue; }

    if (qcfg.common_.mrs_.size() != 1) {
      DAQIRI_LOG_ERROR(
          "Queue '{}'/{} using reorder config '{}' must define exactly one source memory "
          "region. Header-data split reordering is not supported.",
          qcfg.common_.name_,
          qcfg.common_.id_,
          reorder_cfg.name_);
      return false;
    }

    const std::string& source_mr_name = qcfg.common_.mrs_[0];
    const auto source_mr_it = cfg_.mrs_.find(source_mr_name);
    if (source_mr_it == cfg_.mrs_.end()) {
      DAQIRI_LOG_ERROR("Queue '{}' references unknown memory region '{}'",
                       qcfg.common_.name_,
                       source_mr_name);
      return false;
    }

    const auto& source_mr = source_mr_it->second;
    const MemoryRegionConfig* copy_src_mr = &source_mr;
    uint32_t copy_source_offset = reorder_cfg.payload_byte_offset_;
    uint32_t logical_source_size = static_cast<uint32_t>(source_mr.buf_size_);

    if (use_gpu_backend && !is_cuda_accessible_packet_memory(source_mr.kind_)) {
      DAQIRI_LOG_ERROR(
          "Queue '{}' with reorder config '{}' must use DEVICE or HOST_PINNED packet memory "
          "for GPU reorder",
          qcfg.common_.name_,
          reorder_cfg.name_);
      return false;
    }
    if (!use_gpu_backend && !is_cpu_accessible_memory(source_mr.kind_)) {
      DAQIRI_LOG_ERROR(
          "Queue '{}' with reorder config '{}' must use HOST, HOST_PINNED, or HUGE packet "
          "memory for CPU reorder",
          qcfg.common_.name_,
          reorder_cfg.name_);
      return false;
    }

    const auto out_mr_it = cfg_.mrs_.find(reorder_cfg.memory_region_);
    if (out_mr_it == cfg_.mrs_.end()) {
      DAQIRI_LOG_ERROR("Reorder config '{}' references unknown output memory region '{}'",
                       reorder_cfg.name_,
                       reorder_cfg.memory_region_);
      return false;
    }

    const auto& out_mr = out_mr_it->second;
    if (use_gpu_backend && out_mr.kind_ != MemoryKind::DEVICE
        && out_mr.kind_ != MemoryKind::HOST_PINNED) {
      DAQIRI_LOG_ERROR(
          "Reorder output memory region '{}' in config '{}' must be DEVICE or HOST_PINNED memory",
                       reorder_cfg.memory_region_,
                       reorder_cfg.name_);
      return false;
    }
    if (!use_gpu_backend && !is_cpu_accessible_memory(out_mr.kind_)) {
      DAQIRI_LOG_ERROR(
          "Reorder output memory region '{}' in config '{}' must be HOST, HOST_PINNED, or HUGE "
          "memory for CPU reorder",
          reorder_cfg.memory_region_,
          reorder_cfg.name_);
      return false;
    }

    if (use_gpu_backend && copy_src_mr->kind_ == MemoryKind::DEVICE && out_mr.kind_ == MemoryKind::DEVICE
        && out_mr.affinity_ != copy_src_mr->affinity_) {
      DAQIRI_LOG_ERROR(
          "Reorder config '{}' requires input/output memory on the same GPU (src affinity {} "
          "!= reorder affinity {})",
          reorder_cfg.name_,
          copy_src_mr->affinity_,
          out_mr.affinity_);
      return false;
    }

    if (reorder_cfg.payload_byte_offset_ >= logical_source_size) {
      DAQIRI_LOG_ERROR(
          "Reorder config '{}' payload_byte_offset {} is out of range for source buffer size {}",
          reorder_cfg.name_,
          reorder_cfg.payload_byte_offset_,
          logical_source_size);
      return false;
    }

    uint32_t packets_per_batch = 0;
    if (reorder_cfg.method_ == ReorderMethod::SEQ_BATCH_NUMBER) {
      packets_per_batch = reorder_cfg.seq_batch_number_.packets_per_batch_;
    } else if (reorder_cfg.method_ == ReorderMethod::SEQ_PACKETS_PER_BATCH) {
      packets_per_batch = reorder_cfg.seq_packets_per_batch_.packets_per_batch_;
    } else {
      DAQIRI_LOG_ERROR("Reorder config '{}' has invalid method", reorder_cfg.name_);
      return false;
    }

    if (packets_per_batch == 0 || packets_per_batch > static_cast<uint32_t>(qcfg.common_.batch_size_)) {
      DAQIRI_LOG_ERROR(
          "Reorder config '{}' packets_per_batch {} must be in range [1, queue batch_size {}]",
          reorder_cfg.name_,
          packets_per_batch,
          qcfg.common_.batch_size_);
      return false;
    }

    if (copy_source_offset >= copy_src_mr->buf_size_) {
      DAQIRI_LOG_ERROR(
          "Reorder config '{}' copy source offset {} is out of range for source MR '{}' size {}",
          reorder_cfg.name_,
          copy_source_offset,
          copy_src_mr->name_,
          copy_src_mr->buf_size_);
      return false;
    }

    const uint32_t slot_stride =
        static_cast<uint32_t>(copy_src_mr->buf_size_ - copy_source_offset);
    uint32_t output_slot_stride = slot_stride;
    if (!compute_reorder_max_output_payload_len(reorder_cfg, slot_stride, &output_slot_stride)) {
      DAQIRI_LOG_ERROR(
          "Reorder config '{}' cannot convert source slot size {} from {} to {}",
          reorder_cfg.name_,
          slot_stride,
          reorder_data_type_to_string(reorder_cfg.data_types_.input_type_),
          reorder_data_type_to_string(reorder_cfg.data_types_.output_type_));
      return false;
    }
    const bool use_data_type_conversion = reorder_uses_data_type_conversion(reorder_cfg);
    if (use_data_type_conversion && !use_gpu_backend) {
      DAQIRI_LOG_ERROR(
          "Reorder config '{}' data type conversion is supported only for gpu reorder_type",
          reorder_cfg.name_);
      return false;
    }

    const uint64_t min_required_out =
        static_cast<uint64_t>(output_slot_stride) * static_cast<uint64_t>(packets_per_batch);
    if (min_required_out > out_mr.buf_size_) {
      DAQIRI_LOG_ERROR(
          "Reorder output MR '{}' buffer size {} is too small for config '{}' "
          "(required at least {} = packets_per_batch {} * output_slot_stride {})",
          out_mr.name_,
          out_mr.buf_size_,
          reorder_cfg.name_,
          min_required_out,
          packets_per_batch,
          output_slot_stride);
      return false;
    }

    int cuda_device_id = 0;
    if (use_gpu_backend) {
      if (copy_src_mr->kind_ == MemoryKind::DEVICE) {
        cuda_device_id = copy_src_mr->affinity_;
      } else if (out_mr.kind_ == MemoryKind::DEVICE) {
        cuda_device_id = out_mr.affinity_;
      } else {
        cuda_device_id = copy_src_mr->affinity_;
      }
    }

    auto pool_it = reorder_output_pools_.find(out_mr.name_);
    std::shared_ptr<ReorderOutputPool> output_pool;
    const auto destroy_cuda_events = [](ReorderOutputBufferState& buffer) {
      if (buffer.event != nullptr) {
        cudaEventDestroy(buffer.event);
        buffer.event = nullptr;
      }
      if (buffer.h_batch_id != nullptr) {
        cudaFreeHost(buffer.h_batch_id);
        buffer.h_batch_id = nullptr;
      }
      if (buffer.d_batch_id != nullptr) {
        cudaFree(buffer.d_batch_id);
        buffer.d_batch_id = nullptr;
      }
#if DAQIRI_REORDER_GPU_PROFILE
      if (buffer.kernel_start_event != nullptr) {
        cudaEventDestroy(buffer.kernel_start_event);
        buffer.kernel_start_event = nullptr;
      }
      if (buffer.kernel_stop_event != nullptr) {
        cudaEventDestroy(buffer.kernel_stop_event);
        buffer.kernel_stop_event = nullptr;
      }
#endif
      buffer.event_complete = true;
    };
    const auto enable_cuda_events = [&destroy_cuda_events](ReorderOutputPool& pool,
                                                           int device_id) -> bool {
      if (pool.cuda_events_enabled) { return true; }
      cudaSetDevice(device_id);
      for (size_t i = 0; i < pool.buffers.size(); ++i) {
        auto& buffer = pool.buffers[i];
        if (cudaEventCreateWithFlags(&buffer.event, cudaEventDisableTiming) != cudaSuccess) {
          for (auto& created : pool.buffers) { destroy_cuda_events(created); }
          return false;
        }
        if (cudaHostAlloc(reinterpret_cast<void**>(&buffer.h_batch_id),
                          sizeof(uint64_t),
                          cudaHostAllocDefault) != cudaSuccess
            || cudaMalloc(reinterpret_cast<void**>(&buffer.d_batch_id), sizeof(uint64_t))
                   != cudaSuccess) {
          for (auto& created : pool.buffers) { destroy_cuda_events(created); }
          return false;
        }
        *buffer.h_batch_id = 0;
#if DAQIRI_REORDER_GPU_PROFILE
        if (cudaEventCreate(&buffer.kernel_start_event) != cudaSuccess
            || cudaEventCreate(&buffer.kernel_stop_event) != cudaSuccess) {
          for (auto& created : pool.buffers) { destroy_cuda_events(created); }
          return false;
        }
#endif
      }
      pool.cuda_device_id = device_id;
      pool.cuda_events_enabled = true;
      return true;
    };

    if (pool_it == reorder_output_pools_.end()) {
      const auto ar_it = ar_.find(out_mr.name_);
      if (ar_it == ar_.end()) {
        DAQIRI_LOG_ERROR("No allocated memory found for reorder MR '{}'", out_mr.name_);
        return false;
      }

      output_pool = std::make_shared<ReorderOutputPool>();
      output_pool->mr_name = out_mr.name_;
      output_pool->cuda_device_id = cuda_device_id;
      output_pool->buffers.resize(out_mr.num_bufs_);
      auto* base = static_cast<uint8_t*>(ar_it->second.ptr_);
      for (size_t i = 0; i < out_mr.num_bufs_; ++i) {
        auto* buf_ptr = base + (i * out_mr.adj_size_);
        output_pool->buffers[i].ptr = buf_ptr;
      }
      if (use_gpu_backend && !enable_cuda_events(*output_pool, cuda_device_id)) {
        DAQIRI_LOG_ERROR("Failed to create CUDA events for reorder output MR '{}'", out_mr.name_);
        return false;
      }

      reorder_output_pools_[out_mr.name_] = output_pool;
    } else {
      output_pool = pool_it->second;
      if (use_gpu_backend && output_pool->cuda_events_enabled
          && output_pool->cuda_device_id != cuda_device_id) {
        DAQIRI_LOG_ERROR(
            "Reorder output MR '{}' is shared by configs using different CUDA devices ({} != {})",
            out_mr.name_,
            output_pool->cuda_device_id,
            cuda_device_id);
        return false;
      }
      if (use_gpu_backend && !enable_cuda_events(*output_pool, cuda_device_id)) {
        DAQIRI_LOG_ERROR("Failed to create CUDA events for reorder output MR '{}'", out_mr.name_);
        return false;
      }
    }
    for (auto& buffer : output_pool->buffers) {
      if (buffer.source_mbufs.size() < packets_per_batch) {
        buffer.source_mbufs.resize(packets_per_batch);
      }
      buffer.source_packet_count = 0;
    }

    ReorderPlanRuntime plan;
    plan.config = &reorder_cfg;
    plan.port_id = static_cast<uint16_t>(intf.port_id_);
    plan.queue_id = static_cast<uint16_t>(qcfg.common_.id_);
    plan.memory_region_name = out_mr.name_;
    plan.output_pool = output_pool;
    plan.packets_per_batch = packets_per_batch;
    plan.payload_byte_offset = reorder_cfg.payload_byte_offset_;
    plan.copy_source_offset = copy_source_offset;
    plan.slot_stride = slot_stride;
    plan.data_type_conversion_enabled = use_data_type_conversion;
    plan.input_data_type = reorder_cfg.data_types_.input_type_;
    plan.output_data_type = reorder_cfg.data_types_.output_type_;
    plan.input_endianness = reorder_cfg.data_types_.input_endianness_;
    plan.use_gpu_backend = use_gpu_backend;
    plan.cuda_staging_capacity = std::max<uint32_t>(
        packets_per_batch, static_cast<uint32_t>(qcfg.common_.batch_size_));
    plan.h_input_ptrs.resize(plan.cuda_staging_capacity);
    plan.h_source_mbufs.resize(plan.cuda_staging_capacity);
    plan.cuda_device_id = cuda_device_id;
    plan.timeout_cycles = (qcfg.timeout_us_ == 0)
                              ? 0
                              : (static_cast<uint64_t>(qcfg.timeout_us_)
                                 * rte_get_timer_hz() / 1000000ULL);

    if (use_gpu_backend) {
      cudaSetDevice(plan.cuda_device_id);
      if (cudaMalloc(reinterpret_cast<void**>(&plan.d_input_ptrs),
                     sizeof(void*) * plan.cuda_staging_capacity) != cudaSuccess) {
        DAQIRI_LOG_ERROR("Failed to allocate CUDA staging buffers for reorder config '{}'",
                         reorder_cfg.name_);
        if (plan.d_input_ptrs != nullptr) { cudaFree(plan.d_input_ptrs); }
        return false;
      }
    }

    const size_t plan_idx = qstate.plans.size();
    qstate.plans.emplace_back(std::move(plan));

    for (const auto flow_id : queue_flow_ids) {
      if (qstate.flow_id_to_plan.find(flow_id) != qstate.flow_id_to_plan.end()) {
        DAQIRI_LOG_ERROR(
            "Flow ID {} is mapped to multiple reorder configs on interface '{}' queue {}",
            flow_id,
            intf.name_,
            qcfg.common_.id_);
        return false;
      }
      qstate.flow_id_to_plan[flow_id] = plan_idx;
    }
  }

  if (!qstate.plans.empty()) {
    qstate.enabled = true;
    qstate.single_plan_fast_path = qstate.plans.size() == 1;
    const auto queue_batch_size = static_cast<size_t>(qcfg.common_.batch_size_);
    qstate.plan_pkt_indices.resize(qstate.plans.size());
    qstate.plan_pkt_counts.resize(qstate.plans.size());
    for (auto& idxs : qstate.plan_pkt_indices) { idxs.resize(queue_batch_size); }
    qstate.unmatched_indices.resize(queue_batch_size);
    reorder_queue_states_[key] = std::move(qstate);
  }

  return true;
}

bool DpdkEngine::init_reorder_state() {
  std::lock_guard<std::mutex> guard(reorder_lock_);
  cleanup_reorder_state();

  for (const auto& intf : cfg_.ifs_) {
    for (const auto& qcfg : intf.rx_.queues_) {
      if (!init_reorder_queue_state(intf, qcfg)) {
        cleanup_reorder_state();
        return false;
      }
    }
  }

  if (!reorder_queue_states_.empty()) {
    DAQIRI_LOG_INFO("Initialized {} DPDK reorder queue state entries",
                    reorder_queue_states_.size());
  }

  return true;
}

void DpdkEngine::cleanup_reorder_state() {
  for (auto& [qkey, qstate] : reorder_queue_states_) {
    (void)qkey;
    for (auto& plan : qstate.plans) {
#if DAQIRI_REORDER_GPU_PROFILE
      if (plan.gpu_profile.gpu_kernel_samples != 0) {
        const double avg_kernel_us =
            plan.gpu_profile.gpu_kernel_total_us
            / static_cast<double>(plan.gpu_profile.gpu_kernel_samples);
        DAQIRI_LOG_INFO(
            "Reorder GPU profile '{}' kernel_samples={} avg_kernel_us={:.2f} max_kernel_us={:.2f}",
            plan.config != nullptr ? plan.config->name_ : "<unknown>",
            plan.gpu_profile.gpu_kernel_samples,
            avg_kernel_us,
            plan.gpu_profile.gpu_kernel_max_us);
      }
#endif
      if (plan.direct_arrival_batch.packet_count != 0 && !plan.h_source_mbufs.empty()) {
        rte_pktmbuf_free_bulk(plan.h_source_mbufs.data(),
                              static_cast<unsigned int>(plan.direct_arrival_batch.packet_count));
      }
      plan.direct_arrival_batch.first_packet_cycles = 0;
      plan.direct_arrival_batch.first_packet_rx_timestamp_ns = 0;
      plan.direct_arrival_batch.first_packet_rx_timestamp_ns_valid = false;
      plan.direct_arrival_batch.input_payload_len = 0;
      plan.direct_arrival_batch.payload_len = 0;
      plan.direct_arrival_batch.packet_count = 0;

      for (auto& pending : plan.pending_copies) {
        if (pending.output_pool != nullptr && pending.buffer_idx < pending.output_pool->buffers.size()) {
          auto& buffer = pending.output_pool->buffers[pending.buffer_idx];
          if (buffer.event != nullptr && !buffer.event_complete) {
            cudaEventSynchronize(buffer.event);
          }
          buffer.event_complete = true;
          buffer.consumer_done = true;
          if (buffer.source_packet_count != 0) {
            rte_pktmbuf_free_bulk(buffer.source_mbufs.data(),
                                  static_cast<unsigned int>(buffer.source_packet_count));
            buffer.source_packet_count = 0;
          }
        }
      }
      plan.pending_copies.clear();

      if (plan.d_input_ptrs != nullptr) {
        cudaFree(plan.d_input_ptrs);
        plan.d_input_ptrs = nullptr;
      }
    }

    while (!qstate.ready_outputs.empty()) {
      auto* burst = qstate.ready_outputs.front();
      qstate.ready_outputs.pop_front();
      if (burst != nullptr) { free_rx_burst(burst); }
    }
  }
  reorder_queue_states_.clear();

  for (auto& [mr_name, output_pool] : reorder_output_pools_) {
    (void)mr_name;
    if (output_pool == nullptr) { continue; }
    if (output_pool->cuda_events_enabled) { cudaSetDevice(output_pool->cuda_device_id); }
    for (auto& buffer : output_pool->buffers) {
      if (buffer.event != nullptr) {
        cudaEventDestroy(buffer.event);
        buffer.event = nullptr;
      }
      if (buffer.h_batch_id != nullptr) {
        cudaFreeHost(buffer.h_batch_id);
        buffer.h_batch_id = nullptr;
      }
      if (buffer.d_batch_id != nullptr) {
        cudaFree(buffer.d_batch_id);
        buffer.d_batch_id = nullptr;
      }
#if DAQIRI_REORDER_GPU_PROFILE
      if (buffer.kernel_start_event != nullptr) {
        cudaEventDestroy(buffer.kernel_start_event);
        buffer.kernel_start_event = nullptr;
      }
      if (buffer.kernel_stop_event != nullptr) {
        cudaEventDestroy(buffer.kernel_stop_event);
        buffer.kernel_stop_event = nullptr;
      }
#endif
      buffer.event_complete = true;
      buffer.consumer_done = true;
      buffer.source_packet_count = 0;
    }
  }
  reorder_output_pools_.clear();
}

Status DpdkEngine::acquire_reorder_output_buffer(ReorderPlanRuntime& plan,
                                              size_t* buffer_idx,
                                              void** output_buffer) {
  if (buffer_idx == nullptr || output_buffer == nullptr) { return Status::NULL_PTR; }
  *output_buffer = nullptr;
  if (plan.output_pool == nullptr || plan.output_pool->buffers.empty()) {
    DAQIRI_LOG_ERROR("Reorder config '{}' has no output buffers", plan.config->name_);
    return Status::NO_FREE_PACKET_BUFFERS;
  }

  auto& pool = *plan.output_pool;
  for (size_t attempt = 0; attempt < pool.buffers.size(); ++attempt) {
    const size_t idx = (pool.next_buffer + attempt) % pool.buffers.size();
    auto& buffer = pool.buffers[idx];
    if (buffer.consumer_done && buffer.event_complete) {
      buffer.consumer_done = false;
      pool.next_buffer = (idx + 1) % pool.buffers.size();
      *buffer_idx = idx;
      *output_buffer = buffer.ptr;
      return Status::SUCCESS;
    }
  }

  DAQIRI_LOG_ERROR("No free reorder output buffers for MR '{}'", plan.memory_region_name);
  return Status::NO_FREE_PACKET_BUFFERS;
}

void DpdkEngine::release_reorder_output_buffer(std::shared_ptr<ReorderOutputPool> output_pool,
                                            size_t buffer_idx) {
  if (output_pool == nullptr || buffer_idx >= output_pool->buffers.size()) { return; }

  auto& buffer = output_pool->buffers[buffer_idx];
  buffer.consumer_done = true;
  if (buffer.event == nullptr) {
    buffer.event_complete = true;
  }
}

Status DpdkEngine::poll_reorder_events(ReorderPlanRuntime& plan) {
  Status final_status = Status::SUCCESS;

  for (auto it = plan.pending_copies.begin(); it != plan.pending_copies.end();) {
    auto& pending = *it;
    if (pending.output_pool == nullptr || pending.buffer_idx >= pending.output_pool->buffers.size()) {
      it = plan.pending_copies.erase(it);
      continue;
    }

    auto& buffer = pending.output_pool->buffers[pending.buffer_idx];
    if (buffer.event == nullptr) {
      it = plan.pending_copies.erase(it);
      continue;
    }

    const cudaError_t event_status = cudaEventQuery(buffer.event);
    if (event_status == cudaErrorNotReady) {
      ++it;
      continue;
    }
    if (event_status != cudaSuccess) {
      DAQIRI_LOG_ERROR("Failed while polling reorder CUDA event for config '{}'",
                       plan.config->name_);
      (void)cudaGetLastError();
      final_status = Status::INTERNAL_ERROR;
      ++it;
      continue;
    }

#if DAQIRI_REORDER_GPU_PROFILE
    if (buffer.kernel_start_event != nullptr && buffer.kernel_stop_event != nullptr) {
      float kernel_ms = 0.0f;
      const cudaError_t elapsed_status =
          cudaEventElapsedTime(&kernel_ms, buffer.kernel_start_event, buffer.kernel_stop_event);
      if (elapsed_status == cudaSuccess) {
        const double kernel_us = static_cast<double>(kernel_ms) * 1000.0;
        plan.gpu_profile.gpu_kernel_samples++;
        plan.gpu_profile.gpu_kernel_total_us += kernel_us;
        plan.gpu_profile.gpu_kernel_max_us =
            std::max(plan.gpu_profile.gpu_kernel_max_us, kernel_us);
      } else {
        DAQIRI_LOG_ERROR("Failed to compute reorder kernel elapsed time for config '{}'",
                         plan.config->name_);
        (void)cudaGetLastError();
        final_status = Status::INTERNAL_ERROR;
      }
    }
#endif

    if (buffer.source_packet_count != 0) {
      rte_pktmbuf_free_bulk(buffer.source_mbufs.data(),
                            static_cast<unsigned int>(buffer.source_packet_count));
      buffer.source_packet_count = 0;
    }
    buffer.event_complete = true;

    it = plan.pending_copies.erase(it);
  }

  return final_status;
}

Status DpdkEngine::append_reorder_packet(ReorderPlanRuntime& plan,
                                      struct rte_mbuf* mbuf,
                                      void* pkt_ptr,
                                      uint64_t now_cycles,
                                      size_t* batch_size) {
  if (batch_size == nullptr) { return Status::NULL_PTR; }
  *batch_size = 0;

  auto& batch = plan.direct_arrival_batch;
  const uint32_t packet_idx = batch.packet_count;
  if (packet_idx >= plan.h_input_ptrs.size() || packet_idx >= plan.h_source_mbufs.size()) {
    DAQIRI_LOG_ERROR("Reorder packet staging storage is full for config '{}'",
                     plan.config->name_);
    return Status::NO_SPACE_AVAILABLE;
  }

  if (packet_idx == 0) {
    batch.first_packet_cycles = now_cycles;
    batch.first_packet_rx_timestamp_ns = 0;
    batch.first_packet_rx_timestamp_ns_valid =
        extract_mbuf_rx_timestamp_ns(mbuf,
                                     timestamp_dynfield_offset_,
                                     rx_timestamp_dynflag_mask_,
                                     rx_timestamp_conversions_[plan.port_id],
                                     &batch.first_packet_rx_timestamp_ns);
    const uint32_t pkt_len = mbuf != nullptr ? mbuf->pkt_len : 0U;
    uint32_t copy_len = 0;
    if (pkt_len > plan.payload_byte_offset) { copy_len = pkt_len - plan.payload_byte_offset; }
    batch.input_payload_len = std::min(copy_len, plan.slot_stride);
    if (!compute_reorder_output_payload_len(
            *(plan.config), batch.input_payload_len, &batch.payload_len)) {
      DAQIRI_LOG_ERROR(
          "Reorder config '{}' cannot convert packet payload size {} from {} to {}",
          plan.config->name_,
          batch.input_payload_len,
          reorder_data_type_to_string(plan.input_data_type),
          reorder_data_type_to_string(plan.output_data_type));
      batch.first_packet_cycles = 0;
      batch.first_packet_rx_timestamp_ns = 0;
      batch.first_packet_rx_timestamp_ns_valid = false;
      batch.input_payload_len = 0;
      batch.payload_len = 0;
      return Status::INVALID_PARAMETER;
    }
  }

  plan.h_source_mbufs[packet_idx] = mbuf;
  plan.h_input_ptrs[packet_idx] = pkt_ptr;
  batch.packet_count = packet_idx + 1U;
  *batch_size = batch.packet_count;
  return Status::SUCCESS;
}

Status DpdkEngine::create_reorder_output_burst(ReorderPlanRuntime& plan,
                                            std::shared_ptr<ReorderOutputPool> output_pool,
                                            size_t buffer_idx,
                                            void* output_buffer,
                                            uint32_t aggregate_len,
                                            uint32_t source_packet_count,
                                            uint32_t payload_len,
                                            uint64_t batch_id,
                                            bool batch_id_ready,
                                            const uint64_t* h_batch_id,
                                            uint64_t rx_timestamp_ns,
                                            bool rx_timestamp_ns_valid,
                                            cudaEvent_t event,
                                            bool timeout_flush,
                                            BurstParams** out_burst) {
  if (out_burst == nullptr) { return Status::NULL_PTR; }
  *out_burst = nullptr;

  auto* burst = new BurstParams{};
  burst->hdr.hdr.port_id = plan.port_id;
  burst->hdr.hdr.q_id = plan.queue_id;
  burst->hdr.hdr.num_segs = 1;
  burst->hdr.hdr.num_pkts = 1;
  burst->hdr.hdr.nbytes = aggregate_len;
  burst->hdr.hdr.max_pkt = source_packet_count;
  burst->hdr.hdr.burst_flags =
      kBurstFlagDpdkReordered | (timeout_flush ? kBurstFlagDpdkReorderTimeout : 0U);
  burst->pkt_extra_info = nullptr;
  burst->event = event;

  auto ctx = std::make_shared<ReorderBurstContext>();
  ctx->mr_name = plan.memory_region_name;
  ctx->output_pool = output_pool;
  ctx->buffer_idx = buffer_idx;
  ctx->pkt_ptrs[0] = output_buffer;
  ctx->pkt_lens[0] = aggregate_len;
  ctx->info.batch_id = batch_id_ready ? batch_id : 0U;
  ctx->info.source_packet_count = source_packet_count;
  ctx->info.packets_per_batch = plan.packets_per_batch;
  ctx->info.payload_len = payload_len;
  ctx->info.aggregate_len = aggregate_len;
  ctx->info.burst_flags = burst->hdr.hdr.burst_flags;
  ctx->h_batch_id = batch_id_ready ? nullptr : h_batch_id;
  ctx->rx_timestamp_ns = rx_timestamp_ns;
  ctx->rx_timestamp_ns_valid = rx_timestamp_ns_valid;
  static_assert(sizeof(ReorderBurstInfo)
                <= ADV_NETWORK_HEADER_SIZE_BYTES - sizeof(void*) - sizeof(BurstHeaderParams),
                "ReorderBurstInfo must fit in BurstHeader::custom_burst_data");
  std::memcpy(burst->hdr.custom_burst_data, &ctx->info, sizeof(ctx->info));
  burst->custom_pkt_data = std::static_pointer_cast<void>(ctx);

  burst->pkts[0] = ctx->pkt_ptrs.data();
  burst->pkt_lens[0] = ctx->pkt_lens.data();
  for (int seg = 1; seg < MAX_NUM_SEGS; ++seg) {
    burst->pkts[seg] = nullptr;
    burst->pkt_lens[seg] = nullptr;
  }

  *out_burst = burst;
  return Status::SUCCESS;
}

Status DpdkEngine::flush_reorder_batch(ReorderPlanRuntime& plan,
                                    uint32_t batch_id,
                                    bool timeout_flush,
                                    BurstParams** out_burst) {
  if (out_burst == nullptr) { return Status::NULL_PTR; }
  *out_burst = nullptr;
  (void)batch_id;

  auto* batch = &plan.direct_arrival_batch;

  if (batch->packet_count == 0) {
    batch->first_packet_cycles = 0;
    batch->first_packet_rx_timestamp_ns = 0;
    batch->first_packet_rx_timestamp_ns_valid = false;
    batch->input_payload_len = 0;
    batch->payload_len = 0;
    return Status::SUCCESS;
  }

  const uint32_t input_payload_len = batch->input_payload_len;
  const uint32_t output_payload_len = batch->payload_len;

  const uint32_t num_pkts = batch->packet_count;
  const uint32_t output_slots = plan.packets_per_batch;
  const uint64_t aggregate_len64 =
      static_cast<uint64_t>(output_slots) * static_cast<uint64_t>(output_payload_len);
  if (aggregate_len64 > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
    DAQIRI_LOG_ERROR("Reorder output length {} is too large for config '{}'",
                     aggregate_len64,
                     plan.config->name_);
    return Status::INVALID_PARAMETER;
  }
  uint32_t aggregate_len = static_cast<uint32_t>(aggregate_len64);

  auto& input_ptrs = plan.h_input_ptrs;

  size_t output_buffer_idx = 0;
  void* output_buffer = nullptr;
  Status status = acquire_reorder_output_buffer(plan, &output_buffer_idx, &output_buffer);
  if (status != Status::SUCCESS) { return status; }

  cudaEvent_t copy_done = nullptr;
  uint64_t output_batch_id = 0;
  bool output_batch_id_ready = false;
  const uint64_t* output_batch_id_host = nullptr;
  if (plan.use_gpu_backend) {
    auto& output_state = plan.output_pool->buffers[output_buffer_idx];
    if (output_state.source_mbufs.size() < num_pkts) {
      DAQIRI_LOG_ERROR("Reorder output buffer has insufficient source packet tracking storage");
      release_reorder_output_buffer(plan.output_pool, output_buffer_idx);
      return Status::INTERNAL_ERROR;
    }
    std::memcpy(output_state.source_mbufs.data(),
                plan.h_source_mbufs.data(),
                sizeof(struct rte_mbuf*) * num_pkts);
    output_state.source_packet_count = num_pkts;
    if (output_state.h_batch_id == nullptr || output_state.d_batch_id == nullptr) {
      DAQIRI_LOG_ERROR("Reorder output buffer has no GPU batch metadata storage");
      release_reorder_output_buffer(plan.output_pool, output_buffer_idx);
      return Status::INTERNAL_ERROR;
    }
    output_batch_id_host = output_state.h_batch_id;

    if (cudaMemcpyAsync(plan.d_input_ptrs,
                        input_ptrs.data(),
                        sizeof(void*) * num_pkts,
                        cudaMemcpyHostToDevice,
                        plan.stream) != cudaSuccess) {
      DAQIRI_LOG_ERROR("Failed to copy ordered reorder packet pointers to device");
      release_reorder_output_buffer(plan.output_pool, output_buffer_idx);
      return Status::INTERNAL_ERROR;
    }

#if DAQIRI_REORDER_GPU_PROFILE
    if (output_state.kernel_start_event == nullptr || output_state.kernel_stop_event == nullptr) {
      DAQIRI_LOG_ERROR("Reorder output buffer has no CUDA backend timing events");
      release_reorder_output_buffer(plan.output_pool, output_buffer_idx);
      return Status::INTERNAL_ERROR;
    }
    if (cudaEventRecord(output_state.kernel_start_event, plan.stream) != cudaSuccess) {
      DAQIRI_LOG_ERROR("Failed to record reorder backend start event");
      release_reorder_output_buffer(plan.output_pool, output_buffer_idx);
      return Status::INTERNAL_ERROR;
    }
#endif

    const auto& cfg = *(plan.config);
    packet_reorder_copy_payload_by_sequence(
        output_buffer,
        reinterpret_cast<const void* const*>(plan.d_input_ptrs),
        input_payload_len,
        output_payload_len,
        plan.copy_source_offset,
        num_pkts,
        get_reorder_seq_bit_offset(cfg),
        get_reorder_seq_bit_width(cfg),
        get_reorder_batch_bit_offset(cfg),
        get_reorder_batch_bit_width(cfg),
        cfg.method_ == ReorderMethod::SEQ_BATCH_NUMBER ? 1U : 0U,
        plan.packets_per_batch,
        output_slots - 1U,
        plan.data_type_conversion_enabled ? static_cast<uint8_t>(plan.input_data_type) : 0U,
        plan.data_type_conversion_enabled ? static_cast<uint8_t>(plan.output_data_type) : 0U,
        plan.data_type_conversion_enabled ? static_cast<uint8_t>(plan.input_endianness) : 0U,
        output_state.d_batch_id,
        plan.stream);
    if (cudaGetLastError() != cudaSuccess) {
      DAQIRI_LOG_ERROR("Failed to launch reorder payload copy kernel");
      release_reorder_output_buffer(plan.output_pool, output_buffer_idx);
      return Status::INTERNAL_ERROR;
    }

#if DAQIRI_REORDER_GPU_PROFILE
    if (cudaEventRecord(output_state.kernel_stop_event, plan.stream) != cudaSuccess) {
      DAQIRI_LOG_ERROR("Failed to record reorder backend stop event");
      release_reorder_output_buffer(plan.output_pool, output_buffer_idx);
      return Status::INTERNAL_ERROR;
    }
#endif

    if (cudaMemcpyAsync(output_state.h_batch_id,
                        output_state.d_batch_id,
                        sizeof(uint64_t),
                        cudaMemcpyDeviceToHost,
                        plan.stream) != cudaSuccess) {
      DAQIRI_LOG_ERROR("Failed to copy reorder batch ID to host");
      release_reorder_output_buffer(plan.output_pool, output_buffer_idx);
      return Status::INTERNAL_ERROR;
    }

    if (output_state.event == nullptr) {
      DAQIRI_LOG_ERROR("Reorder output buffer has no CUDA event");
      release_reorder_output_buffer(plan.output_pool, output_buffer_idx);
      return Status::INTERNAL_ERROR;
    }
    copy_done = output_state.event;
    if (cudaEventRecord(copy_done, plan.stream) != cudaSuccess) {
      DAQIRI_LOG_ERROR("Failed to record reorder completion event");
      release_reorder_output_buffer(plan.output_pool, output_buffer_idx);
      return Status::INTERNAL_ERROR;
    }

    output_state.event_complete = false;
  } else {
    const auto& cfg = *(plan.config);
    auto* out_bytes = static_cast<uint8_t*>(output_buffer);
    for (uint32_t i = 0; i < num_pkts; ++i) {
      const auto* src_pkt = static_cast<const uint8_t*>(input_ptrs[i]);
      if (src_pkt == nullptr) { continue; }

      if (!output_batch_id_ready) {
        output_batch_id = derive_reorder_batch_id_host(cfg, src_pkt, plan.packets_per_batch);
        output_batch_id_ready = true;
      }
      const uint32_t seq =
          extract_bits_be_host(src_pkt, get_reorder_seq_bit_offset(cfg), get_reorder_seq_bit_width(cfg));
      const uint32_t slot_idx = seq % plan.packets_per_batch;
      if (slot_idx >= output_slots) { continue; }

      std::memcpy(out_bytes + (static_cast<size_t>(slot_idx) * output_payload_len),
                  src_pkt + plan.copy_source_offset,
                  output_payload_len);
    }
  }

  status = create_reorder_output_burst(plan,
                                       plan.output_pool,
                                       output_buffer_idx,
                                       output_buffer,
                                       aggregate_len,
                                       num_pkts,
                                       output_payload_len,
                                       output_batch_id,
                                       output_batch_id_ready,
                                       output_batch_id_host,
                                       batch->first_packet_rx_timestamp_ns,
                                       batch->first_packet_rx_timestamp_ns_valid,
                                       copy_done,
                                       timeout_flush,
                                       out_burst);
  if (status != Status::SUCCESS) {
    release_reorder_output_buffer(plan.output_pool, output_buffer_idx);
    return status;
  }

  if (plan.use_gpu_backend) {
    ReorderPendingCopy pending;
    pending.output_pool = plan.output_pool;
    pending.buffer_idx = output_buffer_idx;
    plan.pending_copies.push_back(std::move(pending));
  } else if (num_pkts != 0) {
    rte_pktmbuf_free_bulk(plan.h_source_mbufs.data(), static_cast<unsigned int>(num_pkts));
  }

  batch->first_packet_cycles = 0;
  batch->first_packet_rx_timestamp_ns = 0;
  batch->first_packet_rx_timestamp_ns_valid = false;
  batch->input_payload_len = 0;
  batch->payload_len = 0;
  batch->packet_count = 0;

  return Status::SUCCESS;
}

Status DpdkEngine::flush_reorder_timeouts(ReorderQueueState& qstate, uint64_t now_cycles) {
  Status final_status = Status::SUCCESS;

  for (auto& plan : qstate.plans) {
    const auto poll_status = poll_reorder_events(plan);
    if (poll_status != Status::SUCCESS && final_status == Status::SUCCESS) {
      final_status = poll_status;
    }
    if (plan.timeout_cycles == 0) { continue; }

    auto& batch = plan.direct_arrival_batch;
    if (batch.first_packet_cycles != 0 && batch.packet_count != 0
        && now_cycles - batch.first_packet_cycles >= plan.timeout_cycles) {
      BurstParams* out = nullptr;
      const auto status = flush_reorder_batch(plan, 0, true, &out);
      if (status != Status::SUCCESS && final_status == Status::SUCCESS) {
        final_status = status;
      }
      if (out != nullptr) {
        qstate.ready_outputs.push_back(out);
      }
    }
  }

  return final_status;
}

Status DpdkEngine::process_burst_for_reorder(uint32_t key, ReorderQueueState& qstate, BurstParams* burst) {
  (void)key;
  if (burst == nullptr) { return Status::NULL_PTR; }

  Status final_status = Status::SUCCESS;
  const uint64_t now_cycles = rte_get_timer_cycles();
  const int num_pkts = static_cast<int>(burst->hdr.hdr.num_pkts);

  if (qstate.single_plan_fast_path && qstate.plans.size() == 1) {
    auto& plan = qstate.plans[0];

    if (plan.use_gpu_backend && plan.stream == nullptr) {
      DAQIRI_LOG_ERROR("Reorder stream is not set for interface port {} queue {} config '{}'",
                       plan.port_id,
                       plan.queue_id,
                       plan.config->name_);
      free_all_packets(burst);
      free_rx_burst(burst);
      return Status::INVALID_PARAMETER;
    }

    for (int pkt_idx = 0; pkt_idx < num_pkts; ++pkt_idx) {
      auto* mbuf = reinterpret_cast<rte_mbuf*>(burst->pkts[0][pkt_idx]);

      void* pkt_ptr = rte_pktmbuf_mtod(mbuf, void*);

      size_t batch_size = 0;
      const auto append_status =
          append_reorder_packet(plan, mbuf, pkt_ptr, now_cycles, &batch_size);
      if (append_status != Status::SUCCESS) {
        if (final_status == Status::SUCCESS) { final_status = append_status; }
        rte_pktmbuf_free(mbuf);
        continue;
      }
      if (batch_size >= plan.packets_per_batch) {
        BurstParams* out = nullptr;
        const auto status = flush_reorder_batch(plan, 0, false, &out);
        if (status != Status::SUCCESS && final_status == Status::SUCCESS) {
          final_status = status;
        }
        if (out != nullptr) {
          qstate.ready_outputs.push_back(out);
        }
      }
    }

    // In the queue-owned fast path all source packets are retained by reorder state until
    // their CUDA event completes. The raw burst metadata is not exposed to the consumer.
    free_rx_burst(burst);
    return final_status;
  }

  std::fill(qstate.plan_pkt_counts.begin(), qstate.plan_pkt_counts.end(), 0U);
  auto& unmatched_indices = qstate.unmatched_indices;
  qstate.unmatched_count = 0;

  for (int i = 0; i < num_pkts; ++i) {
    const FlowId flow_id = get_packet_flow_id(burst, i);
    const auto flow_it = qstate.flow_id_to_plan.find(flow_id);
    if (flow_it == qstate.flow_id_to_plan.end()) {
      if (qstate.unmatched_count >= unmatched_indices.size()) {
        DAQIRI_LOG_ERROR("Reorder unmatched packet scratch storage is full");
        final_status = Status::INTERNAL_ERROR;
        continue;
      }
      unmatched_indices[qstate.unmatched_count++] = i;
      continue;
    }

    const size_t plan_idx = flow_it->second;
    if (plan_idx >= qstate.plans.size()) {
      DAQIRI_LOG_ERROR("Invalid reorder plan index {} for flow {}", plan_idx, flow_id);
      final_status = Status::INTERNAL_ERROR;
      continue;
    }
    if (qstate.plans[plan_idx].use_gpu_backend && qstate.plans[plan_idx].stream == nullptr) {
      DAQIRI_LOG_ERROR("Reorder stream is not set for interface port {} queue {} config '{}'",
                       qstate.plans[plan_idx].port_id,
                       qstate.plans[plan_idx].queue_id,
                       qstate.plans[plan_idx].config->name_);
      free_all_packets(burst);
      free_rx_burst(burst);
      return Status::INVALID_PARAMETER;
    }

    auto& plan_count = qstate.plan_pkt_counts[plan_idx];
    auto& plan_indices = qstate.plan_pkt_indices[plan_idx];
    if (plan_count >= plan_indices.size()) {
      DAQIRI_LOG_ERROR("Reorder plan packet scratch storage is full for plan {}", plan_idx);
      final_status = Status::INTERNAL_ERROR;
      continue;
    }
    plan_indices[plan_count++] = i;
  }

  for (size_t plan_idx = 0; plan_idx < qstate.plan_pkt_indices.size(); ++plan_idx) {
    auto& idxs = qstate.plan_pkt_indices[plan_idx];
    const size_t idx_count = qstate.plan_pkt_counts[plan_idx];
    if (idx_count == 0) { continue; }
    auto& plan = qstate.plans[plan_idx];

    Status status = Status::SUCCESS;
    for (size_t j = 0; j < idx_count; ++j) {
      const int pkt_idx = idxs[j];

      auto* mbuf = reinterpret_cast<rte_mbuf*>(burst->pkts[0][pkt_idx]);

      void* pkt_ptr = rte_pktmbuf_mtod(mbuf, void*);
      size_t batch_size = 0;
      const auto append_status =
          append_reorder_packet(plan, mbuf, pkt_ptr, now_cycles, &batch_size);
      if (append_status != Status::SUCCESS) {
        if (final_status == Status::SUCCESS) { final_status = append_status; }
        rte_pktmbuf_free(mbuf);
        continue;
      }
      if (batch_size >= plan.packets_per_batch) {
        BurstParams* out = nullptr;
        status = flush_reorder_batch(plan, 0, false, &out);
        if (status != Status::SUCCESS && final_status == Status::SUCCESS) {
          final_status = status;
        }
        if (out != nullptr) {
          qstate.ready_outputs.push_back(out);
        }
      }
    }
  }

  const int unmatched_count = static_cast<int>(qstate.unmatched_count);
  if (unmatched_count > 0) {
    for (int out_idx = 0; out_idx < unmatched_count; ++out_idx) {
      const int in_idx = unmatched_indices[out_idx];
      for (int seg = 0; seg < burst->hdr.hdr.num_segs; ++seg) {
        burst->pkts[seg][out_idx] = burst->pkts[seg][in_idx];
      }
    }
    burst->hdr.hdr.num_pkts = unmatched_count;
    qstate.ready_outputs.push_back(burst);
  } else {
    // Matched packets are kept in reorder state and freed when output is emitted.
    free_rx_burst(burst);
  }

  return final_status;
}

Status DpdkEngine::get_next_output_or_ready(uint32_t key, ReorderQueueState& qstate, BurstParams** burst) {
  if (burst == nullptr) { return Status::NULL_PTR; }
  *burst = nullptr;

  for (auto& plan : qstate.plans) {
    const auto poll_status = poll_reorder_events(plan);
    if (poll_status != Status::SUCCESS) { return poll_status; }
  }

  if (!qstate.ready_outputs.empty()) {
    *burst = qstate.ready_outputs.front();
    qstate.ready_outputs.pop_front();
    return Status::SUCCESS;
  }

  Status timeout_status = flush_reorder_timeouts(qstate, rte_get_timer_cycles());
  if (timeout_status != Status::SUCCESS) { return timeout_status; }
  if (!qstate.ready_outputs.empty()) {
    *burst = qstate.ready_outputs.front();
    qstate.ready_outputs.pop_front();
    return Status::SUCCESS;
  }

  const auto ring_it = rx_rings.find(key);
  if (ring_it == rx_rings.end()) { return Status::INVALID_PARAMETER; }

  while (true) {
    BurstParams* raw = nullptr;
    if (rte_ring_dequeue(ring_it->second, reinterpret_cast<void**>(&raw)) < 0) { break; }

    const auto status = process_burst_for_reorder(key, qstate, raw);
    if (status != Status::SUCCESS) { return status; }

    if (!qstate.ready_outputs.empty()) {
      *burst = qstate.ready_outputs.front();
      qstate.ready_outputs.pop_front();
      return Status::SUCCESS;
    }
  }

  timeout_status = flush_reorder_timeouts(qstate, rte_get_timer_cycles());
  if (timeout_status != Status::SUCCESS) { return timeout_status; }
  if (!qstate.ready_outputs.empty()) {
    *burst = qstate.ready_outputs.front();
    qstate.ready_outputs.pop_front();
    return Status::SUCCESS;
  }

  return Status::NOT_READY;
}

void DpdkEngine::release_reorder_output_context(BurstParams* burst) {
  if (burst == nullptr
      || (burst->hdr.hdr.burst_flags & kBurstFlagDpdkReordered) == 0U
      || burst->custom_pkt_data == nullptr) {
    return;
  }

  auto ctx = std::static_pointer_cast<ReorderBurstContext>(burst->custom_pkt_data);
  if (!ctx || ctx->released) { return; }
  release_reorder_output_buffer(ctx->output_pool, ctx->buffer_idx);
  ctx->released = true;
}

Status DpdkEngine::get_reorder_burst_info(BurstParams* burst, ReorderBurstInfo* info) {
  if (burst == nullptr || info == nullptr) { return Status::NULL_PTR; }
  if ((burst->hdr.hdr.burst_flags & kBurstFlagDpdkReordered) == 0U
      || burst->custom_pkt_data == nullptr) {
    return Status::INVALID_PARAMETER;
  }

  auto ctx = std::static_pointer_cast<ReorderBurstContext>(burst->custom_pkt_data);
  if (!ctx) { return Status::INVALID_PARAMETER; }

  if (ctx->h_batch_id != nullptr) {
    if (burst->event != nullptr) {
      const cudaError_t event_status = cudaEventQuery(burst->event);
      if (event_status == cudaErrorNotReady) { return Status::NOT_READY; }
      if (event_status != cudaSuccess) {
        (void)cudaGetLastError();
        return Status::INTERNAL_ERROR;
      }
    }

    ctx->info.batch_id = *(ctx->h_batch_id);
    std::memcpy(burst->hdr.custom_burst_data, &ctx->info, sizeof(ctx->info));
  }

  *info = ctx->info;
  return Status::SUCCESS;
}

Status DpdkEngine::set_reorder_cuda_stream(const std::string& interface_name,
                                        const std::string& reorder_name,
                                        cudaStream_t stream) {
  std::lock_guard<std::mutex> guard(reorder_lock_);

  const int port = get_port_id(interface_name);
  if (port < 0) {
    DAQIRI_LOG_ERROR("Invalid interface '{}' in set_reorder_cuda_stream", interface_name);
    return Status::INVALID_PARAMETER;
  }

  bool found = false;
  for (auto& [qkey, qstate] : reorder_queue_states_) {
    if (get_port_from_key(qkey) != port) { continue; }
    for (auto& plan : qstate.plans) {
      if (plan.config != nullptr && plan.config->name_ == reorder_name) {
        if (!plan.use_gpu_backend) {
          found = true;
          continue;
        }
        const auto mr_it = cfg_.mrs_.find(plan.memory_region_name);
        if (mr_it == cfg_.mrs_.end()) { return Status::INVALID_PARAMETER; }
        cudaSetDevice(plan.cuda_device_id);
        plan.stream = stream;
        found = true;
      }
    }
  }

  if (!found) {
    DAQIRI_LOG_ERROR("Could not find reorder config '{}' on interface '{}'",
                     reorder_name,
                     interface_name);
    return Status::INVALID_PARAMETER;
  }

  return Status::SUCCESS;
}


////////////////////////////////////////////////////////////////////////////////
///
///  \brief Init
///
////////////////////////////////////////////////////////////////////////////////
bool DpdkEngine::set_config_and_initialize(const NetworkConfig& cfg) {
  if (!this->initialized_) {
    cfg_ = cfg;

    if (!validate_config()) {
      DAQIRI_LOG_CRITICAL("Config validation failed");
      return false;
    }
    if (!reserve_static_flow_ids()) {
      DAQIRI_LOG_CRITICAL("Static flow ID reservation failed");
      return false;
    }

    num_init++;

    cpu_set_t mask;
    long nproc, i;

    // Start Initialize in a separate thread so it doesn't set the affinity for the
    // whole application
    std::thread proc_thread(&DpdkEngine::initialize, this);
    proc_thread.join();

    // Our thread should have set the flag if it succeeded
    if (!this->initialized_) {
      num_init--;
      DAQIRI_LOG_CRITICAL("Failed to initialize DPDK");
      return false;
    }

    stats_.Init(cfg_);
    stats_thread_ = std::thread(&DpdkStats::Run, &stats_);

    if (!init_reorder_state()) {
      DAQIRI_LOG_CRITICAL("Failed to initialize reorder state");
      return false;
    }

    run();
  } else {
    num_init++;
  }

  return true;
}

Status DpdkEngine::get_mac_addr(int port, char* mac) {
  if (port > mac_addrs.size()) {
    DAQIRI_LOG_CRITICAL("Port {} out of range in get_mac_addr() lookup");
    return Status::INVALID_PARAMETER;
  }

  memcpy(mac, reinterpret_cast<char*>(&mac_addrs[port]), sizeof(mac_addrs[port]));
  return Status::SUCCESS;
}

void* DpdkEngine::alloc_huge(size_t bytes, int numa) {
  // EAL hugepage allocation: IOVA-contiguous and registrable with the NIC.
  return rte_malloc_socket(nullptr, bytes, 0, numa);
}

void DpdkEngine::adjust_memory_regions() {
  // num_bufs smaller than ~1.5x the NIC descriptor ring deadlock the worker once the ring
  // fills (the ring holds every buffer in the pool with no replacement available, so the
  // next rte_pktmbuf_alloc blocks). Bump such MRs to 3x the ring size up-front -- this runs
  // before allocate_memory_regions(), so the underlying GPU/CPU buffer is sized correctly.
  const uint32_t ring_size          = std::max(default_num_rx_desc, default_num_tx_desc);
  const uint32_t deadlock_threshold = (ring_size * 3) / 2;  // 1.5x ring size
  const uint32_t bumped_num_bufs    = ring_size * 3;        // 3x ring size

  std::unordered_set<std::string> queue_backed_mrs;
  for (const auto& intf : cfg_.ifs_) {
    for (const auto& q : intf.rx_.queues_) {
      for (const auto& n : q.common_.mrs_) { queue_backed_mrs.insert(n); }
    }
    for (const auto& q : intf.tx_.queues_) {
      for (const auto& n : q.common_.mrs_) { queue_backed_mrs.insert(n); }
    }
  }

  for (auto& mr : cfg_.mrs_) {
    if (queue_backed_mrs.count(mr.second.name_) &&
        mr.second.num_bufs_ < deadlock_threshold) {
      DAQIRI_LOG_WARN(
          "MR '{}' had num_bufs={} which is below the {} threshold (1.5x the {} NIC descriptors) "
          "and would deadlock the worker once the ring fills. Bumping to {} (3x ring).",
          mr.second.name_, mr.second.num_bufs_, deadlock_threshold, ring_size, bumped_num_bufs);
      mr.second.num_bufs_ = bumped_num_bufs;
    }

    mr.second.adj_size_ = mr.second.buf_size_ + RTE_PKTMBUF_HEADROOM;
    DAQIRI_LOG_INFO("Adjusting buffer size to {} for headroom", mr.second.adj_size_);
  }
}


bool DpdkEngine::setup_rx_timestamp_dynfield() {
  static bool done = false;
  static int registered_offset = -1;
  static uint64_t registered_mask = 0;

  if (!done) {
    if (rte_mbuf_dyn_rx_timestamp_register(&registered_offset, &registered_mask) < 0) {
      DAQIRI_LOG_CRITICAL("RX timestamp dynamic field registration error: {}",
                          rte_strerror(rte_errno));
      return false;
    }
    done = true;
  }

  if (timestamp_dynfield_offset_ >= 0 && timestamp_dynfield_offset_ != registered_offset) {
    DAQIRI_LOG_CRITICAL("RX timestamp field offset {} does not match existing offset {}",
                        registered_offset,
                        timestamp_dynfield_offset_);
    return false;
  }

  timestamp_dynfield_offset_ = registered_offset;
  rx_timestamp_dynflag_mask_ = registered_mask;
  DAQIRI_LOG_INFO("Done setting up RX timestamping with mask {:x}", rx_timestamp_dynflag_mask_);
  return true;
}

bool DpdkEngine::calibrate_rx_timestamp_clock(uint16_t port_id) {
  uint64_t start_clock = 0;
  int ret = rte_eth_read_clock(port_id, &start_clock);
  if (ret < 0) {
    rte_eth_dev_info dev_info{};
    const int info_ret = rte_eth_dev_info_get(port_id, &dev_info);
    const std::string driver_name =
        (info_ret == 0 && dev_info.driver_name != nullptr) ? dev_info.driver_name : "";
    if (ret == -ENOTSUP && driver_name.find("mlx5") != std::string::npos) {
      rx_timestamp_conversions_[port_id].valid = true;
      rx_timestamp_conversions_[port_id].ticks_per_second = 1000000000ULL;
      DAQIRI_LOG_WARN(
          "rte_eth_read_clock() is not supported on mlx5 port {}; treating PMD-provided "
          "RX hardware timestamps as nanoseconds",
          port_id);
      return true;
    }

    DAQIRI_LOG_CRITICAL(
        "RX hardware timestamps require rte_eth_read_clock() for nanosecond conversion, "
        "but port {} returned err={} ({})",
        port_id,
        ret,
        rte_strerror(-ret));
    return false;
  }

  const auto start_time = std::chrono::steady_clock::now();
  rte_delay_us_block(100000);

  uint64_t end_clock = 0;
  ret = rte_eth_read_clock(port_id, &end_clock);
  if (ret < 0) {
    DAQIRI_LOG_CRITICAL(
        "RX hardware timestamp clock calibration failed on port {}: err={} ({})",
        port_id,
        ret,
        rte_strerror(-ret));
    return false;
  }

  const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now() - start_time)
                              .count();
  if (elapsed_ns <= 0 || end_clock <= start_clock) {
    DAQIRI_LOG_CRITICAL(
        "RX hardware timestamp clock calibration produced an invalid sample on port {} "
        "(start={}, end={}, elapsed_ns={})",
        port_id,
        start_clock,
        end_clock,
        elapsed_ns);
    return false;
  }

  const uint64_t delta_ticks = end_clock - start_clock;
  const auto ticks_per_second =
      (static_cast<unsigned __int128>(delta_ticks) * 1000000000ULL +
       static_cast<uint64_t>(elapsed_ns / 2)) /
      static_cast<uint64_t>(elapsed_ns);
  if (ticks_per_second == 0 ||
      ticks_per_second > static_cast<unsigned __int128>(std::numeric_limits<uint64_t>::max())) {
    DAQIRI_LOG_CRITICAL(
        "RX hardware timestamp clock calibration produced an invalid frequency on port {}",
        port_id);
    return false;
  }

  rx_timestamp_conversions_[port_id].valid = true;
  rx_timestamp_conversions_[port_id].ticks_per_second = static_cast<uint64_t>(ticks_per_second);
  DAQIRI_LOG_INFO("Calibrated RX timestamp clock for port {} at {} ticks/second",
                  port_id,
                  rx_timestamp_conversions_[port_id].ticks_per_second);
  return true;
}

bool DpdkEngine::setup_tx_timestamp_dynfield() {
  static bool done = false;
  static int registered_offset = -1;
  static uint64_t registered_mask = 0;

  if (!done) {
    if (rte_mbuf_dyn_tx_timestamp_register(&registered_offset, &registered_mask) < 0) {
      DAQIRI_LOG_CRITICAL("TX timestamp dynamic field registration error: {}",
                          rte_strerror(rte_errno));
      return false;
    }
    done = true;
  }

  if (timestamp_dynfield_offset_ >= 0 && timestamp_dynfield_offset_ != registered_offset) {
    DAQIRI_LOG_CRITICAL("TX timestamp field offset {} does not match existing offset {}",
                        registered_offset,
                        timestamp_dynfield_offset_);
    return false;
  }

  timestamp_dynfield_offset_ = registered_offset;
  tx_timestamp_dynflag_mask_ = registered_mask;
  DAQIRI_LOG_INFO("Done setting up accurate send scheduling with mask {:x}",
                  tx_timestamp_dynflag_mask_);
  return true;
}

uint64_t DpdkEngine::now_tx_ns(uint16_t port) {
  // The mlx5 send scheduler (tx_pp / SEND_ON_TIMESTAMP) releases packets when its
  // own PTP hardware clock reaches the per-packet timestamp. To pace accurately we
  // MUST seed pace_next_ns from that same clock: any offset between the seed clock
  // and the NIC clock becomes a fixed per-burst scheduling latency (e.g. seeding
  // from a host clock that runs ~0.2 s ahead parks every burst ~0.2 s in the NIC's
  // future, which caps throughput). rte_eth_read_clock() returns exactly that
  // clock, so prefer it.
  uint64_t clk = 0;
  if (rte_eth_read_clock(port, &clk) == 0) {
    const uint64_t tps =
        (port < rx_timestamp_conversions_.size() && rx_timestamp_conversions_[port].valid)
            ? rx_timestamp_conversions_[port].ticks_per_second
            : 0;
    // With REAL_TIME_CLOCK_ENABLE the device clock already counts nanoseconds, so
    // an uncalibrated tps (no RX-timestamp path active) means clk is ns as-is.
    if (tps == 0 || tps == 1000000000ULL) { return clk; }
    return static_cast<uint64_t>(static_cast<unsigned __int128>(clk) * 1000000000ULL / tps);
  }
  // Fallback: rte_eth_read_clock unsupported. The NIC PTP clock free-runs from ~0
  // at driver load (unless a PTP daemon disciplines it), tracking CLOCK_MONOTONIC,
  // NOT CLOCK_REALTIME (UTC). A small constant host/NIC offset only shifts the
  // schedule, not the inter-burst spacing, so CLOCK_MONOTONIC keeps it closest.
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

// HWS doesn't allow zero queues on an interface, so we make some dummy interfaces here for
// users that are only doing TX
void DpdkEngine::create_dummy_rx_q() {
  for (auto& intf : cfg_.ifs_) {
    auto& rx = intf.rx_;

    // With hardware steering we need to create a fake queue or DPDK will segfault when setting
    // the template parameters
    if (rx.queues_.size() == 0) {
      DAQIRI_LOG_INFO("Port {} has no RX queues. Creating dummy queue.", intf.port_id_);
      const std::string mr_name = "MR_Unused_P" + std::to_string(intf.port_id_);
      RxQueueConfig tmp_q;
      tmp_q.common_.name_ = "UNUSED_P" + std::to_string(intf.port_id_) + "_Q0";
      tmp_q.common_.id_ = 0;
      tmp_q.common_.batch_size_ = 1;
      tmp_q.common_.split_boundary_ = 0;
      tmp_q.common_.cpu_core_ = "0";
      tmp_q.common_.mrs_.push_back(mr_name);
      tmp_q.common_.extra_queue_config_ = nullptr;
      rx.queues_.push_back(tmp_q);


      // Create unused MR
      MemoryRegionConfig tmp_mr;
      tmp_mr.name_ = mr_name;
      tmp_mr.kind_ = MemoryKind::HUGE;
      tmp_mr.affinity_ = 0;
      tmp_mr.access_ = 0;
      tmp_mr.buf_size_ = JUMBOFRAME_SIZE;
      tmp_mr.num_bufs_ = 32768;
      tmp_mr.owned_ = true;
      cfg_.mrs_[mr_name] = tmp_mr;
    }
  }
}

// DPDK 25.11+ requires at least one TX queue per initialized interface.
void DpdkEngine::create_dummy_tx_q() {
  for (auto& intf : cfg_.ifs_) {
    auto& tx = intf.tx_;

    if (tx.queues_.size() == 0) {
      DAQIRI_LOG_INFO("Port {} has no TX queues. Creating dummy queue.", intf.port_id_);
      const std::string mr_name = "MR_Unused_TX_P" + std::to_string(intf.port_id_);
      TxQueueConfig tmp_q;
      tmp_q.common_.name_ = "UNUSED_TX_P" + std::to_string(intf.port_id_) + "_Q0";
      tmp_q.common_.id_ = 0;
      tmp_q.common_.batch_size_ = 1;
      tmp_q.common_.split_boundary_ = 0;
      tmp_q.common_.cpu_core_ = "0";
      tmp_q.common_.mrs_.push_back(mr_name);
      tmp_q.common_.extra_queue_config_ = nullptr;
      tx.queues_.push_back(tmp_q);

      // Create unused MR
      MemoryRegionConfig tmp_mr;
      tmp_mr.name_ = mr_name;
      tmp_mr.kind_ = MemoryKind::HUGE;
      tmp_mr.affinity_ = 0;
      tmp_mr.access_ = 0;
      tmp_mr.buf_size_ = JUMBOFRAME_SIZE;
      tmp_mr.num_bufs_ = 32768;
      tmp_mr.owned_ = true;
      cfg_.mrs_[mr_name] = tmp_mr;
    }
  }
}

void DpdkEngine::initialize() {
  int ret;

  // Cleanup-on-failure guard: if initialize() returns without setting
  // initialized_ = true, this will call rte_eal_cleanup() and best-effort
  // unlink any --file-prefix=<...>map_* files we created. Prevents pinned
  // hugepages from blocking the next run.
  struct EalCleanupGuard {
    DpdkEngine* engine;
    ~EalCleanupGuard() {
      if (!engine->initialized_) {
        engine->cleanup_dynamic_flows();
        engine->destroy_all_flow_rules();
        engine->cleanup_eal();
      }
    }
  } cleanup_guard{this};

  if (!check_hugepage_availability()) {
    DAQIRI_LOG_CRITICAL("Aborting before rte_eal_init() to keep /dev/hugepages clean");
    return;
  }

  static struct rte_eth_conf conf_eth_port = {
      .rxmode = {
              .mq_mode = RTE_ETH_MQ_RX_RSS,
              .offloads = 0,
          },
      .txmode = {
              .mq_mode = RTE_ETH_MQ_TX_NONE,
              .offloads = 0,
          },
      .rx_adv_conf = {
              .rss_conf = {.rss_key = NULL, .rss_hf = RTE_ETH_RSS_IP},
          },
  };

  loopback_ = cfg_.common_.loopback_;
  if (loopback_ == LoopbackType::LOOPBACK_TYPE_SW) {
    if (cfg_.ifs_.size() > 1) {
      DAQIRI_LOG_CRITICAL("Only a single interface allowed for loopback mode");
      return;
    }

    if (cfg_.ifs_[0].rx_.queues_.size() > 1 || cfg_.ifs_[0].tx_.queues_.size() > 1) {
      DAQIRI_LOG_CRITICAL("Only one queue allowed for loopback mode");
      return;
    }
  }

  for (auto& conf : local_port_conf) { conf = conf_eth_port; }

  /* Initialize DPDK params */
  constexpr int max_nargs = 32;
  constexpr int max_arg_size = 64;
  char** _argv;
  _argv = (char**)malloc(sizeof(char*) * max_nargs);
  for (int i = 0; i < max_nargs; i++) { _argv[i] = (char*)malloc(max_arg_size); }

  int arg = 0;
  std::string cores = std::to_string(cfg_.common_.master_core_) + ",";  // Master core must be first
  std::set<std::string> ifs;

  std::unordered_map<uint16_t, std::string> port_id_to_name;

  // Get GPU PCIe BDFs since they're needed to pass to DPDK
  for (const auto& intf : cfg_.ifs_) {
    if (loopback_ != LoopbackType::LOOPBACK_TYPE_SW) {
      if (looks_like_mac_address(intf.address_)) {
        DAQIRI_LOG_CRITICAL(
            "Interface '{}' has address '{}' which looks like a MAC address. "
            "DPDK expects a PCI BDF for interfaces[].address (e.g. 0000:17:00.0).",
            intf.name_,
            intf.address_);
        return;
      }
      if (!looks_like_pci_bdf(intf.address_)) {
        DAQIRI_LOG_CRITICAL(
            "Interface '{}' has invalid address '{}'. DPDK expects a PCI BDF in the format "
            "dddd:bb:dd.f (example: 0000:17:00.0). Remove placeholders like '<...>'.",
            intf.name_,
            intf.address_);
        return;
      }
    } else if (intf.address_ != "loopback" && !looks_like_pci_bdf(intf.address_)) {
      DAQIRI_LOG_CRITICAL(
          "Interface '{}' has invalid loopback address '{}'. In software loopback mode use "
          "'loopback' (recommended) or a valid PCI BDF.",
          intf.name_,
          intf.address_);
      return;
    }

    ifs.emplace(intf.address_);
    for (const auto& q : intf.rx_.queues_) { cores += q.common_.cpu_core_ + ","; }

    for (const auto& q : intf.tx_.queues_) { cores += q.common_.cpu_core_ + ","; }
  }

  cores = cores.substr(0, cores.size() - 1);
  // Get a unique set of interfaces
  num_ports = ifs.size();
  DAQIRI_LOG_INFO("Attempting to use {} ports for high-speed network", num_ports);

  strncpy(_argv[arg++], "operator", max_arg_size - 1);
  eal_file_prefix_ = generate_random_string(10);
  strncpy(_argv[arg++],
          (std::string("--file-prefix=") + eal_file_prefix_).c_str(),
          max_arg_size - 1);
  strncpy(_argv[arg++], "-l", max_arg_size - 1);
  strncpy(_argv[arg++], cores.c_str(), max_arg_size - 1);

  DAQIRI_LOG_INFO(
      "Setting DPDK log level to: {}",
      DpdkLogLevel::to_description_string(DpdkLogLevel::from_ano_log_level(cfg_.log_level_)));

  DpdkLogLevelCommandBuilder cmd(cfg_.log_level_);
  for (auto& c : cmd.get_cmd_flags_strings()) {
    strncpy(_argv[arg++], c.c_str(), max_arg_size - 1);
  }

  bool requires_extmem_iova_va = false;
  for (const auto& mr : cfg_.mrs_) {
    if (mr.second.kind_ != MemoryKind::HUGE) {
      requires_extmem_iova_va = true;
      break;
    }
  }
  if (requires_extmem_iova_va) {
    // External memory registrations use RTE_BAD_IOVA, which requires VA IOVA mode.
    strncpy(_argv[arg++], "--iova-mode=va", max_arg_size - 1);
  }

  // Accurate send scheduling (accurate_send) and packet pacing (pacing_mbps)
  // ride the mlx5 send-on-timestamp scheduler, which only runs when the tx_pp
  // devarg (pacing granularity in ns) is set. Add it once when any TX path needs
  // scheduling so the offload enabled later actually engages.
  bool needs_send_sched = false;
  for (const auto& intf : cfg_.ifs_) {
    if (intf.tx_.accurate_send_) {
      needs_send_sched = true;
      break;
    }
    for (const auto& q : intf.tx_.queues_) {
      if (q.pacing_mbps_ > 0) {
        needs_send_sched = true;
        break;
      }
    }
    if (needs_send_sched) {
      break;
    }
  }

  if (loopback_ != LoopbackType::LOOPBACK_TYPE_SW) {
    for (const auto& name : ifs) {
      strncpy(_argv[arg++], "-a", max_arg_size - 1);
      std::string devargs = name + std::string(",txq_inline_max=0,dv_flow_en=2");
      if (needs_send_sched) {
        devargs += ",tx_pp=500";  // 500 ns scheduling granularity
      }
      strncpy(_argv[arg++], devargs.c_str(), max_arg_size - 1);
    }
  }

  _argv[arg] = nullptr;
  std::string dpdk_args = "";
  for (int ac = 0; ac < arg; ac++) { dpdk_args += std::string(_argv[ac]) + " "; }

  DAQIRI_LOG_INFO("DPDK EAL arguments: {}", dpdk_args);

  ret = rte_eal_init(arg, _argv);
  if (ret < 0) {
    DAQIRI_LOG_CRITICAL(
        "Invalid EAL arguments: errno={} ({})",
        rte_errno,
        rte_strerror(rte_errno));
    if (rte_errno == EACCES || rte_errno == EPERM) {
      DAQIRI_LOG_CRITICAL(
          "DPDK permission error. Verify hugepages are configured and mounted, container has "
          "required privileges (vfio/uio), and NIC PCI devices are bound to a DPDK-compatible driver.");
    }
    return;
  }
  eal_initialized_ = true;

  // Set up the port IDs to map to DPDK port IDs
  for (auto& intf : cfg_.ifs_) {
    if (loopback_ == LoopbackType::LOOPBACK_TYPE_SW) {
      intf.port_id_ = 0;
    } else if (rte_eth_dev_get_port_by_name(intf.address_.c_str(), &intf.port_id_) < 0) {
      DAQIRI_LOG_CRITICAL("Failed to get port number for {} ({})", intf.name_, intf.address_);
      return;
    }
    DAQIRI_LOG_INFO("{} ({}): identified as port {}", intf.name_, intf.address_, intf.port_id_);
  }

  for (int i = 0; i < num_ports; i++) {
    rte_eth_macaddr_get(i, &mac_addrs[i]);
  }

  // Initialize the mapping to determine how many RX queues per core
  this->init_rx_core_q_map();

  DAQIRI_LOG_INFO("Creating dummy RX and TX queues");
  create_dummy_rx_q();
  create_dummy_tx_q();

  // Adjust the sizes to accommodate any padding/alignment restrictions by this library
  adjust_memory_regions();

  if (allocate_memory_regions() != Status::SUCCESS) {
    DAQIRI_LOG_CRITICAL("Failed to allocate memory");
    return;
  }

  if (register_memory_regions() != Status::SUCCESS) {
    DAQIRI_LOG_CRITICAL("Failed to register MRs");
    return;
  }

  if (loopback_ != LoopbackType::LOOPBACK_TYPE_SW) {
    if (map_memory_regions() != Status::SUCCESS) {
      DAQIRI_LOG_CRITICAL("Failed to map MRs");
      return;
    }
  }

  // Build name to id mapping
  int max_rx_batch_size = 0;
  int max_tx_batch_size = 0;
  for (auto& intf : cfg_.ifs_) {
    [[maybe_unused]] struct rte_eth_dev_info dev_info;

    if (loopback_ != LoopbackType::LOOPBACK_TYPE_SW) {
      int ret = rte_eth_dev_info_get(intf.port_id_, &dev_info);
      if (ret != 0) {
        DAQIRI_LOG_CRITICAL("Failed to get device info for port {}", intf.port_id_);
        return;
      }
    }

    port_q_num[intf.port_id_] = {intf.rx_.queues_.size(), intf.tx_.queues_.size()};
    port_id_to_name[intf.port_id_] = intf.address_;

    DAQIRI_LOG_INFO("DPDK init ({}) -- RX: {} TX: {}",
                      intf.address_,
                      intf.rx_.queues_.size() > 0 ? "ENABLED" : "DISABLED",
                      intf.tx_.queues_.size() > 0 ? "ENABLED" : "DISABLED");

    // Queue setup
    size_t max_pkt_size = 0;
    size_t max_rx_pkt_size = 0;
    bool single_seg_rx_needs_scatter = false;
    size_t min_single_seg_rx_buf_size = std::numeric_limits<size_t>::max();
    const auto& rx = intf.rx_;

    for (auto& q : rx.queues_) {
      DAQIRI_LOG_INFO("Configuring RX queue: {} ({}) on port {}",
                        q.common_.name_,
                        q.common_.id_,
                        intf.port_id_);
      auto q_backend = new DPDKQueueConfig{};
      max_rx_batch_size = std::max(max_rx_batch_size, q.common_.batch_size_);

      size_t q_packet_size = 0;
      for (int mr_num = 0; mr_num < q.common_.mrs_.size(); mr_num++) {
        std::string append = "_P" + std::to_string(intf.port_id_) + "_Q" +
                            std::to_string(q.common_.id_) + "_MR" + std::to_string(mr_num);
        std::string pool_name = std::string("RXP") + append;
        const auto& mr = cfg_.mrs_[q.common_.mrs_[mr_num]];

        if (loopback_ != LoopbackType::LOOPBACK_TYPE_SW) {  // Loopback needs no RX pools

          struct rte_mempool* pool = create_pktmbuf_pool(pool_name, mr);
          if (pool == nullptr) {
            DAQIRI_LOG_CRITICAL(
                  "Could not create external memory mempool {}: mbufs={} elsize={} ptr={}",
                  pool_name,
                  mr.num_bufs_,
                  mr.adj_size_,
                  (void*)pool);
            return;
          }

          q_backend->pools.push_back(pool);
          DAQIRI_LOG_INFO("Created mempool {} : mbufs={} elsize={} ptr={}",
                            pool_name,
                            mr.num_bufs_,
                            mr.adj_size_,
                            (void*)pool);

          q_packet_size += mr.buf_size_;
        }
      }

      max_pkt_size = std::max(max_pkt_size, q_packet_size);
      max_rx_pkt_size = std::max(max_rx_pkt_size, q_packet_size);
      DAQIRI_LOG_INFO("Max packet size needed for RX: {}", max_pkt_size);

      // Keep scatter available for single-segment RX so queue setup is not tied to MTU sizing.
      if (q.common_.mrs_.size() == 1 && q_packet_size > 0) {
        single_seg_rx_needs_scatter = true;
        min_single_seg_rx_buf_size = std::min(min_single_seg_rx_buf_size, q_packet_size);
      }

      // Multiple segments
      if (q.common_.mrs_.size() > 1) {
        memcpy(
            &q_backend->rxconf_qsplit, &dev_info.default_rxconf, sizeof(q_backend->rxconf_qsplit));

        q_backend->rxconf_qsplit.offloads =
            RTE_ETH_RX_OFFLOAD_SCATTER | RTE_ETH_RX_OFFLOAD_BUFFER_SPLIT;
        if (rx.hardware_timestamps_) {
          q_backend->rxconf_qsplit.offloads |= RTE_ETH_RX_OFFLOAD_TIMESTAMP;
        }
        q_backend->rxconf_qsplit.rx_nseg = q.common_.mrs_.size();

        q_backend->rx_useg.resize(q.common_.mrs_.size());
        q_backend->rxconf_qsplit.rx_seg = &q_backend->rx_useg[0];

        for (int seg = 0; seg < q.common_.mrs_.size(); seg++) {
          struct rte_eth_rxseg_split* rx_seg;

          rx_seg = &q_backend->rx_useg[seg].split;
          rx_seg->mp = q_backend->pools[seg];
          rx_seg->length = (seg == (q.common_.mrs_.size() - 1))
                              ? 0
                              : cfg_.mrs_[q.common_.mrs_[seg]].adj_size_ - RTE_PKTMBUF_HEADROOM;
          rx_seg->offset = 0;
        }

        local_port_conf[intf.port_id_].rxmode.offloads |=
            RTE_ETH_RX_OFFLOAD_SCATTER | RTE_ETH_RX_OFFLOAD_BUFFER_SPLIT;
      }

      uint32_t key = generate_queue_key(intf.port_id_, q.common_.id_);
      rx_dpdk_q_map_[key] = q_backend;
      rx_cfg_q_map_[key]  = &q;
    }

    local_port_conf[intf.port_id_].rxmode.offloads |= RTE_ETH_RX_OFFLOAD_CHECKSUM;

    if (rx.hardware_timestamps_) {
      if (loopback_ == LoopbackType::LOOPBACK_TYPE_SW) {
        DAQIRI_LOG_CRITICAL(
            "RX hardware timestamps are enabled for interface '{}', but software loopback "
            "does not support hardware timestamping",
            intf.name_);
        return;
      }
      if ((dev_info.rx_offload_capa & RTE_ETH_RX_OFFLOAD_TIMESTAMP) == 0) {
        DAQIRI_LOG_CRITICAL(
            "RX hardware timestamps are enabled for interface '{}', but port {} does not "
            "support RTE_ETH_RX_OFFLOAD_TIMESTAMP",
            intf.name_,
            intf.port_id_);
        return;
      }
      if (!setup_rx_timestamp_dynfield()) { return; }
      local_port_conf[intf.port_id_].rxmode.offloads |= RTE_ETH_RX_OFFLOAD_TIMESTAMP;
    }

    // TX now
    // For now make a single queue. Support more sophisticated TX on next release
    const auto& tx = intf.tx_;

    for (auto& q : tx.queues_) {
      DAQIRI_LOG_INFO("Configuring TX queue: {} ({}) on port {}",
                        q.common_.name_,
                        q.common_.id_,
                        intf.port_id_);
      auto q_backend = new DPDKQueueConfig{};
      max_tx_batch_size = std::max(max_tx_batch_size, q.common_.batch_size_);
      size_t q_packet_size = 0;

      for (int mr_num = 0; mr_num < q.common_.mrs_.size(); mr_num++) {
        std::string append = "_P" + std::to_string(intf.port_id_) + "_Q" +
                             std::to_string(q.common_.id_) + "_MR" + std::to_string(mr_num);
        std::string pool_name = std::string("TXP") + append;
        const auto& mr = cfg_.mrs_[q.common_.mrs_[mr_num]];

        struct rte_mempool* pool = create_pktmbuf_pool(pool_name, mr);
        if (pool == nullptr) {
          DAQIRI_LOG_CRITICAL("Could not create external memory mempool");
          return;
        }

        q_backend->pools.push_back(pool);
        DAQIRI_LOG_INFO("Created mempool {} : mbufs={} elsize={} ptr={}",
                          pool_name,
                          mr.num_bufs_,
                          mr.buf_size_,
                          (void*)pool);

        q_packet_size += mr.buf_size_;
      }

      max_pkt_size = std::max(max_pkt_size, q_packet_size);
      uint32_t key = generate_queue_key(intf.port_id_, q.common_.id_);
      tx_dpdk_q_map_[key] = q_backend;
    }

    const size_t rx_tunnel_overhead = flow_max_decap_wire_overhead(rx.flows_);
    if (rx_tunnel_overhead > 0) {
      DAQIRI_LOG_INFO("Adding {} bytes of RX tunnel overhead for MTU sizing", rx_tunnel_overhead);
      max_rx_pkt_size += rx_tunnel_overhead;
      max_pkt_size = std::max(max_pkt_size, max_rx_pkt_size);
    }

    DAQIRI_LOG_INFO("Max packet size needed with TX: {}", max_pkt_size);
    DAQIRI_LOG_INFO("Max packet size needed with RX only: {}", max_rx_pkt_size);

    local_port_conf[intf.port_id_].txmode.offloads = 0;

    // Compute MTU conservatively. Clamp to jumbo frame bounds and enforce a valid minimum MTU.
    max_rx_pkt_size = std::max(max_rx_pkt_size, 64UL);
    const size_t requested_frame_size =
        std::min(max_rx_pkt_size, static_cast<size_t>(JUMBOFRAME_SIZE));
    const size_t crc_and_hdr = RTE_ETHER_CRC_LEN + RTE_ETHER_HDR_LEN;
    const size_t requested_mtu =
        (requested_frame_size > crc_and_hdr) ? (requested_frame_size - crc_and_hdr) : 0;
    local_port_conf[intf.port_id_].rxmode.mtu =
        std::max(requested_mtu, static_cast<size_t>(RTE_ETHER_MIN_MTU));
    local_port_conf[intf.port_id_].rxmode.max_lro_pkt_size =
        local_port_conf[intf.port_id_].rxmode.mtu;

    DAQIRI_LOG_INFO("Setting port config for port {} mtu:{}",
                      intf.port_id_,
                      local_port_conf[intf.port_id_].rxmode.mtu);

    if (single_seg_rx_needs_scatter) {
      if (loopback_ != LoopbackType::LOOPBACK_TYPE_SW) {
        if ((dev_info.rx_offload_capa & RTE_ETH_RX_OFFLOAD_SCATTER) == 0) {
          DAQIRI_LOG_CRITICAL(
              "Single-segment RX queue present (min buffer size {}). NIC does not "
              "support RTE_ETH_RX_OFFLOAD_SCATTER",
              min_single_seg_rx_buf_size);
          return;
        }
        local_port_conf[intf.port_id_].rxmode.offloads |= RTE_ETH_RX_OFFLOAD_SCATTER;
        DAQIRI_LOG_INFO(
            "Enabling RX scatter offload for single-segment RX queues (min buffer size: {})",
            min_single_seg_rx_buf_size);
      } else {
        DAQIRI_LOG_WARN(
            "Single-segment RX queue appears to need scatter in SW loopback; skipping offload check");
      }
    }

    // The SEND_ON_TIMESTAMP offload backs both accurate_send (per-packet
    // set_packet_tx_time) and per-queue packet pacing (pacing_mbps), so enable
    // it when either is requested.
    bool tx_pacing_requested = false;
    for (const auto& pq : tx.queues_) {
      if (pq.pacing_mbps_ > 0) {
        tx_pacing_requested = true;
        break;
      }
    }
    if (loopback_ != LoopbackType::LOOPBACK_TYPE_SW &&
        (tx.accurate_send_ || tx_pacing_requested)) {
      // dev_info was already fetched and validated at the top of this loop
      // iteration (the non-SW-loopback branch returns on failure), so it is
      // safe to inspect tx_offload_capa here directly.
      if ((dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_SEND_ON_TIMESTAMP) == 0) {
        // accurate_send is a hard requirement; pacing degrades to line rate.
        if (tx.accurate_send_) {
          DAQIRI_LOG_CRITICAL(
              "Accurate send scheduling enabled in config, but not supported by NIC!");
          return;
        }
        DAQIRI_LOG_WARN(
            "TX packet pacing requested on port {} but NIC lacks SEND_ON_TIMESTAMP offload; "
            "pacing disabled (line rate)",
            intf.port_id_);
      } else {
        if (!setup_tx_timestamp_dynfield()) { return; }
        local_port_conf[intf.port_id_].txmode.offloads |= RTE_ETH_TX_OFFLOAD_SEND_ON_TIMESTAMP;
      }
    }

    if ((dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE) != 0) {
      local_port_conf[intf.port_id_].txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;
    }

    local_port_conf[intf.port_id_].txmode.mq_mode = RTE_ETH_MQ_TX_NONE;
    local_port_conf[intf.port_id_].txmode.offloads |=
        RTE_ETH_TX_OFFLOAD_IPV4_CKSUM | RTE_ETH_TX_OFFLOAD_UDP_CKSUM |
        RTE_ETH_TX_OFFLOAD_TCP_CKSUM | RTE_ETH_TX_OFFLOAD_MULTI_SEGS;


    DAQIRI_LOG_INFO("Initializing port {} with {} RX queues and {} TX queues...",
                      intf.port_id_,
                      intf.rx_.queues_.size(),
                      intf.tx_.queues_.size());

    if (loopback_ != LoopbackType::LOOPBACK_TYPE_SW) {
      if (intf.rx_.flow_isolation_) {
        struct rte_flow_error error;
        ret = rte_flow_isolate(intf.port_id_, 1, &error);
        if (ret < 0) {
          DAQIRI_LOG_CRITICAL("Failed to set flow isolation on port {}: {} ({})",
                              intf.port_id_,
                              error.message ? error.message : "unknown error",
                              rte_strerror(rte_errno));
          return;
        } else {
          DAQIRI_LOG_INFO("Port {} in isolation mode", intf.port_id_);
        }
      } else {
        DAQIRI_LOG_INFO("Port {} not in isolation mode", intf.port_id_);
      }

      ret = rte_eth_dev_configure(intf.port_id_,
                                  intf.rx_.queues_.size(),
                                  intf.tx_.queues_.size(),
                                  &local_port_conf[intf.port_id_]);
      if (ret < 0) {
        DAQIRI_LOG_CRITICAL("Cannot configure device: err={}, str={}, port={}",
                              ret,
                              rte_strerror(ret),
                              intf.port_id_);
        return;
      } else {
        DAQIRI_LOG_INFO("Successfully configured ethdev");
      }

      ret =
          rte_eth_dev_adjust_nb_rx_tx_desc(intf.port_id_,
                                           &default_num_rx_desc,
                                           &default_num_tx_desc);
      if (ret < 0) {
        DAQIRI_LOG_CRITICAL(
            "Cannot adjust number of descriptors: err={}, port={}", ret, intf.port_id_);
        return;
      } else {
        DAQIRI_LOG_INFO("Successfully set descriptors to {}/{}",
          default_num_rx_desc, default_num_tx_desc);
      }

      rte_eth_macaddr_get(intf.port_id_, &conf_ports_eth_addr[intf.port_id_]);

      if (rx.dynamic_flow_capacity_ > 0) {
        configure_flow_api_for_port(intf.port_id_, rx.dynamic_flow_capacity_);
      }

      for (const auto& q : rx.queues_) {
        // Assume one core for now
        auto socketid = rte_lcore_to_socket_id(strtol(q.common_.cpu_core_.c_str(), nullptr, 10));
        uint32_t key = generate_queue_key(intf.port_id_, q.common_.id_);
        auto qinfo = rx_dpdk_q_map_[key];

        DAQIRI_LOG_INFO("Setting up port:{}, queue:{}, Num scatter:{} pool:{}",
                          intf.port_id_,
                          q.common_.id_,
                          q.common_.mrs_.size(),
                          (void*)qinfo->pools[0]);
        if (q.common_.mrs_.size() > 1) {
          ret = rte_eth_rx_queue_setup(intf.port_id_,
                                       q.common_.id_,
                                       default_num_rx_desc,
                                       socketid,
                                       &qinfo->rxconf_qsplit,
                                       NULL);
        } else {
          ret = rte_eth_rx_queue_setup(
              intf.port_id_, q.common_.id_, default_num_rx_desc, socketid, NULL, qinfo->pools[0]);
        }

        if (ret < 0) {
          DAQIRI_LOG_CRITICAL("rte_eth_rx_queue_setup: err={}, port={}", ret, intf.port_id_);
          return;
        } else {
          DAQIRI_LOG_INFO("Successfully setup RX port {} queue {}", intf.port_id_, q.common_.id_);
        }
      }

      struct rte_eth_txconf txq_conf;
      for (const auto& q : tx.queues_) {
        txq_conf = dev_info.default_txconf;
        txq_conf.offloads = local_port_conf[intf.port_id_].txmode.offloads;
        ret = rte_eth_tx_queue_setup(intf.port_id_,
                                     q.common_.id_,
                                     default_num_tx_desc,
                                     rte_eth_dev_socket_id(intf.port_id_),
                                     &txq_conf);
        if (ret < 0) {
          DAQIRI_LOG_CRITICAL("Queue setup error {}:{}, port={} caps={:x} set={:x}",
                                ret,
                                rte_strerror(ret),
                                intf.port_id_,
                                dev_info.tx_offload_capa,
                                local_port_conf[intf.port_id_].txmode.offloads);
          return;
        } else {
          DAQIRI_LOG_INFO("Successfully set up TX queue {}/{}", intf.port_id_, q.common_.id_);
        }
      }

      if (!intf.rx_.flow_isolation_) {
        DAQIRI_LOG_INFO("Enabling promiscuous mode for port {}", intf.port_id_);
        rte_eth_promiscuous_enable(intf.port_id_);
      } else {
        DAQIRI_LOG_INFO(
            "Not enabling promiscuous mode on port {} "
            "since flow isolation is enabled",
            intf.port_id_);
      }

      ret = rte_eth_dev_start(intf.port_id_);
      if (ret != 0) {
        DAQIRI_LOG_CRITICAL("Cannot start device err={}, port={}", ret, intf.port_id_);
        return;
      } else {
        DAQIRI_LOG_INFO("Successfully started port {}", intf.port_id_);
      }

      DAQIRI_LOG_INFO("Port {}, MAC address: {:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}",
                        intf.port_id_,
                        conf_ports_eth_addr[intf.port_id_].addr_bytes[0],
                        conf_ports_eth_addr[intf.port_id_].addr_bytes[1],
                        conf_ports_eth_addr[intf.port_id_].addr_bytes[2],
                        conf_ports_eth_addr[intf.port_id_].addr_bytes[3],
                        conf_ports_eth_addr[intf.port_id_].addr_bytes[4],
                        conf_ports_eth_addr[intf.port_id_].addr_bytes[5]);

      if (rx.hardware_timestamps_ && !calibrate_rx_timestamp_clock(intf.port_id_)) { return; }

      // Standard (group 3) and flex-item (group 1) flows use separate DPDK flow
      // groups with conflicting group-0 jump rules; validate_config() rejects mixed
      // configs per interface.
      bool has_standard_flows = false;
      bool has_flex_item_flows = false;
      for (const auto& flow : rx.flows_) {
        DAQIRI_LOG_INFO("Adding RX flow {}", flow.name_);
        struct rte_flow* created = nullptr;
        if (flow.match_.type_ == FlowMatchType::FLEX_ITEM) {
          created = add_flex_item_flow(
              intf.port_id_, flow.match_.flex_item_match_, flow.action_.id_, flow.id_);
          if (created != nullptr) { has_flex_item_flows = true; }
        } else {
          created = add_flow(intf.port_id_, flow);
          if (created != nullptr) { has_standard_flows = true; }
        }
        if (created == nullptr) { return; }
      }

      if (intf.rx_.flow_isolation_) {
        if (has_standard_flows && !add_send_to_kernel_fallback(intf.port_id_, 3)) { return; }
        if (has_flex_item_flows && !add_send_to_kernel_fallback(intf.port_id_, 1)) { return; }
      }

      for (const auto& flow : tx.flows_) {
        DAQIRI_LOG_INFO("Adding TX flow {}", flow.name_);
        if (add_tx_flow(intf.port_id_, flow) == nullptr) { return; }
      }

      if (!apply_tx_offloads(intf.port_id_)) { return; }
    } else {
      DAQIRI_LOG_INFO("Software loopback mode configured");
    }
  }


  if (setup_pools_and_rings(max_rx_batch_size, max_tx_batch_size) < 0) {
    DAQIRI_LOG_ERROR("Failed to set up pools and rings!");
    return;
  }

  // Initialize all drop_all_traffic_flow pointers to nullptr
  for (auto& config : drop_all_traffic_flow) {
    config.drop = nullptr;
  }

  this->initialized_ = true;
}

int DpdkEngine::setup_pools_and_rings(int max_rx_batch, int max_tx_batch) {
  DAQIRI_LOG_DEBUG("Setting up RX rings");
  for (int i = 0; i < cfg_.ifs_.size(); i++) {
    int port_id = cfg_.ifs_[i].port_id_;
    for (int j = 0; j < cfg_.ifs_[i].rx_.queues_.size(); j++) {
      int q_id = cfg_.ifs_[i].rx_.queues_[j].common_.id_;
      std::string ring_name = "RX_RING_P" + std::to_string(port_id) + "_Q" + std::to_string(q_id);

      struct rte_ring* ring = rte_ring_create(
          ring_name.c_str(), 2048, rte_socket_id(),
          RING_F_SC_DEQ | RING_F_SP_ENQ);

      if (ring == nullptr) {
        DAQIRI_LOG_CRITICAL(
          "Failed to allocate ring {}! err={}", ring_name, rte_strerror(rte_errno));
        return -1;
      }

      uint32_t key = generate_queue_key(port_id, q_id);
      rx_rings[key] = ring;
      DAQIRI_LOG_DEBUG("Created RX ring: {}", ring_name);
    }
  }

  auto num_rx_ptrs_buffers = (1UL << 13) - 1;
  DAQIRI_LOG_INFO("Setting up RX burst pool with {} batches of size {}",
                    num_rx_ptrs_buffers,
                    sizeof(void*) * max_rx_batch);
  rx_burst_buffer = rte_mempool_create("RX_BURST_POOL",
                                       num_rx_ptrs_buffers,
                                       sizeof(void*) * max_rx_batch,
                                       0,
                                       0,
                                       nullptr,
                                       nullptr,
                                       nullptr,
                                       nullptr,
                                       rte_socket_id(),
                                       0);
  if (rx_burst_buffer == nullptr) {
    DAQIRI_LOG_CRITICAL("Failed to allocate RX burst pool!");
    return -1;
  }

  DAQIRI_LOG_INFO("Setting up RX meta pool with {} buffers", cfg_.rx_meta_buffers_);
  rx_metadata = rte_mempool_create("RX_META_POOL",
                               cfg_.rx_meta_buffers_ - 1U,
                               sizeof(BurstParams),
                               0,
                               0,
                               nullptr,
                               nullptr,
                               nullptr,
                               nullptr,
                               rte_socket_id(),
                               0);
  if (rx_metadata == nullptr) {
    DAQIRI_LOG_CRITICAL("Failed to allocate RX meta pool!");
    return -1;
  }

  for (const auto& intf : cfg_.ifs_) {
    for (const auto& q : intf.tx_.queues_) {
      const auto append =
          "P" + std::to_string(intf.port_id_) + "_Q" + std::to_string(q.common_.id_);

      auto name = "TX_RING_" + append;
      DAQIRI_LOG_INFO("Setting up TX ring {}", name);
      uint32_t key = generate_queue_key(intf.port_id_, q.common_.id_);
      tx_rings[key] = rte_ring_create(
          name.c_str(), 2048, rte_socket_id(), RING_F_MC_RTS_DEQ | RING_F_MP_RTS_ENQ);
      if (tx_rings[key] == nullptr) {
        DAQIRI_LOG_CRITICAL("Failed to allocate ring!");
        return -1;
      }

      name = "TX_BURST_POOL_" + append;
      tx_burst_buffers[key] = rte_mempool_create(name.c_str(),
                                                 (1U << 7) - 1U,
                                                 sizeof(void*) * max_tx_batch,
                                                 0,
                                                 0,
                                                 nullptr,
                                                 nullptr,
                                                 nullptr,
                                                 nullptr,
                                                 rte_socket_id(),
                                                 0);
      if (tx_burst_buffers[key] == nullptr) {
        DAQIRI_LOG_CRITICAL("Failed to allocate TX message pool!");
        return -1;
      }

      DAQIRI_LOG_INFO("Setting up TX burst pool {} with {} pointers at {}",
                        name,
                        max_tx_batch,
                        (void*)tx_burst_buffers[key]);
    }
  }

  DAQIRI_LOG_INFO("Setting up TX meta pool with {} buffers", cfg_.tx_meta_buffers_);
  tx_metadata = rte_mempool_create("TX_META_POOL",
                               cfg_.tx_meta_buffers_ - 1U,
                               sizeof(BurstParams),
                               0,
                               0,
                               nullptr,
                               nullptr,
                               nullptr,
                               nullptr,
                               rte_socket_id(),
                               0);
  if (tx_metadata == nullptr) {
    DAQIRI_LOG_CRITICAL("Failed to allocate TX meta pool!");
    return -1;
  }

  if (loopback_ == LoopbackType::LOOPBACK_TYPE_SW) {
    loopback_ring =
        rte_ring_create("LOOPBACK_RING", 4096, rte_socket_id(),
            RING_F_MC_RTS_DEQ | RING_F_MP_RTS_ENQ);
    if (loopback_ring == nullptr) {
      DAQIRI_LOG_CRITICAL("Failed to allocate loopback ring!");
      return -1;
    }
  }

  return 0;
}

#define MAX_PATTERN_NUM 16
#define MAX_ACTION_NUM 12

struct FlowTemplateCreateStorage {
  struct rte_flow_item pattern[MAX_PATTERN_NUM];
  struct rte_flow_action action[MAX_ACTION_NUM];
  struct rte_flow_item_ipv4 ip_spec;
  struct rte_flow_item_udp udp_spec;
  struct rte_flow_action_mark mark;
  struct rte_flow_action_queue queue;
};

static constexpr uint32_t kDynamicFlowQueueId = 0;
static constexpr FlowId kMaxDynamicFlowMarkId = 0x00ffffffU;
static constexpr uint32_t kRxFlowGroup = 3;
static constexpr uint32_t kRxFlowPriority = 1;
static constexpr size_t kMaxIpv4UdpFlowTemplateFieldsForSingleTable = 7;

enum class Ipv4UdpFlowTemplateField {
  IPV4_LEN,
  IPV4_SRC,
  IPV4_DST,
  UDP_SRC,
  UDP_DST,
};

struct Ipv4UdpFlowTemplateKey {
  std::vector<Ipv4UdpFlowTemplateField> fields;
};

static bool operator==(const Ipv4UdpFlowTemplateKey& lhs,
                       const Ipv4UdpFlowTemplateKey& rhs) {
  return lhs.fields == rhs.fields;
}

static const std::vector<Ipv4UdpFlowTemplateField>& ipv4_udp_flow_template_fields() {
  static const std::vector<Ipv4UdpFlowTemplateField> fields = {
      Ipv4UdpFlowTemplateField::IPV4_LEN,
      Ipv4UdpFlowTemplateField::IPV4_SRC,
      Ipv4UdpFlowTemplateField::IPV4_DST,
      Ipv4UdpFlowTemplateField::UDP_SRC,
      Ipv4UdpFlowTemplateField::UDP_DST,
  };
  return fields;
}

static void append_ipv4_udp_flow_template_keys(size_t field_idx,
                                                std::vector<Ipv4UdpFlowTemplateField>& selected,
                                                std::vector<Ipv4UdpFlowTemplateKey>& keys) {
  const auto& fields = ipv4_udp_flow_template_fields();
  if (field_idx == fields.size()) {
    keys.push_back({selected});
    return;
  }

  append_ipv4_udp_flow_template_keys(field_idx + 1, selected, keys);
  selected.push_back(fields[field_idx]);
  append_ipv4_udp_flow_template_keys(field_idx + 1, selected, keys);
  selected.pop_back();
}

static const std::vector<Ipv4UdpFlowTemplateKey>& ipv4_udp_flow_template_keys() {
  static const std::vector<Ipv4UdpFlowTemplateKey> keys = [] {
    std::vector<Ipv4UdpFlowTemplateKey> generated_keys;
    if (ipv4_udp_flow_template_fields().size() >
        kMaxIpv4UdpFlowTemplateFieldsForSingleTable) {
      return generated_keys;
    }

    std::vector<Ipv4UdpFlowTemplateField> selected;
    append_ipv4_udp_flow_template_keys(0, selected, generated_keys);
    return generated_keys;
  }();
  return keys;
}

static Ipv4UdpFlowTemplateKey ipv4_udp_flow_template_key(const FlowMatch& match) {
  Ipv4UdpFlowTemplateKey key;
  if (match.ipv4_len_ > 0) {
    key.fields.push_back(Ipv4UdpFlowTemplateField::IPV4_LEN);
  }
  if (match.ipv4_src_ != INADDR_ANY) {
    key.fields.push_back(Ipv4UdpFlowTemplateField::IPV4_SRC);
  }
  if (match.ipv4_dst_ != INADDR_ANY) {
    key.fields.push_back(Ipv4UdpFlowTemplateField::IPV4_DST);
  }
  if (match.udp_src_ > 0) {
    key.fields.push_back(Ipv4UdpFlowTemplateField::UDP_SRC);
  }
  if (match.udp_dst_ > 0) {
    key.fields.push_back(Ipv4UdpFlowTemplateField::UDP_DST);
  }
  return key;
}

static bool ipv4_udp_flow_template_key_has_field(const Ipv4UdpFlowTemplateKey& key,
                                                  Ipv4UdpFlowTemplateField field) {
  return std::find(key.fields.begin(), key.fields.end(), field) != key.fields.end();
}

static void build_ipv4_udp_template_pattern(const Ipv4UdpFlowTemplateKey& key,
                                             struct rte_flow_item pattern[MAX_PATTERN_NUM],
                                             struct rte_flow_item_ipv4* ip_mask,
                                             struct rte_flow_item_udp* udp_mask) {
  memset(pattern, 0, sizeof(struct rte_flow_item) * MAX_PATTERN_NUM);
  memset(ip_mask, 0, sizeof(*ip_mask));
  memset(udp_mask, 0, sizeof(*udp_mask));

  pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
  pattern[1].type = RTE_FLOW_ITEM_TYPE_IPV4;
  pattern[2].type = RTE_FLOW_ITEM_TYPE_UDP;
  pattern[3].type = RTE_FLOW_ITEM_TYPE_END;

  if (ipv4_udp_flow_template_key_has_field(key, Ipv4UdpFlowTemplateField::IPV4_LEN)) {
    ip_mask->hdr.total_length = 0xffff;
  }
  if (ipv4_udp_flow_template_key_has_field(key, Ipv4UdpFlowTemplateField::IPV4_SRC)) {
    ip_mask->hdr.src_addr = 0xffffffff;
  }
  if (ipv4_udp_flow_template_key_has_field(key, Ipv4UdpFlowTemplateField::IPV4_DST)) {
    ip_mask->hdr.dst_addr = 0xffffffff;
  }
  if (ipv4_udp_flow_template_key_has_field(key, Ipv4UdpFlowTemplateField::IPV4_LEN) ||
      ipv4_udp_flow_template_key_has_field(key, Ipv4UdpFlowTemplateField::IPV4_SRC) ||
      ipv4_udp_flow_template_key_has_field(key, Ipv4UdpFlowTemplateField::IPV4_DST)) {
    pattern[1].mask = ip_mask;
  }

  if (ipv4_udp_flow_template_key_has_field(key, Ipv4UdpFlowTemplateField::UDP_SRC)) {
    udp_mask->hdr.src_port = 0xffff;
  }
  if (ipv4_udp_flow_template_key_has_field(key, Ipv4UdpFlowTemplateField::UDP_DST)) {
    udp_mask->hdr.dst_port = 0xffff;
  }
  if (ipv4_udp_flow_template_key_has_field(key, Ipv4UdpFlowTemplateField::UDP_SRC) ||
      ipv4_udp_flow_template_key_has_field(key, Ipv4UdpFlowTemplateField::UDP_DST)) {
    pattern[2].mask = udp_mask;
  }
}

void* DpdkEngine::flow_op_user_data(FlowOpId op_id) {
  return reinterpret_cast<void*>(static_cast<uintptr_t>(op_id));
}

FlowOpId DpdkEngine::flow_op_id_from_user_data(void* user_data) {
  return static_cast<FlowOpId>(reinterpret_cast<uintptr_t>(user_data));
}

FlowOpId DpdkEngine::allocate_flow_op_id() {
  if (next_flow_op_id_ == 0) { next_flow_op_id_ = 1; }
  return next_flow_op_id_++;
}

FlowId DpdkEngine::allocate_dynamic_flow_id() {
  while (!free_dynamic_flow_ids_.empty()) {
    const FlowId candidate = free_dynamic_flow_ids_.front();
    free_dynamic_flow_ids_.pop();
    if (candidate == 0 || candidate > kMaxDynamicFlowMarkId) { continue; }
    if (static_flow_ids_.find(candidate) != static_flow_ids_.end()) { continue; }
    if (dynamic_flows_.find(candidate) != dynamic_flows_.end()) { continue; }
    return candidate;
  }

  while (next_dynamic_flow_id_ != 0 &&
         next_dynamic_flow_id_ <= kMaxDynamicFlowMarkId) {
    const FlowId candidate = next_dynamic_flow_id_++;
    if (candidate == 0) { continue; }
    if (static_flow_ids_.find(candidate) != static_flow_ids_.end()) { continue; }
    if (dynamic_flows_.find(candidate) != dynamic_flows_.end()) { continue; }
    return candidate;
  }
  return 0;
}

void DpdkEngine::release_dynamic_flow_id(FlowId flow_id) {
  if (flow_id == 0 || flow_id > kMaxDynamicFlowMarkId) { return; }
  if (static_flow_ids_.find(flow_id) != static_flow_ids_.end()) { return; }
  if (dynamic_flows_.find(flow_id) != dynamic_flows_.end()) { return; }
  free_dynamic_flow_ids_.push(flow_id);
}

bool DpdkEngine::reserve_static_flow_ids() {
  static_flow_ids_.clear();
  while (!free_dynamic_flow_ids_.empty()) { free_dynamic_flow_ids_.pop(); }
  for (const auto& intf : cfg_.ifs_) {
    for (const auto& flow : intf.rx_.flows_) {
      if (flow.id_ == 0) { continue; }
      if (!static_flow_ids_.insert(flow.id_).second) {
        DAQIRI_LOG_ERROR("Duplicate static flow ID {}", flow.id_);
        return false;
      }
    }
  }
  next_dynamic_flow_id_ = 1;
  return true;
}

bool DpdkEngine::is_valid_rx_queue(int port, uint16_t queue_id) const {
  if (port < 0 || port >= static_cast<int>(cfg_.ifs_.size())) { return false; }
  const auto& queues = cfg_.ifs_[port].rx_.queues_;
  return std::any_of(queues.begin(), queues.end(), [queue_id](const RxQueueConfig& q) {
    return q.common_.id_ == queue_id;
  });
}

bool DpdkEngine::ipv4_udp_flow_template_index(const FlowMatch& match,
                                              uint8_t* template_index) const {
  if (template_index == nullptr) { return false; }
  *template_index = 0;

  const auto key = ipv4_udp_flow_template_key(match);
  if (key.fields.empty()) { return false; }

  const auto& keys = ipv4_udp_flow_template_keys();
  const auto key_it = std::find(keys.begin(), keys.end(), key);
  if (key_it == keys.end()) { return false; }

  const auto distance = std::distance(keys.begin(), key_it);
  if (distance < 0 || distance > std::numeric_limits<uint8_t>::max()) {
    return false;
  }
  *template_index = static_cast<uint8_t>(distance);
  return true;
}

bool DpdkEngine::is_ipv4_udp_flow_match(const FlowMatch& match) const {
  return match.type_ == FlowMatchType::IPV4_UDP &&
         !ipv4_udp_flow_template_key(match).fields.empty();
}

bool DpdkEngine::validate_dynamic_rx_flow(int port, const FlowRuleConfig& flow) const {
  if (!initialized_) {
    DAQIRI_LOG_ERROR("Cannot add dynamic RX flow before DAQIRI initialization");
    return false;
  }
  if (loopback_ == LoopbackType::LOOPBACK_TYPE_SW) {
    DAQIRI_LOG_ERROR("Dynamic RX flows are not supported in software loopback mode");
    return false;
  }
  if (port < 0 || port >= static_cast<int>(cfg_.ifs_.size())) {
    DAQIRI_LOG_ERROR("Invalid dynamic RX flow port {}", port);
    return false;
  }
  const auto actions = flow_rule_actions(flow);
  if (actions.empty() || actions.back().type_ != FlowType::QUEUE) {
    DAQIRI_LOG_ERROR("Dynamic RX flow must end with a queue action");
    return false;
  }
  if (actions.size() > 7) {
    DAQIRI_LOG_ERROR("Dynamic RX flow '{}' has too many actions", flow.name_);
    return false;
  }
  const FlowAction queue_action = flow_queue_action(actions);
  if (!is_valid_rx_queue(port, queue_action.id_)) {
    DAQIRI_LOG_ERROR("Dynamic RX flow targets invalid port/queue {}/{}", port, queue_action.id_);
    return false;
  }
  const bool has_transform = flow_actions_have_transform(actions);
  for (const auto& action : actions) {
    if (action.type_ == FlowType::VLAN_PUSH || action.type_ == FlowType::TUNNEL_ENCAP) {
      DAQIRI_LOG_ERROR("Dynamic RX flow '{}' can only use decap/pop transform actions",
                       flow.name_);
      return false;
    }
  }
  if (has_transform && flow.match_.type_ == FlowMatchType::FLEX_ITEM) {
    DAQIRI_LOG_ERROR("Dynamic RX flow '{}' cannot combine flex-item matching with tunnel/VLAN "
                     "actions",
                     flow.name_);
    return false;
  }
  if (is_ipv4_udp_flow_match(flow.match_)) { return true; }
  if (flow.match_.type_ == FlowMatchType::FLEX_ITEM) {
    if (find_flex_item_config(cfg_, port, flow.match_.flex_item_match_.flex_item_id_) == nullptr) {
      DAQIRI_LOG_ERROR("Dynamic RX flow references invalid flex item ID {}",
                       flow.match_.flex_item_match_.flex_item_id_);
      return false;
    }
    return true;
  }

  DAQIRI_LOG_ERROR("Dynamic RX flow must define an IPv4/UDP or flex-item match");
  return false;
}

void DpdkEngine::build_ipv4_udp_flow_pattern(const FlowMatch& match,
                                             struct rte_flow_item pattern[MAX_PATTERN_NUM],
                                             struct rte_flow_item_ipv4* ip_spec,
                                             struct rte_flow_item_udp* udp_spec) const {
  if (ip_spec == nullptr || udp_spec == nullptr) { return; }

  memset(pattern, 0, sizeof(struct rte_flow_item) * MAX_PATTERN_NUM);
  memset(ip_spec, 0, sizeof(*ip_spec));
  memset(udp_spec, 0, sizeof(*udp_spec));

  pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
  pattern[1].type = RTE_FLOW_ITEM_TYPE_IPV4;
  pattern[2].type = RTE_FLOW_ITEM_TYPE_UDP;
  pattern[3].type = RTE_FLOW_ITEM_TYPE_END;

  bool has_ip_match = false;
  if (match.ipv4_len_ > 0) {
    ip_spec->hdr.total_length = htons(match.ipv4_len_);
    has_ip_match = true;
  }
  if (match.ipv4_src_ != INADDR_ANY) {
    ip_spec->hdr.src_addr = match.ipv4_src_;
    has_ip_match = true;
  }
  if (match.ipv4_dst_ != INADDR_ANY) {
    ip_spec->hdr.dst_addr = match.ipv4_dst_;
    has_ip_match = true;
  }
  if (has_ip_match) { pattern[1].spec = ip_spec; }

  bool has_udp_match = false;
  if (match.udp_src_ > 0) {
    udp_spec->hdr.src_port = htons(match.udp_src_);
    has_udp_match = true;
  }
  if (match.udp_dst_ > 0) {
    udp_spec->hdr.dst_port = htons(match.udp_dst_);
    has_udp_match = true;
  }
  if (has_udp_match) { pattern[2].spec = udp_spec; }
}

void DpdkEngine::build_mark_queue_actions(FlowId flow_id,
                                          uint16_t queue_id,
                                          struct rte_flow_action action[MAX_ACTION_NUM],
                                          struct rte_flow_action_mark* mark,
                                          struct rte_flow_action_queue* queue) const {
  if (mark == nullptr || queue == nullptr) { return; }

  memset(action, 0, sizeof(struct rte_flow_action) * MAX_ACTION_NUM);
  memset(mark, 0, sizeof(*mark));
  memset(queue, 0, sizeof(*queue));

  mark->id = flow_id;
  queue->index = queue_id;
  action[0].type = RTE_FLOW_ACTION_TYPE_MARK;
  action[0].conf = mark;
  action[1].type = RTE_FLOW_ACTION_TYPE_QUEUE;
  action[1].conf = queue;
  action[2].type = RTE_FLOW_ACTION_TYPE_END;
}

Status DpdkEngine::enqueue_software_flow_completion(const FlowOpResult& result) {
  ready_flow_ops_.push(result);
  return Status::SUCCESS;
}

bool DpdkEngine::configure_flow_api_for_port(uint16_t port, uint32_t capacity) {
  if (port >= flow_template_states_.size()) { return false; }
  if (capacity == 0) { return false; }
  auto& state = flow_template_states_[port];
  if (state.configured) { return true; }

  struct rte_flow_error error;
  struct rte_flow_port_info port_info;
  struct rte_flow_queue_info queue_info;
  memset(&error, 0, sizeof(error));
  memset(&port_info, 0, sizeof(port_info));
  memset(&queue_info, 0, sizeof(queue_info));

  int ret = rte_flow_info_get(port, &port_info, &queue_info, &error);
  if (ret < 0 || port_info.max_nb_queues == 0) {
    DAQIRI_LOG_WARN("DPDK async flow API is not available on port {}: {}",
                    port,
                    error.message ? error.message : rte_strerror(rte_errno));
    return false;
  }

  state.capacity = capacity;
  struct rte_flow_port_attr port_attr;
  struct rte_flow_queue_attr queue_attr;
  memset(&port_attr, 0, sizeof(port_attr));
  memset(&queue_attr, 0, sizeof(queue_attr));

  if ((port_info.supported_flags & RTE_FLOW_PORT_FLAG_STRICT_QUEUE) != 0) {
    port_attr.flags |= RTE_FLOW_PORT_FLAG_STRICT_QUEUE;
  }
  queue_attr.size = std::max<uint32_t>(64, state.capacity * 2);
  if (queue_info.max_size > 0) { queue_attr.size = std::min(queue_attr.size, queue_info.max_size); }
  const struct rte_flow_queue_attr* queue_attrs[] = {&queue_attr};

  ret = rte_flow_configure(port, &port_attr, 1, queue_attrs, &error);
  if (ret < 0) {
    DAQIRI_LOG_WARN("Failed to configure DPDK async flow API on port {}: {}",
                    port,
                    error.message ? error.message : rte_strerror(rte_errno));
    return false;
  }

  state.configured = true;
  state.flow_queue_id = kDynamicFlowQueueId;
  DAQIRI_LOG_INFO("Configured DPDK async flow API on port {} with queue size {}",
                  port,
                  queue_attr.size);
  return true;
}

bool DpdkEngine::ensure_ipv4_udp_flow_template_table(uint16_t port) {
  if (port >= flow_template_states_.size()) { return false; }
  auto& state = flow_template_states_[port];
  if (state.templates_ready) { return true; }
  if (!state.configured) { return false; }

  struct rte_flow_error error;
  memset(&error, 0, sizeof(error));

  const auto& template_keys = ipv4_udp_flow_template_keys();
  if (template_keys.empty()) {
    DAQIRI_LOG_WARN("IPv4/UDP RX flow template field count {} cannot fit in one DPDK "
                    "template table; falling back to legacy flow create",
                    ipv4_udp_flow_template_fields().size());
    return false;
  }
  if (template_keys.size() > std::numeric_limits<uint8_t>::max()) {
    DAQIRI_LOG_WARN("DPDK supports at most {} pattern templates per table; {} IPv4/UDP RX "
                    "flow templates requested",
                    static_cast<unsigned int>(std::numeric_limits<uint8_t>::max()),
                    template_keys.size());
    return false;
  }

  state.ipv4_udp_pattern_templates.assign(template_keys.size(), nullptr);
  for (size_t pattern_idx = 0; pattern_idx < template_keys.size(); ++pattern_idx) {
    struct rte_flow_pattern_template_attr pattern_attr;
    struct rte_flow_item pattern[MAX_PATTERN_NUM];
    struct rte_flow_item_ipv4 ip_mask;
    struct rte_flow_item_udp udp_mask;
    memset(&pattern_attr, 0, sizeof(pattern_attr));
    pattern_attr.ingress = 1;
    pattern_attr.relaxed_matching = 1;
    build_ipv4_udp_template_pattern(template_keys[pattern_idx], pattern, &ip_mask, &udp_mask);

    state.ipv4_udp_pattern_templates[pattern_idx] =
        rte_flow_pattern_template_create(port, &pattern_attr, pattern, &error);
    if (state.ipv4_udp_pattern_templates[pattern_idx] == nullptr) {
      DAQIRI_LOG_WARN("Failed to create RX flow pattern template {} on port {}: {}",
                      pattern_idx,
                      port,
                      error.message ? error.message : rte_strerror(rte_errno));
      for (auto* tmpl : state.ipv4_udp_pattern_templates) {
        if (tmpl != nullptr) { rte_flow_pattern_template_destroy(port, tmpl, &error); }
      }
      state.ipv4_udp_pattern_templates.clear();
      return false;
    }
  }

  struct rte_flow_actions_template_attr actions_attr;
  struct rte_flow_action actions[MAX_ACTION_NUM];
  struct rte_flow_action masks[MAX_ACTION_NUM];
  struct rte_flow_action_mark mark;
  struct rte_flow_action_queue queue;
  struct rte_flow_action_mark mark_mask;
  struct rte_flow_action_queue queue_mask;
  memset(&actions_attr, 0, sizeof(actions_attr));
  memset(actions, 0, sizeof(actions));
  memset(masks, 0, sizeof(masks));
  memset(&mark, 0, sizeof(mark));
  memset(&queue, 0, sizeof(queue));
  memset(&mark_mask, 0, sizeof(mark_mask));
  memset(&queue_mask, 0, sizeof(queue_mask));
  actions_attr.ingress = 1;
  actions[0].type = RTE_FLOW_ACTION_TYPE_MARK;
  actions[0].conf = &mark;
  actions[1].type = RTE_FLOW_ACTION_TYPE_QUEUE;
  actions[1].conf = &queue;
  actions[2].type = RTE_FLOW_ACTION_TYPE_END;
  masks[0].type = RTE_FLOW_ACTION_TYPE_MARK;
  masks[0].conf = &mark_mask;
  masks[1].type = RTE_FLOW_ACTION_TYPE_QUEUE;
  masks[1].conf = &queue_mask;
  masks[2].type = RTE_FLOW_ACTION_TYPE_END;

  state.mark_queue_actions_template =
      rte_flow_actions_template_create(port, &actions_attr, actions, masks, &error);
  if (state.mark_queue_actions_template == nullptr) {
    DAQIRI_LOG_WARN("Failed to create RX flow actions template on port {}: {}",
                    port,
                    error.message ? error.message : rte_strerror(rte_errno));
    for (auto* tmpl : state.ipv4_udp_pattern_templates) {
      if (tmpl != nullptr) { rte_flow_pattern_template_destroy(port, tmpl, &error); }
    }
    state.ipv4_udp_pattern_templates.clear();
    return false;
  }

  struct rte_flow_template_table_attr table_attr;
  memset(&table_attr, 0, sizeof(table_attr));
  table_attr.flow_attr.ingress = 1;
  table_attr.flow_attr.group = kRxFlowGroup;
  table_attr.flow_attr.priority = kRxFlowPriority;
  table_attr.nb_flows = state.capacity;
  table_attr.insertion_type = RTE_FLOW_TABLE_INSERTION_TYPE_PATTERN;
  table_attr.hash_func = RTE_FLOW_TABLE_HASH_FUNC_DEFAULT;

  std::vector<struct rte_flow_actions_template*> action_templates = {
      state.mark_queue_actions_template};
  state.ipv4_udp_table =
      rte_flow_template_table_create(port,
                                     &table_attr,
                                     state.ipv4_udp_pattern_templates.data(),
                                     static_cast<uint8_t>(state.ipv4_udp_pattern_templates.size()),
                                     action_templates.data(),
                                     static_cast<uint8_t>(action_templates.size()),
                                     &error);
  if (state.ipv4_udp_table == nullptr) {
    DAQIRI_LOG_WARN("Failed to create RX flow template table on port {}: {}",
                    port,
                    error.message ? error.message : rte_strerror(rte_errno));
    rte_flow_actions_template_destroy(port, state.mark_queue_actions_template, &error);
    state.mark_queue_actions_template = nullptr;
    for (auto* tmpl : state.ipv4_udp_pattern_templates) {
      if (tmpl != nullptr) { rte_flow_pattern_template_destroy(port, tmpl, &error); }
    }
    state.ipv4_udp_pattern_templates.clear();
    return false;
  }

  state.templates_ready = true;
  DAQIRI_LOG_INFO("Created RX flow template table on port {} with capacity {}",
                  port,
                  state.capacity);
  return true;
}

Status DpdkEngine::create_dynamic_flow_legacy_locked(int port,
                                                     const FlowRuleConfig& flow,
                                                     FlowId flow_id) {
  FlowConfig cfg;
  cfg.name_ = flow.name_;
  cfg.id_ = flow_id;
  cfg.actions_ = flow_rule_actions(flow);
  cfg.action_ = flow_queue_action(cfg.actions_);
  cfg.match_ = flow.match_;
  cfg.backend_config_ = flow.backend_config_;

  struct rte_flow* rte_flow = nullptr;
  std::shared_ptr<DpdkFlowResource> resource;
  if (cfg.match_.type_ == FlowMatchType::FLEX_ITEM) {
    rte_flow = add_flex_item_flow(
        port, cfg.match_.flex_item_match_, cfg.action_.id_, cfg.id_, false);
  } else {
    rte_flow = add_flow(port, cfg, false, &resource);
  }

  if (rte_flow == nullptr) {
    return Status::INTERNAL_ERROR;
  }

  DynamicFlowEntry entry;
  entry.flow_id = flow_id;
  entry.port = static_cast<uint16_t>(port);
  entry.queue = flow.action_.id_;
  entry.flow = rte_flow;
  entry.backend = DynamicFlowBackend::LEGACY;
  entry.state = DynamicFlowState::ACTIVE;
  entry.backend_storage = resource;
  dynamic_flows_[flow_id] = entry;
  return Status::SUCCESS;
}

Status DpdkEngine::add_rx_flow_legacy_locked(int port,
                                             const FlowRuleConfig& flow,
                                             FlowId flow_id,
                                             FlowOpId op_id) {
  FlowOpResult result;
  result.op_id_ = op_id;
  result.type_ = FlowOpType::ADD_RX;
  result.flow_id_ = flow_id;
  result.flow_ids_ = {flow_id};
  result.status_ = create_dynamic_flow_legacy_locked(port, flow, flow_id);
  if (result.status_ != Status::SUCCESS) {
    result.flow_id_ = 0;
    result.flow_ids_[0] = 0;
    release_dynamic_flow_id(flow_id);
  }

  enqueue_software_flow_completion(result);
  return Status::SUCCESS;
}

Status DpdkEngine::add_rx_flows_legacy_locked(int port,
                                              const std::vector<FlowRuleConfig>& flows,
                                              const std::vector<FlowId>& flow_ids,
                                              FlowOpId op_id) {
  FlowOpResult result;
  result.op_id_ = op_id;
  result.type_ = FlowOpType::ADD_RX_BATCH;
  result.status_ = Status::SUCCESS;
  result.flow_ids_ = flow_ids;

  for (size_t i = 0; i < flows.size(); ++i) {
    const Status status = create_dynamic_flow_legacy_locked(port, flows[i], flow_ids[i]);
    if (status != Status::SUCCESS) {
      result.status_ = status;
      result.flow_ids_[i] = 0;
      release_dynamic_flow_id(flow_ids[i]);
    }
  }

  enqueue_software_flow_completion(result);
  return Status::SUCCESS;
}

Status DpdkEngine::enqueue_rx_flow_template_create_locked(int port,
                                                          const FlowRuleConfig& flow,
                                                          FlowId flow_id,
                                                          FlowOpId completion_id) {
  auto& state = flow_template_states_[port];
  struct rte_flow_op_attr op_attr;
  struct rte_flow_error error;
  memset(&op_attr, 0, sizeof(op_attr));
  memset(&error, 0, sizeof(error));

  auto storage = std::make_shared<FlowTemplateCreateStorage>();
  build_ipv4_udp_flow_pattern(flow.match_, storage->pattern, &storage->ip_spec, &storage->udp_spec);
  build_mark_queue_actions(flow_id,
                           flow.action_.id_,
                           storage->action,
                           &storage->mark,
                           &storage->queue);

  uint8_t template_index = 0;
  if (!ipv4_udp_flow_template_index(flow.match_, &template_index)) {
    return Status::INTERNAL_ERROR;
  }
  struct rte_flow* rte_flow =
      rte_flow_async_create(static_cast<uint16_t>(port),
                            state.flow_queue_id,
                            &op_attr,
                            state.ipv4_udp_table,
                            storage->pattern,
                            template_index,
                            storage->action,
                            0,
                            flow_op_user_data(completion_id),
                            &error);
  if (rte_flow == nullptr) {
    DAQIRI_LOG_WARN("Failed to enqueue async RX flow create on port {}: {}",
                    port,
                    error.message ? error.message : rte_strerror(rte_errno));
    return Status::INTERNAL_ERROR;
  }

  DynamicFlowEntry entry;
  entry.flow_id = flow_id;
  entry.port = static_cast<uint16_t>(port);
  entry.queue = flow.action_.id_;
  entry.flow = rte_flow;
  entry.backend = DynamicFlowBackend::TEMPLATE;
  entry.state = DynamicFlowState::ADDING;
  entry.flow_queue_id = state.flow_queue_id;
  entry.backend_storage = storage;
  dynamic_flows_[flow_id] = entry;
  return Status::SUCCESS;
}

void DpdkEngine::discard_unpushed_template_creates_locked(const std::vector<FlowId>& flow_ids) {
  struct rte_flow_error error;
  struct rte_flow_op_attr op_attr;
  std::array<uint32_t, RTE_MAX_ETHPORTS> pending_destroys{};

  for (const FlowId flow_id : flow_ids) {
    auto flow_it = dynamic_flows_.find(flow_id);
    if (flow_it != dynamic_flows_.end() &&
        flow_it->second.backend == DynamicFlowBackend::TEMPLATE &&
        flow_it->second.state == DynamicFlowState::ADDING) {
      DynamicFlowEntry& entry = flow_it->second;
      if (entry.flow != nullptr && entry.port < flow_template_states_.size() &&
          flow_template_states_[entry.port].configured) {
        memset(&error, 0, sizeof(error));
        memset(&op_attr, 0, sizeof(op_attr));
        if (rte_flow_async_destroy(entry.port,
                                   entry.flow_queue_id,
                                   &op_attr,
                                   entry.flow,
                                   nullptr,
                                   &error) < 0) {
          DAQIRI_LOG_WARN("Failed to enqueue cleanup destroy for unpushed dynamic RX flow {} "
                          "on port {}: {}",
                          flow_id,
                          entry.port,
                          error.message ? error.message : rte_strerror(rte_errno));
        } else {
          ++pending_destroys[entry.port];
        }
      }
      dynamic_flows_.erase(flow_it);
    }
    release_dynamic_flow_id(flow_id);
  }

  for (uint16_t port = 0; port < pending_destroys.size(); ++port) {
    if (pending_destroys[port] == 0) { continue; }
    auto& state = flow_template_states_[port];
    memset(&error, 0, sizeof(error));
    if (rte_flow_push(port, state.flow_queue_id, &error) < 0) {
      DAQIRI_LOG_WARN("Failed to push cleanup destroys for unpushed dynamic RX flows "
                      "on port {}: {}",
                      port,
                      error.message ? error.message : rte_strerror(rte_errno));
      continue;
    }

    uint32_t remaining = pending_destroys[port];
    unsigned idle_polls = 0;
    struct rte_flow_op_result results[16];
    while (remaining > 0 && idle_polls < 100) {
      memset(&error, 0, sizeof(error));
      memset(results, 0, sizeof(results));
      const int ret = rte_flow_pull(port, state.flow_queue_id, results, 16, &error);
      if (ret < 0) {
        DAQIRI_LOG_WARN("Failed to pull cleanup destroy completions on port {}: {}",
                        port,
                        error.message ? error.message : rte_strerror(rte_errno));
        break;
      }
      if (ret == 0) {
        ++idle_polls;
        rte_delay_us_sleep(1000);
        continue;
      }

      idle_polls = 0;
      for (int i = 0; i < ret && remaining > 0; ++i) {
        if (results[i].user_data != nullptr) { continue; }
        if (results[i].status != RTE_FLOW_OP_SUCCESS) {
          DAQIRI_LOG_WARN("Cleanup destroy failed on port {}", port);
        }
        --remaining;
      }
    }

    if (remaining > 0) {
      DAQIRI_LOG_WARN("{} cleanup destroy completions were not received on port {}",
                      remaining,
                      port);
    }
  }
}

Status DpdkEngine::add_rx_flow_template_locked(int port,
                                               const FlowRuleConfig& flow,
                                               FlowId flow_id,
                                               FlowOpId op_id) {
  if (!ensure_eth_jump_rule(port, kRxFlowGroup) ||
      !ensure_ipv4_udp_flow_template_table(static_cast<uint16_t>(port))) {
    return add_rx_flow_legacy_locked(port, flow, flow_id, op_id);
  }

  PendingFlowBatch pending;
  pending.result.op_id_ = op_id;
  pending.result.type_ = FlowOpType::ADD_RX;
  pending.result.status_ = Status::NOT_READY;
  pending.result.flow_id_ = flow_id;
  pending.result.flow_ids_ = {flow_id};
  pending.remaining = 1;

  if (enqueue_rx_flow_template_create_locked(port, flow, flow_id, op_id) != Status::SUCCESS) {
    return add_rx_flow_legacy_locked(port, flow, flow_id, op_id);
  }

  auto& state = flow_template_states_[port];
  struct rte_flow_error error;
  memset(&error, 0, sizeof(error));
  pending_flow_batches_[op_id] = pending;
  pending_flow_completions_[op_id] = {op_id, flow_id, 0};

  if (rte_flow_push(static_cast<uint16_t>(port), state.flow_queue_id, &error) < 0) {
    DAQIRI_LOG_WARN("Failed to push async RX flow create on port {}: {}",
                    port,
                    error.message ? error.message : rte_strerror(rte_errno));

    pending_flow_completions_.erase(op_id);
    pending_flow_batches_.erase(op_id);
    discard_unpushed_template_creates_locked({flow_id});
    FlowOpResult result;
    result.op_id_ = op_id;
    result.type_ = FlowOpType::ADD_RX;
    result.status_ = Status::INTERNAL_ERROR;
    result.flow_id_ = 0;
    result.flow_ids_ = {0};
    enqueue_software_flow_completion(result);
    return Status::SUCCESS;
  }

  return Status::SUCCESS;
}

Status DpdkEngine::add_rx_flows_template_locked(int port,
                                                const std::vector<FlowRuleConfig>& flows,
                                                const std::vector<FlowId>& flow_ids,
                                                FlowOpId op_id) {
  if (!ensure_eth_jump_rule(port, kRxFlowGroup) ||
      !ensure_ipv4_udp_flow_template_table(static_cast<uint16_t>(port))) {
    return add_rx_flows_legacy_locked(port, flows, flow_ids, op_id);
  }

  for (const auto& flow : flows) {
    uint8_t template_index = 0;
    if (!ipv4_udp_flow_template_index(flow.match_, &template_index)) {
      return add_rx_flows_legacy_locked(port, flows, flow_ids, op_id);
    }
  }

  PendingFlowBatch pending;
  pending.result.op_id_ = op_id;
  pending.result.type_ = FlowOpType::ADD_RX_BATCH;
  pending.result.status_ = Status::NOT_READY;
  pending.result.flow_ids_ = flow_ids;

  size_t enqueued = 0;
  std::vector<FlowId> enqueued_flow_ids;
  enqueued_flow_ids.reserve(flows.size());
  for (size_t i = 0; i < flows.size(); ++i) {
    const FlowOpId completion_id = allocate_flow_op_id();
    const Status status =
        enqueue_rx_flow_template_create_locked(port, flows[i], flow_ids[i], completion_id);
    if (status != Status::SUCCESS) {
      pending.result.status_ = status;
      pending.result.flow_ids_[i] = 0;
      release_dynamic_flow_id(flow_ids[i]);
      for (size_t j = i + 1; j < pending.result.flow_ids_.size(); ++j) {
        pending.result.flow_ids_[j] = 0;
        release_dynamic_flow_id(flow_ids[j]);
      }
      break;
    }

    pending_flow_completions_[completion_id] = {op_id, flow_ids[i], i};
    enqueued_flow_ids.push_back(flow_ids[i]);
    ++enqueued;
  }

  if (enqueued == 0) {
    if (pending.result.status_ == Status::NOT_READY) {
      pending.result.status_ = Status::INTERNAL_ERROR;
      std::fill(pending.result.flow_ids_.begin(), pending.result.flow_ids_.end(), 0);
      for (const FlowId flow_id : flow_ids) {
        release_dynamic_flow_id(flow_id);
      }
    }
    enqueue_software_flow_completion(pending.result);
    return Status::SUCCESS;
  }

  pending.remaining = enqueued;
  pending_flow_batches_[op_id] = pending;

  auto& state = flow_template_states_[port];
  struct rte_flow_error error;
  memset(&error, 0, sizeof(error));
  if (rte_flow_push(static_cast<uint16_t>(port), state.flow_queue_id, &error) < 0) {
    DAQIRI_LOG_WARN("Failed to push async RX flow batch create on port {}: {}",
                    port,
                    error.message ? error.message : rte_strerror(rte_errno));

    for (auto completion_it = pending_flow_completions_.begin();
         completion_it != pending_flow_completions_.end();) {
      if (completion_it->second.op_id == op_id) {
        completion_it = pending_flow_completions_.erase(completion_it);
      } else {
        ++completion_it;
      }
    }
    pending_flow_batches_.erase(op_id);
    discard_unpushed_template_creates_locked(enqueued_flow_ids);

    pending.result.status_ = Status::INTERNAL_ERROR;
    std::fill(pending.result.flow_ids_.begin(), pending.result.flow_ids_.end(), 0);
    enqueue_software_flow_completion(pending.result);
  }

  return Status::SUCCESS;
}

Status DpdkEngine::delete_flow_legacy_locked(DynamicFlowEntry& entry, FlowOpId op_id) {
  struct rte_flow_error error;
  memset(&error, 0, sizeof(error));

  FlowOpResult result;
  result.op_id_ = op_id;
  result.type_ = FlowOpType::DELETE;
  result.flow_id_ = entry.flow_id;
  result.status_ = Status::SUCCESS;

  const FlowId flow_id = entry.flow_id;
  if (rte_flow_destroy(entry.port, entry.flow, &error) != 0) {
    DAQIRI_LOG_ERROR("Failed to destroy dynamic RX flow {} on port {}: {}",
                     entry.flow_id,
                     entry.port,
                     error.message ? error.message : rte_strerror(rte_errno));
    result.status_ = Status::INTERNAL_ERROR;
  } else {
    dynamic_flows_.erase(flow_id);
    release_dynamic_flow_id(flow_id);
  }

  enqueue_software_flow_completion(result);
  return Status::SUCCESS;
}

Status DpdkEngine::delete_flow_template_locked(DynamicFlowEntry& entry, FlowOpId op_id) {
  struct rte_flow_op_attr op_attr;
  struct rte_flow_error error;
  memset(&op_attr, 0, sizeof(op_attr));
  memset(&error, 0, sizeof(error));

  if (rte_flow_async_destroy(entry.port,
                             entry.flow_queue_id,
                             &op_attr,
                             entry.flow,
                             flow_op_user_data(op_id),
                             &error) < 0) {
    DAQIRI_LOG_ERROR("Failed to enqueue async RX flow destroy for {} on port {}: {}",
                     entry.flow_id,
                     entry.port,
                     error.message ? error.message : rte_strerror(rte_errno));
    return Status::INTERNAL_ERROR;
  }
  if (rte_flow_push(entry.port, entry.flow_queue_id, &error) < 0) {
    DAQIRI_LOG_ERROR("Failed to push async RX flow destroy for {} on port {}: {}",
                     entry.flow_id,
                     entry.port,
                     error.message ? error.message : rte_strerror(rte_errno));
    return Status::INTERNAL_ERROR;
  }

  entry.state = DynamicFlowState::DELETING;
  PendingFlowBatch pending;
  pending.result.op_id_ = op_id;
  pending.result.type_ = FlowOpType::DELETE;
  pending.result.status_ = Status::NOT_READY;
  pending.result.flow_id_ = entry.flow_id;
  pending.remaining = 1;
  pending_flow_batches_[op_id] = pending;
  pending_flow_completions_[op_id] = {op_id, entry.flow_id, 0};
  return Status::SUCCESS;
}

void DpdkEngine::poll_dpdk_flow_completions_locked() {
  struct rte_flow_error error;
  struct rte_flow_op_result results[16];

  for (uint16_t port = 0; port < flow_template_states_.size(); ++port) {
    auto& state = flow_template_states_[port];
    if (!state.configured) { continue; }

    while (true) {
      memset(&error, 0, sizeof(error));
      memset(results, 0, sizeof(results));
      const int ret = rte_flow_pull(port, state.flow_queue_id, results, 16, &error);
      if (ret <= 0) { break; }

      for (int i = 0; i < ret; ++i) {
        const FlowOpId completion_id = flow_op_id_from_user_data(results[i].user_data);
        const auto completion_it = pending_flow_completions_.find(completion_id);
        if (completion_it == pending_flow_completions_.end()) { continue; }

        const PendingFlowCompletion completion = completion_it->second;
        pending_flow_completions_.erase(completion_it);

        const auto batch_it = pending_flow_batches_.find(completion.op_id);
        if (batch_it == pending_flow_batches_.end()) { continue; }
        PendingFlowBatch& batch = batch_it->second;
        const Status completion_status = results[i].status == RTE_FLOW_OP_SUCCESS
                                             ? Status::SUCCESS
                                             : Status::INTERNAL_ERROR;
        const bool is_add_op = batch.result.type_ == FlowOpType::ADD_RX ||
                               batch.result.type_ == FlowOpType::ADD_RX_BATCH;
        if (completion_status != Status::SUCCESS) {
          batch.result.status_ = completion_status;
          if (is_add_op && completion.flow_index < batch.result.flow_ids_.size()) {
            batch.result.flow_ids_[completion.flow_index] = 0;
          }
          if (is_add_op && batch.result.flow_id_ == completion.flow_id) {
            batch.result.flow_id_ = 0;
          }
        }

        const auto flow_it = dynamic_flows_.find(completion.flow_id);
        if (flow_it != dynamic_flows_.end()) {
          if (is_add_op) {
            if (completion_status == Status::SUCCESS) {
              flow_it->second.state = DynamicFlowState::ACTIVE;
              flow_it->second.backend_storage.reset();
            } else {
              const FlowId failed_flow_id = flow_it->first;
              dynamic_flows_.erase(flow_it);
              release_dynamic_flow_id(failed_flow_id);
            }
          } else {
            if (completion_status == Status::SUCCESS) {
              const FlowId deleted_flow_id = flow_it->first;
              dynamic_flows_.erase(flow_it);
              release_dynamic_flow_id(deleted_flow_id);
            } else {
              flow_it->second.state = DynamicFlowState::ACTIVE;
            }
          }
        }

        if (batch.remaining > 0) { --batch.remaining; }
        if (batch.remaining == 0) {
          if (batch.result.status_ == Status::NOT_READY) {
            batch.result.status_ = Status::SUCCESS;
          }
          ready_flow_ops_.push(batch.result);
          pending_flow_batches_.erase(batch_it);
        }
      }
    }
  }
}

void DpdkEngine::cleanup_template_dynamic_flows_locked() {
  struct rte_flow_error error;
  struct rte_flow_op_attr op_attr;
  std::array<uint32_t, RTE_MAX_ETHPORTS> pending_destroys{};

  for (auto& [flow_id, entry] : dynamic_flows_) {
    if (entry.backend != DynamicFlowBackend::TEMPLATE || entry.flow == nullptr) {
      continue;
    }
    if (entry.port >= flow_template_states_.size() ||
        !flow_template_states_[entry.port].configured) {
      DAQIRI_LOG_WARN("Skipping async destroy for dynamic RX flow {} on unconfigured port {}",
                      flow_id,
                      entry.port);
      continue;
    }

    memset(&error, 0, sizeof(error));
    memset(&op_attr, 0, sizeof(op_attr));
    if (rte_flow_async_destroy(entry.port,
                               entry.flow_queue_id,
                               &op_attr,
                               entry.flow,
                               nullptr,
                               &error) < 0) {
      DAQIRI_LOG_WARN("Failed to enqueue shutdown async destroy for dynamic RX flow {} "
                      "on port {}: {}",
                      flow_id,
                      entry.port,
                      error.message ? error.message : rte_strerror(rte_errno));
      continue;
    }
    ++pending_destroys[entry.port];
  }

  for (uint16_t port = 0; port < pending_destroys.size(); ++port) {
    if (pending_destroys[port] == 0) { continue; }
    auto& state = flow_template_states_[port];
    memset(&error, 0, sizeof(error));
    if (rte_flow_push(port, state.flow_queue_id, &error) < 0) {
      DAQIRI_LOG_WARN("Failed to push shutdown async destroys on port {}: {}",
                      port,
                      error.message ? error.message : rte_strerror(rte_errno));
      continue;
    }

    uint32_t remaining = pending_destroys[port];
    unsigned idle_polls = 0;
    struct rte_flow_op_result results[16];
    while (remaining > 0 && idle_polls < 1000) {
      memset(&error, 0, sizeof(error));
      memset(results, 0, sizeof(results));
      const int ret = rte_flow_pull(port, state.flow_queue_id, results, 16, &error);
      if (ret < 0) {
        DAQIRI_LOG_WARN("Failed to pull shutdown async destroy completions on port {}: {}",
                        port,
                        error.message ? error.message : rte_strerror(rte_errno));
        break;
      }
      if (ret == 0) {
        ++idle_polls;
        rte_delay_us_sleep(1000);
        continue;
      }

      idle_polls = 0;
      for (int i = 0; i < ret && remaining > 0; ++i) {
        if (results[i].user_data != nullptr) { continue; }
        if (results[i].status != RTE_FLOW_OP_SUCCESS) {
          DAQIRI_LOG_WARN("Shutdown async destroy failed on port {}", port);
        }
        --remaining;
      }
    }

    if (remaining > 0) {
      DAQIRI_LOG_WARN("{} shutdown async dynamic RX flow destroys did not complete on port {}",
                      remaining,
                      port);
    }
  }
}

void DpdkEngine::cleanup_dynamic_flows() {
  std::lock_guard<std::mutex> guard(flow_lock_);
  struct rte_flow_error error;
  memset(&error, 0, sizeof(error));

  poll_dpdk_flow_completions_locked();
  pending_flow_batches_.clear();
  pending_flow_completions_.clear();
  while (!ready_flow_ops_.empty()) { ready_flow_ops_.pop(); }

  cleanup_template_dynamic_flows_locked();

  for (auto& [flow_id, entry] : dynamic_flows_) {
    if (entry.backend == DynamicFlowBackend::LEGACY && entry.flow != nullptr) {
      rte_flow_destroy(entry.port, entry.flow, &error);
    }
  }
  dynamic_flows_.clear();

  for (uint16_t port = 0; port < flow_template_states_.size(); ++port) {
    auto& state = flow_template_states_[port];
    if (state.ipv4_udp_table != nullptr) {
      rte_flow_template_table_destroy(port, state.ipv4_udp_table, &error);
      state.ipv4_udp_table = nullptr;
    }
    if (state.mark_queue_actions_template != nullptr) {
      rte_flow_actions_template_destroy(port, state.mark_queue_actions_template, &error);
      state.mark_queue_actions_template = nullptr;
    }
    for (auto* tmpl : state.ipv4_udp_pattern_templates) {
      if (tmpl != nullptr) { rte_flow_pattern_template_destroy(port, tmpl, &error); }
    }
    state.ipv4_udp_pattern_templates.clear();
    state.templates_ready = false;
    state.configured = false;
  }
}

Status DpdkEngine::add_rx_flow_async(int port, const FlowRuleConfig& flow, FlowOpId* op_id) {
  if (op_id == nullptr) { return Status::NULL_PTR; }
  *op_id = 0;
  std::lock_guard<std::mutex> guard(flow_lock_);
  if (!validate_dynamic_rx_flow(port, flow)) { return Status::INVALID_PARAMETER; }

  const FlowId flow_id = allocate_dynamic_flow_id();
  if (flow_id == 0) { return Status::NO_SPACE_AVAILABLE; }
  const FlowOpId new_op_id = allocate_flow_op_id();
  *op_id = new_op_id;

  if (is_ipv4_udp_flow_match(flow.match_) && !flow_rule_has_transform_actions(flow)) {
    return add_rx_flow_template_locked(port, flow, flow_id, new_op_id);
  }
  return add_rx_flow_legacy_locked(port, flow, flow_id, new_op_id);
}

Status DpdkEngine::add_rx_flows_async(int port,
                                      const std::vector<FlowRuleConfig>& flows,
                                      FlowOpId* op_id) {
  if (op_id == nullptr) { return Status::NULL_PTR; }
  *op_id = 0;
  if (flows.empty()) { return Status::INVALID_PARAMETER; }

  std::lock_guard<std::mutex> guard(flow_lock_);
  for (const auto& flow : flows) {
    if (!validate_dynamic_rx_flow(port, flow)) { return Status::INVALID_PARAMETER; }
  }

  std::vector<FlowId> flow_ids;
  flow_ids.reserve(flows.size());
  for (size_t i = 0; i < flows.size(); ++i) {
    const FlowId flow_id = allocate_dynamic_flow_id();
    if (flow_id == 0) {
      for (const FlowId allocated_flow_id : flow_ids) {
        release_dynamic_flow_id(allocated_flow_id);
      }
      return Status::NO_SPACE_AVAILABLE;
    }
    flow_ids.push_back(flow_id);
  }

  const FlowOpId new_op_id = allocate_flow_op_id();
  *op_id = new_op_id;

  const bool all_ipv4_udp =
      std::all_of(flows.begin(), flows.end(), [this](const FlowRuleConfig& flow) {
        return is_ipv4_udp_flow_match(flow.match_) && !flow_rule_has_transform_actions(flow);
      });
  if (all_ipv4_udp) {
    return add_rx_flows_template_locked(port, flows, flow_ids, new_op_id);
  }
  return add_rx_flows_legacy_locked(port, flows, flow_ids, new_op_id);
}

Status DpdkEngine::delete_flow_async(FlowId flow_id, FlowOpId* op_id) {
  if (op_id == nullptr) { return Status::NULL_PTR; }
  *op_id = 0;
  std::lock_guard<std::mutex> guard(flow_lock_);
  if (!initialized_) { return Status::NOT_READY; }
  if (static_flow_ids_.find(flow_id) != static_flow_ids_.end()) { return Status::INVALID_PARAMETER; }
  const auto flow_it = dynamic_flows_.find(flow_id);
  if (flow_it == dynamic_flows_.end() || flow_it->second.state != DynamicFlowState::ACTIVE) {
    return Status::INVALID_PARAMETER;
  }

  const FlowOpId new_op_id = allocate_flow_op_id();
  if (flow_it->second.backend == DynamicFlowBackend::TEMPLATE) {
    const Status status = delete_flow_template_locked(flow_it->second, new_op_id);
    if (status == Status::SUCCESS) { *op_id = new_op_id; }
    return status;
  }
  const Status status = delete_flow_legacy_locked(flow_it->second, new_op_id);
  if (status == Status::SUCCESS) { *op_id = new_op_id; }
  return status;
}

Status DpdkEngine::poll_flow_op(FlowOpResult* result) {
  if (result == nullptr) { return Status::NULL_PTR; }
  std::lock_guard<std::mutex> guard(flow_lock_);
  poll_dpdk_flow_completions_locked();
  if (ready_flow_ops_.empty()) { return Status::NOT_READY; }
  *result = ready_flow_ops_.front();
  ready_flow_ops_.pop();
  return Status::SUCCESS;
}

static uint64_t eth_jump_key(int port, uint32_t group) {
  return (static_cast<uint64_t>(static_cast<uint16_t>(port)) << 32) | group;
}

bool DpdkEngine::ensure_eth_jump_rule(int port, uint32_t group) {
  const uint64_t key = eth_jump_key(port, group);
  if (eth_jump_installed_.count(key) != 0) { return true; }

  struct rte_flow_error jump_error;
  struct rte_flow_attr jump_attr{.group = 0, .ingress = 1};
  struct rte_flow_action_jump jump_v = {.group = group};
  struct rte_flow_action jump_actions[] = {
      {.type = RTE_FLOW_ACTION_TYPE_JUMP, .conf = &jump_v},
      {.type = RTE_FLOW_ACTION_TYPE_END},
  };
  struct rte_flow_item jump_pattern[] = {
      {.type = RTE_FLOW_ITEM_TYPE_ETH, .spec = 0, .mask = 0},
      {.type = RTE_FLOW_ITEM_TYPE_END},
  };

  int res = rte_flow_validate(port, &jump_attr, jump_pattern, jump_actions, &jump_error);
  if (res != 0) {
    DAQIRI_LOG_CRITICAL("Failed to validate ETH jump rule on port {} to group {}: {} ({})",
                        port,
                        group,
                        jump_error.message ? jump_error.message : "unknown error",
                        rte_strerror(rte_errno));
    return false;
  }

  struct rte_flow* jump_flow =
      rte_flow_create(port, &jump_attr, jump_pattern, jump_actions, &jump_error);
  if (jump_flow == nullptr) {
    DAQIRI_LOG_CRITICAL("Failed to create ETH jump rule on port {} to group {}: {} ({})",
                        port,
                        group,
                        jump_error.message ? jump_error.message : "unknown error",
                        rte_strerror(rte_errno));
    return false;
  }

  eth_jump_installed_.insert(key);
  eth_jump_flows_[key] = jump_flow;
  return true;
}

void DpdkEngine::track_flow(uint16_t port, struct rte_flow* flow) {
  if (flow != nullptr) {
    programmed_flows_.push_back({port, flow});
  }
}

void DpdkEngine::destroy_programmed_flows() {
  for (const auto& [port, flow] : programmed_flows_) {
    if (flow == nullptr) { continue; }
    struct rte_flow_error error;
    const int ret = rte_flow_destroy(port, flow, &error);
    if (ret != 0) {
      DAQIRI_LOG_ERROR("Failed to destroy flow on port {}: {}",
                       port,
                       error.message ? error.message : "unknown error");
    }
  }
  programmed_flows_.clear();

  for (uint16_t port = 0; port < drop_all_traffic_flow.size(); ++port) {
    struct rte_flow* drop_flow = drop_all_traffic_flow[port].drop;
    if (drop_flow == nullptr) { continue; }
    struct rte_flow_error error;
    const int ret = rte_flow_destroy(port, drop_flow, &error);
    if (ret != 0) {
      DAQIRI_LOG_ERROR("Failed to destroy drop-all-traffic rule on port {}: {}",
                       port,
                       error.message ? error.message : "unknown error");
    }
    drop_all_traffic_flow[port].drop = nullptr;
  }
}

void DpdkEngine::destroy_flex_item_handles() {
  for (const auto& [flex_item_id, flex_handle] : flex_item_handles_) {
    if (flex_handle.handle == nullptr) { continue; }
    struct rte_flow_error error;
    const int ret =
        rte_flow_flex_item_release(flex_handle.port, flex_handle.handle, &error);
    if (ret != 0) {
      DAQIRI_LOG_ERROR("Failed to destroy flex item {} on port {}: {}",
                       flex_item_id,
                       flex_handle.port,
                       error.message ? error.message : "unknown error");
    }
  }
  flex_item_handles_.clear();
}

void DpdkEngine::destroy_all_flow_rules() {
  destroy_owned_flows();
  destroy_programmed_flows();
  destroy_flex_item_handles();
  destroy_eth_jump_rules();
}

void DpdkEngine::destroy_eth_jump_rules() {
  for (const auto& [key, flow] : eth_jump_flows_) {
    if (flow == nullptr) { continue; }
    const int port = static_cast<int>(key >> 32);
    struct rte_flow_error error;
    const int ret = rte_flow_destroy(port, flow, &error);
    if (ret != 0) {
      DAQIRI_LOG_ERROR("Failed to destroy ETH jump rule on port {}: {}",
                       port,
                       error.message ? error.message : "unknown error");
    }
  }
  eth_jump_flows_.clear();
  eth_jump_installed_.clear();
}

void DpdkEngine::destroy_owned_flows() {
  for (auto& resource : owned_flows_) {
    if (resource == nullptr || resource->flow == nullptr || resource->port < 0) { continue; }
    struct rte_flow_error error;
    const int ret = rte_flow_destroy(resource->port, resource->flow, &error);
    if (ret != 0) {
      DAQIRI_LOG_ERROR("Failed to destroy DPDK flow on port {}: {}",
                       resource->port,
                       error.message ? error.message : "unknown error");
    }
    resource->flow = nullptr;
  }
  owned_flows_.clear();
}

struct rte_flow* DpdkEngine::add_flex_item_flow(
    int port,
    const FlexItemMatch& match_info,
    uint16_t queue_id,
    FlowId mark_id,
    bool track) {
  /* Declaring structs being used. 8< */
  struct rte_flow_attr attr;
  struct rte_flow_item pattern[MAX_PATTERN_NUM];
  struct rte_flow_action action[MAX_ACTION_NUM];
  struct rte_flow* flow = NULL;
  struct rte_flow_action_queue queue = {.index = queue_id};
  struct rte_flow_action_mark mark = {.id = mark_id};
  struct rte_flow_error error;
  struct rte_flow_item_udp udp_spec;
  struct rte_flow_item_udp udp_mask;
  struct rte_flow_item_ipv4  ip_spec;
  struct rte_flow_item_ipv4  ip_mask;
  int res;
  const FlexItemConfig* flex_item_config =
      find_flex_item_config(cfg_, port, match_info.flex_item_id_);
  if (flex_item_config == nullptr) {
    DAQIRI_LOG_CRITICAL("Flow references unknown flex item id {} on port {}", match_info.flex_item_id_,
                        port);
    return nullptr;
  }

  memset(pattern, 0, sizeof(pattern));
  memset(action, 0, sizeof(action));
  memset(&attr, 0, sizeof(struct rte_flow_attr));
  memset(&ip_spec, 0, sizeof(struct rte_flow_item_ipv4));
  memset(&ip_mask, 0, sizeof(struct rte_flow_item_ipv4));
  memset(&udp_spec, 0, sizeof(struct rte_flow_item_udp));
  memset(&udp_mask, 0, sizeof(struct rte_flow_item_udp));

  if (mark_id != 0) {
    action[0].type = RTE_FLOW_ACTION_TYPE_MARK;
    action[0].conf = &mark;
    action[1].type = RTE_FLOW_ACTION_TYPE_QUEUE;
    action[1].conf = &queue;
    action[2].type = RTE_FLOW_ACTION_TYPE_END;
  } else {
    action[0].type = RTE_FLOW_ACTION_TYPE_QUEUE;
    action[0].conf = &queue;
    action[1].type = RTE_FLOW_ACTION_TYPE_END;
  }

  pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
  pattern[1].type = RTE_FLOW_ITEM_TYPE_IPV4;
  //  pattern[3].type = RTE_FLOW_ITEM_TYPE_FLEX; // defined later
  pattern[4].type = RTE_FLOW_ITEM_TYPE_END;

  struct rte_flow_item udp_item;
  udp_spec.hdr.src_port = 0;
  udp_spec.hdr.dst_port = htons(flex_item_config->udp_dst_port_);
  udp_spec.hdr.dgram_len = 0;
  udp_spec.hdr.dgram_cksum = 0;

  udp_mask.hdr.src_port = 0;
  udp_mask.hdr.dst_port = 0xffff;
  udp_mask.hdr.dgram_len = 0;
  udp_mask.hdr.dgram_cksum = 0;

  udp_item.type = RTE_FLOW_ITEM_TYPE_UDP;
  udp_item.spec = &udp_spec;
  udp_item.mask = &udp_mask;
  udp_item.last = NULL;

  pattern[2] = udp_item;

  const uint32_t handle_key =
      flex_item_handle_key(static_cast<uint16_t>(port), match_info.flex_item_id_);
  auto handle_it = flex_item_handles_.find(handle_key);
  if (handle_it == flex_item_handles_.end()) {
    if (!ensure_eth_jump_rule(port, 1)) { return nullptr; }

    struct rte_flow_item_flex_conf flex_conf;
    flex_conf.tunnel = FLEX_TUNNEL_MODE_SINGLE;
    memset(&flex_conf.next_header, 0, sizeof(flex_conf.next_header));
    flex_conf.next_header.field_mode = FIELD_MODE_FIXED;
    flex_conf.next_header.field_base = 32 * 8;

    memset(&flex_conf.next_protocol, 0, sizeof(flex_conf.next_protocol));

    struct rte_flow_item_flex_field sample_data[1];
    memset(&sample_data[0], 0, sizeof(sample_data));
    sample_data[0].field_mode = FIELD_MODE_FIXED;
    sample_data[0].field_size = 32;
    sample_data[0].field_base = flex_item_config->offset_ * 8;
    flex_conf.sample_data = &sample_data[0];
    flex_conf.nb_samples = 1;

    struct rte_flow_item_flex_link input_link;
    memset(&input_link, 0, sizeof(input_link));
    input_link.item = udp_item;
    flex_conf.input_link = &input_link;
    flex_conf.nb_inputs = 1;

    struct rte_flow_item_flex_link output_link;
    memset(&output_link, 0, sizeof(output_link));
    output_link.item = pattern[4];
    flex_conf.output_link = &output_link;
    flex_conf.nb_outputs = 0;

    struct rte_flow_item_flex_handle* item_handle =
        rte_flow_flex_item_create(port, &flex_conf, &error);
    if (item_handle == nullptr) {
      DAQIRI_LOG_CRITICAL("Failed to create flex item on port {}: {}",
                          port,
                          error.message ? error.message : "unknown error");
      return nullptr;
    }
    flex_item_handles_[handle_key] = {static_cast<uint16_t>(port), item_handle};
    handle_it = flex_item_handles_.find(handle_key);
  }

  if (handle_it == flex_item_handles_.end() || handle_it->second.handle == nullptr) {
    return nullptr;
  }

  struct rte_flow_item_flex_handle* item_handle = handle_it->second.handle;

  /* Define the new protocol header structure */
  struct rte_udp_flex_hdr {
    rte_be32_t my_header;    /* my header */
  } __attribute__((packed));

  struct rte_udp_flex_hdr flex_spec;
  flex_spec.my_header = match_info.val_;

  struct rte_udp_flex_hdr flex_mask;
  flex_mask.my_header = match_info.mask_;

  /* Initialize spec and mask accessing the structure fields */
  struct rte_flow_item_flex spec = {
    .handle = item_handle,   /* opaque item handle */
    .length = sizeof(struct rte_udp_flex_hdr),
    .pattern = (const uint8_t *)&flex_spec
  };

  struct rte_flow_item_flex mask = {
    .handle = item_handle,   /* opaque item handle */
    .length = sizeof(struct rte_udp_flex_hdr),
    .pattern = (const uint8_t *)&flex_mask
  };

  /* Initialize RTE Flow item itself */
  struct rte_flow_item item = {
    .type = RTE_FLOW_ITEM_TYPE_FLEX,
    .spec = (const void *)&spec,
    .mask = (const void *)&mask
  };
  pattern[3] = item;

  attr.ingress = 1;
  attr.priority = 0;
  attr.group = 1;

  res = rte_flow_validate(port, &attr, pattern, action, &error);
  if (res != 0) {
    DAQIRI_LOG_CRITICAL("Failed to validate flex-item RX flow on port {} (match {:08x}): {} ({})",
                        port,
                        match_info.val_,
                        error.message ? error.message : "unknown error",
                        rte_strerror(rte_errno));
    return nullptr;
  }

  flow = rte_flow_create(port, &attr, pattern, action, &error);
  if (flow == nullptr) {
    DAQIRI_LOG_CRITICAL("Failed to create flex-item RX flow on port {} (match {:08x}): {} ({})",
                        port,
                        match_info.val_,
                        error.message ? error.message : "unknown error",
                        rte_strerror(rte_errno));
  } else if (track) {
    track_flow(static_cast<uint16_t>(port), flow);
  }

  return flow;
}

namespace {

bool parse_ipv4_be(const std::string& text, rte_be32_t* out) {
  struct in_addr addr {};
  if (out == nullptr || inet_pton(AF_INET, text.c_str(), &addr) != 1) { return false; }
  *out = addr.s_addr;
  return true;
}

bool parse_mac_addr(const std::string& text, struct rte_ether_addr* out) {
  return out != nullptr && rte_ether_unformat_addr(text.c_str(), out) == 0;
}

void set_u24_be(uint8_t out[3], uint32_t value) {
  out[0] = static_cast<uint8_t>((value >> 16) & 0xff);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xff);
  out[2] = static_cast<uint8_t>(value & 0xff);
}

void append_bytes(std::vector<uint8_t>& dst, const void* src, size_t len) {
  const auto* bytes = static_cast<const uint8_t*>(src);
  dst.insert(dst.end(), bytes, bytes + len);
}

bool fill_outer_eth_ip(DpdkEngine::DpdkFlowResource::ActionStorage& storage,
                       const TunnelConfig& tunnel, uint8_t proto) {
  rte_be32_t src_addr = 0;
  rte_be32_t dst_addr = 0;
  if (!parse_mac_addr(tunnel.outer_eth_dst_, &storage.eth_spec.hdr.dst_addr) ||
      !parse_mac_addr(tunnel.outer_eth_src_, &storage.eth_spec.hdr.src_addr) ||
      !parse_ipv4_be(tunnel.outer_ipv4_src_, &src_addr) ||
      !parse_ipv4_be(tunnel.outer_ipv4_dst_, &dst_addr)) {
    return false;
  }

  memset(&storage.eth_mask, 0, sizeof(storage.eth_mask));
  memset(&storage.ipv4_mask, 0, sizeof(storage.ipv4_mask));
  storage.eth_spec.hdr.ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
  memset(&storage.eth_mask.hdr.dst_addr, 0xff, sizeof(storage.eth_mask.hdr.dst_addr));
  memset(&storage.eth_mask.hdr.src_addr, 0xff, sizeof(storage.eth_mask.hdr.src_addr));
  storage.eth_mask.hdr.ether_type = 0xffff;

  storage.ipv4_spec.hdr.version_ihl = RTE_IPV4_VHL_DEF;
  storage.ipv4_spec.hdr.type_of_service = tunnel.outer_ipv4_tos_;
  storage.ipv4_spec.hdr.time_to_live = tunnel.outer_ipv4_ttl_;
  storage.ipv4_spec.hdr.next_proto_id = proto;
  storage.ipv4_spec.hdr.src_addr = src_addr;
  storage.ipv4_spec.hdr.dst_addr = dst_addr;
  storage.ipv4_mask.hdr.src_addr = 0xffffffffu;
  storage.ipv4_mask.hdr.dst_addr = 0xffffffffu;
  storage.ipv4_mask.hdr.next_proto_id = 0xff;
  return true;
}

bool fill_vxlan_storage(DpdkEngine::DpdkFlowResource::ActionStorage& storage,
                        const TunnelConfig& tunnel) {
  if (!fill_outer_eth_ip(storage, tunnel, IPPROTO_UDP)) { return false; }
  storage.udp_spec.hdr.src_port = rte_cpu_to_be_16(tunnel.outer_udp_src_);
  storage.udp_spec.hdr.dst_port = rte_cpu_to_be_16(tunnel.outer_udp_dst_);
  storage.udp_mask.hdr.src_port = tunnel.outer_udp_src_ == 0 ? 0 : 0xffff;
  storage.udp_mask.hdr.dst_port = 0xffff;
  storage.vxlan_spec.hdr.vx_flags = rte_cpu_to_be_32(0x08000000u);
  storage.vxlan_spec.hdr.vx_vni = rte_cpu_to_be_32(tunnel.vni_ << 8);
  storage.vxlan_mask.hdr.vx_flags = rte_cpu_to_be_32(0xff000000u);
  storage.vxlan_mask.hdr.vx_vni = rte_cpu_to_be_32(0xffffff00u);

  storage.definition[0] = {.type = RTE_FLOW_ITEM_TYPE_ETH,
                           .spec = &storage.eth_spec,
                           .last = nullptr,
                           .mask = &storage.eth_mask};
  storage.definition[1] = {.type = RTE_FLOW_ITEM_TYPE_IPV4,
                           .spec = &storage.ipv4_spec,
                           .last = nullptr,
                           .mask = &storage.ipv4_mask};
  storage.definition[2] = {.type = RTE_FLOW_ITEM_TYPE_UDP,
                           .spec = &storage.udp_spec,
                           .last = nullptr,
                           .mask = &storage.udp_mask};
  storage.definition[3] = {.type = RTE_FLOW_ITEM_TYPE_VXLAN,
                           .spec = &storage.vxlan_spec,
                           .last = nullptr,
                           .mask = &storage.vxlan_mask};
  storage.definition[4] = {.type = RTE_FLOW_ITEM_TYPE_END};
  storage.vxlan_encap.definition = storage.definition.data();
  return true;
}

bool fill_nvgre_storage(DpdkEngine::DpdkFlowResource::ActionStorage& storage,
                        const TunnelConfig& tunnel) {
  if (!fill_outer_eth_ip(storage, tunnel, IPPROTO_GRE)) { return false; }
  storage.nvgre_spec.c_k_s_rsvd0_ver = rte_cpu_to_be_16(0x2000);
  storage.nvgre_spec.protocol = rte_cpu_to_be_16(RTE_ETHER_TYPE_TEB);
  set_u24_be(storage.nvgre_spec.tni, tunnel.tni_);
  storage.nvgre_spec.flow_id = tunnel.flow_id_;
  storage.nvgre_mask.c_k_s_rsvd0_ver = 0xffff;
  storage.nvgre_mask.protocol = 0xffff;
  storage.nvgre_mask.tni[0] = 0xff;
  storage.nvgre_mask.tni[1] = 0xff;
  storage.nvgre_mask.tni[2] = 0xff;
  storage.nvgre_mask.flow_id = 0xff;

  storage.definition[0] = {.type = RTE_FLOW_ITEM_TYPE_ETH,
                           .spec = &storage.eth_spec,
                           .last = nullptr,
                           .mask = &storage.eth_mask};
  storage.definition[1] = {.type = RTE_FLOW_ITEM_TYPE_IPV4,
                           .spec = &storage.ipv4_spec,
                           .last = nullptr,
                           .mask = &storage.ipv4_mask};
  storage.definition[2] = {.type = RTE_FLOW_ITEM_TYPE_NVGRE,
                           .spec = &storage.nvgre_spec,
                           .last = nullptr,
                           .mask = &storage.nvgre_mask};
  storage.definition[3] = {.type = RTE_FLOW_ITEM_TYPE_END};
  storage.nvgre_encap.definition = storage.definition.data();
  return true;
}

bool fill_gre_raw_storage(DpdkEngine::DpdkFlowResource::ActionStorage& storage,
                          const TunnelConfig& tunnel) {
  if (!fill_outer_eth_ip(storage, tunnel, IPPROTO_GRE)) { return false; }
  struct rte_gre_hdr gre {};
  gre.proto = rte_cpu_to_be_16(tunnel.gre_protocol_);
  storage.gre_spec.protocol = rte_cpu_to_be_16(tunnel.gre_protocol_);
  storage.gre_mask.protocol = 0xffff;
  append_bytes(storage.raw_data, &storage.eth_spec.hdr, sizeof(storage.eth_spec.hdr));
  append_bytes(storage.raw_data, &storage.ipv4_spec.hdr, sizeof(storage.ipv4_spec.hdr));
  append_bytes(storage.raw_data, &gre, sizeof(gre));
  storage.raw_encap.data = storage.raw_data.data();
  storage.raw_encap.preserve = nullptr;
  storage.raw_encap.size = storage.raw_data.size();
  storage.raw_decap.data = storage.raw_data.data();
  storage.raw_decap.size = storage.raw_data.size();
  return true;
}

void append_normal_match(struct rte_flow_item* pattern, int* idx, const FlowMatch& match,
                         struct rte_flow_item_ipv4* ip_spec,
                         struct rte_flow_item_ipv4* ip_mask,
                         struct rte_flow_item_udp* udp_spec,
                         struct rte_flow_item_udp* udp_mask) {
  bool has_ip_match = false;
  bool has_udp_match = false;
  if (match.ipv4_len_ > 0) {
    ip_spec->hdr.total_length = htons(match.ipv4_len_);
    ip_mask->hdr.total_length = 0xffff;
    has_ip_match = true;
  }
  if (match.ipv4_src_ != INADDR_ANY) {
    ip_spec->hdr.src_addr = match.ipv4_src_;
    ip_mask->hdr.src_addr = 0xffffffff;
    has_ip_match = true;
  }
  if (match.ipv4_dst_ != INADDR_ANY) {
    ip_spec->hdr.dst_addr = match.ipv4_dst_;
    ip_mask->hdr.dst_addr = 0xffffffff;
    has_ip_match = true;
  }
  if (match.udp_src_ > 0) {
    udp_spec->hdr.src_port = htons(match.udp_src_);
    udp_mask->hdr.src_port = 0xffff;
    has_udp_match = true;
  }
  if (match.udp_dst_ > 0) {
    udp_spec->hdr.dst_port = htons(match.udp_dst_);
    udp_mask->hdr.dst_port = 0xffff;
    has_udp_match = true;
  }
  if (has_udp_match) { has_ip_match = true; }
  if (has_ip_match) {
    pattern[(*idx)++] = {.type = RTE_FLOW_ITEM_TYPE_IPV4,
                         .spec = ip_spec,
                         .last = nullptr,
                         .mask = ip_mask};
  }
  if (has_udp_match) {
    pattern[(*idx)++] = {.type = RTE_FLOW_ITEM_TYPE_UDP,
                         .spec = udp_spec,
                         .last = nullptr,
                         .mask = udp_mask};
  }
}

int normal_match_pattern_item_count(const FlowMatch& match) {
  const bool has_udp_match = match.udp_src_ > 0 || match.udp_dst_ > 0;
  const bool has_ip_match = has_udp_match || match.ipv4_len_ > 0 ||
                            match.ipv4_src_ != INADDR_ANY ||
                            match.ipv4_dst_ != INADDR_ANY;
  return (has_ip_match ? 1 : 0) + (has_udp_match ? 1 : 0);
}

int transform_pattern_item_count(const FlowAction& flow_action) {
  if (flow_action.type_ == FlowType::VLAN_POP) { return 2; }
  if (flow_action.type_ != FlowType::TUNNEL_DECAP) { return 0; }

  switch (flow_action.tunnel_.type_) {
    case TunnelType::VXLAN:
      return 4;
    case TunnelType::GRE:
    case TunnelType::NVGRE:
      return 3;
    case TunnelType::NONE:
      return 0;
  }
  return 0;
}

int transform_action_item_count(const FlowAction& flow_action) {
  switch (flow_action.type_) {
    case FlowType::VLAN_PUSH:
      return 3;
    case FlowType::VLAN_POP:
    case FlowType::TUNNEL_ENCAP:
    case FlowType::TUNNEL_DECAP:
      return 1;
    case FlowType::QUEUE:
      return 0;
  }
  return 0;
}

bool append_tunnel_pattern(struct rte_flow_item* pattern, int* idx,
                           DpdkEngine::DpdkFlowResource& resource,
                           const TunnelConfig& tunnel) {
  resource.action_storage.emplace_back();
  auto& storage = resource.action_storage.back();
  switch (tunnel.type_) {
    case TunnelType::VXLAN:
      if (!fill_vxlan_storage(storage, tunnel)) { return false; }
      pattern[(*idx)++] = storage.definition[0];
      pattern[(*idx)++] = storage.definition[1];
      pattern[(*idx)++] = storage.definition[2];
      pattern[(*idx)++] = storage.definition[3];
      return true;
    case TunnelType::GRE:
      if (!fill_gre_raw_storage(storage, tunnel)) { return false; }
      pattern[(*idx)++] = {.type = RTE_FLOW_ITEM_TYPE_ETH,
                           .spec = &storage.eth_spec,
                           .last = nullptr,
                           .mask = &storage.eth_mask};
      pattern[(*idx)++] = {.type = RTE_FLOW_ITEM_TYPE_IPV4,
                           .spec = &storage.ipv4_spec,
                           .last = nullptr,
                           .mask = &storage.ipv4_mask};
      pattern[(*idx)++] = {.type = RTE_FLOW_ITEM_TYPE_GRE,
                           .spec = &storage.gre_spec,
                           .last = nullptr,
                           .mask = &storage.gre_mask};
      return true;
    case TunnelType::NVGRE:
      if (!fill_nvgre_storage(storage, tunnel)) { return false; }
      pattern[(*idx)++] = storage.definition[0];
      pattern[(*idx)++] = storage.definition[1];
      pattern[(*idx)++] = storage.definition[2];
      return true;
    case TunnelType::NONE:
      return false;
  }
  return false;
}

bool append_transform_action(struct rte_flow_action* actions, int* idx,
                             DpdkEngine::DpdkFlowResource& resource,
                             const FlowAction& flow_action) {
  resource.action_storage.emplace_back();
  auto& storage = resource.action_storage.back();
  switch (flow_action.type_) {
    case FlowType::VLAN_PUSH:
      storage.push_vlan.ethertype = rte_cpu_to_be_16(flow_action.vlan_.ethertype_);
      storage.set_vlan_vid.vlan_vid = rte_cpu_to_be_16(
          static_cast<uint16_t>((flow_action.vlan_.dei_ ? 0x1000 : 0) |
                                (flow_action.vlan_.vlan_id_ & 0x0fff)));
      storage.set_vlan_pcp.vlan_pcp = flow_action.vlan_.pcp_;
      actions[(*idx)++] = {.type = RTE_FLOW_ACTION_TYPE_OF_PUSH_VLAN,
                           .conf = &storage.push_vlan};
      actions[(*idx)++] = {.type = RTE_FLOW_ACTION_TYPE_OF_SET_VLAN_VID,
                           .conf = &storage.set_vlan_vid};
      actions[(*idx)++] = {.type = RTE_FLOW_ACTION_TYPE_OF_SET_VLAN_PCP,
                           .conf = &storage.set_vlan_pcp};
      return true;
    case FlowType::VLAN_POP:
      actions[(*idx)++] = {.type = RTE_FLOW_ACTION_TYPE_OF_POP_VLAN, .conf = nullptr};
      return true;
    case FlowType::TUNNEL_ENCAP:
      switch (flow_action.tunnel_.type_) {
        case TunnelType::VXLAN:
          if (!fill_vxlan_storage(storage, flow_action.tunnel_)) { return false; }
          actions[(*idx)++] = {.type = RTE_FLOW_ACTION_TYPE_VXLAN_ENCAP,
                               .conf = &storage.vxlan_encap};
          return true;
        case TunnelType::GRE:
          if (!fill_gre_raw_storage(storage, flow_action.tunnel_)) { return false; }
          actions[(*idx)++] = {.type = RTE_FLOW_ACTION_TYPE_RAW_ENCAP,
                               .conf = &storage.raw_encap};
          return true;
        case TunnelType::NVGRE:
          if (!fill_nvgre_storage(storage, flow_action.tunnel_)) { return false; }
          actions[(*idx)++] = {.type = RTE_FLOW_ACTION_TYPE_NVGRE_ENCAP,
                               .conf = &storage.nvgre_encap};
          return true;
        case TunnelType::NONE:
          return false;
      }
      break;
    case FlowType::TUNNEL_DECAP:
      switch (flow_action.tunnel_.type_) {
        case TunnelType::VXLAN:
          actions[(*idx)++] = {.type = RTE_FLOW_ACTION_TYPE_VXLAN_DECAP, .conf = nullptr};
          return true;
        case TunnelType::GRE:
          if (!fill_gre_raw_storage(storage, flow_action.tunnel_)) { return false; }
          actions[(*idx)++] = {.type = RTE_FLOW_ACTION_TYPE_RAW_DECAP,
                               .conf = &storage.raw_decap};
          return true;
        case TunnelType::NVGRE:
          actions[(*idx)++] = {.type = RTE_FLOW_ACTION_TYPE_NVGRE_DECAP, .conf = nullptr};
          return true;
        case TunnelType::NONE:
          return false;
      }
      break;
    case FlowType::QUEUE:
      return false;
  }
  return false;
}

}  // namespace


// Taken from flow_block.c DPDK example */
struct rte_flow* DpdkEngine::add_flow(int port,
                                      const FlowConfig& cfg,
                                      bool track,
                                      std::shared_ptr<DpdkFlowResource>* resource_out) {
  struct rte_flow_attr attr {};
  struct rte_flow_item pattern[MAX_PATTERN_NUM] {};
  struct rte_flow_action action[MAX_ACTION_NUM] {};
  struct rte_flow_action_queue queue = {.index = cfg.action_.id_};
  struct rte_flow_action_mark  mark = {.id = cfg.id_};
  struct rte_flow_error error {};
  struct rte_flow_item_udp udp_spec {};
  struct rte_flow_item_udp udp_mask {};
  struct rte_flow_item_ipv4 ip_spec {};
  struct rte_flow_item_ipv4 ip_mask {};

  if (!ensure_eth_jump_rule(port, 3)) { return nullptr; }

  auto resource = std::make_shared<DpdkFlowResource>();
  resource->port = port;
  resource->action_storage.reserve(cfg.actions_.size() * 2 + 2);

  int required_pattern_items = 1 + normal_match_pattern_item_count(cfg.match_) + 1;
  int required_action_items = 2 + 1;
  for (const auto& flow_action : cfg.actions_) {
    required_pattern_items += transform_pattern_item_count(flow_action);
    required_action_items += transform_action_item_count(flow_action);
  }
  if (required_pattern_items > MAX_PATTERN_NUM) {
    DAQIRI_LOG_CRITICAL("RX flow '{}' requires {} DPDK pattern entries, maximum is {}",
                        cfg.name_, required_pattern_items, MAX_PATTERN_NUM);
    return nullptr;
  }
  if (required_action_items > MAX_ACTION_NUM) {
    DAQIRI_LOG_CRITICAL("RX flow '{}' requires {} DPDK action entries, maximum is {}",
                        cfg.name_, required_action_items, MAX_ACTION_NUM);
    return nullptr;
  }

  int pi = 0;
  bool has_outer_transform = false;
  for (const auto& flow_action : cfg.actions_) {
    if (flow_action.type_ == FlowType::TUNNEL_DECAP) {
      if (!append_tunnel_pattern(pattern, &pi, *resource, flow_action.tunnel_)) {
        DAQIRI_LOG_CRITICAL("Failed to build tunnel pattern for RX flow '{}'", cfg.name_);
        return nullptr;
      }
      has_outer_transform = true;
    } else if (flow_action.type_ == FlowType::VLAN_POP) {
      pattern[pi++].type = RTE_FLOW_ITEM_TYPE_ETH;
      pattern[pi++].type = RTE_FLOW_ITEM_TYPE_VLAN;
      has_outer_transform = true;
    }
  }
  pattern[pi++].type = RTE_FLOW_ITEM_TYPE_ETH;
  append_normal_match(pattern, &pi, cfg.match_, &ip_spec, &ip_mask, &udp_spec, &udp_mask);
  pattern[pi].type = RTE_FLOW_ITEM_TYPE_END;

  int ai = 0;
  for (const auto& flow_action : cfg.actions_) {
    if (flow_action.type_ == FlowType::QUEUE) { continue; }
    if (!append_transform_action(action, &ai, *resource, flow_action)) {
      DAQIRI_LOG_CRITICAL("Failed to build RX action '{}' for flow '{}'",
                          flow_type_to_string(flow_action.type_), cfg.name_);
      return nullptr;
    }
  }
  action[ai++] = {.type = RTE_FLOW_ACTION_TYPE_MARK, .conf = &mark};
  action[ai++] = {.type = RTE_FLOW_ACTION_TYPE_QUEUE, .conf = &queue};
  action[ai].type = RTE_FLOW_ACTION_TYPE_END;

  attr.ingress = 1;
  attr.priority = has_outer_transform ? 0 : 1;
  attr.group = 3;

  int res = rte_flow_validate(port, &attr, pattern, action, &error);
  if (res != 0) {
    DAQIRI_LOG_CRITICAL("Failed to validate RX flow '{}' on port {}: {} ({})",
                        cfg.name_,
                        port,
                        error.message ? error.message : "unknown error",
                        rte_strerror(rte_errno));
    return nullptr;
  }

  struct rte_flow* flow = rte_flow_create(port, &attr, pattern, action, &error);
  if (flow == nullptr) {
    DAQIRI_LOG_CRITICAL("Failed to create RX flow '{}' on port {}: {} ({})",
                        cfg.name_,
                        port,
                        error.message ? error.message : "unknown error",
                        rte_strerror(rte_errno));
    return nullptr;
  }
  resource->flow = flow;
  if (resource_out != nullptr) { *resource_out = resource; }
  if (track) { owned_flows_.push_back(resource); }
  return flow;
}

struct rte_flow* DpdkEngine::add_tx_flow(int port, const FlowConfig& cfg) {
  struct rte_flow_attr attr {};
  struct rte_flow_item pattern[MAX_PATTERN_NUM] {};
  struct rte_flow_action action[MAX_ACTION_NUM] {};
  struct rte_flow_error error {};
  struct rte_flow_item_udp udp_spec {};
  struct rte_flow_item_udp udp_mask {};
  struct rte_flow_item_ipv4 ip_spec {};
  struct rte_flow_item_ipv4 ip_mask {};

  auto resource = std::make_shared<DpdkFlowResource>();
  resource->port = port;
  resource->action_storage.reserve(cfg.actions_.size() * 2 + 1);

  const int required_pattern_items = 1 + normal_match_pattern_item_count(cfg.match_) + 1;
  int required_action_items = 1;
  for (const auto& flow_action : cfg.actions_) {
    required_action_items += transform_action_item_count(flow_action);
  }
  if (required_pattern_items > MAX_PATTERN_NUM) {
    DAQIRI_LOG_CRITICAL("TX flow '{}' requires {} DPDK pattern entries, maximum is {}",
                        cfg.name_, required_pattern_items, MAX_PATTERN_NUM);
    return nullptr;
  }
  if (required_action_items > MAX_ACTION_NUM) {
    DAQIRI_LOG_CRITICAL("TX flow '{}' requires {} DPDK action entries, maximum is {}",
                        cfg.name_, required_action_items, MAX_ACTION_NUM);
    return nullptr;
  }

  int pi = 0;
  pattern[pi++].type = RTE_FLOW_ITEM_TYPE_ETH;
  append_normal_match(pattern, &pi, cfg.match_, &ip_spec, &ip_mask, &udp_spec, &udp_mask);
  pattern[pi].type = RTE_FLOW_ITEM_TYPE_END;

  int ai = 0;
  for (const auto& flow_action : cfg.actions_) {
    if (!append_transform_action(action, &ai, *resource, flow_action)) {
      DAQIRI_LOG_CRITICAL("Failed to build TX action '{}' for flow '{}'",
                          flow_type_to_string(flow_action.type_), cfg.name_);
      return nullptr;
    }
  }
  action[ai].type = RTE_FLOW_ACTION_TYPE_END;

  attr.egress = 1;
  attr.priority = 1;

  int res = rte_flow_validate(port, &attr, pattern, action, &error);
  if (res != 0) {
    DAQIRI_LOG_CRITICAL("Failed to validate TX flow '{}' on port {}: {} ({})",
                        cfg.name_,
                        port,
                        error.message ? error.message : "unknown error",
                        rte_strerror(rte_errno));
    return nullptr;
  }

  struct rte_flow* flow = rte_flow_create(port, &attr, pattern, action, &error);
  if (flow == nullptr) {
    DAQIRI_LOG_CRITICAL("Failed to create TX flow '{}' on port {}: {} ({})",
                        cfg.name_,
                        port,
                        error.message ? error.message : "unknown error",
                        rte_strerror(rte_errno));
    return nullptr;
  }
  resource->flow = flow;
  owned_flows_.push_back(resource);
  return flow;
}

bool DpdkEngine::add_send_to_kernel_fallback(int port, uint32_t group) {
  struct rte_flow_attr attr;
  struct rte_flow_item pattern[MAX_PATTERN_NUM];
  struct rte_flow_action action[MAX_ACTION_NUM];
  struct rte_flow_error error;

  memset(&attr, 0, sizeof(attr));
  memset(pattern, 0, sizeof(pattern));
  memset(action, 0, sizeof(action));
  memset(&error, 0, sizeof(error));

  attr.ingress = 1;
  attr.group = group;
  attr.priority = 100;

  pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
  pattern[1].type = RTE_FLOW_ITEM_TYPE_END;

  action[0].type = RTE_FLOW_ACTION_TYPE_SEND_TO_KERNEL;
  action[1].type = RTE_FLOW_ACTION_TYPE_END;

  DAQIRI_LOG_INFO("Adding send-to-kernel fallback on port {} group {}", port, group);

  int ret = rte_flow_validate(port, &attr, pattern, action, &error);
  if (ret != 0) {
    DAQIRI_LOG_CRITICAL("Failed to validate send-to-kernel fallback on port {} group {}: {} ({})",
                        port,
                        group,
                        error.message ? error.message : "unknown error",
                        rte_strerror(rte_errno));
    return false;
  }

  struct rte_flow* flow = rte_flow_create(port, &attr, pattern, action, &error);
  if (flow == nullptr) {
    DAQIRI_LOG_CRITICAL("Failed to create send-to-kernel fallback on port {} group {}: {} ({})",
                        port,
                        group,
                        error.message ? error.message : "unknown error",
                        rte_strerror(rte_errno));
    return false;
  }

  track_flow(static_cast<uint16_t>(port), flow);
  return true;
}

Status DpdkEngine::drop_all_traffic(int port) {
  /* Declaring structs being used. */
  struct rte_flow_attr attr;
  struct rte_flow_item pattern[MAX_PATTERN_NUM];
  struct rte_flow_action action[MAX_ACTION_NUM];
  struct rte_flow* flow = NULL;
  struct rte_flow_error error;
  DropTrafficConfig config{};

  // Jump to group 3 is shared with init-time RX flows; only the drop rule is owned here.
  if (!ensure_eth_jump_rule(port, 3)) {
    DAQIRI_LOG_CRITICAL("Cannot drop all traffic on port {}: ETH jump to group 3 unavailable",
                        port);
    return Status::INTERNAL_ERROR;
  }

  // Clear all structures
  memset(pattern, 0, sizeof(pattern));
  memset(action, 0, sizeof(action));
  memset(&attr, 0, sizeof(struct rte_flow_attr));

  // Set DROP action
  action[0].type = RTE_FLOW_ACTION_TYPE_DROP;
  action[1].type = RTE_FLOW_ACTION_TYPE_END;

  // Match all ethernet packets
  pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
  pattern[1].type = RTE_FLOW_ITEM_TYPE_END;

  // Set highest priority (0) and use group 3 (consistent with add_flow)
  attr.ingress = 1;
  attr.priority = 0;  // Highest priority - blocks all traffic
  attr.group = 3;

  DAQIRI_LOG_INFO("Creating drop all traffic rule on port {} with priority {}",
                  port,
                  attr.priority);

  int ret = rte_flow_validate(port, &attr, pattern, action, &error);
  if (ret != 0) {
    DAQIRI_LOG_CRITICAL("Failed to validate drop-all-traffic rule on port {}: {} ({})",
                        port,
                        error.message ? error.message : "unknown error",
                        rte_strerror(rte_errno));
    return Status::INTERNAL_ERROR;
  }

  config.drop = rte_flow_create(port, &attr, pattern, action, &error);

  if (config.drop == nullptr) {
    DAQIRI_LOG_CRITICAL("Failed to create drop all traffic flow rule on port {}: {}",
                        port,
                        error.message ? error.message : "unknown error");
    return Status::INTERNAL_ERROR;
  } else {
    DAQIRI_LOG_INFO("Successfully created drop all traffic rule on port {}", port);
  }

  drop_all_traffic_flow[port] = config;
  flush_rx_queues.store(true);

  return Status::SUCCESS;
}

Status DpdkEngine::allow_all_traffic(int port) {
  if (drop_all_traffic_flow[port].drop == nullptr) {
    DAQIRI_LOG_ERROR("Cannot remove drop rule: flow pointer is null");
    return Status::INVALID_PARAMETER;
  }

  // Tell the RX threads they can keep processing packets
  flush_rx_queues.store(false);

  // ETH jump to group 3 is shared with init-time RX flows and is not destroyed here.
  struct rte_flow_error drop_error;
  int drop_ret = rte_flow_destroy(port, drop_all_traffic_flow[port].drop, &drop_error);

  if (drop_ret != 0) {
    DAQIRI_LOG_ERROR("Failed to destroy drop all traffic flow rule on port {}: {}",
                     port,
                     drop_error.message ? drop_error.message : "unknown error");
    return Status::INTERNAL_ERROR;
  }

  drop_all_traffic_flow[port].drop = nullptr;

  DAQIRI_LOG_INFO(
    "Successfully removed drop all traffic rule on port {}, traffic is now allowed", port);
  return Status::SUCCESS;
}

struct rte_flow* DpdkEngine::add_modify_flow_set(int port, int queue, const char* buf, int len,
                                              Direction direction) {
  struct rte_flow_attr attr;
  struct rte_flow_item pattern[MAX_PATTERN_NUM];
  struct rte_flow_action action[MAX_ACTION_NUM];
  struct rte_flow* flow = NULL;
  struct rte_flow_action_modify_field mf;
  struct rte_flow_error error;
  struct rte_flow_item_eth eth;
  struct rte_flow_field_data src;
  struct rte_flow_field_data dst;

  int res;

  memset(pattern, 0, sizeof(pattern));
  memset(action, 0, sizeof(action));
  memset(&eth, 0, sizeof(struct rte_flow_item_eth));

  /* Set the rule attribute, only ingress packets will be checked. 8< */
  memset(&attr, 0, sizeof(struct rte_flow_attr));
  attr.ingress = (direction == Direction::RX) ? 1 : 0;
  attr.egress = (direction == Direction::TX) ? 1 : 0;

  // mf.operation = RTE_FLOW_MODIFY_SET;

  // mf.src.field      = RTE_FLOW_FIELD_VALUE;
  // mf.src.level      = 0;
  // mf.src.tag_index  = 0;
  // mf.src.type       = 0;
  // mf.src.class_id   = 0;
  // mf.src.offset     = 0;
  // memcpy(mf.src.value, buf, len / 8);
  // printf("%02x %02x %02x %02x %02x %02x %d\n", mf.src.value[0], mf.src.value[1], mf.src.value[2],
  // mf.src.value[3], mf.src.value[4], mf.src.value[5],len / 8);

  // mf.dst.field      = RTE_FLOW_FIELD_MAC_SRC;
  // mf.dst.level      = 0;
  // mf.dst.tag_index  = 0;
  // mf.src.type       = 0;
  // mf.src.class_id   = 0;
  // mf.src.offset     = 0;

  // mf.width = len;

  // action[0].type  = RTE_FLOW_ACTION_TYPE_MODIFY_FIELD;
  // action[0].conf  = &mf;
  // action[1].type  = RTE_FLOW_ACTION_TYPE_END;
  // pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
  // pattern[0].spec = &eth;
  // pattern[0].mask = &eth;
  // attr.priority = 0;

  // pattern[1].type = RTE_FLOW_ITEM_TYPE_END;

  struct rte_flow_action_set_mac sm;
  memcpy(&sm, buf, len / 8);
  action[0].type = RTE_FLOW_ACTION_TYPE_SET_MAC_SRC;
  action[0].conf = &sm;
  action[1].type = RTE_FLOW_ACTION_TYPE_END;
  pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
  pattern[0].spec = &eth;
  pattern[0].mask = &eth;
  attr.priority = 1;

  pattern[1].type = RTE_FLOW_ITEM_TYPE_END;

  res = rte_flow_validate(port, &attr, pattern, action, &error);
  if (res != 0) {
    DAQIRI_LOG_CRITICAL("Failed to validate TX modify flow on port {} queue {}: {} ({})",
                        port,
                        queue,
                        error.message ? error.message : "unknown error",
                        rte_strerror(rte_errno));
    return nullptr;
  }

  flow = rte_flow_create(port, &attr, pattern, action, &error);
  if (flow == nullptr) {
    DAQIRI_LOG_CRITICAL("Failed to create TX modify flow on port {} queue {}: {} ({})",
                        port,
                        queue,
                        error.message ? error.message : "unknown error",
                        rte_strerror(rte_errno));
  } else {
    track_flow(static_cast<uint16_t>(port), flow);
  }
  return flow;
}

bool DpdkEngine::apply_tx_offloads(int port) {
  for (const auto& q : cfg_.ifs_[port].tx_.queues_) {
    for (const auto& off : q.common_.offloads_) {
      if (off == "tx_eth_src") {  // Offload Ethernet source copy
        DAQIRI_LOG_INFO("Applying {} offload for port {}", off, port);
        const auto mac_bytes = mac_addrs[port];
        if (add_modify_flow_set(port,
                                q.common_.id_,
                                reinterpret_cast<const char*>(&mac_bytes),
                                sizeof(mac_bytes) * 8,
                                Direction::TX) == nullptr) {
          DAQIRI_LOG_CRITICAL("Failed to apply {} offload for port {} queue {}",
                              off,
                              port,
                              q.common_.id_);
          return false;
        }
      }
    }
  }
  return true;
}

////////////////////////////////////////////////////////////////////////////////
///
///  \brief
///
////////////////////////////////////////////////////////////////////////////////
void DpdkEngine::PrintDpdkStats(int port) {
  struct rte_eth_stats eth_stats;
  int len, ret;

  rte_eth_stats_get(port, &eth_stats);
  DAQIRI_LOG_INFO("Port {}:", port);

  DAQIRI_LOG_INFO(" - Received packets:    {}", eth_stats.ipackets);
  DAQIRI_LOG_INFO(" - Transmit packets:    {}", eth_stats.opackets);
  DAQIRI_LOG_INFO(" - Received bytes:      {}", eth_stats.ibytes);
  DAQIRI_LOG_INFO(" - Transmit bytes:      {}", eth_stats.obytes);
  DAQIRI_LOG_INFO(" - Missed packets:      {}", eth_stats.imissed);
  DAQIRI_LOG_INFO(" - Errored packets:     {}", eth_stats.ierrors);
  DAQIRI_LOG_INFO(" - RX out of buffers:   {}", eth_stats.rx_nombuf);

  DAQIRI_LOG_INFO("   ** Extended Stats **");

  struct rte_eth_xstat *xstats;
  struct rte_eth_xstat_name *xstats_names;

  /* Clear screen and move to top left */
  len = rte_eth_xstats_get(port, NULL, 0);
  if (len < 0)
    rte_exit(EXIT_FAILURE, "rte_eth_xstats_get(%u) failed: %d", 0, len);
  xstats = (struct rte_eth_xstat *)calloc(len, sizeof(*xstats));
  if (xstats == NULL)
    rte_exit(EXIT_FAILURE, "Failed to calloc memory for xstats");
  ret = rte_eth_xstats_get(port, xstats, len);
  if (ret < 0 || ret > len) {
    free(xstats);
    rte_exit(EXIT_FAILURE, "rte_eth_xstats_get(%u) len%i failed: %d", 0, len, ret);
  }
  xstats_names = (struct rte_eth_xstat_name *)calloc(len, sizeof(*xstats_names));
  if (xstats_names == NULL) {
    free(xstats);
    rte_exit(EXIT_FAILURE, "Failed to calloc memory for xstats_names");
  }
  ret = rte_eth_xstats_get_names(port, xstats_names, len);
  if (ret < 0 || ret > len) {
    free(xstats);
    free(xstats_names);
    rte_exit(EXIT_FAILURE, "rte_eth_xstats_get_names(%u) len%i failed: %d", 0, len, ret);
  }
  for (int i = 0; i < len; i++) {
    if (xstats[i].value > 0)
    DAQIRI_LOG_INFO("      {}:\t\t{}", xstats_names[i].name, xstats[i].value);
  }

  free(xstats);
  free(xstats_names);
}

DpdkEngine::~DpdkEngine() {
    cleanup_reorder_state();

    // shutdown() handles ring cleanup BEFORE rte_eal_cleanup(), so the maps
    // are empty by the time we get here on the normal exit path. The loops
    // below cover the partial-init / no-shutdown path, where eal_initialized_
    // may still be true and the rings (if any were created) are still valid.
    if (eal_initialized_) {
      for (auto const& [key, val] : rx_rings) {
        if (val != nullptr) { rte_ring_free(val); }
      }
      for (auto const& [key, val] : tx_rings) {
        if (val != nullptr) { rte_ring_free(val); }
      }
    }
    rx_rings.clear();
    tx_rings.clear();

    cleanup_dynamic_flows();
    destroy_all_flow_rules();

    // Final safety net: if shutdown() was never called (e.g. process exiting
    // via std::exit after a partial init), still release EAL and unlink any
    // leftover hugepage files we own.
    cleanup_eal();
}

bool DpdkEngine::validate_config() const {
  if (!Engine::validate_config()) { return false; }

  for (const auto& intf : cfg_.ifs_) {
    std::unordered_set<uint16_t> rx_queue_ids;
    for (const auto& q : intf.rx_.queues_) { rx_queue_ids.insert(q.common_.id_); }

    std::unordered_set<uint16_t> flex_ids;
    for (const auto& item : intf.rx_.flex_items_) {
      if (!flex_ids.insert(item.id_).second) {
        DAQIRI_LOG_ERROR("Duplicate flex item id {} on interface '{}'", item.id_, intf.name_);
        return false;
      }
    }

    std::unordered_map<FlowId, uint16_t> flow_to_queue;
    bool has_standard_flows = false;
    bool has_flex_item_flows = false;
    for (const auto& flow : intf.rx_.flows_) {
      if (flow_to_queue.find(flow.id_) != flow_to_queue.end()) {
        DAQIRI_LOG_ERROR("Duplicate flow ID {} on interface '{}'", flow.id_, intf.name_);
        return false;
      }
      if (rx_queue_ids.find(flow.action_.id_) == rx_queue_ids.end()) {
        DAQIRI_LOG_ERROR("Flow '{}' references unknown RX queue {} on interface '{}'",
                         flow.name_,
                         flow.action_.id_,
                         intf.name_);
        return false;
      }
      flow_to_queue.emplace(flow.id_, flow.action_.id_);
      if (flow.match_.type_ == FlowMatchType::FLEX_ITEM) {
        has_flex_item_flows = true;
        const uint16_t flex_item_id = flow.match_.flex_item_match_.flex_item_id_;
        const auto flex_item_it =
            std::find_if(intf.rx_.flex_items_.begin(),
                         intf.rx_.flex_items_.end(),
                         [flex_item_id](const FlexItemConfig& item) {
                           return item.id_ == flex_item_id;
                         });
        if (flex_item_it == intf.rx_.flex_items_.end()) {
          DAQIRI_LOG_ERROR("Flow '{}' references unknown flex item id {} on interface '{}'",
                           flow.name_,
                           flex_item_id,
                           intf.name_);
          return false;
        }
      } else {
        has_standard_flows = true;
      }
      if (has_standard_flows && has_flex_item_flows) {
        DAQIRI_LOG_ERROR(
            "Interface '{}' mixes standard (UDP/IP) and flex-item RX flows, which is not "
            "supported. Use only one flow class per interface.",
            intf.name_);
        return false;
      }
    }

    std::unordered_set<FlowId> reorder_flow_ids;
    for (const auto& reorder_cfg : intf.rx_.reorder_configs_) {
      const bool use_gpu_backend = reorder_cfg.reorder_type_ == "gpu";
      const bool use_cpu_backend = reorder_cfg.reorder_type_ == "cpu";
      if (!use_gpu_backend && !use_cpu_backend) {
        DAQIRI_LOG_ERROR("Unsupported reorder_type '{}' for config '{}' on interface '{}'",
                         reorder_cfg.reorder_type_,
                         reorder_cfg.name_,
                         intf.name_);
        return false;
      }

      int reorder_queue_id = -1;
      for (const auto flow_id : reorder_cfg.flow_ids_) {
        const auto flow_queue_it = flow_to_queue.find(flow_id);
        if (flow_queue_it == flow_to_queue.end()) {
          DAQIRI_LOG_ERROR("Reorder config '{}' references unknown flow ID {} on interface '{}'",
                           reorder_cfg.name_,
                           flow_id,
                           intf.name_);
          return false;
        }
        if (reorder_queue_id < 0) {
          reorder_queue_id = static_cast<int>(flow_queue_it->second);
        } else if (reorder_queue_id != static_cast<int>(flow_queue_it->second)) {
          DAQIRI_LOG_ERROR(
              "Reorder config '{}' has flow IDs mapped to multiple queues on interface '{}'. "
              "Each reorder config must map to a single RX queue",
              reorder_cfg.name_,
              intf.name_);
          return false;
        }
        if (reorder_flow_ids.find(flow_id) != reorder_flow_ids.end()) {
          DAQIRI_LOG_ERROR(
              "Flow ID {} appears in multiple reorder configs on interface '{}'. "
              "Flow overlap is not allowed in v1",
              flow_id,
              intf.name_);
          return false;
        }
        reorder_flow_ids.insert(flow_id);
      }

      auto queue_it = std::find_if(intf.rx_.queues_.begin(),
                                   intf.rx_.queues_.end(),
                                   [reorder_queue_id](const RxQueueConfig& qcfg) {
                                     return qcfg.common_.id_ == reorder_queue_id;
                                   });
      if (queue_it == intf.rx_.queues_.end()) {
        DAQIRI_LOG_ERROR("Reorder config '{}' maps to unknown RX queue {} on interface '{}'",
                         reorder_cfg.name_,
                         reorder_queue_id,
                         intf.name_);
        return false;
      }
      if (queue_it->common_.mrs_.size() != 1) {
        DAQIRI_LOG_ERROR(
            "Queue '{}'/{} using reorder config '{}' must define exactly one source memory "
            "region. Header-data split reordering is not supported.",
            queue_it->common_.name_,
            queue_it->common_.id_,
            reorder_cfg.name_);
        return false;
      }
      const auto source_mr_it = cfg_.mrs_.find(queue_it->common_.mrs_[0]);
      if (source_mr_it == cfg_.mrs_.end()) {
        DAQIRI_LOG_ERROR("Queue '{}'/{} using reorder config '{}' references unknown source MR '{}'",
                         queue_it->common_.name_,
                         queue_it->common_.id_,
                         reorder_cfg.name_,
                         queue_it->common_.mrs_[0]);
        return false;
      }
      if (use_gpu_backend && !is_cuda_accessible_packet_memory(source_mr_it->second.kind_)) {
        DAQIRI_LOG_ERROR(
            "Queue '{}'/{} using reorder config '{}' must use DEVICE or HOST_PINNED packet "
            "memory for GPU reorder",
            queue_it->common_.name_,
            queue_it->common_.id_,
            reorder_cfg.name_);
        return false;
      }
      if (use_cpu_backend && !is_cpu_accessible_memory(source_mr_it->second.kind_)) {
        DAQIRI_LOG_ERROR(
            "Queue '{}'/{} using reorder config '{}' must use HOST, HOST_PINNED, or HUGE packet "
            "memory for CPU reorder",
            queue_it->common_.name_,
            queue_it->common_.id_,
            reorder_cfg.name_);
        return false;
      }
      const auto output_mr_it = cfg_.mrs_.find(reorder_cfg.memory_region_);
      if (output_mr_it == cfg_.mrs_.end()) {
        DAQIRI_LOG_ERROR("Reorder config '{}' references unknown output memory region '{}'",
                         reorder_cfg.name_,
                         reorder_cfg.memory_region_);
        return false;
      }
      if (use_gpu_backend && output_mr_it->second.kind_ != MemoryKind::DEVICE
          && output_mr_it->second.kind_ != MemoryKind::HOST_PINNED) {
        DAQIRI_LOG_ERROR(
            "Reorder output memory region '{}' in config '{}' must be DEVICE or HOST_PINNED "
            "memory for GPU reorder",
            reorder_cfg.memory_region_,
            reorder_cfg.name_);
        return false;
      }
      if (use_cpu_backend && !is_cpu_accessible_memory(output_mr_it->second.kind_)) {
        DAQIRI_LOG_ERROR(
            "Reorder output memory region '{}' in config '{}' must be HOST, HOST_PINNED, or HUGE "
            "memory for CPU reorder",
            reorder_cfg.memory_region_,
            reorder_cfg.name_);
        return false;
      }
      const bool use_data_type_conversion = reorder_uses_data_type_conversion(reorder_cfg);
      if (use_data_type_conversion && use_cpu_backend) {
        DAQIRI_LOG_ERROR(
            "Reorder config '{}' data type conversion is supported only for gpu reorder_type",
            reorder_cfg.name_);
        return false;
      }
      if (reorder_cfg.payload_byte_offset_ >= source_mr_it->second.buf_size_) {
        DAQIRI_LOG_ERROR(
            "Reorder config '{}' payload_byte_offset {} is out of range for source MR '{}' size {}",
            reorder_cfg.name_,
            reorder_cfg.payload_byte_offset_,
            source_mr_it->second.name_,
            source_mr_it->second.buf_size_);
        return false;
      }

      const size_t source_slot_stride_size =
          source_mr_it->second.buf_size_ - reorder_cfg.payload_byte_offset_;
      if (source_slot_stride_size > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
        DAQIRI_LOG_ERROR(
            "Reorder config '{}' source slot size {} is too large",
            reorder_cfg.name_,
            source_slot_stride_size);
        return false;
      }
      const auto source_slot_stride = static_cast<uint32_t>(source_slot_stride_size);
      uint32_t output_slot_stride = 0;
      if (!compute_reorder_max_output_payload_len(
              reorder_cfg, source_slot_stride, &output_slot_stride)) {
        DAQIRI_LOG_ERROR(
            "Reorder config '{}' cannot convert source slot size {} from {} to {}",
            reorder_cfg.name_,
            source_slot_stride,
            reorder_data_type_to_string(reorder_cfg.data_types_.input_type_),
            reorder_data_type_to_string(reorder_cfg.data_types_.output_type_));
        return false;
      }

      uint32_t packets_per_batch = 0;
      if (reorder_cfg.method_ == ReorderMethod::SEQ_BATCH_NUMBER) {
        packets_per_batch = reorder_cfg.seq_batch_number_.packets_per_batch_;
      } else if (reorder_cfg.method_ == ReorderMethod::SEQ_PACKETS_PER_BATCH) {
        packets_per_batch = reorder_cfg.seq_packets_per_batch_.packets_per_batch_;
      }
      const uint64_t min_required_out =
          static_cast<uint64_t>(output_slot_stride) * static_cast<uint64_t>(packets_per_batch);
      if (min_required_out > output_mr_it->second.buf_size_) {
        DAQIRI_LOG_ERROR(
            "Reorder output MR '{}' buffer size {} is too small for config '{}' "
            "(required at least {} = packets_per_batch {} * output_slot_stride {})",
            output_mr_it->second.name_,
            output_mr_it->second.buf_size_,
            reorder_cfg.name_,
            min_required_out,
            packets_per_batch,
            output_slot_stride);
        return false;
      }
    }
  }

  DAQIRI_LOG_INFO("Config validated successfully");
  return true;
}

////////////////////////////////////////////////////////////////////////////////
///
///  \brief
///
////////////////////////////////////////////////////////////////////////////////
void DpdkEngine::run() {
  int secondary_id = 0;
  int icore;

  DAQIRI_LOG_INFO("Starting DAQIRI workers");

  // determine the correct process types for input/output
  int (*rx_worker)(void*);
  int (*tx_worker)(void*);

  if (loopback_ != LoopbackType::LOOPBACK_TYPE_SW) {
    rx_worker = rx_core_worker;
    tx_worker = tx_core_worker;
  } else {
    rx_worker = rx_lb_worker;
    tx_worker = tx_lb_worker;
  }

  // Launch RX workers. If the core is serving multiple queues then we launch a multi-q worker
  for (const auto &el : this->rx_core_q_map) {
    // Single queue
    if (el.second.size() == 1) {
      uint16_t port_id = el.second[0].first;
      uint16_t q_id    = el.second[0].second;
      uint32_t key     = generate_queue_key(port_id, q_id);
      const auto &q    = rx_cfg_q_map_[key];

      // Dummy queue made to appease HWS. Don't launch worker
      if (q->common_.name_.find("UNUSED") == 0) {
        continue;
      }

      auto params = new RxWorkerParams;
      params->port = port_id;
      params->num_segs = q->common_.mrs_.size();
      params->ring = rx_rings[key];
      params->lb_ring = loopback_ring;
      params->queue = q_id;
      params->rx_burst_pool = rx_burst_buffer;
      params->tx_burst_pool = tx_burst_buffers[0];
      params->rx_meta_pool = rx_metadata;
      params->tx_meta_pool = tx_metadata;
      params->batch_size = q->common_.batch_size_;
      params->timeout_us = q->timeout_us_;
      params->rx_meta_pool_size = cfg_.rx_meta_buffers_;
      rte_eal_remote_launch(
        rx_worker, (void*)params, strtol(q->common_.cpu_core_.c_str(), NULL, 10));
    } else {
      if (loopback_ == LoopbackType::LOOPBACK_TYPE_SW) {
        DAQIRI_LOG_CRITICAL("Loopback mode not yet configured for multiple RX queues!");
        exit(1);
      }
      // Multi-q worker
      auto params = new RxWorkerMultiQParams;
      for (const auto &q_info : el.second) {
        uint16_t port_id = q_info.first;
        uint16_t q_id    = q_info.second;
        uint32_t key     = generate_queue_key(port_id, q_id);
        const auto &q    = rx_cfg_q_map_[key];
        struct rte_ring* ring_ptr = rx_rings[key];

        params->q_params.push_back({port_id,
                                    q_id,
                                    (int)q->common_.mrs_.size(),
                                    q->common_.batch_size_,
                                    q->timeout_us_,
                                    ring_ptr,
                                    nullptr});
      }

      params->rx_burst_pool = rx_burst_buffer;
      params->rx_meta_pool = rx_metadata;
      params->rx_meta_pool_size = cfg_.rx_meta_buffers_;
      rte_eal_remote_launch(rx_core_multi_q_worker, (void*)params, el.first);
    }
  }

  for (const auto& intf : cfg_.ifs_) {
    if (intf.tx_.queues_.size() > 0) {
      const auto& tx = intf.tx_;
      for (auto& q : tx.queues_) {
        // Dummy queue is only for DPDK compatibility.
        if (q.common_.name_.find("UNUSED_TX_P") == 0) {
          continue;
        }

        uint32_t key = generate_queue_key(intf.port_id_, q.common_.id_);
        auto params = new TxWorkerParams;
        //  params->hds    = q.common_.hds_ > 0;
        params->port = intf.port_id_;
        params->ring = tx_rings[key];
        params->lb_ring = loopback_ring;
        params->queue = q.common_.id_;
        params->burst_pool = tx_burst_buffers[key];
        params->meta_pool = tx_metadata;
        params->batch_size = q.common_.batch_size_;
        rte_eth_macaddr_get(intf.port_id_, &params->mac_addr);
        // Packet pacing: active only when the SEND_ON_TIMESTAMP offload was
        // enabled above (timestamp_dynfield_offset_ >= 0). A zero offset/-1 makes
        // the worker skip pacing and send at line rate.
        params->engine = this;
        params->pacing_mbps = q.pacing_mbps_;
        params->tx_ts_dynfield_offset = timestamp_dynfield_offset_;
        params->tx_ts_dynflag_mask = tx_timestamp_dynflag_mask_;
        if (q.pacing_mbps_ > 0) {
          if (timestamp_dynfield_offset_ >= 0) {
            DAQIRI_LOG_INFO("TX port {} queue {} packet pacing enabled: {} Mbps", intf.port_id_,
                            q.common_.id_, q.pacing_mbps_);
          } else {
            DAQIRI_LOG_WARN(
                "TX port {} queue {} pacing_mbps={} ignored: SEND_ON_TIMESTAMP offload not active",
                intf.port_id_, q.common_.id_, q.pacing_mbps_);
          }
        }
        rte_eal_remote_launch(
            tx_worker, (void*)params, strtol(q.common_.cpu_core_.c_str(), NULL, 10));
      }
    }
  }

  DAQIRI_LOG_INFO("Done starting workers");
}

////////////////////////////////////////////////////////////////////////////////
///
///  \brief
///
////////////////////////////////////////////////////////////////////////////////
void DpdkEngine::flush_port_queue_impl(int port, int queue) {
  struct rte_mbuf* rx_mbuf;
  DAQIRI_LOG_INFO("Flushing packets on port {} queue {}", port, queue);
  while (rte_eth_rx_burst(port, queue, &rx_mbuf, 1) != 0) { rte_pktmbuf_free(rx_mbuf); }
}

void DpdkEngine::flush_port_queue(int port, int queue) {
  flush_port_queue_impl(port, queue);
}

/*
  RX worker supporting multiple queues for a single core. This is useful when a user wants
  to segregate traffic by queues, but they don't want to waste extra CPU cores by mapping a
  core per queue.
*/
int DpdkEngine::rx_core_multi_q_worker(void* arg) {
  RxWorkerMultiQParams* tparams = (RxWorkerMultiQParams*)arg;

  int ret = 0;
  uint64_t freq = rte_get_tsc_hz();
  uint64_t timeout_ticks = freq * 0.02;  // expect all packets within 20ms
  uint16_t num_queues = tparams->q_params.size();

  std::string pq_str = "";
  for (const auto &pq : tparams->q_params) {
    pq_str += std::to_string(pq.port) + "/" + std::to_string(pq.queue) + " ";
  }

  DAQIRI_LOG_INFO("Starting multi-queue RX Core {}, P/Q: {}, socket {}",
                    rte_lcore_id(),
                    pq_str,
                    rte_socket_id());

  std::array<int, Engine::MAX_RX_Q_PER_CORE> nb_rx{};
  std::array<int, Engine::MAX_RX_Q_PER_CORE> cur_pkt_in_batch{};
  std::array<BurstParams*, Engine::MAX_RX_Q_PER_CORE> bursts{};
  std::array<std::array<rte_mbuf*, DEFAULT_NUM_RX_BURST>, Engine::MAX_RX_Q_PER_CORE> mbuf_arr{};
  std::array<uint64_t, Engine::MAX_RX_Q_PER_CORE> total_pkts{};
  std::array<uint64_t, Engine::MAX_RX_Q_PER_CORE> last_cycles;
  std::generate(last_cycles.begin(), last_cycles.end(), rte_get_tsc_cycles);

  uint16_t cur_idx            = 0;
  uint16_t cur_port;
  uint16_t cur_q;
  uint16_t cur_segs;
  uint32_t cur_batch_size;
  uint64_t cur_timeout_cycles;

  auto update_cur_idx = [&]() {
    cur_idx            = (cur_idx + 1) % num_queues;
    cur_port           = tparams->q_params[cur_idx].port;
    cur_q              = tparams->q_params[cur_idx].queue;
    cur_segs           = tparams->q_params[cur_idx].num_segs;
    cur_batch_size     = tparams->q_params[cur_idx].batch_size;
    cur_timeout_cycles = tparams->q_params[cur_idx].timeout_us;
  };

  update_cur_idx();

  for (const auto& pq : tparams->q_params) {
    flush_port_queue_impl(pq.port, pq.queue);
  }

  //
  //  run loop
  //
  while (!force_quit.load()) {
    const bool should_flush = flush_rx_queues.load();

    if (should_flush) {
      // Free any packets we have stored in temporary arrays that are not pushed to the ring
      for (int i = 0; i < num_queues; i++) {
        // Free any unprocessed packets in mbuf_arr
        if (nb_rx[i] > 0) {
          for (int p = cur_pkt_in_batch[i]; p < cur_pkt_in_batch[i] + nb_rx[i]; p++) {
            rte_pktmbuf_free(mbuf_arr[i][p]);
          }
          nb_rx[i] = 0;
          cur_pkt_in_batch[i] = 0;
        }

        // Free any packets already stored in bursts that haven't been pushed to ring yet
        if (bursts[i] != nullptr) {
          // Free all packet mbufs stored in the burst
          for (int p = 0; p < bursts[i]->hdr.hdr.num_pkts; p++) {
            rte_pktmbuf_free(reinterpret_cast<rte_mbuf**>(bursts[i]->pkts[0])[p]);
          }

          // Free the segment buffers back to the burst pool
          release_rx_burst_segment_arrays(bursts[i],
                                          tparams->rx_burst_pool,
                                          bursts[i]->hdr.hdr.num_segs);

          // Free the burst metadata back to the meta pool
          rte_mempool_put(tparams->rx_meta_pool, bursts[i]);
          bursts[i] = nullptr;
        }
      }
    }

    if (bursts[cur_idx] == nullptr) {  // Allocate a new burst
      if (rte_mempool_get(tparams->rx_meta_pool, reinterpret_cast<void**>(&bursts[cur_idx])) < 0) {
        DAQIRI_LOG_CRITICAL("Running out of RX meta buffers due to high rates. Either increase "\
          "your number of metadata buffers (current: {}) with `rx_meta_buffers` (will "\
          "increase memory usage) or increase your `batch_size` for port {} queue {} (will "\
          "increase latency)", tparams->rx_meta_pool_size, cur_port, cur_q);
        exit(1);
      }

      if (!initialize_rx_burst(bursts[cur_idx],
                               cur_port,
                               cur_q,
                               cur_segs,
                               tparams->rx_burst_pool)) {
        rte_mempool_put(tparams->rx_meta_pool, bursts[cur_idx]);
        bursts[cur_idx] = nullptr;
        drop_pending_rx_mbufs(mbuf_arr[cur_idx].data(),
                              cur_pkt_in_batch[cur_idx],
                              nb_rx[cur_idx],
                              cur_port,
                              cur_q,
                              "no free RX burst metadata for split packet pointers");
        nb_rx[cur_idx] = 0;
        cur_pkt_in_batch[cur_idx] = 0;
        update_cur_idx();
        continue;
      }
    }

    // Check if we need to get more packets
    if (nb_rx[cur_idx] == 0) {
      cur_pkt_in_batch[cur_idx] = 0;
      nb_rx[cur_idx] = rte_eth_rx_burst(cur_port, cur_q,
                      reinterpret_cast<rte_mbuf**>(&mbuf_arr[cur_idx][0]), DEFAULT_NUM_RX_BURST);

      if (nb_rx[cur_idx] == 0) {
        if (bursts[cur_idx]->hdr.hdr.num_pkts > 0 && cur_timeout_cycles > 0) {
          const auto cur_cycles = rte_get_tsc_cycles();

          // We hit our timeout. Send the partial batch immediately
          if ((cur_cycles - last_cycles[cur_idx]) > cur_timeout_cycles) {
            rte_ring_enqueue(tparams->q_params[cur_idx].ring,
                        reinterpret_cast<void*>(bursts[cur_idx]));
            last_cycles[cur_idx] = cur_cycles;
            bursts[cur_idx] = nullptr;
          }
        }

        update_cur_idx();
        continue;
      }
    }

    // At this point we have some packets to copy either from a new batch or an existing one. Check
    // if we are finishing a batch or copying all packets that came in
    int to_copy = std::min(static_cast<size_t>(nb_rx[cur_idx]),
                           cur_batch_size - bursts[cur_idx]->hdr.hdr.num_pkts);
    int appended = 0;
    if (cur_segs == 1) {
      memcpy(&bursts[cur_idx]->pkts[0][bursts[cur_idx]->hdr.hdr.num_pkts],
             &mbuf_arr[cur_idx][cur_pkt_in_batch[cur_idx]],
             sizeof(rte_mbuf*) * to_copy);
      bursts[cur_idx]->hdr.hdr.num_pkts += to_copy;
      appended = to_copy;
    } else {  // Extra work when buffers are scattered
      for (int p = 0; p < to_copy; p++) {
        struct rte_mbuf* mbuf = mbuf_arr[cur_idx][cur_pkt_in_batch[cur_idx] + p];
        const auto pkt_idx = static_cast<int>(bursts[cur_idx]->hdr.hdr.num_pkts);
        if (populate_split_packet_segments(bursts[cur_idx], mbuf, pkt_idx, cur_segs, cur_port, cur_q)) {
          bursts[cur_idx]->hdr.hdr.num_pkts++;
          appended++;
          continue;
        }
        rte_pktmbuf_free(mbuf);
      }
    }

    cur_pkt_in_batch[cur_idx]         += to_copy;
    nb_rx[cur_idx]                    -= to_copy;
    total_pkts[cur_idx]               += appended;

    if (bursts[cur_idx]->hdr.hdr.num_pkts == cur_batch_size) {
      rte_ring_enqueue(tparams->q_params[cur_idx].ring, reinterpret_cast<void*>(bursts[cur_idx]));
      last_cycles[cur_idx] = rte_get_tsc_cycles();
      bursts[cur_idx] = nullptr;
    } else if (bursts[cur_idx]->hdr.hdr.num_pkts > 0 && cur_timeout_cycles > 0) {
      const auto cur_cycles = rte_get_tsc_cycles();

      // We hit our timeout. Send the partial batch immediately
      if ((cur_cycles - last_cycles[cur_idx]) > cur_timeout_cycles) {
        rte_ring_enqueue(tparams->q_params[cur_idx].ring, reinterpret_cast<void*>(bursts[cur_idx]));
        last_cycles[cur_idx] = cur_cycles;
        bursts[cur_idx] = nullptr;
      }
    }

    update_cur_idx();
  } while (!force_quit.load());

  for (int i = 0; i < num_queues; i++) {
    cur_port           = tparams->q_params[i].port;
    cur_q              = tparams->q_params[i].queue;
    DAQIRI_LOG_INFO("Total packets received by RX core {} (Port/Queue {}/{}): {}",
                     rte_lcore_id(),
                     cur_port,
                     cur_q,
                     total_pkts[i]);
  }

  return 0;
}


////////////////////////////////////////////////////////////////////////////////
///
///  \brief
///
////////////////////////////////////////////////////////////////////////////////
int DpdkEngine::rx_core_worker(void* arg) {
  RxWorkerParams* tparams = (RxWorkerParams*)arg;

  // In the future we may want to periodically update this if the CPU clock drifts
  uint64_t freq = rte_get_tsc_hz();
  uint64_t timeout_cycles = freq * (tparams->timeout_us/1e6);
  uint64_t last_cycles = rte_get_tsc_cycles();
  uint64_t total_pkts = 0;

  flush_port_queue_impl(tparams->port, tparams->queue);
  struct rte_mbuf* mbuf_arr[DEFAULT_NUM_RX_BURST];

  DAQIRI_LOG_INFO("Starting RX Core {}, port {}, queue {}, socket {}",
                    rte_lcore_id(),
                    tparams->port,
                    tparams->queue,
                    rte_socket_id());
  int nb_rx = 0;
  int cur_pkt_in_batch = 0;
  BurstParams* burst = nullptr;
  //
  //  run loop
  //
  while (!force_quit.load()) {
    const bool should_flush = flush_rx_queues.load();

    if (should_flush) {
      // Free any packets we have stored in temporary arrays that are not pushed to the ring
      // Free any unprocessed packets in mbuf_arr
      if (nb_rx > 0) {
        for (int p = cur_pkt_in_batch; p < cur_pkt_in_batch + nb_rx; p++) {
          rte_pktmbuf_free(mbuf_arr[p]);
        }
        nb_rx = 0;
        cur_pkt_in_batch = 0;
      }

      // Free any packets already stored in burst that haven't been pushed to ring yet
      if (burst != nullptr) {
        // Free all packet mbufs stored in the burst
        for (int p = 0; p < burst->hdr.hdr.num_pkts; p++) {
          rte_pktmbuf_free(reinterpret_cast<rte_mbuf**>(burst->pkts[0])[p]);
        }

        // Free the segment buffers back to the burst pool
        release_rx_burst_segment_arrays(burst, tparams->rx_burst_pool, burst->hdr.hdr.num_segs);

        // Free the burst metadata back to the meta pool
        rte_mempool_put(tparams->rx_meta_pool, burst);
        burst = nullptr;
      }
    }

    if (burst == nullptr) {  // Allocate a new burst
      if (rte_mempool_get(tparams->rx_meta_pool, reinterpret_cast<void**>(&burst)) < 0) {
        DAQIRI_LOG_CRITICAL("Running out of RX meta buffers due to high rates. Either increase "\
          "your number of metadata buffers (current: {}) with `rx_meta_buffers` (will "\
          "increase memory usage) or increase your `batch_size` for port {} queue {} (will "\
          "increase latency)", tparams->rx_meta_pool_size, tparams->port, tparams->queue);
        exit(1);
      }

      if (!initialize_rx_burst(burst,
                               tparams->port,
                               tparams->queue,
                               tparams->num_segs,
                               tparams->rx_burst_pool)) {
        rte_mempool_put(tparams->rx_meta_pool, burst);
        burst = nullptr;
        drop_pending_rx_mbufs(mbuf_arr,
                              cur_pkt_in_batch,
                              nb_rx,
                              tparams->port,
                              tparams->queue,
                              "no free RX burst metadata for split packet pointers");
        nb_rx = 0;
        cur_pkt_in_batch = 0;
        continue;
      }
    }

    // Check if we need to get more packets
    if (nb_rx == 0) {
      cur_pkt_in_batch = 0;
      nb_rx = rte_eth_rx_burst(tparams->port, tparams->queue,
                          reinterpret_cast<rte_mbuf**>(&mbuf_arr[0]), DEFAULT_NUM_RX_BURST);

      if (nb_rx == 0) {
        if (burst->hdr.hdr.num_pkts > 0 && timeout_cycles > 0) {
          const auto cur_cycles = rte_get_tsc_cycles();

          // We hit our timeout. Send the partial batch immediately
          if ((cur_cycles - last_cycles) > timeout_cycles) {
            rte_ring_enqueue(tparams->ring, reinterpret_cast<void*>(burst));
            last_cycles = cur_cycles;
            burst = nullptr;
          }
        }

        continue;
      }
    }

    // At this point we have some packets to copy either from a new batch or an existing one. Check
    // if we are finishing a batch or copying all packets that came in
    int to_copy = std::min(static_cast<size_t>(nb_rx),
                           tparams->batch_size - burst->hdr.hdr.num_pkts);
    int appended = 0;
    if (tparams->num_segs == 1) {
      memcpy(&burst->pkts[0][burst->hdr.hdr.num_pkts],
             &mbuf_arr[cur_pkt_in_batch],
             sizeof(rte_mbuf*) * to_copy);
      burst->hdr.hdr.num_pkts += to_copy;
      appended = to_copy;
    } else {  // Extra work when buffers are scattered
      for (int p = 0; p < to_copy; p++) {
        struct rte_mbuf* mbuf = mbuf_arr[cur_pkt_in_batch + p];
        const auto pkt_idx = static_cast<int>(burst->hdr.hdr.num_pkts);
        if (populate_split_packet_segments(
                burst, mbuf, pkt_idx, tparams->num_segs, tparams->port, tparams->queue)) {
          burst->hdr.hdr.num_pkts++;
          appended++;
          continue;
        }
        rte_pktmbuf_free(mbuf);
      }
    }

    cur_pkt_in_batch        += to_copy;
    nb_rx                   -= to_copy;
    total_pkts              += appended;

    if (burst->hdr.hdr.num_pkts == tparams->batch_size) {
      rte_ring_enqueue(tparams->ring, reinterpret_cast<void*>(burst));
      last_cycles = rte_get_tsc_cycles();
      burst = nullptr;
    } else if (burst->hdr.hdr.num_pkts > 0 && timeout_cycles > 0) {
      const auto cur_cycles = rte_get_tsc_cycles();

      // We hit our timeout. Send the partial batch immediately
      if ((cur_cycles - last_cycles) > timeout_cycles) {
        rte_ring_enqueue(tparams->ring, reinterpret_cast<void*>(burst));
        last_cycles = cur_cycles;
        burst = nullptr;
      }
    }
  } while (!force_quit.load());

  DAQIRI_LOG_INFO("Total packets received by application (port/queue {}/{}): {}",
                     tparams->port,
                     tparams->queue,
                     total_pkts);

  return 0;
}

int DpdkEngine::rx_lb_worker(void* arg) {
  RxWorkerParams* tparams = (RxWorkerParams*)arg;
  int ret = 0;
  uint64_t freq = rte_get_tsc_hz();
  uint64_t timeout_ticks = freq * 0.02;  // expect all packets within 20ms
  uint64_t total_pkts = 0;

  DAQIRI_LOG_INFO("Starting RX Loopback Core {}, port {}, queue {}, socket {}",
                    rte_lcore_id(),
                    tparams->port,
                    tparams->queue,
                    rte_socket_id());
  int nb_rx = 0;
  int to_copy = 0;
  int cur_pkt_in_batch = 0;
  //
  //  run loop
  //
  while (!force_quit.load()) {
    BurstParams* meta_burst;
    if (rte_mempool_get(tparams->rx_meta_pool, reinterpret_cast<void**>(&meta_burst)) < 0) {
      DAQIRI_LOG_ERROR("Processing function falling behind. No free buffers for metadata!");
      exit(1);
    }

    if (!initialize_rx_burst(meta_burst,
                             tparams->port,
                             tparams->queue,
                             tparams->num_segs,
                             tparams->rx_burst_pool)) {
      rte_mempool_put(tparams->rx_meta_pool, meta_burst);
      continue;
    }

    BurstParams* tx_burst;
    while (!force_quit.load()) {
      if (rte_ring_dequeue(tparams->lb_ring, reinterpret_cast<void**>(&tx_burst)) == 0) {
        meta_burst->hdr = tx_burst->hdr;
        for (int s = 0; s < tx_burst->hdr.hdr.num_segs; s++) {
          memcpy(&meta_burst->pkts[s][0], &tx_burst->pkts[s][0],
                sizeof(void*) * tx_burst->hdr.hdr.num_pkts);
        }

        total_pkts += tx_burst->hdr.hdr.num_pkts;

        // Free the metadata and rx burst buffers associated with the TX burst. This will *not*
        // free the packet mbufs since we want to avoid copying the data.
        for (int seg = 0; seg < tx_burst->hdr.hdr.num_segs; seg++) {
          rte_mempool_put(tparams->tx_burst_pool, (void*)tx_burst->pkts[seg]);
        }
        rte_mempool_put(tparams->tx_meta_pool, tx_burst);

        // mbufs are not freed yet. RX will free them as part of normal processing
        rte_ring_enqueue(tparams->ring, reinterpret_cast<void*>(meta_burst));
        break;
      }
    }
  }

  DAQIRI_LOG_INFO("Total packets received by application (port/queue {}/{}): {}",
                     tparams->port,
                     tparams->queue,
                     total_pkts);

  return 0;
}

int DpdkEngine::tx_core_worker(void* arg) {
  TxWorkerParams* tparams = (TxWorkerParams*)arg;
  uint64_t seq;
  uint64_t ttl_pkts_tx = 0;
  BurstParams* msg;
  int64_t bursts = 0;

  DAQIRI_LOG_INFO("Starting TX Core {}, port {}, queue {} socket {} using burst pool {} ring {}",
                    rte_lcore_id(),
                    tparams->port,
                    tparams->queue,
                    rte_socket_id(),
                    (void*)tparams->burst_pool,
                    (void*)tparams->ring);

  while (!force_quit.load()) {
    if (rte_ring_dequeue(tparams->ring, reinterpret_cast<void**>(&msg)) != 0) { continue; }

    // Scatter mode needs to chain all the buffers
    if (msg->hdr.hdr.num_segs > 1) {
      for (size_t p = 0; p < msg->hdr.hdr.num_pkts; p++) {
        for (int seg = 0; seg < msg->hdr.hdr.num_segs - 1; seg++) {
          auto* mbuf = reinterpret_cast<struct rte_mbuf*>(msg->pkts[seg][p]);
          mbuf->next = reinterpret_cast<struct rte_mbuf*>(msg->pkts[seg + 1][p]);
        }

        // The next pointer of the last segment should be nullptr
        reinterpret_cast<struct rte_mbuf*>(msg->pkts[msg->hdr.hdr.num_segs - 1][p])->next = nullptr;
        reinterpret_cast<struct rte_mbuf*>(msg->pkts[0][p])->nb_segs = msg->hdr.hdr.num_segs;
      }
    }

    // Packet pacing: meter this queue at (at most) pacing_mbps on average by
    // gating each burst behind the NIC SEND_ON_TIMESTAMP scheduler. Tag only the
    // first packet of the burst with a scheduled release time -- the NIC holds it
    // (one WAIT) until pace_next_ns and sends the rest of the burst right after,
    // so no per-packet schedule is needed. pace_next_ns then advances by the time
    // the burst's L2 frame bytes take at the configured rate.
    if (tparams->pacing_mbps > 0 && tparams->tx_ts_dynfield_offset >= 0 &&
        msg->hdr.hdr.num_pkts > 0) {
      const uint64_t now = tparams->engine->now_tx_ns(static_cast<uint16_t>(tparams->port));
      // Seed on the first burst, and never let an idle gap accrue send credit: if
      // the virtual clock has fallen behind real time, restart it at now so a
      // backlog cannot burst above the configured rate.
      if (tparams->pace_next_ns == 0 || tparams->pace_next_ns < now) {
        tparams->pace_next_ns = now;
        tparams->pace_rem = 0;
      }
      auto* m0 = reinterpret_cast<struct rte_mbuf*>(msg->pkts[0][0]);
      m0->ol_flags |= tparams->tx_ts_dynflag_mask;
      *RTE_MBUF_DYNFIELD(m0, tparams->tx_ts_dynfield_offset, uint64_t*) = tparams->pace_next_ns;
      // ns = bytes * 8 bits / (Mbps * 1e6 bits/s) * 1e9 ns/s = 8000 * bytes / Mbps.
      // Carry the division remainder so the long-run average is exactly pacing_mbps.
      // The set_*_packet_lengths helpers maintain hdr.nbytes as the burst's L2 byte
      // total; use it directly to avoid walking the burst. Fall back to a walk for
      // bursts populated without those helpers (nbytes left at 0).
      uint64_t bytes = msg->hdr.hdr.nbytes;
      if (bytes == 0) {
        for (size_t p = 0; p < msg->hdr.hdr.num_pkts; p++) {
          bytes += reinterpret_cast<struct rte_mbuf*>(msg->pkts[0][p])->pkt_len;
        }
      }
      const uint64_t numer = 8000ULL * bytes + tparams->pace_rem;
      tparams->pace_next_ns += numer / tparams->pacing_mbps;
      tparams->pace_rem = numer % tparams->pacing_mbps;
    }

    auto pkts_to_transmit = static_cast<int64_t>(msg->hdr.hdr.num_pkts);

    size_t pkts_tx = 0;
    while (pkts_tx != msg->hdr.hdr.num_pkts && !force_quit.load()) {
      auto to_send = static_cast<uint16_t>(
          std::min(static_cast<size_t>(DEFAULT_NUM_TX_BURST), msg->hdr.hdr.num_pkts - pkts_tx));

      // CPU-only or HDS mode
      int tx;
      tx = rte_eth_tx_burst(tparams->port,
                            tparams->queue,
                            reinterpret_cast<rte_mbuf**>(&msg->pkts[0][pkts_tx]),
                            to_send);

      pkts_tx += tx;
    }

    ttl_pkts_tx += pkts_tx;

    for (int seg = 0; seg < msg->hdr.hdr.num_segs; seg++) {
      rte_mempool_put(tparams->burst_pool, static_cast<void*>(msg->pkts[seg]));
    }

    rte_mempool_put(tparams->meta_pool, msg);
    bursts++;
  }

  DAQIRI_LOG_INFO("Total packets transmitted by application (port/queue {}/{}): {}",
                     tparams->port,
                     tparams->queue,
                     ttl_pkts_tx);

  return 0;
}

int DpdkEngine::tx_lb_worker(void* arg) {
  TxWorkerParams* tparams = (TxWorkerParams*)arg;
  uint64_t seq;
  uint64_t ttl_pkts_tx = 0;
  BurstParams* msg;
  int64_t bursts = 0;

  DAQIRI_LOG_INFO(
        "Starting TX Loopback Core {}, port {}, queue {} socket {} using burst pool {} ring {}",
        rte_lcore_id(),
        tparams->port,
        tparams->queue,
        rte_socket_id(),
        (void*)tparams->burst_pool,
        (void*)tparams->ring);

  while (!force_quit.load()) {
    if (rte_ring_dequeue(tparams->ring, reinterpret_cast<void**>(&msg)) != 0) {
      continue;
    }

    // Scatter mode needs to chain all the buffers
    if (msg->hdr.hdr.num_segs > 1) {
      for (size_t p = 0; p < msg->hdr.hdr.num_pkts; p++) {
        for (int seg = 0; seg < msg->hdr.hdr.num_segs - 1; seg++) {
          auto* mbuf = reinterpret_cast<struct rte_mbuf*>(msg->pkts[seg][p]);
          mbuf->next = reinterpret_cast<struct rte_mbuf*>(msg->pkts[seg + 1][p]);
        }

        reinterpret_cast<struct rte_mbuf*>(msg->pkts[0][p])->nb_segs = msg->hdr.hdr.num_segs;
      }
    }

    auto pkts_to_transmit = static_cast<int64_t>(msg->hdr.hdr.num_pkts);

    if (rte_ring_enqueue(tparams->lb_ring, reinterpret_cast<void*>(msg)) != 0) {
      DAQIRI_LOG_CRITICAL("Failed to enqueue TX work");
      break;
    }

    ttl_pkts_tx += msg->hdr.hdr.num_pkts;
  }

  DAQIRI_LOG_INFO("Total packets transmitted by application (port/queue {}/{}): {}",
                     tparams->port,
                     tparams->queue,
                     ttl_pkts_tx);

  return 0;
}

/* daqiri interface implementations */
void* DpdkEngine::get_segment_packet_ptr(BurstParams* burst, int seg, int idx) {
  const auto limits = burst_validation::header_limits(burst);
  if ((burst == nullptr) ||
      burst_validation::validate_segment_packet_storage(
          burst, limits, seg, idx, true, false, "DpdkEngine::get_segment_packet_ptr")
          != Status::SUCCESS) {
    return nullptr;
  }
  if ((burst->hdr.hdr.burst_flags & kBurstFlagDpdkReordered) != 0U) {
    if (seg != 0) { return nullptr; }
    return burst->pkts[0][idx];
  }
  return rte_pktmbuf_mtod(reinterpret_cast<rte_mbuf*>(burst->pkts[seg][idx]), void*);
}

void* DpdkEngine::get_packet_ptr(BurstParams* burst, int idx) {
  return get_segment_packet_ptr(burst, 0, idx);
}

uint32_t DpdkEngine::get_segment_packet_length(BurstParams* burst, int seg, int idx) {
  if (burst == nullptr) { return 0; }
  const auto limits = burst_validation::header_limits(burst);
  if ((burst->hdr.hdr.burst_flags & kBurstFlagDpdkReordered) != 0U) {
    if (seg != 0 ||
        burst_validation::validate_segment_packet_storage(
            burst, limits, seg, idx, false, true, "DpdkEngine::get_segment_packet_length")
            != Status::SUCCESS) {
      return 0;
    }
    return burst->pkt_lens[0][idx];
  }
  if (burst_validation::validate_segment_packet_storage(
          burst, limits, seg, idx, true, false, "DpdkEngine::get_segment_packet_length")
      != Status::SUCCESS) {
    return 0;
  }
  return reinterpret_cast<rte_mbuf*>(burst->pkts[seg][idx])->data_len;
}

uint32_t DpdkEngine::get_packet_length(BurstParams* burst, int idx) {
  if (burst == nullptr) { return 0; }
  const auto limits = burst_validation::header_limits(burst);
  if ((burst->hdr.hdr.burst_flags & kBurstFlagDpdkReordered) != 0U) {
    if (burst_validation::validate_segment_packet_storage(
            burst, limits, 0, idx, false, true, "DpdkEngine::get_packet_length")
        != Status::SUCCESS) {
      return 0;
    }
    return burst->pkt_lens[0][idx];
  }
  if (burst_validation::validate_segment_packet_storage(
          burst, limits, 0, idx, true, false, "DpdkEngine::get_packet_length")
      != Status::SUCCESS) {
    return 0;
  }
  return reinterpret_cast<rte_mbuf*>(burst->pkts[0][idx])->pkt_len;
}

FlowId DpdkEngine::get_packet_flow_id(BurstParams* burst, int idx) {
  if (burst == nullptr) { return 0; }
  if ((burst->hdr.hdr.burst_flags & kBurstFlagDpdkReordered) != 0U) { return 0; }
  if (burst_validation::validate_segment_packet_storage(
          burst,
          burst_validation::header_limits(burst),
          0,
          idx,
          true,
          false,
          "DpdkEngine::get_packet_flow_id") != Status::SUCCESS) {
    return 0;
  }
  const auto* mbuf = reinterpret_cast<const rte_mbuf*>(burst->pkts[0][idx]);
  if (mbuf == nullptr || (mbuf->ol_flags & RTE_MBUF_F_RX_FDIR_ID) == 0) { return 0; }
  return mbuf->hash.fdir.hi;
}

Status DpdkEngine::get_packet_rx_timestamp(BurstParams* burst, int idx, uint64_t* timestamp_ns) {
  if (burst == nullptr || timestamp_ns == nullptr) { return Status::NULL_PTR; }
  *timestamp_ns = 0;

  if ((burst->hdr.hdr.burst_flags & kBurstFlagDpdkReordered) != 0U) {
    Status status = burst_validation::validate_packet_index(
        burst, burst_validation::header_limits(burst), idx, "DpdkEngine::get_packet_rx_timestamp");
    if (status != Status::SUCCESS) { return status; }
    if (idx != 0 || burst->custom_pkt_data == nullptr) { return Status::INVALID_PARAMETER; }
    auto ctx = std::static_pointer_cast<ReorderBurstContext>(burst->custom_pkt_data);
    if (!ctx || !ctx->rx_timestamp_ns_valid) { return Status::NOT_SUPPORTED; }
    *timestamp_ns = ctx->rx_timestamp_ns;
    return Status::SUCCESS;
  }

  Status status = burst_validation::validate_segment_packet_storage(
      burst,
      burst_validation::header_limits(burst),
      0,
      idx,
      true,
      false,
      "DpdkEngine::get_packet_rx_timestamp");
  if (status != Status::SUCCESS) { return status; }
  auto* mbuf = reinterpret_cast<rte_mbuf*>(burst->pkts[0][idx]);
  if (mbuf == nullptr || burst->hdr.hdr.port_id >= rx_timestamp_conversions_.size()) {
    return Status::INVALID_PARAMETER;
  }

  if (!extract_mbuf_rx_timestamp_ns(mbuf,
                                    timestamp_dynfield_offset_,
                                    rx_timestamp_dynflag_mask_,
                                    rx_timestamp_conversions_[burst->hdr.hdr.port_id],
                                    timestamp_ns)) {
    return Status::NOT_SUPPORTED;
  }
  return Status::SUCCESS;
}

Status DpdkEngine::set_packet_tx_time(BurstParams* burst, int idx, uint64_t timestamp) {
  if (burst == nullptr) { return Status::NULL_PTR; }
  Status status = burst_validation::validate_segment_packet_storage(
      burst,
      burst_validation::header_limits(burst),
      0,
      idx,
      true,
      false,
      "DpdkEngine::set_packet_tx_time");
  if (status != Status::SUCCESS) { return status; }
  if (timestamp_dynfield_offset_ < 0 || tx_timestamp_dynflag_mask_ == 0) {
    return Status::NOT_SUPPORTED;
  }
  reinterpret_cast<struct rte_mbuf**>(burst->pkts[0])[idx]->ol_flags |= tx_timestamp_dynflag_mask_;
  *RTE_MBUF_DYNFIELD(
      reinterpret_cast<rte_mbuf**>(burst->pkts[0])[idx],
      timestamp_dynfield_offset_,
      uint64_t*) = timestamp;

  return Status::SUCCESS;
}

//  The number of RX can differ from the configured queues in TX-only mode where we need to create
//  fake RX queues.
uint16_t DpdkEngine::get_num_rx_queues(int port_id) const {
  return port_q_num.at(static_cast<uint16_t>(port_id)).first;
}

void* DpdkEngine::get_packet_extra_info(BurstParams* burst, int idx) {
  if (burst == nullptr || idx < 0) { return nullptr; }
  if ((burst->hdr.hdr.burst_flags & kBurstFlagDpdkReordered) != 0U) { return nullptr; }
  return nullptr;
}

Status DpdkEngine::get_tx_packet_burst(BurstParams* burst) {
  if (burst == nullptr) { return Status::NULL_PTR; }
  const auto limits = burst_validation::header_limits(burst);
  Status status = burst_validation::validate_segment_count(
      burst, limits, "DpdkEngine::get_tx_packet_burst");
  if (status != Status::SUCCESS) { return status; }
  status = burst_validation::validate_packet_count(
      burst,
      limits,
      static_cast<size_t>(std::numeric_limits<int>::max()),
      "DpdkEngine::get_tx_packet_burst");
  if (status != Status::SUCCESS) { return status; }

  const uint32_t key = generate_queue_key(burst->hdr.hdr.port_id, burst->hdr.hdr.q_id);
  const auto q_it = tx_dpdk_q_map_.find(key);
  if (q_it == tx_dpdk_q_map_.end() || q_it->second == nullptr) {
    DAQIRI_LOG_ERROR("Invalid TX queue key {} for port {} queue {}",
                     key,
                     burst->hdr.hdr.port_id,
                     burst->hdr.hdr.q_id);
    return Status::INVALID_PARAMETER;
  }
  const auto& q = q_it->second;
  if (q->pools.size() < static_cast<size_t>(limits.num_segs)) {
    DAQIRI_LOG_ERROR("TX queue has {} pool(s), burst requires {} segment(s)",
                     q->pools.size(),
                     limits.num_segs);
    return Status::INVALID_PARAMETER;
  }

  const auto burst_pool = tx_burst_buffers.find(key);
  if (burst_pool == tx_burst_buffers.end()) {
    DAQIRI_LOG_ERROR("Failed to look up burst pool name for port {} queue {}",
                       burst->hdr.hdr.port_id,
                       burst->hdr.hdr.q_id);
    return Status::NO_FREE_BURST_BUFFERS;
  }

  for (int seg = 0; seg < burst->hdr.hdr.num_segs; seg++) {
    if (rte_mempool_get(burst_pool->second, reinterpret_cast<void**>(&burst->pkts[seg])) != 0) {
      return Status::NO_FREE_BURST_BUFFERS;
    }

    if (rte_pktmbuf_alloc_bulk(q->pools[seg],
                               reinterpret_cast<rte_mbuf**>(burst->pkts[seg]),
                               static_cast<int>(burst->hdr.hdr.num_pkts)) != 0) {
      rte_mempool_put(burst_pool->second, reinterpret_cast<void*>(burst->pkts[seg]));
      return Status::NO_FREE_PACKET_BUFFERS;
    }
  }

  return Status::SUCCESS;
}

Status DpdkEngine::set_eth_header(BurstParams* burst, int idx, char* dst_addr) {
  if (dst_addr == nullptr) { return Status::NULL_PTR; }
  Status status = validate_dpdk_first_segment_write(
      burst, idx, sizeof(UDPPkt), "DpdkEngine::set_eth_header");
  if (status != Status::SUCCESS) { return status; }

  auto mbuf = reinterpret_cast<rte_mbuf*>(burst->pkts[0][idx]);
  auto mbuf_data = rte_pktmbuf_mtod(mbuf, UDPPkt*);
  memcpy(reinterpret_cast<void*>(&mbuf_data->eth.dst_addr),
         reinterpret_cast<void*>(dst_addr),
         sizeof(mbuf_data->eth.dst_addr));

  mbuf_data->eth.ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
  return Status::SUCCESS;
}

Status DpdkEngine::set_ipv4_header(BurstParams* burst, int idx, int ip_len, uint8_t proto,
                                   unsigned int src_host, unsigned int dst_host) {
  if (ip_len < 0 ||
      static_cast<size_t>(ip_len) >
          std::numeric_limits<uint16_t>::max() - sizeof(rte_ipv4_hdr)) {
    return Status::INVALID_PARAMETER;
  }
  Status status = validate_dpdk_first_segment_write(
      burst, idx, sizeof(UDPPkt), "DpdkEngine::set_ipv4_header");
  if (status != Status::SUCCESS) { return status; }

  auto mbuf = reinterpret_cast<rte_mbuf*>(burst->pkts[0][idx]);
  auto mbuf_data = rte_pktmbuf_mtod(mbuf, UDPPkt*);
  mbuf_data->ip.next_proto_id = proto;
  mbuf_data->ip.ihl = 5;
  mbuf_data->ip.total_length = rte_cpu_to_be_16(sizeof(mbuf_data->ip) + ip_len);
  mbuf_data->ip.version = 4;
  mbuf_data->ip.src_addr = htonl(src_host);
  mbuf_data->ip.dst_addr = htonl(dst_host);
  return Status::SUCCESS;
}

Status DpdkEngine::set_udp_header(BurstParams* burst, int idx, int udp_len, uint16_t src_port,
                                  uint16_t dst_port) {
  if (udp_len < 0 ||
      static_cast<size_t>(udp_len) >
          std::numeric_limits<uint16_t>::max() - sizeof(rte_udp_hdr)) {
    return Status::INVALID_PARAMETER;
  }
  Status status = validate_dpdk_first_segment_write(
      burst, idx, sizeof(UDPPkt), "DpdkEngine::set_udp_header");
  if (status != Status::SUCCESS) { return status; }

  auto mbuf = reinterpret_cast<rte_mbuf*>(burst->pkts[0][idx]);
  auto mbuf_data = rte_pktmbuf_mtod(mbuf, UDPPkt*);

  mbuf_data->udp.dgram_cksum = 0;
  mbuf_data->udp.src_port = htons(src_port);
  mbuf_data->udp.dst_port = htons(dst_port);
  mbuf_data->udp.dgram_len = htons(udp_len + sizeof(mbuf_data->udp));
  return Status::SUCCESS;
}

Status DpdkEngine::set_udp_payload(BurstParams* burst, int idx, void* data, int len) {
  size_t payload_capacity = burst_validation::kUnknownCapacity;
  bool header_fits = true;
  if (burst_validation::strict_enabled() && burst != nullptr &&
      burst_validation::validate_segment_packet_storage(
          burst,
          burst_validation::header_limits(burst),
          0,
          idx,
          true,
          false,
          "DpdkEngine::set_udp_payload")
          == Status::SUCCESS) {
    const auto* mbuf = reinterpret_cast<const rte_mbuf*>(burst->pkts[0][idx]);
    const size_t capacity = mbuf_data_capacity(mbuf);
    header_fits = capacity >= sizeof(UDPPkt);
    payload_capacity = capacity >= sizeof(UDPPkt) ? capacity - sizeof(UDPPkt) : 0;
  }
  Status status = burst_validation::validate_payload_write(
      burst,
      burst_validation::header_limits(burst),
      idx,
      data,
      len,
      payload_capacity,
      "DpdkEngine::set_udp_payload");
  if (status != Status::SUCCESS) { return status; }
  if (!header_fits) { return Status::INVALID_PARAMETER; }

  auto mbuf = reinterpret_cast<rte_mbuf*>(burst->pkts[0][idx]);
  auto mbuf_data = rte_pktmbuf_mtod(mbuf, UDPPkt*);

  if (len > 0) { rte_memcpy(mbuf_data->payload, data, len); }
  return Status::SUCCESS;
}

bool DpdkEngine::is_tx_burst_available(BurstParams* burst) {
  if (burst == nullptr) { return false; }
  const auto limits = burst_validation::header_limits(burst);
  if (burst_validation::validate_segment_count(
          burst, limits, "DpdkEngine::is_tx_burst_available") != Status::SUCCESS) {
    return false;
  }
  if (burst_validation::validate_packet_count(
          burst,
          limits,
          static_cast<size_t>(std::numeric_limits<int>::max()),
          "DpdkEngine::is_tx_burst_available") != Status::SUCCESS) {
    return false;
  }
  const uint32_t key = generate_queue_key(burst->hdr.hdr.port_id, burst->hdr.hdr.q_id);
  const auto item = tx_dpdk_q_map_.find(key);
  if (item == tx_dpdk_q_map_.end() ||
      item->second->pools.size() < static_cast<size_t>(limits.num_segs)) {
    return false;
  }

  const auto& q = item->second;
  for (int seg = 0; seg < burst->hdr.hdr.num_segs; seg++) {
    if (rte_mempool_avail_count(q->pools[seg]) < burst->hdr.hdr.num_pkts * 2) { return false; }
  }

  return true;
}

Status DpdkEngine::set_packet_lengths(BurstParams* burst, int idx,
                                   const std::initializer_list<int>& lens) {
  uint32_t ttl_len = 0;
  Status status = validate_dpdk_packet_lengths(
      burst, idx, lens, &ttl_len, "DpdkEngine::set_packet_lengths");
  if (status != Status::SUCCESS) { return status; }

  int seg_idx = 0;
  for (int seg = 0; seg < burst->hdr.hdr.num_segs; seg++) {
    reinterpret_cast<rte_mbuf**>(burst->pkts[seg])[idx]->data_len = *(lens.begin() + seg_idx);
    ++seg_idx;
  }

  reinterpret_cast<rte_mbuf**>(burst->pkts[0])[idx]->pkt_len = ttl_len;
  // Accumulate the burst's L2 byte total so TX pacing need not walk the burst.
  burst->hdr.hdr.nbytes += ttl_len;

  return Status::SUCCESS;
}

Status DpdkEngine::set_all_packet_lengths(BurstParams* burst,
                                       const std::initializer_list<int>& lens) {
  if (burst == nullptr) { return Status::NULL_PTR; }
  if (burst_validation::validate_segment_count(
          burst,
          burst_validation::header_limits(burst),
          "DpdkEngine::set_all_packet_lengths") != Status::SUCCESS) {
    return Status::INVALID_PARAMETER;
  }
  if (burst->hdr.hdr.num_pkts == 0 ||
      burst->hdr.hdr.num_pkts > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return Status::INVALID_PARAMETER;
  }

  std::array<uint32_t, MAX_NUM_SEGS> seg_lens{};
  uint32_t ttl_len = 0;
  const auto num_pkts = static_cast<int>(burst->hdr.hdr.num_pkts);
  for (int idx = 0; idx < num_pkts; ++idx) {
    uint32_t packet_len = 0;
    Status status = validate_dpdk_packet_lengths(
        burst, idx, lens, &packet_len, "DpdkEngine::set_all_packet_lengths");
    if (status != Status::SUCCESS) { return status; }
    ttl_len = packet_len;
  }

  int lens_idx = 0;
  for (int seg = 0; seg < burst->hdr.hdr.num_segs; ++seg) {
    seg_lens[seg] = static_cast<uint32_t>(*(lens.begin() + lens_idx));
    ++lens_idx;
  }

  for (int seg = 0; seg < burst->hdr.hdr.num_segs; ++seg) {
    auto** mbufs = reinterpret_cast<rte_mbuf**>(burst->pkts[seg]);
    for (int idx = 0; idx < num_pkts; ++idx) { mbufs[idx]->data_len = seg_lens[seg]; }
  }

  auto** first_seg = reinterpret_cast<rte_mbuf**>(burst->pkts[0]);
  for (int idx = 0; idx < num_pkts; ++idx) { first_seg[idx]->pkt_len = ttl_len; }
  // Record the burst's L2 byte total so TX pacing need not walk the burst.
  burst->hdr.hdr.nbytes = static_cast<uint64_t>(ttl_len) * static_cast<uint64_t>(num_pkts);

  return Status::SUCCESS;
}

void DpdkEngine::free_packet_segment(BurstParams* burst, int seg, int pkt) {
  if (burst == nullptr) { return; }
  if ((burst->hdr.hdr.burst_flags & kBurstFlagDpdkReordered) != 0U) {
    release_reorder_output_context(burst);
    if (seg == 0 && burst->pkt_lens[0] != nullptr &&
        burst_validation::validate_packet_index(
            burst,
            burst_validation::header_limits(burst),
            pkt,
            "DpdkEngine::free_packet_segment") == Status::SUCCESS) {
      burst->pkt_lens[0][pkt] = 0;
    }
    return;
  }
  if (burst_validation::validate_segment_packet_storage(
          burst,
          burst_validation::header_limits(burst),
          seg,
          pkt,
          true,
          false,
          "DpdkEngine::free_packet_segment") != Status::SUCCESS) {
    return;
  }
  rte_pktmbuf_free_seg(reinterpret_cast<rte_mbuf**>(burst->pkts[seg])[pkt]);
}

void DpdkEngine::free_all_segment_packets(BurstParams* burst, int seg) {
  if (burst == nullptr) { return; }
  const auto limits = burst_validation::header_limits(burst);
  if ((burst->hdr.hdr.burst_flags & kBurstFlagDpdkReordered) != 0U) {
    release_reorder_output_context(burst);
    if (seg == 0 && burst->pkt_lens[0] != nullptr &&
        burst_validation::validate_packet_count(
            burst,
            limits,
            static_cast<size_t>(std::numeric_limits<int>::max()),
            "DpdkEngine::free_all_segment_packets") == Status::SUCCESS) {
      for (int p = 0; p < static_cast<int>(burst->hdr.hdr.num_pkts); p++) {
        burst->pkt_lens[0][p] = 0;
      }
    }
    return;
  }
  if (burst_validation::validate_segment_index(
          burst, limits, seg, "DpdkEngine::free_all_segment_packets") != Status::SUCCESS ||
      burst_validation::validate_packet_count(
          burst,
          limits,
          static_cast<size_t>(std::numeric_limits<int>::max()),
          "DpdkEngine::free_all_segment_packets") != Status::SUCCESS ||
      burst->pkts[seg] == nullptr) {
    return;
  }
  for (int p = 0; p < static_cast<int>(burst->hdr.hdr.num_pkts); p++) {
    rte_pktmbuf_free_seg(reinterpret_cast<rte_mbuf**>(burst->pkts[seg])[p]);
  }
}

void DpdkEngine::free_packet(BurstParams* burst, int pkt) {
  if (burst == nullptr) { return; }
  if ((burst->hdr.hdr.burst_flags & kBurstFlagDpdkReordered) != 0U) {
    release_reorder_output_context(burst);
    if (burst->pkt_lens[0] != nullptr &&
        burst_validation::validate_packet_index(
            burst, burst_validation::header_limits(burst), pkt, "DpdkEngine::free_packet") ==
            Status::SUCCESS) {
      burst->pkt_lens[0][pkt] = 0;
    }
    return;
  }
  if (burst_validation::validate_segment_packet_storage(
          burst,
          burst_validation::header_limits(burst),
          0,
          pkt,
          true,
          false,
          "DpdkEngine::free_packet") != Status::SUCCESS) {
    return;
  }
  rte_pktmbuf_free(reinterpret_cast<rte_mbuf**>(burst->pkts[0])[pkt]);
}

void DpdkEngine::free_all_packets(BurstParams* burst) {
  if (burst == nullptr) { return; }
  const auto limits = burst_validation::header_limits(burst);
  if ((burst->hdr.hdr.burst_flags & kBurstFlagDpdkReordered) != 0U) {
    release_reorder_output_context(burst);
    if (burst->pkt_lens[0] != nullptr &&
        burst_validation::validate_packet_count(
            burst,
            limits,
            static_cast<size_t>(std::numeric_limits<int>::max()),
            "DpdkEngine::free_all_packets") == Status::SUCCESS) {
      for (int p = 0; p < static_cast<int>(burst->hdr.hdr.num_pkts); p++) {
        burst->pkt_lens[0][p] = 0;
      }
    }
    return;
  }
  if (burst_validation::validate_packet_count(
          burst,
          limits,
          static_cast<size_t>(std::numeric_limits<int>::max()),
          "DpdkEngine::free_all_packets") != Status::SUCCESS ||
      burst->pkts[0] == nullptr) {
    return;
  }
  for (int p = 0; p < static_cast<int>(burst->hdr.hdr.num_pkts); p++) {
    rte_pktmbuf_free(reinterpret_cast<rte_mbuf**>(burst->pkts[0])[p]);
  }
}

void DpdkEngine::free_rx_burst(BurstParams* burst) {
  if (burst == nullptr) { return; }
  if ((burst->hdr.hdr.burst_flags & kBurstFlagDpdkReordered) != 0U) {
    release_reorder_output_context(burst);

    for (int seg = 0; seg < MAX_NUM_SEGS; ++seg) {
      burst->pkts[seg] = nullptr;
      burst->pkt_lens[seg] = nullptr;
    }
    burst->custom_pkt_data.reset();
    delete burst;
    return;
  }

  const auto limits = burst_validation::header_limits(burst);
  if (burst_validation::validate_segment_count(
          burst, limits, "DpdkEngine::free_rx_burst") == Status::SUCCESS) {
    for (int seg = 0; seg < burst->hdr.hdr.num_segs; seg++) {
      if (burst->pkts[seg] != nullptr) {
        rte_mempool_put(rx_burst_buffer, (void*)burst->pkts[seg]);
      }
    }
  }

  burst->hdr.hdr.num_pkts = 0;
  burst->pkt_extra_info = nullptr;
  rte_mempool_put(rx_metadata, burst);
}

void DpdkEngine::free_tx_burst(BurstParams* burst) {
  if (burst == nullptr) { return; }
  const uint32_t key = generate_queue_key(burst->hdr.hdr.port_id, burst->hdr.hdr.q_id);
  const auto burst_pool = tx_burst_buffers.find(key);

  const auto limits = burst_validation::header_limits(burst);
  if (burst_pool != tx_burst_buffers.end() &&
      burst_validation::validate_segment_count(
          burst, limits, "DpdkEngine::free_tx_burst") == Status::SUCCESS) {
    for (int seg = 0; seg < burst->hdr.hdr.num_segs; seg++) {
      if (burst->pkts[seg] != nullptr) {
        rte_mempool_put(burst_pool->second, (void*)burst->pkts[seg]);
      }
    }
  }

  burst->hdr.hdr.num_pkts = 0;
  rte_mempool_put(tx_metadata, burst);
}

Status DpdkEngine::get_rx_burst(BurstParams** burst, int port, int q) {
  uint32_t key = generate_queue_key(port, q);
  const auto reorder_it = reorder_queue_states_.find(key);
  if (reorder_it != reorder_queue_states_.end() && reorder_it->second.enabled) {
    std::lock_guard<std::mutex> guard(reorder_lock_);
    return get_next_output_or_ready(key, reorder_it->second, burst);
  }

  const auto ring_it = rx_rings.find(key);
  if (ring_it == rx_rings.end()) {
    DAQIRI_LOG_ERROR("Invalid port/queue combination in get_rx_burst: {}/{}", port, q);
    return Status::INVALID_PARAMETER;
  }
  if (rte_ring_dequeue(ring_it->second, reinterpret_cast<void**>(burst)) < 0) {
    return Status::NOT_READY;
  }
  return Status::SUCCESS;
}

void DpdkEngine::free_rx_metadata(BurstParams* burst) {
  if (burst != nullptr && (burst->hdr.hdr.burst_flags & kBurstFlagDpdkReordered) != 0U) {
    free_rx_burst(burst);
    return;
  }
  rte_mempool_put(rx_metadata, burst);
}

void DpdkEngine::free_tx_metadata(BurstParams* burst) {
  rte_mempool_put(tx_metadata, burst);
}

Status DpdkEngine::get_tx_metadata_buffer(BurstParams** burst) {
  if (rte_mempool_get(tx_metadata, reinterpret_cast<void**>(burst)) != 0) {
    DAQIRI_LOG_CRITICAL("Running out of TX meta buffers due to high rates. Either increase "\
      "your number of metadata buffers (current: {}) with `tx_meta_buffers` (will "\
      "increase memory usage) or increase your `batch_size` for port {} queue {} (will increase "\
      "latency)", cfg_.tx_meta_buffers_, (*burst)->hdr.hdr.port_id, (*burst)->hdr.hdr.q_id);
    return Status::NO_FREE_BURST_BUFFERS;
  }
  // Clear the recycled running L2 byte total (see create_tx_burst_params).
  (*burst)->hdr.hdr.nbytes = 0;

  return Status::SUCCESS;
}

Status DpdkEngine::send_tx_burst(BurstParams* burst) {
  if (burst == nullptr) { return Status::NULL_PTR; }
  const auto limits = burst_validation::header_limits(burst);
  Status status = burst_validation::validate_segment_count(
      burst, limits, "DpdkEngine::send_tx_burst");
  if (status != Status::SUCCESS) { return status; }
  status = burst_validation::validate_packet_count(
      burst,
      limits,
      static_cast<size_t>(std::numeric_limits<int>::max()),
      "DpdkEngine::send_tx_burst");
  if (status != Status::SUCCESS) { return status; }

  uint32_t key = generate_queue_key(burst->hdr.hdr.port_id, burst->hdr.hdr.q_id);
  const auto ring = tx_rings.find(key);

  if (ring == tx_rings.end()) {
    DAQIRI_LOG_ERROR("Invalid port/queue combination in send_tx_burst: {}/{}",
                       burst->hdr.hdr.port_id,
                       burst->hdr.hdr.q_id);
    return Status::INVALID_PARAMETER;
  }

  if (rte_ring_enqueue(ring->second, reinterpret_cast<void*>(burst)) != 0) {
    free_unsubmitted_tx_packets(burst);
    free_tx_burst(burst);
    DAQIRI_LOG_CRITICAL("Failed to enqueue TX work");
    return Status::NO_SPACE_AVAILABLE;
  }

  return Status::SUCCESS;
}

void DpdkEngine::shutdown() {
  // Idempotency guard: shutdown() may be invoked a second time via ~DpdkEngine
  // during C++ __cxa_finalize, by which point the spdlog default logger has
  // already been destroyed and any DAQIRI_LOG_INFO here crashes inside
  // spdlog::sink_it_. Skip the body (and the log calls) if already torn down.
  if (!initialized_) { return; }
  DAQIRI_LOG_INFO("daqiri DPDK engine shutdown called {}", num_init);

  if (--num_init == 0) {
    DAQIRI_LOG_INFO("daqiri DPDK engine shutting down");
    cleanup_reorder_state();
    force_quit.store(true);

    stats_.Shutdown();
    stats_thread_.join();

    // Release DPDK resources BEFORE rte_eal_cleanup(). Ring/mempool pointers
    // are owned by EAL memzones and become invalid once cleanup runs, so the
    // destructor must not touch them after this point.
    for (auto const& [key, val] : rx_rings) {
      if (val != nullptr) { rte_ring_free(val); }
    }
    rx_rings.clear();
    for (auto const& [key, val] : tx_rings) {
      if (val != nullptr) { rte_ring_free(val); }
    }
    tx_rings.clear();

    cleanup_dynamic_flows();
    destroy_all_flow_rules();
    cleanup_eal();
    initialized_ = false;
  }
}

void DpdkEngine::print_stats() {
  DAQIRI_LOG_INFO("daqiri DPDK engine stats");
  if (loopback_ == LoopbackType::LOOPBACK_TYPE_SW) {
    return;
  }
  int portid;
  RTE_ETH_FOREACH_DEV(portid) {
    PrintDpdkStats(portid);
  }
}

uint64_t DpdkEngine::get_burst_tot_byte(BurstParams* burst) {
  if (burst == nullptr) { return 0; }
  const auto limits = burst_validation::header_limits(burst);
  if (burst_validation::validate_packet_count(
          burst,
          limits,
          static_cast<size_t>(std::numeric_limits<int>::max()),
          "DpdkEngine::get_burst_tot_byte") != Status::SUCCESS) {
    return 0;
  }
  uint64_t total = 0;
  if ((burst->hdr.hdr.burst_flags & kBurstFlagDpdkReordered) != 0U) {
    if (burst->pkt_lens[0] != nullptr) {
      for (int i = 0; i < static_cast<int>(burst->hdr.hdr.num_pkts); ++i) {
        total += burst->pkt_lens[0][i];
      }
    }
    return total;
  }

  for (int i = 0; i < static_cast<int>(burst->hdr.hdr.num_pkts); ++i) {
    total += get_packet_length(burst, i);
  }
  return total;
}

BurstParams* DpdkEngine::create_tx_burst_params() {
  BurstParams* burst = nullptr;
  if (rte_mempool_get(tx_metadata, reinterpret_cast<void**>(&burst)) != 0) {
    DAQIRI_LOG_CRITICAL("Failed to get TX meta descriptor");
    return nullptr;
  }
  // TX metadata buffers are recycled from a mempool, so clear the running L2
  // byte total here (the start of a burst's lifecycle). set_header also resets
  // it, but doing it on acquire keeps the pacing path's "nbytes == 0 means not
  // yet accumulated" sentinel correct even if a caller fills lengths without
  // calling set_header first.
  burst->hdr.hdr.nbytes = 0;
  return burst;
}
};  // namespace daqiri
