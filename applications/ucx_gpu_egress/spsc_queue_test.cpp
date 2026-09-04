// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "pipeline_spsc_queue.h"

#include <atomic>
#include <cstddef>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

}  // namespace

int main() {
  daqiri::ucx_example::SpscQueue<std::unique_ptr<int>> exact(2);
  require(exact.capacity() == 2, "SPSC capacity is not exact");
  require(exact.try_push(std::make_unique<int>(1)), "first SPSC push failed");
  require(exact.try_push(std::make_unique<int>(2)), "second SPSC push failed");
  require(!exact.try_push(std::make_unique<int>(3)), "SPSC exceeded its configured capacity");
  std::unique_ptr<int> value;
  require(exact.try_pop(value) && *value == 1, "move-only SPSC pop failed");

  constexpr std::size_t kIterations = 1'000'000;
  daqiri::ucx_example::SpscQueue<std::size_t> retirements(1);
  std::atomic<unsigned> slot_state{0};  // 0=pending, 1=free/published
  std::atomic<bool> failed{false};

  std::thread consumer([&] {
    for (std::size_t expected = 0; expected < kIterations; ++expected) {
      std::size_t retired = 0;
      while (!retirements.try_pop(retired)) {
        std::this_thread::yield();
      }
      if (retired != expected || slot_state.load(std::memory_order_acquire) != 1) {
        failed.store(true, std::memory_order_release);
      }
      slot_state.store(0, std::memory_order_release);
    }
  });

  for (std::size_t sequence = 0; sequence < kIterations; ++sequence) {
    while (slot_state.load(std::memory_order_acquire) != 0) {
      std::this_thread::yield();
    }
    // This is the ordering used by flush_retirements(): the slot is made free
    // before the release-publishing SPSC push.
    slot_state.store(1, std::memory_order_release);
    while (!retirements.try_push(sequence)) {
      std::this_thread::yield();
    }
  }
  consumer.join();
  require(!failed.load(std::memory_order_acquire),
          "consumer observed retirement before the slot became free");

  std::cout << "SPSC queue and retirement publication tests passed\n";
  return 0;
}
