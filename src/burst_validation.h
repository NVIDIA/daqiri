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

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>

#include <daqiri/logging.hpp>
#include <daqiri/types.h>

#ifndef DAQIRI_ENABLE_BURST_VALIDATION
#define DAQIRI_ENABLE_BURST_VALIDATION 1
#endif

namespace daqiri::burst_validation {

struct BurstLimits {
  size_t num_pkts = 0;
  int num_segs = 0;
};

static inline constexpr size_t kUnknownCapacity = std::numeric_limits<size_t>::max();

inline constexpr bool strict_enabled() {
  return DAQIRI_ENABLE_BURST_VALIDATION != 0;
}

inline BurstLimits header_limits(const BurstParams* burst) {
  if (burst == nullptr) { return {}; }
  return {burst->hdr.hdr.num_pkts, burst->hdr.hdr.num_segs};
}

inline BurstLimits transport_limits(const BurstParams* burst) {
  if (burst == nullptr) { return {}; }
  return {burst->transport_hdr.num_pkts, burst->transport_hdr.num_segs};
}

inline Status validate_burst(const BurstParams* burst, const char* op_name) {
  if (burst != nullptr) { return Status::SUCCESS; }
  DAQIRI_LOG_ERROR("{}: burst is null", op_name);
  return Status::NULL_PTR;
}

inline Status validate_packet_index(const BurstParams* burst,
                                    BurstLimits limits,
                                    int idx,
                                    const char* op_name) {
  Status status = validate_burst(burst, op_name);
  if (status != Status::SUCCESS) { return status; }
  if (idx < 0 || (strict_enabled() && static_cast<size_t>(idx) >= limits.num_pkts)) {
    DAQIRI_LOG_ERROR("{}: packet index {} out of range [0, {})", op_name, idx, limits.num_pkts);
    return Status::INVALID_PARAMETER;
  }
  return Status::SUCCESS;
}

inline Status validate_packet_count(const BurstParams* burst,
                                    BurstLimits limits,
                                    size_t max_pkts,
                                    const char* op_name) {
  Status status = validate_burst(burst, op_name);
  if (status != Status::SUCCESS) { return status; }
  if (!strict_enabled()) { return Status::SUCCESS; }
  if (limits.num_pkts == 0 || limits.num_pkts > max_pkts) {
    DAQIRI_LOG_ERROR("{}: invalid packet count {} (max {})",
                     op_name,
                     limits.num_pkts,
                     max_pkts);
    return Status::INVALID_PARAMETER;
  }
  return Status::SUCCESS;
}

inline Status validate_segment_count(const BurstParams* burst,
                                     BurstLimits limits,
                                     const char* op_name) {
  Status status = validate_burst(burst, op_name);
  if (status != Status::SUCCESS) { return status; }
  if (!strict_enabled()) { return Status::SUCCESS; }
  if (limits.num_segs <= 0 || limits.num_segs > MAX_NUM_SEGS) {
    DAQIRI_LOG_ERROR("{}: invalid segment count {}", op_name, limits.num_segs);
    return Status::INVALID_PARAMETER;
  }
  return Status::SUCCESS;
}

inline Status validate_segment_index(const BurstParams* burst,
                                     BurstLimits limits,
                                     int seg,
                                     const char* op_name) {
  Status status = validate_burst(burst, op_name);
  if (status != Status::SUCCESS) { return status; }
  if (!strict_enabled()) {
    if (seg < 0 || seg >= MAX_NUM_SEGS) {
      DAQIRI_LOG_ERROR("{}: segment index {} out of range [0, {})", op_name, seg, MAX_NUM_SEGS);
      return Status::INVALID_PARAMETER;
    }
    return Status::SUCCESS;
  }
  status = validate_segment_count(burst, limits, op_name);
  if (status != Status::SUCCESS) { return status; }
  if (seg < 0 || seg >= limits.num_segs) {
    DAQIRI_LOG_ERROR("{}: segment index {} out of range [0, {})", op_name, seg, limits.num_segs);
    return Status::INVALID_PARAMETER;
  }
  return Status::SUCCESS;
}

inline Status validate_segment_packet_storage(const BurstParams* burst,
                                              BurstLimits limits,
                                              int seg,
                                              int idx,
                                              bool require_packet_ptr,
                                              bool require_length_ptr,
                                              const char* op_name) {
  Status status = validate_packet_index(burst, limits, idx, op_name);
  if (status != Status::SUCCESS) { return status; }
  status = validate_segment_index(burst, limits, seg, op_name);
  if (status != Status::SUCCESS) { return status; }
  if (!strict_enabled()) { return Status::SUCCESS; }

  if (require_packet_ptr) {
    if (burst->pkts[seg] == nullptr) {
      DAQIRI_LOG_ERROR("{}: packet pointer array is null for segment {}", op_name, seg);
      return Status::NULL_PTR;
    }
    if (burst->pkts[seg][idx] == nullptr) {
      DAQIRI_LOG_ERROR("{}: packet pointer is null for packet {} segment {}", op_name, idx, seg);
      return Status::NULL_PTR;
    }
  }

  if (require_length_ptr && burst->pkt_lens[seg] == nullptr) {
    DAQIRI_LOG_ERROR("{}: packet length array is null for segment {}", op_name, seg);
    return Status::NULL_PTR;
  }

  return Status::SUCCESS;
}

inline Status validate_payload_write(const BurstParams* burst,
                                     BurstLimits limits,
                                     int idx,
                                     const void* data,
                                     int len,
                                     size_t payload_capacity,
                                     const char* op_name) {
  Status status = validate_segment_packet_storage(
      burst, limits, 0, idx, true, false, op_name);
  if (status != Status::SUCCESS) { return status; }
  if (!strict_enabled()) { return Status::SUCCESS; }
  if (len < 0) {
    DAQIRI_LOG_ERROR("{}: payload length {} is negative", op_name, len);
    return Status::INVALID_PARAMETER;
  }
  if (len > 0 && data == nullptr) {
    DAQIRI_LOG_ERROR("{}: payload source is null for non-empty payload", op_name);
    return Status::NULL_PTR;
  }
  const size_t payload_len = static_cast<size_t>(len);
  if (payload_capacity != kUnknownCapacity && payload_len > payload_capacity) {
    DAQIRI_LOG_ERROR("{}: payload length {} exceeds backing capacity {}",
                     op_name,
                     payload_len,
                     payload_capacity);
    return Status::INVALID_PARAMETER;
  }
  return Status::SUCCESS;
}

inline Status validate_packet_lengths(const BurstParams* burst,
                                      BurstLimits limits,
                                      int idx,
                                      const std::initializer_list<int>& lens,
                                      const std::array<size_t, MAX_NUM_SEGS>& capacities,
                                      bool require_packet_ptrs,
                                      bool require_length_ptrs,
                                      size_t max_segment_len,
                                      const char* op_name,
                                      uint32_t* total_len) {
  if (total_len != nullptr) { *total_len = 0; }

  Status status = validate_packet_index(burst, limits, idx, op_name);
  if (status != Status::SUCCESS) { return status; }
  status = validate_segment_count(burst, limits, op_name);
  if (status != Status::SUCCESS) { return status; }

  if (!strict_enabled()) {
    uint64_t total = 0;
    for (int len : lens) {
      total += static_cast<uint32_t>(len);
      if (total > std::numeric_limits<uint32_t>::max()) {
        DAQIRI_LOG_ERROR("{}: total packet length {} exceeds uint32_t", op_name, total);
        return Status::INVALID_PARAMETER;
      }
    }
    if (total_len != nullptr) { *total_len = static_cast<uint32_t>(total); }
    return Status::SUCCESS;
  }

  if (lens.size() != static_cast<size_t>(limits.num_segs)) {
    DAQIRI_LOG_ERROR("{}: got {} packet length(s), expected {} segment length(s)",
                     op_name,
                     lens.size(),
                     limits.num_segs);
    return Status::INVALID_PARAMETER;
  }

  uint64_t total = 0;
  int seg = 0;
  for (int len : lens) {
    status = validate_segment_packet_storage(
        burst, limits, seg, idx, require_packet_ptrs, require_length_ptrs, op_name);
    if (status != Status::SUCCESS) { return status; }
    if (len < 0) {
      DAQIRI_LOG_ERROR("{}: packet length {} is negative for segment {}",
                       op_name,
                       len,
                       seg);
      return Status::INVALID_PARAMETER;
    }

    const size_t seg_len = static_cast<size_t>(len);
    if (seg_len > max_segment_len) {
      DAQIRI_LOG_ERROR("{}: packet length {} exceeds maximum segment length {} for segment {}",
                       op_name,
                       seg_len,
                       max_segment_len,
                       seg);
      return Status::INVALID_PARAMETER;
    }
    if (capacities[seg] != kUnknownCapacity && seg_len > capacities[seg]) {
      DAQIRI_LOG_ERROR("{}: packet length {} exceeds backing capacity {} for segment {}",
                       op_name,
                       seg_len,
                       capacities[seg],
                       seg);
      return Status::INVALID_PARAMETER;
    }
    total += seg_len;
    if (total > std::numeric_limits<uint32_t>::max()) {
      DAQIRI_LOG_ERROR("{}: total packet length {} exceeds uint32_t", op_name, total);
      return Status::INVALID_PARAMETER;
    }
    ++seg;
  }

  if (total_len != nullptr) { *total_len = static_cast<uint32_t>(total); }
  return Status::SUCCESS;
}

inline std::array<size_t, MAX_NUM_SEGS> unknown_capacities() {
  std::array<size_t, MAX_NUM_SEGS> capacities{};
  capacities.fill(kUnknownCapacity);
  return capacities;
}

}  // namespace daqiri::burst_validation
