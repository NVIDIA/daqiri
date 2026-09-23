/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gvsp_packet.h"

namespace daqiri::examples::gvsp {
namespace {

constexpr uint32_t kExtendedIdMask = 0x80000000U;
constexpr uint32_t kContentTypeMask = 0x7f000000U;
constexpr uint32_t kPacketIdMask = 0x00ffffffU;
constexpr uint16_t kPacketResendStatus = 0x0100;
constexpr uint16_t kImagePayloadType = 0x0001;

uint16_t read_be16(const uint8_t* data) {
  return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

uint32_t read_be32(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

uint64_t read_be64(const uint8_t* data) {
  return (static_cast<uint64_t>(read_be32(data)) << 32) | read_be32(data + 4);
}

bool fail(std::string* error, const char* message) {
  if (error != nullptr) {
    *error = message;
  }
  return false;
}

}  // namespace

bool parse_gvsp_packet(const uint8_t* payload, size_t payload_size, GvspPacketView* view,
                       std::string* error) {
  if (payload == nullptr || view == nullptr) {
    return fail(error, "null GVSP payload or output view");
  }
  constexpr size_t kLegacyHeaderSize = 8;
  constexpr size_t kExtendedHeaderSize = 20;
  if (payload_size < kLegacyHeaderSize) {
    return fail(error, "truncated GVSP header");
  }

  GvspPacketView parsed;
  parsed.status = read_be16(payload);
  parsed.resent = parsed.status == kPacketResendStatus;
  if ((parsed.status & 0x8000U) != 0) {
    return fail(error, "GVSP packet reports an error status");
  }

  const uint32_t packet_info = read_be32(payload + 4);
  parsed.extended_ids = (packet_info & kExtendedIdMask) != 0;
  size_t header_size = kLegacyHeaderSize;
  if (parsed.extended_ids) {
    if (payload_size < kExtendedHeaderSize) {
      return fail(error, "truncated extended GVSP header");
    }
    parsed.block_id = read_be64(payload + 8);
    parsed.packet_id = read_be32(payload + 16);
    header_size = kExtendedHeaderSize;
  } else {
    parsed.block_id = read_be16(payload + 2);
    parsed.packet_id = packet_info & kPacketIdMask;
  }

  switch ((packet_info & kContentTypeMask) >> 24) {
    case 1:
      parsed.kind = PacketKind::LEADER;
      break;
    case 2:
      parsed.kind = PacketKind::TRAILER;
      break;
    case 3:
      parsed.kind = PacketKind::DATA;
      break;
    default:
      parsed.kind = PacketKind::UNSUPPORTED;
      break;
  }
  parsed.data = payload + header_size;
  parsed.data_size = payload_size - header_size;

  if (parsed.kind == PacketKind::LEADER) {
    constexpr size_t kImageLeaderSize = 32;
    if (parsed.data_size < kImageLeaderSize) {
      return fail(error, "truncated GVSP image leader");
    }
    ImageLeader leader;
    leader.payload_type = read_be16(parsed.data + 2) & 0x3fffU;
    if (leader.payload_type != kImagePayloadType) {
      parsed.kind = PacketKind::UNSUPPORTED;
    } else {
      leader.timestamp_ticks = read_be64(parsed.data + 4);
      leader.pixel_format = read_be32(parsed.data + 12);
      leader.width = read_be32(parsed.data + 16);
      leader.height = read_be32(parsed.data + 20);
      leader.x_offset = read_be32(parsed.data + 24);
      leader.y_offset = read_be32(parsed.data + 28);
      parsed.leader = leader;
    }
  }

  *view = parsed;
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

}  // namespace daqiri::examples::gvsp
