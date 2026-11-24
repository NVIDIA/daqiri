// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <unordered_map>

namespace daqiri::detail {

inline bool rdma_range_valid(uint64_t address, uint64_t length, uint64_t base, uint64_t size) {
  return address >= base && address - base <= size && length <= size - (address - base) &&
         length <= std::numeric_limits<uint64_t>::max() - address;
}

// Internal monotonically increasing SQ IDs keep caller-provided IDs independent
// of completion ordering. A signaled SQ completion retires preceding WRs.
// Each burst is delivered once, after every posted WR has retired.
template <typename Burst>
class RdmaWorkTracking {
 public:
  struct Entry {
    Burst* burst;
    uint32_t packet;
  };
  std::optional<Entry> find(uint64_t id) const {
    const auto it = entries_.lower_bound(id);
    if (it == entries_.end() || id < it->second.first) return std::nullopt;
    const auto& range = it->second;
    return Entry{range.burst, range.packet + static_cast<uint32_t>(id - range.first)};
  }
  bool empty() const {
    return entries_.empty();
  }
  void clear() {
    entries_.clear();
    pending_.clear();
    last_read_id_ = 0;
    retired_id_ = 0;
  }
  template <typename Release>
  void reclaim(Release release) {
    for (const auto& entry : pending_) release(entry.first);
    clear();
  }
  uint64_t next_id() const {
    return next_id_;
  }
  size_t size() const {
    return entries_.empty() ? 0 : next_id_ - entries_.begin()->second.first;
  }

  bool read_pending() const {
    return last_read_id_ > retired_id_;
  }

  void posted(Burst* burst, uint32_t packet, bool is_read = false) {
    if (is_read) last_read_id_ = next_id_;
    posted_range(burst, packet, 1);
  }

  void posted_range(Burst* burst, uint32_t first_packet, uint32_t count) {
    const auto first = next_id_;
    next_id_ += count;
    entries_.emplace(next_id_ - 1, Range{first, burst, first_packet});
    pending_[burst].count += count;
  }

  void mark_failed(Burst* burst) {
    pending_.at(burst).failed = true;
  }

  template <typename PacketDone, typename BurstDone>
  bool complete_through(uint64_t id, bool failed, PacketDone packet_done, BurstDone burst_done) {
    return complete_ranges_through(
        id, failed,
        [&](Burst* burst, uint32_t first, uint32_t count, bool range_failed) {
          for (uint32_t offset = 0; offset < count; ++offset)
            packet_done(burst, first + offset, range_failed);
        },
        burst_done);
  }

  template <typename RangeDone, typename BurstDone>
  bool complete_ranges_through(uint64_t id, bool failed, RangeDone range_done,
                               BurstDone burst_done) {
    // A completion may only retire work actually posted to this SQ.
    if (!find(id)) {
      return false;
    }
    retired_id_ = id;
    for (auto it = entries_.begin(); it != entries_.end() && it->second.first <= id;) {
      auto& range = it->second;
      auto* burst = range.burst;
      auto& state = pending_.at(burst);
      state.failed |= failed;
      const uint64_t last = std::min(id, it->first);
      const auto count = static_cast<uint32_t>(last - range.first + 1);
      range_done(burst, range.packet, count, failed);
      state.count -= count;
      const bool burst_failed = state.failed;
      if (last == it->first) {
        it = entries_.erase(it);
      } else {
        range.first += count;
        range.packet += count;
        ++it;
      }
      if (state.count == 0) {
        pending_.erase(burst);
        burst_done(burst, burst_failed);
      }
    }
    return true;
  }

 private:
  struct Range {
    uint64_t first;
    Burst* burst;
    uint32_t packet;
  };
  struct Pending {
    size_t count = 0;
    bool failed = false;
  };
  uint64_t next_id_ = 1;
  uint64_t last_read_id_ = 0;
  uint64_t retired_id_ = 0;
  std::map<uint64_t, Range> entries_;
  std::unordered_map<Burst*, Pending> pending_;
};

}  // namespace daqiri::detail
