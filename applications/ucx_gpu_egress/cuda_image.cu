// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#include "cuda_image.h"

#include "protocol.h"
#include "raw/image_pattern.h"

#include <cuda_runtime.h>

namespace daqiri::ucx_gpu {
namespace {

__host__ __device__ std::uint16_t expected_pixel(std::uint64_t sequence, std::uint32_t index) {
  const std::uint64_t mixed =
      sequence * 0x9e3779b97f4a7c15ULL + static_cast<std::uint64_t>(index) * 0xd1b54a32d192ed03ULL;
  return static_cast<std::uint16_t>((mixed ^ (mixed >> 32U)) & 0xffffU);
}

__global__ void fill_image_kernel(std::uint16_t* pixels, std::uint64_t sequence) {
  const auto index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index < kImagePixels) {
    pixels[index] = expected_pixel(sequence, index);
  }
}

__global__ void validate_image_kernel(const std::uint16_t* pixels, std::uint64_t sequence,
                                      ValidationResult* result) {
  const auto index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= kImagePixels) {
    return;
  }
  const std::uint16_t expected = expected_pixel(sequence, index);
  const std::uint16_t actual = pixels[index];
  if (expected == actual) {
    return;
  }
  atomicAdd(&result->error_count, 1ULL);
  if (atomicCAS(&result->sample_bad_index, ~0ULL, static_cast<unsigned long long>(index)) ==
      ~0ULL) {
    result->sample_expected = expected;
    result->sample_actual = actual;
  }
}

__device__ std::uint16_t transform_pixel(std::uint16_t input, float scale, float offset) {
  float transformed = fmaf(scale, static_cast<float>(input), offset);
  transformed = fminf(65535.0F, fmaxf(0.0F, transformed));
  return static_cast<std::uint16_t>(__float2uint_rn(transformed));
}

__global__ void validate_transformed_raw_image_kernel(const std::uint16_t* pixels,
                                                      std::uint64_t sequence, float scale,
                                                      float offset, ValidationResult* result) {
  const auto index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= kImagePixels) {
    return;
  }
  const std::uint16_t expected = transform_pixel(
      ucx_example::raw_image_pixel(sequence, static_cast<std::uint32_t>(index)), scale, offset);
  const std::uint16_t actual = pixels[index];
  if (expected == actual) {
    return;
  }
  atomicAdd(&result->error_count, 1ULL);
  if (atomicCAS(&result->sample_bad_index, ~0ULL, static_cast<unsigned long long>(index)) ==
      ~0ULL) {
    result->sample_expected = expected;
    result->sample_actual = actual;
  }
}

}  // namespace

cudaError_t fill_image_async(void* device_data, std::uint64_t sequence, cudaStream_t stream) {
  constexpr unsigned int kThreads = 256;
  constexpr unsigned int kBlocks =
      static_cast<unsigned int>((kImagePixels + kThreads - 1) / kThreads);
  fill_image_kernel<<<kBlocks, kThreads, 0, stream>>>(static_cast<std::uint16_t*>(device_data),
                                                      sequence);
  return cudaGetLastError();
}

cudaError_t validate_image_async(const void* device_data, std::uint64_t sequence,
                                 ValidationResult* device_result, cudaStream_t stream) {
  constexpr unsigned int kThreads = 256;
  constexpr unsigned int kBlocks =
      static_cast<unsigned int>((kImagePixels + kThreads - 1) / kThreads);
  validate_image_kernel<<<kBlocks, kThreads, 0, stream>>>(
      static_cast<const std::uint16_t*>(device_data), sequence, device_result);
  return cudaGetLastError();
}

cudaError_t validate_transformed_raw_image_async(const void* device_data, std::uint64_t sequence,
                                                 float scale, float offset,
                                                 ValidationResult* device_result,
                                                 cudaStream_t stream) {
  constexpr unsigned int kThreads = 256;
  constexpr unsigned int kBlocks =
      static_cast<unsigned int>((kImagePixels + kThreads - 1) / kThreads);
  validate_transformed_raw_image_kernel<<<kBlocks, kThreads, 0, stream>>>(
      static_cast<const std::uint16_t*>(device_data), sequence, scale, offset, device_result);
  return cudaGetLastError();
}

}  // namespace daqiri::ucx_gpu
