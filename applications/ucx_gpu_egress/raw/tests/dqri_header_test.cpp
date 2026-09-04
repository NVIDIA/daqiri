// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "raw/dqri_header.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

namespace dqri = daqiri::ucx_example;

namespace {

int failures = 0;

#define CHECK(condition)                                                                    \
  do {                                                                                      \
    if (!(condition)) {                                                                     \
      std::cerr << __FILE__ << ':' << __LINE__ << ": check failed: " << #condition << '\n'; \
      ++failures;                                                                           \
    }                                                                                       \
  } while (false)

void store_u32_be(std::uint8_t* output, std::uint32_t value) {
  output[0] = static_cast<std::uint8_t>(value >> 24);
  output[1] = static_cast<std::uint8_t>(value >> 16);
  output[2] = static_cast<std::uint8_t>(value >> 8);
  output[3] = static_cast<std::uint8_t>(value);
}

void store_u16_be(std::uint8_t* output, std::uint16_t value) {
  output[0] = static_cast<std::uint8_t>(value >> 8);
  output[1] = static_cast<std::uint8_t>(value);
}

void refresh_crc(std::array<std::uint8_t, dqri::kDqriHeaderBytes>& bytes) {
  store_u32_be(bytes.data() + 30, 0);
  store_u32_be(bytes.data() + 30, dqri::dqri_header_crc32c(bytes.data(), bytes.size()));
}

void expect_status(std::array<std::uint8_t, dqri::kDqriHeaderBytes> bytes,
                   dqri::HeaderStatus expected) {
  dqri::DqriHeader parsed;
  const auto actual = dqri::parse_dqri_header(bytes.data(), bytes.size(), &parsed);
  if (actual != expected) {
    std::cerr << "expected " << dqri::header_status_string(expected) << ", got "
              << dqri::header_status_string(actual) << '\n';
    ++failures;
  }
}

}  // namespace

int main() {
  const char vector[] = "123456789";
  CHECK(dqri::crc32c(vector, 9) == 0xe3069283u);

  constexpr std::uint32_t kBatch = 0x01020304u;
  constexpr std::uint64_t kEpoch = 0x0102030405060708ull;
  constexpr std::uint16_t kSlot = 0x00abu;
  const dqri::DqriHeader original = dqri::make_dqri_header(kBatch, kEpoch, kSlot);
  std::array<std::uint8_t, dqri::kDqriHeaderBytes> bytes{};
  CHECK(dqri::serialize_dqri_header(original, bytes.data(), bytes.size()) ==
        dqri::HeaderStatus::kOk);

  CHECK(std::memcmp(bytes.data(), "DQRI", 4) == 0);
  CHECK(bytes[4] == 1);
  CHECK(bytes[5] == 0);
  CHECK(bytes[6] == 0 && bytes[7] == 38);
  CHECK(bytes[8] == 0 && bytes[9] == 0xab);
  CHECK(bytes[10] == 1 && bytes[11] == 2 && bytes[12] == 3 && bytes[13] == 4);
  CHECK(bytes[14] == 1 && bytes[21] == 8);
  CHECK(bytes[26] == 0x20 && bytes[27] == 0x00);
  CHECK(bytes[28] == 0x01 && bytes[29] == 0x00);
  CHECK(bytes[34] == 0 && bytes[35] == 0 && bytes[36] == 0 && bytes[37] == 0);

  dqri::DqriHeader parsed;
  dqri::HeaderValidation matching_epoch;
  matching_epoch.enforce_source_epoch = true;
  matching_epoch.expected_source_epoch = kEpoch;
  CHECK(dqri::parse_dqri_header(bytes.data(), bytes.size(), &parsed, matching_epoch) ==
        dqri::HeaderStatus::kOk);
  CHECK(parsed.fragment_slot == original.fragment_slot);
  CHECK(parsed.batch_id == original.batch_id);
  CHECK(parsed.source_epoch == original.source_epoch);
  CHECK(parsed.packet_sequence == original.packet_sequence);

  CHECK(dqri::parse_dqri_header(bytes.data(), bytes.size() - 1, &parsed) ==
        dqri::HeaderStatus::kTruncated);
  CHECK(dqri::parse_dqri_header(bytes.data(), bytes.size(), nullptr) ==
        dqri::HeaderStatus::kNullOutput);

  auto malformed = bytes;
  malformed[0] = 'X';
  expect_status(malformed, dqri::HeaderStatus::kBadMagic);

  malformed = bytes;
  malformed[4] = 2;
  expect_status(malformed, dqri::HeaderStatus::kUnsupportedVersion);

  malformed = bytes;
  malformed[5] = 1;
  expect_status(malformed, dqri::HeaderStatus::kUnsupportedFlags);

  malformed = bytes;
  malformed[7] = 37;
  expect_status(malformed, dqri::HeaderStatus::kBadHeaderLength);

  malformed = bytes;
  malformed[10] ^= 1;
  expect_status(malformed, dqri::HeaderStatus::kBadHeaderCrc);

  malformed = bytes;
  malformed[37] = 1;
  refresh_crc(malformed);
  expect_status(malformed, dqri::HeaderStatus::kReservedNotZero);

  malformed = bytes;
  malformed[8] = 1;
  malformed[9] = 0;
  refresh_crc(malformed);
  expect_status(malformed, dqri::HeaderStatus::kFragmentOutOfRange);

  malformed = bytes;
  std::fill(malformed.begin() + 14, malformed.begin() + 22, 0);
  refresh_crc(malformed);
  expect_status(malformed, dqri::HeaderStatus::kZeroSourceEpoch);

  malformed = bytes;
  malformed[27] = 0x01;
  refresh_crc(malformed);
  expect_status(malformed, dqri::HeaderStatus::kBadPayloadLength);

  malformed = bytes;
  malformed[29] = 0xff;
  refresh_crc(malformed);
  expect_status(malformed, dqri::HeaderStatus::kBadFragmentCount);

  malformed = bytes;
  malformed[25] ^= 1;
  refresh_crc(malformed);
  expect_status(malformed, dqri::HeaderStatus::kSequenceMismatch);

  dqri::HeaderValidation wrong_epoch;
  wrong_epoch.enforce_source_epoch = true;
  wrong_epoch.expected_source_epoch = kEpoch + 1;
  CHECK(dqri::parse_dqri_header(bytes.data(), bytes.size(), &parsed, wrong_epoch) ==
        dqri::HeaderStatus::kUnexpectedSourceEpoch);

  std::vector<std::uint8_t> frame(dqri::kRawFrameBytes);
  store_u16_be(frame.data() + 12, 0x0800);
  frame[14] = 0x45;
  store_u16_be(frame.data() + 16, dqri::kRawFrameBytes - 14);
  store_u16_be(frame.data() + 20, 0x4000);  // DF is valid; no MF/offset.
  frame[23] = 17;
  store_u16_be(frame.data() + 38, dqri::kRawFrameBytes - 34);
  std::copy(bytes.begin(), bytes.end(), frame.begin() + dqri::kEtherIpv4UdpBytes);
  dqri::ParsedPacket packet;
  CHECK(dqri::parse_dqri_frame(frame.data(), frame.size(), &packet) == dqri::HeaderStatus::kOk);
  CHECK(packet.payload == frame.data() + dqri::kPayloadOffsetBytes);
  CHECK(dqri::parse_dqri_frame(frame.data(), frame.size() - 1, &packet) ==
        dqri::HeaderStatus::kTruncated);
  frame.push_back(0);
  CHECK(dqri::parse_dqri_frame(frame.data(), frame.size(), &packet) ==
        dqri::HeaderStatus::kWrongFrameLength);
  frame.pop_back();

  auto malformed_frame = frame;
  store_u16_be(malformed_frame.data() + 12, 0x86dd);
  CHECK(dqri::parse_dqri_frame(malformed_frame.data(), malformed_frame.size(), &packet) ==
        dqri::HeaderStatus::kBadEtherType);
  malformed_frame = frame;
  malformed_frame[14] = 0x46;
  CHECK(dqri::parse_dqri_frame(malformed_frame.data(), malformed_frame.size(), &packet) ==
        dqri::HeaderStatus::kBadIpv4Header);
  malformed_frame = frame;
  store_u16_be(malformed_frame.data() + 20, 0x2000);
  CHECK(dqri::parse_dqri_frame(malformed_frame.data(), malformed_frame.size(), &packet) ==
        dqri::HeaderStatus::kFragmentedIpv4);
  malformed_frame = frame;
  store_u16_be(malformed_frame.data() + 16, dqri::kRawFrameBytes - 15);
  CHECK(dqri::parse_dqri_frame(malformed_frame.data(), malformed_frame.size(), &packet) ==
        dqri::HeaderStatus::kBadIpv4TotalLength);
  malformed_frame = frame;
  malformed_frame[23] = 6;
  CHECK(dqri::parse_dqri_frame(malformed_frame.data(), malformed_frame.size(), &packet) ==
        dqri::HeaderStatus::kNotUdp);
  malformed_frame = frame;
  store_u16_be(malformed_frame.data() + 38, dqri::kRawFrameBytes - 35);
  CHECK(dqri::parse_dqri_frame(malformed_frame.data(), malformed_frame.size(), &packet) ==
        dqri::HeaderStatus::kBadUdpLength);

  dqri::FragmentBitmap bitmap;
  for (std::uint16_t slot = 0; slot < dqri::kFragmentsPerBatch; ++slot) {
    CHECK(bitmap.insert(slot) == dqri::FragmentInsertResult::kInserted);
    CHECK(bitmap.contains(slot));
  }
  CHECK(bitmap.complete());
  CHECK(bitmap.count() == dqri::kFragmentsPerBatch);
  CHECK(bitmap.insert(17) == dqri::FragmentInsertResult::kDuplicate);
  CHECK(bitmap.insert(256) == dqri::FragmentInsertResult::kOutOfRange);
  bitmap.reset();
  CHECK(!bitmap.complete() && bitmap.count() == 0 && !bitmap.contains(17));

  CHECK(dqri::compare_serial32(0, 0xffffffffu) == dqri::Serial32Relation::kNewer);
  CHECK(dqri::compare_serial32(0xffffffffu, 0) == dqri::Serial32Relation::kOlder);
  CHECK(dqri::compare_serial32(0x80000000u, 0) == dqri::Serial32Relation::kAmbiguous);
  CHECK(dqri::unwrap_serial32_near(0, 0xffffffffull).value() == 0x100000000ull);
  CHECK(dqri::unwrap_serial32_near(0xffffffffu, 0x100000000ull).value() == 0xffffffffull);
  CHECK(!dqri::unwrap_serial32_near(0xffffffffu, 0).has_value());
  CHECK(!dqri::unwrap_serial32_near(0, std::numeric_limits<std::uint64_t>::max()).has_value());

  const auto wrapped = dqri::make_dqri_header(0x01000000u, kEpoch, 7);
  CHECK(wrapped.packet_sequence == 7);
  CHECK(dqri::serialize_dqri_header(wrapped, bytes.data(), bytes.size()) ==
        dqri::HeaderStatus::kOk);
  CHECK(dqri::parse_dqri_header(bytes.data(), bytes.size(), &parsed) == dqri::HeaderStatus::kOk);

  if (failures != 0) {
    std::cerr << failures << " DQRI header test(s) failed\n";
    return 1;
  }
  std::cout << "DQRI header, CRC32C, bitmap, and serial tests passed\n";
  return 0;
}
