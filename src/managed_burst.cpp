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

#include <daqiri/managed_burst.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <daqiri/common.h>
#include <daqiri/logging.hpp>

#include "managed_burst_internal.h"

namespace daqiri {
namespace {

using Clock = std::chrono::steady_clock;

struct DeferredRx {
  BurstParams* burst = nullptr;
  cudaEvent_t event = nullptr;
  int device = 0;
  uint64_t generation = 0;
  bool connection_completion = false;
  Clock::time_point queued_at;
};

struct ManagedRuntime {
  std::atomic<uint64_t> active_generation{0};
  std::atomic<uint64_t> next_generation{0};
  std::atomic<bool> stopping{false};

  std::atomic<uint64_t> outstanding_rx{0};
  std::atomic<uint64_t> outstanding_tx{0};
  std::atomic<uint64_t> deferred_rx{0};
  std::atomic<uint64_t> peak_outstanding_rx{0};
  std::atomic<uint64_t> peak_outstanding_tx{0};
  std::atomic<uint64_t> total_rx_acquired{0};
  std::atomic<uint64_t> total_tx_acquired{0};
  std::atomic<uint64_t> total_deferred_rx{0};
  std::atomic<uint64_t> total_deferred_wait_ns{0};
  std::atomic<uint64_t> max_deferred_wait_ns{0};
  std::atomic<uint64_t> lifecycle_errors{0};

  std::mutex mutex;
  std::condition_variable cv;
  std::deque<DeferredRx> pending;
  std::unordered_map<int, std::vector<cudaEvent_t>> event_pool;
  std::thread worker;
};

// Intentionally process-lifetime storage. If an application omits
// daqiri::shutdown(), a joinable static std::thread must not cause terminate()
// during C++ static destruction after the engine has already gone away.
ManagedRuntime& runtime() {
  static auto* state = new ManagedRuntime();
  return *state;
}

void update_peak(std::atomic<uint64_t>& peak, uint64_t value) noexcept {
  uint64_t current = peak.load(std::memory_order_relaxed);
  while (current < value && !peak.compare_exchange_weak(current, value, std::memory_order_relaxed,
                                                        std::memory_order_relaxed)) {
  }
}

void record_lifecycle_error(const char* message) noexcept {
  runtime().lifecycle_errors.fetch_add(1, std::memory_order_relaxed);
  DAQIRI_LOG_ERROR("Managed burst lifecycle error: {}", message);
}

bool generation_is_active(uint64_t generation) noexcept {
  return generation != 0 &&
         runtime().active_generation.load(std::memory_order_acquire) == generation;
}

void finish_rx_release(BurstParams* burst, uint64_t generation, bool deferred,
                       bool connection_completion) noexcept {
  auto& state = runtime();
  if (generation_is_active(generation)) {
    managed_rx_release(burst, connection_completion);
  } else {
    record_lifecycle_error("RX owner outlived the active DAQIRI engine; burst was not returned");
  }
  state.outstanding_rx.fetch_sub(1, std::memory_order_relaxed);
  if (deferred) {
    state.deferred_rx.fetch_sub(1, std::memory_order_relaxed);
  }
}

void finish_tx_release(BurstParams* burst, uint64_t generation, bool has_packet_buffers) noexcept {
  auto& state = runtime();
  if (generation_is_active(generation)) {
    if (has_packet_buffers) {
      free_all_packets_and_burst_tx(burst);
    } else {
      free_tx_metadata(burst);
    }
  } else {
    record_lifecycle_error("TX owner outlived the active DAQIRI engine; burst was not returned");
  }
  state.outstanding_tx.fetch_sub(1, std::memory_order_relaxed);
}

void recycle_event(int device, cudaEvent_t event) noexcept {
  auto& state = runtime();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.event_pool[device].push_back(event);
}

void record_deferred_duration(const DeferredRx& item) noexcept {
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - item.queued_at).count();
  const uint64_t wait_ns = elapsed > 0 ? static_cast<uint64_t>(elapsed) : 0;
  auto& state = runtime();
  state.total_deferred_wait_ns.fetch_add(wait_ns, std::memory_order_relaxed);
  update_peak(state.max_deferred_wait_ns, wait_ns);
}

void deferred_worker() noexcept {
  auto& state = runtime();

  for (;;) {
    DeferredRx item;
    {
      std::unique_lock<std::mutex> lock(state.mutex);
      state.cv.wait(lock, [&] {
        return state.stopping.load(std::memory_order_acquire) || !state.pending.empty();
      });
      if (state.pending.empty()) {
        if (state.stopping.load(std::memory_order_acquire)) {
          break;
        }
        continue;
      }
      item = state.pending.front();
      state.pending.pop_front();
    }

    cudaError_t cuda_status = cudaSetDevice(item.device);
    if (cuda_status == cudaSuccess) {
      if (state.stopping.load(std::memory_order_acquire)) {
        cuda_status = cudaEventSynchronize(item.event);
      } else {
        cuda_status = cudaEventQuery(item.event);
      }
    }

    if (cuda_status == cudaErrorNotReady) {
      {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.pending.push_back(item);
      }
      std::unique_lock<std::mutex> lock(state.mutex);
      state.cv.wait_for(lock, std::chrono::microseconds(50),
                        [&] { return state.stopping.load(std::memory_order_acquire); });
      continue;
    }

    if (cuda_status != cudaSuccess) {
      DAQIRI_LOG_ERROR("CUDA deferred burst release failed: {}", cudaGetErrorString(cuda_status));
      state.lifecycle_errors.fetch_add(1, std::memory_order_relaxed);
      (void)cudaEventDestroy(item.event);
      record_deferred_duration(item);
      // An unknown event state cannot prove the GPU stopped using this memory.
      // Leak the burst rather than return it to the NIC pool prematurely.
      state.outstanding_rx.fetch_sub(1, std::memory_order_relaxed);
      state.deferred_rx.fetch_sub(1, std::memory_order_relaxed);
      continue;
    }

    record_deferred_duration(item);
    finish_rx_release(item.burst, item.generation, true, item.connection_completion);
    recycle_event(item.device, item.event);
  }
}

Status enqueue_deferred_rx(BurstParams* burst, uint64_t generation, bool connection_completion,
                           cudaStream_t stream) noexcept {
  auto& state = runtime();
  if (!generation_is_active(generation) || state.stopping.load(std::memory_order_acquire)) {
    record_lifecycle_error("cannot defer RX release without an active managed-burst runtime");
    return Status::INTERNAL_ERROR;
  }

  int device = 0;
  cudaError_t cuda_status = cudaGetDevice(&device);
  if (cuda_status != cudaSuccess) {
    DAQIRI_LOG_ERROR("cudaGetDevice failed while deferring burst release: {}",
                     cudaGetErrorString(cuda_status));
    return Status::INTERNAL_ERROR;
  }

  cudaEvent_t event = nullptr;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    auto& events = state.event_pool[device];
    if (!events.empty()) {
      event = events.back();
      events.pop_back();
    }
  }
  if (event == nullptr) {
    cuda_status = cudaEventCreateWithFlags(&event, cudaEventDisableTiming);
    if (cuda_status != cudaSuccess) {
      DAQIRI_LOG_ERROR("cudaEventCreateWithFlags failed while deferring burst release: {}",
                       cudaGetErrorString(cuda_status));
      return Status::INTERNAL_ERROR;
    }
  }

  cuda_status = cudaEventRecord(event, stream);
  if (cuda_status != cudaSuccess) {
    DAQIRI_LOG_ERROR("cudaEventRecord failed while deferring burst release: {}",
                     cudaGetErrorString(cuda_status));
    recycle_event(device, event);
    return Status::INTERNAL_ERROR;
  }

  {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!generation_is_active(generation) || state.stopping.load(std::memory_order_acquire)) {
      (void)cudaEventSynchronize(event);
      state.event_pool[device].push_back(event);
      return Status::INTERNAL_ERROR;
    }
    state.deferred_rx.fetch_add(1, std::memory_order_relaxed);
    state.total_deferred_rx.fetch_add(1, std::memory_order_relaxed);
    state.pending.push_back(
        DeferredRx{burst, event, device, generation, connection_completion, Clock::now()});
    if (!state.worker.joinable()) {
      state.worker = std::thread(deferred_worker);
    }
  }
  state.cv.notify_one();
  return Status::SUCCESS;
}

void note_rx_acquired() noexcept {
  auto& state = runtime();
  const uint64_t outstanding = state.outstanding_rx.fetch_add(1, std::memory_order_relaxed) + 1;
  state.total_rx_acquired.fetch_add(1, std::memory_order_relaxed);
  update_peak(state.peak_outstanding_rx, outstanding);
}

void note_tx_acquired() noexcept {
  auto& state = runtime();
  const uint64_t outstanding = state.outstanding_tx.fetch_add(1, std::memory_order_relaxed) + 1;
  state.total_tx_acquired.fetch_add(1, std::memory_order_relaxed);
  update_peak(state.peak_outstanding_tx, outstanding);
}

}  // namespace

void managed_burst_runtime_start() {
  auto& state = runtime();
  if (state.active_generation.load(std::memory_order_acquire) != 0) {
    return;
  }
  state.stopping.store(false, std::memory_order_release);
  const uint64_t generation = state.next_generation.fetch_add(1, std::memory_order_relaxed) + 1;
  state.active_generation.store(generation, std::memory_order_release);
}

void managed_burst_runtime_shutdown() {
  auto& state = runtime();
  if (state.active_generation.load(std::memory_order_acquire) == 0) {
    return;
  }

  state.stopping.store(true, std::memory_order_release);
  state.cv.notify_all();
  if (state.worker.joinable()) {
    state.worker.join();
  }

  {
    std::lock_guard<std::mutex> lock(state.mutex);
    for (auto& entry : state.event_pool) {
      (void)cudaSetDevice(entry.first);
      for (cudaEvent_t event : entry.second) {
        (void)cudaEventDestroy(event);
      }
    }
    state.event_pool.clear();
  }

  state.active_generation.store(0, std::memory_order_release);
  const uint64_t live_rx = state.outstanding_rx.load(std::memory_order_relaxed);
  const uint64_t live_tx = state.outstanding_tx.load(std::memory_order_relaxed);
  if (live_rx != 0 || live_tx != 0) {
    state.lifecycle_errors.fetch_add(1, std::memory_order_relaxed);
    DAQIRI_LOG_ERROR(
        "DAQIRI shutdown with managed bursts still held by the application: RX={} TX={}", live_rx,
        live_tx);
  }
}

uint64_t managed_burst_runtime_generation() noexcept {
  return runtime().active_generation.load(std::memory_order_acquire);
}

RxBurst::~RxBurst() {
  reset();
}

RxBurst::RxBurst(RxBurst&& other) noexcept
    : burst_(std::exchange(other.burst_, nullptr)),
      generation_(std::exchange(other.generation_, 0)),
      connection_completion_(std::exchange(other.connection_completion_, false)) {}

RxBurst& RxBurst::operator=(RxBurst&& other) noexcept {
  if (this != &other) {
    reset();
    burst_ = std::exchange(other.burst_, nullptr);
    generation_ = std::exchange(other.generation_, 0);
    connection_completion_ = std::exchange(other.connection_completion_, false);
  }
  return *this;
}

void RxBurst::adopt(BurstParams* burst, uint64_t generation, bool connection_completion) noexcept {
  burst_ = burst;
  generation_ = generation;
  connection_completion_ = connection_completion;
  note_rx_acquired();
}

void RxBurst::reset() noexcept {
  BurstParams* burst = std::exchange(burst_, nullptr);
  const uint64_t generation = std::exchange(generation_, 0);
  const bool connection_completion = std::exchange(connection_completion_, false);
  if (burst != nullptr) {
    finish_rx_release(burst, generation, false, connection_completion);
  }
}

BurstParams* RxBurst::release() noexcept {
  BurstParams* burst = std::exchange(burst_, nullptr);
  generation_ = 0;
  connection_completion_ = false;
  if (burst != nullptr) {
    runtime().outstanding_rx.fetch_sub(1, std::memory_order_relaxed);
  }
  return burst;
}

Status RxBurst::release_on_stream(cudaStream_t stream) noexcept {
  if (burst_ == nullptr) {
    return Status::NULL_PTR;
  }
  const Status status = enqueue_deferred_rx(burst_, generation_, connection_completion_, stream);
  if (status == Status::SUCCESS) {
    burst_ = nullptr;
    generation_ = 0;
    connection_completion_ = false;
  }
  return status;
}

TxBurst::~TxBurst() {
  reset();
}

TxBurst::TxBurst(TxBurst&& other) noexcept
    : burst_(std::exchange(other.burst_, nullptr)),
      generation_(std::exchange(other.generation_, 0)),
      has_packet_buffers_(std::exchange(other.has_packet_buffers_, false)) {}

TxBurst& TxBurst::operator=(TxBurst&& other) noexcept {
  if (this != &other) {
    reset();
    burst_ = std::exchange(other.burst_, nullptr);
    generation_ = std::exchange(other.generation_, 0);
    has_packet_buffers_ = std::exchange(other.has_packet_buffers_, false);
  }
  return *this;
}

void TxBurst::adopt(BurstParams* burst, uint64_t generation) noexcept {
  burst_ = burst;
  generation_ = generation;
  has_packet_buffers_ = false;
  note_tx_acquired();
}

void TxBurst::reset() noexcept {
  BurstParams* burst = std::exchange(burst_, nullptr);
  const uint64_t generation = std::exchange(generation_, 0);
  const bool has_packets = std::exchange(has_packet_buffers_, false);
  if (burst != nullptr) {
    finish_tx_release(burst, generation, has_packets);
  }
}

BurstParams* TxBurst::release() noexcept {
  BurstParams* burst = std::exchange(burst_, nullptr);
  generation_ = 0;
  has_packet_buffers_ = false;
  if (burst != nullptr) {
    runtime().outstanding_tx.fetch_sub(1, std::memory_order_relaxed);
  }
  return burst;
}

Status TxBurst::send() noexcept {
  if (burst_ == nullptr) {
    return Status::NULL_PTR;
  }
  if (!has_packet_buffers_) {
    return Status::INVALID_PARAMETER;
  }

  const Status status = send_tx_burst(burst_);
  if (managed_tx_send_consumed(status)) {
    burst_ = nullptr;
    generation_ = 0;
    has_packet_buffers_ = false;
    runtime().outstanding_tx.fetch_sub(1, std::memory_order_relaxed);
  }
  return status;
}

Status get_rx_burst(RxBurst* burst, int port, int queue) {
  if (burst == nullptr) {
    return Status::NULL_PTR;
  }
  if (*burst) {
    return Status::INVALID_PARAMETER;
  }
  BurstParams* raw = nullptr;
  const Status status = get_rx_burst(&raw, port, queue);
  if (status != Status::SUCCESS) {
    return status;
  }
  if (raw == nullptr) {
    return Status::INTERNAL_ERROR;
  }
  const uint64_t generation = managed_burst_runtime_generation();
  if (generation == 0) {
    managed_rx_release(raw, false);
    return Status::INTERNAL_ERROR;
  }
  burst->adopt(raw, generation, false);
  return Status::SUCCESS;
}

Status get_rx_burst(RxBurst* burst, int port) {
  if (burst == nullptr) {
    return Status::NULL_PTR;
  }
  if (*burst) {
    return Status::INVALID_PARAMETER;
  }
  BurstParams* raw = nullptr;
  const Status status = get_rx_burst(&raw, port);
  if (status != Status::SUCCESS) {
    return status;
  }
  if (raw == nullptr) {
    return Status::INTERNAL_ERROR;
  }
  const uint64_t generation = managed_burst_runtime_generation();
  if (generation == 0) {
    managed_rx_release(raw, false);
    return Status::INTERNAL_ERROR;
  }
  burst->adopt(raw, generation, false);
  return Status::SUCCESS;
}

Status get_rx_burst(RxBurst* burst) {
  if (burst == nullptr) {
    return Status::NULL_PTR;
  }
  if (*burst) {
    return Status::INVALID_PARAMETER;
  }
  BurstParams* raw = nullptr;
  const Status status = get_rx_burst(&raw);
  if (status != Status::SUCCESS) {
    return status;
  }
  if (raw == nullptr) {
    return Status::INTERNAL_ERROR;
  }
  const uint64_t generation = managed_burst_runtime_generation();
  if (generation == 0) {
    managed_rx_release(raw, false);
    return Status::INTERNAL_ERROR;
  }
  burst->adopt(raw, generation, false);
  return Status::SUCCESS;
}

Status get_rx_burst(RxBurst* burst, uintptr_t conn_id, bool server) {
  if (burst == nullptr) {
    return Status::NULL_PTR;
  }
  if (*burst) {
    return Status::INVALID_PARAMETER;
  }
  BurstParams* raw = nullptr;
  const Status status = get_rx_burst(&raw, conn_id, server);
  if (status != Status::SUCCESS) {
    return status;
  }
  if (raw == nullptr) {
    return Status::INTERNAL_ERROR;
  }
  const uint64_t generation = managed_burst_runtime_generation();
  if (generation == 0) {
    managed_rx_release(raw, true);
    return Status::INTERNAL_ERROR;
  }
  burst->adopt(raw, generation, true);
  return Status::SUCCESS;
}

Status create_tx_burst(TxBurst* burst) {
  if (burst == nullptr) {
    return Status::NULL_PTR;
  }
  if (*burst) {
    return Status::INVALID_PARAMETER;
  }
  BurstParams* raw = create_tx_burst_params();
  if (raw == nullptr) {
    return Status::NO_FREE_BURST_BUFFERS;
  }
  const uint64_t generation = managed_burst_runtime_generation();
  if (generation == 0) {
    free_tx_metadata(raw);
    return Status::INTERNAL_ERROR;
  }
  burst->adopt(raw, generation);
  return Status::SUCCESS;
}

Status get_tx_packet_burst(TxBurst* burst) {
  if (burst == nullptr || !*burst) {
    return Status::NULL_PTR;
  }
  if (burst->has_packet_buffers_) {
    return Status::INVALID_PARAMETER;
  }
  const Status status = get_tx_packet_burst(burst->burst_);
  if (status == Status::SUCCESS) {
    burst->has_packet_buffers_ = true;
  }
  return status;
}

ManagedBurstStats get_managed_burst_stats() noexcept {
  auto& state = runtime();
  ManagedBurstStats stats;
  stats.outstanding_rx = state.outstanding_rx.load(std::memory_order_relaxed);
  stats.outstanding_tx = state.outstanding_tx.load(std::memory_order_relaxed);
  stats.deferred_rx = state.deferred_rx.load(std::memory_order_relaxed);
  stats.peak_outstanding_rx = state.peak_outstanding_rx.load(std::memory_order_relaxed);
  stats.peak_outstanding_tx = state.peak_outstanding_tx.load(std::memory_order_relaxed);
  stats.total_rx_acquired = state.total_rx_acquired.load(std::memory_order_relaxed);
  stats.total_tx_acquired = state.total_tx_acquired.load(std::memory_order_relaxed);
  stats.total_deferred_rx = state.total_deferred_rx.load(std::memory_order_relaxed);
  stats.total_deferred_wait_ns = state.total_deferred_wait_ns.load(std::memory_order_relaxed);
  stats.max_deferred_wait_ns = state.max_deferred_wait_ns.load(std::memory_order_relaxed);
  stats.lifecycle_errors = state.lifecycle_errors.load(std::memory_order_relaxed);
  return stats;
}

}  // namespace daqiri
