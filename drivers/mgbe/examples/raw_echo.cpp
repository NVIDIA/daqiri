// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#include <daqiri/daqiri.h>
#include <algorithm>
#include <array>
#include <csignal>
#include <cstring>
#include <iostream>

namespace {
volatile std::sig_atomic_t stopped = 0;
void stop(int) {
  stopped = 1;
}
}  // namespace
int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "Usage: daqiri_mgbe_raw_echo config.yaml\n";
    return 2;
  }
  std::signal(SIGINT, stop);
  std::signal(SIGTERM, stop);
  if (daqiri::daqiri_init_from_yaml_file(argv[1]) != daqiri::Status::SUCCESS) return 1;
  if (daqiri::get_engine_type() != daqiri::EngineType::MGBE) {
    daqiri::shutdown();
    return 1;
  }
  std::array<char, 6> mac{};
  if (daqiri::get_mac_addr(0, mac.data()) != daqiri::Status::SUCCESS) {
    daqiri::shutdown();
    return 1;
  }
  bool failed = false;
  uint64_t echoed = 0;
  std::cout << "MGBE raw echo ready" << std::endl;
  while (!stopped && !failed) {
    daqiri::BurstParams* rx = nullptr;
    const auto status = daqiri::get_rx_burst(&rx, 0, 0);
    if (status == daqiri::Status::NOT_READY) continue;
    if (status != daqiri::Status::SUCCESS) {
      failed = true;
      break;
    }
    for (int i = 0; i < daqiri::get_num_packets(rx) && !failed; ++i) {
      const auto length = daqiri::get_packet_length(rx, i);
      const auto* input = static_cast<const uint8_t*>(daqiri::get_packet_ptr(rx, i));
      if (length < sizeof(daqiri::UDPIPV4Pkt) || std::memcmp(input, mac.data(), 6) ||
          input[12] != 8 || input[13] != 0 || input[14] != 0x45 || input[23] != 17 ||
          (input[20] & 0x3f) || input[21])
        continue;
      auto* tx = daqiri::create_tx_burst_params();
      if (!tx) {
        failed = true;
        break;
      }
      daqiri::set_header(tx, 0, 0, 1, 1);
      if (daqiri::get_tx_packet_burst(tx) != daqiri::Status::SUCCESS ||
          daqiri::set_packet_lengths(tx, 0, {int(length)}) != daqiri::Status::SUCCESS) {
        daqiri::free_all_packets_and_burst_tx(tx);
        failed = true;
        break;
      }
      auto* output = static_cast<uint8_t*>(daqiri::get_packet_ptr(tx, 0));
      std::memcpy(output, input, length);
      for (int j = 0; j < 6; ++j) std::swap(output[j], output[j + 6]);
      for (int j = 0; j < 4; ++j) std::swap(output[26 + j], output[30 + j]);
      for (int j = 0; j < 2; ++j) std::swap(output[34 + j], output[36 + j]);
      // Swapping endpoints preserves the IPv4 and UDP checksum sums.
      if (daqiri::send_tx_burst(tx) != daqiri::Status::SUCCESS) {
        daqiri::free_all_packets_and_burst_tx(tx);
        failed = true;
        break;
      }
      ++echoed;
    }
    daqiri::free_all_packets_and_burst_rx(rx);
  }
  if (daqiri::wait_for_tx_idle(1000) != daqiri::Status::SUCCESS) failed = true;
  daqiri::shutdown();
  std::cout << "Packets echoed: " << echoed << '\n';
  return failed ? 1 : 0;
}
