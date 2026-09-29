/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <string>

#include <daqiri/types.h>

namespace daqiri {

/// Resolve the IPv4 next-hop MAC selected by Linux for `dst_host` on `netdev`.
/// `dst_host` is in host byte order. Linux routing and neighbour tables remain
/// the source of truth; this helper does not maintain a private cache.
Status resolve_ipv4_neighbor(const std::string& netdev, uint32_t dst_host, char* mac,
                             uint32_t timeout_ms);

}  // namespace daqiri
