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

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <cuda_runtime.h>

#include <daqiri/daqiri.h>

namespace daqiri::apps::resnet {

// Lightweight descriptor for one config-reordered inference batch. The reorder
// output MR buffer pool is the "image ring"; freeing the burst is the reuse
// back-edge after TRT has finished reading the previous batch.
struct InferenceJob {
  daqiri::BurstParams* burst = nullptr;  // reordered burst; free after TRT consumes it
  void* dev_input = nullptr;             // fp16 NCHW batch = get_packet_ptr(burst, 0)
  uint32_t batch_size = 0;               // images in this batch
  cudaEvent_t input_ready = nullptr;     // burst->event (reorder-kernel completion)
};

// Fixed-capacity SPSC ring (power-of-two kCap). Modeled on the reference
// InferenceQueue; no per-slot release events (back-edge is TrtRunner's
// one-batch-late sync + free of prev_burst).
template <std::size_t kCap>
class InferenceQueue {
  static_assert(kCap >= 2 && (kCap & (kCap - 1)) == 0, "kCap must be a power of two >= 2");

 public:
  bool try_push(const InferenceJob& job) {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t next = (head + 1) & (kCap - 1);
    if (next == tail_.load(std::memory_order_acquire)) return false;
    slots_[head] = job;
    head_.store(next, std::memory_order_release);
    return true;
  }

  bool try_pop(InferenceJob& job) {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) return false;
    job = slots_[tail];
    tail_.store((tail + 1) & (kCap - 1), std::memory_order_release);
    return true;
  }

  std::size_t depth() const {
    const std::size_t head = head_.load(std::memory_order_acquire);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    return (head - tail) & (kCap - 1);
  }

  bool empty() const { return depth() == 0; }
  bool full() const { return depth() == kCap - 1; }

 private:
  InferenceJob slots_[kCap]{};
  alignas(64) std::atomic<std::size_t> head_{0};
  alignas(64) std::atomic<std::size_t> tail_{0};
};

inline constexpr std::size_t kInferenceQueueCap = 8;

}  // namespace daqiri::apps::resnet
