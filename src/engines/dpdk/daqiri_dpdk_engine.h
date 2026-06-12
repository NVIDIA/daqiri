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

#include <vector>
#include <string>
#include <tuple>
#include <rte_common.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_eal.h>
#include <rte_launch.h>
#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_metrics.h>
#include <rte_bitrate.h>
#include <rte_latencystats.h>
#include <rte_flow.h>
#include <rte_gpudev.h>
#include <atomic>
#include <thread>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include "src/engine.h"
#include <daqiri/daqiri.h>
#include "daqiri_dpdk_stats.h"

#ifndef DAQIRI_REORDER_GPU_PROFILE
#define DAQIRI_REORDER_GPU_PROFILE 0
#endif

namespace daqiri {

struct DPDKQueueConfig {
  std::vector<struct rte_mempool*> pools;
  struct rte_eth_rxconf rxconf_qsplit;
  std::vector<union rte_eth_rxseg> rx_useg;
};

struct DropTrafficConfig {
  struct rte_flow* jump;  // unused; port jump is owned by ensure_eth_jump_rule()
  struct rte_flow* drop;
};

struct RxTimestampConversion {
  bool valid = false;
  uint64_t ticks_per_second = 0;
};

class DpdkEngine : public Engine {
 public:
  static_assert(MAX_INTERFACES <= RTE_MAX_ETHPORTS, "Too many interfaces configured");

  DpdkEngine() = default;
  ~DpdkEngine();
  bool set_config_and_initialize(const NetworkConfig& cfg) override;
  void initialize() override;
  void run() override;
  static constexpr int JUMBOFRAME_SIZE = 9100;
  static constexpr int DEFAULT_NUM_TX_BURST = 256;
  static constexpr int DEFAULT_NUM_RX_BURST = 64;
  static constexpr int MAX_NUM_RX_QUEUES = 64;
  uint16_t default_num_rx_desc = 8192;
  uint16_t default_num_tx_desc = 8192;
  int num_ports = 0;
  static constexpr int MEMPOOL_CACHE_SIZE = 32;

  static constexpr uint32_t GPU_PAGE_OFFSET = (GPU_PAGE_SIZE - 1);
  static constexpr uint32_t GPU_PAGE_MASK = (~GPU_PAGE_OFFSET);
  static constexpr uint32_t CPU_PAGE_SIZE = 4096;
  static constexpr int BUFFER_SPLIT_SEGS = 2;
  static constexpr int MAX_ETH_HDR_SIZE = 18;

  void* get_segment_packet_ptr(BurstParams* burst, int seg, int idx) override;
  void* get_packet_ptr(BurstParams* burst, int idx) override;
  uint32_t get_segment_packet_length(BurstParams* burst, int seg, int idx) override;
  uint32_t get_packet_length(BurstParams* burst, int idx) override;
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
  Status set_all_packet_lengths(BurstParams* burst,
                                const std::initializer_list<int>& lens) override;
  void free_all_segment_packets(BurstParams* burst, int seg) override;
  void free_packet_segment(BurstParams* burst, int seg, int pkt) override;
  void free_packet(BurstParams* burst, int pkt) override;
  void free_all_packets(BurstParams* burst) override;
  void free_rx_burst(BurstParams* burst) override;
  void free_tx_burst(BurstParams* burst) override;

  Status get_rx_burst(BurstParams** burst, int port, int q) override;
  using daqiri::Engine::get_rx_burst;  // for overloads
  Status set_reorder_cuda_stream(const std::string& interface_name,
                                 const std::string& reorder_name,
                                 cudaStream_t stream) override;
  Status get_reorder_burst_info(BurstParams* burst, ReorderBurstInfo* info) override;
  Status set_packet_tx_time(BurstParams* burst, int idx, uint64_t timestamp);
  void free_rx_metadata(BurstParams* burst) override;
  void free_tx_metadata(BurstParams* burst) override;
  Status get_tx_metadata_buffer(BurstParams** burst) override;
  Status send_tx_burst(BurstParams* burst) override;
  Status get_mac_addr(int port, char* mac) override;
  Status drop_all_traffic(int port) override;
  Status allow_all_traffic(int port) override;
  void shutdown() override;
  void print_stats() override;
  void adjust_memory_regions() override;
  uint64_t get_burst_tot_byte(BurstParams* burst) override;
  BurstParams* create_tx_burst_params() override;
  bool validate_config() const override;
  uint16_t get_num_rx_queues(int port_id) const override;
  void flush_port_queue(int port, int queue) override;

 private:
  static void PrintDpdkStats(int port);
  static int rx_core_worker(void* arg);
  static int rx_core_multi_q_worker(void* arg);
  static int tx_core_worker(void* arg);
  static int rx_lb_worker(void* arg);
  static int tx_lb_worker(void* arg);
  static void flush_port_queue_impl(int port, int queue);
  bool setup_rx_timestamp_dynfield();
  bool setup_tx_timestamp_dynfield();
  bool calibrate_rx_timestamp_clock(uint16_t port_id);
  int setup_pools_and_rings(int max_rx_batch, int max_tx_batch);
  struct rte_flow* add_flow(int port, const FlowConfig& cfg);
  bool ensure_eth_jump_rule(int port, uint32_t group);
  void destroy_eth_jump_rules();
  bool add_send_to_kernel_fallback(int port, uint32_t group);
  void create_dummy_rx_q();
  void create_dummy_tx_q();
  struct rte_flow* add_modify_flow_set(int port, int queue, const char* buf, int len,
                                       Direction direction);

  struct rte_flow_item_flex_handle *create_flex_flow_rule(
    int port, int offset, struct rte_flow_item *udp_item, struct rte_flow_item *end_pattern);
  struct rte_flow* add_flex_item_flow(int port, const FlexItemMatch& match, uint16_t queue_id);

  bool apply_tx_offloads(int port);

  struct ReorderBatchState {
    uint64_t first_packet_cycles = 0;
    uint64_t first_packet_rx_timestamp_ns = 0;
    uint32_t input_payload_len = 0;
    uint32_t payload_len = 0;
    uint32_t packet_count = 0;
    bool first_packet_rx_timestamp_ns_valid = false;
  };

  struct ReorderOutputBufferState {
    void* ptr = nullptr;
    bool consumer_done = true;
    bool event_complete = true;
    cudaEvent_t event = nullptr;
    uint64_t* h_batch_id = nullptr;
    uint64_t* d_batch_id = nullptr;
    std::vector<struct rte_mbuf*> source_mbufs;
    uint32_t source_packet_count = 0;
#if DAQIRI_REORDER_GPU_PROFILE
    cudaEvent_t kernel_start_event = nullptr;
    cudaEvent_t kernel_stop_event = nullptr;
#endif
  };

  struct ReorderOutputPool {
    std::string mr_name;
    std::vector<ReorderOutputBufferState> buffers;
    size_t next_buffer = 0;
    int cuda_device_id = 0;
    bool cuda_events_enabled = false;
  };

  struct ReorderPendingCopy {
    std::shared_ptr<ReorderOutputPool> output_pool;
    size_t buffer_idx = 0;
  };

#if DAQIRI_REORDER_GPU_PROFILE
  struct ReorderGpuProfileStats {
    uint64_t gpu_kernel_samples = 0;
    double gpu_kernel_total_us = 0.0;
    double gpu_kernel_max_us = 0.0;
  };
#endif

  struct ReorderPlanRuntime {
    const ReorderConfig* config = nullptr;
    uint16_t port_id = 0;
    uint16_t queue_id = 0;
    std::string memory_region_name;
    std::shared_ptr<ReorderOutputPool> output_pool;
    uint32_t packets_per_batch = 0;
    uint32_t payload_byte_offset = 0;
    uint32_t copy_source_offset = 0;
    uint32_t slot_stride = 0;
    bool data_type_conversion_enabled = false;
    ReorderDataType input_data_type = ReorderDataType::SAME;
    ReorderDataType output_data_type = ReorderDataType::SAME;
    ReorderEndianness input_endianness = ReorderEndianness::HOST;
    uint64_t timeout_cycles = 0;
    bool use_gpu_backend = false;
    int cuda_device_id = 0;
    cudaStream_t stream = nullptr;
    uint32_t cuda_staging_capacity = 0;

    std::vector<void*> h_input_ptrs;
    std::vector<struct rte_mbuf*> h_source_mbufs;
    void** d_input_ptrs = nullptr;

    ReorderBatchState direct_arrival_batch;
    std::deque<ReorderPendingCopy> pending_copies;
#if DAQIRI_REORDER_GPU_PROFILE
    ReorderGpuProfileStats gpu_profile;
#endif
  };

  struct ReorderBurstContext {
    std::string mr_name;
    std::shared_ptr<ReorderOutputPool> output_pool;
    size_t buffer_idx = 0;
    std::array<void*, 1> pkt_ptrs{};
    std::array<uint32_t, 1> pkt_lens{};
    ReorderBurstInfo info{};
    const uint64_t* h_batch_id = nullptr;
    uint64_t rx_timestamp_ns = 0;
    bool rx_timestamp_ns_valid = false;
    bool released = false;
  };

  struct ReorderQueueState {
    bool enabled = false;
    bool single_plan_fast_path = false;
    std::unordered_map<uint16_t, size_t> flow_id_to_plan;
    std::vector<ReorderPlanRuntime> plans;
    std::vector<std::vector<int>> plan_pkt_indices;
    std::vector<size_t> plan_pkt_counts;
    std::vector<int> unmatched_indices;
    size_t unmatched_count = 0;
    std::deque<BurstParams*> ready_outputs;
  };

  static constexpr uint32_t kBurstFlagDpdkReordered = DAQIRI_BURST_FLAG_REORDERED;
  static constexpr uint32_t kBurstFlagDpdkReorderTimeout = DAQIRI_BURST_FLAG_REORDER_TIMEOUT;

  bool init_reorder_state();
  bool init_reorder_queue_state(const InterfaceConfig& intf, const RxQueueConfig& qcfg);
  void cleanup_reorder_state();
  Status poll_reorder_events(ReorderPlanRuntime& plan);
  Status process_burst_for_reorder(uint32_t key, ReorderQueueState& qstate, BurstParams* burst);
  Status flush_reorder_timeouts(ReorderQueueState& qstate, uint64_t now_cycles);
  Status flush_reorder_batch(ReorderPlanRuntime& plan,
                             uint32_t batch_id,
                             bool timeout_flush,
                             BurstParams** out_burst);
  Status create_reorder_output_burst(ReorderPlanRuntime& plan,
                                     std::shared_ptr<ReorderOutputPool> output_pool,
                                     size_t buffer_idx,
                                     void* output_buffer,
                                     uint32_t aggregate_len,
                                     uint32_t source_packet_count,
                                     uint32_t payload_len,
                                     uint64_t batch_id,
                                     bool batch_id_ready,
                                     const uint64_t* h_batch_id,
                                     uint64_t rx_timestamp_ns,
                                     bool rx_timestamp_ns_valid,
                                     cudaEvent_t event,
                                     bool timeout_flush,
                                     BurstParams** out_burst);
  Status acquire_reorder_output_buffer(ReorderPlanRuntime& plan, size_t* buffer_idx, void** output_buffer);
  void release_reorder_output_buffer(std::shared_ptr<ReorderOutputPool> output_pool,
                                     size_t buffer_idx);
  Status append_reorder_packet(ReorderPlanRuntime& plan,
                               struct rte_mbuf* mbuf,
                               void* pkt_ptr,
                               uint64_t now_cycles,
                               size_t* batch_size);
  Status get_next_output_or_ready(uint32_t key, ReorderQueueState& qstate, BurstParams** burst);
  void release_reorder_output_context(BurstParams* burst);

  std::array<struct rte_ether_addr, MAX_IFS> mac_addrs;
  std::unordered_map<uint32_t, struct rte_ring*> rx_rings;
  struct rte_ether_addr conf_ports_eth_addr[RTE_MAX_ETHPORTS];
  std::unordered_map<uint16_t, struct rte_flow_item_flex_handle*> flex_item_handles_;
  std::unordered_set<uint64_t> eth_jump_installed_;
  std::unordered_map<uint64_t, struct rte_flow*> eth_jump_flows_;
  std::unordered_map<uint32_t, struct rte_ring*> tx_rings;
  std::unordered_map<uint32_t, struct rte_mempool*> tx_burst_buffers;
  std::unordered_map<uint32_t, DPDKQueueConfig*> rx_dpdk_q_map_;
  std::unordered_map<uint32_t, DPDKQueueConfig*> tx_dpdk_q_map_;
  std::unordered_map<uint32_t, const RxQueueConfig*> rx_cfg_q_map_;
  std::unordered_map<uint16_t, std::pair<uint16_t, uint16_t>> port_q_num;
  struct rte_mempool* rx_burst_buffer;
  struct rte_mempool* rx_metadata;
  struct rte_mempool* tx_metadata;
  std::unordered_map<std::string, std::shared_ptr<ReorderOutputPool>> reorder_output_pools_;
  std::unordered_map<uint32_t, ReorderQueueState> reorder_queue_states_;
  std::mutex reorder_lock_;
  std::array<DropTrafficConfig, RTE_MAX_ETHPORTS> drop_all_traffic_flow;
  int timestamp_dynfield_offset_{-1};
  uint64_t rx_timestamp_dynflag_mask_{0};
  uint64_t tx_timestamp_dynflag_mask_{0};
  std::array<RxTimestampConversion, RTE_MAX_ETHPORTS> rx_timestamp_conversions_{};
  std::array<struct rte_eth_conf, MAX_INTERFACES> local_port_conf;
  DpdkStats stats_;
  struct rte_ring* loopback_ring;
  LoopbackType loopback_;
  std::thread stats_thread_;
  int num_init = 0;
};

};  // namespace daqiri
