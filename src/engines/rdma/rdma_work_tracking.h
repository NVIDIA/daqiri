// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <limits>
#include <map>
#include <unordered_map>

namespace daqiri::detail {

inline bool rdma_range_valid(uint64_t address, uint64_t length, uint64_t base, uint64_t size) {
  return address >= base && address - base <= size && length <= size - (address - base) &&
         length <= std::numeric_limits<uint64_t>::max() - address;
}

// Internal monotonically increasing SQ IDs keep caller-provided IDs independent
// of completion ordering. A signaled, fenced SQ completion retires preceding WRs.
// Each burst is delivered once, after every posted WR has retired.
template <typename Burst>
class RdmaWorkTracking {
 public:
  uint64_t next_id() const {
    return next_id_;
  }
  size_t size() const {
    return entries_.size();
  }

  void posted(Burst* burst, uint32_t packet) {
    entries_.emplace(next_id_++, Entry{burst, packet});
    ++pending_[burst].count;
  }

  void mark_failed(Burst* burst) {
    pending_.at(burst).failed = true;
  }

  template <typename PacketDone, typename BurstDone>
  bool complete_through(uint64_t id, bool failed, PacketDone packet_done, BurstDone burst_done) {
    // A completion may only retire work actually posted to this SQ.
    if (entries_.find(id) == entries_.end()) {
      return false;
    }
    auto end = entries_.upper_bound(id);
    for (auto it = entries_.begin(); it != end;) {
      const auto entry = it->second;
      auto& state = pending_.at(entry.burst);
      state.failed |= failed;
      packet_done(entry.burst, entry.packet, failed);
      const bool done = --state.count == 0;
      const bool burst_failed = state.failed;
      it = entries_.erase(it);
      if (done) {
        pending_.erase(entry.burst);
        burst_done(entry.burst, burst_failed);
      }
    }
    return true;
  }

 private:
  struct Entry {
    Burst* burst;
    uint32_t packet;
  };
  struct Pending {
    size_t count = 0;
    bool failed = false;
  };
  uint64_t next_id_ = 1;
  std::map<uint64_t, Entry> entries_;
  std::unordered_map<Burst*, Pending> pending_;
};

}  // namespace daqiri::detail
