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
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_errno.h>

#include <daqiri/daqiri.h>
#include "src/engine.h"

namespace daqiri {

// libfabric (OFI) engine for the AWS Elastic Fabric Adapter (EFA).
//
// EFA is a kernel-bypass engine for raw streams, selected with
// stream_type: "raw" + engine: "efa" (alongside the default `dpdk` and the
// `ibverbs` MPRQ engines). It is a standalone Engine — unlike RoCE it is NOT
// delegated through the socket engine and is NOT reachable via libibverbs RC +
// librdmacm: EFA is connectionless, using the libfabric `efa` provider with an
// FI_EP_RDM (SRD) endpoint, exchanging opaque endpoint addresses out-of-band
// rather than via RDMA-CM. It is built on the OFI fi_* API.
//
// The public contract (burst lifecycle, rdma_* connection helpers, per-conn
// TX/RX rings) matches RdmaEngine so the existing daqiri_bench_rdma driver and
// the RDMAOpCode SEND/RECEIVE burst API work unchanged against EFA. Per-interface
// server/client role and the bootstrap TCP port come from an `efa_config` block
// (parsed into RDMAConfig), since raw interfaces carry no socket_config.

// A single registered memory region. EFA requires FI_MR_HMEM for GPU buffers,
// so `desc` (from fi_mr_desc) must be passed on every fi_send/fi_recv that
// touches this region.
struct efa_mr_params {
  MemoryRegionConfig params_;
  struct fid_mr* mr = nullptr;
  void* desc = nullptr;
  void* ptr_ = nullptr;
};

// Per-connection state. EFA has no connection handshake on the data path; a
// "connection" is a peer address (fi_addr_t) inserted into the AV plus a
// bootstrap TCP socket used once to swap EFA addresses. conn_id handed back to
// the caller is reinterpret_cast<uintptr_t> of this object, mirroring how
// RdmaEngine hands back its rdma_cm_id pointer.
struct efa_conn {
  fi_addr_t peer = FI_ADDR_UNSPEC;
  int boot_fd = -1;
  int if_idx = -1;
  int queue_idx = -1;
  bool server = false;
  struct rte_ring* tx_ring = nullptr;
  struct rte_ring* rx_ring = nullptr;
  std::atomic<bool> ready_to_exit{false};
};

// Per-operation context. Its address is the `void* context` handed to
// fi_send/fi_recv and returned verbatim on the completion queue, giving O(1)
// correlation back to the owning burst (the OFI analog of an ibv wr_id).
struct efa_op_ctx {
  BurstParams* burst = nullptr;
  int pkt_idx = 0;
  bool is_tx = false;
  efa_conn* conn = nullptr;
};

class EfaEngine : public Engine {
 public:
  static constexpr uint16_t DEFAULT_PORT = 18515;

  EfaEngine() = default;
  ~EfaEngine() override;

  bool set_config_and_initialize(const NetworkConfig& cfg) override;
  void initialize() override;
  void run() override;

  void* get_packet_ptr(BurstParams* burst, int idx) override;
  uint32_t get_packet_length(BurstParams* burst, int idx) override;
  uint16_t get_packet_flow_id(BurstParams* burst, int idx) override {
    return 0;
  }
  Status get_packet_rx_timestamp(BurstParams* burst, int idx, uint64_t* timestamp_ns) override {
    (void)burst;
    (void)idx;
    (void)timestamp_ns;
    return Status::NOT_SUPPORTED;
  }
  void* get_packet_extra_info(BurstParams* burst, int idx) override {
    return nullptr;
  }
  void* get_segment_packet_ptr(BurstParams* burst, int seg, int idx) override;
  uint32_t get_segment_packet_length(BurstParams* burst, int seg, int idx) override;
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
  void free_all_segment_packets(BurstParams* burst, int seg) override {}
  void free_packet_segment(BurstParams* burst, int seg, int pkt) override {}
  void free_packet(BurstParams* burst, int pkt) override {}
  void free_all_packets(BurstParams* burst) override {}
  void free_rx_burst(BurstParams* burst) override;
  void free_tx_burst(BurstParams* burst) override;

  Status get_rx_burst(BurstParams** burst, int port, int q) override {
    return Status::NOT_SUPPORTED;
  }
  Status get_rx_burst(BurstParams** burst, uintptr_t conn_id, bool server) override;
  Status set_packet_tx_time(BurstParams* burst, int idx, uint64_t timestamp) override {
    return Status::NOT_SUPPORTED;
  }
  void free_rx_metadata(BurstParams* burst) override;
  void free_tx_metadata(BurstParams* burst) override;
  Status get_tx_metadata_buffer(BurstParams** burst) override;
  Status send_tx_burst(BurstParams* burst) override;
  uint64_t get_burst_tot_byte(BurstParams* burst) override;
  BurstParams* create_tx_burst_params() override;
  Status get_mac_addr(int port, char* mac) override {
    return Status::SUCCESS;
  }
  void shutdown() override;
  void print_stats() override;
  bool validate_config() const override {
    return true;
  }

  // EFA reuses the RDMA two-sided SEND/RECV burst API (RDMAOpCode SEND/RECEIVE)
  // so callers and the daqiri_bench_rdma driver are transport-agnostic.
  Status rdma_connect_to_server(const std::string& dst_addr, uint16_t dst_port,
                                uintptr_t* conn_id) override;
  Status rdma_connect_to_server(const std::string& dst_addr, uint16_t dst_port,
                                const std::string& src_addr, uintptr_t* conn_id) override;
  Status rdma_get_port_queue(uintptr_t conn_id, uint16_t* port, uint16_t* queue) override;
  Status rdma_get_server_conn_id(const std::string& server_addr, uint16_t server_port,
                                 uintptr_t* conn_id) override;
  Status rdma_set_header(BurstParams* burst, RDMAOpCode op_code, uintptr_t conn_id, bool is_server,
                         int num_pkts, uint64_t wr_id, const std::string& local_mr_name) override;
  RDMAOpCode rdma_get_opcode(BurstParams* burst) override;

 private:
  static constexpr int MAX_EFA_CONNECTIONS = 128;
  static constexpr int MAX_EFA_BATCH = 1024;
  static constexpr int CQ_DEPTH = 1024;
  static constexpr int CQ_POLL_BATCH = 16;
  // Largest opaque EFA address libfabric will hand back from fi_getname; queried
  // dynamically but bounded here for the bootstrap exchange buffer.
  static constexpr size_t MAX_EFA_ADDR_LEN = 64;

  // ---- libfabric setup / teardown ----
  bool ofi_init();
  void ofi_teardown();
  int register_cfg_mrs();
  int register_mr(const MemoryRegionConfig& mr, void* ptr);
  void* mr_desc_for_name(const std::string& name);

  // ---- DPDK ring/pool plumbing (shared shape with RdmaEngine) ----
  int setup_pools_and_rings();

  // ---- bootstrap address exchange ----
  // Swap the local EFA address with the peer over an already-connected TCP
  // socket and fi_av_insert the peer, returning its fi_addr_t.
  Status exchange_addr(int boot_fd, fi_addr_t* peer);
  efa_conn* make_connection(int boot_fd, int if_idx, bool server, fi_addr_t peer);

  // ---- progress ----
  // Drain one CQ, route fully-completed bursts to their connection's RX ring.
  void poll_cq(struct fid_cq* cq, efa_conn* conn, std::unordered_map<BurstParams*, int>& pending);
  static int set_affinity(int cpu_core);

  bool initialized_ = false;

  // OFI objects (one fabric/domain/endpoint shared across connections; SRD is
  // connectionless so a single bidirectional endpoint serves all peers).
  struct fi_info* fi_ = nullptr;
  struct fid_fabric* fabric_ = nullptr;
  struct fid_domain* domain_ = nullptr;
  struct fid_ep* ep_ = nullptr;
  struct fid_cq* txcq_ = nullptr;
  struct fid_cq* rxcq_ = nullptr;
  struct fid_av* av_ = nullptr;

  std::unordered_map<std::string, efa_mr_params> mrs_;

  // DPDK memory plumbing, identical in shape to RdmaEngine.
  std::unordered_map<std::string, struct rte_ring*> mem_pools_;
  struct rte_mempool* rx_meta_ = nullptr;
  struct rte_mempool* tx_meta_ = nullptr;
  struct rte_mempool* pkt_len_pool_ = nullptr;
  struct rte_mempool* tx_burst_pool_ = nullptr;
  std::queue<struct rte_ring*> tx_rings_;
  std::queue<struct rte_ring*> rx_rings_;

  // Connection registry. conn objects are owned here; conn_id is their address.
  std::mutex conns_mutex_;
  std::vector<std::unique_ptr<efa_conn>> conns_;
  std::unordered_map<uintptr_t, struct rte_ring*> tx_rings_map_;
  std::unordered_map<uintptr_t, struct rte_ring*> rx_rings_map_;
  std::unordered_map<std::string, std::vector<efa_conn*>> server_conns_;
  std::unordered_map<efa_conn*, std::thread> worker_threads_;

  // Bootstrap TCP listeners, one per server interface.
  std::vector<int> listen_fds_;
  std::thread main_thread_;
  std::atomic<bool> force_quit_{false};

  std::atomic<uint64_t> tx_pkts_{0};
  std::atomic<uint64_t> rx_pkts_{0};

  bool eal_initialized_ = false;
};

}  // namespace daqiri
