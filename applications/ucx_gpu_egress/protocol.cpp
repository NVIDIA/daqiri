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
  if (get_u16(in + 4) != kProtocolMajor || get_u16(in + 6) != kProtocolMinor) {
    error = "unsupported protocol version";
    return false;
  }
  if (get_u16(in + 8) != expected_size) {
    error = "encoded header length mismatch";
    return false;
  }
  std::array<std::uint8_t, kDataHeaderWireBytes> copy{};
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
  put_u16(out.data() + 6, kProtocolMinor);
  put_u16(out.data() + 8, kControlWireBytes);
  put_u16(out.data() + 10, static_cast<std::uint16_t>(message.type));
  put_u32(out.data() + 12, message.flags);
  put_u64(out.data() + 16, message.stream_id);
  put_u64(out.data() + 24, message.connection_epoch);
  put_u64(out.data() + 32, message.value0);
  put_u64(out.data() + 40, message.value1);
  put_u64(out.data() + 48, message.value2);
  put_u32(out.data() + 56, static_cast<std::uint32_t>(message.memory_kind));
  put_u32(out.data() + 60, crc32c(out.data(), out.size()));
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
  const auto type = get_u16(in + 10);
  if (type < static_cast<std::uint16_t>(ControlType::hello) ||
      type > static_cast<std::uint16_t>(ControlType::eos_ack)) {
    error = "unknown control message type";
    return false;
  }
  const auto memory = get_u32(in + 56);
  if (memory != static_cast<std::uint32_t>(MemoryKind::host_pinned_mapped) &&
      memory != static_cast<std::uint32_t>(MemoryKind::cuda_device)) {
    error = "unknown memory kind";
    return false;
  }
  if (get_u32(in + 12) != 0) {
    error = "unsupported control flags";
    return false;
  }
  message.type = static_cast<ControlType>(type);
  message.flags = get_u32(in + 12);
  message.stream_id = get_u64(in + 16);
  message.connection_epoch = get_u64(in + 24);
  message.value0 = get_u64(in + 32);
  message.value1 = get_u64(in + 40);
  message.value2 = get_u64(in + 48);
  message.memory_kind = static_cast<MemoryKind>(memory);
  return true;
}

std::array<std::uint8_t, kDataHeaderWireBytes> encode_data_header(const DataHeader& header) {
  std::array<std::uint8_t, kDataHeaderWireBytes> out{};
  put_u32(out.data(), kDataMagic);
  put_u16(out.data() + 4, kProtocolMajor);
  put_u16(out.data() + 6, kProtocolMinor);
  put_u16(out.data() + 8, kDataHeaderWireBytes);
  put_u16(out.data() + 10, 1);  // IMAGE
  put_u32(out.data() + 12, header.flags);
  put_u64(out.data() + 16, header.stream_id);
  put_u64(out.data() + 24, header.connection_epoch);
  put_u64(out.data() + 32, header.sequence);
  put_u32(out.data() + 40, header.payload_length);
  put_u32(out.data() + 44, header.schema_id);
  put_u64(out.data() + 48, header.timestamp_ns);
  put_u32(out.data() + 56, header.payload_crc32c);
  put_u64(out.data() + 60, header.admission_ordinal);
  put_u32(out.data() + 68, crc32c(out.data(), out.size()));
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
  if (get_u16(in + 10) != 1) {
    error = "unsupported DATA message type";
    return false;
  }
  header.flags = get_u32(in + 12);
  header.stream_id = get_u64(in + 16);
  header.connection_epoch = get_u64(in + 24);
  header.sequence = get_u64(in + 32);
  header.payload_length = get_u32(in + 40);
  header.schema_id = get_u32(in + 44);
  header.timestamp_ns = get_u64(in + 48);
  header.payload_crc32c = get_u32(in + 56);
  header.admission_ordinal = get_u64(in + 60);
  if (header.flags != 0 || header.stream_id != kLogicalStreamId ||
      header.payload_length != kImageBytes || header.schema_id != kImageSchemaId) {
    error = "DATA header does not match the fixed image schema";
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
