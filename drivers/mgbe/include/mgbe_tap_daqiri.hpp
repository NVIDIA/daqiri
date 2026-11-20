// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "mgbe_tap.hpp"
#include <daqiri/daqiri.h>
#include <stdexcept>

namespace mgbe {
// Call from the chosen DAQIRI TX queue's owner, after latency-sensitive work.
// Sends at most one frame. Resource pressure leaves the frame in the bounded
// TAP queue for a later attempt. Every frame uses the ordinary DAQIRI TX API.
inline bool transmit_from_kernel(KernelTap& tap, int port = 0, int queue = 0) {
  if (!tap.has_tx()) return false;
  auto* burst = daqiri::create_tx_burst_params();
  if (!burst) return false;
  try {
    daqiri::set_header(burst, port, queue, 1, 1);
    const auto status = daqiri::get_tx_packet_burst(burst);
    if (status == daqiri::Status::NOT_READY || status == daqiri::Status::NO_FREE_PACKET_BUFFERS ||
        status == daqiri::Status::RESOURCE_IN_USE) {
      daqiri::free_all_packets_and_burst_tx(burst);
      return false;
    }
    if (status != daqiri::Status::SUCCESS)
      throw std::runtime_error("cannot reserve DAQIRI kernel-return TX buffer");
    if (burst->hdr.hdr.max_pkt_size < tap.max_frame_size())
      throw std::runtime_error("TAP MTU exceeds DAQIRI TX buffer capacity");
    auto* frame = daqiri::get_packet_ptr(burst, 0);
    if (!frame) throw std::runtime_error("invalid DAQIRI kernel-return TX buffer");
    size_t length = 0;
    if (!tap.receive_from_kernel(frame, burst->hdr.hdr.max_pkt_size, length)) {
      daqiri::free_all_packets_and_burst_tx(burst);
      return false;
    }
    if (daqiri::set_packet_lengths(burst, 0, {int(length)}) != daqiri::Status::SUCCESS ||
        daqiri::send_tx_burst(burst) != daqiri::Status::SUCCESS)
      throw std::runtime_error("DAQIRI kernel-return TX failed");
    return true;  // Successful submission transfers the burst to the engine.
  } catch (...) {
    daqiri::free_all_packets_and_burst_tx(burst);
    throw;
  }
}
}  // namespace mgbe
