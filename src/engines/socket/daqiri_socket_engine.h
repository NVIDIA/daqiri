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
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <netinet/in.h>

#include <daqiri/daqiri.h>
#include "src/engine.h"
#include "src/metrics.h"

namespace daqiri {

class RdmaEngine;

class SocketEngine : public Engine {
 public:
  SocketEngine() = default;
  ~SocketEngine() override;

  bool set_config_and_initialize(const NetworkConfig& cfg) override;
  void initialize() override;
  void run() override;

  void* get_packet_ptr(BurstParams* burst, int idx) override;
  uint32_t get_packet_length(BurstParams* burst, int idx) override;
  void* get_segment_packet_ptr(BurstParams* burst, int seg, int idx) override;
  uint32_t get_segment_packet_length(BurstParams* burst, int seg, int idx) override;
  uint16_t get_packet_flow_id(BurstParams* burst, int idx) override;
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
  Status get_rx_burst(BurstParams** burst, uintptr_t conn_id, bool server) override;

  void free_rx_metadata(BurstParams* burst) override;
  void free_tx_metadata(BurstParams* burst) override;
  Status get_tx_metadata_buffer(BurstParams** burst) override;
  Status send_tx_burst(BurstParams* burst) override;
  Status get_mac_addr(int port, char* mac) override;

  bool validate_config() const override;

  // Generic socket functions.
  Status socket_connect_to_server(const std::string& dst_addr, uint16_t dst_port,
                                  uintptr_t* conn_id) override;
  Status socket_connect_to_server(const std::string& dst_addr, uint16_t dst_port,
                                  const std::string& src_addr, uintptr_t* conn_id) override;
  Status socket_get_port_queue(uintptr_t conn_id, uint16_t* port, uint16_t* queue) override;
  Status socket_get_server_conn_id(const std::string& server_addr, uint16_t server_port,
                                   uintptr_t* conn_id) override;

  // RoCE-only wrappers.
  Status rdma_connect_to_server(const std::string& dst_addr, uint16_t dst_port,
                                uintptr_t* conn_id) override;
  Status rdma_connect_to_server(const std::string& dst_addr, uint16_t dst_port,
                                const std::string& src_addr, uintptr_t* conn_id) override;
  Status rdma_get_port_queue(uintptr_t conn_id, uint16_t* port, uint16_t* queue) override;
  Status rdma_get_server_conn_id(const std::string& server_addr, uint16_t server_port,
                                 uintptr_t* conn_id) override;
  Status rdma_set_header(BurstParams* burst, RDMAOpCode op_code, uintptr_t conn_id,
                         bool is_server, int num_pkts, uint64_t wr_id,
                         const std::string& local_mr_name) override;
  RDMAOpCode rdma_get_opcode(BurstParams* burst) override;

 private:
  struct RxQueueState {
    uint16_t port = 0;
    uint16_t queue = 0;
    std::mutex mutex;
    std::queue<BurstParams*> bursts;
  };

  struct ConnectionState {
    uintptr_t conn_id = 0;
    int fd = -1;
    uint16_t port = 0;
    uint16_t queue = 0;
    int if_index = -1;
    bool server_side = false;
    bool is_udp = false;
    std::atomic<bool> running{false};
    std::thread rx_thread;
    std::shared_ptr<RxQueueState> rx_queue;
    std::shared_ptr<metrics::CounterSet> rx_metrics;
  };

  struct EndpointState {
    int if_index = -1;
    uint16_t port = 0;
    std::string address;
    uint16_t tx_queue = 0;
    uint16_t rx_queue = 0;
    uint32_t tx_batch_size = 1;
    size_t max_packet_size = 65536;
    SocketConfig socket_cfg;
    int listen_fd = -1;
    int udp_fd = -1;
    std::atomic<bool> accept_running{false};
    std::thread accept_thread;
    std::thread io_thread;
    std::shared_ptr<RxQueueState> rx_queue_state;
    std::shared_ptr<metrics::CounterSet> rx_metrics;
    std::shared_ptr<metrics::CounterSet> tx_metrics;
    sockaddr_in udp_peer_addr{};
    bool udp_peer_valid = false;
    uintptr_t primary_conn_id = 0;
  };

  bool is_roce_protocol() const;
  Status roce_not_initialized(const char* op_name) const;

  void setup_tcp_endpoint(EndpointState& ep);
  void setup_udp_endpoint(EndpointState& ep);
  void tcp_accept_loop(int if_index);
  void tcp_rx_loop(std::shared_ptr<ConnectionState> conn);
  void udp_rx_loop(int if_index);

  uintptr_t next_conn_id();
  std::string endpoint_key(const std::string& ip, uint16_t port) const;
  std::shared_ptr<RxQueueState> get_or_create_rx_queue(uint16_t port, uint16_t queue);

  std::shared_ptr<ConnectionState> create_tcp_client_connection(
      EndpointState& ep, const std::string& dst_addr, uint16_t dst_port,
      const std::string& src_addr, uint16_t src_port, bool set_as_primary);

  std::shared_ptr<ConnectionState> register_connection(
      int fd, uint16_t port, uint16_t queue, int if_index, bool server_side, bool is_udp,
      const std::shared_ptr<RxQueueState>& rx_queue, bool start_rx_thread);

  EndpointState* endpoint_for_port(uint16_t port);
  const EndpointState* endpoint_for_port(uint16_t port) const;

  int select_max_packet_size(const InterfaceConfig& if_cfg) const;
  uint16_t select_queue_id(const std::vector<RxQueueConfig>& queues) const;
  uint16_t select_queue_id(const std::vector<TxQueueConfig>& queues) const;
  uint32_t select_batch_size(const std::vector<TxQueueConfig>& queues) const;

  Status pop_rx_burst(const std::shared_ptr<RxQueueState>& qstate, BurstParams** burst);
  void push_rx_burst(const std::shared_ptr<RxQueueState>& qstate, BurstParams* burst);

  void free_packet_arrays(BurstParams* burst);
  void close_fd(int& fd);
  void close_all_connections();
  void clear_rx_queues();

  bool send_tcp_burst(int fd, BurstParams* burst, size_t* sent_pkts, uint64_t* sent_bytes);
  bool send_udp_burst(EndpointState& ep, BurstParams* burst, size_t* sent_pkts,
                      uint64_t* sent_bytes);

  std::atomic<bool> running_{false};
  std::atomic<uintptr_t> next_conn_id_{1};

  std::vector<std::unique_ptr<EndpointState>> endpoints_;

  mutable std::mutex state_mutex_;
  std::unordered_map<uintptr_t, std::shared_ptr<ConnectionState>> connections_;
  std::unordered_map<uint16_t, uintptr_t> default_connection_by_port_;
  std::unordered_map<std::string, std::vector<uintptr_t>> server_connections_;

  std::atomic<uint64_t> tx_pkts_{0};
  std::atomic<uint64_t> tx_bytes_{0};
  std::atomic<uint64_t> rx_pkts_{0};
  std::atomic<uint64_t> rx_bytes_{0};

#if DAQIRI_ENGINE_RDMA
  std::unique_ptr<RdmaEngine> roce_engine_;
#endif
};

}  // namespace daqiri
