/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "src/daqiri_pool.h"
#include "src/engine.h"
#include <daqiri/daqiri.h>

namespace daqiri {

/**
 * @brief Raw-Ethernet engine built on DOCA GPUNetIO (stream_type: raw, engine: gpunetio)
 *
 * CUDA kernels drive the NIC queues. On each GPU, one resident RX kernel serves all the RX queues,
 * one block per queue: a block receives into a ring in its queue's memory region and publishes
 * bursts, and the buffers of a burst go back to the NIC only when the application frees it. Each
 * TX queue sends from cyclic slots in its memory region, either from a block of the GPU's resident
 * TX kernel or with one kernel launch per burst (TX queue option gpunetio.tx_kernel). One CPU
 * thread per queue moves bursts between the kernels and the application.
 */
class GpunetioEngine : public Engine {
 public:
  GpunetioEngine();
  ~GpunetioEngine() override;

  bool set_config_and_initialize(const NetworkConfig& cfg) override;
  void initialize() override;
  void run() override;
  void shutdown() override;
  void print_stats() override;
  // The queues own their memory regions: caller-owned regions are not supported yet
  bool supports_external_memory_regions() const override {
    return false;
  }
  Status get_mac_addr(int port, char* mac) override;

  // Packet accessors
  void* get_packet_ptr(BurstParams* burst, int idx) override;
  uint32_t get_packet_length(BurstParams* burst, int idx) override;
  void* get_segment_packet_ptr(BurstParams* burst, int seg, int idx) override;
  uint32_t get_segment_packet_length(BurstParams* burst, int seg, int idx) override;
  FlowId get_packet_flow_id(BurstParams* burst, int idx) override;
  Status get_packet_rx_timestamp(BurstParams* burst, int idx, uint64_t* timestamp_ns) override;
  void* get_packet_extra_info(BurstParams* burst, int idx) override;

  // TX
  BurstParams* create_tx_burst_params() override;
  bool is_tx_burst_available(BurstParams* burst) override;
  Status get_tx_packet_burst(BurstParams* burst) override;
  Status set_eth_header(BurstParams* burst, int idx, char* dst_addr) override;
  Status set_ipv4_header(BurstParams* burst, int idx, int ip_len, uint8_t proto,
                         unsigned int src_host, unsigned int dst_host) override;
  Status set_udp_header(BurstParams* burst, int idx, int udp_len, uint16_t src_port,
                        uint16_t dst_port) override;
  Status set_udp_payload(BurstParams* burst, int idx, void* data, int len) override;
  Status set_packet_lengths(BurstParams* burst, int idx,
                            const std::initializer_list<int>& lens) override;
  Status set_packet_tx_time(BurstParams* burst, int idx, uint64_t time) override;
  Status send_tx_burst(BurstParams* burst) override;

  // RX
  Status get_rx_burst(BurstParams** burst, int port, int q) override;

  // Free family
  void free_all_segment_packets(BurstParams* burst, int seg) override;
  void free_all_packets(BurstParams* burst) override;
  void free_packet_segment(BurstParams* burst, int seg, int pkt) override;
  void free_packet(BurstParams* burst, int pkt) override;
  void free_rx_burst(BurstParams* burst) override;
  void free_tx_burst(BurstParams* burst) override;
  void free_rx_metadata(BurstParams* burst) override;
  void free_tx_metadata(BurstParams* burst) override;
  uint64_t get_burst_tot_byte(BurstParams* burst) override;

 private:
  struct Device;
  struct Gpu;
  struct MemoryMap;
  struct RxQueue;
  struct TxQueue;

  bool open_devices();
  Gpu* get_gpu(int ordinal);
  bool size_memory_regions();
  bool map_memory_regions();
  bool start_flow();
  bool create_rx_queue(RxQueue& q);
  bool create_tx_queue(TxQueue& q);
  bool program_steering(Device& dev, const InterfaceConfig& intf);
  bool create_burst_pools();
  bool start_kernels();
  void stop_kernels();

  void rx_worker(RxQueue* q);
  void tx_worker(TxQueue* q);

  RxQueue* find_rx_queue(int port, int q) const;
  TxQueue* find_tx_queue(int port, int q) const;
  Status write_packet(BurstParams* burst, int idx, size_t offset, const void* data, size_t len);

  std::vector<std::unique_ptr<Device>> devices_;
  std::vector<Device*> port_devices_;  // Indexed by port id
  std::map<int, std::unique_ptr<Gpu>> gpus_;
  std::map<std::string, std::unique_ptr<MemoryMap>> memory_maps_;
  std::vector<std::unique_ptr<RxQueue>> rx_queues_;
  std::vector<std::unique_ptr<TxQueue>> tx_queues_;
  bool flow_initialized_ = false;

  daqiri::ObjectPool* rx_meta_pool_ = nullptr;
  daqiri::ObjectPool* tx_meta_pool_ = nullptr;
  std::vector<BurstParams*> burst_objects_;  // Constructed in the pools, destroyed at shutdown
  uint32_t max_tx_batch_ = 0;

  std::atomic<bool> workers_running_{false};
};

}  // namespace daqiri
