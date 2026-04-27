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

#include <atomic>
#include <algorithm>
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
#include <unistd.h>
#include <arpa/inet.h>
#include <cstring>
#include <memory>
#include <utility>

#include "src/dpdk_log.h"
#include "daqiri_dpdk_mgr.h"
#include "src/kernels.h"
#include "src/logging.hpp"

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

// --- End Helper Functions ---


std::atomic<bool> force_quit = false;

// Used to signal to RX threads to flush all existing packets.
// Defaults to seq_cst, so no fences needed.
std::atomic<bool> flush_rx_queues = false;

struct TxWorkerParams {
  int port;
  int queue;
  uint32_t batch_size;
  struct rte_ring* ring;
  struct rte_ring* lb_ring;         // Ring used between TX and RX when in SW loopback mode
  struct rte_mempool* meta_pool;
  struct rte_mempool* burst_pool;
  struct rte_ether_addr mac_addr;
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
  struct rte_mempool* flowid_pool;
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
  struct rte_mempool* flowid_pool;
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

struct ExtraRxPacketInfo {
  uint16_t flow_id;
};

bool DpdkMgr::init_reorder_queue_state(const InterfaceConfig& intf, const RxQueueConfig& qcfg) {
  if (intf.rx_.reorder_configs_.empty()) { return true; }

  const auto key = generate_queue_key(intf.port_id_, qcfg.common_.id_);
  ReorderQueueState qstate;

  std::unordered_map<uint16_t, uint16_t> flow_id_to_queue;
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
    std::vector<uint16_t> queue_flow_ids;
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
    const uint64_t min_required_out =
        static_cast<uint64_t>(slot_stride) * static_cast<uint64_t>(packets_per_batch);
    if (min_required_out > out_mr.buf_size_) {
      DAQIRI_LOG_ERROR(
          "Reorder output MR '{}' buffer size {} is too small for config '{}' "
          "(required at least {} = packets_per_batch {} * slot_stride {})",
          out_mr.name_,
          out_mr.buf_size_,
          reorder_cfg.name_,
          min_required_out,
          packets_per_batch,
          slot_stride);
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

bool DpdkMgr::init_reorder_state() {
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

void DpdkMgr::cleanup_reorder_state() {
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

Status DpdkMgr::acquire_reorder_output_buffer(ReorderPlanRuntime& plan,
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

void DpdkMgr::release_reorder_output_buffer(std::shared_ptr<ReorderOutputPool> output_pool,
                                            size_t buffer_idx) {
  if (output_pool == nullptr || buffer_idx >= output_pool->buffers.size()) { return; }

  auto& buffer = output_pool->buffers[buffer_idx];
  buffer.consumer_done = true;
  if (buffer.event == nullptr) {
    buffer.event_complete = true;
  }
}

Status DpdkMgr::poll_reorder_events(ReorderPlanRuntime& plan) {
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

size_t DpdkMgr::append_reorder_packet(ReorderPlanRuntime& plan,
                                      struct rte_mbuf* mbuf,
                                      void* pkt_ptr,
                                      uint64_t now_cycles) {
  auto& batch = plan.direct_arrival_batch;
  const uint32_t packet_idx = batch.packet_count;
  if (packet_idx == 0) {
    batch.first_packet_cycles = now_cycles;
    const uint32_t pkt_len = mbuf != nullptr ? mbuf->pkt_len : 0U;
    uint32_t copy_len = 0;
    if (pkt_len > plan.payload_byte_offset) { copy_len = pkt_len - plan.payload_byte_offset; }
    batch.payload_len = std::min(copy_len, plan.slot_stride);
  }

  plan.h_source_mbufs[packet_idx] = mbuf;
  plan.h_input_ptrs[packet_idx] = pkt_ptr;
  batch.packet_count = packet_idx + 1U;
  return batch.packet_count;
}

Status DpdkMgr::create_reorder_output_burst(ReorderPlanRuntime& plan,
                                            std::shared_ptr<ReorderOutputPool> output_pool,
                                            size_t buffer_idx,
                                            void* output_buffer,
                                            uint32_t aggregate_len,
                                            uint32_t source_packet_count,
                                            uint32_t payload_len,
                                            uint64_t batch_id,
                                            bool batch_id_ready,
                                            const uint64_t* h_batch_id,
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

Status DpdkMgr::flush_reorder_batch(ReorderPlanRuntime& plan,
                                    uint32_t batch_id,
                                    bool timeout_flush,
                                    BurstParams** out_burst) {
  if (out_burst == nullptr) { return Status::NULL_PTR; }
  *out_burst = nullptr;
  (void)batch_id;

  auto* batch = &plan.direct_arrival_batch;

  if (batch->packet_count == 0) {
    batch->first_packet_cycles = 0;
    batch->payload_len = 0;
    return Status::SUCCESS;
  }

  const uint32_t payload_len = batch->payload_len;

  const uint32_t num_pkts = batch->packet_count;
  const uint32_t output_slots = plan.packets_per_batch;
  const uint64_t aggregate_len64 =
      static_cast<uint64_t>(output_slots) * static_cast<uint64_t>(payload_len);
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
        payload_len,
        plan.copy_source_offset,
        num_pkts,
        get_reorder_seq_bit_offset(cfg),
        get_reorder_seq_bit_width(cfg),
        get_reorder_batch_bit_offset(cfg),
        get_reorder_batch_bit_width(cfg),
        cfg.method_ == ReorderMethod::SEQ_BATCH_NUMBER ? 1U : 0U,
        plan.packets_per_batch,
        output_slots - 1U,
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

      std::memcpy(out_bytes + (static_cast<size_t>(slot_idx) * payload_len),
                  src_pkt + plan.copy_source_offset,
                  payload_len);
    }
  }

  status = create_reorder_output_burst(plan,
                                       plan.output_pool,
                                       output_buffer_idx,
                                       output_buffer,
                                       aggregate_len,
                                       num_pkts,
                                       payload_len,
                                       output_batch_id,
                                       output_batch_id_ready,
                                       output_batch_id_host,
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
  batch->payload_len = 0;
  batch->packet_count = 0;

  return Status::SUCCESS;
}

Status DpdkMgr::flush_reorder_timeouts(ReorderQueueState& qstate, uint64_t now_cycles) {
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

Status DpdkMgr::process_burst_for_reorder(uint32_t key, ReorderQueueState& qstate, BurstParams* burst) {
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

      const size_t batch_size = append_reorder_packet(plan, mbuf, pkt_ptr, now_cycles);
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
    const uint16_t flow_id = get_packet_flow_id(burst, i);
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
      const size_t batch_size = append_reorder_packet(plan, mbuf, pkt_ptr, now_cycles);
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
    auto* flow_info = reinterpret_cast<ExtraRxPacketInfo*>(burst->pkt_extra_info);

    for (int out_idx = 0; out_idx < unmatched_count; ++out_idx) {
      const int in_idx = unmatched_indices[out_idx];
      for (int seg = 0; seg < burst->hdr.hdr.num_segs; ++seg) {
        burst->pkts[seg][out_idx] = burst->pkts[seg][in_idx];
      }
      if (flow_info != nullptr) { flow_info[out_idx] = flow_info[in_idx]; }
    }
    burst->hdr.hdr.num_pkts = unmatched_count;
    qstate.ready_outputs.push_back(burst);
  } else {
    // Matched packets are kept in reorder state and freed when output is emitted.
    free_rx_burst(burst);
  }

  return final_status;
}

Status DpdkMgr::get_next_output_or_ready(uint32_t key, ReorderQueueState& qstate, BurstParams** burst) {
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

void DpdkMgr::release_reorder_output_context(BurstParams* burst) {
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

Status DpdkMgr::get_reorder_burst_info(BurstParams* burst, ReorderBurstInfo* info) {
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

Status DpdkMgr::set_reorder_cuda_stream(const std::string& interface_name,
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
bool DpdkMgr::set_config_and_initialize(const NetworkConfig& cfg) {
  num_init++;

  if (!this->initialized_) {
    cfg_ = cfg;
    cpu_set_t mask;
    long nproc, i;

    // Start Initialize in a separate thread so it doesn't set the affinity for the
    // whole application
    std::thread proc_thread(&DpdkMgr::initialize, this);
    proc_thread.join();

    // Our thread should have set the flag if it succeeded
    if (!this->initialized_) {
      DAQIRI_LOG_CRITICAL("Failed to initialize DPDK");
      return false;
    }

    stats_.Init(cfg_);
    stats_thread_ = std::thread(&DpdkStats::Run, &stats_);

    if (!validate_config()) {
      DAQIRI_LOG_CRITICAL("Config validation failed");
      return false;
    }

    if (!init_reorder_state()) {
      DAQIRI_LOG_CRITICAL("Failed to initialize reorder state");
      return false;
    }

    run();
  }

  return true;
}

Status DpdkMgr::get_mac_addr(int port, char* mac) {
  if (port > mac_addrs.size()) {
    DAQIRI_LOG_CRITICAL("Port {} out of range in get_mac_addr() lookup");
    return Status::INVALID_PARAMETER;
  }

  memcpy(mac, reinterpret_cast<char*>(&mac_addrs[port]), sizeof(mac_addrs[port]));
  return Status::SUCCESS;
}

void DpdkMgr::adjust_memory_regions() {
  for (auto& mr : cfg_.mrs_) {
    // mr.second.buf_size_ = ((target_el_size + 3) / 4) * 4;
    mr.second.adj_size_ = mr.second.buf_size_ + RTE_PKTMBUF_HEADROOM;
    DAQIRI_LOG_INFO("Adjusting buffer size to {} for headroom", mr.second.adj_size_);
  }
}


void DpdkMgr::setup_accurate_send_scheduling_mask() {
  static bool done = false;
  if (done) { return; }

  static const rte_mbuf_dynfield dynfield_desc = {
      RTE_MBUF_DYNFIELD_TIMESTAMP_NAME,
      sizeof(uint64_t),
      .align = __alignof__(uint64_t),
  };

  static const rte_mbuf_dynflag dynflag_desc = {
      RTE_MBUF_DYNFLAG_TX_TIMESTAMP_NAME,
  };

  timestamp_offset_ = rte_mbuf_dynfield_register(&dynfield_desc);
  if (timestamp_offset_ < 0) {
    DAQIRI_LOG_CRITICAL(
        "{} registration error: {}", RTE_MBUF_DYNFIELD_TIMESTAMP_NAME, rte_strerror(rte_errno));
    return;
  }

  int32_t dynflag_bitnum = rte_mbuf_dynflag_register(&dynflag_desc);
  if (dynflag_bitnum == -1) {
    DAQIRI_LOG_CRITICAL(
        "{} registration error: {}", RTE_MBUF_DYNFLAG_TX_TIMESTAMP_NAME, rte_strerror(rte_errno));
    return;
  }

  auto dynflag_shift = static_cast<uint8_t>(dynflag_bitnum);
  timestamp_mask_ = 1ULL << dynflag_shift;
  DAQIRI_LOG_INFO("Done setting up accurate send scheduling with mask {:x}", timestamp_mask_);
  done = true;
}


std::string DpdkMgr::generate_random_string(int len) {
  const char tokens[] = "abcdefghijklmnopqrstuvwxyz";
  std::string tmp;
  tmp.reserve(static_cast<size_t>(len));

  static std::atomic<uint64_t> counter{0};
  uint64_t state =
      static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  state ^= static_cast<uint64_t>(getpid()) << 32U;
  state ^= counter.fetch_add(1, std::memory_order_relaxed);

  for (int i = 0; i < len; i++) {
    state = (state * 2862933555777941757ULL) + 3037000493ULL;
    tmp += tokens[state % (sizeof(tokens) - 1)];
  }

  return tmp;
}

// HWS doesn't allow zero queues on an interface, so we make some dummy interfaces here for
// users that are only doing TX
void DpdkMgr::create_dummy_rx_q() {
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
void DpdkMgr::create_dummy_tx_q() {
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

void DpdkMgr::initialize() {
  int ret;

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
  strncpy(_argv[arg++],
          (std::string("--file-prefix=") + generate_random_string(10)).c_str(),
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

  if (loopback_ != LoopbackType::LOOPBACK_TYPE_SW) {
    for (const auto& name : ifs) {
      strncpy(_argv[arg++], "-a", max_arg_size - 1);
      strncpy(_argv[arg++],
              (name + std::string(",txq_inline_max=0,dv_flow_en=2")).c_str(),
              max_arg_size - 1);
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
          if (mr.num_bufs_ < default_num_rx_desc) {
            DAQIRI_LOG_CRITICAL("Must have at least {} buffers in each RX MR",
                                  default_num_rx_desc);
              return;
          }

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

        if (mr.num_bufs_ < default_num_tx_desc) {
          DAQIRI_LOG_CRITICAL("Must have at least {} buffers in each TX MR", default_num_tx_desc);
          return;
        }

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

    if (loopback_ != LoopbackType::LOOPBACK_TYPE_SW && tx.accurate_send_) {
      setup_accurate_send_scheduling_mask();

      if (ret != 0) {
        DAQIRI_LOG_CRITICAL("Failed to get device info for port {}", intf.port_id_);
        return;
      } else {
        if ((dev_info.tx_offload_capa & RTE_ETH_RX_OFFLOAD_TIMESTAMP) == 0) {
          DAQIRI_LOG_CRITICAL(
              "Accurate send scheduling enabled in config, but not supported by NIC!");
          return;
        } else {
          local_port_conf[intf.port_id_].txmode.offloads |= RTE_ETH_RX_OFFLOAD_TIMESTAMP;
        }
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

      if (intf.rx_.flow_isolation_) {
        struct rte_flow_error error;
        ret = rte_flow_isolate(intf.port_id_, 1, &error);
        if (ret < 0) {
          DAQIRI_LOG_CRITICAL("Failed to set flow isolation");
        } else {
          DAQIRI_LOG_INFO("Port {} in isolation mode", intf.port_id_);
        }
      } else {
        DAQIRI_LOG_INFO("Port {} not in isolation mode", intf.port_id_);
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

      // Start flows
      int flow_num = 0;
      for (const auto& flow : rx.flows_) {
        DAQIRI_LOG_INFO("Adding RX flow {}", flow.name_);
        if (flow.match_.type_ == FlowMatchType::FLEX_ITEM) {
          add_flex_item_flow(intf.port_id_, flow.match_.flex_item_match_, flow.action_.id_);
        } else {
          add_flow(intf.port_id_, flow);
        }
      }

      apply_tx_offloads(intf.port_id_);
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
    config.jump = nullptr;
    config.drop = nullptr;
  }

  this->initialized_ = true;
}

int DpdkMgr::setup_pools_and_rings(int max_rx_batch, int max_tx_batch) {
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

  DAQIRI_LOG_INFO("Setting up RX burst pool with {} batches of size {}",
                    num_rx_ptrs_buffers,
                    sizeof(ExtraRxPacketInfo) * max_rx_batch);
  rx_flow_id_buffer = rte_mempool_create("RX_FLOWID_POOL",
                                       num_rx_ptrs_buffers,
                                       sizeof(ExtraRxPacketInfo) * max_rx_batch,
                                       0,
                                       0,
                                       nullptr,
                                       nullptr,
                                       nullptr,
                                       nullptr,
                                       rte_socket_id(),
                                       0);
  if (rx_flow_id_buffer == nullptr) {
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

#define MAX_PATTERN_NUM 5
#define MAX_ACTION_NUM 4

struct rte_flow_item_flex_handle *DpdkMgr::create_flex_flow_rule(
    int port, int offset, struct rte_flow_item *udp_item, struct rte_flow_item *end_pattern) {
  static struct rte_flow_item_flex_handle *item_handle = NULL;
  struct rte_flow_error error;

  if (item_handle != NULL) {
    return item_handle;
  }

  {
    struct rte_flow_error jump_error;
    struct rte_flow_attr jump_attr;
    jump_attr.group = 0;
    jump_attr.ingress = 1;
    struct rte_flow_action_jump jump_v;
    jump_v.group = 1;
    struct rte_flow_action jump_actions[2];
    jump_actions[0].type = RTE_FLOW_ACTION_TYPE_JUMP;
    jump_actions[0].conf = &jump_v;
    jump_actions[1].type = RTE_FLOW_ACTION_TYPE_END;

    struct rte_flow_item jump_pattern[2];
    jump_pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
    jump_pattern[0].spec = 0;
    jump_pattern[0].mask = 0;
    jump_pattern[1].type = RTE_FLOW_ITEM_TYPE_END;

    int res = rte_flow_validate(port, &jump_attr, jump_pattern, jump_actions, &jump_error);
    if (!res) {
      struct rte_flow* flow = rte_flow_create(
          port, &jump_attr, jump_pattern, jump_actions, &jump_error);
      if (flow == NULL) {
        printf("rte_flow_create failed");
      }
    } else {
      printf("Failed flow validation: %d\n", res);
    }
  }

  struct rte_flow_item_flex_conf flex_conf;
  flex_conf.tunnel = FLEX_TUNNEL_MODE_SINGLE;
  memset(&flex_conf.next_header, 0, sizeof(flex_conf.next_header));
  flex_conf.next_header.field_mode = FIELD_MODE_FIXED;
  flex_conf.next_header.field_base = 32 * 8;  // Always sample 8 32-bit words for now

  memset(&flex_conf.next_protocol, 0, sizeof(flex_conf.next_protocol));

  struct rte_flow_item_flex_field sample_data[1];
  memset(&sample_data[0], 0, sizeof(sample_data));
  sample_data[0].field_mode = FIELD_MODE_FIXED;
  sample_data[0].field_size = 32;
  sample_data[0].field_base = offset * 8;  // Offset is in bytes while DPDK wants bits
  flex_conf.sample_data = &sample_data[0];
  flex_conf.nb_samples = 1;

  struct rte_flow_item_flex_link input_link;
  memset(&input_link, 0, sizeof(input_link));
  input_link.item = *udp_item;
  flex_conf.input_link = &input_link;
  flex_conf.nb_inputs = 1;

  struct rte_flow_item_flex_link output_link;
  memset(&output_link, 0, sizeof(output_link));
  output_link.item = *end_pattern;
  flex_conf.output_link = &output_link;
  flex_conf.nb_outputs = 0;

  item_handle = rte_flow_flex_item_create(port, &flex_conf, &error);
  if (item_handle == NULL) {
    printf("Failed to create flex item: %s\n", error.message);
    return NULL;
  }

  return item_handle;
}

struct rte_flow* DpdkMgr::add_flex_item_flow(
    int port, const FlexItemMatch& match_info, uint16_t queue_id) {
  /* Declaring structs being used. 8< */
  struct rte_flow_attr attr;
  struct rte_flow_item pattern[MAX_PATTERN_NUM];
  struct rte_flow_action action[MAX_ACTION_NUM];
  struct rte_flow* flow = NULL;
  struct rte_flow_action_queue queue = {.index = queue_id};
  struct rte_flow_error error;
  struct rte_flow_item_udp udp_spec;
  struct rte_flow_item_udp udp_mask;
  struct rte_flow_item_ipv4  ip_spec;
  struct rte_flow_item_ipv4  ip_mask;
  int res;
  const auto& flex_item_config = cfg_.ifs_[port].rx_.flex_items_[match_info.flex_item_id_];

  memset(pattern, 0, sizeof(pattern));
  memset(action, 0, sizeof(action));
  memset(&attr, 0, sizeof(struct rte_flow_attr));
  memset(&ip_spec, 0, sizeof(struct rte_flow_item_ipv4));
  memset(&ip_mask, 0, sizeof(struct rte_flow_item_ipv4));
  memset(&udp_spec, 0, sizeof(struct rte_flow_item_udp));
  memset(&udp_mask, 0, sizeof(struct rte_flow_item_udp));

  // struct rte_flow_action_mark mark;
  // mark.id = 0x40 + queue_id;

  action[0].type = RTE_FLOW_ACTION_TYPE_QUEUE;
  action[0].conf = &queue;
  action[1].type = RTE_FLOW_ACTION_TYPE_END;
  //  action[1].type = RTE_FLOW_ACTION_TYPE_MARK;
  //  action[1].conf = &mark;
  //  action[2].type = RTE_FLOW_ACTION_TYPE_END;

  pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
  pattern[1].type = RTE_FLOW_ITEM_TYPE_IPV4;
  //  pattern[3].type = RTE_FLOW_ITEM_TYPE_FLEX; // defined later
  pattern[4].type = RTE_FLOW_ITEM_TYPE_END;

  struct rte_flow_item udp_item;
  udp_spec.hdr.src_port = 0;
  udp_spec.hdr.dst_port = htons(flex_item_config.udp_dst_port_);
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

  if (flex_item_handles_.find(match_info.flex_item_id_) == flex_item_handles_.end()) {
    struct rte_flow_item_flex_handle *item_handle =
      create_flex_flow_rule(port, flex_item_config.offset_, &udp_item, &pattern[4]);
    flex_item_handles_[match_info.flex_item_id_] = item_handle;
  }

  struct rte_flow_item_flex_handle *item_handle = flex_item_handles_[match_info.flex_item_id_];

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

    /* Validate the rule and create it */
     res = rte_flow_validate(port, &attr, pattern, action, &error);
    if (!res) {
        flow = rte_flow_create(port, &attr, pattern, action, &error);
        if (!flow) {
            printf("Flow creation failed for match %08x: %s\n",
                   match_info.val_, error.message);
        } else {
            printf("Created flow rule: match %08x -> Queue %u\n",
                   match_info.val_, queue_id);
        }
    } else {
        printf("Flow validation failed for match %08x: %s\n",
               match_info.val_, error.message);
    }

    return flow;
}


// Taken from flow_block.c DPDK example */
struct rte_flow* DpdkMgr::add_flow(int port, const FlowConfig& cfg) {
  /* Declaring structs being used. 8< */
  struct rte_flow_attr attr;
  struct rte_flow_item pattern[MAX_PATTERN_NUM];
  struct rte_flow_action action[MAX_ACTION_NUM];
  struct rte_flow* flow = NULL;
  struct rte_flow_action_queue queue = {.index = cfg.action_.id_};
  struct rte_flow_action_mark  mark = {.id = cfg.id_};
  struct rte_flow_error error;
  struct rte_flow_item_udp udp_spec;
  struct rte_flow_item_udp udp_mask;
  struct rte_flow_item_ipv4  ip_spec;
  struct rte_flow_item_ipv4  ip_mask;
  int res;

  // HWS requires using a non-zero group, so we make a jump event to group 3 for all ethernet
  // packets
  {
    struct rte_flow_error jump_error;
    struct rte_flow_attr jump_attr{.group = 0, .ingress = 1};
    struct rte_flow_action_jump jump_v = {.group = 3};
    struct rte_flow_action jump_actions[] = {
      { .type = RTE_FLOW_ACTION_TYPE_JUMP, .conf = &jump_v},
      { .type = RTE_FLOW_ACTION_TYPE_END}
    };

    struct rte_flow_item jump_pattern[] = {
      { .type = RTE_FLOW_ITEM_TYPE_ETH, .spec = 0, .mask = 0},
      { .type = RTE_FLOW_ITEM_TYPE_END},
    };

    auto res = rte_flow_validate(port, &jump_attr, jump_pattern, jump_actions, &jump_error);
    if (!res) {
      struct rte_flow* flow = rte_flow_create(
          port, &jump_attr, jump_pattern, jump_actions, &jump_error);
      if (flow == nullptr) {
        DAQIRI_LOG_ERROR("rte_flow_create failed");
      }
    } else {
      DAQIRI_LOG_ERROR("Failed flow validation: {}", res);
    }
  }

  memset(pattern, 0, sizeof(pattern));
  memset(action, 0, sizeof(action));
  memset(&attr, 0, sizeof(struct rte_flow_attr));
  memset(&ip_spec, 0, sizeof(struct rte_flow_item_ipv4));
  memset(&ip_mask, 0, sizeof(struct rte_flow_item_ipv4));
  memset(&udp_spec, 0, sizeof(struct rte_flow_item_udp));
  memset(&udp_mask, 0, sizeof(struct rte_flow_item_udp));

  action[0].type = RTE_FLOW_ACTION_TYPE_MARK;
  action[0].conf = &mark;
  action[1].type = RTE_FLOW_ACTION_TYPE_QUEUE;
  action[1].conf = &queue;
  action[2].type = RTE_FLOW_ACTION_TYPE_END;

  pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
  pattern[1].type = RTE_FLOW_ITEM_TYPE_IPV4;
  pattern[2].type = RTE_FLOW_ITEM_TYPE_UDP;

  bool has_ip_match = false;

  if (cfg.match_.ipv4_len_ > 0) {
    ip_spec.hdr.total_length = htons(cfg.match_.ipv4_len_);
    ip_mask.hdr.total_length = 0xffff;
    has_ip_match = true;
    DAQIRI_LOG_INFO("Adding IPv4 length match for {}", cfg.match_.ipv4_len_);
  }

  if (cfg.match_.ipv4_src_ != INADDR_ANY) {
    char str_ip[INET_ADDRSTRLEN];
    ip_spec.hdr.src_addr = cfg.match_.ipv4_src_;
    ip_mask.hdr.src_addr = 0xffffffff;
    has_ip_match = true;
    inet_ntop(AF_INET, &ip_spec.hdr.src_addr, str_ip, INET_ADDRSTRLEN);
    DAQIRI_LOG_INFO("Adding IPv4 source IP match for {}", str_ip);
  }

  if (cfg.match_.ipv4_dst_ != INADDR_ANY) {
    char str_ip[INET_ADDRSTRLEN];
    ip_spec.hdr.dst_addr = cfg.match_.ipv4_dst_;
    ip_mask.hdr.dst_addr = 0xffffffff;
    has_ip_match = true;
    inet_ntop(AF_INET, &ip_spec.hdr.dst_addr, str_ip, INET_ADDRSTRLEN);
    DAQIRI_LOG_INFO("Adding IPv4 destination IP match for {}", str_ip);
  }

  if (has_ip_match == true) {
    pattern[1].spec = &ip_spec;
    pattern[1].mask = &ip_mask;
  }

  bool has_udp_match = false;

  if (cfg.match_.udp_src_ > 0) {
    udp_spec.hdr.src_port = htons(cfg.match_.udp_src_);
    udp_mask.hdr.src_port = 0xffff;
    has_udp_match = true;
    DAQIRI_LOG_INFO("Adding UDP port match for src {}", cfg.match_.udp_src_);
  }

  if (cfg.match_.udp_dst_ > 0) {
    udp_spec.hdr.dst_port = htons(cfg.match_.udp_dst_);
    udp_mask.hdr.dst_port = 0xffff;
    has_udp_match = true;
    DAQIRI_LOG_INFO("Adding UDP port match for dst {}", cfg.match_.udp_dst_);
  }

  if (has_udp_match == true) {
    udp_spec.hdr.dgram_len = 0;
    udp_spec.hdr.dgram_cksum = 0;
    udp_mask.hdr.dgram_len = 0;
    udp_mask.hdr.dgram_cksum = 0;

    pattern[2].spec = &udp_spec;
    pattern[2].mask = &udp_mask;
  }


  attr.ingress = 1;
  attr.priority = 1;  // Lower priority to allow drop_traffic (priority 0) to take precedence
  attr.group = 3;

  pattern[3].type = RTE_FLOW_ITEM_TYPE_END;

  flow = rte_flow_create(port, &attr, pattern, action, &error);
  return flow;
}

Status DpdkMgr::drop_all_traffic(int port) {
  /* Declaring structs being used. */
  struct rte_flow_attr attr;
  struct rte_flow_item pattern[MAX_PATTERN_NUM];
  struct rte_flow_action action[MAX_ACTION_NUM];
  struct rte_flow* flow = NULL;
  struct rte_flow_error error;
  DropTrafficConfig config;

  // Initialize the jump rule to group 3 (required by HWS)
  {
    struct rte_flow_error jump_error;
    struct rte_flow_attr jump_attr{.group = 0, .ingress = 1};
    struct rte_flow_action_jump jump_v = {.group = 3};
    struct rte_flow_action jump_actions[] = {
      { .type = RTE_FLOW_ACTION_TYPE_JUMP, .conf = &jump_v},
      { .type = RTE_FLOW_ACTION_TYPE_END}
    };

    struct rte_flow_item jump_pattern[] = {
      { .type = RTE_FLOW_ITEM_TYPE_ETH, .spec = 0, .mask = 0},
      { .type = RTE_FLOW_ITEM_TYPE_END},
    };

    auto res = rte_flow_validate(port, &jump_attr, jump_pattern, jump_actions, &jump_error);
    if (!res) {
      config.jump = rte_flow_create(
          port, &jump_attr, jump_pattern, jump_actions, &jump_error);
      if (config.jump == nullptr) {
        DAQIRI_LOG_ERROR("rte_flow_create failed for jump rule in drop_all_traffic");
      }
    } else {
      DAQIRI_LOG_ERROR("Failed flow validation for jump rule in drop_all_traffic: {}", res);
    }
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
    port, attr.priority);

  // Create the flow rule
  config.drop = rte_flow_create(port, &attr, pattern, action, &error);

  if (config.drop == nullptr) {
    DAQIRI_LOG_ERROR("Failed to create drop all traffic flow rule on port {}: {}",
                       port, error.message ? error.message : "unknown error");
    rte_flow_destroy(port, config.jump, &error);
    return Status::INTERNAL_ERROR;
  } else {
    DAQIRI_LOG_INFO("Successfully created drop all traffic rule on port {}", port);
  }

  drop_all_traffic_flow[port] = config;
  flush_rx_queues.store(true);

  return Status::SUCCESS;
}

Status DpdkMgr::allow_all_traffic(int port) {
  if (drop_all_traffic_flow[port].drop == nullptr) {
    DAQIRI_LOG_ERROR("Cannot remove drop rule: flow pointer is null");
    return Status::INVALID_PARAMETER;
  }

  // Tell the RX threads they can keep processing packets
  flush_rx_queues.store(false);

  struct rte_flow_error jump_error;
  struct rte_flow_error drop_error;
  int drop_ret = rte_flow_destroy(port, drop_all_traffic_flow[port].drop, &drop_error);
  int jump_ret = rte_flow_destroy(port, drop_all_traffic_flow[port].jump, &jump_error);

  if (drop_ret != 0) {
    DAQIRI_LOG_ERROR("Failed to destroy drop all traffic flow rule on port {}: {}",
                       port, drop_error.message ? drop_error.message : "unknown error");
  }

  if (jump_ret != 0) {
    DAQIRI_LOG_ERROR("Failed to destroy jump all traffic flow rule on port {}: {}",
                       port, jump_error.message ? jump_error.message : "unknown error");
  }

  if (drop_ret != 0 || jump_ret != 0) {
    return Status::INTERNAL_ERROR;
  }

  drop_all_traffic_flow[port].drop = nullptr;
  drop_all_traffic_flow[port].jump = nullptr;

  DAQIRI_LOG_INFO(
    "Successfully removed drop all traffic rule on port {}, traffic is now allowed", port);
  return Status::SUCCESS;
}

struct rte_flow* DpdkMgr::add_modify_flow_set(int port, int queue, const char* buf, int len,
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
  if (!res) {
    flow = rte_flow_create(port, &attr, pattern, action, &error);
    return flow;
  }

  return nullptr;
}

void DpdkMgr::apply_tx_offloads(int port) {
  for (const auto& q : cfg_.ifs_[port].tx_.queues_) {
    for (const auto& off : q.common_.offloads_) {
      if (off == "tx_eth_src") {  // Offload Ethernet source copy
        DAQIRI_LOG_INFO("Applying {} offload for port {}", off, port);
        const auto mac_bytes = mac_addrs[port];
        add_modify_flow_set(port,
                            q.common_.id_,
                            reinterpret_cast<const char*>(&mac_bytes),
                            sizeof(mac_bytes) * 8,
                            Direction::TX);
      }
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
///
///  \brief
///
////////////////////////////////////////////////////////////////////////////////
void DpdkMgr::PrintDpdkStats(int port) {
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

DpdkMgr::~DpdkMgr() {
    cleanup_reorder_state();

    // Add cleanup for rings in the map
    for (auto const& [key, val] : rx_rings) {
        if (val != nullptr) {
            rte_ring_free(val);
        }
    }
    rx_rings.clear();

    for (auto const& [key, val] : tx_rings) {
        if (val != nullptr) {
            rte_ring_free(val);
        }
    }
    tx_rings.clear();
}

bool DpdkMgr::validate_config() const {
  if (!Manager::validate_config()) { return false; }

  for (const auto& intf : cfg_.ifs_) {
    std::unordered_map<uint16_t, uint16_t> flow_to_queue;
    for (const auto& flow : intf.rx_.flows_) {
      if (flow_to_queue.find(flow.id_) != flow_to_queue.end()) {
        DAQIRI_LOG_ERROR("Duplicate flow ID {} on interface '{}'", flow.id_, intf.name_);
        return false;
      }
      flow_to_queue.emplace(flow.id_, flow.action_.id_);
    }

    std::unordered_set<uint16_t> reorder_flow_ids;
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
void DpdkMgr::run() {
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
      params->flowid_pool = rx_flow_id_buffer;
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

        params->q_params.push_back({port_id, q_id,
                    (int)q->common_.mrs_.size(), q->common_.batch_size_, q->timeout_us_, ring_ptr});
      }

      params->rx_burst_pool = rx_burst_buffer;
      params->flowid_pool = rx_flow_id_buffer;
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
void DpdkMgr::flush_packets(int port) {
  struct rte_mbuf* rx_mbuf;
  DAQIRI_LOG_INFO("Flushing packet on port {}", port);
  while (rte_eth_rx_burst(port, 0, &rx_mbuf, 1) != 0) { rte_pktmbuf_free(rx_mbuf); }
}

void DpdkMgr::flush_port_queue(int port, int queue) {
  struct rte_mbuf* rx_mbuf;
  DAQIRI_LOG_INFO("Flushing packets on port {} queue {}", port, queue);
  while (rte_eth_rx_burst(port, queue, &rx_mbuf, 1) != 0) { rte_pktmbuf_free(rx_mbuf); }
}

/*
  RX worker supporting multiple queues for a single core. This is useful when a user wants
  to segregate traffic by queues, but they don't want to waste extra CPU cores by mapping a
  core per queue.
*/
int DpdkMgr::rx_core_multi_q_worker(void* arg) {
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

  std::array<int, Manager::MAX_RX_Q_PER_CORE> nb_rx{};
  std::array<int, Manager::MAX_RX_Q_PER_CORE> cur_pkt_in_batch{};
  std::array<BurstParams*, Manager::MAX_RX_Q_PER_CORE> bursts{};
  std::array<std::array<rte_mbuf*, DEFAULT_NUM_RX_BURST>, Manager::MAX_RX_Q_PER_CORE> mbuf_arr{};
  std::array<uint64_t, Manager::MAX_RX_Q_PER_CORE> total_pkts{};
  std::array<uint64_t, Manager::MAX_RX_Q_PER_CORE> last_cycles;
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

          // Free the pkt_extra_info buffer back to the pool
          if (bursts[i]->pkt_extra_info != nullptr) {
            rte_mempool_put(tparams->flowid_pool, bursts[i]->pkt_extra_info);
          }

          // Free the segment buffers back to the burst pool
          for (int seg = 0; seg < bursts[i]->hdr.hdr.num_segs; seg++) {
            rte_mempool_put(tparams->rx_burst_pool, (void*)bursts[i]->pkts[seg]);
          }

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

      //  Queue ID for receiver to differentiate
      bursts[cur_idx]->hdr.hdr.q_id     = cur_q;
      bursts[cur_idx]->hdr.hdr.port_id  = cur_port;
      bursts[cur_idx]->hdr.hdr.num_segs = cur_segs;
      bursts[cur_idx]->hdr.hdr.num_pkts = 0;

      for (int seg = 0; seg < cur_segs; seg++) {
        if (rte_mempool_get(tparams->rx_burst_pool,
          reinterpret_cast<void**>(&bursts[cur_idx]->pkts[seg])) < 0) {
          DAQIRI_LOG_ERROR(
              "Processing function falling behind. No free RX bursts!");
          continue;
        }
      }

      if (rte_mempool_get(
            tparams->flowid_pool, reinterpret_cast<void**>(&bursts[cur_idx]->pkt_extra_info)) < 0) {
        DAQIRI_LOG_ERROR("Processing function falling behind. No free CPU buffers for packets!");
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
    memcpy(&bursts[cur_idx]->pkts[0][bursts[cur_idx]->hdr.hdr.num_pkts],
                  &mbuf_arr[cur_idx][cur_pkt_in_batch[cur_idx]], sizeof(rte_mbuf*) * to_copy);

    ExtraRxPacketInfo* pkt_info =
                          reinterpret_cast<ExtraRxPacketInfo*>(bursts[cur_idx]->pkt_extra_info);
    for (int p = 0; p < to_copy; p++) {
      if (mbuf_arr[cur_idx][cur_pkt_in_batch[cur_idx] + p]->ol_flags & RTE_MBUF_F_RX_FDIR_ID) {
        pkt_info[bursts[cur_idx]->hdr.hdr.num_pkts + p].flow_id =
                              mbuf_arr[cur_idx][cur_pkt_in_batch[cur_idx] + p]->hash.fdir.hi;
      } else {
        pkt_info[bursts[cur_idx]->hdr.hdr.num_pkts + p].flow_id = 0;
      }
    }

    if (cur_segs > 1) {  // Extra work when buffers are scattered
      for (int p = 0; p < to_copy; p++) {
        struct rte_mbuf* mbuf = mbuf_arr[cur_idx][cur_pkt_in_batch[cur_idx] + p];
        for (int seg = 1; seg < cur_segs; seg++) {
          mbuf = mbuf->next;
          bursts[cur_idx]->pkts[seg][bursts[cur_idx]->hdr.hdr.num_pkts + p] = mbuf;
        }
      }
    }

    cur_pkt_in_batch[cur_idx]         += to_copy;
    bursts[cur_idx]->hdr.hdr.num_pkts += to_copy;
    nb_rx[cur_idx]                    -= to_copy;
    total_pkts[cur_idx]               += to_copy;

    if (bursts[cur_idx]->hdr.hdr.num_pkts == cur_batch_size) {
      rte_ring_enqueue(tparams->q_params[cur_idx].ring, reinterpret_cast<void*>(bursts[cur_idx]));
      last_cycles[cur_idx] = rte_get_tsc_cycles();
      bursts[cur_idx] = nullptr;
    } else if (cur_timeout_cycles > 0) {
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
int DpdkMgr::rx_core_worker(void* arg) {
  RxWorkerParams* tparams = (RxWorkerParams*)arg;

  // In the future we may want to periodically update this if the CPU clock drifts
  uint64_t freq = rte_get_tsc_hz();
  uint64_t timeout_cycles = freq * (tparams->timeout_us/1e6);
  uint64_t last_cycles = rte_get_tsc_cycles();
  uint64_t total_pkts = 0;

  flush_packets(tparams->port);
  struct rte_mbuf* mbuf_arr[DEFAULT_NUM_RX_BURST];

  DAQIRI_LOG_INFO("Starting RX Core {}, port {}, queue {}, socket {}",
                    rte_lcore_id(),
                    tparams->port,
                    tparams->queue,
                    rte_socket_id());
  int nb_rx = 0;
  int cur_pkt_in_batch = 0;
  BurstParams* burst = nullptr;
  ExtraRxPacketInfo *pkt_info;
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

        // Free the pkt_extra_info buffer back to the pool
        if (burst->pkt_extra_info != nullptr) {
          rte_mempool_put(tparams->flowid_pool, burst->pkt_extra_info);
        }

        // Free the segment buffers back to the burst pool
        for (int seg = 0; seg < burst->hdr.hdr.num_segs; seg++) {
          rte_mempool_put(tparams->rx_burst_pool, (void*)burst->pkts[seg]);
        }

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

      //  Queue ID for receiver to differentiate
      burst->hdr.hdr.q_id     = tparams->queue;
      burst->hdr.hdr.port_id  = tparams->port;
      burst->hdr.hdr.num_segs = tparams->num_segs;
      burst->hdr.hdr.num_pkts = 0;

      for (int seg = 0; seg < tparams->num_segs; seg++) {
        if (rte_mempool_get(tparams->rx_burst_pool,
          reinterpret_cast<void**>(&burst->pkts[seg])) < 0) {
          DAQIRI_LOG_ERROR(
              "Processing function falling behind. No free RX bursts!");
          continue;
        }
      }

      if (rte_mempool_get(
            tparams->flowid_pool, reinterpret_cast<void**>(&burst->pkt_extra_info)) < 0) {
        DAQIRI_LOG_ERROR("Processing function falling behind. No free CPU buffers for packets!");
        continue;
      }

      pkt_info = reinterpret_cast<ExtraRxPacketInfo*>(burst->pkt_extra_info);
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
    memcpy(&burst->pkts[0][burst->hdr.hdr.num_pkts],
                                        &mbuf_arr[cur_pkt_in_batch], sizeof(rte_mbuf*) * to_copy);

    for (int p = 0; p < to_copy; p++) {
      if (mbuf_arr[cur_pkt_in_batch + p]->ol_flags & RTE_MBUF_F_RX_FDIR_ID) {
        pkt_info[burst->hdr.hdr.num_pkts + p].flow_id =
                                          mbuf_arr[cur_pkt_in_batch + p]->hash.fdir.hi;
      } else {
        pkt_info[burst->hdr.hdr.num_pkts + p].flow_id = 0;
      }
    }

    if (tparams->num_segs > 1) {  // Extra work when buffers are scattered
      for (int p = 0; p < to_copy; p++) {
        struct rte_mbuf* mbuf = mbuf_arr[cur_pkt_in_batch + p];
        for (int seg = 1; seg < tparams->num_segs; seg++) {
          mbuf = mbuf->next;
          burst->pkts[seg][burst->hdr.hdr.num_pkts + p] = mbuf;
        }
      }
    }

    cur_pkt_in_batch        += to_copy;
    burst->hdr.hdr.num_pkts += to_copy;
    nb_rx                   -= to_copy;
    total_pkts              += to_copy;

    if (burst->hdr.hdr.num_pkts == tparams->batch_size) {
      rte_ring_enqueue(tparams->ring, reinterpret_cast<void*>(burst));
      last_cycles = rte_get_tsc_cycles();
      burst = nullptr;
    } else if (timeout_cycles > 0) {
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

int DpdkMgr::rx_lb_worker(void* arg) {
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

    meta_burst->hdr.hdr.q_id = tparams->queue;
    meta_burst->hdr.hdr.port_id = tparams->port;
    meta_burst->hdr.hdr.num_segs = tparams->num_segs;

    for (int seg = 0; seg < tparams->num_segs; seg++) {
      if (rte_mempool_get(
            tparams->rx_burst_pool, reinterpret_cast<void**>(&meta_burst->pkts[seg])) < 0) {
        DAQIRI_LOG_ERROR(
            "Processing function falling behind. No free flow ID buffers for packets!");
        continue;
      }
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

int DpdkMgr::tx_core_worker(void* arg) {
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

int DpdkMgr::tx_lb_worker(void* arg) {
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
void* DpdkMgr::get_segment_packet_ptr(BurstParams* burst, int seg, int idx) {
  if (burst == nullptr || idx < 0 || seg < 0 || seg >= MAX_NUM_SEGS) { return nullptr; }
  if ((burst->hdr.hdr.burst_flags & kBurstFlagDpdkReordered) != 0U) {
    if (seg != 0 || burst->pkts[0] == nullptr) { return nullptr; }
    return burst->pkts[0][idx];
  }
  return rte_pktmbuf_mtod(reinterpret_cast<rte_mbuf*>(burst->pkts[seg][idx]), void*);
}

void* DpdkMgr::get_packet_ptr(BurstParams* burst, int idx) {
  if (burst == nullptr || idx < 0) { return nullptr; }
  if ((burst->hdr.hdr.burst_flags & kBurstFlagDpdkReordered) != 0U) {
    if (burst->pkts[0] == nullptr) { return nullptr; }
    return burst->pkts[0][idx];
  }
  return rte_pktmbuf_mtod(reinterpret_cast<rte_mbuf*>(burst->pkts[0][idx]), void*);
}

uint32_t DpdkMgr::get_segment_packet_length(BurstParams* burst, int seg, int idx) {
  if (burst == nullptr || idx < 0 || seg < 0 || seg >= MAX_NUM_SEGS) { return 0; }
  if ((burst->hdr.hdr.burst_flags & kBurstFlagDpdkReordered) != 0U) {
    if (seg != 0 || burst->pkt_lens[0] == nullptr) { return 0; }
    return burst->pkt_lens[0][idx];
  }
  return reinterpret_cast<rte_mbuf*>(burst->pkts[seg][idx])->data_len;
}

uint32_t DpdkMgr::get_packet_length(BurstParams* burst, int idx) {
  if (burst == nullptr || idx < 0) { return 0; }
  if ((burst->hdr.hdr.burst_flags & kBurstFlagDpdkReordered) != 0U) {
    if (burst->pkt_lens[0] == nullptr) { return 0; }
    return burst->pkt_lens[0][idx];
  }
  return reinterpret_cast<rte_mbuf*>(burst->pkts[0][idx])->pkt_len;
}

uint16_t DpdkMgr::get_packet_flow_id(BurstParams* burst, int idx) {
  if (burst == nullptr || idx < 0) { return 0; }
  if ((burst->hdr.hdr.burst_flags & kBurstFlagDpdkReordered) != 0U) { return 0; }
  const ExtraRxPacketInfo* info = reinterpret_cast<ExtraRxPacketInfo*>(burst->pkt_extra_info);
  if (info == nullptr) { return 0; }
  return info[idx].flow_id;
}

Status DpdkMgr::set_packet_tx_time(BurstParams* burst, int idx, uint64_t timestamp) {
  reinterpret_cast<struct rte_mbuf**>(burst->pkts[0])[idx]->ol_flags |= timestamp_mask_;
  *RTE_MBUF_DYNFIELD(
      reinterpret_cast<rte_mbuf**>(burst->pkts[0])[idx], timestamp_offset_, uint64_t*) = timestamp;

  return Status::SUCCESS;
}

//  The number of RX can differ from the configured queues in TX-only mode where we need to create
//  fake RX queues.
uint16_t DpdkMgr::get_num_rx_queues(int port_id) const {
  return port_q_num.at(static_cast<uint16_t>(port_id)).first;
}

void* DpdkMgr::get_packet_extra_info(BurstParams* burst, int idx) {
  if (burst == nullptr || idx < 0) { return nullptr; }
  if ((burst->hdr.hdr.burst_flags & kBurstFlagDpdkReordered) != 0U) { return nullptr; }
  return nullptr;
}

Status DpdkMgr::get_tx_packet_burst(BurstParams* burst) {
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

Status DpdkMgr::set_eth_header(BurstParams* burst, int idx, char* dst_addr) {
  auto mbuf = reinterpret_cast<rte_mbuf*>(burst->pkts[0][idx]);
  auto mbuf_data = rte_pktmbuf_mtod(mbuf, UDPPkt*);
  memcpy(reinterpret_cast<void*>(&mbuf_data->eth.dst_addr),
         reinterpret_cast<void*>(dst_addr),
         sizeof(mbuf_data->eth.dst_addr));

  mbuf_data->eth.ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
  return Status::SUCCESS;
}

Status DpdkMgr::set_ipv4_header(BurstParams* burst, int idx, int ip_len, uint8_t proto,
                                   unsigned int src_host, unsigned int dst_host) {
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

Status DpdkMgr::set_udp_header(BurstParams* burst, int idx, int udp_len, uint16_t src_port,
                                  uint16_t dst_port) {
  auto mbuf = reinterpret_cast<rte_mbuf*>(burst->pkts[0][idx]);
  auto mbuf_data = rte_pktmbuf_mtod(mbuf, UDPPkt*);

  mbuf_data->udp.dgram_cksum = 0;
  mbuf_data->udp.src_port = htons(src_port);
  mbuf_data->udp.dst_port = htons(dst_port);
  mbuf_data->udp.dgram_len = htons(udp_len + sizeof(mbuf_data->udp));
  return Status::SUCCESS;
}

Status DpdkMgr::set_udp_payload(BurstParams* burst, int idx, void* data, int len) {
  auto mbuf = reinterpret_cast<rte_mbuf*>(burst->pkts[0][idx]);
  auto mbuf_data = rte_pktmbuf_mtod(mbuf, UDPPkt*);

  rte_memcpy(mbuf_data->payload, data, len);
  return Status::SUCCESS;
}

bool DpdkMgr::is_tx_burst_available(BurstParams* burst) {
  const uint32_t key = generate_queue_key(burst->hdr.hdr.port_id, burst->hdr.hdr.q_id);
  const auto item = tx_dpdk_q_map_.find(key);
  if (item == tx_dpdk_q_map_.end()) {
    return false;
  }

  const auto& q = item->second;
  for (int seg = 0; seg < burst->hdr.hdr.num_segs; seg++) {
    if (rte_mempool_avail_count(q->pools[seg]) < burst->hdr.hdr.num_pkts * 2) { return false; }
  }

  return true;
}

Status DpdkMgr::set_packet_lengths(BurstParams* burst, int idx,
                                   const std::initializer_list<int>& lens) {
  uint32_t ttl_len = 0;
  for (int seg = 0; seg < burst->hdr.hdr.num_segs; seg++) {
    reinterpret_cast<rte_mbuf**>(burst->pkts[seg])[idx]->data_len = *(lens.begin() + seg);
    ttl_len += *(lens.begin() + seg);
  }

  reinterpret_cast<rte_mbuf**>(burst->pkts[0])[idx]->pkt_len = ttl_len;

  return Status::SUCCESS;
}

void DpdkMgr::free_packet_segment(BurstParams* burst, int seg, int pkt) {
  if (burst == nullptr) { return; }
  if ((burst->hdr.hdr.burst_flags & kBurstFlagDpdkReordered) != 0U) {
    release_reorder_output_context(burst);
    if (seg == 0 && burst->pkt_lens[0] != nullptr && pkt >= 0
        && pkt < static_cast<int>(burst->hdr.hdr.num_pkts)) {
      burst->pkt_lens[0][pkt] = 0;
    }
    return;
  }
  rte_pktmbuf_free_seg(reinterpret_cast<rte_mbuf**>(burst->pkts[seg])[pkt]);
}

void DpdkMgr::free_all_segment_packets(BurstParams* burst, int seg) {
  if (burst == nullptr) { return; }
  if ((burst->hdr.hdr.burst_flags & kBurstFlagDpdkReordered) != 0U) {
    release_reorder_output_context(burst);
    if (seg == 0 && burst->pkt_lens[0] != nullptr) {
      for (int p = 0; p < burst->hdr.hdr.num_pkts; p++) { burst->pkt_lens[0][p] = 0; }
    }
    return;
  }
  for (int p = 0; p < burst->hdr.hdr.num_pkts; p++) {
    rte_pktmbuf_free_seg(reinterpret_cast<rte_mbuf**>(burst->pkts[seg])[p]);
  }
}

void DpdkMgr::free_packet(BurstParams* burst, int pkt) {
  if (burst == nullptr) { return; }
  if ((burst->hdr.hdr.burst_flags & kBurstFlagDpdkReordered) != 0U) {
    release_reorder_output_context(burst);
    if (burst->pkt_lens[0] != nullptr && pkt >= 0 && pkt < static_cast<int>(burst->hdr.hdr.num_pkts)) {
      burst->pkt_lens[0][pkt] = 0;
    }
    return;
  }
  rte_pktmbuf_free(reinterpret_cast<rte_mbuf**>(burst->pkts[0])[pkt]);
}

void DpdkMgr::free_all_packets(BurstParams* burst) {
  if (burst == nullptr) { return; }
  if ((burst->hdr.hdr.burst_flags & kBurstFlagDpdkReordered) != 0U) {
    release_reorder_output_context(burst);
    if (burst->pkt_lens[0] != nullptr) {
      for (int p = 0; p < burst->hdr.hdr.num_pkts; p++) { burst->pkt_lens[0][p] = 0; }
    }
    return;
  }
  for (int p = 0; p < burst->hdr.hdr.num_pkts; p++) {
    rte_pktmbuf_free(reinterpret_cast<rte_mbuf**>(burst->pkts[0])[p]);
  }
}

void DpdkMgr::free_rx_burst(BurstParams* burst) {
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

  if (burst->pkt_extra_info != nullptr) {
    rte_mempool_put(rx_flow_id_buffer, (void*)burst->pkt_extra_info);
  }

  for (int seg = 0; seg < burst->hdr.hdr.num_segs; seg++) {
    rte_mempool_put(rx_burst_buffer, (void*)burst->pkts[seg]);
  }

  burst->hdr.hdr.num_pkts = 0;
  burst->pkt_extra_info = nullptr;
  rte_mempool_put(rx_metadata, burst);
}

void DpdkMgr::free_tx_burst(BurstParams* burst) {
  const uint32_t key = generate_queue_key(burst->hdr.hdr.port_id, burst->hdr.hdr.q_id);
  const auto burst_pool = tx_burst_buffers.find(key);

  for (int seg = 0; seg < burst->hdr.hdr.num_segs; seg++) {
    rte_mempool_put(burst_pool->second, (void*)burst->pkts[seg]);
  }

  burst->hdr.hdr.num_pkts = 0;
  rte_mempool_put(tx_metadata, burst);
}

Status DpdkMgr::get_rx_burst(BurstParams** burst, int port, int q) {
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

void DpdkMgr::free_rx_metadata(BurstParams* burst) {
  if (burst != nullptr && (burst->hdr.hdr.burst_flags & kBurstFlagDpdkReordered) != 0U) {
    free_rx_burst(burst);
    return;
  }
  rte_mempool_put(rx_metadata, burst);
}

void DpdkMgr::free_tx_metadata(BurstParams* burst) {
  rte_mempool_put(tx_metadata, burst);
}

Status DpdkMgr::get_tx_metadata_buffer(BurstParams** burst) {
  if (rte_mempool_get(tx_metadata, reinterpret_cast<void**>(burst)) != 0) {
    DAQIRI_LOG_CRITICAL("Running out of TX meta buffers due to high rates. Either increase "\
      "your number of metadata buffers (current: {}) with `tx_meta_buffers` (will "\
      "increase memory usage) or increase your `batch_size` for port {} queue {} (will increase "\
      "latency)", cfg_.tx_meta_buffers_, (*burst)->hdr.hdr.port_id, (*burst)->hdr.hdr.q_id);
    return Status::NO_FREE_BURST_BUFFERS;
  }

  return Status::SUCCESS;
}

Status DpdkMgr::send_tx_burst(BurstParams* burst) {
  uint32_t key = generate_queue_key(burst->hdr.hdr.port_id, burst->hdr.hdr.q_id);
  const auto ring = tx_rings.find(key);

  if (ring == tx_rings.end()) {
    DAQIRI_LOG_ERROR("Invalid port/queue combination in send_tx_burst: {}/{}",
                       burst->hdr.hdr.port_id,
                       burst->hdr.hdr.q_id);
    return Status::INVALID_PARAMETER;
  }

  if (rte_ring_enqueue(ring->second, reinterpret_cast<void*>(burst)) != 0) {
    free_tx_burst(burst);
    free_tx_metadata(burst);
    DAQIRI_LOG_CRITICAL("Failed to enqueue TX work");
    return Status::NO_SPACE_AVAILABLE;
  }

  return Status::SUCCESS;
}

void DpdkMgr::shutdown() {
  DAQIRI_LOG_INFO("daqiri DPDK manager shutdown called {}", num_init);

  if (--num_init == 0) {
    DAQIRI_LOG_INFO("daqiri DPDK manager shutting down");
    cleanup_reorder_state();
    force_quit.store(true);

    stats_.Shutdown();
    stats_thread_.join();
  }
}

void DpdkMgr::print_stats() {
  DAQIRI_LOG_INFO("daqiri DPDK manager stats");
  if (loopback_ == LoopbackType::LOOPBACK_TYPE_SW) {
    return;
  }
  int portid;
  RTE_ETH_FOREACH_DEV(portid) {
    PrintDpdkStats(portid);
  }
}

uint64_t DpdkMgr::get_burst_tot_byte(BurstParams* burst) {
  if (burst == nullptr) { return 0; }
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

BurstParams* DpdkMgr::create_tx_burst_params() {
  BurstParams* burst = nullptr;
  if (rte_mempool_get(tx_metadata, reinterpret_cast<void**>(&burst)) != 0) {
    DAQIRI_LOG_CRITICAL("Failed to get TX meta descriptor");
    return nullptr;
  }
  return burst;
}
};  // namespace daqiri
