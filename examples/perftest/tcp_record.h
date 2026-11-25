/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <endian.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>

namespace daqiri::perftest {

// TCP read boundaries can split a record or include several records. Only the
// first eight bytes of each fixed-size record carry identity.
class TcpRecord {
 public:
  std::optional<uint64_t> consume(const char*& data, int& available, int record_size) {
    const int bytes = std::min(available, record_size - received_);
    const int prefix = std::min(bytes, std::max(0, static_cast<int>(sizeof(id_)) - received_));
    if (prefix) std::memcpy(reinterpret_cast<char*>(&id_) + received_, data, prefix);
    received_ += bytes;
    data += bytes;
    available -= bytes;
    if (received_ != record_size) return std::nullopt;
    received_ = 0;
    return be64toh(id_);
  }

 private:
  uint64_t id_ = 0;
  int received_ = 0;
};

}  // namespace daqiri::perftest
