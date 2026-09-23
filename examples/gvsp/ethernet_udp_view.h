/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace daqiri::examples::gvsp {

struct EthernetUdpView {
  const uint8_t* payload = nullptr;
  size_t payload_size = 0;
};

// Parse one Ethernet II / optional single 802.1Q-or-802.1ad VLAN / IPv4 / UDP frame.
// IPv4 fragments are rejected: DAQIRI's raw path exposes L2 frames rather than a kernel
// reassembly service.
bool parse_ethernet_udp(const uint8_t* frame, size_t frame_size, EthernetUdpView* view,
                        std::string* error = nullptr);

}  // namespace daqiri::examples::gvsp
