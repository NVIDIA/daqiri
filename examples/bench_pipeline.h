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

#include <cstddef>
#include <cstdint>

namespace daqiri::bench {

// How a burst's packets are assembled into the contiguous device buffer that the
// GpuWorkload then consumes.
enum class ReorderMode {
  // Out-of-order capable transports (DPDK raw, UDP): read a per-packet sequence
  // number and place each payload at slot = seq % packets_per_batch.
  SeqReorder,
  // Reliable / in-order transports (RoCE RC, TCP): place each payload at its
  // arrival index (no reshuffle). When a single already-device-resident buffer
  // makes up the batch (one large message), this is a zero-copy pass-through.
  GatherOnly,
};

// Per-RX-thread helper that turns a burst of received packets into one
// contiguous device buffer for the GpuWorkload, doing the reorder/gather (and,
// for sockets, the host->device staging) on the workload's CUDA stream so the
// kernel orders before the workload with no extra synchronization.
//
// CUDA types are hidden behind opaque void* so this header is includable from
// plain .cpp bench mains (same pattern as bench_workload.h).
//
// Usage per burst:
//   pipe.reset_batch();
//   for each packet p:
//     const void* d = staging_needed ? pipe.stage_host_packet(host_ptr, len)
//                                     : device_ptr;     // GPU-accessible src
//     pipe.add_device_packet(d);
//   const void* ordered = pipe.finish_batch();          // launches kernel
//   workload.run(ordered);
class ReorderPipeline {
public:
  ReorderPipeline() = default;
  ~ReorderPipeline();
  ReorderPipeline(const ReorderPipeline &) = delete;
  ReorderPipeline &operator=(const ReorderPipeline &) = delete;

  // packets_per_batch slots of out_payload_len bytes form the contiguous output
  // buffer. payload_byte_offset / seq_bit_offset / seq_bit_width describe the
  // packet layout (seq fields ignored in GatherOnly). staging_needed allocates a
  // device staging buffer for host sources (sockets). stream is the
  // GpuWorkload's cudaStream_t (share it); pass nullptr to disable the pipeline.
  // Returns false on CUDA error (caller may warn and continue disabled).
  bool init(ReorderMode mode, uint32_t packets_per_batch, uint32_t out_payload_len,
            uint32_t payload_byte_offset, uint16_t seq_bit_offset,
            uint8_t seq_bit_width, bool staging_needed, void *stream);

  // Begin accumulating a new batch.
  void reset_batch();

  // Record one GPU-accessible source pointer (packet base, before
  // payload_byte_offset). Ignored once packets_per_batch is reached.
  void add_device_packet(const void *dptr);

  // Copy `len` host bytes into the device staging buffer and return the device
  // pointer to feed add_device_packet(). Returns nullptr if disabled/full.
  void *stage_host_packet(const void *hptr, uint32_t len);

  uint32_t collected() const { return collected_; }

  // Launch the reorder/gather kernel for the collected packets and return the
  // contiguous ordered device buffer (packets_per_batch * out_payload_len) to
  // hand to GpuWorkload::run(). For the single-packet GatherOnly pass-through it
  // returns the source pointer directly (no kernel, no copy). Returns nullptr if
  // disabled or nothing was collected.
  const void *finish_batch();

  size_t batch_bytes() const { return batch_bytes_; }
  bool enabled() const { return ok_; }

private:
  void destroy();

  ReorderMode mode_ = ReorderMode::GatherOnly;
  bool ok_ = false;
  bool staging_needed_ = false;
  uint32_t packets_per_batch_ = 0;
  uint32_t out_payload_len_ = 0;
  uint32_t payload_byte_offset_ = 0;
  uint16_t seq_bit_offset_ = 0;
  uint8_t seq_bit_width_ = 0;
  size_t batch_bytes_ = 0;
  size_t stage_slot_bytes_ = 0;

  uint32_t collected_ = 0;
  uint32_t staged_ = 0;

  void *stream_ = nullptr;     // cudaStream_t (shared, not owned)
  void *ordered_ = nullptr;    // device output buffer (owned)
  void *staging_ = nullptr;    // device staging buffer (owned, sockets only)
  void *dev_ptrs_ = nullptr;   // device void*[packets_per_batch] (owned)
  void *host_ptrs_ = nullptr;  // host const void*[packets_per_batch] (owned)
};

} // namespace daqiri::bench
