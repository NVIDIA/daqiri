// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
// Explicit hardware correctness check. Never registered with CTest.
#include "src/engines/mgbe/daqiri_mgbe_engine.h"
#include "mgbe_pmd.hpp"
#include <daqiri/daqiri.h>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool pass, const char* operation) {
  if (!pass) throw std::runtime_error(operation);
}
class LoopbackPort final : public daqiri::MgbePort {
 public:
  LoopbackPort(const std::string& interface, uint32_t ring, uint32_t mtu)
      : device_("/dev/mgbe-pmd-" + interface, ring, mtu) {}
  mgbe::Queue& queue() override {
    return device_.queue();
  }
  void start() override {
    device_.enable_mac_loopback();
    device_.start();
  }

 private:
  mgbe::Device device_;
};
class ProbeEngine final : public daqiri::MgbeEngine {
 protected:
  std::unique_ptr<daqiri::MgbePort> open_port(const std::string& interface, uint32_t ring,
                                              uint32_t mtu) override {
    return std::make_unique<LoopbackPort>(interface, ring, mtu);
  }
};
void check(const daqiri::NetworkConfig& base, size_t frame_size) {
  auto config = base;
  for (auto& [name, region] : config.mrs_) {
    region.num_bufs_ = 64;
    region.buf_size_ = frame_size <= 1514 ? 1514 : frame_size;
  }
  ProbeEngine engine;
  require(engine.set_config_and_initialize(config), "MGBE DMA initialization");
  std::array<char, 6> mac{};
  require(engine.get_mac_addr(0, mac.data()) == daqiri::Status::SUCCESS, "read MAC");
  std::array<uint8_t, 9014> expected{};
  for (uint32_t sequence = 0; sequence < 256; ++sequence) {
    auto* tx = engine.create_tx_burst_params();
    require(tx != nullptr, "allocate TX metadata");
    daqiri::set_header(tx, 0, 0, 1, 1);
    require(engine.get_tx_packet_burst(tx) == daqiri::Status::SUCCESS, "reserve TX packet");
    require(engine.set_packet_lengths(tx, 0, {int(frame_size)}) == daqiri::Status::SUCCESS,
            "set TX length");
    auto* data = static_cast<uint8_t*>(engine.get_packet_ptr(tx, 0));
    for (size_t byte = 0; byte < frame_size; ++byte)
      expected[byte] = uint8_t((byte * 131U) ^ sequence ^ (sequence >> 8));
    std::memcpy(expected.data(), mac.data(), 6);
    std::memcpy(expected.data() + 6, mac.data(), 6);
    expected[12] = 0x88;
    expected[13] = 0xb5;  // Experimental EtherType, no kernel IP processing.
    std::memcpy(data, expected.data(), frame_size);
    require(engine.send_tx_burst(tx) == daqiri::Status::SUCCESS, "post TX packet");
    daqiri::BurstParams* rx = nullptr;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    daqiri::Status status;
    do {
      status = engine.get_rx_burst(&rx, 0, 0);
      if (status != daqiri::Status::NOT_READY) break;
    } while (std::chrono::steady_clock::now() < deadline);
    require(status == daqiri::Status::SUCCESS && rx->hdr.hdr.num_pkts == 1, "DMA RX completion");
    require(engine.get_packet_length(rx, 0) == frame_size, "DMA RX length");
    require(std::memcmp(engine.get_packet_ptr(rx, 0), expected.data(), frame_size) == 0,
            "DMA payload mismatch");
    engine.free_rx_burst(rx);
  }
  require(engine.wait_for_tx_idle(1000) == daqiri::Status::SUCCESS, "drain TX");
  engine.shutdown();
}
}  // namespace
int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "Usage: daqiri_mgbe_dma_test config.yaml (exclusive hardware MAC loopback)\n";
    return 2;
  }
  try {
    daqiri::NetworkConfig config{};
    require(daqiri::parse_network_config_from_yaml_file(argv[1], config) == daqiri::Status::SUCCESS,
            "parse configuration");
    for (size_t frame : {size_t(60), size_t(1514), size_t(8192)}) check(config, frame);
    std::cout << "MGBE DMA payload, ring reuse and session recovery checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
