/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "examples/perftest/tcp_record.h"

#include <array>

int main() {
  constexpr int record_size = 32;
  constexpr uint64_t id = 0x123456789abcdef0;
  std::array<char, 2 * record_size> data{};
  const uint64_t first = htobe64(id);
  const uint64_t second = htobe64(id + 1);
  std::memcpy(data.data(), &first, sizeof(first));
  std::memcpy(data.data() + record_size, &second, sizeof(second));

  daqiri::perftest::TcpRecord record;
  const char* source = data.data();
  int available = 5;
  if (record.consume(source, available, record_size) || available) return 1;
  available = 2 * record_size - 5;
  if (record.consume(source, available, record_size) != id || available != record_size) return 2;
  if (record.consume(source, available, record_size) != id + 1 || available) return 3;
  return 0;
}
