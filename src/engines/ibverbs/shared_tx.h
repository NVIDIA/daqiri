// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "src/daqiri_pool.h"
#include <daqiri/types.h>

namespace daqiri::detail {

class SharedTxPool;

// A storage owner, independent of any burst metadata or send descriptor.
struct SharedTxBuffer {
  std::atomic<uint32_t> references{0};
  SharedTxPool* pool = nullptr;
  uint8_t* address = nullptr;
  uint32_t capacity = 0;
  bool cpu_accessible = false;

  bool retain() {
    uint32_t old = references.load(std::memory_order_relaxed);
    while (old != 0 && old != std::numeric_limits<uint32_t>::max()) {
      if (references.compare_exchange_weak(old, old + 1, std::memory_order_relaxed)) return true;
    }
    return false;
  }
  void release();
};

// One independent, MPMC free pool per MR: application cancellation and worker
// completions may return different slots concurrently. No data-path allocation.
class SharedTxPool {
 public:
  ~SharedTxPool() {
    ObjectPool::free(controls_);
  }
  SharedTxPool() = default;
  SharedTxPool(const SharedTxPool&) = delete;
  SharedTxPool& operator=(const SharedTxPool&) = delete;

  bool initialize(uint8_t* base, uint32_t stride, uint32_t capacity, uint32_t count,
                  bool cpu_accessible, int numa_node) {
    if (controls_ != nullptr || base == nullptr || capacity == 0 || stride < capacity ||
        count == 0 || count >= UINT32_MAX)
      return false;
    controls_ = ObjectPool::create("ibv_shared_tx", count, sizeof(SharedTxBuffer), numa_node);
    if (controls_ == nullptr) return false;
    // Hold all controls until initialized; immediately putting each one back
    // could dequeue the same control again before the others are initialized.
    std::vector<SharedTxBuffer*> controls;
    controls.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      void* raw = nullptr;
      if (!controls_->get(&raw)) return false;
      auto* buffer = new (raw) SharedTxBuffer;
      buffer->pool = this;
      buffer->address = base + static_cast<size_t>(i) * stride;
      buffer->capacity = capacity;
      buffer->cpu_accessible = cpu_accessible;
      controls.push_back(buffer);
    }
    for (auto* buffer : controls) controls_->put(buffer);
    return true;
  }

  SharedTxBuffer* acquire() {
    void* raw = nullptr;
    if (!controls_->get(&raw)) return nullptr;
    auto* buffer = static_cast<SharedTxBuffer*>(raw);
    buffer->references.store(1, std::memory_order_relaxed);
    return buffer;
  }
  uint32_t available() const {
    return controls_->avail_count();
  }
  uint32_t in_use() const {
    return controls_->in_use_count();
  }

 private:
  friend struct SharedTxBuffer;
  ObjectPool* controls_ = nullptr;
};

inline void SharedTxBuffer::release() {
  const uint32_t old = references.fetch_sub(1, std::memory_order_release);
  assert(old != 0);
  if (old == 1) {
    std::atomic_thread_fence(std::memory_order_acquire);
    pool->controls_->put(this);
  }
}

using SharedTxPacket = std::array<SharedTxBuffer*, MAX_NUM_SEGS>;

struct SharedTxState {
  std::vector<std::unique_ptr<SharedTxPool>> pools;
  // Accepted send credits and this ordered ledger are independent of pool
  // capacity: two sends can own one payload and two sets of private headers.
  std::vector<SharedTxPacket> ledger;
  uint64_t retired = 0;  // worker-owned, monotonic wire-packet count
  std::atomic<bool> failed{false};

  bool idle_storage() const {
    for (const auto& pool : pools)
      if (pool->in_use() != 0) return false;
    return true;
  }
  bool available(uint32_t packets) const {
    for (const auto& pool : pools)
      if (pool->available() < packets) return false;
    return true;
  }
  void retire(uint64_t completed) {
    assert(completed >= retired && completed - retired <= ledger.size());
    while (retired < completed) {
      auto& packet = ledger[retired % ledger.size()];
      for (auto*& buffer : packet) {
        if (buffer != nullptr) {
          buffer->release();
          buffer = nullptr;
        }
      }
      ++retired;
    }
  }
};
}  // namespace daqiri::detail
