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

#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>

#include <cuda_runtime.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "src/engine.h"
#include <daqiri/daqiri.h>

#include "src/daqiri_ring.h"
#include "src/daqiri_pool.h"

namespace daqiri {

// ---- GPU packet reordering (mirrors the DPDK reorder path, on MPRQ RX) ----
// One output buffer in the reorder output pool. The kernel reorders a batch of
// input packets into `ptr`; `event` fires when the copy completes, at which
// point the source strides (src_wqe/src_strd) are released.
struct IbvReorderOutBuf {
  uint8_t* ptr = nullptr;
  bool consumer_done = true;
  bool event_complete = true;
  cudaEvent_t event = nullptr;
  uint64_t* h_batch_id = nullptr;
  uint64_t* d_batch_id = nullptr;
  std::vector<uint16_t> src_wqe;
  std::vector<uint16_t> src_strd;
  uint32_t src_count = 0;
};

// One reorder plan (one ReorderConfig mapped to this queue). Accumulates input
// packets until packets_per_batch, then launches the reorder kernel.
struct IbvReorderPlan {
  ReorderConfig cfg;
  uint16_t port_id = 0;
  uint16_t queue_id = 0;
  uint32_t packets_per_batch = 0;
  uint32_t copy_source_offset = 0;
  uint32_t slot_stride = 0;
  bool data_type_conversion = false;
  int cuda_device_id = 0;
  cudaStream_t stream = nullptr;
  void** d_input_ptrs = nullptr;
  std::vector<IbvReorderOutBuf> out_bufs;
  size_t next_out = 0;
  // Accumulation for the in-progress batch.
  std::vector<void*> acc_ptrs;
  std::vector<uint16_t> acc_wqe;
  std::vector<uint16_t> acc_strd;
  uint32_t acc_input_payload_len = 0;
  uint32_t acc_output_payload_len = 0;
};

struct IbvReorderState {
  bool enabled = false;
  bool single_plan = false;
  std::vector<IbvReorderPlan> plans;
  std::unordered_map<FlowId, size_t> flow_to_plan;
  std::deque<BurstParams*> ready;
  std::mutex lock;
};

// Per-output-burst context (held in burst->custom_pkt_data) for a reordered
// burst handed to the application.
struct IbvReorderBurstCtx {
  IbvReorderState* state = nullptr;
  IbvReorderPlan* plan = nullptr;
  size_t out_idx = 0;
  std::array<void*, 1> pkt_ptrs{};
  std::array<uint32_t, 1> pkt_lens{};
  ReorderBurstInfo info{};
  const uint64_t* h_batch_id = nullptr;
  bool released = false;
};

/**
 * @brief Per-queue striding-RQ (MPRQ) receive context.
 *
 * One large memory region is carved into `num_wqe` stride regions; each region
 * is posted as a single recv WQE on the striding RQ. The NIC DMAs many packets
 * into each region, strided by `stride_size`, and the CQ reports one CQE per
 * packet carrying its stride count and byte length. Packet pointers are pure
 * arithmetic into the region (no per-packet allocation), and a region is
 * reposted only once every stride in it has been consumed AND released by the
 * application -- this per-WQE reclaim replaces the DPDK per-mbuf free.
 */
struct IbvRxQueue {
  int if_idx = 0;
  int port_id = 0;
  int queue_id = 0;
  int cpu_core = -1;
  int batch_size = 1;
  uint64_t timeout_us = 0;
  int num_segs = 1;
  int split_boundary = 0;
  // Flow id reported for packets on this queue. When a single configured flow
  // steers to this queue, get_packet_flow_id returns its id (per-queue fallback
  // for the flow_tag/MARK the DPDK backend used).
  FlowId flow_id = 0;

  // When false, a DevX *regular* (cyclic) RQ is used instead of MPRQ: one WQE
  // per packet, each WQE a multi-segment scatter list. Selected for physical HDS
  // (header -> CPU MR, payload -> GPU MR) or multi-region queues, which MPRQ's
  // single-stride scatter can't express. Slower than MPRQ (1 WQE+CQE/packet).
  bool striding = true;

  // Striding-RQ geometry.
  uint32_t stride_size = 0;      // bytes per stride
  uint32_t strides_per_wqe = 0;  // strides in one WQE / region
  uint32_t num_wqe = 0;          // number of stride regions / outstanding WQEs
  uint32_t region_size = 0;      // strides_per_wqe * stride_size
  uint8_t two_byte_shift = 0;    // mlx5 2-byte L3 alignment pad

  // RQ WQE geometry (set in devx_create_rq): striding -> 32 B mlx5_wqe_mprq
  // (data seg at +16); regular -> num_segs*16 B of data segs at offset 0.
  uint32_t wqe_stride = 0;    // WQE byte size in the WQ ring
  uint32_t wqe_dseg_off = 0;  // offset of the first data segment within a WQE

  // Backing memory region(s). The striding path uses the single mr_base/lkey;
  // the regular path scatters across `regions` (one data seg per region, in
  // order -- for HDS, region 0 = CPU header (seg_len = split_boundary), region
  // 1 = GPU payload).
  struct RxRegion {
    uint8_t* base = nullptr;
    uint32_t lkey = 0;
    uint32_t slot_size = 0;  // per-packet buffer stride within the region
    uint32_t seg_len = 0;    // scatter length for this region's data seg
  };
  std::vector<RxRegion> regions;
  std::vector<std::string> mr_names;  // configured MR list for this queue
  std::string mr_name;
  uint8_t* mr_base = nullptr;  // base VA of the stride pool (striding path)
  uint32_t lkey = 0;

  // ibverbs/mlx5dv objects.
  struct ibv_context* ctx = nullptr;
  struct ibv_pd* pd = nullptr;
  struct ibv_cq* cq = nullptr;
  struct ibv_wq* wq = nullptr;  // verbs (non-striding / legacy) path
  struct ibv_rwq_ind_table* ind_table = nullptr;
  struct ibv_qp* qp = nullptr;

  // DevX MPRQ objects (striding path). The RQ + its WQE ring/doorbell are built
  // by hand; a direct TIR + mlx5dv_dr rule steer matching traffic to the RQ.
  struct mlx5dv_devx_uar* devx_uar = nullptr;
  struct mlx5dv_devx_obj* cq_obj = nullptr;  // DevX CQ (striding path)
  void* cq_buf = nullptr;                    // CQ buffer + dbr (single umem)
  struct mlx5dv_devx_umem* cq_umem = nullptr;
  void* wq_buf = nullptr;  // RQ WQE ring (one scatter seg/WQE)
  struct mlx5dv_devx_umem* wq_umem = nullptr;
  uint32_t* rq_dbr = nullptr;  // RQ doorbell record
  struct mlx5dv_devx_umem* dbr_umem = nullptr;
  struct mlx5dv_devx_obj* td_obj = nullptr;
  uint32_t td_num = 0;
  struct mlx5dv_devx_obj* rq_obj = nullptr;
  uint32_t rqn = 0;
  struct mlx5dv_devx_obj* tir_obj = nullptr;
  uint32_t rq_pi = 0;  // RQ producer index (WQEs posted)
  struct mlx5dv_dr_domain* dr_domain = nullptr;
  struct mlx5dv_dr_table* dr_table = nullptr;
  struct mlx5dv_dr_matcher* dr_matcher = nullptr;
  struct mlx5dv_dr_rule* dr_rule = nullptr;
  struct mlx5dv_dr_action* dr_action = nullptr;

  // Direct mlx5dv views (we own the CQ consumer index; rdma-core owns the RQ).
  struct mlx5dv_cq dv_cq {};
  uint32_t cq_ci = 0;  // CQ consumer index (monotonic)

  // Per-WQE stride accounting for the reclaim path. Indexed by WQE/region.
  std::vector<uint32_t> consumed_strides;  // strides handed to the app (verbs path)
  std::vector<uint32_t> released_strides;  // strides freed by the app (verbs path)
  // DevX path: strides freed per region, fetch_add'd by both the app (data
  // frees) and the worker (filler frees); drained only by the worker thread.
  std::unique_ptr<std::atomic<uint32_t>[]> freed_strides;
  uint32_t cur_wqe = 0;              // region currently being filled
  uint32_t cur_consumed = 0;         // strides consumed in cur_wqe
  std::atomic<uint64_t> reposts{0};  // diagnostic: WQE reposts

  // App-facing burst ring (worker enqueues, get_rx_burst dequeues).
  daqiri::Ring* ring = nullptr;

  // Optional GPU reordering state for this queue.
  std::unique_ptr<IbvReorderState> reorder;

  // In-progress burst accumulator (kept in the queue, not on the worker stack,
  // so one poller thread can round-robin several queues and hold a partial
  // burst per queue between passes).
  BurstParams* cur_burst = nullptr;
  int cur_n = 0;
  uint64_t last_flush_tsc = 0;
  uint64_t dbg_cqe = 0, dbg_data = 0, dbg_filler = 0, dbg_err = 0;
  // App-ring-full drops: the worker assembled a burst but get_rx_burst wasn't
  // draining fast enough, so the burst was dropped (strides released, metadata
  // returned). Worker-written, read by print_stats.
  uint64_t ring_full_bursts = 0;
  uint64_t ring_full_pkts = 0;

  // Worker thread + lifecycle. With multiple queues sharing a cpu_core, only the
  // group's leader queue owns the thread; the rest are serviced by it.
  std::thread worker;
  std::atomic<bool> running{false};
};

/**
 * @brief Per-queue raw-packet TX context.
 *
 * The TX memory region is carved into fixed-size slots held in a free ring; a
 * burst pulls slots, the app fills them, and send_tx_burst posts one WR per
 * packet (wr_id = slot pointer). A completion worker returns slots to the ring.
 * No per-packet allocation.
 */
struct IbvTxQueue {
  int port_id = 0;
  int queue_id = 0;
  int cpu_core = -1;
  int batch_size = 1;
  int num_segs = 1;
  // One TX region per packet segment. Single-region (normal) TX uses region 0
  // only; HDS TX scatters segment s from regions[s] (region 0 = CPU header MR,
  // region 1 = GPU payload MR), each with its own lkey. mr_base/lkey/slot_size
  // mirror region 0.
  struct TxRegion {
    uint8_t* base = nullptr;
    uint32_t lkey = 0;
    uint32_t slot_size = 0;
  };
  std::vector<TxRegion> regions;
  std::vector<std::string> mr_names;
  std::string mr_name;
  uint8_t* mr_base = nullptr;
  uint32_t lkey = 0;
  uint32_t slot_size = 0;
  uint32_t num_slots = 0;
  struct ibv_context* ctx = nullptr;
  struct ibv_pd* pd = nullptr;
  struct ibv_cq* cq = nullptr;
  struct ibv_qp* qp = nullptr;
  // Direct mlx5 SQ access (build send WQEs by hand, ring the BlueFlame
  // doorbell -- bypasses ibv_post_send's per-WR overhead).
  struct mlx5dv_qp dv_qp {};
  struct mlx5dv_cq dv_txcq {};
  uint32_t sqn = 0;
  uint64_t sq_pi = 0;  // monotonic SQ producer (WQEBB units)
  uint64_t sq_completed = 0;
  uint32_t sq_capacity_wqebbs = 0;
  uint32_t bf_offset = 0;  // toggles between 0 and bf.size each doorbell
  uint32_t tx_cq_ci = 0;   // TX CQ consumer index
  // Slots are handed out cyclically and posted/completed in order, so the whole
  // free/in-flight lifecycle is two counters instead of rings (slot k lives at
  // mr_base + (k % num_slots) * slot_size). alloc_head is owned by the app fill
  // thread; completed_tail is advanced by the TX worker. In flight =
  // alloc_head - completed_tail, which must stay <= num_slots.
  uint64_t alloc_head = 0;
  std::atomic<uint64_t> completed_tail{0};
  // Accurate send scheduling (wait-on-time): a scheduled packet emits a WAIT
  // WQE before its send WQE, so a WQEBB index no longer maps 1:1 to a slot.
  // A WQE may span multiple WQEBBs, so packet-slot and SQ-credit completion
  // progress are tracked independently at each WQE's first WQEBB.
  std::vector<uint64_t> wqe_slot_cum;
  std::vector<uint64_t> wqe_wqebb_cum;
  uint64_t slots_posted = 0;
  bool send_scheduling = false;  // HCA wait_on_time present + real-time clock
  uint64_t rt_timemask = 0;      // wait segment comparison mask
  bool empw_enabled = false;
  // Hand-off-ring-full drops: send_tx_burst couldn't enqueue (TX worker behind),
  // so the burst was dropped and its slots rolled back. App-thread-written.
  uint64_t handoff_drop_bursts = 0;
  uint64_t handoff_drop_pkts = 0;
  // tx_eth_src offload: when set, set_eth_header stamps the port's MAC as the
  // Ethernet source so the application doesn't have to supply it.
  bool insert_eth_src = false;
  uint8_t eth_src[6] = {0};
  daqiri::Ring* send_ring = nullptr;  // bursts handed off for posting
  std::thread compl_worker;
  std::atomic<bool> running{false};
};

/**
 * @brief Pure-ibverbs raw-ethernet backend using a Mellanox/mlx5 Multi-Packet
 * (striding) Receive Queue.
 *
 * Selected via `stream_type: raw` + `engine: ibverbs`. Removes the per-packet
 * rte_mbuf allocation/free overhead of the DPDK backend: packets DMA into one
 * large pre-posted buffer strided by a fixed amount; RX only advances an index,
 * and reclaim happens per-WQE (one repost amortized over many packets).
 *
 * DPDK EAL is still initialized because the worker->app burst handoff reuses
 * rte_ring/rte_mempool (same as the RDMA backend); the NIC itself is driven
 * entirely through ibverbs/mlx5dv.
 */
class IbverbsEngine : public Engine {
 public:
  IbverbsEngine() = default;
  ~IbverbsEngine() override;

  bool set_config_and_initialize(const NetworkConfig& cfg) override;
  void initialize() override;
  void run() override;

  // Packet / segment accessors
  void* get_packet_ptr(BurstParams* burst, int idx) override;
  uint32_t get_packet_length(BurstParams* burst, int idx) override;
  void* get_segment_packet_ptr(BurstParams* burst, int seg, int idx) override;
  uint32_t get_segment_packet_length(BurstParams* burst, int seg, int idx) override;
  FlowId get_packet_flow_id(BurstParams* burst, int idx) override;
  Status get_packet_rx_timestamp(BurstParams* burst, int idx, uint64_t* timestamp_ns) override;
  void* get_packet_extra_info(BurstParams* burst, int idx) override;

  // TX construction / header fill (implemented in the TX milestone)
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
  Status set_packet_tx_time(BurstParams* burst, int idx, uint64_t time) override;

  // Free family
  void free_all_segment_packets(BurstParams* burst, int seg) override;
  void free_all_packets(BurstParams* burst) override;
  void free_packet_segment(BurstParams* burst, int seg, int pkt) override;
  void free_packet(BurstParams* burst, int pkt) override;
  void free_rx_burst(BurstParams* burst) override;
  void free_tx_burst(BurstParams* burst) override;
  void free_rx_metadata(BurstParams* burst) override;
  void free_tx_metadata(BurstParams* burst) override;

  // Burst retrieval / submission
  Status get_rx_burst(BurstParams** burst, int port, int q) override;
  Status get_tx_metadata_buffer(BurstParams** burst) override;
  Status send_tx_burst(BurstParams* burst) override;
  BurstParams* create_tx_burst_params() override;
  uint64_t get_burst_tot_byte(BurstParams* burst) override;

  // Control / stats
  Status get_mac_addr(int port, char* mac) override;
  Status drop_all_traffic(int port) override;
  Status allow_all_traffic(int port) override;
  Status add_rx_flow_async(int port, const FlowRuleConfig& flow, FlowOpId* op_id) override;
  Status add_rx_flows_async(int port, const std::vector<FlowRuleConfig>& flows, FlowOpId* op_id) override;
  Status delete_flow_async(FlowId flow_id, FlowOpId* op_id) override;
  Status poll_flow_op(FlowOpResult* result) override;
  uint16_t get_num_rx_queues(int port_id) const override;
  bool validate_config() const override;
  void shutdown() override;
  void print_stats() override;

  // GPU reorder hooks
  Status set_reorder_cuda_stream(const std::string& interface_name, const std::string& reorder_name,
                                 cudaStream_t stream) override;
  Status get_reorder_burst_info(BurstParams* burst, ReorderBurstInfo* info) override;

 private:
  // ---- bring-up ----
  struct ibv_context* open_device_for_interface(const InterfaceConfig& intf);
  Status setup_rx_queue(IbvRxQueue& q, const InterfaceConfig& intf, const RxQueueConfig& qcfg);
  Status register_rx_mr(IbvRxQueue& q);      // ibv_reg_mr (CPU); dmabuf later
  Status create_striding_rq(IbvRxQueue& q);  // verbs WQ + ind table + raw QP
  Status post_all_wqes(IbvRxQueue& q);
  Status repost_wqe(IbvRxQueue& q, uint32_t wqe_idx);

  // DevX striding-RQ (real MPRQ) path.
  Status create_striding_rq_devx(IbvRxQueue& q);
  Status create_regular_rq(IbvRxQueue& q);  // non-striding DevX RQ (multi-SGE / HDS)
  Status devx_create_cq(IbvRxQueue& q);
  Status devx_create_rq(IbvRxQueue& q, uint32_t stride_log, uint32_t strides_log);
  Status devx_create_tir(IbvRxQueue& q);  // also creates q.dr_action (dest TIR)
  // Per-port flow steering: one shared mlx5dv_dr domain+table per port, with a
  // matcher+rule per configured flow pointing at the target queue's TIR (or a
  // single catch-all rule -> the only queue when no flows are configured).
  // Installed once, after all RX queues on every port are set up.
  Status install_port_flows();
  Status install_tx_flows();
  // Probe HCA caps for accurate-send-scheduling (wait-on-time) support. Logs
  // wait_on_time + device_frequency_khz and returns true when the WAIT-WQE TX
  // scheduling path is usable (wait_on_time + real-time clock).
  bool probe_send_scheduling(struct ibv_context* ctx);
  // Create a flex-parser (parse-graph) node anchored at `arc_node` (entered when
  // that node's transition value equals `compare_value`) that samples 4 bytes at
  // `offset`; returns the DevX object and the device-assigned sample field id
  // (used in the misc4 match parameters). Used for both flex-item matching
  // (anchor UDP, compare = udp dst port) and ipv4_len matching (anchor MAC,
  // compare = IPv4 ethertype, sampling the IPv4 header's total_length field).
  struct mlx5dv_devx_obj* create_flex_parser_node(struct ibv_context* ctx, uint8_t arc_node,
                                                  uint16_t compare_value, uint16_t offset,
                                                  uint32_t* out_sample_id);
  // Create the per-port eCPRI parse-graph node: anchored at MAC on the eCPRI
  // EtherType (0xAEFE) with two 4-byte samples -- offset 0 (common header, the
  // message type) and offset 4 (message body, the pc_id/rtc_id). Returns the
  // DevX object and the two device-assigned sample field ids.
  struct mlx5dv_devx_obj* create_ecpri_parser_node(struct ibv_context* ctx,
                                                   uint32_t* out_type_sample_id,
                                                   uint32_t* out_id_sample_id);
  void devx_build_wqe(IbvRxQueue& q, uint32_t wqe_idx);  // write one RQ WQE
  void devx_ring_rq_doorbell(IbvRxQueue& q);
  void devx_advance_producer(IbvRxQueue& q);  // worker-only: refill freed regions
  void devx_destroy(IbvRxQueue& q);

  // ---- GPU reorder ----
  Status init_reorder(IbvRxQueue& q, const InterfaceConfig& intf, const RxQueueConfig& qcfg);
  Status reorder_get_rx(IbvRxQueue& q, BurstParams** burst);  // reorder path of get_rx_burst
  void reorder_poll_events(IbvRxQueue& q,
                           IbvReorderPlan& plan);             // free sources of finished batches
  void reorder_process_raw(IbvRxQueue& q, BurstParams* raw);  // route + accumulate + flush
  Status reorder_flush_batch(IbvRxQueue& q, IbvReorderPlan& plan, BurstParams** out);
  void reorder_release_output(BurstParams* burst);  // free a delivered reordered burst
  void reorder_cleanup(IbvRxQueue& q);

  // ---- RX hot path ----
  // One poller thread services a group of RX queues that share a cpu_core,
  // round-robin. rx_poll_queue does one bounded pass over a single queue's CQ.
  void rx_worker(std::vector<IbvRxQueue*> group);
  void rx_poll_queue(IbvRxQueue* q);
  bool rx_alloc_burst(IbvRxQueue* q);  // grab a metadata burst into q->cur_burst
  void rx_flush_burst(IbvRxQueue* q);  // enqueue q->cur_burst to the app ring
  // Release `strd` strides belonging to `wqe_idx`; repost the region if fully
  // released. This is the per-burst free hot path.
  void release_strides(IbvRxQueue& q, uint32_t wqe_idx, uint32_t strd);

  // ---- TX path ----
  Status setup_tx_queue(IbvTxQueue& q, const InterfaceConfig& intf, const TxQueueConfig& qcfg);
  Status create_tx_raw_qp(IbvTxQueue& q);                 // IBV_QPT_RAW_PACKET, RESET->RTS
  void post_tx_burst(IbvTxQueue& q, BurstParams* burst);  // build send WQEs + ring doorbell
  void post_tx_burst_empw(IbvTxQueue& q, BurstParams* burst);
  // Build a WAIT-on-time WQE (ctrl + wseg = 1 WQEBB, no slot) at q.sq_pi that
  // holds the following send(s) until the NIC real-time clock reaches when_ns,
  // advance sq_pi, and return its ctrl segment (for the BlueFlame doorbell).
  void* emit_wait_wqe(IbvTxQueue& q, uint64_t when_ns);
  uint64_t tx_burst_wqebbs(const IbvTxQueue& q, const BurstParams* burst) const;
  bool tx_sq_has_space(IbvTxQueue& q, uint64_t needed_wqebbs);
  void poll_tx_completions(IbvTxQueue& q);  // drain TX CQ, reclaim slot-runs
  // One worker services a group of TX queues sharing a cpu_core, round-robin:
  // drains each send_ring (post) + reclaims completions.
  void tx_worker(std::vector<IbvTxQueue*> group);
  IbvTxQueue* find_tx_queue(int port, int q);

  // ---- helpers ----
  static int mr_access_to_ibv(uint32_t access);
  // Register a configured MR (host/huge via ibv_reg_mr, device via dmabuf) and
  // return its base pointer + lkey.
  Status register_mr(struct ibv_pd* pd, const std::string& mr_name, uint8_t** out_base,
                     uint32_t* out_lkey);
  IbvRxQueue* find_rx_queue(int port, int q);
  // Resolve a port's kernel netdev name via sysfs (ibv device -> .../device/net).
  std::string port_netdev(int port) const;
  // Raise each port's netdev MTU to cover the largest configured frame. Unlike
  // DPDK, the ibverbs path uses the kernel netdev directly and does not own the
  // port MTU, so jumbo frames are silently dropped on RX if the MTU is too low.
  void ensure_port_mtus();

  // ---- dynamic RX flow lifecycle ----
  enum class DynamicFlowState { ACTIVE, DELETING };
  struct DynamicFlowEntry {
    FlowId flow_id = 0;
    int port = 0;
    uint16_t queue = 0;
    struct mlx5dv_dr_matcher* matcher = nullptr;
    struct mlx5dv_dr_rule* rule = nullptr;
    struct mlx5dv_dr_action* tag_action = nullptr;
    std::vector<struct mlx5dv_dr_action*> reformat_actions;
    std::vector<std::vector<uint8_t>> reformat_buffers;
    DynamicFlowState state = DynamicFlowState::ACTIVE;
  };
  const InterfaceConfig* find_interface_config(int port) const;
  bool reserve_static_flow_ids();
  FlowOpId allocate_flow_op_id_locked();
  bool has_dynamic_flow_id_capacity_locked(size_t count) const;
  FlowId allocate_dynamic_flow_id_locked();
  void release_dynamic_flow_id_locked(FlowId flow_id);
  bool validate_dynamic_rx_flow_locked(int port, const FlowRuleConfig& flow) const;
  Status create_dynamic_flow_locked(int port, const FlowRuleConfig& flow, FlowId flow_id);
  void destroy_dynamic_flow_entry_locked(DynamicFlowEntry& entry);
  void cleanup_dynamic_flows_locked();
  void enqueue_flow_completion_locked(const FlowOpResult& result);

  // ---- state ----
  std::atomic<bool> force_quit_{false};

  // One ibv_context + PD per opened device, keyed by device name.
  std::unordered_map<std::string, struct ibv_context*> ctx_map_;
  std::unordered_map<struct ibv_context*, struct ibv_pd*> pd_map_;
  // Registrations may be shared by queue setup only through their backing
  // allocation, so retain every verbs object and deregister it before its PD.
  std::vector<struct ibv_mr*> registered_mrs_;

  // Cached mlx5 clock-info per device for converting the CQE's free-running HW
  // timestamp to nanoseconds (mlx5dv_ts_to_ns). Refreshed lazily (the HW clock
  // wraps), guarded because app RX-consume threads may convert concurrently.
  struct ClockCache {
    uint64_t refresh_tsc = 0;  // rte timer cycles at last refresh
    bool valid = false;
    struct mlx5dv_clock_info info {};
  };
  std::unordered_map<struct ibv_context*, ClockCache> clock_cache_;
  std::mutex clock_mtx_;
  uint64_t ts_to_ns(struct ibv_context* ctx, uint64_t raw_ts);

  // RX/TX queues, owned here. Pointers handed to worker threads are stable.
  std::vector<std::unique_ptr<IbvRxQueue>> rx_queues_;
  std::vector<std::unique_ptr<IbvTxQueue>> tx_queues_;

  // Per-port mlx5dv_dr flow steering: one domain + table shared by all RX
  // queues on the port; one matcher+rule per flow (or a catch-all). Torn down
  // before the queues' TIRs in shutdown (rules reference the queue dr_actions).
  struct PortSteering {
    struct mlx5dv_dr_domain* domain = nullptr;
    struct mlx5dv_dr_table* table = nullptr;
    std::vector<struct mlx5dv_dr_matcher*> matchers;
    std::vector<struct mlx5dv_dr_rule*> rules;
    std::vector<struct mlx5dv_dr_action*> tag_actions;  // per-flow MARK tag actions
    std::vector<struct mlx5dv_dr_action*> reformat_actions;
    std::vector<std::vector<uint8_t>> reformat_buffers;
    // Enough to recreate each rule for allow_all_traffic after a drop_all.
    struct RuleSpec {
      struct mlx5dv_dr_matcher* matcher;
      struct mlx5dv_dr_action* action;         // dest-TIR
      struct mlx5dv_dr_action* tag = nullptr;  // optional MARK tag action
      std::vector<struct mlx5dv_dr_action*> reformats;
      size_t value_sz = 0;                     // bytes of `value` in use
      uint64_t value[64];                      // up to full fte_match_param (512 B)
    };
    std::vector<RuleSpec> rule_specs;
    // Flex-parser (parse-graph) nodes for arbitrary-offset / ipv4_len matching,
    // keyed by the config flex-item id. The DevX object must outlive any matcher
    // that references its sample_field_id, so these are torn down after rules.
    struct FlexNode {
      struct mlx5dv_devx_obj* obj = nullptr;
      uint32_t sample_field_id = 0;
    };
    std::map<uint16_t, FlexNode> flex_nodes;
    // Shared parse-graph node sampling the IPv4 header's total_length, created
    // lazily when the first ipv4_len flow is installed (anchored at L2 on the
    // IPv4 ethertype). Torn down with the flex_nodes after rules.
    FlexNode ipv4_len_node;
    // Shared parse-graph node for eCPRI-over-Ethernet matching (anchored at L2
    // on the eCPRI EtherType), created lazily when the first eCPRI flow that
    // matches a message type or identifier is installed. Carries two sample
    // registers: type_sample_id (common header, offset 0) and id_sample_id
    // (message body, offset 4). Torn down with the flex_nodes after rules.
    struct EcpriNode {
      struct mlx5dv_devx_obj* obj = nullptr;
      uint32_t type_sample_id = 0;
      uint32_t id_sample_id = 0;
    };
    EcpriNode ecpri_node;
    bool dropped = false;
    // Continue directly after static rules; sparse high priorities can fail on
    // some mlx5 DR stacks even when the same matcher/action works at init.
    int next_dynamic_priority = 0;
  };
  std::map<int, PortSteering> port_steering_;  // port_id -> steering

  bool create_dr_rule_locked(int port,
                             PortSteering& st,
                             uint16_t criteria,
                             struct mlx5dv_flow_match_parameters* mask,
                             struct mlx5dv_flow_match_parameters* value,
                             IbvRxQueue* dest,
                             int priority,
                             FlowId flow_id,
                             const char* desc,
                             DynamicFlowEntry* dynamic_entry,
                             const std::vector<struct mlx5dv_dr_action*>& reformats =
                                 std::vector<struct mlx5dv_dr_action*>{});
  Status install_flow_rule_locked(int port,
                                  PortSteering& st,
                                  const InterfaceConfig& intf,
                                  const FlowRuleConfig& flow,
                                  FlowId flow_id,
                                  int priority,
                                  DynamicFlowEntry* dynamic_entry);
  // Fill an eCPRI flow's match mask/value (always pinning the eCPRI EtherType in
  // outer_headers, and the message type / identifier in misc_parameters_4 when
  // requested), lazily creating the port's shared eCPRI parse-graph node.
  // Accumulates the match criteria bits. Shared by the static and dynamic flow
  // installers.
  Status build_ecpri_match_locked(struct ibv_context* ctx, PortSteering& st, const EcpriMatch& em,
                                  uint8_t* mask_buf, uint8_t* value_buf, uint16_t* criteria);

  std::mutex flow_lock_;
  FlowId next_dynamic_flow_id_ = 1;
  FlowOpId next_flow_op_id_ = 1;
  std::unordered_set<FlowId> static_flow_ids_;
  std::queue<FlowId> free_dynamic_flow_ids_;
  std::unordered_map<FlowId, DynamicFlowEntry> dynamic_flows_;
  std::queue<FlowOpResult> ready_flow_ops_;

  struct TxPortSteering {
    struct mlx5dv_dr_domain* domain = nullptr;
    struct mlx5dv_dr_table* table = nullptr;
    std::vector<struct mlx5dv_dr_matcher*> matchers;
    std::vector<struct mlx5dv_dr_rule*> rules;
    std::vector<struct mlx5dv_dr_action*> reformat_actions;
    std::vector<std::vector<uint8_t>> reformat_buffers;
  };
  std::map<int, TxPortSteering> tx_port_steering_;

  // Burst metadata pools. Each element is a BurstParams + inline pointer/length
  // /stride arrays; see the .cpp for the layout.
  daqiri::ObjectPool* rx_meta_pool_ = nullptr;
  daqiri::ObjectPool* tx_meta_pool_ = nullptr;
  size_t rx_meta_pool_size_ = 0;
  uint32_t max_batch_ = 0;  // largest batch_size across RX+TX queues

  // Tag every burst metadata block so free paths can find the owning queue.
  // Stored in the burst's BurstHeader.
};

};  // namespace daqiri
