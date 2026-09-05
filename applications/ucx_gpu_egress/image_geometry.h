// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>

namespace daqiri::ucx_example::geometry {

inline constexpr std::size_t kImageWidth = 256;
inline constexpr std::size_t kImageHeight = 256;
inline constexpr std::size_t kImagePixels = kImageWidth * kImageHeight;
inline constexpr std::size_t kImageBytes = kImagePixels * sizeof(std::uint16_t);
inline constexpr std::size_t kImagesPerBatch = 16;
inline constexpr std::size_t kBatchBytes = kImagesPerBatch * kImageBytes;

}  // namespace daqiri::ucx_example::geometry
