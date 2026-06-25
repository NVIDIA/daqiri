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
#include <cuda_bf16.h>
#include <cuda_fp16.h>

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

enum ReorderKernelDataType : uint8_t {
  kReorderDataTypeSame = 0,
  kReorderDataTypeInt4 = 1,
  kReorderDataTypeInt8 = 2,
  kReorderDataTypeInt16 = 3,
  kReorderDataTypeInt32 = 4,
  kReorderDataTypeFp16 = 5,
  kReorderDataTypeBf16 = 6,
  kReorderDataTypeFp32 = 7,
  kReorderDataTypeFp64 = 8,
};

enum ReorderKernelEndianness : uint8_t {
  kReorderEndiannessHost = 0,
  kReorderEndiannessNetwork = 1,
};

__device__ static inline int32_t sign_extend_4(uint8_t value) {
  const int32_t low = static_cast<int32_t>(value & 0x0FU);
  return (low & 0x8) != 0 ? (low | ~0x0F) : low;
}

__device__ static inline bool is_network_endianness(uint8_t input_endianness) {
  return input_endianness == kReorderEndiannessNetwork;
}

__device__ static inline uint16_t bswap16(uint16_t value) {
  return static_cast<uint16_t>(__byte_perm(static_cast<uint32_t>(value), 0U, 0x4401U));
}

__device__ static inline uint32_t bswap32(uint32_t value) {
  return __byte_perm(value, 0U, 0x0123U);
}

__device__ static inline uint32_t bswap16_lanes(uint32_t value) {
  return __byte_perm(value, 0U, 0x2301U);
}

template <uint8_t InputType>
struct ReorderInputReader;

template <>
struct ReorderInputReader<kReorderDataTypeInt4> {
  static constexpr uint32_t kBits = 4;

  __device__ static inline int32_t read(const uint8_t* src,
                                        uint32_t element_idx,
                                        uint8_t input_endianness) {
    (void)input_endianness;
    const uint8_t packed = src[element_idx >> 1U];
    const uint8_t nibble =
        (element_idx & 0x1U) == 0U ? static_cast<uint8_t>(packed >> 4U)
                                   : static_cast<uint8_t>(packed & 0x0FU);
    return sign_extend_4(nibble);
  }
};

template <>
struct ReorderInputReader<kReorderDataTypeInt8> {
  static constexpr uint32_t kBits = 8;

  __device__ static inline int32_t read(const uint8_t* src,
                                        uint32_t element_idx,
                                        uint8_t input_endianness) {
    (void)input_endianness;
    return static_cast<int32_t>(reinterpret_cast<const int8_t*>(src)[element_idx]);
  }
};

template <>
struct ReorderInputReader<kReorderDataTypeInt16> {
  static constexpr uint32_t kBits = 16;

  __device__ static inline int32_t read(const uint8_t* src,
                                        uint32_t element_idx,
                                        uint8_t input_endianness) {
    const uint32_t byte_idx = element_idx * 2U;
    uint16_t value =
        static_cast<uint16_t>(src[byte_idx]) | (static_cast<uint16_t>(src[byte_idx + 1U]) << 8U);
    if (is_network_endianness(input_endianness)) { value = bswap16(value); }
    return static_cast<int32_t>(static_cast<int16_t>(value));
  }
};

template <>
struct ReorderInputReader<kReorderDataTypeInt32> {
  static constexpr uint32_t kBits = 32;

  __device__ static inline int32_t read(const uint8_t* src,
                                        uint32_t element_idx,
                                        uint8_t input_endianness) {
    const uint32_t byte_idx = element_idx * 4U;
    uint32_t value = static_cast<uint32_t>(src[byte_idx])
                     | (static_cast<uint32_t>(src[byte_idx + 1U]) << 8U)
                     | (static_cast<uint32_t>(src[byte_idx + 2U]) << 16U)
                     | (static_cast<uint32_t>(src[byte_idx + 3U]) << 24U);
    if (is_network_endianness(input_endianness)) { value = bswap32(value); }
    return static_cast<int32_t>(value);
  }
};

template <uint8_t OutputType>
struct ReorderOutputWriter;

template <>
struct ReorderOutputWriter<kReorderDataTypeFp16> {
  __device__ static inline void write(uint8_t* dst, uint32_t element_idx, int32_t value) {
    reinterpret_cast<__half*>(dst)[element_idx] = __float2half_rn(static_cast<float>(value));
  }
};

template <>
struct ReorderOutputWriter<kReorderDataTypeBf16> {
  __device__ static inline void write(uint8_t* dst, uint32_t element_idx, int32_t value) {
    reinterpret_cast<__nv_bfloat16*>(dst)[element_idx] =
        __float2bfloat16_rn(static_cast<float>(value));
  }
};

template <>
struct ReorderOutputWriter<kReorderDataTypeFp32> {
  __device__ static inline void write(uint8_t* dst, uint32_t element_idx, int32_t value) {
    reinterpret_cast<float*>(dst)[element_idx] = static_cast<float>(value);
  }
};

template <>
struct ReorderOutputWriter<kReorderDataTypeFp64> {
  __device__ static inline void write(uint8_t* dst, uint32_t element_idx, int32_t value) {
    reinterpret_cast<double*>(dst)[element_idx] = static_cast<double>(value);
  }
};

template <>
struct ReorderOutputWriter<kReorderDataTypeInt32> {
  __device__ static inline void write(uint8_t* dst, uint32_t element_idx, int32_t value) {
    reinterpret_cast<int32_t*>(dst)[element_idx] = value;
  }
};

template <uint8_t InputType, uint8_t OutputType>
__device__ static inline void convert_reorder_value(uint8_t* dst,
                                                    const uint8_t* src,
                                                    uint32_t element_idx,
                                                    uint8_t input_endianness) {
  const int32_t value = ReorderInputReader<InputType>::read(src, element_idx, input_endianness);
  ReorderOutputWriter<OutputType>::write(dst, element_idx, value);
}

template <uint8_t InputAlignment>
__device__ static inline uint32_t load_u32_aligned_or_bytes(const uint8_t* src,
                                                            const uint8_t* read_floor,
                                                            const uint8_t* read_end) {
  if constexpr (InputAlignment == 0U) {
    return *reinterpret_cast<const uint32_t*>(src);
  }

  const uintptr_t src_addr = reinterpret_cast<uintptr_t>(src);
  const uintptr_t aligned_addr = src_addr & ~static_cast<uintptr_t>(0x3U);
  const auto* aligned = reinterpret_cast<const uint8_t*>(aligned_addr);

  if (aligned >= read_floor && aligned + 8 <= read_end) {
    const uint32_t lower = *reinterpret_cast<const uint32_t*>(aligned);
    const uint32_t upper = *reinterpret_cast<const uint32_t*>(aligned + 4);
    if constexpr (InputAlignment == 1U) {
      return __byte_perm(lower, upper, 0x4321U);
    } else if constexpr (InputAlignment == 2U) {
      return __byte_perm(lower, upper, 0x5432U);
    } else {
      return __byte_perm(lower, upper, 0x6543U);
    }
  }

  return static_cast<uint32_t>(src[0]) | (static_cast<uint32_t>(src[1]) << 8U)
         | (static_cast<uint32_t>(src[2]) << 16U) | (static_cast<uint32_t>(src[3]) << 24U);
}

template <uint8_t InputType, uint8_t OutputType, uint8_t InputAlignment>
struct ReorderPayloadConverter;

template <uint8_t OutputType, uint8_t InputAlignment>
struct ReorderPayloadConverter<kReorderDataTypeInt4, OutputType, InputAlignment> {
  __device__ static inline void convert(uint8_t* __restrict__ dst,
                                        const uint8_t* __restrict__ src,
                                        const uint8_t* __restrict__ read_floor,
                                        const uint8_t* __restrict__ read_end,
                                        uint8_t input_endianness,
                                        uint32_t input_payload_len) {
    (void)input_endianness;
    const uint32_t word_count = input_payload_len / 4U;

    for (uint32_t word_idx = static_cast<uint32_t>(threadIdx.x);
         word_idx < word_count;
         word_idx += blockDim.x) {
      const uint32_t word =
          load_u32_aligned_or_bytes<InputAlignment>(src + (word_idx * 4U), read_floor, read_end);
      const uint32_t base_element_idx = word_idx * 8U;

#pragma unroll
      for (uint32_t byte_idx = 0; byte_idx < 4U; ++byte_idx) {
        const uint8_t byte = static_cast<uint8_t>((word >> (byte_idx * 8U)) & 0xFFU);
        const uint32_t element_idx = base_element_idx + (byte_idx * 2U);
        ReorderOutputWriter<OutputType>::write(dst, element_idx, sign_extend_4(byte >> 4U));
        ReorderOutputWriter<OutputType>::write(dst, element_idx + 1U, sign_extend_4(byte));
      }
    }

    const uint32_t first_tail_element = word_count * 8U;
    const uint32_t element_count = input_payload_len * 2U;
    for (uint32_t element_idx = first_tail_element + static_cast<uint32_t>(threadIdx.x);
         element_idx < element_count;
         element_idx += blockDim.x) {
      convert_reorder_value<kReorderDataTypeInt4, OutputType>(
          dst, src, element_idx, input_endianness);
    }
  }
};

template <uint8_t OutputType, uint8_t InputAlignment>
struct ReorderPayloadConverter<kReorderDataTypeInt8, OutputType, InputAlignment> {
  __device__ static inline void convert(uint8_t* __restrict__ dst,
                                        const uint8_t* __restrict__ src,
                                        const uint8_t* __restrict__ read_floor,
                                        const uint8_t* __restrict__ read_end,
                                        uint8_t input_endianness,
                                        uint32_t input_payload_len) {
    (void)input_endianness;
    const uint32_t word_count = input_payload_len / 4U;

    for (uint32_t word_idx = static_cast<uint32_t>(threadIdx.x);
         word_idx < word_count;
         word_idx += blockDim.x) {
      const uint32_t word =
          load_u32_aligned_or_bytes<InputAlignment>(src + (word_idx * 4U), read_floor, read_end);
      const uint32_t base_element_idx = word_idx * 4U;

#pragma unroll
      for (uint32_t byte_idx = 0; byte_idx < 4U; ++byte_idx) {
        const int32_t value =
            static_cast<int32_t>(static_cast<int8_t>((word >> (byte_idx * 8U)) & 0xFFU));
        ReorderOutputWriter<OutputType>::write(dst, base_element_idx + byte_idx, value);
      }
    }

    const uint32_t first_tail_element = word_count * 4U;
    for (uint32_t element_idx = first_tail_element + static_cast<uint32_t>(threadIdx.x);
         element_idx < input_payload_len;
         element_idx += blockDim.x) {
      convert_reorder_value<kReorderDataTypeInt8, OutputType>(
          dst, src, element_idx, input_endianness);
    }
  }
};

template <uint8_t OutputType, uint8_t InputAlignment>
struct ReorderPayloadConverter<kReorderDataTypeInt16, OutputType, InputAlignment> {
  __device__ static inline void convert(uint8_t* __restrict__ dst,
                                        const uint8_t* __restrict__ src,
                                        const uint8_t* __restrict__ read_floor,
                                        const uint8_t* __restrict__ read_end,
                                        uint8_t input_endianness,
                                        uint32_t input_payload_len) {
    const uint32_t word_count = input_payload_len / 4U;

    for (uint32_t word_idx = static_cast<uint32_t>(threadIdx.x);
         word_idx < word_count;
         word_idx += blockDim.x) {
      const uint32_t word =
          load_u32_aligned_or_bytes<InputAlignment>(src + (word_idx * 4U), read_floor, read_end);
      const uint32_t base_element_idx = word_idx * 2U;
      const uint32_t ordered_word =
          is_network_endianness(input_endianness) ? bswap16_lanes(word) : word;
      const uint16_t first_raw = static_cast<uint16_t>(ordered_word & 0xFFFFU);
      const uint16_t second_raw = static_cast<uint16_t>((ordered_word >> 16U) & 0xFFFFU);
      const int32_t first_value = static_cast<int32_t>(static_cast<int16_t>(first_raw));
      const int32_t second_value = static_cast<int32_t>(static_cast<int16_t>(second_raw));
      ReorderOutputWriter<OutputType>::write(dst, base_element_idx, first_value);
      ReorderOutputWriter<OutputType>::write(dst, base_element_idx + 1U, second_value);
    }

    const uint32_t first_tail_element = word_count * 2U;
    const uint32_t element_count = input_payload_len / 2U;
    for (uint32_t element_idx = first_tail_element + static_cast<uint32_t>(threadIdx.x);
         element_idx < element_count;
         element_idx += blockDim.x) {
      convert_reorder_value<kReorderDataTypeInt16, OutputType>(
          dst, src, element_idx, input_endianness);
    }
  }
};

template <uint8_t OutputType, uint8_t InputAlignment>
struct ReorderPayloadConverter<kReorderDataTypeInt32, OutputType, InputAlignment> {
  __device__ static inline void convert(uint8_t* __restrict__ dst,
                                        const uint8_t* __restrict__ src,
                                        const uint8_t* __restrict__ read_floor,
                                        const uint8_t* __restrict__ read_end,
                                        uint8_t input_endianness,
                                        uint32_t input_payload_len) {
    const uint32_t word_count = input_payload_len / 4U;

    for (uint32_t word_idx = static_cast<uint32_t>(threadIdx.x);
         word_idx < word_count;
         word_idx += blockDim.x) {
      const uint32_t word =
          load_u32_aligned_or_bytes<InputAlignment>(src + (word_idx * 4U), read_floor, read_end);
      const uint32_t value = is_network_endianness(input_endianness) ? bswap32(word) : word;
      ReorderOutputWriter<OutputType>::write(dst, word_idx, static_cast<int32_t>(value));
    }
  }
};

template <uint8_t InputType, uint8_t OutputType, uint8_t InputAlignment>
__device__ static inline void convert_reorder_payload_typed(uint8_t* __restrict__ dst,
                                                            const uint8_t* __restrict__ src,
                                                            const uint8_t* __restrict__ read_floor,
                                                            const uint8_t* __restrict__ read_end,
                                                            uint8_t input_endianness,
                                                            uint32_t input_payload_len) {
  ReorderPayloadConverter<InputType, OutputType, InputAlignment>::convert(
      dst, src, read_floor, read_end, input_endianness, input_payload_len);
}

template <uint8_t InputType, uint8_t InputAlignment>
__device__ static inline void convert_reorder_payload_for_input(uint8_t* __restrict__ dst,
                                                                const uint8_t* __restrict__ src,
                                                                const uint8_t* __restrict__ read_floor,
                                                                const uint8_t* __restrict__ read_end,
                                                                uint8_t input_endianness,
                                                                uint32_t input_payload_len,
                                                                uint8_t output_data_type) {
  switch (output_data_type) {
    case kReorderDataTypeFp16:
      convert_reorder_payload_typed<InputType, kReorderDataTypeFp16, InputAlignment>(
          dst, src, read_floor, read_end, input_endianness, input_payload_len);
      break;
    case kReorderDataTypeBf16:
      convert_reorder_payload_typed<InputType, kReorderDataTypeBf16, InputAlignment>(
          dst, src, read_floor, read_end, input_endianness, input_payload_len);
      break;
    case kReorderDataTypeFp32:
      convert_reorder_payload_typed<InputType, kReorderDataTypeFp32, InputAlignment>(
          dst, src, read_floor, read_end, input_endianness, input_payload_len);
      break;
    case kReorderDataTypeFp64:
      convert_reorder_payload_typed<InputType, kReorderDataTypeFp64, InputAlignment>(
          dst, src, read_floor, read_end, input_endianness, input_payload_len);
      break;
    case kReorderDataTypeInt32:
      convert_reorder_payload_typed<InputType, kReorderDataTypeInt32, InputAlignment>(
          dst, src, read_floor, read_end, input_endianness, input_payload_len);
      break;
    default:
      break;
  }
}

template <uint8_t InputAlignment>
__device__ static inline void convert_reorder_payload_for_alignment(
    uint8_t* __restrict__ dst,
    const uint8_t* __restrict__ src,
    uint32_t input_payload_len,
    uint8_t input_data_type,
    uint8_t output_data_type,
    uint8_t input_endianness,
    const uint8_t* __restrict__ read_floor) {
  const auto* read_end = src + input_payload_len;
  switch (input_data_type) {
    case kReorderDataTypeInt4:
      convert_reorder_payload_for_input<kReorderDataTypeInt4, InputAlignment>(
          dst, src, read_floor, read_end, input_endianness, input_payload_len, output_data_type);
      break;
    case kReorderDataTypeInt8:
      convert_reorder_payload_for_input<kReorderDataTypeInt8, InputAlignment>(
          dst, src, read_floor, read_end, input_endianness, input_payload_len, output_data_type);
      break;
    case kReorderDataTypeInt16:
      convert_reorder_payload_for_input<kReorderDataTypeInt16, InputAlignment>(
          dst, src, read_floor, read_end, input_endianness, input_payload_len, output_data_type);
      break;
    case kReorderDataTypeInt32:
      convert_reorder_payload_for_input<kReorderDataTypeInt32, InputAlignment>(
          dst, src, read_floor, read_end, input_endianness, input_payload_len, output_data_type);
      break;
    default:
      break;
  }
}

__device__ static inline void convert_reorder_payload(uint8_t* __restrict__ dst,
                                                      const uint8_t* __restrict__ src,
                                                      uint32_t input_payload_len,
                                                      uint8_t input_data_type,
                                                      uint8_t output_data_type,
                                                      uint8_t input_endianness,
                                                      const uint8_t* __restrict__ read_floor) {
  switch (static_cast<uint8_t>(reinterpret_cast<uintptr_t>(src) & 0x3U)) {
    case 0U:
      convert_reorder_payload_for_alignment<0U>(
          dst, src, input_payload_len, input_data_type, output_data_type, input_endianness, read_floor);
      break;
    case 1U:
      convert_reorder_payload_for_alignment<1U>(
          dst, src, input_payload_len, input_data_type, output_data_type, input_endianness, read_floor);
      break;
    case 2U:
      convert_reorder_payload_for_alignment<2U>(
          dst, src, input_payload_len, input_data_type, output_data_type, input_endianness, read_floor);
      break;
    default:
      convert_reorder_payload_for_alignment<3U>(
          dst, src, input_payload_len, input_data_type, output_data_type, input_endianness, read_floor);
      break;
  }
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
    uint32_t input_payload_len,
    uint32_t output_payload_len,
    uint32_t payload_byte_offset,
    uint32_t num_pkts,
    uint16_t seq_bit_offset,
    uint8_t seq_bit_width,
    uint16_t batch_bit_offset,
    uint8_t batch_bit_width,
    uint8_t has_batch_number,
    uint32_t packets_per_batch,
    uint32_t max_slot_idx,
    uint8_t input_data_type,
    uint8_t output_data_type,
    uint8_t input_endianness,
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
  auto* dst = static_cast<uint8_t*>(out) + (static_cast<size_t>(slot_idx) * output_payload_len);

  if (input_data_type == kReorderDataTypeSame || output_data_type == kReorderDataTypeSame) {
    copy_payload_vectorized(dst, src, input_payload_len);
    return;
  }

  convert_reorder_payload(
      dst, src, input_payload_len, input_data_type, output_data_type, input_endianness, src_pkt);
}

extern "C" void packet_reorder_copy_payload_by_sequence(void* out,
                                                        const void* const* const in,
                                                        uint32_t input_payload_len,
                                                        uint32_t output_payload_len,
                                                        uint32_t payload_byte_offset,
                                                        uint32_t num_pkts,
                                                        uint16_t seq_bit_offset,
                                                        uint8_t seq_bit_width,
                                                        uint16_t batch_bit_offset,
                                                        uint8_t batch_bit_width,
                                                        uint8_t has_batch_number,
                                                        uint32_t packets_per_batch,
                                                        uint32_t max_slot_idx,
                                                        uint8_t input_data_type,
                                                        uint8_t output_data_type,
                                                        uint8_t input_endianness,
                                                        uint64_t* batch_id_out,
                                                        cudaStream_t stream) {
  if (out == nullptr || in == nullptr || input_payload_len == 0 || output_payload_len == 0
      || num_pkts == 0 || packets_per_batch == 0) {
    return;
  }

  packet_reorder_copy_payload_by_sequence_kernel<<<num_pkts, 128, 0, stream>>>(
      out,
      in,
      input_payload_len,
      output_payload_len,
      payload_byte_offset,
      num_pkts,
      seq_bit_offset,
      seq_bit_width,
      batch_bit_offset,
      batch_bit_width,
      has_batch_number,
      packets_per_batch,
      max_slot_idx,
      input_data_type,
      output_data_type,
      input_endianness,
      batch_id_out);
}

__global__ void packet_gather_copy_payload_kernel(void* __restrict__ out,
                                                  const void* const* const __restrict__ in,
                                                  uint32_t payload_len,
                                                  uint32_t payload_byte_offset, uint32_t num_pkts) {
  const uint32_t pkt_idx = static_cast<uint32_t>(blockIdx.x);
  if (pkt_idx >= num_pkts) {
    return;
  }

  const auto* src_pkt = static_cast<const uint8_t*>(in[pkt_idx]);
  if (src_pkt == nullptr) {
    return;
  }

  const auto* src = src_pkt + payload_byte_offset;
  auto* dst = static_cast<uint8_t*>(out) + (static_cast<size_t>(pkt_idx) * payload_len);
  copy_payload_vectorized(dst, src, payload_len);
}

extern "C" void packet_gather_copy_payload(void* out, const void* const* const in,
                                           uint32_t payload_len, uint32_t payload_byte_offset,
                                           uint32_t num_pkts, cudaStream_t stream) {
  if (out == nullptr || in == nullptr || payload_len == 0 || num_pkts == 0) {
    return;
  }

  packet_gather_copy_payload_kernel<<<num_pkts, 128, 0, stream>>>(out, in, payload_len,
                                                                  payload_byte_offset, num_pkts);
}
