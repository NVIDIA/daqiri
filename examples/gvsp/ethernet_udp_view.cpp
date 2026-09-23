/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ethernet_udp_view.h"

namespace daqiri::examples::gvsp {
namespace {

uint16_t read_be16(const uint8_t* data) {
  return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

bool fail(std::string* error, const char* message) {
  if (error != nullptr) {
    *error = message;
  }
  return false;
}

}  // namespace

bool parse_ethernet_udp(const uint8_t* frame, size_t frame_size, EthernetUdpView* view,
                        std::string* error) {
  if (frame == nullptr || view == nullptr) {
    return fail(error, "null frame or output view");
  }
  constexpr size_t kEthernetHeaderSize = 14;
  if (frame_size < kEthernetHeaderSize) {
    return fail(error, "truncated Ethernet header");
  }

  EthernetUdpView parsed;
  uint16_t ether_type = read_be16(frame + 12);
  size_t network_offset = kEthernetHeaderSize;
  if (ether_type == 0x8100 || ether_type == 0x88a8) {
    constexpr size_t kVlanTagSize = 4;
    if (frame_size < network_offset + kVlanTagSize) {
      return fail(error, "truncated VLAN header");
    }
    ether_type = read_be16(frame + network_offset + 2);
    network_offset += kVlanTagSize;
    if (ether_type == 0x8100 || ether_type == 0x88a8) {
      return fail(error, "stacked VLAN tags are not supported by this example");
    }
  }
  if (ether_type != 0x0800) {
    return fail(error, "Ethernet payload is not IPv4");
  }

  constexpr size_t kMinimumIpv4HeaderSize = 20;
  if (frame_size < network_offset + kMinimumIpv4HeaderSize) {
    return fail(error, "truncated IPv4 header");
  }
  const uint8_t version_ihl = frame[network_offset];
  if ((version_ihl >> 4) != 4) {
    return fail(error, "invalid IPv4 version");
  }
  const size_t ip_header_size = static_cast<size_t>(version_ihl & 0x0f) * 4;
  if (ip_header_size < kMinimumIpv4HeaderSize || frame_size < network_offset + ip_header_size) {
    return fail(error, "invalid or truncated IPv4 header length");
  }
  const uint16_t ip_total_size = read_be16(frame + network_offset + 2);
  if (ip_total_size < ip_header_size + 8 || frame_size < network_offset + ip_total_size) {
    return fail(error, "invalid or truncated IPv4 total length");
  }
  const uint16_t fragment = read_be16(frame + network_offset + 6);
  if ((fragment & 0x3fffU) != 0) {
    return fail(error, "fragmented IPv4 datagrams are not supported");
  }
  if (frame[network_offset + 9] != 17) {
    return fail(error, "IPv4 payload is not UDP");
  }

  const size_t udp_offset = network_offset + ip_header_size;
  const uint16_t udp_size = read_be16(frame + udp_offset + 4);
  if (udp_size < 8 || udp_offset + udp_size > network_offset + ip_total_size) {
    return fail(error, "invalid UDP length");
  }
  parsed.payload = frame + udp_offset + 8;
  parsed.payload_size = udp_size - 8;

  *view = parsed;
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

}  // namespace daqiri::examples::gvsp
