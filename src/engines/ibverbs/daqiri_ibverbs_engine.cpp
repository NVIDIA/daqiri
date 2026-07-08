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

#include "src/engines/ibverbs/daqiri_ibverbs_engine.h"
#include "src/engines/ibverbs/mlx5_prm_min.h"
#include "src/kernels.h"

#include <cuda.h>
#include <arpa/inet.h>
#include <endian.h>
#include <dirent.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/sockios.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "src/daqiri_ring.h"
#include "src/daqiri_pool.h"

#include <daqiri/logging.hpp>

namespace daqiri {

namespace {
// Monotonic nanosecond clock used for the TX flush timeouts. Replaces DPDK's
// rte_get_timer_cycles()/rte_get_timer_hz() so the ibverbs engine needs no
// libdpdk: "cycles" are nanoseconds, so the timer frequency is 1e9.
constexpr uint64_t ibv_timer_hz = 1'000'000'000ULL;
constexpr FlowId kMaxIbverbsFlowTag = 0x00ffffffU;
constexpr int kIbverbsCatchAllPriority = 1'000'000;
inline uint64_t ibv_now_ns() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}
}  // namespace

// ---------------------------------------------------------------------------
// mlx5 Multi-Packet RQ CQE decode constants (not exported by mlx5dv.h; these
// mirror the mlx5 PMD's byte_cnt layout). With striding RQ, each CQE's byte_cnt
// packs: bit 31 = filler (region padding, no packet), bits 16..29 = number of
// strides this packet consumed, bits 0..15 = packet byte length.
// ---------------------------------------------------------------------------
static constexpr uint32_t MPRQ_FILLER_FLAG = 0x80000000u;
static constexpr uint32_t MPRQ_STRIDE_NUM_SHIFT = 16u;
static constexpr uint32_t MPRQ_STRIDE_NUM_MASK = 0x3fff0000u;
static constexpr uint32_t MPRQ_LEN_MASK = 0x0000ffffu;

// Marks a burst's metadata block as TX-owned so the shared free family routes
// it to the TX slot ring rather than the RX stride-release path.
static constexpr uint32_t IBV_TX_BURST_FLAG = 1u << 27;

// Defaults for the striding-RQ geometry; clamped to device caps at setup.
static constexpr uint32_t DEFAULT_STRIDE_LOG = 11;          // 2048 B per stride
static constexpr uint32_t DEFAULT_STRIDES_PER_WQE_LOG = 9;  // 512 strides / WQE
static constexpr uint32_t MIN_NUM_WQE = 4;                  // outstanding regions

// Read barrier between observing a CQE owner flip and reading its payload.
// On weakly-ordered ARM this must be a real device barrier.
static inline void cqe_read_barrier() {
#if defined(__aarch64__)
  asm volatile("dmb oshld" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
  asm volatile("" ::: "memory");
#else
  std::atomic_thread_fence(std::memory_order_acquire);
#endif
}

// Store barrier ordering WQE writes before the doorbell record store the device
// reads via DMA (outer-shareable on ARM).
static inline void doorbell_store_barrier() {
#if defined(__aarch64__)
  asm volatile("dmb oshst" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
  asm volatile("sfence" ::: "memory");
#else
  std::atomic_thread_fence(std::memory_order_release);
#endif
}

static void append_bytes(std::vector<uint8_t>& dst, const void* src, size_t len) {
  const auto* bytes = static_cast<const uint8_t*>(src);
  dst.insert(dst.end(), bytes, bytes + len);
}

static void set_u24_be(uint8_t out[3], uint32_t value) {
  out[0] = static_cast<uint8_t>((value >> 16) & 0xff);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xff);
  out[2] = static_cast<uint8_t>(value & 0xff);
}

static bool fill_tunnel_l2_l3(std::vector<uint8_t>& data, const TunnelConfig& tunnel,
                              uint8_t ip_proto) {
  struct ethhdr eth {};
  format_eth_addr(reinterpret_cast<char*>(eth.h_dest), tunnel.outer_eth_dst_);
  format_eth_addr(reinterpret_cast<char*>(eth.h_source), tunnel.outer_eth_src_);
  eth.h_proto = htobe16(ETH_P_IP);

  struct iphdr ip {};
  ip.version = 4;
  ip.ihl = 5;
  ip.ttl = tunnel.outer_ipv4_ttl_;
  ip.tos = tunnel.outer_ipv4_tos_;
  ip.protocol = ip_proto;
  if (inet_pton(AF_INET, tunnel.outer_ipv4_src_.c_str(), &ip.saddr) != 1 ||
      inet_pton(AF_INET, tunnel.outer_ipv4_dst_.c_str(), &ip.daddr) != 1) {
    return false;
  }
  append_bytes(data, &eth, sizeof(eth));
  append_bytes(data, &ip, sizeof(ip));
  return true;
}

static bool build_reformat_buffer(const FlowAction& action, std::vector<uint8_t>& data,
                                  enum mlx5dv_flow_action_packet_reformat_type* type) {
  data.clear();
  if (type == nullptr) { return false; }
  if (action.type_ == FlowType::VLAN_PUSH || action.type_ == FlowType::VLAN_POP) {
    const uint16_t tci = static_cast<uint16_t>(((action.vlan_.pcp_ & 0x7) << 13) |
                                              ((action.vlan_.dei_ & 0x1) << 12) |
                                              (action.vlan_.vlan_id_ & 0x0fff));
    const uint16_t tpid = htobe16(action.vlan_.ethertype_);
    const uint16_t be_tci = htobe16(tci);
    append_bytes(data, &tpid, sizeof(tpid));
    append_bytes(data, &be_tci, sizeof(be_tci));
    *type = action.type_ == FlowType::VLAN_PUSH
                ? MLX5DV_FLOW_ACTION_PACKET_REFORMAT_TYPE_L2_TO_L2_TUNNEL
                : MLX5DV_FLOW_ACTION_PACKET_REFORMAT_TYPE_L2_TUNNEL_TO_L2;
    return true;
  }

  if (action.type_ != FlowType::TUNNEL_ENCAP && action.type_ != FlowType::TUNNEL_DECAP) {
    return false;
  }

  const TunnelConfig& tunnel = action.tunnel_;
  switch (tunnel.type_) {
    case TunnelType::VXLAN: {
      if (!fill_tunnel_l2_l3(data, tunnel, IPPROTO_UDP)) { return false; }
      struct udphdr udp {};
      udp.source = htobe16(tunnel.outer_udp_src_);
      udp.dest = htobe16(tunnel.outer_udp_dst_);
      struct {
        uint32_t flags;
        uint32_t vni;
      } __attribute__((packed)) vxlan {htobe32(0x08000000u), htobe32(tunnel.vni_ << 8)};
      append_bytes(data, &udp, sizeof(udp));
      append_bytes(data, &vxlan, sizeof(vxlan));
      break;
    }
    case TunnelType::GRE: {
      if (!fill_tunnel_l2_l3(data, tunnel, IPPROTO_GRE)) { return false; }
      struct {
        uint16_t flags;
        uint16_t proto;
      } __attribute__((packed)) gre {0, htobe16(tunnel.gre_protocol_)};
      append_bytes(data, &gre, sizeof(gre));
      break;
    }
    case TunnelType::NVGRE: {
      if (!fill_tunnel_l2_l3(data, tunnel, IPPROTO_GRE)) { return false; }
      struct {
        uint16_t flags;
        uint16_t proto;
        uint8_t tni[3];
        uint8_t flow_id;
      } __attribute__((packed)) nvgre {};
      nvgre.flags = htobe16(0x2000);
      nvgre.proto = htobe16(ETH_P_TEB);
      set_u24_be(nvgre.tni, tunnel.tni_);
      nvgre.flow_id = tunnel.flow_id_;
      append_bytes(data, &nvgre, sizeof(nvgre));
      break;
    }
    case TunnelType::NONE:
      return false;
  }

  *type = action.type_ == FlowType::TUNNEL_ENCAP
              ? MLX5DV_FLOW_ACTION_PACKET_REFORMAT_TYPE_L2_TO_L2_TUNNEL
              : MLX5DV_FLOW_ACTION_PACKET_REFORMAT_TYPE_L2_TUNNEL_TO_L2;
  return true;
}

static inline uint32_t log2_floor(uint32_t v) {
  uint32_t r = 0;
  while (v > 1) {
    v >>= 1;
    ++r;
  }
  return r;
}

// ---- reorder bit helpers (mirror the DPDK reorder path) ----
static inline uint16_t rdr_seq_off(const ReorderConfig& c) {
  return c.method_ == ReorderMethod::SEQ_BATCH_NUMBER
             ? c.seq_batch_number_.sequence_number_.bit_offset_
             : c.seq_packets_per_batch_.sequence_number_.bit_offset_;
}
static inline uint8_t rdr_seq_width(const ReorderConfig& c) {
  return c.method_ == ReorderMethod::SEQ_BATCH_NUMBER
             ? c.seq_batch_number_.sequence_number_.bit_width_
             : c.seq_packets_per_batch_.sequence_number_.bit_width_;
}
static inline uint16_t rdr_batch_off(const ReorderConfig& c) {
  return c.method_ == ReorderMethod::SEQ_BATCH_NUMBER
             ? c.seq_batch_number_.batch_number_.bit_offset_
             : 0U;
}
static inline uint8_t rdr_batch_width(const ReorderConfig& c) {
  return c.method_ == ReorderMethod::SEQ_BATCH_NUMBER ? c.seq_batch_number_.batch_number_.bit_width_
                                                      : 0U;
}
static inline bool rdr_uses_conversion(const ReorderConfig& c) {
  if (!c.data_types_.enabled_) {
    return false;
  }
  if (c.data_types_.input_type_ != c.data_types_.output_type_) {
    return true;
  }
  return c.data_types_.input_endianness_ == ReorderEndianness::NETWORK &&
         reorder_data_type_bit_width(c.data_types_.input_type_) > 8 &&
         (reorder_data_type_bit_width(c.data_types_.input_type_) % 8) == 0;
}
// output payload bytes for a given input payload size under the configured
// (optional) data-type conversion. Returns 0 on invalid sizing.
static inline uint32_t rdr_output_payload_len(const ReorderConfig& c, uint32_t in_len) {
  if (!rdr_uses_conversion(c)) {
    return in_len;
  }
  const uint32_t ib = reorder_data_type_bit_width(c.data_types_.input_type_);
  const uint32_t ob = reorder_data_type_bit_width(c.data_types_.output_type_);
  if (ib == 0 || ob == 0) {
    return 0;
  }
  const uint64_t in_bits = static_cast<uint64_t>(in_len) * 8ULL;
  const uint64_t elems = in_bits / ib;
  const uint64_t out_bits = elems * ob;
  if (out_bits % 8ULL) {
    return 0;
  }
  return static_cast<uint32_t>(out_bits / 8ULL);
}

// ---------------------------------------------------------------------------
// Burst metadata block layout. One rte_mempool element holds a BurstParams
// followed by inline arrays sized to the largest configured batch. The free
// paths recover the arrays from a BurstParams* via the fixed offsets below.
// ---------------------------------------------------------------------------
namespace {
struct IbvBurstLayout {
  size_t off_pkts0;
  size_t off_lens0;
  size_t off_pkts1;
  size_t off_lens1;
  size_t off_wqe;      // uint16_t[max_batch] -- region per packet
  size_t off_strd;     // uint16_t[max_batch] -- strides per packet
  size_t off_ts;       // uint64_t[max_batch] -- raw HW RX timestamp per packet
  size_t off_flowtag;  // uint32_t[max_batch] -- per-packet flow tag (MARK)
  size_t total;
};

IbvBurstLayout compute_layout(uint32_t max_batch) {
  IbvBurstLayout l{};
  size_t off = sizeof(BurstParams);
  auto place = [&off](size_t bytes) {
    off = (off + 15) & ~static_cast<size_t>(15);  // 16-byte align
    size_t at = off;
    off += bytes;
    return at;
  };
  l.off_pkts0 = place(sizeof(void*) * max_batch);
  l.off_lens0 = place(sizeof(uint32_t) * max_batch);
  l.off_pkts1 = place(sizeof(void*) * max_batch);
  l.off_lens1 = place(sizeof(uint32_t) * max_batch);
  l.off_wqe = place(sizeof(uint16_t) * max_batch);
  l.off_strd = place(sizeof(uint16_t) * max_batch);
  l.off_ts = place(sizeof(uint64_t) * max_batch);
  l.off_flowtag = place(sizeof(uint32_t) * max_batch);
  l.total = (off + 63) & ~static_cast<size_t>(63);
  return l;
}

// Recovered once at setup; arrays are addressed relative to each block.
IbvBurstLayout g_layout{};

inline uint16_t* burst_wqe_arr(BurstParams* b) {
  return reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(b) + g_layout.off_wqe);
}
inline uint16_t* burst_strd_arr(BurstParams* b) {
  return reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(b) + g_layout.off_strd);
}
inline uint64_t* burst_ts_arr(BurstParams* b) {
  return reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(b) + g_layout.off_ts);
}
inline uint32_t* burst_flowtag_arr(BurstParams* b) {
  return reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(b) + g_layout.off_flowtag);
}
}  // namespace

IbverbsEngine::~IbverbsEngine() {
  shutdown();
}

// ---------------------------------------------------------------------------
// Bring-up
// ---------------------------------------------------------------------------
bool IbverbsEngine::set_config_and_initialize(const NetworkConfig& cfg) {
  if (initialized_) {
    DAQIRI_LOG_ERROR("ibverbs engine is already initialized; call shutdown() first");
    return false;
  }

  force_quit_.store(false, std::memory_order_relaxed);
  max_batch_ = 0;
  cfg_ = cfg;
  if (!reserve_static_flow_ids()) { return false; }
  initialize();
  return initialized_;
}

// Resolve a configured interface to an ibverbs device context. Matching order:
//   1) interface address_/name_ equal to an IB device name (e.g. "mlx5_0")
//   2) interface name_ is a netdev whose IB device is found under
//      /sys/class/net/<name>/device/infiniband/
//   3) first mlx5dv-capable device (with a warning)
struct ibv_context* IbverbsEngine::open_device_for_interface(const InterfaceConfig& intf) {
  int num = 0;
  struct ibv_device** list = ibv_get_device_list(&num);
  if (list == nullptr || num == 0) {
    DAQIRI_LOG_CRITICAL("No ibverbs devices found");
    return nullptr;
  }

  auto open_named = [&](const std::string& name) -> struct ibv_device* {
    for (int i = 0; i < num; i++) {
      if (name == ibv_get_device_name(list[i])) {
        return list[i];
      }
    }
    return nullptr;
  };

  // Resolve an IB device name from a sysfs ".../infiniband/" directory.
  auto ibdev_in = [&](const std::string& path) -> struct ibv_device* {
    if (DIR* d = opendir(path.c_str())) {
      struct ibv_device* found = nullptr;
      struct dirent* e;
      while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') {
          continue;
        }
        found = open_named(e->d_name);
        if (found) {
          break;
        }
      }
      closedir(d);
      return found;
    }
    return nullptr;
  };

  struct ibv_device* dev = nullptr;
  if (!intf.address_.empty()) {
    dev = open_named(intf.address_);
  }
  if (dev == nullptr && !intf.name_.empty()) {
    dev = open_named(intf.name_);
  }

  // address_ as a PCIe BDF (the raw-config convention) -> ibdev via sysfs.
  if (dev == nullptr && !intf.address_.empty()) {
    dev = ibdev_in("/sys/bus/pci/devices/" + intf.address_ + "/infiniband/");
  }

  // netdev -> ibdev via sysfs
  if (dev == nullptr && !intf.name_.empty()) {
    dev = ibdev_in("/sys/class/net/" + intf.name_ + "/device/infiniband/");
  }

  if (dev == nullptr) {
    DAQIRI_LOG_WARN("Could not match interface '{}'/'{}' to an IB device; using first device",
                    intf.name_, intf.address_);
    dev = list[0];
  }

  const std::string dev_name = ibv_get_device_name(dev);
  if (auto it = ctx_map_.find(dev_name); it != ctx_map_.end()) {
    ibv_free_device_list(list);
    return it->second;
  }

  if (!mlx5dv_is_supported(dev)) {
    DAQIRI_LOG_CRITICAL("Device {} is not mlx5dv-capable; striding RQ requires a Mellanox NIC",
                        dev_name);
    ibv_free_device_list(list);
    return nullptr;
  }

  // Open with DevX enabled so DevX objects (the striding RQ) share the context's
  // uid and can reference its PD/CQ.
  struct mlx5dv_context_attr dv_attr {};
  dv_attr.flags = MLX5DV_CONTEXT_FLAGS_DEVX;
  struct ibv_context* ctx = mlx5dv_open_device(dev, &dv_attr);
  ibv_free_device_list(list);
  if (ctx == nullptr) {
    DAQIRI_LOG_CRITICAL("mlx5dv_open_device(DEVX) failed for {}: {}", dev_name, strerror(errno));
    return nullptr;
  }

  struct ibv_pd* pd = ibv_alloc_pd(ctx);
  if (pd == nullptr) {
    DAQIRI_LOG_CRITICAL("ibv_alloc_pd failed for {}", dev_name);
    ibv_close_device(ctx);
    return nullptr;
  }

  ctx_map_[dev_name] = ctx;
  pd_map_[ctx] = pd;
  DAQIRI_LOG_INFO("Opened ibverbs device {} (ctx {}, pd {})", dev_name, (void*)ctx, (void*)pd);
  return ctx;
}

int IbverbsEngine::mr_access_to_ibv(uint32_t access) {
  (void)access;
  // RX strided buffers only need local write. (Relaxed ordering is omitted: on
  // some aarch64/Tegra setups it has caused receive-side local protection
  // errors.)
  return IBV_ACCESS_LOCAL_WRITE;
}

Status IbverbsEngine::register_mr(struct ibv_pd* pd, const std::string& mr_name, uint8_t** out_base,
                                  uint32_t* out_lkey) {
  const auto& mr = cfg_.mrs_[mr_name];
  void* base = ar_[mr_name].ptr_;
  if (base == nullptr) {
    DAQIRI_LOG_CRITICAL("MR {} was not allocated", mr_name);
    return Status::NULL_PTR;
  }
  const int access = mr_access_to_ibv(mr.access_);

  if (mr.kind_ == MemoryKind::DEVICE) {
    // GPUDirect: export the CUDA allocation as a dma-buf fd and register it so
    // the NIC DMAs packets straight to/from GPU memory.
    cudaSetDevice(mr.affinity_);
    const size_t page = sysconf(_SC_PAGESIZE);
    const auto va = reinterpret_cast<uintptr_t>(base);
    const uintptr_t aligned = va & ~(static_cast<uintptr_t>(page) - 1);
    const size_t offset = va - aligned;
    const size_t aligned_size = (mr.ttl_size_ + offset + page - 1) & ~(page - 1);
    int dmabuf_fd = -1;
    CUresult cres =
        cuMemGetHandleForAddressRange(&dmabuf_fd, static_cast<CUdeviceptr>(aligned), aligned_size,
                                      CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD, 0);
    if (cres != CUDA_SUCCESS) {
      const char* es = nullptr;
      cuGetErrorString(cres, &es);
      DAQIRI_LOG_CRITICAL("cuMemGetHandleForAddressRange failed for MR {}: {}", mr_name,
                          es ? es : "?");
      return Status::GENERIC_FAILURE;
    }
    struct ibv_mr* gmr = ibv_reg_dmabuf_mr(pd, offset, mr.ttl_size_, va, dmabuf_fd, access);
    const int reg_errno = errno;
    close(dmabuf_fd);
    if (gmr == nullptr) {
      DAQIRI_LOG_CRITICAL("ibv_reg_dmabuf_mr failed for MR {} ({} bytes): {}", mr_name,
                          mr.ttl_size_, strerror(reg_errno));
      return Status::NULL_PTR;
    }
    registered_mrs_.push_back(gmr);
    *out_base = static_cast<uint8_t*>(base);
    *out_lkey = gmr->lkey;
    DAQIRI_LOG_INFO("Registered GPU MR {} (dmabuf) base {} size {} lkey {}", mr_name, base,
                    mr.ttl_size_, *out_lkey);
    return Status::SUCCESS;
  }

  struct ibv_mr* ib_mr = ibv_reg_mr(pd, base, mr.ttl_size_, access);
  if (ib_mr == nullptr) {
    DAQIRI_LOG_CRITICAL("ibv_reg_mr failed for {} ({} bytes)", mr_name, mr.ttl_size_);
    return Status::NULL_PTR;
  }
  registered_mrs_.push_back(ib_mr);
  *out_base = static_cast<uint8_t*>(base);
  *out_lkey = ib_mr->lkey;
  DAQIRI_LOG_INFO("Registered MR {} base {} size {} lkey {}", mr_name, base, mr.ttl_size_,
                  *out_lkey);
  return Status::SUCCESS;
}

Status IbverbsEngine::register_rx_mr(IbvRxQueue& q) {
  // Register every MR the queue references and record it as a scatter region
  // (the regular/HDS RQ scatters one data segment per region, in order). The
  // striding path uses only region 0 via mr_base/lkey.
  q.regions.clear();
  for (const std::string& name : q.mr_names) {
    IbvRxQueue::RxRegion r;
    if (Status s = register_mr(q.pd, name, &r.base, &r.lkey); s != Status::SUCCESS) {
      return s;
    }
    const auto& mr = cfg_.mrs_[name];
    r.slot_size = static_cast<uint32_t>(mr.adj_size_);
    r.seg_len = r.slot_size;
    q.regions.push_back(r);
  }
  // Region 0 = CPU header in HDS mode: its scatter length is the split boundary,
  // so the first split_boundary bytes land in the (CPU) header MR and the rest
  // overflow into region 1 (the GPU payload MR).
  if (q.num_segs == 2 && q.split_boundary > 0 && q.regions.size() >= 2) {
    q.regions[0].seg_len = static_cast<uint32_t>(q.split_boundary);
  }
  q.mr_base = q.regions[0].base;
  q.lkey = q.regions[0].lkey;
  return Status::SUCCESS;
}

Status IbverbsEngine::create_striding_rq(IbvRxQueue& q) {
  if (!q.striding) {
    return create_regular_rq(q);
  }
  // Query striding-RQ capabilities and clamp the geometry.
  struct mlx5dv_context dv_attr {};
  dv_attr.comp_mask = MLX5DV_CONTEXT_MASK_STRIDING_RQ;
  if (mlx5dv_query_device(q.ctx, &dv_attr) != 0 ||
      !(dv_attr.comp_mask & MLX5DV_CONTEXT_MASK_STRIDING_RQ)) {
    DAQIRI_LOG_CRITICAL("Device does not report striding-RQ capabilities");
    return Status::NOT_SUPPORTED;
  }
  const auto& caps = dv_attr.striding_rq_caps;

  const auto& mr = cfg_.mrs_[q.mr_name];
  // Size the stride to the smallest power of two that holds the MR's buffer
  // size, starting from the device minimum (so small packets don't waste a
  // large stride). DEFAULT_STRIDE_LOG is only a floor when buf_size is tiny.
  uint32_t stride_log = caps.min_single_stride_log_num_of_bytes;
  while ((1u << stride_log) < mr.buf_size_) {
    ++stride_log;
  }
  stride_log = std::min(stride_log, caps.max_single_stride_log_num_of_bytes);

  uint32_t strides_log = DEFAULT_STRIDES_PER_WQE_LOG;
  strides_log = std::max(strides_log, caps.min_single_wqe_log_num_of_strides);
  strides_log = std::min(strides_log, caps.max_single_wqe_log_num_of_strides);

  q.stride_size = 1u << stride_log;
  q.strides_per_wqe = q.striding ? (1u << strides_log) : 1u;
  q.region_size = q.stride_size * q.strides_per_wqe;

  // The strided buffer must be aligned for the device's MPRQ DMA. rte_malloc
  // hands back only cache-aligned pointers, so align the region base up to the
  // page size (>= stride) and shrink the usable span accordingly. The MR is
  // still registered over the full allocation, so the aligned sub-range is
  // covered by the same lkey.
  const size_t page = sysconf(_SC_PAGESIZE);
  const size_t base_align = std::max<size_t>(page, q.stride_size);
  uint8_t* orig_base = q.mr_base;
  uint8_t* aligned_base = reinterpret_cast<uint8_t*>(
      (reinterpret_cast<uintptr_t>(orig_base) + base_align - 1) & ~(base_align - 1));
  const size_t lost = static_cast<size_t>(aligned_base - orig_base);
  const size_t usable = (mr.ttl_size_ > lost) ? (mr.ttl_size_ - lost) : 0;
  q.mr_base = aligned_base;

  // Round the region count DOWN to a power of two: the mlx5 RQ depth is a power
  // of two and the CQE wqe_counter is taken modulo it, so `wqe_counter &
  // (num_wqe - 1)` only recovers the region index when num_wqe is a power of 2.
  uint32_t fit = static_cast<uint32_t>(usable / q.region_size);
  // Cap the per-packet buffer count in non-striding mode so the RQ depth stays
  // within device limits.
  if (!q.striding && fit > 4096) {
    fit = 4096;
  }
  q.num_wqe = fit ? (1u << log2_floor(fit)) : 0;
  if (q.num_wqe < MIN_NUM_WQE) {
    DAQIRI_LOG_CRITICAL(
        "MR {} too small for striding RQ: {} bytes / region {} = {} WQEs (need >= {}). Increase "
        "num_bufs/buf_size.",
        q.mr_name, mr.ttl_size_, q.region_size, q.num_wqe, MIN_NUM_WQE);
    return Status::INVALID_PARAMETER;
  }
  q.consumed_strides.assign(q.num_wqe, 0);
  q.released_strides.assign(q.num_wqe, 0);
  q.freed_strides = std::make_unique<std::atomic<uint32_t>[]>(q.num_wqe);
  for (uint32_t i = 0; i < q.num_wqe; i++) {
    q.freed_strides[i].store(0);
  }

  DAQIRI_LOG_INFO(
      "Striding RQ q{}: stride={}B strides/WQE={} region={}B num_wqe={} (two_byte_shift={})",
      q.queue_id, q.stride_size, q.strides_per_wqe, q.region_size, q.num_wqe, q.two_byte_shift);

  DAQIRI_LOG_INFO(
      "striding_rq_caps q{}: stride_log[{},{}] strides_log[{},{}] (using stride_log={} "
      "strides_log={})",
      q.queue_id, caps.min_single_stride_log_num_of_bytes, caps.max_single_stride_log_num_of_bytes,
      caps.min_single_wqe_log_num_of_strides, caps.max_single_wqe_log_num_of_strides, stride_log,
      strides_log);

  q.cq_ci = 0;
  // Striding WQE = mlx5_wqe_mprq (16 B next-seg + 16 B data-seg).
  q.wqe_stride = 32;
  q.wqe_dseg_off = 16;

  // Real MPRQ uses a DevX-built CQ + striding RQ + TIR + mlx5dv_dr steering. The
  // DevX RQ can only reference a DevX-created CQ (a verbs CQ is rejected).
  if (Status s = create_striding_rq_devx(q); s != Status::SUCCESS) {
    return s;
  }
  DAQIRI_LOG_INFO("DevX striding RQ ready: rqn {} (q{})", q.rqn, q.queue_id);
  return Status::SUCCESS;
}

// Non-striding DevX RQ for physical HDS / multi-region: one WQE per packet, each
// WQE a scatter list of one data segment per region (region 0 = CPU header in
// HDS, region 1 = GPU payload). Reuses the DevX CQ/TIR/dr-steering/doorbell of
// the striding path; only the RQ wq_type, WQE format, and buffer model differ.
Status IbverbsEngine::create_regular_rq(IbvRxQueue& q) {
  const uint32_t num_segs = static_cast<uint32_t>(q.regions.size());
  // num_wqe (packet slots) = smallest region's buffer count, power-of-two so the
  // CQE wqe_counter recovers the slot via `& (num_wqe - 1)`.
  uint64_t fit = UINT64_MAX;
  for (const std::string& name : q.mr_names) {
    fit = std::min<uint64_t>(fit, cfg_.mrs_[name].num_bufs_);
  }
  // The regular RQ holds one packet per WQE (unlike MPRQ's hundreds of strides
  // per WQE), so deep buffering needs many WQEs -- allow up to 64K so the RQ
  // doesn't starve and PAUSE the link while the free->refill pipeline catches up.
  if (fit > 32768) {
    fit = 32768;
  }
  q.num_wqe = fit ? (1u << log2_floor(static_cast<uint32_t>(fit))) : 0;
  if (q.num_wqe < MIN_NUM_WQE) {
    DAQIRI_LOG_CRITICAL("Regular RQ q{}: too few buffers ({} < {})", q.queue_id, q.num_wqe,
                        MIN_NUM_WQE);
    return Status::INVALID_PARAMETER;
  }
  q.strides_per_wqe = 1;  // one packet per WQE; reuses the stride-free reclaim
  q.consumed_strides.assign(q.num_wqe, 0);
  q.released_strides.assign(q.num_wqe, 0);
  q.freed_strides = std::make_unique<std::atomic<uint32_t>[]>(q.num_wqe);
  for (uint32_t i = 0; i < q.num_wqe; i++) {
    q.freed_strides[i].store(0);
  }

  // WQE = num_segs data segments at offset 0; stride rounded up to a power of 2.
  uint32_t wsz = num_segs * 16u;
  uint32_t stride = 16;
  while (stride < wsz) {
    stride <<= 1;
  }
  q.wqe_stride = stride;
  q.wqe_dseg_off = 0;
  q.cq_ci = 0;

  std::string seglog;
  for (const auto& r : q.regions) {
    seglog +=
        " [seg_len=" + std::to_string(r.seg_len) + " slot=" + std::to_string(r.slot_size) + "]";
  }
  DAQIRI_LOG_INFO("Regular RQ q{}: num_segs={} num_wqe={} wqe_stride={}B split={}{}", q.queue_id,
                  num_segs, q.num_wqe, q.wqe_stride, q.split_boundary, seglog);

  if (Status s = create_striding_rq_devx(q); s != Status::SUCCESS) {
    return s;
  }
  DAQIRI_LOG_INFO("DevX regular RQ ready: rqn {} (q{})", q.rqn, q.queue_id);
  return Status::SUCCESS;
}

Status IbverbsEngine::repost_wqe(IbvRxQueue& q, uint32_t wqe_idx) {
  struct ibv_sge sge {};
  sge.addr = reinterpret_cast<uint64_t>(q.mr_base + static_cast<size_t>(wqe_idx) * q.region_size);
  sge.length = q.region_size;  // full per-WQE buffer (num_strides * stride_size)
  sge.lkey = q.lkey;

  struct ibv_recv_wr wr {};
  wr.wr_id = wqe_idx;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  struct ibv_recv_wr* bad = nullptr;
  if (ibv_post_wq_recv(q.wq, &wr, &bad) != 0) {
    DAQIRI_LOG_CRITICAL("ibv_post_wq_recv failed for region {}", wqe_idx);
    return Status::GENERIC_FAILURE;
  }
  q.consumed_strides[wqe_idx] = 0;
  q.released_strides[wqe_idx] = 0;
  q.reposts.fetch_add(1, std::memory_order_relaxed);
  return Status::SUCCESS;
}

Status IbverbsEngine::post_all_wqes(IbvRxQueue& q) {
  for (uint32_t i = 0; i < q.num_wqe; i++) {
    if (repost_wqe(q, i) != Status::SUCCESS) {
      return Status::GENERIC_FAILURE;
    }
  }
  q.cur_wqe = 0;
  q.cur_consumed = 0;
  return Status::SUCCESS;
}

// ---------------------------------------------------------------------------
// DevX striding-RQ (real MPRQ) path
// ---------------------------------------------------------------------------
// The striding-RQ WQE is a `struct mlx5_wqe_mprq` = a 16-byte next-segment
// followed by a 16-byte data segment (32 bytes total, log_wq_stride = 5). Only
// the data segment is filled; the next segment stays zero for a cyclic RQ.
static constexpr size_t MPRQ_WQE_SIZE = 32;
static constexpr size_t MPRQ_WQE_DSEG_OFFSET = 16;
static constexpr uint32_t MPRQ_LOG_WQ_STRIDE = 5;  // log2(MPRQ_WQE_SIZE)

void IbverbsEngine::devx_build_wqe(IbvRxQueue& q, uint32_t slot) {
  uint8_t* base = static_cast<uint8_t*>(q.wq_buf) + static_cast<size_t>(slot) * q.wqe_stride;
  if (q.striding) {
    // MPRQ: a single scatter entry (after the next-seg) covering the whole
    // region; the HW splits arriving packets into strides within it.
    auto* seg = reinterpret_cast<struct mlx5_wqe_data_seg_min*>(base + q.wqe_dseg_off);
    seg->byte_count = htobe32(q.region_size);
    seg->lkey = htobe32(q.lkey);
    seg->addr = htobe64(reinterpret_cast<uint64_t>(q.mr_base) +
                        static_cast<uint64_t>(slot) * q.region_size);
    return;
  }
  // Regular RQ: one data segment per region. The NIC fills segment 0 to its
  // length, then overflows into the next -- so a header-length seg 0 (CPU MR)
  // plus a payload seg 1 (GPU MR) yields a physical header/data split.
  auto* segs = reinterpret_cast<struct mlx5_wqe_data_seg_min*>(base);
  for (size_t r = 0; r < q.regions.size(); ++r) {
    const auto& reg = q.regions[r];
    segs[r].byte_count = htobe32(reg.seg_len);
    segs[r].lkey = htobe32(reg.lkey);
    segs[r].addr =
        htobe64(reinterpret_cast<uint64_t>(reg.base) + static_cast<uint64_t>(slot) * reg.slot_size);
  }
}

void IbverbsEngine::devx_ring_rq_doorbell(IbvRxQueue& q) {
  doorbell_store_barrier();
  q.rq_dbr[0] = htobe32(q.rq_pi & 0xffff);  // MLX5_RCV_DBR = index 0
}

void IbverbsEngine::devx_advance_producer(IbvRxQueue& q) {
  // Worker-only. Refill regions that are fully freed, in cyclic producer order.
  uint32_t slot = q.rq_pi % q.num_wqe;
  bool advanced = false;
  while (q.freed_strides[slot].load(std::memory_order_acquire) >= q.strides_per_wqe) {
    q.freed_strides[slot].store(0, std::memory_order_relaxed);
    devx_build_wqe(q, slot);  // content identical; re-arms the slot
    q.rq_pi++;
    q.reposts.fetch_add(1, std::memory_order_relaxed);
    advanced = true;
    slot = q.rq_pi % q.num_wqe;
  }
  if (advanced) {
    devx_ring_rq_doorbell(q);
  }
}

Status IbverbsEngine::devx_create_rq(IbvRxQueue& q, uint32_t stride_log, uint32_t strides_log) {
  // PD number.
  struct mlx5dv_pd dvpd {};
  struct mlx5dv_obj pdobj {};
  pdobj.pd.in = q.pd;
  pdobj.pd.out = &dvpd;
  if (mlx5dv_init_obj(&pdobj, MLX5DV_OBJ_PD) != 0) {
    DAQIRI_LOG_CRITICAL("mlx5dv_init_obj(PD) failed");
    return Status::GENERIC_FAILURE;
  }

  // UAR was allocated by create_striding_rq_devx (shared with the CQ).
  const size_t page = sysconf(_SC_PAGESIZE);
  // Single umem holding the RQ WQE ring (one 32-byte mlx5_wqe_mprq per WQE)
  // followed by the doorbell record, matching DPDK's mlx5_common_devx layout.
  constexpr size_t DBR_SIZE = 64;
  const size_t wq_bytes = static_cast<size_t>(q.num_wqe) * q.wqe_stride;
  const size_t dbr_off = (wq_bytes + DBR_SIZE - 1) & ~(DBR_SIZE - 1);
  const size_t umem_bytes = (dbr_off + DBR_SIZE + page - 1) & ~(page - 1);
  if (posix_memalign(&q.wq_buf, page, umem_bytes) != 0) {
    DAQIRI_LOG_CRITICAL("posix_memalign(umem) failed");
    return Status::GENERIC_FAILURE;
  }
  memset(q.wq_buf, 0, umem_bytes);
  q.rq_dbr = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(q.wq_buf) + dbr_off);
  q.wq_umem = mlx5dv_devx_umem_reg(q.ctx, q.wq_buf, umem_bytes, 0x7 /*rw*/);
  if (q.wq_umem == nullptr) {
    DAQIRI_LOG_CRITICAL("mlx5dv_devx_umem_reg failed: {}", strerror(errno));
    return Status::GENERIC_FAILURE;
  }
  q.dbr_umem = nullptr;  // shared with wq_umem

  uint32_t in[DEVX_ST_SZ_DW(create_rq_in)] = {0};
  uint32_t out[DEVX_ST_SZ_DW(create_rq_out)] = {0};
  void* rqc = DEVX_ADDR_OF(create_rq_in, in, ctx);
  void* wq = DEVX_ADDR_OF(rqc, rqc, wq);

  DEVX_SET(create_rq_in, in, opcode, MLX5_CMD_OP_CREATE_RQ);
  DEVX_SET(rqc, rqc, state, MLX5_RQC_STATE_RST);
  DEVX_SET(rqc, rqc, cqn, q.dv_cq.cqn);
  DEVX_SET(rqc, rqc, flush_in_error_en, 1);
  DEVX_SET(rqc, rqc, vsd, 1);  // do not strip VLAN

  DEVX_SET(wq, wq, wq_type, q.striding ? MLX5_WQ_TYPE_CYCLIC_STRIDING_RQ : MLX5_WQ_TYPE_CYCLIC);
  DEVX_SET(wq, wq, log_wq_stride, log2_floor(q.wqe_stride));
  DEVX_SET(wq, wq, log_wq_sz, log2_floor(q.num_wqe));
  DEVX_SET(wq, wq, pd, dvpd.pdn);
  // No uar_page: an RQ rings its doorbell via the memory dbr record, not a UAR.
  DEVX_SET(wq, wq, log_wq_pg_sz, log2_floor(static_cast<uint32_t>(page)) - 12u);
  DEVX_SET64(wq, wq, dbr_addr, dbr_off);
  DEVX_SET(wq, wq, dbr_umem_id, q.wq_umem->umem_id);
  DEVX_SET(wq, wq, wq_umem_id, q.wq_umem->umem_id);
  DEVX_SET64(wq, wq, wq_umem_offset, 0);
  DEVX_SET(wq, wq, dbr_umem_valid, 1);
  DEVX_SET(wq, wq, wq_umem_valid, 1);
  // Striding-RQ-only geometry.
  if (q.striding) {
    DEVX_SET(wq, wq, single_wqe_log_num_of_strides,
             strides_log - MLX5_MIN_SINGLE_WQE_LOG_NUM_STRIDES);
    DEVX_SET(wq, wq, two_byte_shift_en, q.two_byte_shift);
    DEVX_SET(wq, wq, single_stride_log_num_of_bytes,
             stride_log - MLX5_MIN_SINGLE_STRIDE_LOG_NUM_BYTES);
  }

  q.rq_obj = mlx5dv_devx_obj_create(q.ctx, in, sizeof(in), out, sizeof(out));
  if (q.rq_obj == nullptr) {
    DAQIRI_LOG_CRITICAL("CREATE_RQ (DevX) failed: {} (syndrome 0x{:x})", strerror(errno),
                        DEVX_GET(create_rq_out, out, syndrome));
    return Status::GENERIC_FAILURE;
  }
  q.rqn = DEVX_GET(create_rq_out, out, rqn);

  // Arm all WQEs, then advance to RDY and ring the doorbell.
  for (uint32_t i = 0; i < q.num_wqe; i++) {
    devx_build_wqe(q, i);
  }
  q.rq_pi = q.num_wqe;

  uint32_t min[DEVX_ST_SZ_DW(modify_rq_in)] = {0};
  uint32_t mout[DEVX_ST_SZ_DW(modify_rq_out)] = {0};
  void* mrqc = DEVX_ADDR_OF(modify_rq_in, min, ctx);
  DEVX_SET(modify_rq_in, min, opcode, MLX5_CMD_OP_MODIFY_RQ);
  DEVX_SET(modify_rq_in, min, rqn, q.rqn);
  DEVX_SET(modify_rq_in, min, rq_state, MLX5_RQC_STATE_RST);
  DEVX_SET(rqc, mrqc, state, MLX5_RQC_STATE_RDY);
  if (mlx5dv_devx_obj_modify(q.rq_obj, min, sizeof(min), mout, sizeof(mout)) != 0) {
    DAQIRI_LOG_CRITICAL("MODIFY_RQ rst2rdy failed: {} (syndrome 0x{:x})", strerror(errno),
                        DEVX_GET(modify_rq_out, mout, syndrome));
    return Status::GENERIC_FAILURE;
  }
  devx_ring_rq_doorbell(q);
  return Status::SUCCESS;
}

Status IbverbsEngine::devx_create_tir(IbvRxQueue& q) {
  // Transport domain for the TIR.
  uint32_t tdin[DEVX_ST_SZ_DW(alloc_transport_domain_in)] = {0};
  uint32_t tdout[DEVX_ST_SZ_DW(alloc_transport_domain_out)] = {0};
  DEVX_SET(alloc_transport_domain_in, tdin, opcode, MLX5_CMD_OP_ALLOC_TRANSPORT_DOMAIN);
  q.td_obj = mlx5dv_devx_obj_create(q.ctx, tdin, sizeof(tdin), tdout, sizeof(tdout));
  if (q.td_obj == nullptr) {
    DAQIRI_LOG_CRITICAL("ALLOC_TRANSPORT_DOMAIN failed: {}", strerror(errno));
    return Status::GENERIC_FAILURE;
  }
  q.td_num = DEVX_GET(alloc_transport_domain_out, tdout, transport_domain);

  uint32_t in[DEVX_ST_SZ_DW(create_tir_in)] = {0};
  uint32_t out[DEVX_ST_SZ_DW(create_tir_out)] = {0};
  void* tirc = DEVX_ADDR_OF(create_tir_in, in, ctx);
  DEVX_SET(create_tir_in, in, opcode, MLX5_CMD_OP_CREATE_TIR);
  DEVX_SET(tirc, tirc, disp_type, MLX5_TIRC_DISP_TYPE_DIRECT);
  DEVX_SET(tirc, tirc, inline_rqn, q.rqn);
  DEVX_SET(tirc, tirc, rx_hash_fn, MLX5_RX_HASH_FN_NONE);
  DEVX_SET(tirc, tirc, transport_domain, q.td_num);
  q.tir_obj = mlx5dv_devx_obj_create(q.ctx, in, sizeof(in), out, sizeof(out));
  if (q.tir_obj == nullptr) {
    DAQIRI_LOG_CRITICAL("CREATE_TIR failed: {} (syndrome 0x{:x})", strerror(errno),
                        DEVX_GET(create_tir_out, out, syndrome));
    return Status::GENERIC_FAILURE;
  }
  // Reusable steer-to-this-queue action; flow rules (per-port) reference it.
  q.dr_action = mlx5dv_dr_action_create_dest_devx_tir(q.tir_obj);
  if (q.dr_action == nullptr) {
    DAQIRI_LOG_CRITICAL("dr_action_create_dest_devx_tir failed (q{}): {}", q.queue_id,
                        strerror(errno));
    return Status::GENERIC_FAILURE;
  }
  return Status::SUCCESS;
}

// mlx5dv_dr match buffer: 64 bytes interpreted as fte_match_set_lyr_2_4 when
// criteria_enable selects outer_headers.
namespace {
struct DrMatchBuf {
  size_t match_sz;
  uint64_t buf[8];  // == sizeof(fte_match_set_lyr_2_4)
};
// Full match parameter (512 B) for matches that reach misc_parameters_4 (the
// flex-parser sample registers), which sits past the first 64-byte section.
struct DrMatchParam {
  size_t match_sz;
  uint64_t buf[64];  // == sizeof(fte_match_param)
};
}  // namespace

const InterfaceConfig* IbverbsEngine::find_interface_config(int port) const {
  for (const auto& intf : cfg_.ifs_) {
    if (intf.port_id_ == port) { return &intf; }
  }
  return nullptr;
}

bool IbverbsEngine::reserve_static_flow_ids() {
  static_flow_ids_.clear();
  while (!free_dynamic_flow_ids_.empty()) { free_dynamic_flow_ids_.pop(); }
  for (const auto& intf : cfg_.ifs_) {
    for (const auto& flow : intf.rx_.flows_) {
      if (flow.id_ == 0) { continue; }
      if (!static_flow_ids_.insert(flow.id_).second) {
        DAQIRI_LOG_ERROR("Duplicate static flow ID {}", flow.id_);
        return false;
      }
    }
  }
  next_dynamic_flow_id_ = 1;
  return true;
}

FlowOpId IbverbsEngine::allocate_flow_op_id_locked() {
  if (next_flow_op_id_ == 0) { next_flow_op_id_ = 1; }
  return next_flow_op_id_++;
}

bool IbverbsEngine::has_dynamic_flow_id_capacity_locked(size_t count) const {
  if (free_dynamic_flow_ids_.size() >= count) { return true; }
  count -= free_dynamic_flow_ids_.size();

  FlowId candidate = next_dynamic_flow_id_;
  while (count > 0 && candidate != 0 && candidate <= kMaxIbverbsFlowTag) {
    const FlowId flow_id = candidate++;
    if (flow_id == 0) { continue; }
    if (static_flow_ids_.find(flow_id) != static_flow_ids_.end()) { continue; }
    if (dynamic_flows_.find(flow_id) != dynamic_flows_.end()) { continue; }
    --count;
  }
  return count == 0;
}

FlowId IbverbsEngine::allocate_dynamic_flow_id_locked() {
  while (!free_dynamic_flow_ids_.empty()) {
    const FlowId candidate = free_dynamic_flow_ids_.front();
    free_dynamic_flow_ids_.pop();
    if (candidate == 0 || candidate > kMaxIbverbsFlowTag) { continue; }
    if (static_flow_ids_.find(candidate) != static_flow_ids_.end()) { continue; }
    if (dynamic_flows_.find(candidate) != dynamic_flows_.end()) { continue; }
    return candidate;
  }

  while (next_dynamic_flow_id_ != 0 && next_dynamic_flow_id_ <= kMaxIbverbsFlowTag) {
    const FlowId candidate = next_dynamic_flow_id_++;
    if (candidate == 0) { continue; }
    if (static_flow_ids_.find(candidate) != static_flow_ids_.end()) { continue; }
    if (dynamic_flows_.find(candidate) != dynamic_flows_.end()) { continue; }
    return candidate;
  }
  return 0;
}

void IbverbsEngine::release_dynamic_flow_id_locked(FlowId flow_id) {
  if (flow_id == 0 || flow_id > kMaxIbverbsFlowTag) { return; }
  if (static_flow_ids_.find(flow_id) != static_flow_ids_.end()) { return; }
  if (dynamic_flows_.find(flow_id) != dynamic_flows_.end()) { return; }
  free_dynamic_flow_ids_.push(flow_id);
}

bool IbverbsEngine::validate_dynamic_rx_flow_locked(int port, const FlowRuleConfig& flow) const {
  if (!initialized_) {
    DAQIRI_LOG_ERROR("Cannot add dynamic RX flow before DAQIRI initialization");
    return false;
  }

  const InterfaceConfig* intf = find_interface_config(port);
  if (intf == nullptr) {
    DAQIRI_LOG_ERROR("Invalid dynamic RX flow port {}", port);
    return false;
  }

  const auto actions = flow_rule_actions(flow);
  if (actions.empty() || actions.back().type_ != FlowType::QUEUE) {
    DAQIRI_LOG_ERROR("Dynamic RX flow must end with a queue action");
    return false;
  }
  if (actions.size() > 7) {
    DAQIRI_LOG_ERROR("Dynamic RX flow '{}' has too many actions", flow.name_);
    return false;
  }
  const FlowAction queue_action = flow_queue_action(actions);

  const auto queue_it = std::find_if(intf->rx_.queues_.begin(), intf->rx_.queues_.end(),
                                     [&](const RxQueueConfig& q) {
                                       return q.common_.id_ == queue_action.id_;
                                     });
  if (queue_it == intf->rx_.queues_.end()) {
    DAQIRI_LOG_ERROR("Dynamic RX flow targets invalid port/queue {}/{}", port,
                     queue_action.id_);
    return false;
  }
  const bool has_transform = flow_actions_have_transform(actions);
  for (const auto& action : actions) {
    if (action.type_ == FlowType::VLAN_PUSH || action.type_ == FlowType::TUNNEL_ENCAP) {
      DAQIRI_LOG_ERROR("Dynamic RX flow '{}' can only use decap/pop transform actions",
                       flow.name_);
      return false;
    }
  }

  const FlowMatch& match = flow.match_;
  if (match.type_ == FlowMatchType::FLEX_ITEM) {
    if (has_transform) {
      DAQIRI_LOG_ERROR("Dynamic RX flow '{}' cannot combine flex-item matching with tunnel/VLAN "
                       "actions",
                       flow.name_);
      return false;
    }
    const uint16_t flex_item_id = match.flex_item_match_.flex_item_id_;
    const auto flex_it = std::find_if(intf->rx_.flex_items_.begin(), intf->rx_.flex_items_.end(),
                                      [&](const FlexItemConfig& c) {
                                        return c.id_ == flex_item_id;
                                      });
    if (flex_it == intf->rx_.flex_items_.end()) {
      DAQIRI_LOG_ERROR("Dynamic RX flow references invalid flex item ID {}", flex_item_id);
      return false;
    }
    return true;
  }

  if (match.type_ == FlowMatchType::ECPRI) {
    if (match.ecpri_match_.match_id_ && !match.ecpri_match_.match_msg_type_) {
      DAQIRI_LOG_ERROR("Dynamic eCPRI RX flow matches pc_id/rtc_id but no msg_type");
      return false;
    }
    return true;
  }

  if (match.type_ == FlowMatchType::IPV4_UDP &&
      (match.udp_src_ > 0 || match.udp_dst_ > 0 || match.ipv4_src_ != INADDR_ANY ||
       match.ipv4_dst_ != INADDR_ANY || match.ipv4_len_ > 0)) {
    return true;
  }

  DAQIRI_LOG_ERROR("Dynamic RX flow must define an IPv4/UDP, flex-item or eCPRI match");
  return false;
}

bool IbverbsEngine::create_dr_rule_locked(int port,
                                          PortSteering& st,
                                          uint16_t criteria,
                                          struct mlx5dv_flow_match_parameters* mask,
                                          struct mlx5dv_flow_match_parameters* value,
                                          IbvRxQueue* dest,
                                          int priority,
                                          FlowId flow_id,
                                          const char* desc,
                                          DynamicFlowEntry* dynamic_entry,
                                          const std::vector<struct mlx5dv_dr_action*>& reformats) {
  struct mlx5dv_dr_matcher* matcher = mlx5dv_dr_matcher_create(st.table, priority, criteria, mask);
  if (matcher == nullptr) {
    DAQIRI_LOG_CRITICAL("dr_matcher_create failed (port {} {}): {}", port, desc, strerror(errno));
    return false;
  }

  struct mlx5dv_dr_action* tag_action = nullptr;
  struct mlx5dv_dr_action* actions[8];
  int num_actions = 0;
  if (flow_id != 0) {
    tag_action = mlx5dv_dr_action_create_tag(flow_id);
    if (tag_action == nullptr) {
      DAQIRI_LOG_CRITICAL("dr_action_create_tag({}) failed (port {} {}): {}", flow_id, port, desc,
                          strerror(errno));
      mlx5dv_dr_matcher_destroy(matcher);
      return false;
    }
    actions[num_actions++] = tag_action;
  }
  for (auto* reformat : reformats) {
    if (num_actions >= static_cast<int>(sizeof(actions) / sizeof(actions[0])) - 1) {
      DAQIRI_LOG_CRITICAL("Too many reformat actions for flow '{}' on port {}", desc, port);
      if (tag_action != nullptr) { mlx5dv_dr_action_destroy(tag_action); }
      mlx5dv_dr_matcher_destroy(matcher);
      return false;
    }
    actions[num_actions++] = reformat;
  }
  actions[num_actions++] = dest->dr_action;

  struct mlx5dv_dr_rule* rule = mlx5dv_dr_rule_create(matcher, value, num_actions, actions);
  if (rule == nullptr) {
    DAQIRI_LOG_CRITICAL("dr_rule_create failed (port {} {}): {}", port, desc, strerror(errno));
    if (tag_action != nullptr) { mlx5dv_dr_action_destroy(tag_action); }
    mlx5dv_dr_matcher_destroy(matcher);
    return false;
  }

  if (dynamic_entry != nullptr) {
    dynamic_entry->port = port;
    dynamic_entry->queue = static_cast<uint16_t>(dest->queue_id);
    dynamic_entry->matcher = matcher;
    dynamic_entry->rule = rule;
    dynamic_entry->tag_action = tag_action;
    dynamic_entry->state = DynamicFlowState::ACTIVE;
    return true;
  }

  st.matchers.push_back(matcher);
  st.rules.push_back(rule);
  if (tag_action != nullptr) { st.tag_actions.push_back(tag_action); }

  PortSteering::RuleSpec spec{matcher, dest->dr_action, tag_action, reformats, 0, {}};
  spec.value_sz = std::min(value->match_sz, sizeof(spec.value));
  memcpy(spec.value, value->match_buf, spec.value_sz);
  st.rule_specs.push_back(spec);
  return true;
}

Status IbverbsEngine::install_flow_rule_locked(int port,
                                               PortSteering& st,
                                               const InterfaceConfig& intf,
                                               const FlowRuleConfig& flow,
                                               FlowId flow_id,
                                               int priority,
                                               DynamicFlowEntry* dynamic_entry) {
  if (st.dropped) {
    DAQIRI_LOG_ERROR("Cannot modify dynamic RX flows while port {} is dropped", port);
    return Status::INVALID_PARAMETER;
  }
  if (st.table == nullptr) { return Status::NOT_READY; }

  const auto actions = flow_rule_actions(flow);
  const FlowAction queue_action = flow_queue_action(actions);
  IbvRxQueue* dest = find_rx_queue(port, queue_action.id_);
  if (dest == nullptr || dest->dr_action == nullptr) {
    DAQIRI_LOG_ERROR("Flow '{}' targets unknown queue id {} on port {}", flow.name_, queue_action.id_,
                     port);
    return Status::INVALID_PARAMETER;
  }

  std::vector<struct mlx5dv_dr_action*> flow_reformats;
  auto cleanup_dynamic_reformats = [&]() {
    if (dynamic_entry == nullptr) { return; }
    for (auto* reformat : dynamic_entry->reformat_actions) {
      if (reformat != nullptr) { mlx5dv_dr_action_destroy(reformat); }
    }
    dynamic_entry->reformat_actions.clear();
    dynamic_entry->reformat_buffers.clear();
  };
  for (const auto& action : actions) {
    if (!flow_action_is_transform(action)) { continue; }
    std::vector<uint8_t>* buffer = nullptr;
    if (dynamic_entry != nullptr) {
      dynamic_entry->reformat_buffers.emplace_back();
      buffer = &dynamic_entry->reformat_buffers.back();
    } else {
      st.reformat_buffers.emplace_back();
      buffer = &st.reformat_buffers.back();
    }
    enum mlx5dv_flow_action_packet_reformat_type reformat_type {};
    if (!build_reformat_buffer(action, *buffer, &reformat_type)) {
      DAQIRI_LOG_CRITICAL("Flow '{}' failed to build ibverbs reformat buffer", flow.name_);
      cleanup_dynamic_reformats();
      return Status::GENERIC_FAILURE;
    }
    struct mlx5dv_dr_action* reformat = mlx5dv_dr_action_create_packet_reformat(
        st.domain, 0, reformat_type, buffer->size(), buffer->data());
    if (reformat == nullptr) {
      DAQIRI_LOG_CRITICAL("Flow '{}' failed to create ibverbs packet reformat action: {}",
                          flow.name_, strerror(errno));
      cleanup_dynamic_reformats();
      return Status::GENERIC_FAILURE;
    }
    if (dynamic_entry != nullptr) {
      dynamic_entry->reformat_actions.push_back(reformat);
    } else {
      st.reformat_actions.push_back(reformat);
    }
    flow_reformats.push_back(reformat);
  }

  const FlowMatch& mt = flow.match_;
  if (mt.type_ == FlowMatchType::FLEX_ITEM) {
    const uint16_t flex_item_id = mt.flex_item_match_.flex_item_id_;
    auto node_it = st.flex_nodes.find(flex_item_id);
    auto cfg_it = std::find_if(intf.rx_.flex_items_.begin(), intf.rx_.flex_items_.end(),
                               [&](const FlexItemConfig& c) { return c.id_ == flex_item_id; });
    if (node_it == st.flex_nodes.end() || cfg_it == intf.rx_.flex_items_.end()) {
      DAQIRI_LOG_ERROR("Flow '{}' references unknown flex item id {}", flow.name_, flex_item_id);
      cleanup_dynamic_reformats();
      return Status::INVALID_PARAMETER;
    }

    DrMatchParam mask{}, value{};
    mask.match_sz = sizeof(mask.buf);
    value.match_sz = sizeof(value.buf);
    auto* mask_buf = reinterpret_cast<uint8_t*>(mask.buf);
    auto* value_buf = reinterpret_cast<uint8_t*>(value.buf);
    DEVX_SET(fte_match_set_lyr_2_4, mask_buf, ethertype, 0xffff);
    DEVX_SET(fte_match_set_lyr_2_4, value_buf, ethertype, MLX5_ETHERTYPE_IPV4);
    DEVX_SET(fte_match_set_lyr_2_4, mask_buf, ip_protocol, 0xff);
    DEVX_SET(fte_match_set_lyr_2_4, value_buf, ip_protocol, MLX5_IP_PROTOCOL_UDP);
    DEVX_SET(fte_match_set_lyr_2_4, mask_buf, udp_dport, 0xffff);
    DEVX_SET(fte_match_set_lyr_2_4, value_buf, udp_dport, cfg_it->udp_dst_port_);

    void* m4_mask = DEVX_ADDR_OF(fte_match_param, mask_buf, misc_parameters_4);
    void* m4_value = DEVX_ADDR_OF(fte_match_param, value_buf, misc_parameters_4);
    DEVX_SET(fte_match_set_misc4, m4_mask, prog_sample_field_id_0,
             node_it->second.sample_field_id);
    DEVX_SET(fte_match_set_misc4, m4_value, prog_sample_field_id_0,
             node_it->second.sample_field_id);
    DEVX_SET(fte_match_set_misc4, m4_mask, prog_sample_field_value_0,
             mt.flex_item_match_.mask_);
    DEVX_SET(fte_match_set_misc4, m4_value, prog_sample_field_value_0,
             mt.flex_item_match_.val_ & mt.flex_item_match_.mask_);

    const bool ok =
        create_dr_rule_locked(port,
                              st,
                              MLX5_DR_MATCH_CRITERIA_OUTER | MLX5_DR_MATCH_CRITERIA_MISC4,
                              reinterpret_cast<struct mlx5dv_flow_match_parameters*>(&mask),
                              reinterpret_cast<struct mlx5dv_flow_match_parameters*>(&value),
                              dest,
                              priority,
                              flow_id,
                              flow.name_.c_str(),
                              dynamic_entry,
                              flow_reformats);
    if (!ok) { cleanup_dynamic_reformats(); }
    return ok ? Status::SUCCESS : Status::GENERIC_FAILURE;
  }

  if (mt.type_ == FlowMatchType::ECPRI) {
    DrMatchParam mask{}, value{};
    mask.match_sz = sizeof(mask.buf);
    value.match_sz = sizeof(value.buf);
    uint16_t criteria = 0;
    if (build_ecpri_match_locked(
            dest->ctx, st, mt.ecpri_match_, reinterpret_cast<uint8_t*>(mask.buf),
            reinterpret_cast<uint8_t*>(value.buf), &criteria) != Status::SUCCESS) {
      return Status::GENERIC_FAILURE;
    }
    return create_dr_rule_locked(port, st, criteria,
                                 reinterpret_cast<struct mlx5dv_flow_match_parameters*>(&mask),
                                 reinterpret_cast<struct mlx5dv_flow_match_parameters*>(&value),
                                 dest, priority, flow_id, flow.name_.c_str(), dynamic_entry)
               ? Status::SUCCESS
               : Status::GENERIC_FAILURE;
  }

  DrMatchParam mask{}, value{};
  mask.match_sz = sizeof(mask.buf);
  value.match_sz = sizeof(value.buf);
  uint16_t criteria = 0;
  bool any = false;
  auto* mask_buf = reinterpret_cast<uint8_t*>(mask.buf);
  auto* value_buf = reinterpret_cast<uint8_t*>(value.buf);

  auto pin_ipv4 = [&]() {
    DEVX_SET(fte_match_set_lyr_2_4, mask_buf, ethertype, 0xffff);
    DEVX_SET(fte_match_set_lyr_2_4, value_buf, ethertype, MLX5_ETHERTYPE_IPV4);
    criteria |= MLX5_DR_MATCH_CRITERIA_OUTER;
  };
  auto pin_ipv4_udp = [&]() {
    pin_ipv4();
    DEVX_SET(fte_match_set_lyr_2_4, mask_buf, ip_protocol, 0xff);
    DEVX_SET(fte_match_set_lyr_2_4, value_buf, ip_protocol, MLX5_IP_PROTOCOL_UDP);
  };

  if (mt.udp_src_ > 0) {
    pin_ipv4_udp();
    DEVX_SET(fte_match_set_lyr_2_4, mask_buf, udp_sport, 0xffff);
    DEVX_SET(fte_match_set_lyr_2_4, value_buf, udp_sport, mt.udp_src_);
    any = true;
  }
  if (mt.udp_dst_ > 0) {
    pin_ipv4_udp();
    DEVX_SET(fte_match_set_lyr_2_4, mask_buf, udp_dport, 0xffff);
    DEVX_SET(fte_match_set_lyr_2_4, value_buf, udp_dport, mt.udp_dst_);
    any = true;
  }
  if (mt.ipv4_src_ != INADDR_ANY) {
    pin_ipv4();
    DEVX_SET(fte_match_set_lyr_2_4, mask_buf, src_ipv4_src_ipv6.ipv4_layout.ipv4, 0xffffffff);
    DEVX_SET(fte_match_set_lyr_2_4, value_buf, src_ipv4_src_ipv6.ipv4_layout.ipv4,
             ntohl(mt.ipv4_src_));
    any = true;
  }
  if (mt.ipv4_dst_ != INADDR_ANY) {
    pin_ipv4();
    DEVX_SET(fte_match_set_lyr_2_4, mask_buf, dst_ipv4_dst_ipv6.ipv4_layout.ipv4, 0xffffffff);
    DEVX_SET(fte_match_set_lyr_2_4, value_buf, dst_ipv4_dst_ipv6.ipv4_layout.ipv4,
             ntohl(mt.ipv4_dst_));
    any = true;
  }
  if (mt.ipv4_len_ > 0) {
    if (st.ipv4_len_node.obj == nullptr) {
      uint32_t sample_id = 0;
      struct mlx5dv_devx_obj* node =
          create_flex_parser_node(dest->ctx, MLX5_GRAPH_ARC_NODE_MAC, MLX5_ETHERTYPE_IPV4, 0,
                                  &sample_id);
      if (node == nullptr) {
        DAQIRI_LOG_CRITICAL("ipv4_len: parse-graph node creation failed on port {}", port);
        return Status::GENERIC_FAILURE;
      }
      st.ipv4_len_node = PortSteering::FlexNode{node, sample_id};
    }
    pin_ipv4();
    void* m4_mask = DEVX_ADDR_OF(fte_match_param, mask_buf, misc_parameters_4);
    void* m4_value = DEVX_ADDR_OF(fte_match_param, value_buf, misc_parameters_4);
    DEVX_SET(fte_match_set_misc4, m4_mask, prog_sample_field_id_0,
             st.ipv4_len_node.sample_field_id);
    DEVX_SET(fte_match_set_misc4, m4_value, prog_sample_field_id_0,
             st.ipv4_len_node.sample_field_id);
    DEVX_SET(fte_match_set_misc4, m4_mask, prog_sample_field_value_0, 0x0000ffff);
    DEVX_SET(fte_match_set_misc4, m4_value, prog_sample_field_value_0, mt.ipv4_len_);
    criteria |= MLX5_DR_MATCH_CRITERIA_MISC4;
    any = true;
  }

  if (!any) {
    DAQIRI_LOG_WARN("Flow '{}' has no supported match field", flow.name_);
    cleanup_dynamic_reformats();
    return Status::INVALID_PARAMETER;
  }

  const bool ok =
      create_dr_rule_locked(port,
                            st,
                            criteria,
                            reinterpret_cast<struct mlx5dv_flow_match_parameters*>(&mask),
                            reinterpret_cast<struct mlx5dv_flow_match_parameters*>(&value),
                            dest,
                            priority,
                            flow_id,
                            flow.name_.c_str(),
                            dynamic_entry,
                            flow_reformats);
  if (!ok) { cleanup_dynamic_reformats(); }
  return ok ? Status::SUCCESS : Status::GENERIC_FAILURE;
}

bool IbverbsEngine::probe_send_scheduling(struct ibv_context* ctx) {
  uint32_t in[DEVX_ST_SZ_DW(query_hca_cap_in)] = {0};
  std::vector<uint32_t> out(DEVX_ST_SZ_DW(query_hca_cap_out), 0);
  DEVX_SET(query_hca_cap_in, in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
  DEVX_SET(query_hca_cap_in, in, op_mod, MLX5_HCA_CAP_OPMOD_GENERAL_CUR);
  if (mlx5dv_devx_general_cmd(ctx, in, sizeof(in), out.data(), out.size() * sizeof(uint32_t)) !=
      0) {
    DAQIRI_LOG_WARN("QUERY_HCA_CAP failed ({}); TX send-scheduling disabled", strerror(errno));
    return false;
  }
  void* cap = DEVX_ADDR_OF(query_hca_cap_out, out.data(), capability);
  const uint32_t wot = DEVX_GET(cmd_hca_cap_min, cap, wait_on_time);
  const uint32_t wod = DEVX_GET(cmd_hca_cap_min, cap, wait_on_data);
  const uint32_t freq = DEVX_GET(cmd_hca_cap_min, cap, device_frequency_khz);
  const uint64_t obj_types = DEVX_GET64(cmd_hca_cap_min, cap, general_obj_types);
  const bool flex_parse = (obj_types >> 0x22) & 0x1;  // FLEX_PARSE_GRAPH general object
  DAQIRI_LOG_INFO("HCA flex-parser ({}): FLEX_PARSE_GRAPH general-object {}",
                  ibv_get_device_name(ctx->device), flex_parse ? "SUPPORTED" : "not supported");
  const bool real_time = (freq == 1000000u);  // 1 GHz -> 1 ns ticks
  const bool usable = (wot != 0) && real_time;
  DAQIRI_LOG_INFO(
      "HCA send-scheduling caps ({}): wait_on_time={} wait_on_data={} device_frequency_khz={} "
      "real_time_clock={} -> set_packet_tx_time {}",
      ibv_get_device_name(ctx->device), wot, wod, freq, real_time ? "yes" : "no",
      usable ? "ENABLED" : "disabled");
  return usable;
}

struct mlx5dv_devx_obj* IbverbsEngine::create_flex_parser_node(struct ibv_context* ctx,
                                                               uint8_t arc_node,
                                                               uint16_t compare_value,
                                                               uint16_t offset,
                                                               uint32_t* out_sample_id) {
  uint32_t in[DEVX_ST_SZ_DW(create_flex_parser_in)] = {0};
  uint32_t out[DEVX_ST_SZ_DW(create_flex_parser_out)] = {0};
  void* hdr = DEVX_ADDR_OF(create_flex_parser_in, in, hdr);
  void* flex = DEVX_ADDR_OF(create_flex_parser_in, in, flex);
  DEVX_SET(general_obj_in_cmd_hdr, hdr, opcode, MLX5_CMD_OP_CREATE_GENERAL_OBJECT);
  DEVX_SET(general_obj_in_cmd_hdr, hdr, obj_type, MLX5_GENERAL_OBJ_TYPE_FLEX_PARSE_GRAPH);

  // Fixed-length custom header just long enough to cover the 4-byte sample.
  DEVX_SET(parse_graph_flex, flex, header_length_mode, MLX5_GRAPH_NODE_LEN_FIXED);
  DEVX_SET(parse_graph_flex, flex, header_length_base_value, offset + 4u);

  // One 4-byte sample at the fixed byte offset within the anchored header.
  void* sample = DEVX_ADDR_OF(parse_graph_flex, flex, sample_table);
  DEVX_SET(parse_graph_flow_match_sample, sample, flow_match_sample_en, 1);
  DEVX_SET(parse_graph_flow_match_sample, sample, flow_match_sample_offset_mode,
           MLX5_GRAPH_SAMPLE_OFFSET_FIXED);
  DEVX_SET(parse_graph_flow_match_sample, sample, flow_match_sample_field_base_offset, offset);
  // FIELD-mode params (offset/mask/shift) stay zero in FIXED offset mode.

  // Anchor the node. The parse graph runs as an adjunct pass producing the
  // sample register; it does not displace the NIC's built-in L2-L4 parsing.
  void* in_arc = DEVX_ADDR_OF(parse_graph_flex, flex, input_arc);
  DEVX_SET(parse_graph_arc, in_arc, arc_parse_graph_node, arc_node);
  DEVX_SET(parse_graph_arc, in_arc, compare_condition_value, compare_value);

  struct mlx5dv_devx_obj* obj = mlx5dv_devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
  if (obj == nullptr) {
    DAQIRI_LOG_CRITICAL("CREATE_GENERAL_OBJECT(FLEX_PARSE_GRAPH) failed: {} (syndrome 0x{:x})",
                        strerror(errno), DEVX_GET(general_obj_out_cmd_hdr, out, syndrome));
    return nullptr;
  }
  const uint32_t node_id = DEVX_GET(general_obj_out_cmd_hdr, out, obj_id);

  // The device assigns the sample register id; it isn't reliably echoed in the
  // CREATE response, so QUERY the object back to read flow_match_sample_field_id.
  uint32_t qin[DEVX_ST_SZ_DW(general_obj_in_cmd_hdr)] = {0};
  uint32_t qout[DEVX_ST_SZ_DW(create_flex_parser_out)] = {0};
  DEVX_SET(general_obj_in_cmd_hdr, qin, opcode, MLX5_CMD_OP_QUERY_GENERAL_OBJECT);
  DEVX_SET(general_obj_in_cmd_hdr, qin, obj_type, MLX5_GENERAL_OBJ_TYPE_FLEX_PARSE_GRAPH);
  DEVX_SET(general_obj_in_cmd_hdr, qin, obj_id, node_id);
  if (mlx5dv_devx_obj_query(obj, qin, sizeof(qin), qout, sizeof(qout)) != 0) {
    DAQIRI_LOG_CRITICAL("QUERY_GENERAL_OBJECT(FLEX_PARSE_GRAPH) failed: {} (syndrome 0x{:x})",
                        strerror(errno), DEVX_GET(general_obj_out_cmd_hdr, qout, syndrome));
    mlx5dv_devx_obj_destroy(obj);
    return nullptr;
  }
  void* q_flex = DEVX_ADDR_OF(create_flex_parser_out, qout, flex);
  void* q_sample = DEVX_ADDR_OF(parse_graph_flex, q_flex, sample_table);
  *out_sample_id = DEVX_GET(parse_graph_flow_match_sample, q_sample, flow_match_sample_field_id);
  DAQIRI_LOG_INFO(
      "Flex parser node created: id {} arc {} compare {} offset {} -> sample_field_id {}", node_id,
      arc_node, compare_value, offset, *out_sample_id);
  return obj;
}

struct mlx5dv_devx_obj* IbverbsEngine::create_ecpri_parser_node(struct ibv_context* ctx,
                                                                uint32_t* out_type_sample_id,
                                                                uint32_t* out_id_sample_id) {
  uint32_t in[DEVX_ST_SZ_DW(create_flex_parser_in)] = {0};
  uint32_t out[DEVX_ST_SZ_DW(create_flex_parser_out)] = {0};
  void* hdr = DEVX_ADDR_OF(create_flex_parser_in, in, hdr);
  void* flex = DEVX_ADDR_OF(create_flex_parser_in, in, flex);
  DEVX_SET(general_obj_in_cmd_hdr, hdr, opcode, MLX5_CMD_OP_CREATE_GENERAL_OBJECT);
  DEVX_SET(general_obj_in_cmd_hdr, hdr, obj_type, MLX5_GENERAL_OBJ_TYPE_FLEX_PARSE_GRAPH);

  // Fixed-length custom header covering both 4-byte samples (offsets 0 and 4).
  DEVX_SET(parse_graph_flex, flex, header_length_mode, MLX5_GRAPH_NODE_LEN_FIXED);
  DEVX_SET(parse_graph_flex, flex, header_length_base_value, 8u);

  // Sample 0: eCPRI common header (message type lives in the second byte), at
  // offset 0. Sample 1: eCPRI message body (pc_id/rtc_id), at offset 4.
  void* s0 = DEVX_ADDR_OF(parse_graph_flex, flex, sample_table[0]);
  DEVX_SET(parse_graph_flow_match_sample, s0, flow_match_sample_en, 1);
  DEVX_SET(parse_graph_flow_match_sample, s0, flow_match_sample_offset_mode,
           MLX5_GRAPH_SAMPLE_OFFSET_FIXED);
  DEVX_SET(parse_graph_flow_match_sample, s0, flow_match_sample_field_base_offset, 0);
  void* s1 = DEVX_ADDR_OF(parse_graph_flex, flex, sample_table[1]);
  DEVX_SET(parse_graph_flow_match_sample, s1, flow_match_sample_en, 1);
  DEVX_SET(parse_graph_flow_match_sample, s1, flow_match_sample_offset_mode,
           MLX5_GRAPH_SAMPLE_OFFSET_FIXED);
  DEVX_SET(parse_graph_flow_match_sample, s1, flow_match_sample_field_base_offset, 4);

  // Anchor the node at L2, entered on the eCPRI EtherType.
  void* in_arc = DEVX_ADDR_OF(parse_graph_flex, flex, input_arc);
  DEVX_SET(parse_graph_arc, in_arc, arc_parse_graph_node, MLX5_GRAPH_ARC_NODE_MAC);
  DEVX_SET(parse_graph_arc, in_arc, compare_condition_value, MLX5_ETHERTYPE_ECPRI);

  struct mlx5dv_devx_obj* obj = mlx5dv_devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
  if (obj == nullptr) {
    DAQIRI_LOG_CRITICAL(
        "CREATE_GENERAL_OBJECT(FLEX_PARSE_GRAPH/eCPRI) failed: {} (syndrome 0x{:x})",
        strerror(errno), DEVX_GET(general_obj_out_cmd_hdr, out, syndrome));
    return nullptr;
  }
  const uint32_t node_id = DEVX_GET(general_obj_out_cmd_hdr, out, obj_id);

  // Query the device-assigned sample field ids back (not reliably echoed by CREATE).
  uint32_t qin[DEVX_ST_SZ_DW(general_obj_in_cmd_hdr)] = {0};
  uint32_t qout[DEVX_ST_SZ_DW(create_flex_parser_out)] = {0};
  DEVX_SET(general_obj_in_cmd_hdr, qin, opcode, MLX5_CMD_OP_QUERY_GENERAL_OBJECT);
  DEVX_SET(general_obj_in_cmd_hdr, qin, obj_type, MLX5_GENERAL_OBJ_TYPE_FLEX_PARSE_GRAPH);
  DEVX_SET(general_obj_in_cmd_hdr, qin, obj_id, node_id);
  if (mlx5dv_devx_obj_query(obj, qin, sizeof(qin), qout, sizeof(qout)) != 0) {
    DAQIRI_LOG_CRITICAL("QUERY_GENERAL_OBJECT(FLEX_PARSE_GRAPH/eCPRI) failed: {} (syndrome 0x{:x})",
                        strerror(errno), DEVX_GET(general_obj_out_cmd_hdr, qout, syndrome));
    mlx5dv_devx_obj_destroy(obj);
    return nullptr;
  }
  void* q_flex = DEVX_ADDR_OF(create_flex_parser_out, qout, flex);
  void* q_s0 = DEVX_ADDR_OF(parse_graph_flex, q_flex, sample_table[0]);
  void* q_s1 = DEVX_ADDR_OF(parse_graph_flex, q_flex, sample_table[1]);
  *out_type_sample_id = DEVX_GET(parse_graph_flow_match_sample, q_s0, flow_match_sample_field_id);
  *out_id_sample_id = DEVX_GET(parse_graph_flow_match_sample, q_s1, flow_match_sample_field_id);
  DAQIRI_LOG_INFO("eCPRI flex parser node created: id {} -> type_sample_id {} id_sample_id {}",
                  node_id, *out_type_sample_id, *out_id_sample_id);
  return obj;
}

Status IbverbsEngine::build_ecpri_match_locked(struct ibv_context* ctx, PortSteering& st,
                                               const EcpriMatch& em, uint8_t* mask_buf,
                                               uint8_t* value_buf, uint16_t* criteria) {
  // Always pin the eCPRI EtherType so the rule only matches eCPRI-over-Ethernet
  // frames (the parse-graph sample register is meaningful only for them).
  DEVX_SET(fte_match_set_lyr_2_4, mask_buf, ethertype, 0xffff);
  DEVX_SET(fte_match_set_lyr_2_4, value_buf, ethertype, MLX5_ETHERTYPE_ECPRI);
  *criteria |= MLX5_DR_MATCH_CRITERIA_OUTER;

  if (!em.match_msg_type_ && !em.match_id_) {
    return Status::SUCCESS;  // EtherType-only match -- no flex sampling needed.
  }

  if (st.ecpri_node.obj == nullptr) {
    uint32_t type_sid = 0;
    uint32_t id_sid = 0;
    struct mlx5dv_devx_obj* node = create_ecpri_parser_node(ctx, &type_sid, &id_sid);
    if (node == nullptr) {
      DAQIRI_LOG_CRITICAL("eCPRI: parse-graph node creation failed");
      return Status::GENERIC_FAILURE;
    }
    st.ecpri_node = PortSteering::EcpriNode{node, type_sid, id_sid};
  }

  void* m4_mask = DEVX_ADDR_OF(fte_match_param, mask_buf, misc_parameters_4);
  void* m4_value = DEVX_ADDR_OF(fte_match_param, value_buf, misc_parameters_4);
  // prog_sample_field_id_N selects which parse-graph sample register; set
  // identically in mask and value (a selector, not a matched field). The sample
  // register holds the 4 sampled bytes in big-endian wire order, so DEVX_SET's
  // host->BE conversion of the (value, mask) pair lines up with it -- the same
  // arithmetic the ipv4_len path relies on.
  int slot = 0;
  auto set_sample = [&](uint32_t sid, uint32_t v, uint32_t msk) {
    switch (slot) {
      case 0:
        DEVX_SET(fte_match_set_misc4, m4_mask, prog_sample_field_id_0, sid);
        DEVX_SET(fte_match_set_misc4, m4_value, prog_sample_field_id_0, sid);
        DEVX_SET(fte_match_set_misc4, m4_mask, prog_sample_field_value_0, msk);
        DEVX_SET(fte_match_set_misc4, m4_value, prog_sample_field_value_0, v & msk);
        break;
      case 1:
        DEVX_SET(fte_match_set_misc4, m4_mask, prog_sample_field_id_1, sid);
        DEVX_SET(fte_match_set_misc4, m4_value, prog_sample_field_id_1, sid);
        DEVX_SET(fte_match_set_misc4, m4_mask, prog_sample_field_value_1, msk);
        DEVX_SET(fte_match_set_misc4, m4_value, prog_sample_field_value_1, v & msk);
        break;
      default:
        break;
    }
    slot++;
  };

  // Message type: second byte of the common-header dword (sample at offset 0) ->
  // big-endian register bits [23:16], mask 0x00ff0000.
  if (em.match_msg_type_) {
    set_sample(st.ecpri_node.type_sample_id, static_cast<uint32_t>(em.msg_type_) << 16,
               0x00ff0000u);
  }
  // Identifier: top 16 bits of the message-body dword (sample at offset 4).
  if (em.match_id_) {
    set_sample(st.ecpri_node.id_sample_id, static_cast<uint32_t>(em.id_) << 16, 0xffff0000u);
  }
  *criteria |= MLX5_DR_MATCH_CRITERIA_MISC4;
  return Status::SUCCESS;
}

Status IbverbsEngine::install_port_flows() {
  for (const auto& intf : cfg_.ifs_) {
    if (intf.rx_.queues_.empty()) {
      continue;
    }
    const int port = intf.port_id_;

    // Collect this port's RX queues by queue id, and a context for the domain.
    std::unordered_map<int, IbvRxQueue*> by_id;
    struct ibv_context* ctx = nullptr;
    for (auto& q : rx_queues_) {
      if (q->port_id == port) {
        by_id[q->queue_id] = q.get();
        ctx = q->ctx;
      }
    }
    if (ctx == nullptr) {
      continue;
    }

    PortSteering& st = port_steering_[port];
    st.domain = mlx5dv_dr_domain_create(ctx, MLX5DV_DR_DOMAIN_TYPE_NIC_RX);
    if (st.domain == nullptr) {
      DAQIRI_LOG_CRITICAL("mlx5dv_dr_domain_create failed (port {}): {}", port, strerror(errno));
      return Status::GENERIC_FAILURE;
    }
    st.table = mlx5dv_dr_table_create(st.domain, 0);
    if (st.table == nullptr) {
      DAQIRI_LOG_CRITICAL("mlx5dv_dr_table_create failed (port {}): {}", port, strerror(errno));
      return Status::GENERIC_FAILURE;
    }

    // Create a flex-parser (parse-graph) node per configured flex item. Each
    // node anchors after the UDP header (selected by udp_dst_port) and exposes a
    // 4-byte sample at the configured offset; the device-assigned sample field
    // id is matched later via misc_parameters_4. Flows reference a flex item by
    // id (FlowMatchType::FLEX_ITEM).
    for (const auto& fi : intf.rx_.flex_items_) {
      uint32_t sample_id = 0;
      struct mlx5dv_devx_obj* node = create_flex_parser_node(
          ctx, MLX5_GRAPH_ARC_NODE_UDP, fi.udp_dst_port_, fi.offset_, &sample_id);
      if (node == nullptr) {
        DAQIRI_LOG_CRITICAL("Flex item '{}' (id {}): parse-graph node creation failed on port {}",
                            fi.name_, fi.id_, port);
        return Status::GENERIC_FAILURE;
      }
      st.flex_nodes[fi.id_] = PortSteering::FlexNode{node, sample_id};
      DAQIRI_LOG_INFO("Flex item '{}' id {} -> udp_dst {} offset {} sample_field_id {} (port {})",
                      fi.name_, fi.id_, fi.udp_dst_port_, fi.offset_, sample_id, port);
    }

    // Build the per-flow rules. Each flow with a queue action steers its matched
    // 5-tuple to that queue's TIR. A flow that matches no supported field, and
    // the no-flows case, fall through to a catch-all -> the first queue.
    auto add_rule = [&](uint16_t crit, struct mlx5dv_flow_match_parameters* mask,
                        struct mlx5dv_flow_match_parameters* val, IbvRxQueue* dest, int prio,
                        uint32_t tag, const char* desc,
                        const std::vector<struct mlx5dv_dr_action*>& reformats =
                            std::vector<struct mlx5dv_dr_action*>{}) -> bool {
      struct mlx5dv_dr_matcher* m = mlx5dv_dr_matcher_create(st.table, prio, crit, mask);
      if (m == nullptr) {
        DAQIRI_LOG_CRITICAL("dr_matcher_create failed (port {} {}): {}", port, desc,
                            strerror(errno));
        return false;
      }
      st.matchers.push_back(m);
      // A non-zero tag (the flow id) is delivered per-packet in the CQE
      // (sop_drop_qpn) so get_packet_flow_id can tell flows apart even when they
      // share a queue. The tag action precedes the dest-TIR action.
      struct mlx5dv_dr_action* tag_act = nullptr;
      struct mlx5dv_dr_action* actions[8];
      int na = 0;
      if (tag != 0) {
        tag_act = mlx5dv_dr_action_create_tag(tag);
        if (tag_act == nullptr) {
          DAQIRI_LOG_CRITICAL("dr_action_create_tag({}) failed (port {} {}): {}", tag, port, desc,
                              strerror(errno));
          return false;
        }
        st.tag_actions.push_back(tag_act);
        actions[na++] = tag_act;
      }
      for (auto* reformat : reformats) {
        if (na >= static_cast<int>(sizeof(actions) / sizeof(actions[0])) - 1) {
          DAQIRI_LOG_CRITICAL("Too many reformat actions for flow '{}' on port {}", desc, port);
          return false;
        }
        actions[na++] = reformat;
      }
      actions[na++] = dest->dr_action;
      struct mlx5dv_dr_rule* r = mlx5dv_dr_rule_create(m, val, na, actions);
      if (r == nullptr) {
        DAQIRI_LOG_CRITICAL("dr_rule_create failed (port {} {}): {}", port, desc, strerror(errno));
        return false;
      }
      st.rules.push_back(r);
      PortSteering::RuleSpec spec{m, dest->dr_action, tag_act, reformats, 0, {}};
      spec.value_sz = std::min(val->match_sz, sizeof(spec.value));
      memcpy(spec.value, val->match_buf, spec.value_sz);
      st.rule_specs.push_back(spec);
      return true;
    };

    int installed = 0;
    int prio = 0;
    for (const auto& fl : intf.rx_.flows_) {
      if (fl.action_.type_ != FlowType::QUEUE) {
        continue;
      }
      auto it = by_id.find(fl.action_.id_);
      if (it == by_id.end()) {
        DAQIRI_LOG_ERROR("Flow '{}' targets unknown queue id {} on port {}", fl.name_,
                         fl.action_.id_, port);
        continue;
      }
      const FlowMatch& mt = fl.match_;
      std::vector<struct mlx5dv_dr_action*> flow_reformats;
      for (const auto& action : fl.actions_) {
        if (!flow_action_is_transform(action)) { continue; }
        st.reformat_buffers.emplace_back();
        auto& buffer = st.reformat_buffers.back();
        enum mlx5dv_flow_action_packet_reformat_type reformat_type {};
        if (!build_reformat_buffer(action, buffer, &reformat_type)) {
          DAQIRI_LOG_CRITICAL("Flow '{}' failed to build ibverbs reformat buffer", fl.name_);
          return Status::GENERIC_FAILURE;
        }
        struct mlx5dv_dr_action* reformat = mlx5dv_dr_action_create_packet_reformat(
            st.domain, 0, reformat_type, buffer.size(), buffer.data());
        if (reformat == nullptr) {
          DAQIRI_LOG_CRITICAL("Flow '{}' failed to create ibverbs packet reformat action: {}",
                              fl.name_, strerror(errno));
          return Status::GENERIC_FAILURE;
        }
        st.reformat_actions.push_back(reformat);
        flow_reformats.push_back(reformat);
      }

      // Flex-item match: combine the flex item's UDP destination port (outer)
      // with the parse-graph sample register (misc_parameters_4). Pinning the
      // UDP port keeps the sample meaningful -- the register is only populated
      // for packets that traversed the node.
      if (mt.type_ == FlowMatchType::FLEX_ITEM) {
        const uint16_t fid = mt.flex_item_match_.flex_item_id_;
        auto fn = st.flex_nodes.find(fid);
        auto fcfg = std::find_if(intf.rx_.flex_items_.begin(), intf.rx_.flex_items_.end(),
                                 [&](const FlexItemConfig& c) { return c.id_ == fid; });
        if (fn == st.flex_nodes.end() || fcfg == intf.rx_.flex_items_.end()) {
          DAQIRI_LOG_ERROR("Flow '{}' references unknown flex item id {}; skipping", fl.name_, fid);
          continue;
        }
        DrMatchParam fmask{}, fval{};
        fmask.match_sz = sizeof(fmask.buf);
        fval.match_sz = sizeof(fval.buf);
        auto* fmk = reinterpret_cast<uint8_t*>(fmask.buf);
        auto* fvl = reinterpret_cast<uint8_t*>(fval.buf);
        DEVX_SET(fte_match_set_lyr_2_4, fmk, ethertype, 0xffff);
        DEVX_SET(fte_match_set_lyr_2_4, fvl, ethertype, MLX5_ETHERTYPE_IPV4);
        DEVX_SET(fte_match_set_lyr_2_4, fmk, ip_protocol, 0xff);
        DEVX_SET(fte_match_set_lyr_2_4, fvl, ip_protocol, MLX5_IP_PROTOCOL_UDP);
        DEVX_SET(fte_match_set_lyr_2_4, fmk, udp_dport, 0xffff);
        DEVX_SET(fte_match_set_lyr_2_4, fvl, udp_dport, fcfg->udp_dst_port_);
        void* m4_mask = DEVX_ADDR_OF(fte_match_param, fmk, misc_parameters_4);
        void* m4_val = DEVX_ADDR_OF(fte_match_param, fvl, misc_parameters_4);
        // prog_sample_field_id_N selects which parse-graph sample register; it
        // is set identically in mask and value (a selector, not a matched field).
        DEVX_SET(fte_match_set_misc4, m4_mask, prog_sample_field_id_0, fn->second.sample_field_id);
        DEVX_SET(fte_match_set_misc4, m4_val, prog_sample_field_id_0, fn->second.sample_field_id);
        DEVX_SET(fte_match_set_misc4, m4_mask, prog_sample_field_value_0,
                 mt.flex_item_match_.mask_);
        DEVX_SET(fte_match_set_misc4, m4_val, prog_sample_field_value_0,
                 mt.flex_item_match_.val_ & mt.flex_item_match_.mask_);
        if (!add_rule(MLX5_DR_MATCH_CRITERIA_OUTER | MLX5_DR_MATCH_CRITERIA_MISC4,
                      reinterpret_cast<struct mlx5dv_flow_match_parameters*>(&fmask),
                      reinterpret_cast<struct mlx5dv_flow_match_parameters*>(&fval), it->second,
                      prio++, fl.id_, fl.name_.c_str(), flow_reformats)) {
          return Status::GENERIC_FAILURE;
        }
        DAQIRI_LOG_INFO(
            "Flow '{}' (flex item {}) -> queue {} (port {}): udp_dst={} sample==0x{:x}/0x{:x}",
            fl.name_, fid, fl.action_.id_, port, fcfg->udp_dst_port_, mt.flex_item_match_.val_,
            mt.flex_item_match_.mask_);
        installed++;
        continue;
      }

      // eCPRI-over-Ethernet match: pin the eCPRI EtherType (outer) and, when a
      // message type / identifier is requested, the parse-graph sample registers
      // (misc_parameters_4). One shared eCPRI node per port, created lazily.
      if (mt.type_ == FlowMatchType::ECPRI) {
        DrMatchParam emask{}, eval{};
        emask.match_sz = sizeof(emask.buf);
        eval.match_sz = sizeof(eval.buf);
        uint16_t ecrit = 0;
        if (build_ecpri_match_locked(
                ctx, st, mt.ecpri_match_, reinterpret_cast<uint8_t*>(emask.buf),
                reinterpret_cast<uint8_t*>(eval.buf), &ecrit) != Status::SUCCESS) {
          return Status::GENERIC_FAILURE;
        }
        if (!add_rule(ecrit, reinterpret_cast<struct mlx5dv_flow_match_parameters*>(&emask),
                      reinterpret_cast<struct mlx5dv_flow_match_parameters*>(&eval), it->second,
                      prio++, fl.id_, fl.name_.c_str())) {
          return Status::GENERIC_FAILURE;
        }
        DAQIRI_LOG_INFO(
            "Flow '{}' (eCPRI) -> queue {} (port {}): msg_type={}(matched={}) id={}(matched={})",
            fl.name_, fl.action_.id_, port, mt.ecpri_match_.msg_type_,
            mt.ecpri_match_.match_msg_type_, mt.ecpri_match_.id_, mt.ecpri_match_.match_id_);
        installed++;
        continue;
      }

      // Full-size param so an ipv4_len match can reach misc_parameters_4; outer
      // fields live at offset 0 either way. criteria accumulates as fields are set.
      DrMatchParam mask{}, val{};
      mask.match_sz = sizeof(mask.buf);
      val.match_sz = sizeof(val.buf);
      uint16_t crit = 0;
      bool any = false;
      auto* mk = reinterpret_cast<uint8_t*>(mask.buf);
      auto* vl = reinterpret_cast<uint8_t*>(val.buf);
      // L3/L4 matches imply IPv4 + UDP -- pin those so the match is unambiguous.
      auto pin_ipv4 = [&]() {
        DEVX_SET(fte_match_set_lyr_2_4, mk, ethertype, 0xffff);
        DEVX_SET(fte_match_set_lyr_2_4, vl, ethertype, MLX5_ETHERTYPE_IPV4);
        crit |= MLX5_DR_MATCH_CRITERIA_OUTER;
      };
      auto pin_ipv4_udp = [&]() {
        pin_ipv4();
        DEVX_SET(fte_match_set_lyr_2_4, mk, ip_protocol, 0xff);
        DEVX_SET(fte_match_set_lyr_2_4, vl, ip_protocol, MLX5_IP_PROTOCOL_UDP);
      };
      if (mt.udp_src_ > 0) {
        pin_ipv4_udp();
        DEVX_SET(fte_match_set_lyr_2_4, mk, udp_sport, 0xffff);
        DEVX_SET(fte_match_set_lyr_2_4, vl, udp_sport, mt.udp_src_);
        any = true;
      }
      if (mt.udp_dst_ > 0) {
        pin_ipv4_udp();
        DEVX_SET(fte_match_set_lyr_2_4, mk, udp_dport, 0xffff);
        DEVX_SET(fte_match_set_lyr_2_4, vl, udp_dport, mt.udp_dst_);
        any = true;
      }
      if (mt.ipv4_src_ != INADDR_ANY) {
        pin_ipv4();
        DEVX_SET(fte_match_set_lyr_2_4, mk, src_ipv4_src_ipv6.ipv4_layout.ipv4, 0xffffffff);
        DEVX_SET(fte_match_set_lyr_2_4, vl, src_ipv4_src_ipv6.ipv4_layout.ipv4,
                 ntohl(mt.ipv4_src_));
        any = true;
      }
      if (mt.ipv4_dst_ != INADDR_ANY) {
        pin_ipv4();
        DEVX_SET(fte_match_set_lyr_2_4, mk, dst_ipv4_dst_ipv6.ipv4_layout.ipv4, 0xffffffff);
        DEVX_SET(fte_match_set_lyr_2_4, vl, dst_ipv4_dst_ipv6.ipv4_layout.ipv4,
                 ntohl(mt.ipv4_dst_));
        any = true;
      }
      if (mt.ipv4_len_ > 0) {
        // IPv4 total-length isn't a native steering field. Sample it with an
        // L2-anchored parse-graph node (entered on the IPv4 ethertype) that reads
        // the first 4 bytes of the IPv4 header; total_length is the low 16 bits of
        // that big-endian dword. One shared node per port, matched via misc4.
        if (st.ipv4_len_node.obj == nullptr) {
          uint32_t sid = 0;
          struct mlx5dv_devx_obj* node =
              create_flex_parser_node(ctx, MLX5_GRAPH_ARC_NODE_MAC, MLX5_ETHERTYPE_IPV4, 0, &sid);
          if (node == nullptr) {
            DAQIRI_LOG_CRITICAL("ipv4_len: parse-graph node creation failed on port {}", port);
            return Status::GENERIC_FAILURE;
          }
          st.ipv4_len_node = PortSteering::FlexNode{node, sid};
        }
        pin_ipv4();
        void* m4_mask = DEVX_ADDR_OF(fte_match_param, mk, misc_parameters_4);
        void* m4_val = DEVX_ADDR_OF(fte_match_param, vl, misc_parameters_4);
        DEVX_SET(fte_match_set_misc4, m4_mask, prog_sample_field_id_0,
                 st.ipv4_len_node.sample_field_id);
        DEVX_SET(fte_match_set_misc4, m4_val, prog_sample_field_id_0,
                 st.ipv4_len_node.sample_field_id);
        DEVX_SET(fte_match_set_misc4, m4_mask, prog_sample_field_value_0, 0x0000ffff);
        DEVX_SET(fte_match_set_misc4, m4_val, prog_sample_field_value_0, mt.ipv4_len_);
        crit |= MLX5_DR_MATCH_CRITERIA_MISC4;
        any = true;
      }
      if (!any) {
        DAQIRI_LOG_WARN("Flow '{}' has no supported match field; skipping", fl.name_);
        continue;
      }
      if (!add_rule(crit, reinterpret_cast<struct mlx5dv_flow_match_parameters*>(&mask),
                    reinterpret_cast<struct mlx5dv_flow_match_parameters*>(&val), it->second,
                    prio++, fl.id_, fl.name_.c_str(), flow_reformats)) {
        return Status::GENERIC_FAILURE;
      }
      DAQIRI_LOG_INFO("Flow '{}' -> queue {} (port {}): udp_src={} udp_dst={} ipv4_len={}",
                      fl.name_, fl.action_.id_, port, mt.udp_src_, mt.udp_dst_, mt.ipv4_len_);
      installed++;
    }

    if (installed == 0 && intf.rx_.flow_isolation_) {
      DAQIRI_LOG_INFO("No static RX flows on isolated ibverbs port {}; dynamic flows may be added "
                      "after initialization",
                      port);
    } else if (installed == 0) {
      // No per-flow rules: catch-all (criteria_enable = 0) -> first queue.
      DrMatchBuf mask{}, val{};
      mask.match_sz = sizeof(mask.buf);
      val.match_sz = sizeof(val.buf);
      IbvRxQueue* dest = by_id.begin()->second;
      if (!add_rule(0, reinterpret_cast<struct mlx5dv_flow_match_parameters*>(&mask),
                    reinterpret_cast<struct mlx5dv_flow_match_parameters*>(&val), dest,
                    kIbverbsCatchAllPriority, 0, "catch-all")) {
        return Status::GENERIC_FAILURE;
      }
      DAQIRI_LOG_INFO("DevX catch-all flow -> queue {} (port {})", dest->queue_id, port);
    }
    st.next_dynamic_priority = std::max(st.next_dynamic_priority, prio);
  }
  return Status::SUCCESS;
}

Status IbverbsEngine::install_tx_flows() {
  for (const auto& intf : cfg_.ifs_) {
    if (intf.tx_.flows_.empty()) {
      continue;
    }
    const int port = intf.port_id_;
    struct ibv_context* ctx = nullptr;
    for (auto& q : tx_queues_) {
      if (q->port_id == port) {
        ctx = q->ctx;
        break;
      }
    }
    if (ctx == nullptr) {
      DAQIRI_LOG_CRITICAL("TX flow configured for port {} but no TX queue context exists", port);
      return Status::GENERIC_FAILURE;
    }

    TxPortSteering& st = tx_port_steering_[port];
    st.domain = mlx5dv_dr_domain_create(ctx, MLX5DV_DR_DOMAIN_TYPE_NIC_TX);
    if (st.domain == nullptr) {
      DAQIRI_LOG_CRITICAL("TX mlx5dv_dr_domain_create failed (port {}): {}", port,
                          strerror(errno));
      return Status::GENERIC_FAILURE;
    }
    st.table = mlx5dv_dr_table_create(st.domain, 0);
    if (st.table == nullptr) {
      DAQIRI_LOG_CRITICAL("TX mlx5dv_dr_table_create failed (port {}): {}", port,
                          strerror(errno));
      return Status::GENERIC_FAILURE;
    }

    int prio = 0;
    for (const auto& fl : intf.tx_.flows_) {
      DrMatchParam mask{}, val{};
      mask.match_sz = sizeof(mask.buf);
      val.match_sz = sizeof(val.buf);
      uint16_t crit = 0;
      bool any = false;
      auto* mk = reinterpret_cast<uint8_t*>(mask.buf);
      auto* vl = reinterpret_cast<uint8_t*>(val.buf);
      auto pin_ipv4 = [&]() {
        DEVX_SET(fte_match_set_lyr_2_4, mk, ethertype, 0xffff);
        DEVX_SET(fte_match_set_lyr_2_4, vl, ethertype, MLX5_ETHERTYPE_IPV4);
        crit |= MLX5_DR_MATCH_CRITERIA_OUTER;
      };
      auto pin_ipv4_udp = [&]() {
        pin_ipv4();
        DEVX_SET(fte_match_set_lyr_2_4, mk, ip_protocol, 0xff);
        DEVX_SET(fte_match_set_lyr_2_4, vl, ip_protocol, MLX5_IP_PROTOCOL_UDP);
      };
      if (fl.match_.udp_src_ > 0) {
        pin_ipv4_udp();
        DEVX_SET(fte_match_set_lyr_2_4, mk, udp_sport, 0xffff);
        DEVX_SET(fte_match_set_lyr_2_4, vl, udp_sport, fl.match_.udp_src_);
        any = true;
      }
      if (fl.match_.udp_dst_ > 0) {
        pin_ipv4_udp();
        DEVX_SET(fte_match_set_lyr_2_4, mk, udp_dport, 0xffff);
        DEVX_SET(fte_match_set_lyr_2_4, vl, udp_dport, fl.match_.udp_dst_);
        any = true;
      }
      if (fl.match_.ipv4_src_ != INADDR_ANY) {
        pin_ipv4();
        DEVX_SET(fte_match_set_lyr_2_4, mk, src_ipv4_src_ipv6.ipv4_layout.ipv4, 0xffffffff);
        DEVX_SET(fte_match_set_lyr_2_4, vl, src_ipv4_src_ipv6.ipv4_layout.ipv4,
                 ntohl(fl.match_.ipv4_src_));
        any = true;
      }
      if (fl.match_.ipv4_dst_ != INADDR_ANY) {
        pin_ipv4();
        DEVX_SET(fte_match_set_lyr_2_4, mk, dst_ipv4_dst_ipv6.ipv4_layout.ipv4, 0xffffffff);
        DEVX_SET(fte_match_set_lyr_2_4, vl, dst_ipv4_dst_ipv6.ipv4_layout.ipv4,
                 ntohl(fl.match_.ipv4_dst_));
        any = true;
      }
      if (fl.match_.ipv4_len_ > 0) {
        DAQIRI_LOG_ERROR("TX flow '{}' uses ipv4_len, which is not supported by ibverbs TX "
                         "steering",
                         fl.name_);
        return Status::GENERIC_FAILURE;
      }

      struct mlx5dv_dr_matcher* matcher = mlx5dv_dr_matcher_create(
          st.table, prio++, any ? crit : 0,
          reinterpret_cast<struct mlx5dv_flow_match_parameters*>(&mask));
      if (matcher == nullptr) {
        DAQIRI_LOG_CRITICAL("TX dr_matcher_create failed (port {} flow '{}'): {}", port,
                            fl.name_, strerror(errno));
        return Status::GENERIC_FAILURE;
      }
      st.matchers.push_back(matcher);

      struct mlx5dv_dr_action* actions[8];
      int na = 0;
      for (const auto& action : fl.actions_) {
        st.reformat_buffers.emplace_back();
        auto& buffer = st.reformat_buffers.back();
        enum mlx5dv_flow_action_packet_reformat_type reformat_type {};
        if (!build_reformat_buffer(action, buffer, &reformat_type)) {
          DAQIRI_LOG_CRITICAL("TX flow '{}' failed to build ibverbs reformat buffer", fl.name_);
          return Status::GENERIC_FAILURE;
        }
        struct mlx5dv_dr_action* reformat = mlx5dv_dr_action_create_packet_reformat(
            st.domain, 0, reformat_type, buffer.size(), buffer.data());
        if (reformat == nullptr) {
          DAQIRI_LOG_CRITICAL("TX flow '{}' failed to create ibverbs packet reformat action: {}",
                              fl.name_, strerror(errno));
          return Status::GENERIC_FAILURE;
        }
        st.reformat_actions.push_back(reformat);
        actions[na++] = reformat;
      }

      struct mlx5dv_dr_rule* rule = mlx5dv_dr_rule_create(
          matcher, reinterpret_cast<struct mlx5dv_flow_match_parameters*>(&val), na, actions);
      if (rule == nullptr) {
        DAQIRI_LOG_CRITICAL("TX dr_rule_create failed (port {} flow '{}'): {}", port, fl.name_,
                            strerror(errno));
        return Status::GENERIC_FAILURE;
      }
      st.rules.push_back(rule);
      DAQIRI_LOG_INFO("TX flow '{}' installed on ibverbs port {}", fl.name_, port);
    }
  }
  return Status::SUCCESS;
}

Status IbverbsEngine::devx_create_cq(IbvRxQueue& q) {
  const size_t page = sysconf(_SC_PAGESIZE);
  // The CQ only needs to hold in-flight completions (the worker drains it
  // continuously); sizing it to num_wqe*strides_per_wqe is both wasteful and,
  // for small packets, large enough that the device may clamp it -- which would
  // desync our assumed cqe_cnt from the real CQ size and stall the poll loop.
  // Use a fixed, comfortably-large power-of-two depth.
  static constexpr uint32_t MAX_CQE = 1u << 17;  // 131072 entries (8 MiB CQ)
  uint64_t want = static_cast<uint64_t>(q.num_wqe) * q.strides_per_wqe;
  if (want > MAX_CQE) {
    want = MAX_CQE;
  }
  const uint32_t log_cq = log2_floor(static_cast<uint32_t>(want));
  const uint32_t cqe_cnt = 1u << log_cq;  // power of two
  constexpr size_t CQE_SZ = 64;
  constexpr size_t DBR_SIZE = 64;
  const size_t cq_bytes = static_cast<size_t>(cqe_cnt) * CQE_SZ;
  const size_t dbr_off = cq_bytes;  // cq_bytes is 64-aligned
  const size_t umem_bytes = (dbr_off + DBR_SIZE + page - 1) & ~(page - 1);

  if (posix_memalign(&q.cq_buf, page, umem_bytes) != 0) {
    DAQIRI_LOG_CRITICAL("posix_memalign(cq umem) failed");
    return Status::GENERIC_FAILURE;
  }
  // Initialize CQEs as invalid (op_own opcode = INVALID) so the poll loop treats
  // them as empty until the HW writes real completions; zero the doorbell.
  memset(q.cq_buf, 0xff, cq_bytes);
  memset(static_cast<uint8_t*>(q.cq_buf) + dbr_off, 0, DBR_SIZE);
  uint32_t* cq_dbr = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(q.cq_buf) + dbr_off);

  q.cq_umem = mlx5dv_devx_umem_reg(q.ctx, q.cq_buf, umem_bytes, 0x7);
  if (q.cq_umem == nullptr) {
    DAQIRI_LOG_CRITICAL("mlx5dv_devx_umem_reg(cq) failed: {}", strerror(errno));
    return Status::GENERIC_FAILURE;
  }

  uint32_t eqn = 0;
  if (mlx5dv_devx_query_eqn(q.ctx, 0, &eqn) != 0) {
    DAQIRI_LOG_CRITICAL("mlx5dv_devx_query_eqn failed: {}", strerror(errno));
    return Status::GENERIC_FAILURE;
  }

  uint32_t in[DEVX_ST_SZ_DW(create_cq_in)] = {0};
  uint32_t out[DEVX_ST_SZ_DW(create_cq_out)] = {0};
  void* cqc = DEVX_ADDR_OF(create_cq_in, in, cq_context);
  DEVX_SET(create_cq_in, in, opcode, MLX5_CMD_OP_CREATE_CQ);
  DEVX_SET(cqc, cqc, cqe_sz, MLX5_CQE_SIZE_64B);
  DEVX_SET(cqc, cqc, log_cq_size, log_cq);
  DEVX_SET(cqc, cqc, uar_page, q.devx_uar->page_id);
  DEVX_SET(cqc, cqc, c_eqn, eqn);
  DEVX_SET(cqc, cqc, log_page_size, log2_floor(static_cast<uint32_t>(page)) - 12u);
  DEVX_SET(cqc, cqc, dbr_umem_valid, 1);
  DEVX_SET(cqc, cqc, dbr_umem_id, q.cq_umem->umem_id);
  DEVX_SET64(cqc, cqc, dbr_addr, dbr_off);
  DEVX_SET(create_cq_in, in, cq_umem_id, q.cq_umem->umem_id);
  DEVX_SET64(create_cq_in, in, cq_umem_offset, 0);
  DEVX_SET(create_cq_in, in, cq_umem_valid, 1);

  q.cq_obj = mlx5dv_devx_obj_create(q.ctx, in, sizeof(in), out, sizeof(out));
  if (q.cq_obj == nullptr) {
    DAQIRI_LOG_CRITICAL("CREATE_CQ (DevX) failed: {} (syndrome 0x{:x})", strerror(errno),
                        DEVX_GET(create_cq_out, out, syndrome));
    return Status::GENERIC_FAILURE;
  }

  // Present the DevX CQ to the worker through the same dv_cq view it already
  // uses for the verbs path (we own the buffer/consumer index/doorbell).
  q.dv_cq.buf = q.cq_buf;
  q.dv_cq.dbrec = reinterpret_cast<__be32*>(cq_dbr);
  q.dv_cq.cqe_cnt = cqe_cnt;
  q.dv_cq.cqe_size = CQE_SZ;
  q.dv_cq.cqn = DEVX_GET(create_cq_out, out, cqn);
  q.cq_ci = 0;
  DAQIRI_LOG_INFO("DevX CQ q{}: cqn {} cqe_cnt {} (eqn {})", q.queue_id, q.dv_cq.cqn, cqe_cnt, eqn);
  return Status::SUCCESS;
}

Status IbverbsEngine::create_striding_rq_devx(IbvRxQueue& q) {
  // Stride geometry only applies to the striding RQ; the regular RQ ignores it.
  uint32_t stride_log = q.striding ? log2_floor(q.stride_size) : 0;
  uint32_t strides_log = q.striding ? log2_floor(q.strides_per_wqe) : 0;
  // CQ first: the RQ references its cqn, and the CQ needs the UAR. Allocate the
  // UAR up front (shared by CQ and RQ).
  q.devx_uar = mlx5dv_devx_alloc_uar(q.ctx, MLX5DV_UAR_ALLOC_TYPE_NC);
  if (q.devx_uar == nullptr) {
    q.devx_uar = mlx5dv_devx_alloc_uar(q.ctx, MLX5DV_UAR_ALLOC_TYPE_BF);
  }
  if (q.devx_uar == nullptr) {
    DAQIRI_LOG_CRITICAL("mlx5dv_devx_alloc_uar failed: {}", strerror(errno));
    return Status::GENERIC_FAILURE;
  }
  if (Status s = devx_create_cq(q); s != Status::SUCCESS) {
    return s;
  }
  if (Status s = devx_create_rq(q, stride_log, strides_log); s != Status::SUCCESS) {
    return s;
  }
  if (Status s = devx_create_tir(q); s != Status::SUCCESS) {
    return s;
  }
  // Flow steering is installed per-port (install_port_flows) once every RX queue
  // exists, so a flow can target any queue's TIR.
  return Status::SUCCESS;
}

void IbverbsEngine::devx_destroy(IbvRxQueue& q) {
  // The dr domain/table/matchers/rules are owned per-port (port_steering_) and
  // torn down before this is called. Only the per-queue steer action + TIR live
  // here.
  if (q.dr_action) {
    mlx5dv_dr_action_destroy(q.dr_action);
    q.dr_action = nullptr;
  }
  if (q.tir_obj) {
    mlx5dv_devx_obj_destroy(q.tir_obj);
    q.tir_obj = nullptr;
  }
  if (q.rq_obj) {
    mlx5dv_devx_obj_destroy(q.rq_obj);
    q.rq_obj = nullptr;
  }
  if (q.cq_obj) {
    mlx5dv_devx_obj_destroy(q.cq_obj);
    q.cq_obj = nullptr;
  }
  if (q.td_obj) {
    mlx5dv_devx_obj_destroy(q.td_obj);
    q.td_obj = nullptr;
  }
  if (q.wq_umem) {
    mlx5dv_devx_umem_dereg(q.wq_umem);
    q.wq_umem = nullptr;
  }
  if (q.cq_umem) {
    mlx5dv_devx_umem_dereg(q.cq_umem);
    q.cq_umem = nullptr;
  }
  if (q.dbr_umem) {
    mlx5dv_devx_umem_dereg(q.dbr_umem);
    q.dbr_umem = nullptr;
  }
  if (q.devx_uar) {
    mlx5dv_devx_free_uar(q.devx_uar);
    q.devx_uar = nullptr;
  }
  // rq_dbr points inside wq_buf (single umem); freed with it.
  if (q.wq_buf) {
    free(q.wq_buf);
    q.wq_buf = nullptr;
  }
  if (q.cq_buf) {
    free(q.cq_buf);
    q.cq_buf = nullptr;
  }
  q.rq_dbr = nullptr;
}

Status IbverbsEngine::setup_rx_queue(IbvRxQueue& q, const InterfaceConfig& intf,
                                     const RxQueueConfig& qcfg) {
  q.port_id = intf.port_id_;
  q.queue_id = qcfg.common_.id_;
  q.batch_size = std::max(1, qcfg.common_.batch_size_);
  q.timeout_us = qcfg.timeout_us_;
  if (!qcfg.common_.cpu_core_.empty()) {
    q.cpu_core = std::stoi(qcfg.common_.cpu_core_);
  }
  if (qcfg.common_.mrs_.empty()) {
    DAQIRI_LOG_CRITICAL("RX queue {} has no memory region", q.queue_id);
    return Status::INVALID_PARAMETER;
  }
  q.mr_names = qcfg.common_.mrs_;
  q.mr_name = qcfg.common_.mrs_[0];
  // A queue with >1 memory region is header-data split: region 0 (CPU) holds the
  // header, region 1 (GPU) the payload, split at region 0's buf_size. Physical
  // HDS needs per-packet multi-segment scatter, which MPRQ can't express -- those
  // queues use the (slower) regular RQ. MPRQ stays the default everywhere else.
  if (qcfg.common_.mrs_.size() > 1) {
    q.num_segs = std::min<int>(static_cast<int>(qcfg.common_.mrs_.size()), MAX_NUM_SEGS);
    q.split_boundary = static_cast<int>(cfg_.mrs_[q.mr_name].buf_size_);
    q.striding = false;
  } else {
    q.num_segs = 1;
    q.split_boundary = 0;
  }
  if (const char* s = getenv("DAQIRI_IBV_STRIDING")) {
    q.striding = (s[0] != '0');
  }

  // Per-queue flow id: if a configured flow steers to this queue, report its id
  // for packets received here (see get_packet_flow_id).
  for (const auto& fl : intf.rx_.flows_) {
    if (fl.action_.type_ == FlowType::QUEUE && fl.action_.id_ == q.queue_id) {
      q.flow_id = fl.id_;
      break;
    }
  }

  q.ctx = open_device_for_interface(intf);
  if (q.ctx == nullptr) {
    return Status::GENERIC_FAILURE;
  }
  q.pd = pd_map_[q.ctx];

  if (Status s = register_rx_mr(q); s != Status::SUCCESS) {
    return s;
  }
  if (Status s = create_striding_rq(q); s != Status::SUCCESS) {
    return s;
  }

  // App-facing burst ring.
  const std::string ring_name =
      "ibv_rx_" + std::to_string(q.port_id) + "_" + std::to_string(q.queue_id);
  q.ring = daqiri::Ring::create(ring_name.c_str(), 2048, daqiri::RingMode::MPMC,
                                daqiri::detail::numa_node_for_cpu(cfg_.common_.master_core_));
  if (q.ring == nullptr) {
    DAQIRI_LOG_CRITICAL("Failed to create RX ring {}", ring_name);
    return Status::GENERIC_FAILURE;
  }

  if (Status s = init_reorder(q, intf, qcfg); s != Status::SUCCESS) {
    return s;
  }

  return Status::SUCCESS;
}

void IbverbsEngine::initialize() {
  DAQIRI_LOG_INFO("Initializing ibverbs (MPRQ) raw backend");

  // Assign port ids and compute MR sizing (one large contiguous region per MR).
  int if_num = 0;
  for (auto& intf : cfg_.ifs_) {
    intf.port_id_ = if_num++;
  }
  for (auto& mr : cfg_.mrs_) {
    const size_t align = std::max<size_t>(get_alignment(mr.second.kind_), GPU_PAGE_SIZE);
    mr.second.adj_size_ = (mr.second.buf_size_ + align - 1) & ~(align - 1);
  }

  if (allocate_memory_regions() != Status::SUCCESS) {
    DAQIRI_LOG_CRITICAL("Failed to allocate memory regions");
    return;
  }

  // Determine the largest batch (RX + TX) and build the burst metadata pools.
  for (const auto& intf : cfg_.ifs_) {
    for (const auto& q : intf.rx_.queues_) {
      max_batch_ = std::max<uint32_t>(max_batch_, std::max(1, q.common_.batch_size_));
    }
    for (const auto& q : intf.tx_.queues_) {
      max_batch_ = std::max<uint32_t>(max_batch_, std::max(1, q.common_.batch_size_));
    }
  }
  if (max_batch_ == 0) {
    DAQIRI_LOG_WARN("No RX/TX queues configured for ibverbs backend");
    initialized_ = true;
    return;
  }
  g_layout = compute_layout(max_batch_);
  rx_meta_pool_size_ = cfg_.rx_meta_buffers_ ? cfg_.rx_meta_buffers_ : 4096;
  const int pool_numa = daqiri::detail::numa_node_for_cpu(cfg_.common_.master_core_);
  rx_meta_pool_ =
      daqiri::ObjectPool::create("IBV_RX_META", rx_meta_pool_size_ - 1, g_layout.total, pool_numa);
  if (rx_meta_pool_ == nullptr) {
    DAQIRI_LOG_CRITICAL("Failed to create RX metadata pool");
    return;
  }
  const size_t tx_meta_n = cfg_.tx_meta_buffers_ ? cfg_.tx_meta_buffers_ : 4096;
  tx_meta_pool_ =
      daqiri::ObjectPool::create("IBV_TX_META", tx_meta_n - 1, g_layout.total, pool_numa);
  if (tx_meta_pool_ == nullptr) {
    DAQIRI_LOG_CRITICAL("Failed to create TX metadata pool");
    return;
  }

  for (auto& intf : cfg_.ifs_) {
    for (const auto& qcfg : intf.rx_.queues_) {
      auto q = std::make_unique<IbvRxQueue>();
      if (Status s = setup_rx_queue(*q, intf, qcfg); s != Status::SUCCESS) {
        DAQIRI_LOG_CRITICAL("Failed to set up RX queue {} on port {}", qcfg.common_.id_,
                            intf.port_id_);
        return;
      }
      rx_queues_.push_back(std::move(q));
    }
    for (const auto& qcfg : intf.tx_.queues_) {
      auto q = std::make_unique<IbvTxQueue>();
      if (Status s = setup_tx_queue(*q, intf, qcfg); s != Status::SUCCESS) {
        DAQIRI_LOG_CRITICAL("Failed to set up TX queue {} on port {}", qcfg.common_.id_,
                            intf.port_id_);
        return;
      }
      tx_queues_.push_back(std::move(q));
    }
  }

  // Raise each port's netdev MTU to cover the configured frame size before any
  // traffic flows (the kernel netdev MTU gates jumbo RX on this path).
  ensure_port_mtus();

  // All RX queues (and their TIRs) now exist -- install per-port flow steering.
  if (install_port_flows() != Status::SUCCESS) {
    DAQIRI_LOG_CRITICAL("Failed to install RX flow steering");
    return;
  }
  if (install_tx_flows() != Status::SUCCESS) {
    DAQIRI_LOG_CRITICAL("Failed to install TX flow steering");
    return;
  }

  // Probe accurate-send-scheduling support once per TX device, then enable it
  // on that device's TX queues.
  {
    std::map<struct ibv_context*, bool> probed;
    for (auto& q : tx_queues_) {
      if (q->ctx == nullptr) {
        continue;
      }
      auto it = probed.find(q->ctx);
      if (it == probed.end()) {
        it = probed.emplace(q->ctx, probe_send_scheduling(q->ctx)).first;
      }
      q->send_scheduling = it->second;
      // Real-time wait mask: compare the low 3 bits of seconds + 32 bits of ns,
      // i.e. an 8-second cyclic window ((MLX5_TS_MASK_SECS=8 << 32) - 1). A full
      // mask makes the seconds field compare wrong (the wait never satisfies).
      q->rt_timemask = (8ULL << 32) - 1ULL;
    }
  }

  // Resolve the port MAC for any TX queue with the tx_eth_src offload (queues
  // are now in tx_queues_, so get_mac_addr can map port -> device).
  for (auto& q : tx_queues_) {
    if (!q->insert_eth_src) {
      continue;
    }
    if (get_mac_addr(q->port_id, reinterpret_cast<char*>(q->eth_src)) != Status::SUCCESS) {
      DAQIRI_LOG_ERROR("tx_eth_src: could not resolve port {} MAC; source MAC not stamped",
                       q->port_id);
      q->insert_eth_src = false;
    } else {
      DAQIRI_LOG_INFO(
          "TX queue {} tx_eth_src: source MAC {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
          q->queue_id, q->eth_src[0], q->eth_src[1], q->eth_src[2], q->eth_src[3], q->eth_src[4],
          q->eth_src[5]);
    }
  }

  initialized_ = true;
  DAQIRI_LOG_INFO("ibverbs backend initialized with {} RX queue(s), {} TX queue(s)",
                  rx_queues_.size(), tx_queues_.size());

  // Engines self-start their workers at the end of initialize() (mirrors
  // DpdkEngine); daqiri_init does not call run() explicitly.
  run();
}

void IbverbsEngine::run() {
  // Group queues by cpu_core: queues that share an explicit core (>= 0) are
  // serviced by ONE round-robin poller thread, so a single CPU can drive
  // multiple queues (matching the DPDK multi-queue poller). Each unpinned queue
  // (cpu_core < 0) keeps its own thread. The group's first ("leader") queue owns
  // the thread handle; shutdown joins it and the rest ride along.
  std::map<int, std::vector<IbvRxQueue*>> rx_groups;
  std::vector<std::vector<IbvRxQueue*>> rx_threads;
  for (auto& q : rx_queues_) {
    q->running = true;
    if (q->cpu_core >= 0) {
      rx_groups[q->cpu_core].push_back(q.get());
    } else {
      rx_threads.push_back({q.get()});
    }
  }
  for (auto& [core, grp] : rx_groups) {
    rx_threads.push_back(grp);
  }
  for (auto& grp : rx_threads) {
    IbvRxQueue* leader = grp[0];
    leader->worker = std::thread([this, grp]() { rx_worker(grp); });
  }

  std::map<int, std::vector<IbvTxQueue*>> tx_groups;
  std::vector<std::vector<IbvTxQueue*>> tx_threads;
  for (auto& q : tx_queues_) {
    q->running = true;
    if (q->cpu_core >= 0) {
      tx_groups[q->cpu_core].push_back(q.get());
    } else {
      tx_threads.push_back({q.get()});
    }
  }
  for (auto& [core, grp] : tx_groups) {
    tx_threads.push_back(grp);
  }
  for (auto& grp : tx_threads) {
    IbvTxQueue* leader = grp[0];
    leader->compl_worker = std::thread([this, grp]() { tx_worker(grp); });
  }
}

// ---------------------------------------------------------------------------
// RX hot path
// ---------------------------------------------------------------------------
bool IbverbsEngine::rx_alloc_burst(IbvRxQueue* q) {
  if (!rx_meta_pool_->get(reinterpret_cast<void**>(&q->cur_burst))) {
    q->cur_burst = nullptr;
    return false;
  }
  BurstParams* b = q->cur_burst;
  b->hdr.hdr.port_id = q->port_id;
  b->hdr.hdr.q_id = q->queue_id;
  b->hdr.hdr.num_pkts = 0;
  b->hdr.hdr.num_segs = q->num_segs;
  b->pkts[0] = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(b) + g_layout.off_pkts0);
  b->pkt_lens[0] = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(b) + g_layout.off_lens0);
  b->pkts[1] = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(b) + g_layout.off_pkts1);
  b->pkt_lens[1] = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(b) + g_layout.off_lens1);
  q->cur_n = 0;
  return true;
}

void IbverbsEngine::rx_flush_burst(IbvRxQueue* q) {
  if (q->cur_burst == nullptr || q->cur_n == 0) {
    return;
  }
  q->cur_burst->hdr.hdr.num_pkts = q->cur_n;
  if (!q->ring->enqueue(q->cur_burst)) {
    // Ring full: drop the burst's claims by releasing its strides.
    uint16_t* wqe_arr = burst_wqe_arr(q->cur_burst);
    uint16_t* strd_arr = burst_strd_arr(q->cur_burst);
    for (int i = 0; i < q->cur_n; i++) {
      release_strides(*q, wqe_arr[i], strd_arr[i]);
    }
    rx_meta_pool_->put(q->cur_burst);
    q->ring_full_bursts++;
    q->ring_full_pkts += static_cast<uint64_t>(q->cur_n);
  }
  q->cur_burst = nullptr;
  q->cur_n = 0;
}

// One bounded pass over a single queue's CQ: drains up to ~batch_size CQEs into
// the queue's in-progress burst, then yields so a shared poller can round-robin
// the next queue. All cursors/accumulators live in the queue, so the partial
// burst persists across passes.
void IbverbsEngine::rx_poll_queue(IbvRxQueue* q) {
  const uint32_t cqe_cnt = q->dv_cq.cqe_cnt;
  const uint32_t cqe_size = q->dv_cq.cqe_size;
  uint8_t* cq_buf = static_cast<uint8_t*>(q->dv_cq.buf);
  const uint32_t wqe_mask = q->num_wqe - 1;
  const uint64_t timeout_cycles = q->timeout_us ? (ibv_timer_hz * q->timeout_us) / 1'000'000ULL : 0;

  if (q->cur_burst == nullptr && !rx_alloc_burst(q)) {
    DAQIRI_LOG_ERROR("RX worker q{}: metadata pool exhausted (increase rx_meta_buffers)",
                     q->queue_id);
    return;
  }
  uint16_t* wqe_arr = burst_wqe_arr(q->cur_burst);
  uint16_t* strd_arr = burst_strd_arr(q->cur_burst);

  const int budget = q->batch_size > 0 ? q->batch_size : 256;
  const uint32_t start_ci = q->cq_ci;
  int processed = 0;
  while (processed < budget) {
    uint8_t* cqe = cq_buf + (q->cq_ci & (cqe_cnt - 1)) * cqe_size;
    // For 128B CQEs the 64B CQE is in the second half.
    struct mlx5_cqe64* cqe64 =
        reinterpret_cast<struct mlx5_cqe64*>(cqe + (cqe_size - sizeof(struct mlx5_cqe64)));

    // A CQE is valid (HW-produced) when its owner bit matches the current lap
    // parity and the opcode is not INVALID -- matches rdma-core's get_sw_cqe.
    const uint8_t op_own = cqe64->op_own;
    const uint8_t opcode = op_own >> 4;
    const uint8_t owner = op_own & 1;
    const uint8_t phase = (q->cq_ci / cqe_cnt) & 1;
    if (opcode == MLX5_CQE_INVALID || owner != phase) {
      // CQ drained. Reclaim freed regions and maybe flush a partial burst on
      // timeout, then yield to the next queue.
      devx_advance_producer(*q);
      if (timeout_cycles && q->cur_n > 0 && (ibv_now_ns() - q->last_flush_tsc) > timeout_cycles) {
        rx_flush_burst(q);
        if (!rx_alloc_burst(q)) {
          return;
        }
        wqe_arr = burst_wqe_arr(q->cur_burst);
        strd_arr = burst_strd_arr(q->cur_burst);
        q->last_flush_tsc = ibv_now_ns();
      }
      break;
    }
    cqe_read_barrier();

    q->cq_ci++;
    q->dbg_cqe++;
    processed++;
    if (opcode == MLX5_CQE_RESP_ERR || opcode == MLX5_CQE_REQ_ERR) {
      if (q->dbg_err < 4) {
        auto* ecqe = reinterpret_cast<struct mlx5_err_cqe*>(cqe64);
        DAQIRI_LOG_ERROR("RX CQE error q{} opcode {} syndrome 0x{:02x} vendor 0x{:02x}",
                         q->queue_id, opcode, ecqe->syndrome, ecqe->vendor_err_synd);
      }
      q->dbg_err++;
      continue;
    }

    const uint32_t byte_cnt = be32toh(cqe64->byte_cnt);
    // Striding RQ packs filler/stride-count/len into byte_cnt; a plain RQ
    // reports the full packet length and consumes exactly one WQE buffer.
    const uint32_t strd =
        q->striding ? ((byte_cnt & MPRQ_STRIDE_NUM_MASK) >> MPRQ_STRIDE_NUM_SHIFT) : 1u;
    const uint32_t len = q->striding ? (byte_cnt & MPRQ_LEN_MASK) : (byte_cnt & 0x00ffffffu);

    // Region (WQE) + intra-WQE stride offset. For a striding RQ the CQE's
    // wqe_counter is the STRIDE index within the WQE, not the WQE index, so the
    // region is tracked in software by accumulating stride counts and advancing
    // to the next WQE when the current one is exhausted. (For a plain RQ the
    // wqe_counter is the WQE index and each WQE holds one packet.)
    uint32_t region;
    uint32_t strd_off;
    if (q->striding) {
      region = q->cur_wqe;
      strd_off = q->cur_consumed;
      q->cur_consumed += strd;
      if (q->cur_consumed >= q->strides_per_wqe) {
        q->cur_consumed = 0;
        q->cur_wqe = (q->cur_wqe + 1) % q->num_wqe;
      }
    } else {
      region = be16toh(cqe64->wqe_counter) & wqe_mask;
      strd_off = 0;
    }

    if (q->striding && (byte_cnt & MPRQ_FILLER_FLAG)) {
      // Region padding -- no packet. Release immediately so it can be reposted.
      q->dbg_filler++;
      release_strides(*q, region, strd);
      continue;
    }
    q->dbg_data++;

    const int n = q->cur_n;
    const uint32_t hdr = (static_cast<uint32_t>(q->split_boundary) < len)
                             ? static_cast<uint32_t>(q->split_boundary)
                             : len;
    if (!q->striding) {
      // Regular RQ: each segment is in its own region (physical split). Slot
      // `region` indexes the per-packet buffer within each region.
      uint8_t* hbuf = q->regions[0].base + static_cast<size_t>(region) * q->regions[0].slot_size;
      if (q->num_segs == 2) {
        uint8_t* pbuf = q->regions[1].base + static_cast<size_t>(region) * q->regions[1].slot_size;
        q->cur_burst->pkts[0][n] = hbuf;
        q->cur_burst->pkt_lens[0][n] = hdr;
        q->cur_burst->pkts[1][n] = pbuf;
        q->cur_burst->pkt_lens[1][n] = len - hdr;
      } else {
        q->cur_burst->pkts[0][n] = hbuf;
        q->cur_burst->pkt_lens[0][n] = len;
      }
    } else {
      uint8_t* pkt = q->mr_base + static_cast<size_t>(region) * q->region_size +
                     static_cast<size_t>(strd_off) * q->stride_size + q->two_byte_shift * 2;
      if (q->num_segs == 2) {
        // Logical header-data split: seg 0 = first split_boundary bytes (header),
        // seg 1 = remaining payload. Both alias the same contiguous stride; one
        // stride-release frees the whole packet (not a physical CPU/GPU split).
        q->cur_burst->pkts[0][n] = pkt;
        q->cur_burst->pkt_lens[0][n] = hdr;
        q->cur_burst->pkts[1][n] = pkt + hdr;
        q->cur_burst->pkt_lens[1][n] = len - hdr;
      } else {
        q->cur_burst->pkts[0][n] = pkt;
        q->cur_burst->pkt_lens[0][n] = len;
      }
    }
    wqe_arr[n] = static_cast<uint16_t>(region);
    strd_arr[n] = static_cast<uint16_t>(strd);
    // Raw free-running HW timestamp from the CQE; converted to ns on demand in
    // get_packet_rx_timestamp (mlx5dv_ts_to_ns).
    burst_ts_arr(q->cur_burst)[n] = be64toh(cqe64->timestamp);
    // Per-packet flow tag (MARK): the steering rule's tag action sets it, the HW
    // delivers it in sop_drop_qpn (24-bit). 0 = untagged.
    burst_flowtag_arr(q->cur_burst)[n] = be32toh(cqe64->sop_drop_qpn) & 0x00ffffffu;
    q->cur_n++;
    if (q->cur_n == 1) {
      q->last_flush_tsc = ibv_now_ns();
    }

    if (q->cur_n >= q->batch_size) {
      rx_flush_burst(q);
      if (!rx_alloc_burst(q)) {
        DAQIRI_LOG_ERROR("RX worker q{}: metadata pool exhausted (increase rx_meta_buffers)",
                         q->queue_id);
        return;
      }
      wqe_arr = burst_wqe_arr(q->cur_burst);
      strd_arr = burst_strd_arr(q->cur_burst);
    }

    // Periodically advance the CQ doorbell so the HW can recycle CQEs, and
    // refill freed striding regions.
    if ((q->cq_ci & 0x3f) == 0) {
      *q->dv_cq.dbrec = htobe32(q->cq_ci & 0xffffff);
      devx_advance_producer(*q);
    }
  }

  // Publish the CQ consumer index for any CQEs drained this pass.
  if (q->cq_ci != start_ci) {
    *q->dv_cq.dbrec = htobe32(q->cq_ci & 0xffffff);
  }
}

void IbverbsEngine::rx_worker(std::vector<IbvRxQueue*> group) {
  if (group.empty()) {
    return;
  }
  IbvRxQueue* leader = group[0];
  if (leader->cpu_core >= 0) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(leader->cpu_core, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
  }
  for (auto* q : group) {
    DAQIRI_LOG_INFO("RX worker started for port {} queue {} on core {} ({} queue(s)/core)",
                    q->port_id, q->queue_id, leader->cpu_core, group.size());
  }

  while (leader->running.load(std::memory_order_relaxed) &&
         !force_quit_.load(std::memory_order_relaxed)) {
    for (auto* q : group) {
      rx_poll_queue(q);
    }
  }

  for (auto* q : group) {
    rx_flush_burst(q);
    if (q->cur_burst) {
      rx_meta_pool_->put(q->cur_burst);
      q->cur_burst = nullptr;
    }
    DAQIRI_LOG_INFO(
        "RX worker stopped for port {} queue {}: cqe_seen={} data={} filler={} err={} cq_ci={} "
        "reposts={}",
        q->port_id, q->queue_id, q->dbg_cqe, q->dbg_data, q->dbg_filler, q->dbg_err, q->cq_ci,
        q->reposts.load());
  }
}

void IbverbsEngine::release_strides(IbvRxQueue& q, uint32_t wqe_idx, uint32_t strd) {
  // Both the striding and the regular RQ are DevX: record the freed strides
  // (regular RQ = 1 per packet); the worker thread drains and reposts WQEs in
  // cyclic order (devx_advance_producer). Safe from the app and worker threads.
  q.freed_strides[wqe_idx].fetch_add(strd, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Burst retrieval / free
// ---------------------------------------------------------------------------
IbvRxQueue* IbverbsEngine::find_rx_queue(int port, int q) {
  for (auto& rq : rx_queues_) {
    if (rq->port_id == port && rq->queue_id == q) {
      return rq.get();
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// GPU packet reordering (reuses the src/kernels.cu C ABI). Mirrors the DPDK
// reorder path but feeds it from the MPRQ RX bursts. Single plan per queue.
// ---------------------------------------------------------------------------
Status IbverbsEngine::init_reorder(IbvRxQueue& q, const InterfaceConfig& intf,
                                   const RxQueueConfig& qcfg) {
  if (intf.rx_.reorder_configs_.empty()) {
    return Status::SUCCESS;
  }
  const auto& src_mr = cfg_.mrs_[q.mr_name];

  // flow id -> queue, to find which reorder configs belong to this queue.
  std::unordered_map<FlowId, uint16_t> flow_to_queue;
  for (const auto& fl : intf.rx_.flows_) {
    flow_to_queue[fl.id_] = fl.action_.id_;
  }

  auto st = std::make_unique<IbvReorderState>();
  for (const auto& rc : intf.rx_.reorder_configs_) {
    if (rc.reorder_type_ != "gpu") {
      continue;
    }
    // Does this reorder config map to this queue?
    bool mine = false;
    for (FlowId fid : rc.flow_ids_) {
      auto it = flow_to_queue.find(fid);
      if (it != flow_to_queue.end() && it->second == static_cast<uint16_t>(q.queue_id)) {
        mine = true;
      }
    }
    if (!mine) {
      continue;
    }

    const auto out_it = cfg_.mrs_.find(rc.memory_region_);
    if (out_it == cfg_.mrs_.end() || ar_[rc.memory_region_].ptr_ == nullptr) {
      DAQIRI_LOG_CRITICAL("Reorder '{}' output MR '{}' missing", rc.name_, rc.memory_region_);
      return Status::INVALID_PARAMETER;
    }
    const auto& out_mr = out_it->second;

    IbvReorderPlan plan;
    plan.cfg = rc;
    plan.port_id = q.port_id;
    plan.queue_id = q.queue_id;
    plan.packets_per_batch = (rc.method_ == ReorderMethod::SEQ_BATCH_NUMBER)
                                 ? rc.seq_batch_number_.packets_per_batch_
                                 : rc.seq_packets_per_batch_.packets_per_batch_;
    if (plan.packets_per_batch == 0) {
      DAQIRI_LOG_CRITICAL("Reorder '{}' packets_per_batch is 0", rc.name_);
      return Status::INVALID_PARAMETER;
    }
    plan.copy_source_offset = rc.payload_byte_offset_;
    plan.slot_stride = static_cast<uint32_t>(src_mr.buf_size_ - rc.payload_byte_offset_);
    plan.data_type_conversion = rdr_uses_conversion(rc);
    plan.cuda_device_id = src_mr.affinity_;
    plan.acc_ptrs.reserve(plan.packets_per_batch);

    cudaSetDevice(plan.cuda_device_id);
    if (cudaMalloc(reinterpret_cast<void**>(&plan.d_input_ptrs),
                   sizeof(void*) * plan.packets_per_batch) != cudaSuccess) {
      DAQIRI_LOG_CRITICAL("Reorder '{}' cudaMalloc(d_input_ptrs) failed", rc.name_);
      return Status::GENERIC_FAILURE;
    }

    // Output pool: carve the output MR into num_bufs buffers.
    auto* out_base = static_cast<uint8_t*>(ar_[rc.memory_region_].ptr_);
    plan.out_bufs.resize(out_mr.num_bufs_);
    for (size_t i = 0; i < out_mr.num_bufs_; i++) {
      auto& ob = plan.out_bufs[i];
      ob.ptr = out_base + i * out_mr.adj_size_;
      ob.src_wqe.reserve(plan.packets_per_batch);
      ob.src_strd.reserve(plan.packets_per_batch);
      if (cudaEventCreateWithFlags(&ob.event, cudaEventDisableTiming) != cudaSuccess ||
          cudaHostAlloc(reinterpret_cast<void**>(&ob.h_batch_id), sizeof(uint64_t),
                        cudaHostAllocDefault) != cudaSuccess ||
          cudaMalloc(reinterpret_cast<void**>(&ob.d_batch_id), sizeof(uint64_t)) != cudaSuccess) {
        DAQIRI_LOG_CRITICAL("Reorder '{}' CUDA event/batch-id alloc failed", rc.name_);
        return Status::GENERIC_FAILURE;
      }
      *ob.h_batch_id = 0;
    }

    const size_t plan_idx = st->plans.size();
    st->plans.push_back(std::move(plan));
    for (FlowId fid : rc.flow_ids_) {
      if (flow_to_queue[fid] == static_cast<uint16_t>(q.queue_id)) {
        st->flow_to_plan[fid] = plan_idx;
      }
    }
    DAQIRI_LOG_INFO("Reorder plan '{}' on q{}: ppb={} offset={} out_bufs={}", rc.name_, q.queue_id,
                    st->plans.back().packets_per_batch, st->plans.back().copy_source_offset,
                    out_mr.num_bufs_);
  }

  if (st->plans.empty()) {
    return Status::SUCCESS;
  }
  st->single_plan = (st->plans.size() == 1);
  st->enabled = true;
  q.reorder = std::move(st);
  return Status::SUCCESS;
}

void IbverbsEngine::reorder_poll_events(IbvRxQueue& q, IbvReorderPlan& plan) {
  for (auto& ob : plan.out_bufs) {
    if (ob.event_complete || ob.event == nullptr) {
      continue;
    }
    if (cudaEventQuery(ob.event) != cudaSuccess) {
      continue;
    }  // still running
    // Kernel finished reading the source packets -> release their strides.
    for (uint32_t i = 0; i < ob.src_count; i++) {
      release_strides(q, ob.src_wqe[i], ob.src_strd[i]);
    }
    ob.src_wqe.clear();
    ob.src_strd.clear();
    ob.src_count = 0;
    ob.event_complete = true;
  }
}

Status IbverbsEngine::reorder_flush_batch(IbvRxQueue& q, IbvReorderPlan& plan, BurstParams** out) {
  *out = nullptr;
  const uint32_t num_pkts = static_cast<uint32_t>(plan.acc_ptrs.size());
  if (num_pkts == 0) {
    return Status::SUCCESS;
  }

  // Acquire a free output buffer (round-robin).
  size_t idx = plan.out_bufs.size();
  for (size_t a = 0; a < plan.out_bufs.size(); a++) {
    const size_t i = (plan.next_out + a) % plan.out_bufs.size();
    if (plan.out_bufs[i].consumer_done && plan.out_bufs[i].event_complete) {
      idx = i;
      plan.next_out = (i + 1) % plan.out_bufs.size();
      break;
    }
  }
  if (idx == plan.out_bufs.size()) {
    DAQIRI_LOG_ERROR("Reorder '{}' has no free output buffer", plan.cfg.name_);
    // Drop this batch's sources so the RQ can recycle.
    for (uint32_t i = 0; i < num_pkts; i++) {
      release_strides(q, plan.acc_wqe[i], plan.acc_strd[i]);
    }
    plan.acc_ptrs.clear();
    plan.acc_wqe.clear();
    plan.acc_strd.clear();
    return Status::NO_FREE_PACKET_BUFFERS;
  }
  auto& ob = plan.out_bufs[idx];
  ob.consumer_done = false;
  ob.event_complete = false;

  const ReorderConfig& c = plan.cfg;
  const uint32_t out_payload = plan.acc_output_payload_len;
  const uint32_t aggregate_len = plan.packets_per_batch * out_payload;

  cudaSetDevice(plan.cuda_device_id);
  cudaMemcpyAsync(plan.d_input_ptrs, plan.acc_ptrs.data(), sizeof(void*) * num_pkts,
                  cudaMemcpyHostToDevice, plan.stream);
  packet_reorder_copy_payload_by_sequence(
      ob.ptr, reinterpret_cast<const void* const*>(plan.d_input_ptrs), plan.acc_input_payload_len,
      out_payload, plan.copy_source_offset, num_pkts, rdr_seq_off(c), rdr_seq_width(c),
      rdr_batch_off(c), rdr_batch_width(c), c.method_ == ReorderMethod::SEQ_BATCH_NUMBER ? 1U : 0U,
      plan.packets_per_batch, plan.packets_per_batch - 1U,
      plan.data_type_conversion ? static_cast<uint8_t>(c.data_types_.input_type_) : 0U,
      plan.data_type_conversion ? static_cast<uint8_t>(c.data_types_.output_type_) : 0U,
      plan.data_type_conversion ? static_cast<uint8_t>(c.data_types_.input_endianness_) : 0U,
      ob.d_batch_id, plan.stream);
  if (cudaGetLastError() != cudaSuccess) {
    DAQIRI_LOG_ERROR("Reorder '{}' kernel launch failed", plan.cfg.name_);
    ob.consumer_done = true;
    ob.event_complete = true;
    return Status::INTERNAL_ERROR;
  }
  cudaMemcpyAsync(ob.h_batch_id, ob.d_batch_id, sizeof(uint64_t), cudaMemcpyDeviceToHost,
                  plan.stream);
  cudaEventRecord(ob.event, plan.stream);

  // Hand the source strides to the output buffer; released when the event fires.
  ob.src_wqe = std::move(plan.acc_wqe);
  ob.src_strd = std::move(plan.acc_strd);
  ob.src_count = num_pkts;
  plan.acc_wqe.clear();
  plan.acc_strd.clear();
  plan.acc_ptrs.clear();

  // Build the reordered output burst (a heap BurstParams, like the DPDK path).
  auto* burst = new BurstParams{};
  burst->hdr.hdr.port_id = plan.port_id;
  burst->hdr.hdr.q_id = plan.queue_id;
  burst->hdr.hdr.num_segs = 1;
  burst->hdr.hdr.num_pkts = 1;
  burst->hdr.hdr.nbytes = aggregate_len;
  burst->hdr.hdr.max_pkt = num_pkts;
  burst->hdr.hdr.burst_flags = DAQIRI_BURST_FLAG_REORDERED;
  burst->event = ob.event;
  auto ctx = std::make_shared<IbvReorderBurstCtx>();
  ctx->state = q.reorder.get();
  ctx->plan = &plan;
  ctx->out_idx = idx;
  ctx->pkt_ptrs[0] = ob.ptr;
  ctx->pkt_lens[0] = aggregate_len;
  ctx->info.source_packet_count = num_pkts;
  ctx->info.packets_per_batch = plan.packets_per_batch;
  ctx->info.payload_len = out_payload;
  ctx->info.aggregate_len = aggregate_len;
  ctx->info.burst_flags = burst->hdr.hdr.burst_flags;
  ctx->h_batch_id = ob.h_batch_id;
  std::memcpy(burst->hdr.custom_burst_data, &ctx->info, sizeof(ctx->info));
  burst->custom_pkt_data = std::static_pointer_cast<void>(ctx);
  burst->pkts[0] = ctx->pkt_ptrs.data();
  burst->pkt_lens[0] = ctx->pkt_lens.data();
  *out = burst;
  return Status::SUCCESS;
}

void IbverbsEngine::reorder_process_raw(IbvRxQueue& q, BurstParams* raw) {
  auto& st = *q.reorder;
  uint16_t* wqe_arr = burst_wqe_arr(raw);
  uint16_t* strd_arr = burst_strd_arr(raw);
  const int num = static_cast<int>(raw->hdr.hdr.num_pkts);
  for (int i = 0; i < num; i++) {
    // Route to a plan. Single plan: index 0; else by this queue's flow id.
    size_t plan_idx = 0;
    if (!st.single_plan) {
      auto it = st.flow_to_plan.find(q.flow_id);
      if (it == st.flow_to_plan.end()) {
        release_strides(q, wqe_arr[i], strd_arr[i]);  // unmatched -> drop
        continue;
      }
      plan_idx = it->second;
    }
    auto& plan = st.plans[plan_idx];
    if (plan.acc_ptrs.empty()) {
      const uint32_t len = raw->pkt_lens[0][i];
      uint32_t in_payload = (len > plan.copy_source_offset) ? (len - plan.copy_source_offset) : 0;
      in_payload = std::min(in_payload, plan.slot_stride);
      plan.acc_input_payload_len = in_payload;
      plan.acc_output_payload_len = rdr_output_payload_len(plan.cfg, in_payload);
    }
    plan.acc_ptrs.push_back(raw->pkts[0][i]);
    plan.acc_wqe.push_back(wqe_arr[i]);
    plan.acc_strd.push_back(strd_arr[i]);
    if (plan.acc_ptrs.size() >= plan.packets_per_batch) {
      BurstParams* out = nullptr;
      if (reorder_flush_batch(q, plan, &out) == Status::SUCCESS && out != nullptr) {
        st.ready.push_back(out);
      }
    }
  }
  // Source strides stay held by reorder state until their kernel completes;
  // return only the raw burst metadata block to the pool.
  rx_meta_pool_->put(raw);
}

Status IbverbsEngine::reorder_get_rx(IbvRxQueue& q, BurstParams** burst) {
  auto& st = *q.reorder;
  std::lock_guard<std::mutex> guard(st.lock);
  for (auto& plan : st.plans) {
    reorder_poll_events(q, plan);
  }
  if (!st.ready.empty()) {
    *burst = st.ready.front();
    st.ready.pop_front();
    return Status::SUCCESS;
  }
  void* b = nullptr;
  while (q.ring->dequeue(&b)) {
    reorder_process_raw(q, static_cast<BurstParams*>(b));
    if (!st.ready.empty()) {
      *burst = st.ready.front();
      st.ready.pop_front();
      return Status::SUCCESS;
    }
  }
  return Status::NOT_READY;
}

void IbverbsEngine::reorder_release_output(BurstParams* burst) {
  if (burst == nullptr || burst->custom_pkt_data == nullptr) {
    return;
  }
  auto ctx = std::static_pointer_cast<IbvReorderBurstCtx>(burst->custom_pkt_data);
  if (ctx && !ctx->released && ctx->plan != nullptr && ctx->out_idx < ctx->plan->out_bufs.size()) {
    ctx->plan->out_bufs[ctx->out_idx].consumer_done = true;
    ctx->released = true;
  }
  delete burst;
}

void IbverbsEngine::reorder_cleanup(IbvRxQueue& q) {
  if (!q.reorder) {
    return;
  }
  for (auto& plan : q.reorder->plans) {
    for (auto& ob : plan.out_bufs) {
      if (ob.event) {
        cudaEventDestroy(ob.event);
      }
      if (ob.h_batch_id) {
        cudaFreeHost(ob.h_batch_id);
      }
      if (ob.d_batch_id) {
        cudaFree(ob.d_batch_id);
      }
    }
    if (plan.d_input_ptrs) {
      cudaFree(plan.d_input_ptrs);
    }
  }
  q.reorder.reset();
}

Status IbverbsEngine::set_reorder_cuda_stream(const std::string& interface_name,
                                              const std::string& reorder_name,
                                              cudaStream_t stream) {
  const int port = get_port_id(interface_name);
  for (auto& q : rx_queues_) {
    if (q->port_id != port || !q->reorder) {
      continue;
    }
    for (auto& plan : q->reorder->plans) {
      if (plan.cfg.name_ == reorder_name) {
        plan.stream = stream;
        DAQIRI_LOG_INFO("Reorder '{}' stream set on port {} q{}", reorder_name, port, q->queue_id);
        return Status::SUCCESS;
      }
    }
  }
  DAQIRI_LOG_ERROR("set_reorder_cuda_stream: no reorder plan '{}' on interface '{}'", reorder_name,
                   interface_name);
  return Status::INVALID_PARAMETER;
}

Status IbverbsEngine::get_reorder_burst_info(BurstParams* burst, ReorderBurstInfo* info) {
  if (burst == nullptr || info == nullptr) {
    return Status::NULL_PTR;
  }
  if (!(burst->hdr.hdr.burst_flags & DAQIRI_BURST_FLAG_REORDERED) ||
      burst->custom_pkt_data == nullptr) {
    return Status::INVALID_PARAMETER;
  }
  auto ctx = std::static_pointer_cast<IbvReorderBurstCtx>(burst->custom_pkt_data);
  if (!ctx) {
    return Status::INVALID_PARAMETER;
  }
  if (ctx->h_batch_id != nullptr) {
    if (burst->event != nullptr) {
      const cudaError_t s = cudaEventQuery(burst->event);
      if (s == cudaErrorNotReady) {
        return Status::NOT_READY;
      }
      if (s != cudaSuccess) {
        (void)cudaGetLastError();
        return Status::INTERNAL_ERROR;
      }
    }
    ctx->info.batch_id = *(ctx->h_batch_id);
  }
  *info = ctx->info;
  return Status::SUCCESS;
}

Status IbverbsEngine::get_rx_burst(BurstParams** burst, int port, int q) {
  IbvRxQueue* rq = find_rx_queue(port, q);
  if (rq == nullptr) {
    return Status::INVALID_PARAMETER;
  }
  if (rq->reorder && rq->reorder->enabled) {
    return reorder_get_rx(*rq, burst);
  }
  void* b = nullptr;
  if (!rq->ring->dequeue(&b)) {
    return Status::NOT_READY;
  }
  *burst = static_cast<BurstParams*>(b);
  return Status::SUCCESS;
}

// Releasing packet data (free_*_packets) reclaims strides and reposts WQEs;
// returning the burst (free_rx_burst / free_rx_metadata) hands the metadata
// block back to the pool. These are distinct resources, mirroring the DPDK
// manager -- the standard RX free path (free_all_packets_and_burst_rx) calls
// free_all_packets then free_rx_burst, so strides are released exactly once
// and the block returned exactly once.
void IbverbsEngine::free_packet(BurstParams* burst, int pkt) {
  if (burst == nullptr) {
    return;
  }
  IbvRxQueue* q = find_rx_queue(burst->hdr.hdr.port_id, burst->hdr.hdr.q_id);
  if (q == nullptr) {
    return;
  }
  release_strides(*q, burst_wqe_arr(burst)[pkt], burst_strd_arr(burst)[pkt]);
}

void IbverbsEngine::free_all_packets(BurstParams* burst) {
  if (burst == nullptr) {
    return;
  }
  if (burst->hdr.hdr.burst_flags & DAQIRI_BURST_FLAG_REORDERED) {
    // Reordered output: the "packet" is the GPU output buffer, not strides;
    // it's reclaimed in free_rx_burst. Nothing to release here.
    return;
  }
  if (burst->hdr.hdr.burst_flags & IBV_TX_BURST_FLAG) {
    // TX burst the app allocated but never sent (a local fill failure): roll
    // back the allocation. The bench only does this for the just-allocated
    // burst, so these are the most-recent (unposted) cyclic slots.
    IbvTxQueue* tq = find_tx_queue(burst->hdr.hdr.port_id, burst->hdr.hdr.q_id);
    if (tq == nullptr) {
      return;
    }
    tq->alloc_head -= static_cast<uint64_t>(burst->hdr.hdr.num_pkts);
    return;
  }
  IbvRxQueue* q = find_rx_queue(burst->hdr.hdr.port_id, burst->hdr.hdr.q_id);
  if (q == nullptr) {
    return;
  }
  uint16_t* wqe_arr = burst_wqe_arr(burst);
  uint16_t* strd_arr = burst_strd_arr(burst);
  const int num = static_cast<int>(burst->hdr.hdr.num_pkts);
  for (int i = 0; i < num; i++) {
    release_strides(*q, wqe_arr[i], strd_arr[i]);
  }
}

void IbverbsEngine::free_packet_segment(BurstParams* burst, int seg, int pkt) {
  // Segments alias one stride; releasing seg 0 releases the packet.
  if (seg == 0) {
    free_packet(burst, pkt);
  }
}

void IbverbsEngine::free_all_segment_packets(BurstParams* burst, int seg) {
  if (seg == 0) {
    free_all_packets(burst);
  }
}

void IbverbsEngine::free_rx_burst(BurstParams* burst) {
  if (burst == nullptr) {
    return;
  }
  if (burst->hdr.hdr.burst_flags & DAQIRI_BURST_FLAG_REORDERED) {
    // Heap-allocated reordered burst: release its output buffer and delete it.
    reorder_release_output(burst);
    return;
  }
  rx_meta_pool_->put(burst);
}

void IbverbsEngine::free_rx_metadata(BurstParams* burst) {
  if (burst == nullptr) {
    return;
  }
  if (burst->hdr.hdr.burst_flags & DAQIRI_BURST_FLAG_REORDERED) {
    reorder_release_output(burst);
    return;
  }
  rx_meta_pool_->put(burst);
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------
void* IbverbsEngine::get_packet_ptr(BurstParams* burst, int idx) {
  return burst->pkts[0][idx];
}

uint32_t IbverbsEngine::get_packet_length(BurstParams* burst, int idx) {
  return burst->pkt_lens[0][idx];
}

void* IbverbsEngine::get_segment_packet_ptr(BurstParams* burst, int seg, int idx) {
  return burst->pkts[seg][idx];
}

uint32_t IbverbsEngine::get_segment_packet_length(BurstParams* burst, int seg, int idx) {
  return burst->pkt_lens[seg][idx];
}

FlowId IbverbsEngine::get_packet_flow_id(BurstParams* burst, int idx) {
  // Per-packet MARK tag captured from the CQE (set by the flow's tag action).
  // Distinguishes flows that share a queue. Falls back to the per-queue flow_id
  // for untagged packets (tag 0), e.g. catch-all traffic or single-flow configs.
  const uint32_t tag = burst_flowtag_arr(burst)[idx];
  if (tag != 0) {
    return tag;
  }
  IbvRxQueue* q = find_rx_queue(burst->hdr.hdr.port_id, burst->hdr.hdr.q_id);
  return q ? q->flow_id : 0;
}

// Convert a raw free-running HW timestamp (from the CQE) to nanoseconds using
// rdma-core's wrap-safe helper. The per-device clock_info is cached and
// refreshed ~twice a second (the HW clock wraps, so a stale snapshot would
// mis-convert). Thread-safe: app RX-consume threads may call concurrently.
uint64_t IbverbsEngine::ts_to_ns(struct ibv_context* ctx, uint64_t raw_ts) {
  std::lock_guard<std::mutex> lk(clock_mtx_);
  ClockCache& c = clock_cache_[ctx];
  const uint64_t now = ibv_now_ns();
  if (!c.valid || (now - c.refresh_tsc) > (ibv_timer_hz / 2)) {
    if (mlx5dv_get_clock_info(ctx, &c.info) == 0) {
      c.refresh_tsc = now;
      c.valid = true;
    }
  }
  return c.valid ? mlx5dv_ts_to_ns(&c.info, raw_ts) : 0;
}

Status IbverbsEngine::get_packet_rx_timestamp(BurstParams* burst, int idx, uint64_t* timestamp_ns) {
  if (burst == nullptr || timestamp_ns == nullptr) {
    return Status::NULL_PTR;
  }
  *timestamp_ns = 0;
  if (idx < 0 || idx >= static_cast<int>(burst->hdr.hdr.num_pkts)) {
    return Status::INVALID_PARAMETER;
  }
  // Reordered output bursts are GPU result buffers, not raw packets -- no
  // per-packet HW timestamp is carried.
  if (burst->hdr.hdr.burst_flags & DAQIRI_BURST_FLAG_REORDERED) {
    return Status::NOT_SUPPORTED;
  }
  IbvRxQueue* q = find_rx_queue(burst->hdr.hdr.port_id, burst->hdr.hdr.q_id);
  if (q == nullptr) {
    return Status::INVALID_PARAMETER;
  }
  *timestamp_ns = ts_to_ns(q->ctx, burst_ts_arr(burst)[idx]);
  return Status::SUCCESS;
}

void* IbverbsEngine::get_packet_extra_info(BurstParams* burst, int idx) {
  (void)burst;
  (void)idx;
  return nullptr;
}

uint64_t IbverbsEngine::get_burst_tot_byte(BurstParams* burst) {
  uint64_t tot = 0;
  for (size_t i = 0; i < burst->hdr.hdr.num_pkts; i++) {
    tot += burst->pkt_lens[0][i];
  }
  return tot;
}

uint16_t IbverbsEngine::get_num_rx_queues(int port_id) const {
  uint16_t n = 0;
  for (const auto& q : rx_queues_) {
    if (q->port_id == port_id) {
      ++n;
    }
  }
  return n;
}

std::string IbverbsEngine::port_netdev(int port) const {
  // Resolve port -> ibv device -> netdev. The RDMA device exposes its netdev
  // under /sys/class/infiniband/<dev>/device/net/<netdev>.
  struct ibv_context* ctx = nullptr;
  for (const auto& q : rx_queues_) {
    if (q->port_id == port) {
      ctx = q->ctx;
      break;
    }
  }
  if (ctx == nullptr) {
    for (const auto& q : tx_queues_) {
      if (q->port_id == port) {
        ctx = q->ctx;
        break;
      }
    }
  }
  if (ctx == nullptr) {
    return "";
  }
  const std::string net_dir =
      "/sys/class/infiniband/" + std::string(ibv_get_device_name(ctx->device)) + "/device/net";
  std::string netdev;
  if (DIR* d = opendir(net_dir.c_str())) {
    for (struct dirent* e = readdir(d); e != nullptr; e = readdir(d)) {
      if (e->d_name[0] != '.') {
        netdev = e->d_name;
        break;
      }
    }
    closedir(d);
  }
  return netdev;
}

void IbverbsEngine::ensure_port_mtus() {
  constexpr unsigned kEthHdr = 14;    // L2 header bytes excluded from the MTU
  constexpr unsigned kStdMtu = 1500;  // never lower below the standard MTU
  for (const auto& intf : cfg_.ifs_) {
    // Largest L2 frame the NIC may deliver = max over queues of the sum of the
    // queue's memory-region buffer sizes (HDS spreads one frame across regions).
    size_t max_frame = 0;
    auto accumulate = [&](const CommonQueueConfig& c) {
      size_t sum = 0;
      for (const auto& name : c.mrs_) {
        auto it = cfg_.mrs_.find(name);
        if (it != cfg_.mrs_.end()) {
          sum += it->second.buf_size_;
        }
      }
      max_frame = std::max(max_frame, sum);
    };
    for (const auto& q : intf.rx_.queues_) {
      accumulate(q.common_);
    }
    for (const auto& q : intf.tx_.queues_) {
      accumulate(q.common_);
    }
    max_frame += flow_max_decap_wire_overhead(intf.rx_.flows_);
    if (max_frame <= kStdMtu + kEthHdr) {
      continue;
    }  // standard MTU already fits

    const unsigned need = static_cast<unsigned>(max_frame) - kEthHdr;
    const std::string netdev = port_netdev(intf.port_id_);
    if (netdev.empty()) {
      DAQIRI_LOG_WARN("MTU: no netdev for port {}; cannot raise MTU (need {}) -- jumbo RX may drop",
                      intf.port_id_, need);
      continue;
    }
    // Read the current MTU; only raise it (never shrink, to avoid disturbing
    // other traffic on the interface).
    unsigned cur = 0;
    const std::string mtu_path = "/sys/class/net/" + netdev + "/mtu";
    if (FILE* f = fopen(mtu_path.c_str(), "r")) {
      if (fscanf(f, "%u", &cur) != 1) {
        cur = 0;
      }
      fclose(f);
    }
    if (cur >= need) {
      continue;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
      DAQIRI_LOG_WARN("MTU: socket() failed ({}); cannot raise {} MTU to {}", strerror(errno),
                      netdev, need);
      continue;
    }
    struct ifreq ifr {};
    strncpy(ifr.ifr_name, netdev.c_str(), IFNAMSIZ - 1);
    ifr.ifr_mtu = static_cast<int>(need);
    if (ioctl(fd, SIOCSIFMTU, &ifr) != 0) {
      DAQIRI_LOG_WARN(
          "MTU: could not raise {} MTU {}->{} ({}); jumbo RX may silently drop. Set it manually "
          "(ip link set {} mtu {}) or run with privileges.",
          netdev, cur, need, strerror(errno), netdev, need);
    } else {
      DAQIRI_LOG_INFO("MTU: raised {} (port {}) from {} to {} for max frame {} B", netdev,
                      intf.port_id_, cur, need, max_frame);
    }
    close(fd);
  }
}

Status IbverbsEngine::get_mac_addr(int port, char* mac) {
  // Resolve port -> ibv device -> netdev -> sysfs MAC.
  const std::string netdev = port_netdev(port);
  if (netdev.empty()) {
    DAQIRI_LOG_ERROR("get_mac_addr: no netdev for port {}", port);
    return Status::GENERIC_FAILURE;
  }
  const std::string addr_path = "/sys/class/net/" + netdev + "/address";
  FILE* f = fopen(addr_path.c_str(), "r");
  if (f == nullptr) {
    DAQIRI_LOG_ERROR("get_mac_addr: cannot open {}", addr_path);
    return Status::GENERIC_FAILURE;
  }
  unsigned int b[6] = {0};
  const int n = fscanf(f, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
  fclose(f);
  if (n != 6) {
    DAQIRI_LOG_ERROR("get_mac_addr: failed to parse {}", addr_path);
    return Status::GENERIC_FAILURE;
  }
  for (int i = 0; i < 6; i++) {
    mac[i] = static_cast<char>(b[i]);
  }
  return Status::SUCCESS;
}

Status IbverbsEngine::create_dynamic_flow_locked(int port,
                                                 const FlowRuleConfig& flow,
                                                 FlowId flow_id) {
  const InterfaceConfig* intf = find_interface_config(port);
  if (intf == nullptr) { return Status::INVALID_PARAMETER; }

  auto steering_it = port_steering_.find(port);
  if (steering_it == port_steering_.end()) { return Status::NOT_READY; }

  PortSteering& steering = steering_it->second;
  DynamicFlowEntry entry;
  entry.flow_id = flow_id;
  const int priority = steering.next_dynamic_priority++;
  const Status status =
      install_flow_rule_locked(port, steering, *intf, flow, flow_id, priority, &entry);
  if (status != Status::SUCCESS) { return status; }

  dynamic_flows_[flow_id] = entry;
  return Status::SUCCESS;
}

void IbverbsEngine::destroy_dynamic_flow_entry_locked(DynamicFlowEntry& entry) {
  if (entry.rule != nullptr) {
    mlx5dv_dr_rule_destroy(entry.rule);
    entry.rule = nullptr;
  }
  if (entry.tag_action != nullptr) {
    mlx5dv_dr_action_destroy(entry.tag_action);
    entry.tag_action = nullptr;
  }
  for (auto* reformat : entry.reformat_actions) {
    if (reformat != nullptr) { mlx5dv_dr_action_destroy(reformat); }
  }
  entry.reformat_actions.clear();
  entry.reformat_buffers.clear();
  if (entry.matcher != nullptr) {
    mlx5dv_dr_matcher_destroy(entry.matcher);
    entry.matcher = nullptr;
  }
}

void IbverbsEngine::cleanup_dynamic_flows_locked() {
  for (auto& [flow_id, entry] : dynamic_flows_) { destroy_dynamic_flow_entry_locked(entry); }
  dynamic_flows_.clear();
  while (!ready_flow_ops_.empty()) { ready_flow_ops_.pop(); }
}

void IbverbsEngine::enqueue_flow_completion_locked(const FlowOpResult& result) {
  ready_flow_ops_.push(result);
}

Status IbverbsEngine::add_rx_flow_async(int port, const FlowRuleConfig& flow, FlowOpId* op_id) {
  if (op_id == nullptr) { return Status::NULL_PTR; }
  *op_id = 0;

  std::lock_guard<std::mutex> guard(flow_lock_);
  if (!validate_dynamic_rx_flow_locked(port, flow)) { return Status::INVALID_PARAMETER; }

  const FlowId flow_id = allocate_dynamic_flow_id_locked();
  if (flow_id == 0) { return Status::NO_SPACE_AVAILABLE; }

  const FlowOpId new_op_id = allocate_flow_op_id_locked();
  *op_id = new_op_id;

  FlowOpResult result;
  result.op_id_ = new_op_id;
  result.type_ = FlowOpType::ADD_RX;
  result.flow_id_ = flow_id;
  result.flow_ids_ = {flow_id};
  result.status_ = create_dynamic_flow_locked(port, flow, flow_id);
  if (result.status_ != Status::SUCCESS) {
    result.flow_id_ = 0;
    result.flow_ids_[0] = 0;
    release_dynamic_flow_id_locked(flow_id);
  }
  enqueue_flow_completion_locked(result);
  return Status::SUCCESS;
}

Status IbverbsEngine::add_rx_flows_async(int port,
                                         const std::vector<FlowRuleConfig>& flows,
                                         FlowOpId* op_id) {
  if (op_id == nullptr) { return Status::NULL_PTR; }
  *op_id = 0;
  if (flows.empty()) { return Status::INVALID_PARAMETER; }

  std::lock_guard<std::mutex> guard(flow_lock_);
  for (const auto& flow : flows) {
    if (!validate_dynamic_rx_flow_locked(port, flow)) { return Status::INVALID_PARAMETER; }
  }
  if (!has_dynamic_flow_id_capacity_locked(flows.size())) { return Status::NO_SPACE_AVAILABLE; }

  std::vector<FlowId> flow_ids;
  flow_ids.reserve(flows.size());
  for (size_t i = 0; i < flows.size(); ++i) {
    const FlowId flow_id = allocate_dynamic_flow_id_locked();
    if (flow_id == 0) {
      for (const FlowId allocated_flow_id : flow_ids) {
        release_dynamic_flow_id_locked(allocated_flow_id);
      }
      return Status::NO_SPACE_AVAILABLE;
    }
    flow_ids.push_back(flow_id);
  }

  const FlowOpId new_op_id = allocate_flow_op_id_locked();
  *op_id = new_op_id;

  FlowOpResult result;
  result.op_id_ = new_op_id;
  result.type_ = FlowOpType::ADD_RX_BATCH;
  result.status_ = Status::SUCCESS;
  result.flow_ids_ = flow_ids;

  for (size_t i = 0; i < flows.size(); ++i) {
    const Status status = create_dynamic_flow_locked(port, flows[i], flow_ids[i]);
    if (status != Status::SUCCESS) {
      result.status_ = status;
      result.flow_ids_[i] = 0;
      release_dynamic_flow_id_locked(flow_ids[i]);
    }
  }

  enqueue_flow_completion_locked(result);
  return Status::SUCCESS;
}

Status IbverbsEngine::delete_flow_async(FlowId flow_id, FlowOpId* op_id) {
  if (op_id == nullptr) { return Status::NULL_PTR; }
  *op_id = 0;

  std::lock_guard<std::mutex> guard(flow_lock_);
  if (!initialized_) { return Status::NOT_READY; }
  if (flow_id == 0 || static_flow_ids_.find(flow_id) != static_flow_ids_.end()) {
    return Status::INVALID_PARAMETER;
  }

  auto flow_it = dynamic_flows_.find(flow_id);
  if (flow_it == dynamic_flows_.end() || flow_it->second.state != DynamicFlowState::ACTIVE) {
    return Status::INVALID_PARAMETER;
  }

  const FlowOpId new_op_id = allocate_flow_op_id_locked();
  *op_id = new_op_id;
  flow_it->second.state = DynamicFlowState::DELETING;
  destroy_dynamic_flow_entry_locked(flow_it->second);
  dynamic_flows_.erase(flow_it);
  release_dynamic_flow_id_locked(flow_id);

  FlowOpResult result;
  result.op_id_ = new_op_id;
  result.type_ = FlowOpType::DELETE;
  result.status_ = Status::SUCCESS;
  result.flow_id_ = flow_id;
  result.flow_ids_ = {flow_id};
  enqueue_flow_completion_locked(result);
  return Status::SUCCESS;
}

Status IbverbsEngine::poll_flow_op(FlowOpResult* result) {
  if (result == nullptr) { return Status::NULL_PTR; }

  std::lock_guard<std::mutex> guard(flow_lock_);
  if (ready_flow_ops_.empty()) { return Status::NOT_READY; }
  *result = ready_flow_ops_.front();
  ready_flow_ops_.pop();
  return Status::SUCCESS;
}

bool IbverbsEngine::validate_config() const {
  return Engine::validate_config();
}

void IbverbsEngine::print_stats() {
  for (auto& q : rx_queues_) {
    DAQIRI_LOG_INFO(
        "ibverbs RX port {} q{}: packets={} fillers={} cqe_errors={} reposts={} "
        "app_ring_full_drops={} bursts ({} pkts)",
        q->port_id, q->queue_id, q->dbg_data, q->dbg_filler, q->dbg_err, q->reposts.load(),
        q->ring_full_bursts, q->ring_full_pkts);
  }
  for (auto& q : tx_queues_) {
    // completed_tail is the authoritative uint64 transmitted+completed count;
    // sq_pi is the 32-bit producer, so in-flight is a modular-32 subtraction
    // (in flight is bounded by num_slots, so this is exact).
    const uint64_t completed = q->completed_tail.load(std::memory_order_relaxed);
    const uint32_t inflight = q->sq_pi - static_cast<uint32_t>(completed);
    DAQIRI_LOG_INFO(
        "ibverbs TX port {} q{}: transmitted={} inflight={} handoff_full_drops={} bursts ({} pkts)",
        q->port_id, q->queue_id, completed, inflight, q->handoff_drop_bursts, q->handoff_drop_pkts);
  }
}

void IbverbsEngine::shutdown() {
  force_quit_.store(true, std::memory_order_relaxed);
  for (auto& q : rx_queues_) {
    q->running = false;
    if (q->worker.joinable()) {
      q->worker.join();
    }
  }
  for (auto& q : tx_queues_) {
    q->running = false;
    if (q->compl_worker.joinable()) {
      q->compl_worker.join();
    }
  }
  {
    std::lock_guard<std::mutex> guard(flow_lock_);
    cleanup_dynamic_flows_locked();
  }
  // Tear down per-port flow steering first: rules reference the queues' TIRs/
  // actions, so they must go before devx_destroy frees those.
  for (auto& [port, st] : port_steering_) {
    for (auto* r : st.rules) {
      if (r) {
        mlx5dv_dr_rule_destroy(r);
      }
    }
    for (auto* a : st.tag_actions) {
      if (a) {
        mlx5dv_dr_action_destroy(a);
      }
    }
    for (auto* a : st.reformat_actions) {
      if (a) {
        mlx5dv_dr_action_destroy(a);
      }
    }
    for (auto* m : st.matchers) {
      if (m) {
        mlx5dv_dr_matcher_destroy(m);
      }
    }
    if (st.table) {
      mlx5dv_dr_table_destroy(st.table);
    }
    if (st.domain) {
      mlx5dv_dr_domain_destroy(st.domain);
    }
    // Flex-parser nodes referenced by misc4 matchers -- destroy after the rules.
    for (auto& [id, fn] : st.flex_nodes) {
      if (fn.obj) {
        mlx5dv_devx_obj_destroy(fn.obj);
      }
    }
    if (st.ipv4_len_node.obj) {
      mlx5dv_devx_obj_destroy(st.ipv4_len_node.obj);
    }
    if (st.ecpri_node.obj) {
      mlx5dv_devx_obj_destroy(st.ecpri_node.obj);
    }
  }
  port_steering_.clear();

  for (auto& [port, st] : tx_port_steering_) {
    for (auto* r : st.rules) {
      if (r) {
        mlx5dv_dr_rule_destroy(r);
      }
    }
    for (auto* a : st.reformat_actions) {
      if (a) {
        mlx5dv_dr_action_destroy(a);
      }
    }
    for (auto* m : st.matchers) {
      if (m) {
        mlx5dv_dr_matcher_destroy(m);
      }
    }
    if (st.table) {
      mlx5dv_dr_table_destroy(st.table);
    }
    if (st.domain) {
      mlx5dv_dr_domain_destroy(st.domain);
    }
  }
  tx_port_steering_.clear();

  for (auto& q : rx_queues_) {
    reorder_cleanup(*q);
    devx_destroy(*q);
    if (q->qp) {
      ibv_destroy_qp(q->qp);
    }
    if (q->ind_table) {
      ibv_destroy_rwq_ind_table(q->ind_table);
    }
    if (q->wq) {
      ibv_destroy_wq(q->wq);
    }
    if (q->cq) {
      ibv_destroy_cq(q->cq);
    }
    if (q->ring) {
      daqiri::Ring::free(q->ring);
    }
  }
  rx_queues_.clear();
  for (auto& q : tx_queues_) {
    if (q->qp) {
      ibv_destroy_qp(q->qp);
    }
    if (q->cq) {
      ibv_destroy_cq(q->cq);
    }
    if (q->send_ring) {
      daqiri::Ring::free(q->send_ring);
    }
  }
  tx_queues_.clear();

  for (auto it = registered_mrs_.rbegin(); it != registered_mrs_.rend(); ++it) {
    if (*it != nullptr && ibv_dereg_mr(*it) != 0) {
      DAQIRI_LOG_ERROR("ibv_dereg_mr failed during ibverbs shutdown: {}", strerror(errno));
    }
  }
  registered_mrs_.clear();

  for (auto& pd : pd_map_) {
    if (pd.second && ibv_dealloc_pd(pd.second) != 0) {
      DAQIRI_LOG_ERROR("ibv_dealloc_pd failed during ibverbs shutdown: {}", strerror(errno));
    }
  }
  pd_map_.clear();
  clock_cache_.clear();
  for (auto& c : ctx_map_) {
    if (c.second && ibv_close_device(c.second) != 0) {
      DAQIRI_LOG_ERROR("ibv_close_device failed during ibverbs shutdown: {}", strerror(errno));
    }
  }
  ctx_map_.clear();
  if (rx_meta_pool_) {
    daqiri::ObjectPool::free(rx_meta_pool_);
    rx_meta_pool_ = nullptr;
  }
  if (tx_meta_pool_) {
    daqiri::ObjectPool::free(tx_meta_pool_);
    tx_meta_pool_ = nullptr;
  }
  initialized_ = false;
  force_quit_.store(false, std::memory_order_relaxed);
  max_batch_ = 0;
  rx_meta_pool_size_ = 0;
}

// ---------------------------------------------------------------------------
// TX path: raw-packet QP send from a slab of registered slots.
// ---------------------------------------------------------------------------
IbvTxQueue* IbverbsEngine::find_tx_queue(int port, int q) {
  for (auto& tq : tx_queues_) {
    if (tq->port_id == port && tq->queue_id == q) {
      return tq.get();
    }
  }
  return nullptr;
}

Status IbverbsEngine::create_tx_raw_qp(IbvTxQueue& q) {
  q.cq = ibv_create_cq(q.ctx, static_cast<int>(q.num_slots) + 1, nullptr, nullptr, 0);
  if (q.cq == nullptr) {
    DAQIRI_LOG_CRITICAL("TX ibv_create_cq failed: {}", strerror(errno));
    return Status::GENERIC_FAILURE;
  }
  struct ibv_qp_init_attr attr {};
  attr.qp_type = IBV_QPT_RAW_PACKET;
  attr.send_cq = q.cq;
  attr.recv_cq = q.cq;
  // Room for 2 WQEBBs per slot: a scheduled packet emits a WAIT WQE before its
  // send WQE, so the SQ must hold up to 2x num_slots WQEs in flight.
  attr.cap.max_send_wr = q.num_slots * 2;
  attr.cap.max_send_sge = MAX_NUM_SEGS;
  attr.cap.max_recv_wr = 1;
  attr.cap.max_recv_sge = 1;
  q.qp = ibv_create_qp(q.pd, &attr);
  if (q.qp == nullptr) {
    DAQIRI_LOG_CRITICAL("TX ibv_create_qp (RAW_PACKET) failed: {}", strerror(errno));
    return Status::GENERIC_FAILURE;
  }
  // RESET -> INIT -> RTR -> RTS.
  struct ibv_qp_attr m {};
  m.qp_state = IBV_QPS_INIT;
  m.port_num = 1;
  if (ibv_modify_qp(q.qp, &m, IBV_QP_STATE | IBV_QP_PORT) != 0) {
    DAQIRI_LOG_CRITICAL("TX QP -> INIT failed: {}", strerror(errno));
    return Status::GENERIC_FAILURE;
  }
  memset(&m, 0, sizeof(m));
  m.qp_state = IBV_QPS_RTR;
  if (ibv_modify_qp(q.qp, &m, IBV_QP_STATE) != 0) {
    DAQIRI_LOG_CRITICAL("TX QP -> RTR failed: {}", strerror(errno));
    return Status::GENERIC_FAILURE;
  }
  memset(&m, 0, sizeof(m));
  m.qp_state = IBV_QPS_RTS;
  if (ibv_modify_qp(q.qp, &m, IBV_QP_STATE) != 0) {
    DAQIRI_LOG_CRITICAL("TX QP -> RTS failed: {}", strerror(errno));
    return Status::GENERIC_FAILURE;
  }
  // Map the SQ buffer + doorbell + BlueFlame register and the TX CQ buffer so
  // we can build send WQEs and ring the doorbell directly, bypassing
  // ibv_post_send's per-WR overhead (the same DevX-style technique used on RX).
  // We never call ibv_post_send/ibv_poll_cq on this QP/CQ again -- this thread
  // owns the SQ producer, the CQ consumer, and both doorbells.
  struct mlx5dv_obj obj {};
  obj.qp.in = q.qp;
  obj.qp.out = &q.dv_qp;
  obj.cq.in = q.cq;
  obj.cq.out = &q.dv_txcq;
  if (mlx5dv_init_obj(&obj, MLX5DV_OBJ_QP | MLX5DV_OBJ_CQ) != 0) {
    DAQIRI_LOG_CRITICAL("TX mlx5dv_init_obj(QP|CQ) failed: {}", strerror(errno));
    return Status::GENERIC_FAILURE;
  }
  if (q.dv_qp.bf.reg == nullptr) {
    DAQIRI_LOG_CRITICAL("TX SQ has no BlueFlame register");
    return Status::GENERIC_FAILURE;
  }
  // The send-WQE control segment carries the QP number (qp_num), not dv_qp.sqn
  // -- the latter is only populated for DevX-created SQs and reads back 0 here.
  q.sqn = q.qp->qp_num;
  q.sq_pi = 0;
  q.bf_offset = 0;
  q.tx_cq_ci = 0;
  q.alloc_head = 0;
  q.completed_tail.store(0, std::memory_order_relaxed);
  q.slots_posted = 0;
  q.wqe_slot_cum.assign(q.dv_qp.sq.wqe_cnt, 0);
  DAQIRI_LOG_INFO("TX SQ mapped q{}: sqn {}, wqe_cnt {}, stride {}, bf.size {}, cqe_cnt {}",
                  q.queue_id, q.sqn, q.dv_qp.sq.wqe_cnt, q.dv_qp.sq.stride, q.dv_qp.bf.size,
                  q.dv_txcq.cqe_cnt);
  return Status::SUCCESS;
}

Status IbverbsEngine::setup_tx_queue(IbvTxQueue& q, const InterfaceConfig& intf,
                                     const TxQueueConfig& qcfg) {
  q.port_id = intf.port_id_;
  q.queue_id = qcfg.common_.id_;
  q.batch_size = std::max(1, qcfg.common_.batch_size_);
  if (!qcfg.common_.cpu_core_.empty()) {
    q.cpu_core = std::stoi(qcfg.common_.cpu_core_);
  }
  for (const auto& off : qcfg.common_.offloads_) {
    if (off == "tx_eth_src") {
      q.insert_eth_src = true;
    }
  }
  // Packet pacing (pacing_mbps) is not supported on the ibverbs engine: the
  // wait-on-time WQE it would use is not honored without the mlx5 send-scheduling
  // clock/rearm-queue infrastructure that this engine does not set up, so a paced
  // queue would not meter and would eventually stall. Reject it up front rather
  // than silently running at line rate. Use the DPDK raw engine for pacing.
  if (qcfg.pacing_mbps_ > 0) {
    DAQIRI_LOG_CRITICAL(
        "TX queue {}: pacing_mbps is not supported by the ibverbs engine; use the default "
        "DPDK raw engine (remove engine: \"ibverbs\") for packet pacing",
        q.queue_id);
    return Status::INVALID_PARAMETER;
  }
  if (qcfg.common_.mrs_.empty()) {
    DAQIRI_LOG_CRITICAL("TX queue {} has no memory region", q.queue_id);
    return Status::INVALID_PARAMETER;
  }
  q.mr_names = qcfg.common_.mrs_;
  q.mr_name = qcfg.common_.mrs_[0];
  // >1 region = header-data split TX: each packet segment comes from its own MR
  // (region 0 = CPU header, region 1 = GPU payload), each with its own lkey.
  q.num_segs = std::min<int>(static_cast<int>(qcfg.common_.mrs_.size()), MAX_NUM_SEGS);

  q.ctx = open_device_for_interface(intf);
  if (q.ctx == nullptr) {
    return Status::GENERIC_FAILURE;
  }
  q.pd = pd_map_[q.ctx];

  // Register every MR as a TX scatter region; num_slots is the smallest region's
  // buffer count (a packet consumes slot i from every region).
  q.regions.clear();
  q.num_slots = UINT32_MAX;
  for (const std::string& name : q.mr_names) {
    IbvTxQueue::TxRegion r;
    if (Status s = register_mr(q.pd, name, &r.base, &r.lkey); s != Status::SUCCESS) {
      return s;
    }
    const auto& mr = cfg_.mrs_[name];
    r.slot_size = static_cast<uint32_t>(mr.adj_size_);
    q.regions.push_back(r);
    q.num_slots = std::min<uint32_t>(q.num_slots, static_cast<uint32_t>(mr.num_bufs_));
  }
  q.mr_base = q.regions[0].base;
  q.lkey = q.regions[0].lkey;
  q.slot_size = q.regions[0].slot_size;
  if (Status s = create_tx_raw_qp(q); s != Status::SUCCESS) {
    return s;
  }

  // Hand-off ring: the app fill thread enqueues filled bursts; the pinned TX
  // worker dequeues and does the WQE build + doorbell + completion reclaim.
  const std::string send_name =
      "ibv_txsend_" + std::to_string(q.port_id) + "_" + std::to_string(q.queue_id);
  q.send_ring = daqiri::Ring::create(send_name.c_str(), 4096, daqiri::RingMode::SPSC,
                                     daqiri::detail::numa_node_for_cpu(cfg_.common_.master_core_));
  if (q.send_ring == nullptr) {
    DAQIRI_LOG_CRITICAL("Failed to create TX send ring {}", send_name);
    return Status::GENERIC_FAILURE;
  }
  DAQIRI_LOG_INFO("TX queue {} ready: {} slots of {}B, qp {} (mr {})", q.queue_id, q.num_slots,
                  q.slot_size, (void*)q.qp, q.mr_name);
  return Status::SUCCESS;
}

// Drain the TX CQ and reclaim completed slot-runs back to the free ring. Runs
// inline on the application's TX thread (the DPDK model: the sender reclaims
// its own completions), so the whole TX slot lifecycle stays single-threaded
// and contention-free. Each signaled CQE's wqe_counter is the SQ WQE index (one
// WQEBB / packet) of the signaled WQE; everything up to and including it is done.
void IbverbsEngine::poll_tx_completions(IbvTxQueue& q) {
  uint8_t* const cq_buf = static_cast<uint8_t*>(q.dv_txcq.buf);
  const uint32_t cqe_cnt = q.dv_txcq.cqe_cnt;
  const uint32_t cqe_size = q.dv_txcq.cqe_size;
  const uint32_t wqe_cnt = q.dv_qp.sq.wqe_cnt;
  uint64_t tail = q.completed_tail.load(std::memory_order_relaxed);
  bool any = false;
  for (;;) {
    uint8_t* cqe = cq_buf + (q.tx_cq_ci & (cqe_cnt - 1)) * cqe_size;
    auto* cqe64 =
        reinterpret_cast<struct mlx5_cqe64*>(cqe + (cqe_size - sizeof(struct mlx5_cqe64)));
    const uint8_t op_own = cqe64->op_own;
    const uint8_t opcode = op_own >> 4;
    const uint8_t owner = op_own & 1;
    const uint8_t phase = (q.tx_cq_ci / cqe_cnt) & 1;
    if (opcode == MLX5_CQE_INVALID || owner != phase) {
      break;
    }
    cqe_read_barrier();
    if (opcode == MLX5_CQE_REQ_ERR || opcode == MLX5_CQE_RESP_ERR) {
      DAQIRI_LOG_ERROR("TX CQE error q{} opcode {}", q.queue_id, opcode);
    }
    // wqe_counter is the signaled send WQE's WQEBB index; wqe_slot_cum maps it
    // to the cumulative slot count posted through it (accounting for interleaved
    // WAIT WQEs, which consume a WQEBB but no slot). Advancing completed_tail to
    // that count frees the run of cyclic slots in one step -- no ring ops.
    const uint16_t wc_idx = be16toh(cqe64->wqe_counter);
    tail = q.wqe_slot_cum[wc_idx & (wqe_cnt - 1)];
    q.tx_cq_ci++;
    any = true;
  }
  if (any) {
    q.completed_tail.store(tail, std::memory_order_release);
    doorbell_store_barrier();
    *q.dv_txcq.dbrec = htobe32(q.tx_cq_ci & 0xffffff);
  }
}

// Pinned TX worker: drains the hand-off ring (the cheap DevX WQE build + doorbell
// runs here, on a dedicated core, so the application's fill thread is free to keep
// filling) and reclaims completed slots. Splitting fill and post across two cores
// beats doing both inline -- the doorbell/CQE work overlaps the next fill batch.
void IbverbsEngine::tx_worker(std::vector<IbvTxQueue*> group) {
  if (group.empty()) {
    return;
  }
  IbvTxQueue* leader = group[0];
  if (leader->cpu_core >= 0) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(leader->cpu_core, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
  }
  while (leader->running.load(std::memory_order_relaxed) &&
         !force_quit_.load(std::memory_order_relaxed)) {
    for (auto* q : group) {
      void* b = nullptr;
      while (q->send_ring->dequeue(&b)) {
        auto* burst = static_cast<BurstParams*>(b);
        post_tx_burst(*q, burst);
        tx_meta_pool_->put(burst);
      }
      poll_tx_completions(*q);
    }
  }
}

BurstParams* IbverbsEngine::create_tx_burst_params() {
  BurstParams* burst = nullptr;
  if (!tx_meta_pool_->get(reinterpret_cast<void**>(&burst))) {
    return nullptr;
  }
  burst->hdr.hdr.num_pkts = 0;
  burst->hdr.hdr.num_segs = 1;
  burst->hdr.hdr.burst_flags = IBV_TX_BURST_FLAG;
  burst->pkts[0] = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(burst) + g_layout.off_pkts0);
  burst->pkt_lens[0] =
      reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(burst) + g_layout.off_lens0);
  burst->pkts[1] = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(burst) + g_layout.off_pkts1);
  burst->pkt_lens[1] =
      reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(burst) + g_layout.off_lens1);
  return burst;
}

Status IbverbsEngine::get_tx_metadata_buffer(BurstParams** burst) {
  *burst = create_tx_burst_params();
  return *burst ? Status::SUCCESS : Status::NO_FREE_BURST_BUFFERS;
}

bool IbverbsEngine::is_tx_burst_available(BurstParams* burst) {
  IbvTxQueue* q = find_tx_queue(burst->hdr.hdr.port_id, burst->hdr.hdr.q_id);
  if (q == nullptr) {
    return false;
  }
  const uint64_t in_flight = q->alloc_head - q->completed_tail.load(std::memory_order_acquire);
  return (q->num_slots - in_flight) >= static_cast<uint64_t>(burst->hdr.hdr.num_pkts);
}

Status IbverbsEngine::get_tx_packet_burst(BurstParams* burst) {
  IbvTxQueue* q = find_tx_queue(burst->hdr.hdr.port_id, burst->hdr.hdr.q_id);
  if (q == nullptr) {
    return Status::INVALID_PARAMETER;
  }
  const unsigned n = static_cast<unsigned>(burst->hdr.hdr.num_pkts);
  burst->hdr.hdr.num_segs = q->num_segs;
  // Hand out the next n cyclic slots if they fit under num_slots in flight. No
  // ring ops: each slot pointer is computed from the monotonic alloc_head, and
  // the TX worker frees them by advancing completed_tail as completions arrive.
  const uint64_t in_flight = q->alloc_head - q->completed_tail.load(std::memory_order_acquire);
  if (q->num_slots - in_flight < n) {
    return Status::NO_FREE_PACKET_BUFFERS;
  }
  // Each packet takes slot idx from every region. Single-region: only segment 0.
  // HDS: segment 0 = CPU header buffer, segment 1 = GPU payload buffer (separate
  // MRs, NOT contiguous), so post_tx_burst scatters each from its own region.
  for (unsigned i = 0; i < n; i++) {
    const uint64_t idx = (q->alloc_head + i) % q->num_slots;
    for (int s = 0; s < q->num_segs; s++) {
      burst->pkts[s][i] = q->regions[s].base + idx * q->regions[s].slot_size;
    }
  }
  q->alloc_head += n;
  // Clear per-packet send times; set_packet_tx_time fills the scheduled ones.
  if (q->send_scheduling) {
    memset(burst_ts_arr(burst), 0, n * sizeof(uint64_t));
  }
  return Status::SUCCESS;
}

Status IbverbsEngine::set_packet_lengths(BurstParams* burst, int idx,
                                         const std::initializer_list<int>& lens) {
  int seg = 0;
  for (int v : lens) {
    if (seg >= MAX_NUM_SEGS) {
      break;
    }
    burst->pkt_lens[seg][idx] = static_cast<uint32_t>(v);
    ++seg;
  }
  return Status::SUCCESS;
}

Status IbverbsEngine::set_eth_header(BurstParams* burst, int idx, char* dst_addr) {
  auto* pkt = static_cast<UDPIPV4Pkt*>(burst->pkts[0][idx]);
  memcpy(pkt->eth.h_dest, dst_addr, 6);
  pkt->eth.h_proto = htons(0x0800);  // IPv4
  // tx_eth_src offload: stamp the port MAC as the Ethernet source.
  IbvTxQueue* q = find_tx_queue(burst->hdr.hdr.port_id, burst->hdr.hdr.q_id);
  if (q != nullptr && q->insert_eth_src) {
    memcpy(pkt->eth.h_source, q->eth_src, 6);
  }
  return Status::SUCCESS;
}

Status IbverbsEngine::set_ipv4_header(BurstParams* burst, int idx, int ip_len, uint8_t proto,
                                      unsigned int src_host, unsigned int dst_host) {
  auto* pkt = static_cast<UDPIPV4Pkt*>(burst->pkts[0][idx]);
  pkt->ip.version = 4;
  pkt->ip.ihl = 5;
  pkt->ip.tos = 0;
  pkt->ip.tot_len = htons(static_cast<uint16_t>(ip_len));
  pkt->ip.protocol = proto;
  pkt->ip.saddr = src_host;
  pkt->ip.daddr = dst_host;
  return Status::SUCCESS;
}

Status IbverbsEngine::set_udp_header(BurstParams* burst, int idx, int udp_len, uint16_t src_port,
                                     uint16_t dst_port) {
  auto* pkt = static_cast<UDPIPV4Pkt*>(burst->pkts[0][idx]);
  pkt->udp.source = htons(src_port);
  pkt->udp.dest = htons(dst_port);
  pkt->udp.len = htons(static_cast<uint16_t>(udp_len));
  pkt->udp.check = 0;
  return Status::SUCCESS;
}

Status IbverbsEngine::set_udp_payload(BurstParams* burst, int idx, void* data, int len) {
  auto* p = static_cast<uint8_t*>(burst->pkts[0][idx]) + sizeof(UDPIPV4Pkt);
  memcpy(p, data, static_cast<size_t>(len));
  return Status::SUCCESS;
}

// Accurate send scheduling: hold this packet at the NIC until `time`. `time` is
// nanoseconds in the device's hardware clock domain -- the SAME domain as
// get_packet_rx_timestamp -- readable as "now" via ibv_query_rt_values_ex. The
// hardware compares over an ~8-second cyclic window, so a scheduled time must be
// within ~8 s of the current clock (a time too far ahead never releases; one too
// far in the past wraps). Returns NOT_SUPPORTED when the device lacks wait-on-time
// or a real-time clock.
Status IbverbsEngine::set_packet_tx_time(BurstParams* burst, int idx, uint64_t time) {
  IbvTxQueue* q = find_tx_queue(burst->hdr.hdr.port_id, burst->hdr.hdr.q_id);
  if (q == nullptr) {
    return Status::INVALID_PARAMETER;
  }
  if (!q->send_scheduling) {
    return Status::NOT_SUPPORTED;
  }
  // Stored in the burst's per-packet timestamp array; post_tx_burst emits a
  // WAIT-on-time WQE for any packet whose time is non-zero.
  burst_ts_arr(burst)[idx] = time;
  return Status::SUCCESS;
}

// Build a WAIT-on-time WQE (ctrl + wseg = 48 B = 1 WQEBB) at q.sq_pi that holds
// the following send(s) until the NIC real-time clock reaches when_ns. The WAIT
// WQE consumes no slot; wqe_slot_cum records the cumulative slot count per WQEBB
// so completion can map a CQE's wqe_counter back to slots. Advances sq_pi and
// returns the ctrl segment (the caller writes the burst's last ctrl to BlueFlame).
void* IbverbsEngine::emit_wait_wqe(IbvTxQueue& q, uint64_t when_ns) {
  uint8_t* const sq_buf = static_cast<uint8_t*>(q.dv_qp.sq.buf);
  const uint32_t wqe_cnt = q.dv_qp.sq.wqe_cnt;
  const uint32_t stride = q.dv_qp.sq.stride;
  const uint32_t widx = q.sq_pi % wqe_cnt;
  uint8_t* wbase = sq_buf + static_cast<size_t>(widx) * stride;
  auto* wctrl = reinterpret_cast<struct mlx5_wqe_ctrl_seg*>(wbase);
  wctrl->opmod_idx_opcode = htobe32((static_cast<uint32_t>(MLX5_OPC_MOD_WAIT_TIME) << 24) |
                                    ((q.sq_pi & 0xffff) << 8) | MLX5_OPCODE_WAIT);
  wctrl->qpn_ds = htobe32((q.sqn << 8) | 3u);  // ctrl(1) + wseg(2) = 3 DS
  wctrl->signature = 0;
  wctrl->dci_stream_channel_id = 0;
  wctrl->fm_ce_se = 0;  // unsignaled
  wctrl->imm = 0;
  auto* ws = reinterpret_cast<struct mlx5_wqe_wseg*>(wbase + 16);
  ws->operation = htobe32(MLX5_WAIT_COND_CYCLIC_SMALLER);
  ws->lkey = 0;
  ws->va_high = 0;
  ws->va_low = 0;
  // The HW real-time clock compares in UTC format (seconds<<32 | ns), so the
  // linear nanoseconds (same domain as get_packet_rx_timestamp) are split here.
  const uint64_t v = (when_ns % 1000000000ULL) | ((when_ns / 1000000000ULL) << 32);
  ws->value = htobe64(v);
  ws->mask = htobe64(q.rt_timemask);
  q.wqe_slot_cum[widx] = q.slots_posted;
  q.sq_pi++;
  return wctrl;
}

// Runs on the pinned TX worker thread: builds the burst's send WQEs directly
// into the SQ ring and rings the BlueFlame doorbell once for the whole burst,
// bypassing ibv_post_send. Each WQE is ctrl(16) + minimal eth(16) + data
// segs(16 each); with <=2 segments it is exactly one 64B WQEBB, so the SQ
// producer (and the CQE wqe_counter) advances by one per packet. Signals every
// SIGNAL_EVERY-th WQE (and the last) for completion-driven slot reclaim.
// Does NOT free the metadata block (the caller in tx_worker does).
void IbverbsEngine::post_tx_burst(IbvTxQueue& q, BurstParams* burst) {
  const int n = static_cast<int>(burst->hdr.hdr.num_pkts);
  const int segs = burst->hdr.hdr.num_segs;
  if (n <= 0) {
    return;
  }
  static constexpr int SIGNAL_EVERY = 32;
  uint8_t* const sq_buf = static_cast<uint8_t*>(q.dv_qp.sq.buf);
  const uint32_t wqe_cnt = q.dv_qp.sq.wqe_cnt;
  const uint32_t stride = q.dv_qp.sq.stride;          // 64 B (one WQEBB)
  const uint8_t ds = static_cast<uint8_t>(2 + segs);  // ctrl + eth + segs data
  // Per-packet send times (ns); 0 = send immediately. Reuses the burst's
  // timestamp array (a burst is either RX or TX, never both).
  const uint64_t* txtime = burst_ts_arr(burst);
  void* last_ctrl = nullptr;

  for (int i = 0; i < n; i++) {
    // Accurate send scheduling (per-packet): emit a WAIT-on-time WQE so the NIC
    // holds the following send until its real-time clock reaches txtime[i].
    if (q.send_scheduling && txtime[i] != 0) {
      last_ctrl = emit_wait_wqe(q, txtime[i]);
    }

    const uint32_t idx = q.sq_pi % wqe_cnt;
    uint8_t* seg = sq_buf + static_cast<size_t>(idx) * stride;
    auto* ctrl = reinterpret_cast<struct mlx5_wqe_ctrl_seg*>(seg);
    const bool signaled = ((i % SIGNAL_EVERY) == (SIGNAL_EVERY - 1)) || (i == n - 1);
    ctrl->opmod_idx_opcode = htobe32(((q.sq_pi & 0xffff) << 8) | MLX5_OPCODE_SEND);
    ctrl->qpn_ds = htobe32((q.sqn << 8) | ds);
    ctrl->signature = 0;
    ctrl->dci_stream_channel_id = 0;
    ctrl->fm_ce_se = signaled ? MLX5_WQE_CTRL_CQ_UPDATE : 0;
    ctrl->imm = 0;
    // Minimal (16-byte) Ethernet segment, no inline headers: the NIC DMAs the
    // whole frame from the data segment. Request IPv4 + L4 checksum offload so
    // the NIC fills the IP/UDP checksums (matches the DPDK backend, which has
    // checksum offload always on); the application need not compute them.
    memset(seg + 16, 0, 16);
    reinterpret_cast<struct mlx5_wqe_eth_seg*>(seg + 16)->cs_flags =
        MLX5_ETH_WQE_L3_CSUM | MLX5_ETH_WQE_L4_CSUM;
    auto* dseg = reinterpret_cast<struct mlx5_wqe_data_seg*>(seg + 32);
    for (int s = 0; s < segs; s++) {
      // Each segment uses its own region's lkey (region 0 = header/CPU MR,
      // region 1 = payload/GPU MR for HDS).
      dseg[s].byte_count = htobe32(burst->pkt_lens[s][i]);
      dseg[s].lkey = htobe32(q.regions[s].lkey);
      dseg[s].addr = htobe64(reinterpret_cast<uint64_t>(burst->pkts[s][i]));
    }
    q.slots_posted++;  // this packet consumes one slot
    q.wqe_slot_cum[idx] = q.slots_posted;
    last_ctrl = ctrl;
    q.sq_pi++;
  }

  // Ring the SQ doorbell once for the whole burst: publish the WQEs, bump the
  // doorbell record, then write the last ctrl segment to the BlueFlame register.
  doorbell_store_barrier();  // WQEs visible before the doorbell record
  q.dv_qp.dbrec[MLX5_SND_DBR] = htobe32(q.sq_pi & 0xffff);
  doorbell_store_barrier();  // doorbell record visible before the BF write
  *reinterpret_cast<volatile uint64_t*>(static_cast<uint8_t*>(q.dv_qp.bf.reg) + q.bf_offset) =
      *reinterpret_cast<uint64_t*>(last_ctrl);
  doorbell_store_barrier();
  q.bf_offset ^= q.dv_qp.bf.size;
}

// App fill thread: hand the filled burst to the pinned TX worker, which does
// the DevX WQE build + doorbell + completion reclaim on its own core. This
// overlaps the next fill batch with the post of the previous one and beat the
// single-thread inline model in measurement.
Status IbverbsEngine::send_tx_burst(BurstParams* burst) {
  IbvTxQueue* q = find_tx_queue(burst->hdr.hdr.port_id, burst->hdr.hdr.q_id);
  if (q == nullptr) {
    return Status::INVALID_PARAMETER;
  }
  if (!q->send_ring->enqueue(burst)) {
    // Worker is behind: roll back this (most-recent, unposted) allocation and
    // drop the metadata; caller moves on. Safe because get/send are paired on
    // the fill thread, so these are the last slots handed out and the worker
    // never saw them (they were never enqueued to send_ring).
    q->alloc_head -= static_cast<uint64_t>(burst->hdr.hdr.num_pkts);
    q->handoff_drop_bursts++;
    q->handoff_drop_pkts += static_cast<uint64_t>(burst->hdr.hdr.num_pkts);
    tx_meta_pool_->put(burst);
    return Status::NO_SPACE_AVAILABLE;
  }
  return Status::SUCCESS;
}

void IbverbsEngine::free_tx_burst(BurstParams* burst) {
  if (burst != nullptr) {
    tx_meta_pool_->put(burst);
  }
}

void IbverbsEngine::free_tx_metadata(BurstParams* burst) {
  if (burst != nullptr) {
    tx_meta_pool_->put(burst);
  }
}

// ---------------------------------------------------------------------------
// Flow control -- drop/allow implemented in the flow milestone.
// ---------------------------------------------------------------------------
Status IbverbsEngine::drop_all_traffic(int port) {
  // Destroying the port's steering rules stops delivery to our RQs (traffic
  // falls through to the kernel) -- the practical "drop" for a raw RX backend.
  // The matchers/specs persist so allow_all_traffic can recreate the rules.
  {
    std::lock_guard<std::mutex> guard(flow_lock_);
    const auto has_dynamic_flow_on_port = std::any_of(
        dynamic_flows_.begin(), dynamic_flows_.end(), [&](const auto& item) {
          return item.second.port == port && item.second.state == DynamicFlowState::ACTIVE;
        });
    if (has_dynamic_flow_on_port) {
      DAQIRI_LOG_ERROR("drop_all_traffic is not supported while port {} has dynamic RX flows", port);
      return Status::NOT_SUPPORTED;
    }
  }

  auto it = port_steering_.find(port);
  if (it == port_steering_.end()) {
    return Status::SUCCESS;
  }
  PortSteering& st = it->second;
  for (auto* r : st.rules) {
    if (r) {
      mlx5dv_dr_rule_destroy(r);
    }
  }
  st.rules.clear();
  st.dropped = true;
  return Status::SUCCESS;
}

Status IbverbsEngine::allow_all_traffic(int port) {
  auto it = port_steering_.find(port);
  if (it == port_steering_.end()) {
    return Status::SUCCESS;
  }
  PortSteering& st = it->second;
  if (!st.dropped) {
    return Status::SUCCESS;
  }
  for (auto& spec : st.rule_specs) {
    DrMatchParam val{};
    val.match_sz = spec.value_sz;
    memcpy(val.buf, spec.value, spec.value_sz);
    struct mlx5dv_dr_action* actions[8];
    int na = 0;
    if (spec.tag != nullptr) {
      actions[na++] = spec.tag;
    }
    for (auto* reformat : spec.reformats) {
      if (na >= static_cast<int>(sizeof(actions) / sizeof(actions[0])) - 1) {
        DAQIRI_LOG_ERROR("allow_all_traffic: too many actions on port {}", port);
        return Status::GENERIC_FAILURE;
      }
      actions[na++] = reformat;
    }
    actions[na++] = spec.action;
    struct mlx5dv_dr_rule* r = mlx5dv_dr_rule_create(
        spec.matcher, reinterpret_cast<struct mlx5dv_flow_match_parameters*>(&val), na, actions);
    if (r == nullptr) {
      DAQIRI_LOG_ERROR("allow_all_traffic: failed to recreate rule on port {}", port);
      return Status::GENERIC_FAILURE;
    }
    st.rules.push_back(r);
  }
  st.dropped = false;
  return Status::SUCCESS;
}

};  // namespace daqiri
