// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cuda_runtime_api.h>

#include <cstdint>

namespace daqiri::ucx_gpu {

struct ValidationResult {
  unsigned long long error_count;
  unsigned long long sample_bad_index;
  std::uint32_t sample_expected;
  std::uint32_t sample_actual;
};

cudaError_t fill_image_async(void* device_data, std::uint64_t sequence, cudaStream_t stream);
cudaError_t validate_image_async(const void* device_data, std::uint64_t sequence,
                                 ValidationResult* device_result, cudaStream_t stream);

// Validate the deterministic DQRI raw-image pattern after the example's
// saturating scale/offset transform. Only ValidationResult is copied to the CPU.
cudaError_t validate_transformed_raw_image_async(const void* device_data, std::uint64_t sequence,
                                                 float scale, float offset,
                                                 ValidationResult* device_result,
                                                 cudaStream_t stream);

}  // namespace daqiri::ucx_gpu
