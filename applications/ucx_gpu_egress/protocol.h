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
constexpr std::size_t kImageWidth = ucx_example::geometry::kImageWidth;
constexpr std::size_t kImageHeight = ucx_example::geometry::kImageHeight;
constexpr std::size_t kImagePixels = ucx_example::geometry::kImagePixels;
constexpr std::size_t kImageBytes = ucx_example::geometry::kImageBytes;
constexpr std::uint16_t kControlAmId = 0x10;
constexpr std::uint16_t kDataAmId = 0x20;
constexpr std::size_t kControlWireBytes = 56;
constexpr std::size_t kDataHeaderWireBytes = 40;

enum class MemoryKind : std::uint32_t {
  host_pinned_mapped = 1,
  cuda_device = 2,
};

enum class ControlType : std::uint16_t {
  hello = 1,
  accept = 2,
  credit = 3,
  eos = 4,
  eos_ack = 5,
};

struct ControlMessage {
  ControlType type{ControlType::hello};
  std::uint64_t connection_epoch{0};
  std::uint64_t value0{0};
  std::uint64_t value1{0};
  std::uint64_t value2{0};
  std::uint64_t value3{0};
};

struct DataHeader {
  std::uint64_t connection_epoch{0};
  std::uint64_t first_sequence{0};
  std::uint32_t image_count{0};
  std::uint64_t batch_ordinal{0};
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
