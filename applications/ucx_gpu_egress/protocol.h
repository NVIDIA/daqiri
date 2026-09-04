// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "image_geometry.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace daqiri::ucx_gpu {

constexpr std::uint16_t kProtocolMajor = 1;
constexpr std::uint16_t kProtocolMinor = 0;
constexpr std::uint64_t kLogicalStreamId = 1;
constexpr std::uint32_t kImageSchemaId = 1;
constexpr std::size_t kImageWidth = ucx_example::geometry::kImageWidth;
constexpr std::size_t kImageHeight = ucx_example::geometry::kImageHeight;
constexpr std::size_t kImagePixels = ucx_example::geometry::kImagePixels;
constexpr std::size_t kImageBytes = ucx_example::geometry::kImageBytes;
constexpr std::uint16_t kControlAmId = 0x10;
constexpr std::uint16_t kDataAmId = 0x20;
constexpr std::size_t kControlWireBytes = 64;
constexpr std::size_t kDataHeaderWireBytes = 72;

enum class MemoryKind : std::uint32_t {
  host_pinned_mapped = 1,
  cuda_device = 2,
};

enum class ControlType : std::uint16_t {
  hello = 1,
  accept = 2,
  reject = 3,
  credit = 4,
  eos = 5,
  eos_ack = 6,
};

struct ControlMessage {
  ControlType type{ControlType::reject};
  std::uint32_t flags{0};
  std::uint64_t stream_id{kLogicalStreamId};
  std::uint64_t connection_epoch{0};
  std::uint64_t value0{0};
  std::uint64_t value1{0};
  std::uint64_t value2{0};
  MemoryKind memory_kind{MemoryKind::host_pinned_mapped};
};

struct DataHeader {
  std::uint32_t flags{0};
  std::uint64_t stream_id{kLogicalStreamId};
  std::uint64_t connection_epoch{0};
  std::uint64_t sequence{0};
  std::uint32_t payload_length{kImageBytes};
  std::uint32_t schema_id{kImageSchemaId};
  std::uint64_t timestamp_ns{0};
  std::uint32_t payload_crc32c{0};
  std::uint64_t admission_ordinal{0};
};

std::array<std::uint8_t, kControlWireBytes> encode_control(const ControlMessage& message);
bool decode_control(const void* data, std::size_t size, ControlMessage& message,
                    std::string& error);

std::array<std::uint8_t, kDataHeaderWireBytes> encode_data_header(const DataHeader& header);
bool decode_data_header(const void* data, std::size_t size, DataHeader& header, std::string& error);

std::uint32_t crc32c(const void* data, std::size_t size);
const char* memory_kind_name(MemoryKind kind) noexcept;
bool parse_memory_kind(const std::string& text, MemoryKind& kind) noexcept;

}  // namespace daqiri::ucx_gpu
