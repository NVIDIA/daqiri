// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace daqiri::ibverbs_detail {

enum class BatchRelation { same, newer, older };

inline BatchRelation compare_batch(std::uint32_t candidate, std::uint32_t current,
                                   std::uint64_t id_space) {
  if (candidate == current || id_space <= 1) {
    return BatchRelation::same;
  }
  const std::uint64_t delta =
      (static_cast<std::uint64_t>(candidate) - current) & (id_space - 1U);
  return delta < id_space / 2U ? BatchRelation::newer : BatchRelation::older;
}

enum class PacketDisposition { accept, duplicate, stale };

struct PacketDecision {
  PacketDisposition disposition{PacketDisposition::stale};
  bool closed_incomplete_batch{false};
};

class ReorderBatchTracker {
 public:
  ReorderBatchTracker(std::uint32_t packets_per_batch, std::uint64_t id_space)
      : packets_per_batch_(packets_per_batch),
        id_space_(id_space),
        seen_((packets_per_batch + 63U) / 64U) {}

  PacketDecision observe(std::uint32_t batch, std::uint32_t slot) {
    bool closed_incomplete = false;
    if (current_valid_ && batch != current_) {
      if (compare_batch(batch, current_, id_space_) == BatchRelation::older) {
        return {PacketDisposition::stale, false};
      }
      last_ = current_;
      last_valid_ = true;
      clear_current();
      closed_incomplete = true;
    }

    if (!current_valid_) {
      if (last_valid_ && compare_batch(batch, last_, id_space_) != BatchRelation::newer) {
        return {PacketDisposition::stale, closed_incomplete};
      }
      current_ = batch;
      current_valid_ = true;
    }

    if (slot >= packets_per_batch_) {
      return {PacketDisposition::stale, closed_incomplete};
    }
    const std::size_t word = slot / 64U;
    const std::uint64_t mask = std::uint64_t{1} << (slot % 64U);
    if ((seen_[word] & mask) != 0U) {
      return {PacketDisposition::duplicate, closed_incomplete};
    }
    seen_[word] |= mask;
    return {PacketDisposition::accept, closed_incomplete};
  }

  void complete_batch() {
    if (current_valid_) {
      last_ = current_;
      last_valid_ = true;
    }
    clear_current();
  }

 private:
  void clear_current() {
    current_valid_ = false;
    std::fill(seen_.begin(), seen_.end(), 0U);
  }

  std::uint32_t packets_per_batch_{0};
  std::uint64_t id_space_{0};
  bool current_valid_{false};
  std::uint32_t current_{0};
  bool last_valid_{false};
  std::uint32_t last_{0};
  std::vector<std::uint64_t> seen_;
};

}  // namespace daqiri::ibverbs_detail
