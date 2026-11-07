// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "src/engine.h"
#include <memory>

namespace mgbe {
class Queue;
}

namespace daqiri {

// Kept internal so the engine's ownership paths can be tested with mock DMA
// descriptors without a NIC, ioctl, or kernel-module replacement.
class MgbePort {
 public:
  virtual ~MgbePort() = default;
  virtual mgbe::Queue& queue() = 0;
  virtual void start() = 0;
};

bool validate_mgbe_config(const NetworkConfig& config);

class MgbeEngine : public Engine {
 public:
  MgbeEngine();
  ~MgbeEngine() override;
  bool set_config_and_initialize(const NetworkConfig& config) override;
  void initialize() override;
  void run() override;
  bool validate_config() const override;
  void shutdown() override;
  void print_stats() override;
  BurstParams* create_tx_burst_params() override;
  Status get_rx_burst(BurstParams** burst, int port, int queue) override;
  Status get_tx_packet_burst(BurstParams* burst) override;
  bool is_tx_burst_available(BurstParams* burst) override;
  Status send_tx_burst(BurstParams* burst) override;
  void* get_packet_ptr(BurstParams* burst, int index) override;
  uint32_t get_packet_length(BurstParams* burst, int index) override;
  void* get_segment_packet_ptr(BurstParams* burst, int segment, int index) override;
  uint32_t get_segment_packet_length(BurstParams* burst, int segment, int index) override;
  FlowId get_packet_flow_id(BurstParams* burst, int index) override;
  Status get_packet_rx_timestamp(BurstParams* burst, int index, uint64_t* timestamp) override;
  void* get_packet_extra_info(BurstParams* burst, int index) override;
  Status set_packet_lengths(BurstParams* burst, int index,
                            const std::initializer_list<int>& lengths) override;
  Status set_eth_header(BurstParams* burst, int index, char* destination) override;
  Status set_ipv4_header(BurstParams* burst, int index, int length, uint8_t protocol,
                         unsigned int source, unsigned int destination) override;
  Status set_udp_header(BurstParams* burst, int index, int length, uint16_t source,
                        uint16_t destination) override;
  Status set_udp_payload(BurstParams* burst, int index, void* data, int length) override;
  Status set_packet_tx_time(BurstParams* burst, int index, uint64_t time) override;
  void free_all_segment_packets(BurstParams* burst, int segment) override;
  void free_all_packets(BurstParams* burst) override;
  void free_packet_segment(BurstParams* burst, int segment, int packet) override;
  void free_packet(BurstParams* burst, int packet) override;
  void free_rx_burst(BurstParams* burst) override;
  void free_tx_burst(BurstParams* burst) override;
  void free_rx_metadata(BurstParams* burst) override;
  void free_tx_metadata(BurstParams* burst) override;
  uint64_t get_burst_tot_byte(BurstParams* burst) override;
  Status get_mac_addr(int port, char* mac) override;
  Status wait_for_tx_idle(uint32_t timeout_ms) override;

 protected:
  virtual std::unique_ptr<MgbePort> open_port(const std::string& interface, uint32_t ring_size,
                                              uint32_t mtu);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace daqiri
