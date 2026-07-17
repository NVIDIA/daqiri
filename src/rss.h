/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace daqiri {

// The conventional 40-byte Microsoft RSS key. Keeping the key fixed makes a
// given five-tuple map consistently across restarts and across both raw engines.
inline constexpr std::array<uint8_t, 40> kToeplitzRssKey = {
    0x6d, 0x5a, 0x56, 0xda, 0x25, 0x5b, 0x0e, 0xc2, 0x41, 0x67, 0x25, 0x3d, 0x43, 0xa3,
    0x8f, 0xb0, 0xd0, 0xca, 0x2b, 0xcb, 0xae, 0x7b, 0x30, 0xb4, 0x77, 0xcb, 0x2d, 0xa3,
    0x80, 0x30, 0xf2, 0x0c, 0x6a, 0x42, 0xb7, 0x3b, 0xbe, 0xac, 0x01, 0xfa};

inline size_t rss_table_size(size_t queue_count) {
  size_t size = 1;
  while (size < queue_count) {
    size <<= 1;
  }
  return size;
}

}  // namespace daqiri
