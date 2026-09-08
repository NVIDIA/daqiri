// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#include "protocol.h"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace daqiri::ucx_gpu;

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

int main() {
  ControlMessage hello;
  hello.type = ControlType::hello;
  hello.value0 = 123456;
  hello.value1 = 64;
  hello.value2 = 0;
  hello.value3 = 0x123456789abcdef0ULL;
  auto hello_wire = encode_control(hello);
  ControlMessage decoded_hello;
  std::string error;
  require(decode_control(hello_wire.data(), hello_wire.size(), decoded_hello, error),
          "HELLO decode failed");
  require(decoded_hello.type == ControlType::hello, "HELLO type mismatch");
  require(decoded_hello.value0 == hello.value0, "HELLO value0 mismatch");
  require(decoded_hello.value1 == hello.value1, "HELLO value1 mismatch");
  require(decoded_hello.value2 == hello.value2, "HELLO value2 mismatch");
  require(decoded_hello.value3 == hello.value3, "HELLO value3 mismatch");

  DataHeader data;
  data.connection_epoch = 17;
  data.first_sequence = 992;
  data.image_count = 16;
  data.batch_ordinal = 42;
  auto data_wire = encode_data_header(data);
  DataHeader decoded_data;
  require(decode_data_header(data_wire.data(), data_wire.size(), decoded_data, error),
          "DATA decode failed");
  require(decoded_data.connection_epoch == data.connection_epoch, "DATA epoch mismatch");
  require(decoded_data.first_sequence == data.first_sequence, "DATA sequence mismatch");
  require(decoded_data.image_count == data.image_count, "DATA image count mismatch");
  require(decoded_data.batch_ordinal == data.batch_ordinal, "DATA batch ordinal mismatch");

  data_wire[16] ^= 0x1;
  require(!decode_data_header(data_wire.data(), data_wire.size(), decoded_data, error),
          "corrupt DATA header was accepted");
  require(error == "header CRC32C mismatch", "wrong DATA CRC diagnostic");

  std::cout << "protocol serialization tests passed\n";
  return 0;
}
