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

#include "bench_pipeline.h"

#include <cuda_runtime.h>

#include <cstring>
#include <iostream>
#include <new>

#include "../src/kernels.h"

namespace daqiri::bench {

namespace {
// kReorderDataTypeSame / kReorderEndiannessNetwork from src/kernels.cu — a pure
// vectorized copy (no quantization), reading the seq number network-byte-order.
constexpr uint8_t kDataTypeSame = 0;
constexpr uint8_t kEndianNetwork = 1;

cudaStream_t as_stream(void* s) {
  return static_cast<cudaStream_t>(s);
}
}  // namespace

ReorderPipeline::~ReorderPipeline() {
  destroy();
}

void ReorderPipeline::destroy() {
  if (ordered_ != nullptr) {
    cudaFree(ordered_);
    ordered_ = nullptr;
  }
  if (staging_ != nullptr) {
    cudaFree(staging_);
    staging_ = nullptr;
  }
  if (dev_ptrs_ != nullptr) {
    cudaFree(dev_ptrs_);
    dev_ptrs_ = nullptr;
  }
  if (host_ptrs_ != nullptr) {
    delete[] static_cast<const void**>(host_ptrs_);
    host_ptrs_ = nullptr;
  }
  stream_ = nullptr;  // not owned
  ok_ = false;
}

bool ReorderPipeline::init(ReorderMode mode, uint32_t packets_per_batch, uint32_t out_payload_len,
                           uint32_t payload_byte_offset, uint16_t seq_bit_offset,
                           uint8_t seq_bit_width, bool staging_needed, void* stream) {
  mode_ = mode;
  staging_needed_ = staging_needed;
  packets_per_batch_ = packets_per_batch;
  out_payload_len_ = out_payload_len;
  payload_byte_offset_ = payload_byte_offset;
  seq_bit_offset_ = seq_bit_offset;
  seq_bit_width_ = seq_bit_width;
  collected_ = 0;
  staged_ = 0;

  if (stream == nullptr || packets_per_batch == 0 || out_payload_len == 0) {
    ok_ = false;  // disabled (e.g. workload == none): inert no-op, not an error
    return true;
  }
  stream_ = stream;

  batch_bytes_ = static_cast<size_t>(packets_per_batch) * out_payload_len;
  stage_slot_bytes_ = static_cast<size_t>(payload_byte_offset) + out_payload_len;

  if (cudaMalloc(&ordered_, batch_bytes_) != cudaSuccess ||
      cudaMemset(ordered_, 0, batch_bytes_) != cudaSuccess) {
    std::cerr << "ReorderPipeline: ordered buffer alloc failed\n";
    destroy();
    return false;
  }
  if (cudaMalloc(&dev_ptrs_, static_cast<size_t>(packets_per_batch) * sizeof(const void*)) !=
      cudaSuccess) {
    std::cerr << "ReorderPipeline: pointer array alloc failed\n";
    destroy();
    return false;
  }
  host_ptrs_ = new (std::nothrow) const void*[packets_per_batch];
  if (host_ptrs_ == nullptr) {
    std::cerr << "ReorderPipeline: host pointer array alloc failed\n";
    destroy();
    return false;
  }
  if (staging_needed_) {
    if (cudaMalloc(&staging_, static_cast<size_t>(packets_per_batch) * stage_slot_bytes_) !=
        cudaSuccess) {
      std::cerr << "ReorderPipeline: staging buffer alloc failed\n";
      destroy();
      return false;
    }
  }

  ok_ = true;
  return true;
}

void ReorderPipeline::reset_batch() {
  collected_ = 0;
  staged_ = 0;
}

void ReorderPipeline::add_device_packet(const void* dptr) {
  if (!ok_ || dptr == nullptr || collected_ >= packets_per_batch_) {
    return;
  }
  static_cast<const void**>(host_ptrs_)[collected_++] = dptr;
}

void* ReorderPipeline::stage_host_packet(const void* hptr, uint32_t len) {
  if (!ok_ || !staging_needed_ || hptr == nullptr || staged_ >= packets_per_batch_) {
    return nullptr;
  }
  const uint32_t copy_len =
      len < stage_slot_bytes_ ? len : static_cast<uint32_t>(stage_slot_bytes_);
  auto* dst = static_cast<uint8_t*>(staging_) + static_cast<size_t>(staged_) * stage_slot_bytes_;
  cudaMemcpyAsync(dst, hptr, copy_len, cudaMemcpyHostToDevice, as_stream(stream_));
  ++staged_;
  return dst;
}

const void* ReorderPipeline::finish_batch() {
  if (!ok_ || collected_ == 0) {
    return nullptr;
  }

  // Single-packet in-order case (one large RoCE/TCP message): the source buffer
  // is already contiguous and device-resident, so feed it straight to the
  // workload with no kernel and no copy.
  if (mode_ == ReorderMode::GatherOnly && collected_ == 1) {
    const auto* src = static_cast<const uint8_t*>(static_cast<const void**>(host_ptrs_)[0]);
    return src + payload_byte_offset_;
  }

  cudaMemcpyAsync(dev_ptrs_, host_ptrs_, static_cast<size_t>(collected_) * sizeof(const void*),
                  cudaMemcpyHostToDevice, as_stream(stream_));

  const auto* const* in = static_cast<const void* const*>(dev_ptrs_);
  if (mode_ == ReorderMode::SeqReorder) {
    packet_reorder_copy_payload_by_sequence(
        ordered_, in, out_payload_len_, out_payload_len_, payload_byte_offset_, collected_,
        seq_bit_offset_, seq_bit_width_, /*batch_bit_offset=*/0,
        /*batch_bit_width=*/0, /*has_batch_number=*/0, packets_per_batch_,
        /*max_slot_idx=*/packets_per_batch_ - 1, kDataTypeSame, kDataTypeSame, kEndianNetwork,
        /*batch_id_out=*/nullptr, as_stream(stream_));
  } else {
    packet_gather_copy_payload(ordered_, in, out_payload_len_, payload_byte_offset_, collected_,
                               as_stream(stream_));
  }
  return ordered_;
}

}  // namespace daqiri::bench
