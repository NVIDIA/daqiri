// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#include "scale_offset.h"

#include <cuda_runtime.h>

#include <cmath>

namespace daqiri::ucx_gpu {
namespace {

__global__ void scale_offset_u16_batch_kernel(std::uint16_t* data, float scale, float offset) {
  const std::size_t index = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
  if (index >= kProcessingBatchPixels) {
    return;
  }

  float value = fmaf(scale, static_cast<float>(data[index]), offset);
  value = fminf(fmaxf(value, 0.0F), 65535.0F);
  data[index] = static_cast<std::uint16_t>(__float2uint_rn(value));
}

}  // namespace

cudaError_t scale_offset_u16_batch_async(std::uint16_t* data, float scale, float offset,
                                         cudaStream_t stream) {
  if (data == nullptr || !std::isfinite(scale) || !std::isfinite(offset)) {
    return cudaErrorInvalidValue;
  }

  constexpr unsigned int kThreads = 256;
  constexpr unsigned int kBlocks =
      static_cast<unsigned int>((kProcessingBatchPixels + kThreads - 1) / kThreads);
  scale_offset_u16_batch_kernel<<<kBlocks, kThreads, 0, stream>>>(data, scale, offset);
  return cudaGetLastError();
}

}  // namespace daqiri::ucx_gpu
