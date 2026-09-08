// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#include "protocol.h"

#include <algorithm>
#include <cstring>

namespace daqiri::ucx_gpu {
namespace {

constexpr std::uint32_t kControlMagic = 0x44515543U;  // DQUC
constexpr std::uint32_t kDataMagic = 0x44515547U;     // DQUG

void put_u16(std::uint8_t* out, std::uint16_t value) {
  out[0] = static_cast<std::uint8_t>(value >> 8U);
  out[1] = static_cast<std::uint8_t>(value);
}

void put_u32(std::uint8_t* out, std::uint32_t value) {
  for (int i = 3; i >= 0; --i) {
    out[3 - i] = static_cast<std::uint8_t>(value >> (i * 8));
  }
}

void put_u64(std::uint8_t* out, std::uint64_t value) {
  for (int i = 7; i >= 0; --i) {
    out[7 - i] = static_cast<std::uint8_t>(value >> (i * 8));
  }
}

std::uint16_t get_u16(const std::uint8_t* in) {
  return (static_cast<std::uint16_t>(in[0]) << 8U) | static_cast<std::uint16_t>(in[1]);
}

std::uint32_t get_u32(const std::uint8_t* in) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value = (value << 8U) | in[i];
  }
  return value;
}

std::uint64_t get_u64(const std::uint8_t* in) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value = (value << 8U) | in[i];
  }
  return value;
}

bool validate_common(const std::uint8_t* in, std::size_t actual_size, std::uint32_t expected_magic,
                     std::size_t expected_size, std::string& error) {
  if (actual_size != expected_size) {
    error = "wrong serialized header length";
    return false;
  }
  if (get_u32(in) != expected_magic) {
    error = "wrong protocol magic";
    return false;
  }
  if (get_u16(in + 4) != kProtocolMajor) {
    error = "unsupported protocol version";
    return false;
  }
  if (get_u16(in + 6) != expected_size) {
    error = "encoded header length mismatch";
    return false;
  }
  std::array<std::uint8_t, std::max(kControlWireBytes, kDataHeaderWireBytes)> copy{};
  std::copy_n(in, expected_size, copy.data());
  const std::uint32_t encoded_crc = get_u32(copy.data() + expected_size - 4);
  std::fill(copy.begin() + expected_size - 4, copy.begin() + expected_size, 0);
  if (crc32c(copy.data(), expected_size) != encoded_crc) {
    error = "header CRC32C mismatch";
    return false;
  }
  return true;
}

}  // namespace

std::uint32_t crc32c(const void* data, std::size_t size) {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::uint32_t crc = ~std::uint32_t{0};
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0x82f63b78U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

std::array<std::uint8_t, kControlWireBytes> encode_control(const ControlMessage& message) {
  std::array<std::uint8_t, kControlWireBytes> out{};
  put_u32(out.data(), kControlMagic);
  put_u16(out.data() + 4, kProtocolMajor);
  put_u16(out.data() + 6, kControlWireBytes);
  put_u16(out.data() + 8, static_cast<std::uint16_t>(message.type));
  put_u16(out.data() + 10, 0);
  put_u64(out.data() + 12, message.connection_epoch);
  put_u64(out.data() + 20, message.value0);
  put_u64(out.data() + 28, message.value1);
  put_u64(out.data() + 36, message.value2);
  put_u64(out.data() + 44, message.value3);
  put_u32(out.data() + 52, crc32c(out.data(), out.size()));
  return out;
}

bool decode_control(const void* data, std::size_t size, ControlMessage& message,
                    std::string& error) {
  if (data == nullptr || size < 12) {
    error = "truncated control header";
    return false;
  }
  const auto* in = static_cast<const std::uint8_t*>(data);
  if (!validate_common(in, size, kControlMagic, kControlWireBytes, error)) {
    return false;
  }
  const auto type = get_u16(in + 8);
  if (type < static_cast<std::uint16_t>(ControlType::hello) ||
      type > static_cast<std::uint16_t>(ControlType::eos_ack)) {
    error = "unknown control message type";
    return false;
  }
  if (get_u16(in + 10) != 0) {
    error = "unsupported control reserved bits";
    return false;
  }
  message.type = static_cast<ControlType>(type);
  message.connection_epoch = get_u64(in + 12);
  message.value0 = get_u64(in + 20);
  message.value1 = get_u64(in + 28);
  message.value2 = get_u64(in + 36);
  message.value3 = get_u64(in + 44);
  return true;
}

std::array<std::uint8_t, kDataHeaderWireBytes> encode_data_header(const DataHeader& header) {
  std::array<std::uint8_t, kDataHeaderWireBytes> out{};
  put_u32(out.data(), kDataMagic);
  put_u16(out.data() + 4, kProtocolMajor);
  put_u16(out.data() + 6, kDataHeaderWireBytes);
  put_u64(out.data() + 8, header.connection_epoch);
  put_u64(out.data() + 16, header.first_sequence);
  put_u32(out.data() + 24, header.image_count);
  put_u64(out.data() + 28, header.batch_ordinal);
  put_u32(out.data() + 36, crc32c(out.data(), out.size()));
  return out;
}

bool decode_data_header(const void* data, std::size_t size, DataHeader& header,
                        std::string& error) {
  if (data == nullptr || size < 12) {
    error = "truncated DATA header";
    return false;
  }
  const auto* in = static_cast<const std::uint8_t*>(data);
  if (!validate_common(in, size, kDataMagic, kDataHeaderWireBytes, error)) {
    return false;
  }
  header.connection_epoch = get_u64(in + 8);
  header.first_sequence = get_u64(in + 16);
  header.image_count = get_u32(in + 24);
  header.batch_ordinal = get_u64(in + 28);
  if (header.image_count == 0 || header.image_count > ucx_example::geometry::kImagesPerBatch) {
    error = "DATA image count is outside the fixed batch capacity";
    return false;
  }
  return true;
}

const char* memory_kind_name(MemoryKind kind) noexcept {
  switch (kind) {
    case MemoryKind::host_pinned_mapped:
      return "host_pinned_mapped";
    case MemoryKind::cuda_device:
      return "cuda_device";
  }
  return "unknown";
}

bool parse_memory_kind(const std::string& text, MemoryKind& kind) noexcept {
  if (text == "host_pinned_mapped") {
    kind = MemoryKind::host_pinned_mapped;
    return true;
  }
  if (text == "cuda_device") {
    kind = MemoryKind::cuda_device;
    return true;
  }
  return false;
}

}  // namespace daqiri::ucx_gpu
