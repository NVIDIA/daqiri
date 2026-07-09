/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "src/engine.h"

namespace daqiri {

/*
 * Internal implementation selected directly by stream_type: pcie. PCIe is not
 * a public EngineType and DMA-BUF is a registration mechanism, not an engine.
 */
class PcieEngine final : public Engine {
 public:
  PcieEngine();
  ~PcieEngine() override;

  bool set_config_and_initialize(const NetworkConfig& cfg) override;
  void initialize() override;
  void run() override;

  void* get_packet_ptr(BurstParams* burst, int idx) override;
  uint32_t get_packet_length(BurstParams* burst, int idx) override;
  void* get_segment_packet_ptr(BurstParams* burst, int seg, int idx) override;
  uint32_t get_segment_packet_length(BurstParams* burst, int seg, int idx) override;
  FlowId get_packet_flow_id(BurstParams* burst, int idx) override;
  Status get_packet_rx_timestamp(BurstParams* burst, int idx, uint64_t* timestamp_ns) override;
  void* get_packet_extra_info(BurstParams* burst, int idx) override;

  Status get_tx_packet_burst(BurstParams* burst) override;
  Status set_eth_header(BurstParams* burst, int idx, char* dst_addr) override;
  Status set_ipv4_header(BurstParams* burst, int idx, int ip_len, uint8_t proto,
                         unsigned int src_host, unsigned int dst_host) override;
  Status set_udp_header(BurstParams* burst, int idx, int udp_len, uint16_t src_port,
                        uint16_t dst_port) override;
  Status set_udp_payload(BurstParams* burst, int idx, void* data, int len) override;
  bool is_tx_burst_available(BurstParams* burst) override;
  Status set_packet_lengths(BurstParams* burst, int idx,
                            const std::initializer_list<int>& lens) override;

  void free_all_segment_packets(BurstParams* burst, int seg) override;
  void free_all_packets(BurstParams* burst) override;
  void free_packet_segment(BurstParams* burst, int seg, int pkt) override;
  void free_packet(BurstParams* burst, int pkt) override;
  void free_rx_burst(BurstParams* burst) override;
  void free_tx_burst(BurstParams* burst) override;
  Status set_packet_tx_time(BurstParams* burst, int idx, uint64_t time) override;

  void shutdown() override;
  void print_stats() override;
  uint64_t get_burst_tot_byte(BurstParams* burst) override;
  BurstParams* create_tx_burst_params() override;
  Status get_rx_burst(BurstParams** burst, int port, int q) override;
  void free_rx_metadata(BurstParams* burst) override;
  void free_tx_metadata(BurstParams* burst) override;
  Status get_tx_metadata_buffer(BurstParams** burst) override;
  Status send_tx_burst(BurstParams* burst) override;
  Status get_mac_addr(int port, char* mac) override;

  bool validate_config() const override;

 private:
  struct InterfaceState;
  struct BurstStorage;

  InterfaceState* find_interface(uint16_t port);
  const InterfaceState* find_interface(uint16_t port) const;
  static BurstStorage* burst_storage(BurstParams* burst);
  static const BurstStorage* burst_storage(const BurstParams* burst);

  bool initialize_interface(InterfaceState& state, const InterfaceConfig& config);
  bool initialize_cuda_ordering(InterfaceState& state);
  bool register_region(InterfaceState& state, const std::string& mr_name, bool rx);
  bool post_initial_rx_slots(InterfaceState& state);
  bool post_rx_slot(InterfaceState& state, uint32_t slot_id);
  void retry_deferred_rx_slots(InterfaceState& state);
  bool flush_remote_writes(InterfaceState& state);
  void rx_worker_loop(InterfaceState* state);
  void tx_worker_loop(InterfaceState* state);
  bool provider_is_healthy(InterfaceState& state);
  void process_rx_completions(InterfaceState& state);
  void process_tx_completions(InterfaceState& state);
  bool publish_rx_burst(InterfaceState& state, size_t count);
  bool release_packet(BurstParams* burst, int pkt);
  void reclaim_unsent_tx(BurstParams* burst);
  void mark_unhealthy(InterfaceState& state, const std::string& reason);
  void delete_burst(BurstParams* burst);

  std::vector<std::unique_ptr<InterfaceState>> interfaces_;
  std::atomic<bool> running_{false};
  std::atomic<bool> accepting_tx_{false};
  std::atomic<bool> healthy_{true};

  std::atomic<uint64_t> rx_packets_{0};
  std::atomic<uint64_t> rx_bytes_{0};
  std::atomic<uint64_t> tx_packets_{0};
  std::atomic<uint64_t> tx_bytes_{0};
  std::atomic<uint64_t> rx_backpressure_{0};
  std::atomic<uint64_t> tx_backpressure_{0};
  std::atomic<uint64_t> malformed_completions_{0};
  std::atomic<uint64_t> cuda_flush_failures_{0};
};

}  // namespace daqiri
