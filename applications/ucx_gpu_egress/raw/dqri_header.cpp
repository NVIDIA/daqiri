// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "raw/dqri_header.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace daqiri::ucx_example {
namespace {

constexpr std::uint8_t kMagic[4] = {'D', 'Q', 'R', 'I'};
constexpr std::uint8_t kVersion = 1;
constexpr std::size_t kCrcOffset = 30;
constexpr std::size_t kReservedOffset = 34;
constexpr std::uint32_t kCrc32cPolynomial = 0x82f63b78u;

constexpr std::uint32_t crc32c_table_entry(std::uint32_t value) noexcept {
  for (unsigned bit = 0; bit < 8; ++bit) {
    const std::uint32_t mask = 0u - (value & 1u);
    value = (value >> 1) ^ (kCrc32cPolynomial & mask);
  }
  return value;
}

constexpr std::array<std::uint32_t, 256> make_crc32c_table() noexcept {
  std::array<std::uint32_t, 256> result{};
  for (std::size_t i = 0; i < result.size(); ++i) {
    result[i] = crc32c_table_entry(static_cast<std::uint32_t>(i));
  }
  return result;
}

constexpr auto kCrc32cTable = make_crc32c_table();

void store_u16_be(std::uint8_t* output, std::uint16_t value) noexcept {
  output[0] = static_cast<std::uint8_t>(value >> 8);
  output[1] = static_cast<std::uint8_t>(value);
}

void store_u32_be(std::uint8_t* output, std::uint32_t value) noexcept {
  output[0] = static_cast<std::uint8_t>(value >> 24);
  output[1] = static_cast<std::uint8_t>(value >> 16);
  output[2] = static_cast<std::uint8_t>(value >> 8);
  output[3] = static_cast<std::uint8_t>(value);
}

void store_u64_be(std::uint8_t* output, std::uint64_t value) noexcept {
  store_u32_be(output, static_cast<std::uint32_t>(value >> 32));
  store_u32_be(output + 4, static_cast<std::uint32_t>(value));
}

std::uint16_t load_u16_be(const std::uint8_t* input) noexcept {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(input[0]) << 8) |
                                    static_cast<std::uint16_t>(input[1]));
}

std::uint32_t load_u32_be(const std::uint8_t* input) noexcept {
  return (static_cast<std::uint32_t>(input[0]) << 24) |
         (static_cast<std::uint32_t>(input[1]) << 16) |
         (static_cast<std::uint32_t>(input[2]) << 8) | static_cast<std::uint32_t>(input[3]);
}

std::uint64_t load_u64_be(const std::uint8_t* input) noexcept {
  return (static_cast<std::uint64_t>(load_u32_be(input)) << 32) |
         static_cast<std::uint64_t>(load_u32_be(input + 4));
}

std::uint32_t crc32c_update(std::uint32_t crc, std::uint8_t byte) noexcept {
  return (crc >> 8) ^ kCrc32cTable[(crc ^ byte) & 0xffu];
}

HeaderStatus validate_fields(const DqriHeader& header, HeaderValidation validation) noexcept {
  if (header.flags != 0) {
    return HeaderStatus::kUnsupportedFlags;
  }
  if (header.fragment_slot >= kFragmentsPerBatch) {
    return HeaderStatus::kFragmentOutOfRange;
  }
  if (header.source_epoch == 0) {
    return HeaderStatus::kZeroSourceEpoch;
  }
  if (validation.enforce_source_epoch && header.source_epoch != validation.expected_source_epoch) {
    return HeaderStatus::kUnexpectedSourceEpoch;
  }
  if (header.payload_length != kFragmentPayloadBytes) {
    return HeaderStatus::kBadPayloadLength;
  }
  if (header.fragments_per_batch != kFragmentsPerBatch) {
    return HeaderStatus::kBadFragmentCount;
  }
  const std::uint32_t expected_sequence =
      header.batch_id * static_cast<std::uint32_t>(kFragmentsPerBatch) + header.fragment_slot;
  if (header.packet_sequence != expected_sequence) {
    return HeaderStatus::kSequenceMismatch;
  }
  return HeaderStatus::kOk;
}

}  // namespace

const char* header_status_string(HeaderStatus status) noexcept {
  switch (status) {
    case HeaderStatus::kOk:
      return "ok";
    case HeaderStatus::kTruncated:
      return "truncated";
    case HeaderStatus::kWrongFrameLength:
      return "wrong frame length";
    case HeaderStatus::kBadEtherType:
      return "EtherType is not IPv4";
    case HeaderStatus::kBadIpv4Header:
      return "expected IPv4 with a 20-byte header";
    case HeaderStatus::kFragmentedIpv4:
      return "fragmented IPv4 is unsupported";
    case HeaderStatus::kBadIpv4TotalLength:
      return "bad IPv4 total length";
    case HeaderStatus::kNotUdp:
      return "IPv4 payload is not UDP";
    case HeaderStatus::kBadUdpLength:
      return "bad UDP length";
    case HeaderStatus::kBadMagic:
      return "bad magic";
    case HeaderStatus::kUnsupportedVersion:
      return "unsupported version";
    case HeaderStatus::kUnsupportedFlags:
      return "unsupported flags";
    case HeaderStatus::kBadHeaderLength:
      return "bad header length";
    case HeaderStatus::kBadHeaderCrc:
      return "bad header CRC32C";
    case HeaderStatus::kReservedNotZero:
      return "reserved field is not zero";
    case HeaderStatus::kFragmentOutOfRange:
      return "fragment slot out of range";
    case HeaderStatus::kZeroSourceEpoch:
      return "zero source epoch";
    case HeaderStatus::kUnexpectedSourceEpoch:
      return "unexpected source epoch";
    case HeaderStatus::kBadPayloadLength:
      return "bad payload length";
    case HeaderStatus::kBadFragmentCount:
      return "bad fragment count";
    case HeaderStatus::kSequenceMismatch:
      return "packet sequence mismatch";
    case HeaderStatus::kNullOutput:
      return "null output";
  }
  return "unknown header status";
}

std::uint32_t crc32c(const void* data, std::size_t size) noexcept {
  if (data == nullptr && size != 0) {
    return 0;
  }
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::uint32_t crc = std::numeric_limits<std::uint32_t>::max();
  for (std::size_t i = 0; i < size; ++i) {
    crc = crc32c_update(crc, bytes[i]);
  }
  return ~crc;
}

std::uint32_t dqri_header_crc32c(const std::uint8_t* bytes, std::size_t size) noexcept {
  if (bytes == nullptr || size < kDqriHeaderBytes) {
    return 0;
  }
  std::uint32_t crc = std::numeric_limits<std::uint32_t>::max();
  for (std::size_t i = 0; i < kDqriHeaderBytes; ++i) {
    const std::uint8_t byte = (i >= kCrcOffset && i < kCrcOffset + 4) ? 0 : bytes[i];
    crc = crc32c_update(crc, byte);
  }
  return ~crc;
}

DqriHeader make_dqri_header(std::uint32_t batch_id, std::uint64_t source_epoch,
                            std::uint16_t fragment_slot) noexcept {
  DqriHeader result;
  result.fragment_slot = fragment_slot;
  result.batch_id = batch_id;
  result.source_epoch = source_epoch;
  result.packet_sequence =
      batch_id * static_cast<std::uint32_t>(kFragmentsPerBatch) + fragment_slot;
  return result;
}

HeaderStatus serialize_dqri_header(const DqriHeader& header, std::uint8_t* destination,
                                   std::size_t destination_size) noexcept {
  if (destination == nullptr) {
    return HeaderStatus::kNullOutput;
  }
  if (destination_size < kDqriHeaderBytes) {
    return HeaderStatus::kTruncated;
  }
  const HeaderStatus field_status = validate_fields(header, {});
  if (field_status != HeaderStatus::kOk) {
    return field_status;
  }

  std::memset(destination, 0, kDqriHeaderBytes);
  std::memcpy(destination, kMagic, sizeof(kMagic));
  destination[4] = kVersion;
  destination[5] = header.flags;
  store_u16_be(destination + 6, kDqriHeaderBytes);
  store_u16_be(destination + 8, header.fragment_slot);
  store_u32_be(destination + 10, header.batch_id);
  store_u64_be(destination + 14, header.source_epoch);
  store_u32_be(destination + 22, header.packet_sequence);
  store_u16_be(destination + 26, header.payload_length);
  store_u16_be(destination + 28, header.fragments_per_batch);
  store_u32_be(destination + kCrcOffset, dqri_header_crc32c(destination, kDqriHeaderBytes));
  return HeaderStatus::kOk;
}

HeaderStatus parse_dqri_header(const std::uint8_t* bytes, std::size_t size, DqriHeader* output,
                               HeaderValidation validation) noexcept {
  if (output == nullptr) {
    return HeaderStatus::kNullOutput;
  }
  if (bytes == nullptr || size < kDqriHeaderBytes) {
    return HeaderStatus::kTruncated;
  }
  if (std::memcmp(bytes, kMagic, sizeof(kMagic)) != 0) {
    return HeaderStatus::kBadMagic;
  }
  if (bytes[4] != kVersion) {
    return HeaderStatus::kUnsupportedVersion;
  }
  if (bytes[5] != 0) {
    return HeaderStatus::kUnsupportedFlags;
  }
  if (load_u16_be(bytes + 6) != kDqriHeaderBytes) {
    return HeaderStatus::kBadHeaderLength;
  }
  if (load_u32_be(bytes + kCrcOffset) != dqri_header_crc32c(bytes, kDqriHeaderBytes)) {
    return HeaderStatus::kBadHeaderCrc;
  }
  if (load_u32_be(bytes + kReservedOffset) != 0) {
    return HeaderStatus::kReservedNotZero;
  }

  DqriHeader parsed;
  parsed.flags = bytes[5];
  parsed.fragment_slot = load_u16_be(bytes + 8);
  parsed.batch_id = load_u32_be(bytes + 10);
  parsed.source_epoch = load_u64_be(bytes + 14);
  parsed.packet_sequence = load_u32_be(bytes + 22);
  parsed.payload_length = load_u16_be(bytes + 26);
  parsed.fragments_per_batch = load_u16_be(bytes + 28);
  const HeaderStatus field_status = validate_fields(parsed, validation);
  if (field_status != HeaderStatus::kOk) {
    return field_status;
  }
  *output = parsed;
  return HeaderStatus::kOk;
}

HeaderStatus parse_dqri_frame(const std::uint8_t* frame, std::size_t frame_size,
                              ParsedPacket* output, HeaderValidation validation) noexcept {
  if (output == nullptr) {
    return HeaderStatus::kNullOutput;
  }
  if (frame == nullptr || frame_size < kRawFrameBytes) {
    return HeaderStatus::kTruncated;
  }
  if (frame_size != kRawFrameBytes) {
    return HeaderStatus::kWrongFrameLength;
  }

  // V1 deliberately fixes the outer envelope so that byte 42 is unambiguous:
  // untagged Ethernet-II, IPv4 with IHL=5, and UDP. Checksums and addresses are
  // left to the NIC/flow configuration, but lengths and fragmentation are part
  // of this parser's framing contract.
  if (load_u16_be(frame + 12) != 0x0800) {
    return HeaderStatus::kBadEtherType;
  }
  if (frame[14] != 0x45) {
    return HeaderStatus::kBadIpv4Header;
  }
  if ((load_u16_be(frame + 20) & 0x3fffu) != 0) {
    return HeaderStatus::kFragmentedIpv4;
  }
  if (load_u16_be(frame + 16) != kRawFrameBytes - 14) {
    return HeaderStatus::kBadIpv4TotalLength;
  }
  if (frame[23] != 17) {
    return HeaderStatus::kNotUdp;
  }
  if (load_u16_be(frame + 38) != kRawFrameBytes - 34) {
    return HeaderStatus::kBadUdpLength;
  }

  DqriHeader header;
  const HeaderStatus status =
      parse_dqri_header(frame + kEtherIpv4UdpBytes, kDqriHeaderBytes, &header, validation);
  if (status != HeaderStatus::kOk) {
    return status;
  }
  output->header = header;
  output->payload = frame + kPayloadOffsetBytes;
  return HeaderStatus::kOk;
}

FragmentInsertResult FragmentBitmap::insert(std::uint16_t fragment_slot) noexcept {
  if (fragment_slot >= kFragmentsPerBatch) {
    return FragmentInsertResult::kOutOfRange;
  }
  const std::size_t word_index = fragment_slot / 64;
  const std::uint64_t bit = std::uint64_t{1} << (fragment_slot % 64);
  if ((words_[word_index] & bit) != 0) {
    return FragmentInsertResult::kDuplicate;
  }
  words_[word_index] |= bit;
  ++count_;
  return FragmentInsertResult::kInserted;
}

bool FragmentBitmap::contains(std::uint16_t fragment_slot) const noexcept {
  if (fragment_slot >= kFragmentsPerBatch) {
    return false;
  }
  const std::size_t word_index = fragment_slot / 64;
  const std::uint64_t bit = std::uint64_t{1} << (fragment_slot % 64);
  return (words_[word_index] & bit) != 0;
}

void FragmentBitmap::reset() noexcept {
  words_.fill(0);
  count_ = 0;
}

Serial32Relation compare_serial32(std::uint32_t value, std::uint32_t reference) noexcept {
  if (value == reference) {
    return Serial32Relation::kEqual;
  }
  const std::uint32_t distance = value - reference;
  if (distance == 0x80000000u) {
    return Serial32Relation::kAmbiguous;
  }
  return distance < 0x80000000u ? Serial32Relation::kNewer : Serial32Relation::kOlder;
}

std::optional<std::uint64_t> unwrap_serial32_near(std::uint32_t value,
                                                  std::uint64_t reference) noexcept {
  constexpr std::uint64_t kModulus = std::uint64_t{1} << 32;
  constexpr std::uint64_t kHalfRange = std::uint64_t{1} << 31;
  std::uint64_t candidate = (reference & ~(kModulus - 1)) | value;
  if (candidate > reference && candidate - reference > kHalfRange) {
    if (candidate < kModulus) {
      return std::nullopt;
    }
    candidate -= kModulus;
  } else if (reference > candidate && reference - candidate > kHalfRange) {
    if (candidate > std::numeric_limits<std::uint64_t>::max() - kModulus) {
      return std::nullopt;
    }
    candidate += kModulus;
  }
  return candidate;
}

}  // namespace daqiri::ucx_example
