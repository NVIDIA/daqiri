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

#include <cstdint>

#include <cuda_runtime_api.h>

#include <daqiri/types.h>

namespace daqiri {

/**
 * @brief Snapshot of managed-burst lifecycle diagnostics.
 *
 * Outstanding counts include bursts whose release is deferred behind CUDA
 * work. The peak and total counters cover the process lifetime.
 */
struct ManagedBurstStats {
  uint64_t outstanding_rx = 0;
  uint64_t outstanding_tx = 0;
  uint64_t deferred_rx = 0;
  uint64_t peak_outstanding_rx = 0;
  uint64_t peak_outstanding_tx = 0;
  uint64_t total_rx_acquired = 0;
  uint64_t total_tx_acquired = 0;
  uint64_t total_deferred_rx = 0;
  uint64_t total_deferred_wait_ns = 0;
  uint64_t max_deferred_wait_ns = 0;
  uint64_t lifecycle_errors = 0;
};

/**
 * @brief Move-only owner for an RX BurstParams and all of its packet buffers.
 *
 * Destroying or resetting a non-empty RxBurst returns the packets and metadata
 * through the active engine's normal RX free path. Use release_on_stream() if
 * CUDA work still references the packet buffers.
 */
class RxBurst {
 public:
  RxBurst() noexcept = default;
  ~RxBurst();

  RxBurst(const RxBurst&) = delete;
  RxBurst& operator=(const RxBurst&) = delete;

  RxBurst(RxBurst&& other) noexcept;
  RxBurst& operator=(RxBurst&& other) noexcept;

  BurstParams* get() const noexcept {
    return burst_;
  }
  BurstParams* operator->() const noexcept {
    return burst_;
  }
  explicit operator bool() const noexcept {
    return burst_ != nullptr;
  }

  /** Immediately return the owned burst to DAQIRI. Safe to call repeatedly. */
  void reset() noexcept;

  /**
   * Relinquish ownership without freeing and return the raw burst pointer.
   * The caller becomes responsible for releasing it with the legacy API.
   */
  BurstParams* release() noexcept;

  /**
   * Record completion after prior work in stream and defer burst reclamation.
   * On success this object becomes empty. On failure it retains ownership.
   */
  Status release_on_stream(cudaStream_t stream) noexcept;

 private:
  friend Status get_rx_burst(RxBurst* burst, int port, int queue);
  friend Status get_rx_burst(RxBurst* burst, int port);
  friend Status get_rx_burst(RxBurst* burst);
  friend Status get_rx_burst(RxBurst* burst, uintptr_t conn_id, bool server);

  void adopt(BurstParams* burst, uint64_t generation) noexcept;

  BurstParams* burst_ = nullptr;
  uint64_t generation_ = 0;
};

/**
 * @brief Move-only owner for TX metadata and, once allocated, packet buffers.
 *
 * Use the TxBurst overload of get_tx_packet_burst() so the owner can track
 * whether packet buffers were allocated. send() accounts for each engine's
 * ownership-on-error behavior and empties this object only when consumed.
 */
class TxBurst {
 public:
  TxBurst() noexcept = default;
  ~TxBurst();

  TxBurst(const TxBurst&) = delete;
  TxBurst& operator=(const TxBurst&) = delete;

  TxBurst(TxBurst&& other) noexcept;
  TxBurst& operator=(TxBurst&& other) noexcept;

  BurstParams* get() const noexcept {
    return burst_;
  }
  BurstParams* operator->() const noexcept {
    return burst_;
  }
  explicit operator bool() const noexcept {
    return burst_ != nullptr;
  }
  bool has_packet_buffers() const noexcept {
    return has_packet_buffers_;
  }

  /** Immediately release owned TX metadata and any allocated packets. */
  void reset() noexcept;

  /** Relinquish ownership to legacy code without freeing. */
  BurstParams* release() noexcept;

  /** Submit the burst. This object becomes empty if the engine consumed it. */
  Status send() noexcept;

 private:
  friend Status create_tx_burst(TxBurst* burst);
  friend Status get_tx_packet_burst(TxBurst* burst);

  void adopt(BurstParams* burst, uint64_t generation) noexcept;

  BurstParams* burst_ = nullptr;
  uint64_t generation_ = 0;
  bool has_packet_buffers_ = false;
};

/** Managed overloads of the existing RX acquisition functions. */
Status get_rx_burst(RxBurst* burst, int port, int queue);
Status get_rx_burst(RxBurst* burst, int port);
Status get_rx_burst(RxBurst* burst);
Status get_rx_burst(RxBurst* burst, uintptr_t conn_id, bool server);

/** Allocate managed TX metadata. */
Status create_tx_burst(TxBurst* burst);

/** Allocate packet buffers for managed TX metadata. */
Status get_tx_packet_burst(TxBurst* burst);

/** Return a point-in-time lifecycle diagnostic snapshot. */
ManagedBurstStats get_managed_burst_stats() noexcept;

}  // namespace daqiri
