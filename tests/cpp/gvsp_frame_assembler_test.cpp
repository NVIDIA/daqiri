/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "ethernet_udp_view.h"
#include "frame_assembler.h"
#include "gvsp_packet.h"

namespace {

using daqiri::examples::gvsp::EthernetUdpView;
using daqiri::examples::gvsp::FrameAssembler;
using daqiri::examples::gvsp::FrameAssemblerConfig;
using daqiri::examples::gvsp::GvspPacketView;
using daqiri::examples::gvsp::PacketKind;

int failures = 0;

#define CHECK(condition)                                                                 \
  do {                                                                                   \
    if (!(condition)) {                                                                  \
      std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #condition << '\n'; \
      ++failures;                                                                        \
    }                                                                                    \
  } while (false)

void put_be16(std::vector<uint8_t>& out, size_t offset, uint16_t value) {
  out[offset] = static_cast<uint8_t>(value >> 8);
  out[offset + 1] = static_cast<uint8_t>(value);
}

void put_be32(std::vector<uint8_t>& out, size_t offset, uint32_t value) {
  out[offset] = static_cast<uint8_t>(value >> 24);
  out[offset + 1] = static_cast<uint8_t>(value >> 16);
  out[offset + 2] = static_cast<uint8_t>(value >> 8);
  out[offset + 3] = static_cast<uint8_t>(value);
}

void put_be64(std::vector<uint8_t>& out, size_t offset, uint64_t value) {
  put_be32(out, offset, static_cast<uint32_t>(value >> 32));
  put_be32(out, offset + 4, static_cast<uint32_t>(value));
}

std::vector<uint8_t> make_gvsp(uint16_t block, uint8_t kind, uint32_t packet_id,
                               const std::vector<uint8_t>& data) {
  std::vector<uint8_t> packet(8 + data.size());
  put_be16(packet, 0, 0);
  put_be16(packet, 2, block);
  put_be32(packet, 4, (static_cast<uint32_t>(kind) << 24) | packet_id);
  std::copy(data.begin(), data.end(), packet.begin() + 8);
  return packet;
}

std::vector<uint8_t> make_gvsp_extended(uint64_t block, uint8_t kind, uint32_t packet_id,
                                        const std::vector<uint8_t>& data) {
  std::vector<uint8_t> packet(20 + data.size());
  put_be16(packet, 0, 0);
  put_be16(packet, 2, 0);
  put_be32(packet, 4, 0x80000000U | (static_cast<uint32_t>(kind) << 24));
  put_be64(packet, 8, block);
  put_be32(packet, 16, packet_id);
  std::copy(data.begin(), data.end(), packet.begin() + 20);
  return packet;
}

std::vector<uint8_t> make_leader(uint16_t block, uint64_t timestamp, uint32_t pixel_format,
                                 uint32_t width, uint32_t height) {
  std::vector<uint8_t> data(32);
  put_be16(data, 2, 1);
  put_be64(data, 4, timestamp);
  put_be32(data, 12, pixel_format);
  put_be32(data, 16, width);
  put_be32(data, 20, height);
  return make_gvsp(block, 1, 0, data);
}

GvspPacketView parse(const std::vector<uint8_t>& bytes) {
  GvspPacketView packet;
  std::string error;
  CHECK(daqiri::examples::gvsp::parse_gvsp_packet(bytes.data(), bytes.size(), &packet, &error));
  return packet;
}

std::vector<uint8_t> make_ethernet_udp(const std::vector<uint8_t>& payload, bool vlan = false,
                                       uint8_t ihl_words = 5) {
  const size_t ethernet_size = vlan ? 18 : 14;
  const size_t ip_size = static_cast<size_t>(ihl_words) * 4;
  const uint16_t udp_size = static_cast<uint16_t>(8 + payload.size());
  const uint16_t ip_total = static_cast<uint16_t>(ip_size + udp_size);
  std::vector<uint8_t> frame(ethernet_size + ip_total);
  for (size_t i = 0; i < 6; ++i) {
    frame[i] = static_cast<uint8_t>(i);
    frame[6 + i] = static_cast<uint8_t>(10 + i);
  }
  if (vlan) {
    put_be16(frame, 12, 0x8100);
    put_be16(frame, 14, 42);
    put_be16(frame, 16, 0x0800);
  } else {
    put_be16(frame, 12, 0x0800);
  }
  const size_t ip = ethernet_size;
  frame[ip] = static_cast<uint8_t>(0x40 | ihl_words);
  put_be16(frame, ip + 2, ip_total);
  frame[ip + 9] = 17;
  put_be32(frame, ip + 12, 0x0a000001);
  put_be32(frame, ip + 16, 0x0a000002);
  const size_t udp = ip + ip_size;
  put_be16(frame, udp, 5000);
  put_be16(frame, udp + 2, 5001);
  put_be16(frame, udp + 4, udp_size);
  std::copy(payload.begin(), payload.end(), frame.begin() + udp + 8);
  return frame;
}

void test_wire_parser() {
  const std::vector<uint8_t> payload{1, 2, 3, 4};
  for (const bool vlan : {false, true}) {
    for (const uint8_t ihl : {static_cast<uint8_t>(5), static_cast<uint8_t>(6)}) {
      auto frame = make_ethernet_udp(payload, vlan, ihl);
      EthernetUdpView view;
      std::string error;
      CHECK(daqiri::examples::gvsp::parse_ethernet_udp(frame.data(), frame.size(), &view, &error));
      CHECK(view.payload_size == payload.size());
      CHECK(std::memcmp(view.payload, payload.data(), payload.size()) == 0);
    }
  }

  auto fragmented = make_ethernet_udp(payload);
  put_be16(fragmented, 14 + 6, 0x2000);
  EthernetUdpView view;
  CHECK(!daqiri::examples::gvsp::parse_ethernet_udp(fragmented.data(), fragmented.size(), &view));
  CHECK(!daqiri::examples::gvsp::parse_ethernet_udp(fragmented.data(), 10, &view));

  auto bad_udp = make_ethernet_udp(payload);
  put_be16(bad_udp, 14 + 20 + 4, 1000);
  CHECK(!daqiri::examples::gvsp::parse_ethernet_udp(bad_udp.data(), bad_udp.size(), &view));
}

void test_gvsp_parser() {
  constexpr uint32_t kMono8 = 0x01080001;
  auto leader_bytes = make_leader(7, 123456, kMono8, 4, 2);
  auto leader = parse(leader_bytes);
  CHECK(leader.kind == PacketKind::LEADER);
  CHECK(leader.block_id == 7);
  CHECK(leader.packet_id == 0);
  CHECK(leader.leader.has_value());
  CHECK(leader.leader->timestamp_ticks == 123456);
  CHECK(leader.leader->pixel_format == kMono8);
  CHECK(leader.leader->width == 4);
  CHECK(leader.leader->height == 2);

  const auto data_bytes = make_gvsp(7, 3, 1, {1, 2, 3, 4});
  auto data = parse(data_bytes);
  CHECK(data.kind == PacketKind::DATA);
  CHECK(data.data_size == 4);
  const auto trailer_bytes = make_gvsp(7, 2, 3, std::vector<uint8_t>(8));
  auto trailer = parse(trailer_bytes);
  CHECK(trailer.kind == PacketKind::TRAILER);

  const auto extended_bytes = make_gvsp_extended(0x0102030405060708ULL, 3, 0x10203040U, {9, 8});
  auto extended = parse(extended_bytes);
  CHECK(extended.extended_ids);
  CHECK(extended.kind == PacketKind::DATA);
  CHECK(extended.block_id == 0x0102030405060708ULL);
  CHECK(extended.packet_id == 0x10203040U);
  CHECK(extended.data_size == 2);

  GvspPacketView ignored;
  CHECK(!daqiri::examples::gvsp::parse_gvsp_packet(leader_bytes.data(), 7, &ignored));
}

void test_frame_assembly() {
  constexpr uint32_t kMono8 = 0x01080001;
  FrameAssemblerConfig config;
  config.width = 4;
  config.height = 2;
  config.pixel_format = kMono8;
  config.frame_bytes = 8;
  config.data_payload_bytes = 4;
  config.frame_timeout = std::chrono::milliseconds(10);

  uint32_t completed = 0;
  uint32_t incomplete = 0;
  std::vector<uint8_t> image;
  FrameAssembler assembler(
      config,
      [&](const auto& frame) {
        ++completed;
        image.resize(frame.size_bytes);
        std::memcpy(image.data(), frame.data, frame.size_bytes);
      },
      [&](const auto&) { ++incomplete; });
  std::string error;
  CHECK(assembler.valid(&error));

  const auto now = FrameAssembler::Clock::now();
  const auto leader_bytes = make_leader(9, 77, kMono8, 4, 2);
  const auto first_bytes = make_gvsp(9, 3, 1, {1, 2, 3, 4});
  const auto second_bytes = make_gvsp(9, 3, 2, {5, 6, 7, 8});
  const auto trailer_bytes = make_gvsp(9, 2, 3, std::vector<uint8_t>(8));
  auto leader = parse(leader_bytes);
  auto first = parse(first_bytes);
  auto second = parse(second_bytes);
  auto trailer = parse(trailer_bytes);

  assembler.consume(leader, now, 100);
  assembler.consume(trailer, now, 101);
  assembler.consume(second, now, 102);
  assembler.consume(second, now, 103);
  assembler.consume(first, now, 104);
  CHECK(completed == 1);
  CHECK(image == std::vector<uint8_t>({1, 2, 3, 4, 5, 6, 7, 8}));
  CHECK(assembler.stats().duplicate_packets == 1);

  const auto leader_10_bytes = make_leader(10, 78, kMono8, 4, 2);
  const auto data_10_bytes = make_gvsp(10, 3, 1, {1, 2, 3, 4});
  assembler.consume(parse(leader_10_bytes), now);
  assembler.consume(parse(data_10_bytes), now);
  assembler.check_timeout(now + std::chrono::milliseconds(11));
  CHECK(incomplete == 1);

  const auto leader_11_bytes = make_leader(11, 79, kMono8, 4, 2);
  const auto leader_12_bytes = make_leader(12, 80, kMono8, 4, 2);
  assembler.consume(parse(leader_11_bytes), now);
  assembler.consume(parse(leader_12_bytes), now);
  CHECK(incomplete == 2);
  assembler.shutdown();
  CHECK(incomplete == 3);
}

}  // namespace

int main() {
  test_wire_parser();
  test_gvsp_parser();
  test_frame_assembly();
  if (failures != 0) {
    std::cerr << failures << " test assertion(s) failed\n";
    return 1;
  }
  std::cout << "GVSP parser and assembler tests passed\n";
  return 0;
}
