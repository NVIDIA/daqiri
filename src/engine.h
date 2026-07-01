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

#include "src/daqiri_ring.h"
#include <daqiri/types.h>
#include <optional>

// Forward declarations of the DPDK types used only by the DPDK engine. Keeping
// them as forward declarations (the members are a pointer-returning helper and a
// shared_ptr to an incomplete type) means engine.h pulls in no libdpdk headers,
// so daqiri_common and the rdma/ibverbs/socket engines build without DPDK. The
// definitions of the methods that use them live in engine_dpdk.cpp, compiled
// into daqiri_common only when the dpdk engine is selected.
struct rte_mempool;
struct rte_pktmbuf_extmem;

namespace daqiri {

struct AllocRegion {
  std::string mr_name_;
  void* ptr_;
};

/**
 * @brief (Almost) ABC representing an interface into a daqiri engine implementation
 *
 */
class Engine {
 public:
  static constexpr size_t MAX_RX_Q_PER_CORE = 16;

  virtual void initialize() = 0;
  virtual bool is_initialized() const { return initialized_; }
  virtual bool set_config_and_initialize(const NetworkConfig& cfg) = 0;
  virtual void run() = 0;

  // Common free functions to override
  virtual void* get_packet_ptr(BurstParams* burst, int idx) = 0;
  virtual uint32_t get_packet_length(BurstParams* burst, int idx) = 0;
  virtual void* get_segment_packet_ptr(BurstParams* burst, int seg, int idx) = 0;
  virtual uint32_t get_segment_packet_length(BurstParams* burst, int seg, int idx) = 0;
  virtual FlowId get_packet_flow_id(BurstParams* burst, int idx) = 0;
  virtual Status get_packet_rx_timestamp(BurstParams* burst, int idx, uint64_t* timestamp_ns) = 0;
  virtual void* get_packet_extra_info(BurstParams* burst, int idx) = 0;
  virtual Status get_tx_packet_burst(BurstParams* burst) = 0;
  virtual Status set_eth_header(BurstParams* burst, int idx, char* dst_addr) = 0;
  virtual Status set_ipv4_header(BurstParams* burst, int idx, int ip_len, uint8_t proto,
                                 unsigned int src_host, unsigned int dst_host) = 0;
  virtual Status set_udp_header(BurstParams* burst, int idx, int udp_len, uint16_t src_port,
                                uint16_t dst_port) = 0;
  virtual Status set_udp_payload(BurstParams* burst, int idx, void* data, int len) = 0;
  virtual bool is_tx_burst_available(BurstParams* burst) = 0;

  virtual Status set_packet_lengths(BurstParams* burst, int idx,
                                    const std::initializer_list<int>& lens) = 0;
  virtual Status set_all_packet_lengths(BurstParams* burst,
                                        const std::initializer_list<int>& lens);
  virtual void free_all_segment_packets(BurstParams* burst, int seg) = 0;
  virtual void free_all_packets(BurstParams* burst) = 0;
  virtual void free_packet_segment(BurstParams* burst, int seg, int pkt) = 0;
  virtual void free_packet(BurstParams* burst, int pkt) = 0;
  virtual void free_rx_burst(BurstParams* burst) = 0;
  virtual void free_tx_burst(BurstParams* burst) = 0;
  virtual Status set_packet_tx_time(BurstParams* burst, int idx, uint64_t time) = 0;
  virtual void shutdown() = 0;
  virtual void print_stats() = 0;
  virtual uint64_t get_burst_tot_byte(BurstParams* burst) = 0;
  virtual BurstParams* create_tx_burst_params() = 0;
  virtual Status get_rx_burst(BurstParams** burst, uintptr_t conn_id, bool server);
  virtual Status get_rx_burst(BurstParams** burst, int port, int q) = 0;
  virtual Status get_rx_burst(BurstParams** burst, int port_id);
  virtual Status get_rx_burst(BurstParams** burst);
  virtual Status set_reorder_cuda_stream(const std::string& interface_name,
                                         const std::string& reorder_name,
                                         cudaStream_t stream);
  virtual Status get_reorder_burst_info(BurstParams* burst, ReorderBurstInfo* info);
  virtual void free_rx_metadata(BurstParams* burst) = 0;
  virtual void free_tx_metadata(BurstParams* burst) = 0;
  virtual Status get_tx_metadata_buffer(BurstParams** burst) = 0;
  virtual Status send_tx_burst(BurstParams* burst) = 0;
  virtual Status get_mac_addr(int port, char* mac) = 0;
  virtual Status drop_all_traffic(int port);
  virtual Status allow_all_traffic(int port);
  virtual Status add_rx_flow_async(int port, const FlowRuleConfig& flow, FlowOpId* op_id);
  virtual Status add_rx_flows_async(int port,
                                    const std::vector<FlowRuleConfig>& flows,
                                    FlowOpId* op_id);
  virtual Status delete_flow_async(FlowId flow_id, FlowOpId* op_id);
  virtual Status poll_flow_op(FlowOpResult* result);
  virtual int get_port_id(const std::string& key) final;  // NOLINT(readability/inheritance)
  virtual bool validate_config() const;
  virtual uint16_t get_num_rx_queues(int port_id) const;
  virtual void flush_port_queue(int port, int queue);

  virtual Status socket_connect_to_server(const std::string& dst_addr, uint16_t dst_port,
                                          uintptr_t* conn_id);
  virtual Status socket_connect_to_server(const std::string& dst_addr, uint16_t dst_port,
                                          const std::string& src_addr, uintptr_t* conn_id);
  virtual Status socket_get_port_queue(uintptr_t conn_id, uint16_t* port, uint16_t* queue);
  virtual Status socket_get_server_conn_id(const std::string& server_addr, uint16_t server_port,
                                           uintptr_t* conn_id);
  virtual Status socket_setsockopt(uintptr_t conn_id, int level, int optname, const void* optval,
                                   size_t optlen);

  virtual Status rdma_connect_to_server(const std::string& dst_addr, uint16_t dst_port,
                                        uintptr_t* conn_id);
  virtual Status rdma_connect_to_server(const std::string& dst_addr, uint16_t dst_port,
                                        const std::string& src_addr, uintptr_t* conn_id);
  virtual Status rdma_get_port_queue(uintptr_t conn_id, uint16_t* port, uint16_t* queue);
  virtual Status rdma_get_server_conn_id(const std::string& server_addr, uint16_t server_port,
                                         uintptr_t* conn_id);
  virtual Status rdma_set_header(BurstParams* burst, RDMAOpCode op_code, uintptr_t conn_id,
                                 bool is_server, int num_pkts, uint64_t wr_id,
                                 const std::string& local_mr_name);
  virtual RDMAOpCode rdma_get_opcode(BurstParams* burst);
  int numa_from_mem(const MemoryRegionConfig& mr) const;
  // mbuf/extmem-based helpers used only by the DPDK engine. Their definitions
  // live in engine_dpdk.cpp, compiled into daqiri_common only when the dpdk
  // engine is selected; in non-DPDK builds they are declared-but-undefined and
  // never referenced (they are only called from DpdkEngine), so they link fine.
  Status register_memory_regions();
  Status map_memory_regions();
  struct rte_mempool* create_pktmbuf_pool(const std::string& name, const MemoryRegionConfig& mr);
  struct rte_mempool* create_generic_pool(const std::string& name, const MemoryRegionConfig& mr);

  virtual ~Engine() = default;

 protected:
  static constexpr int MAX_IFS = 4;
  static constexpr int MAX_GPUS = 8;
  static constexpr uint32_t GPU_PAGE_SHIFT = 16;
  static constexpr uint32_t GPU_PAGE_SIZE = (1UL << GPU_PAGE_SHIFT);
  static constexpr uint32_t JUMBO_FRAME_MAX_SIZE = 0x2600;
  static constexpr uint32_t NON_JUMBO_FRAME_MAX_SIZE = 1518;

  // DPDK hugepage-accounting constants used only by the DPDK engine's preflight
  // (engine_dpdk.cpp). Harmless to keep in the shared base; they pull in no
  // libdpdk types.
  static constexpr size_t DPDK_PER_POOL_HUGEPAGE_OVERHEAD = 32UL * 1024 * 1024;
  static constexpr size_t DPDK_EAL_FIXED_OVERHEAD = 64UL * 1024 * 1024;

  bool initialized_ = false;
  NetworkConfig cfg_;
  std::unordered_map<std::string, AllocRegion> ar_;
  // shared_ptr to an incomplete type -- only populated by the DPDK engine
  // (engine_dpdk.cpp). Layout is identical in every build.
  std::unordered_map<std::string, std::shared_ptr<struct rte_pktmbuf_extmem>> ext_pktmbufs_;
  std::unordered_map<uint32_t, std::vector<std::pair<uint16_t, uint16_t>>> rx_core_q_map;

  // EAL lifecycle state, used only by the DPDK engine. Mirrors the
  // --file-prefix= value passed to EAL so cleanup targets only this process's
  // hugepage files.
  bool eal_initialized_ = false;
  std::string eal_file_prefix_;

  // State for round-robin burst retrieval
  size_t next_port_index_ = 0;                            // For get_rx_burst next port check
  std::unordered_map<int, size_t> next_queue_index_map_;  // For get_rx_burst next queue check

  virtual Status allocate_memory_regions();
  virtual void adjust_memory_regions() {}
  // Allocate hugepage-backed memory for a kind: HUGE region. The base provides
  // a libdpdk-free implementation (MAP_HUGETLB with a regular-page fallback);
  // DpdkEngine overrides it with rte_malloc_socket so HUGE regions come from EAL
  // hugepages that are IOVA-contiguous for the NIC.
  virtual void* alloc_huge(size_t bytes, int numa);
  void init_rx_core_q_map();
  static std::string generate_random_string(int len);
  size_t get_alignment(MemoryKind kind);
  Status populate_pool(daqiri::Ring* ring, const std::string& mr_name);

  // The hugepage-preflight and EAL-cleanup helpers below are defined only in
  // engine_dpdk.cpp (DPDK builds). They are declared unconditionally but, like
  // the pool helpers above, are only referenced from the DPDK engine.
  // Breakdown of the hugepage estimate (all values in bytes). Used by both
  // the preflight log line and the failure diagnostic so users can see
  // exactly which knob drives the number they're seeing.
  struct HugepageEstimate {
    size_t huge_mr_bytes = 0;        // sum of kind: HUGE memory regions
    size_t pool_overhead_bytes = 0;  // ~32 MiB per mempool (extbuf or huge)
    size_t dummy_queue_bytes = 0;    // dummy MR(s) DpdkEngine injects for empty TX/RX
    size_t eal_fixed_bytes = 0;      // EAL services / memzones / ethdev tables
    size_t total_bytes = 0;
    size_t huge_mr_count = 0;        // for diagnostic output
    size_t extbuf_pool_count = 0;
    size_t dummy_queue_count = 0;
  };

  // The hugepage/EAL preflight helpers below (estimate_required_hugepages,
  // available_hugepage_bytes, check_hugepage_availability, cleanup_eal) are,
  // like the mbuf/pool helpers above, defined only in engine_dpdk.cpp and so
  // compiled into daqiri_common only in DPDK builds. They are called solely
  // from DpdkEngine; in non-DPDK builds they are declared-but-undefined and
  // never referenced, so the library still links.

  // Estimate hugepage bytes required by the current cfg_. Heuristic;
  // intentionally errs on the high side.
  HugepageEstimate estimate_required_hugepages() const;

  // Sum of free hugepages across all pagesizes mounted on this host, in
  // bytes. Reads /sys/kernel/mm/hugepages/hugepages-*kB. Returns 0 if no
  // hugepages are configured.
  static size_t available_hugepage_bytes();

  // Preflight check run before rte_eal_init(). Logs an actionable critical
  // error and returns false if there is not enough free hugepage memory to
  // back the configured memory regions plus DPDK overhead. Returning true
  // does NOT guarantee init will succeed (DPDK has additional, version- and
  // NIC-specific allocations) but it catches the common kernel-default
  // 1024 x 2 MiB starvation case before EAL leaves files in /dev/hugepages.
  bool check_hugepage_availability() const;

  // Best-effort cleanup of EAL state and leftover hugepage files. Safe to
  // call multiple times and on partial-init failures. Resets
  // eal_initialized_ to false on the way out.
  void cleanup_eal();
};

class EngineFactory {
 public:
  static void set_engine_type(EngineType type) {
    if (EngineType_ != EngineType::UNKNOWN && EngineType_ != type) {
      throw std::logic_error("Engine type is already set with another engine type.");
    }
    if (type == EngineType::DEFAULT) {
      EngineType_ = get_default_engine_type();
    } else {
      EngineType_ = type;
    }
  }

  static EngineType get_engine_type() { return EngineType_; }

  template <typename Config>
  static EngineType get_engine_type(const Config& config);

  static EngineType get_default_engine_type();

  static Engine& get_active_engine() {
    if (EngineType_ == EngineType::UNKNOWN) { throw std::logic_error("EngineType not set"); }
    if (!EngineInstance_) { EngineInstance_ = create_instance(EngineType_); }
    return *EngineInstance_;
  }

 private:
  EngineFactory() = default;
  ~EngineFactory() = default;
  EngineFactory(const EngineFactory&) = delete;
  EngineFactory& operator=(const EngineFactory&) = delete;

  static std::unique_ptr<Engine> EngineInstance_;
  static EngineType EngineType_;

  static std::unique_ptr<Engine> create_instance(EngineType type);
};

};  // namespace daqiri
