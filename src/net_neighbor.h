/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
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
