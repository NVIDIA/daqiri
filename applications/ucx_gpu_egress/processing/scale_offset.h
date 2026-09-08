// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../image_geometry.h"

#include <cuda_runtime_api.h>

#include <cstddef>
#include <cstdint>

namespace daqiri::ucx_gpu {

constexpr std::size_t kProcessingImagePixels = ucx_example::geometry::kImagePixels;
constexpr std::size_t kProcessingBatchImages = ucx_example::geometry::kImagesPerBatch;
constexpr std::size_t kProcessingBatchPixels = kProcessingBatchImages * kProcessingImagePixels;
constexpr std::size_t kProcessingBatchBytes = ucx_example::geometry::kBatchBytes;

// Launches an in-place transform over exactly sixteen contiguous 256x256 uint16
// images. The function is stream-asynchronous: successful return only means the
// kernel was enqueued. The caller owns data until work on stream has completed.
// scale and offset must both be finite.
cudaError_t scale_offset_u16_batch_async(std::uint16_t* data, float scale, float offset,
                                         cudaStream_t stream);

}  // namespace daqiri::ucx_gpu
