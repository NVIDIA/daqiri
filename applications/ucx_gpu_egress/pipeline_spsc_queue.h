// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace daqiri::ucx_example {

// Runtime-sized, bounded SPSC queue. One element is intentionally left unused
// so equality of the producer and consumer cursors means empty. The requested
// capacity is exact, and move-only descriptors are supported.
template <typename T>
class SpscQueue {
 public:
  explicit SpscQueue(std::size_t requested_capacity) : slots_(requested_capacity + 1) {
    if (requested_capacity == 0) {
      throw std::invalid_argument("SpscQueue capacity must be nonzero");
    }
  }

  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;

  bool try_push(T value) {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t next = increment(head);
    if (next == tail_.load(std::memory_order_acquire)) {
      return false;
    }
    slots_[head].emplace(std::move(value));
    head_.store(next, std::memory_order_release);
    return true;
  }

  bool try_pop(T& value) {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) {
      return false;
    }
    value = std::move(*slots_[tail]);
    slots_[tail].reset();
    tail_.store(increment(tail), std::memory_order_release);
    return true;
  }

  std::size_t depth() const noexcept {
    const std::size_t head = head_.load(std::memory_order_acquire);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    return head >= tail ? head - tail : slots_.size() - tail + head;
  }

  std::size_t capacity() const noexcept {
    return slots_.size() - 1;
  }

  bool empty() const noexcept {
    return depth() == 0;
  }

 private:
  std::size_t increment(std::size_t index) const noexcept {
    ++index;
    return index == slots_.size() ? 0 : index;
  }

  std::vector<std::optional<T>> slots_;
  alignas(64) std::atomic<std::size_t> head_{0};
  alignas(64) std::atomic<std::size_t> tail_{0};
};

}  // namespace daqiri::ucx_example
