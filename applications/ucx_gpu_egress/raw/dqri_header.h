// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "image_geometry.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace daqiri::ucx_example {

inline constexpr std::size_t kEtherIpv4UdpBytes = 42;
inline constexpr std::size_t kDqriHeaderBytes = 38;
inline constexpr std::size_t kPayloadOffsetBytes = 80;
inline constexpr std::size_t kFragmentPayloadBytes = 8192;
inline constexpr std::size_t kFragmentsPerBatch = 256;
inline constexpr std::size_t kImagesPerBatch = geometry::kImagesPerBatch;
inline constexpr std::size_t kFragmentsPerImage = 16;
inline constexpr std::size_t kImageBytes = geometry::kImageBytes;
inline constexpr std::size_t kBatchBytes = geometry::kBatchBytes;
inline constexpr std::size_t kRawFrameBytes = kPayloadOffsetBytes + kFragmentPayloadBytes;

static_assert(kEtherIpv4UdpBytes + kDqriHeaderBytes == kPayloadOffsetBytes);
static_assert(kFragmentsPerBatch * kFragmentPayloadBytes == kBatchBytes);
static_assert(kImagesPerBatch * kImageBytes == kBatchBytes);

struct DqriHeader {
  std::uint8_t flags = 0;
  std::uint16_t fragment_slot = 0;
  std::uint32_t batch_id = 0;
  std::uint64_t source_epoch = 0;
  std::uint32_t packet_sequence = 0;
  std::uint16_t payload_length = kFragmentPayloadBytes;
  std::uint16_t fragments_per_batch = kFragmentsPerBatch;
};

struct HeaderValidation {
  bool enforce_source_epoch = false;
  std::uint64_t expected_source_epoch = 0;
};

struct ParsedPacket {
  DqriHeader header;
  const std::uint8_t* payload = nullptr;
};

enum class HeaderStatus {
  kOk,
  kTruncated,
  kWrongFrameLength,
  kBadEtherType,
  kBadIpv4Header,
  kFragmentedIpv4,
  kBadIpv4TotalLength,
  kNotUdp,
  kBadUdpLength,
  kBadMagic,
  kUnsupportedVersion,
  kUnsupportedFlags,
  kBadHeaderLength,
  kBadHeaderCrc,
  kReservedNotZero,
  kFragmentOutOfRange,
  kZeroSourceEpoch,
  kUnexpectedSourceEpoch,
  kBadPayloadLength,
  kBadFragmentCount,
  kSequenceMismatch,
  kNullOutput,
};

const char* header_status_string(HeaderStatus status) noexcept;

// Castagnoli CRC-32C, reflected representation, initial/final XOR 0xffffffff.
std::uint32_t crc32c(const void* data, std::size_t size) noexcept;

// Compute the DQRI header CRC while treating bytes 30..33 (the serialized CRC
// field) as zero, as required by the v1 wire format.
std::uint32_t dqri_header_crc32c(const std::uint8_t* bytes, std::size_t size) noexcept;

DqriHeader make_dqri_header(std::uint32_t batch_id, std::uint64_t source_epoch,
                            std::uint16_t fragment_slot) noexcept;

HeaderStatus serialize_dqri_header(const DqriHeader& header, std::uint8_t* destination,
                                   std::size_t destination_size) noexcept;

HeaderStatus parse_dqri_header(const std::uint8_t* bytes, std::size_t size, DqriHeader* output,
                               HeaderValidation validation = {}) noexcept;

HeaderStatus parse_dqri_frame(const std::uint8_t* frame, std::size_t frame_size,
                              ParsedPacket* output, HeaderValidation validation = {}) noexcept;

enum class FragmentInsertResult { kInserted, kDuplicate, kOutOfRange };

class FragmentBitmap {
 public:
  FragmentInsertResult insert(std::uint16_t fragment_slot) noexcept;
  bool contains(std::uint16_t fragment_slot) const noexcept;
  std::size_t count() const noexcept {
    return count_;
  }
  bool complete() const noexcept {
    return count_ == kFragmentsPerBatch;
  }
  void reset() noexcept;

 private:
  std::array<std::uint64_t, 4> words_{};
  std::size_t count_ = 0;
};

enum class Serial32Relation { kOlder, kEqual, kNewer, kAmbiguous };

// RFC-1982-style comparison. A distance of exactly 2^31 is ambiguous.
Serial32Relation compare_serial32(std::uint32_t value, std::uint32_t reference) noexcept;

// Select the 64-bit instance of value nearest reference. Returns nullopt when
// that mathematical instance would be below zero or above UINT64_MAX. The
// caller must reject an ambiguous half-range jump before calling this helper.
std::optional<std::uint64_t> unwrap_serial32_near(std::uint32_t value,
                                                  std::uint64_t reference) noexcept;

}  // namespace daqiri::ucx_example
