/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace daqiri::examples::gvsp {

enum class PacketKind { LEADER, DATA, TRAILER, UNSUPPORTED };

struct ImageLeader {
  uint16_t payload_type = 0;
  uint64_t timestamp_ticks = 0;
  uint32_t pixel_format = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t x_offset = 0;
  uint32_t y_offset = 0;
};

struct GvspPacketView {
  PacketKind kind = PacketKind::UNSUPPORTED;
  uint16_t status = 0;
  uint64_t block_id = 0;
  uint32_t packet_id = 0;
  bool extended_ids = false;
  bool resent = false;
  const uint8_t* data = nullptr;
  size_t data_size = 0;
  std::optional<ImageLeader> leader;
};

// Parse the conventional GVSP leader/payload/trailer packet forms. Both the legacy
// 16-bit block / 24-bit packet ID header and the extended ID header are normalized.
bool parse_gvsp_packet(const uint8_t* payload, size_t payload_size, GvspPacketView* view,
                       std::string* error = nullptr);

}  // namespace daqiri::examples::gvsp
