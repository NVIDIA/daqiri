// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

#ifdef __CUDACC__
#define DAQIRI_UCX_HOST_DEVICE __host__ __device__
#else
#define DAQIRI_UCX_HOST_DEVICE
#endif

namespace daqiri::ucx_example {

// The first four uint16 pixels carry the full image sequence in little-endian
// word order. The rest deliberately depends only on the image's position in a
// 16-image source batch, so a raw TX packet buffer can be initialized once and
// reused while only its eight-byte sequence tag and DQRI header change.
DAQIRI_UCX_HOST_DEVICE inline std::uint16_t raw_image_pixel(std::uint64_t sequence,
                                                            std::uint32_t pixel_index) noexcept {
  if (pixel_index < 4) {
    return static_cast<std::uint16_t>(sequence >> (pixel_index * 16U));
  }
  const std::uint64_t image_in_batch = sequence & 15U;
  const std::uint64_t mixed = image_in_batch * 0x9e3779b97f4a7c15ULL +
                              static_cast<std::uint64_t>(pixel_index) * 0xd1b54a32d192ed03ULL;
  return static_cast<std::uint16_t>((mixed ^ (mixed >> 32U)) & 0xffffU);
}

}  // namespace daqiri::ucx_example

#undef DAQIRI_UCX_HOST_DEVICE
