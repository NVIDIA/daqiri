/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
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

#include "src/kernels.h"
#include <stdio.h>
#include <assert.h>

/**
 * @brief Simple packet reorder kernel to demonstrate reordering a batch of packets into
 *        contiguous memory
 *
 * @param out Output buffer
 * @param in Pointer to list of input packet pointers
 * @param pkt_len Length of each packet. All packets must be same length for this example
 * @param num_pkts Number of packets
 */
__global__ void simple_packet_reorder_kernel(void* __restrict__ out,
                                             const void* const* const __restrict__ in,
                                             uint16_t pkt_len, uint32_t num_pkts) {
  // Warmup
  if (out == nullptr) return;

  const int pkt_idx = blockIdx.x;
  const int len = pkt_len;
  const void* in_pkt = in[pkt_idx];

  if (pkt_idx < num_pkts) {
    for (int pos = threadIdx.x; pos < len / 4; pos += blockDim.x) {
      const uint32_t* in_ptr = static_cast<const uint32_t*>(in_pkt) + pos;
      uint32_t* out_ptr = (uint32_t*)((uint8_t*)out + pkt_idx * pkt_len) + pos;
      *out_ptr = *in_ptr;
    }
  }
}

/**
 * @brief Wrapper to launch packet reorder kernel
 *
 * @param out Output buffer
 * @param in Pointer to list of input packet pointers
 * @param pkt_len Length of each packet in bytes. Must be a multiple of 4
 * @param num_pkts Number of packets
 * @param offset Offset into packet to start
 * @param stream CUDA stream
 */
extern "C" void simple_packet_reorder(void* out, const void* const* const in, uint16_t pkt_len,
                                      uint32_t num_pkts, cudaStream_t stream) {
  simple_packet_reorder_kernel<<<num_pkts, 128, 0, stream>>>(out, in, pkt_len, num_pkts);
}

__device__ static inline uint32_t extract_bits_be(const uint8_t* data,
                                                  uint16_t bit_offset,
                                                  uint8_t bit_width) {
  uint32_t value = 0;
  for (uint8_t i = 0; i < bit_width; ++i) {
    const uint32_t bit_idx = static_cast<uint32_t>(bit_offset) + static_cast<uint32_t>(i);
    const uint8_t byte = data[bit_idx / 8U];
    const uint8_t bit_pos = static_cast<uint8_t>(7U - (bit_idx % 8U));
    value = (value << 1U) | static_cast<uint32_t>((byte >> bit_pos) & 0x1U);
  }
  return value;
}

template <typename T>
__device__ static inline uint32_t copy_vector_chunks(uint8_t* __restrict__ dst,
                                                     const uint8_t* __restrict__ src,
                                                     uint32_t len) {
  const uint32_t count = len / static_cast<uint32_t>(sizeof(T));
  auto* dst_vec = reinterpret_cast<T*>(dst);
  const auto* src_vec = reinterpret_cast<const T*>(src);

  for (uint32_t pos = static_cast<uint32_t>(threadIdx.x); pos < count; pos += blockDim.x) {
    dst_vec[pos] = src_vec[pos];
  }

  return count * static_cast<uint32_t>(sizeof(T));
}

__device__ static inline void copy_payload_vectorized(uint8_t* __restrict__ dst,
                                                      const uint8_t* __restrict__ src,
                                                      uint32_t len) {
  const uintptr_t src_addr = reinterpret_cast<uintptr_t>(src);
  const uintptr_t dst_addr = reinterpret_cast<uintptr_t>(dst);
  const uintptr_t alignment = src_addr | dst_addr;
  uint32_t copied = 0;

  if ((alignment & 0xFU) == 0U) {
    copied += copy_vector_chunks<uint4>(dst + copied, src + copied, len - copied);
  }
  if (((alignment | static_cast<uintptr_t>(copied)) & 0x7U) == 0U) {
    copied += copy_vector_chunks<uint2>(dst + copied, src + copied, len - copied);
  }
  if (((alignment | static_cast<uintptr_t>(copied)) & 0x3U) == 0U) {
    copied += copy_vector_chunks<uint32_t>(dst + copied, src + copied, len - copied);
  }

  for (uint32_t pos = copied + static_cast<uint32_t>(threadIdx.x);
       pos < len;
       pos += blockDim.x) {
    dst[pos] = src[pos];
  }
}

__global__ void packet_reorder_copy_payload_by_sequence_kernel(
    void* __restrict__ out,
    const void* const* const __restrict__ in,
    uint32_t payload_len,
    uint32_t payload_byte_offset,
    uint32_t num_pkts,
    uint16_t seq_bit_offset,
    uint8_t seq_bit_width,
    uint16_t batch_bit_offset,
    uint8_t batch_bit_width,
    uint8_t has_batch_number,
    uint32_t packets_per_batch,
    uint32_t max_slot_idx,
    uint64_t* __restrict__ batch_id_out) {
  const uint32_t pkt_idx = static_cast<uint32_t>(blockIdx.x);
  if (pkt_idx >= num_pkts) { return; }

  const auto* src_pkt = static_cast<const uint8_t*>(in[pkt_idx]);
  if (src_pkt == nullptr) { return; }

  const uint32_t seq = extract_bits_be(src_pkt, seq_bit_offset, seq_bit_width);
  if (pkt_idx == 0 && batch_id_out != nullptr) {
    const uint64_t batch_id =
        (has_batch_number != 0U)
            ? static_cast<uint64_t>(extract_bits_be(src_pkt, batch_bit_offset, batch_bit_width))
            : static_cast<uint64_t>(seq / packets_per_batch);
    *batch_id_out = batch_id;
  }

  const uint32_t slot_idx = seq % packets_per_batch;
  if (slot_idx > max_slot_idx) { return; }

  const auto* src = src_pkt + payload_byte_offset;
  auto* dst = static_cast<uint8_t*>(out) + (static_cast<size_t>(slot_idx) * payload_len);

  copy_payload_vectorized(dst, src, payload_len);
}

extern "C" void packet_reorder_copy_payload_by_sequence(void* out,
                                                        const void* const* const in,
                                                        uint32_t payload_len,
                                                        uint32_t payload_byte_offset,
                                                        uint32_t num_pkts,
                                                        uint16_t seq_bit_offset,
                                                        uint8_t seq_bit_width,
                                                        uint16_t batch_bit_offset,
                                                        uint8_t batch_bit_width,
                                                        uint8_t has_batch_number,
                                                        uint32_t packets_per_batch,
                                                        uint32_t max_slot_idx,
                                                        uint64_t* batch_id_out,
                                                        cudaStream_t stream) {
  if (out == nullptr || in == nullptr || payload_len == 0 || num_pkts == 0
      || packets_per_batch == 0) {
    return;
  }

  packet_reorder_copy_payload_by_sequence_kernel<<<num_pkts, 128, 0, stream>>>(
      out,
      in,
      payload_len,
      payload_byte_offset,
      num_pkts,
      seq_bit_offset,
      seq_bit_width,
      batch_bit_offset,
      batch_bit_width,
      has_batch_number,
      packets_per_batch,
      max_slot_idx,
      batch_id_out);
}
