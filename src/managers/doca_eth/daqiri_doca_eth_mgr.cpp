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

#include "daqiri_doca_eth_mgr.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cuda.h>
#include <cuda_runtime.h>
#include <doca_bitfield.h>
#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_eth_rxq.h>
#include <doca_eth_rxq_cpu_data_path.h>
#include <doca_eth_txq.h>
#include <doca_eth_txq_cpu_data_path.h>
#include <doca_flow.h>
#include <doca_mmap.h>
#include <doca_pe.h>
#include <rte_common.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_malloc.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "src/logging.hpp"

namespace daqiri {
namespace {

using Clock = std::chrono::steady_clock;

static constexpr uint32_t kMaxFlowCounters = 1U << 16;
static constexpr size_t kMinRxSlotsPerQueue = 256;
static constexpr size_t kMaxRxSlotsPerQueue = 4096;
static constexpr uint32_t kBurstFlagTxContext = 1U << 0;
static constexpr uint32_t kBurstFlagRxContext = 1U << 1;

static inline uint32_t queue_key(uint16_t port_id, uint16_t queue_id) {
  return (static_cast<uint32_t>(port_id) << 16U) | static_cast<uint32_t>(queue_id);
}

static inline uint64_t now_ns() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
          .count());
}

static inline void log_doca_status(const char* prefix, doca_error_t status) {
  DAQIRI_LOG_ERROR("{}: {}", prefix, doca_error_get_descr(status));
}

static inline uint64_t align_up(uint64_t x, uint64_t align) {
  return ((x + align - 1U) / align) * align;
}

static inline bool looks_like_placeholder_bdf(const std::string& bdf) {
  return bdf.find('<') != std::string::npos || bdf.find('>') != std::string::npos;
}

struct DocaFlowPortState {
  struct doca_flow_port* flow_port = nullptr;
  struct doca_flow_pipe* root_pipe = nullptr;
  struct doca_flow_pipe_entry* default_entry = nullptr;
  bool drop_traffic = false;
  uint16_t default_flow_id = 0;
};

struct RxQueueState;
struct TxQueueState;

struct RxSlot {
  enum class State { IDLE, POSTED, COMPLETED, IN_APP, ERROR };

  RxQueueState* queue = nullptr;
  uint32_t slot_id = 0;
  std::vector<void*> seg_ptrs;
  std::vector<size_t> seg_caps;
  std::vector<struct doca_buf*> seg_bufs;
  struct doca_buf* head_buf = nullptr;
  std::array<uint32_t, MAX_NUM_SEGS> seg_lens{};
  uint32_t total_len = 0;
  uint16_t flow_id = 0;
  State state = State::IDLE;
};

struct TxSlot {
  TxQueueState* queue = nullptr;
  uint32_t slot_id = 0;
  std::vector<void*> seg_ptrs;
  std::vector<size_t> seg_caps;
  std::vector<struct doca_buf*> seg_bufs;
  struct doca_buf* head_buf = nullptr;
  std::array<uint32_t, MAX_NUM_SEGS> seg_lens{};
  int tx_status = 0;
};

struct RxQueueState {
  uint32_t key = 0;
  uint16_t port_id = 0;
  uint16_t queue_id = 0;
  uint32_t batch_size = 0;
  uint32_t num_segs = 0;
  uint32_t max_packet_size = 0;

  struct doca_pe* pe = nullptr;
  struct doca_eth_rxq* rxq = nullptr;
  struct doca_ctx* ctx = nullptr;

  std::vector<RxSlot> slots;
  std::deque<uint32_t> completed_slots;
  std::mutex lock;
};

struct TxQueueState {
  uint32_t key = 0;
  uint16_t port_id = 0;
  uint16_t queue_id = 0;
  uint32_t batch_size = 0;
  uint32_t num_segs = 0;

  struct doca_pe* pe = nullptr;
  struct doca_eth_txq* txq = nullptr;
  struct doca_ctx* ctx = nullptr;

  std::vector<TxSlot> slots;
  std::deque<uint32_t> free_slots;
  std::mutex lock;
};

struct TxBurstContext {
  uint32_t key = 0;
  std::vector<int32_t> slot_ids;
  std::vector<uint64_t> tx_times_ns;
};

struct RxBurstContext {
  uint32_t key = 0;
  std::vector<int32_t> slot_ids;
  std::vector<uint16_t> flow_ids;
};

static std::shared_ptr<TxBurstContext> get_tx_burst_context(BurstParams* burst) {
  if (burst == nullptr || burst->custom_pkt_data == nullptr) { return nullptr; }
  if ((burst->hdr.hdr.burst_flags & kBurstFlagTxContext) == 0U) { return nullptr; }
  return std::static_pointer_cast<TxBurstContext>(burst->custom_pkt_data);
}

static std::shared_ptr<RxBurstContext> get_rx_burst_context(BurstParams* burst) {
  if (burst == nullptr || burst->custom_pkt_data == nullptr) { return nullptr; }
  if ((burst->hdr.hdr.burst_flags & kBurstFlagRxContext) == 0U) { return nullptr; }
  return std::static_pointer_cast<RxBurstContext>(burst->custom_pkt_data);
}

struct PortState {
  struct doca_dev* dev = nullptr;
  char pci_addr[DOCA_DEVINFO_PCI_ADDR_SIZE]{};
  std::array<uint8_t, DOCA_DEVINFO_MAC_ADDR_SIZE> mac{};
  DocaFlowPortState flow;
};

static void rx_recv_task_success_cb(struct doca_eth_rxq_task_recv* task_recv,
                                    union doca_data task_user_data,
                                    union doca_data ctx_user_data) {
  (void)ctx_user_data;
  auto* slot = reinterpret_cast<RxSlot*>(task_user_data.ptr);
  if (slot == nullptr || slot->queue == nullptr) {
    doca_task_free(doca_eth_rxq_task_recv_as_doca_task(task_recv));
    return;
  }

  uint32_t flow_tag = 0;
  (void)doca_eth_rxq_task_recv_get_flow_tag(task_recv, &flow_tag);
  slot->flow_id = static_cast<uint16_t>(flow_tag & 0xffffU);

  size_t data_len = 0;
  slot->total_len = 0;
  for (size_t seg = 0; seg < slot->seg_bufs.size() && seg < MAX_NUM_SEGS; ++seg) {
    if (doca_buf_get_data_len(slot->seg_bufs[seg], &data_len) == DOCA_SUCCESS) {
      slot->seg_lens[seg] = static_cast<uint32_t>(data_len);
      slot->total_len += slot->seg_lens[seg];
    } else {
      slot->seg_lens[seg] = 0;
    }
  }

  slot->state = RxSlot::State::COMPLETED;
  {
    std::lock_guard<std::mutex> guard(slot->queue->lock);
    slot->queue->completed_slots.push_back(slot->slot_id);
  }

  doca_task_free(doca_eth_rxq_task_recv_as_doca_task(task_recv));
}

static void rx_recv_task_error_cb(struct doca_eth_rxq_task_recv* task_recv,
                                  union doca_data task_user_data,
                                  union doca_data ctx_user_data) {
  (void)ctx_user_data;
  auto* slot = reinterpret_cast<RxSlot*>(task_user_data.ptr);
  if (slot != nullptr && slot->queue != nullptr) {
    slot->state = RxSlot::State::ERROR;
    std::lock_guard<std::mutex> guard(slot->queue->lock);
    slot->queue->completed_slots.push_back(slot->slot_id);
  }

  doca_task_free(doca_eth_rxq_task_recv_as_doca_task(task_recv));
}

static void tx_send_task_success_cb(struct doca_eth_txq_task_send* task_send,
                                    union doca_data task_user_data,
                                    union doca_data ctx_user_data) {
  (void)ctx_user_data;
  auto* slot = reinterpret_cast<TxSlot*>(task_user_data.ptr);
  if (slot != nullptr) { slot->tx_status = 1; }
  doca_task_free(doca_eth_txq_task_send_as_doca_task(task_send));
}

static void tx_send_task_error_cb(struct doca_eth_txq_task_send* task_send,
                                  union doca_data task_user_data,
                                  union doca_data ctx_user_data) {
  (void)ctx_user_data;
  auto* slot = reinterpret_cast<TxSlot*>(task_user_data.ptr);
  if (slot != nullptr) { slot->tx_status = 2; }
  doca_task_free(doca_eth_txq_task_send_as_doca_task(task_send));
}

}  // namespace

struct DocaEthMgr::Impl {
  explicit Impl(DocaEthMgr* owner) : owner_(owner) {}

  DocaEthMgr* owner_;

  bool eal_initialized_ = false;
  bool flow_initialized_ = false;
  bool running_ = false;

  struct doca_buf_inventory* buf_inv_ = nullptr;
  std::unordered_map<std::string, struct doca_mmap*> mmaps_;

  std::unordered_map<uint16_t, PortState> ports_;
  std::unordered_map<uint32_t, RxQueueState> rx_queues_;
  std::unordered_map<uint32_t, TxQueueState> tx_queues_;

  std::unordered_map<std::string, size_t> mr_next_idx_;
  std::unordered_map<int, uint16_t> port_num_rx_queues_;

  std::atomic<uint64_t> rx_packets_{0};
  std::atomic<uint64_t> tx_packets_{0};
  std::atomic<uint64_t> rx_bytes_{0};
  std::atomic<uint64_t> tx_bytes_{0};

  bool init_eal();
  bool adjust_and_allocate_memory_regions();
  bool open_ports();
  bool init_mmaps_and_inventory();
  bool init_tx_queues();
  bool init_rx_queues();
  bool init_flow();

  bool init_tx_queue(uint16_t port_id, const TxQueueConfig& qcfg);
  bool init_rx_queue(uint16_t port_id, const RxQueueConfig& qcfg);

  bool create_slot_chain(const std::vector<std::string>& mrs,
                         std::vector<void*>& seg_ptrs,
                         std::vector<size_t>& seg_caps,
                         std::vector<struct doca_buf*>& seg_bufs,
                         struct doca_buf** head_buf);
  bool submit_rx_slot(RxSlot& slot);
  void progress_rx_queue(RxQueueState& qstate);
  void progress_tx_queue(TxQueueState& qstate);

  void release_slot_buffers();
  void shutdown();
};

bool DocaEthMgr::Impl::init_eal() {
  if (eal_initialized_) { return true; }

  std::set<std::string> core_set;
  core_set.emplace(std::to_string(owner_->cfg_.common_.master_core_));
  for (const auto& intf : owner_->cfg_.ifs_) {
    for (const auto& q : intf.rx_.queues_) {
      if (!q.common_.cpu_core_.empty()) { core_set.emplace(q.common_.cpu_core_); }
    }
    for (const auto& q : intf.tx_.queues_) {
      if (!q.common_.cpu_core_.empty()) { core_set.emplace(q.common_.cpu_core_); }
    }
  }

  std::string core_list;
  for (auto it = core_set.begin(); it != core_set.end(); ++it) {
    if (!core_list.empty()) { core_list += ","; }
    core_list += *it;
  }
  if (core_list.empty()) { core_list = "0"; }

  std::vector<std::string> args;
  args.emplace_back("daqiri_doca_eth");
  args.emplace_back("--file-prefix=daqiri_doca_eth");
  args.emplace_back("-l");
  args.emplace_back(core_list);

  bool requires_va_iova = false;
  for (const auto& mr : owner_->cfg_.mrs_) {
    if (mr.second.kind_ != MemoryKind::HUGE) {
      requires_va_iova = true;
      break;
    }
  }
  if (requires_va_iova) { args.emplace_back("--iova-mode=va"); }

  std::vector<char*> argv;
  argv.reserve(args.size());
  for (auto& arg : args) { argv.push_back(arg.data()); }

  std::string arg_str;
  for (const auto& arg : args) {
    if (!arg_str.empty()) { arg_str += " "; }
    arg_str += arg;
  }
  DAQIRI_LOG_INFO("doca_eth EAL arguments: {}", arg_str);

  int ret = rte_eal_init(static_cast<int>(argv.size()), argv.data());
  if (ret < 0) {
    if (rte_errno == EALREADY) {
      eal_initialized_ = true;
      return true;
    }
    DAQIRI_LOG_ERROR("doca_eth failed to initialize DPDK EAL: {}", rte_strerror(rte_errno));
    return false;
  }

  eal_initialized_ = true;
  return true;
}

bool DocaEthMgr::Impl::adjust_and_allocate_memory_regions() {
  for (auto& mr : owner_->cfg_.mrs_) {
    mr.second.adj_size_ = static_cast<size_t>(
        align_up(static_cast<uint64_t>(mr.second.buf_size_), owner_->get_alignment(mr.second.kind_)));
  }

  const auto status = owner_->allocate_memory_regions();
  if (status != Status::SUCCESS) {
    DAQIRI_LOG_ERROR("doca_eth failed to allocate memory regions");
    return false;
  }
  return true;
}

bool DocaEthMgr::Impl::open_ports() {
  struct doca_devinfo** devinfo_list = nullptr;
  uint32_t nb_devs = 0;
  doca_error_t status = doca_devinfo_create_list(&devinfo_list, &nb_devs);
  if (status != DOCA_SUCCESS) {
    log_doca_status("doca_devinfo_create_list failed", status);
    return false;
  }

  std::unordered_map<std::string, struct doca_dev*> opened_devs;
  std::unordered_map<std::string, std::array<uint8_t, DOCA_DEVINFO_MAC_ADDR_SIZE>> opened_macs;

  for (auto& intf : owner_->cfg_.ifs_) {
    if (looks_like_placeholder_bdf(intf.address_)) {
      DAQIRI_LOG_ERROR(
          "Interface '{}' has placeholder PCI BDF '{}'. Provide a real PCI address",
          intf.name_,
          intf.address_);
      (void)doca_devinfo_destroy_list(devinfo_list);
      return false;
    }

    if (opened_devs.find(intf.address_) == opened_devs.end()) {
      struct doca_dev* dev = nullptr;
      struct std::array<uint8_t, DOCA_DEVINFO_MAC_ADDR_SIZE> mac{};
      bool found = false;

      for (uint32_t i = 0; i < nb_devs; ++i) {
        uint8_t is_equal = 0;
        status = doca_devinfo_is_equal_pci_addr(devinfo_list[i], intf.address_.c_str(), &is_equal);
        if (status != DOCA_SUCCESS || is_equal == 0) { continue; }

        status = doca_dev_open(devinfo_list[i], &dev);
        if (status != DOCA_SUCCESS) {
          log_doca_status("doca_dev_open failed", status);
          (void)doca_devinfo_destroy_list(devinfo_list);
          return false;
        }

        status =
            doca_devinfo_get_mac_addr(devinfo_list[i], mac.data(), static_cast<uint32_t>(mac.size()));
        if (status != DOCA_SUCCESS) {
          log_doca_status("doca_devinfo_get_mac_addr failed", status);
          (void)doca_dev_close(dev);
          (void)doca_devinfo_destroy_list(devinfo_list);
          return false;
        }

        found = true;
        break;
      }

      if (!found || dev == nullptr) {
        DAQIRI_LOG_ERROR("doca_eth failed to find/open device {}", intf.address_);
        (void)doca_devinfo_destroy_list(devinfo_list);
        return false;
      }

      opened_devs[intf.address_] = dev;
      opened_macs[intf.address_] = mac;
    }

    PortState pst;
    pst.dev = opened_devs[intf.address_];
    pst.mac = opened_macs[intf.address_];
    std::snprintf(pst.pci_addr, sizeof(pst.pci_addr), "%s", intf.address_.c_str());
    ports_[intf.port_id_] = pst;
  }

  status = doca_devinfo_destroy_list(devinfo_list);
  if (status != DOCA_SUCCESS) {
    log_doca_status("doca_devinfo_destroy_list failed", status);
    return false;
  }

  return true;
}

bool DocaEthMgr::Impl::init_mmaps_and_inventory() {
  size_t total_num_bufs = 0;
  for (const auto& mr : owner_->cfg_.mrs_) { total_num_bufs += mr.second.num_bufs_; }

  size_t inv_elements = std::max<size_t>(1024, total_num_bufs * 4);
  doca_error_t status = doca_buf_inventory_create(inv_elements, &buf_inv_);
  if (status != DOCA_SUCCESS) {
    log_doca_status("doca_buf_inventory_create failed", status);
    return false;
  }

  status = doca_buf_inventory_start(buf_inv_);
  if (status != DOCA_SUCCESS) {
    log_doca_status("doca_buf_inventory_start failed", status);
    return false;
  }

  for (const auto& mr : owner_->cfg_.mrs_) {
    struct doca_mmap* mmap = nullptr;
    status = doca_mmap_create(&mmap);
    if (status != DOCA_SUCCESS) {
      log_doca_status("doca_mmap_create failed", status);
      return false;
    }

    status = doca_mmap_set_permissions(mmap, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE);
    if (status != DOCA_SUCCESS) {
      log_doca_status("doca_mmap_set_permissions failed", status);
      return false;
    }

    std::unordered_set<struct doca_dev*> required_devs;
    for (const auto& intf : owner_->cfg_.ifs_) {
      bool mr_used_by_intf = false;
      for (const auto& q : intf.rx_.queues_) {
        if (std::find(q.common_.mrs_.begin(), q.common_.mrs_.end(), mr.first) !=
            q.common_.mrs_.end()) {
          mr_used_by_intf = true;
          break;
        }
      }
      if (!mr_used_by_intf) {
        for (const auto& q : intf.tx_.queues_) {
          if (std::find(q.common_.mrs_.begin(), q.common_.mrs_.end(), mr.first) !=
              q.common_.mrs_.end()) {
            mr_used_by_intf = true;
            break;
          }
        }
      }

      if (mr_used_by_intf) {
        auto pit = ports_.find(intf.port_id_);
        if (pit != ports_.end() && pit->second.dev != nullptr) { required_devs.insert(pit->second.dev); }
      }
    }

    if (required_devs.empty()) {
      DAQIRI_LOG_ERROR("No DOCA devices use memory region '{}'", mr.first);
      return false;
    }

    for (auto* dev : required_devs) {
      status = doca_mmap_add_dev(mmap, dev);
      if (status != DOCA_SUCCESS) {
        log_doca_status("doca_mmap_add_dev failed", status);
        return false;
      }
    }

    auto ar_it = owner_->ar_.find(mr.first);
    if (ar_it == owner_->ar_.end()) {
      DAQIRI_LOG_ERROR("Memory region '{}' not found in allocated region map", mr.first);
      return false;
    }

    void* base_ptr = ar_it->second.ptr_;
    const size_t mem_len = mr.second.adj_size_ * mr.second.num_bufs_;
    status = doca_mmap_set_memrange(mmap, base_ptr, mem_len);
    if (status != DOCA_SUCCESS) {
      log_doca_status("doca_mmap_set_memrange failed", status);
      return false;
    }

    status = doca_mmap_start(mmap);
    if (status != DOCA_SUCCESS) {
      log_doca_status("doca_mmap_start failed", status);
      return false;
    }

    mmaps_[mr.first] = mmap;
    mr_next_idx_[mr.first] = 0;
  }

  return true;
}

bool DocaEthMgr::Impl::create_slot_chain(const std::vector<std::string>& mrs,
                                         std::vector<void*>& seg_ptrs,
                                         std::vector<size_t>& seg_caps,
                                         std::vector<struct doca_buf*>& seg_bufs,
                                         struct doca_buf** head_buf) {
  seg_ptrs.clear();
  seg_caps.clear();
  seg_bufs.clear();
  *head_buf = nullptr;

  for (const auto& mr_name : mrs) {
    const auto mr_it = owner_->cfg_.mrs_.find(mr_name);
    if (mr_it == owner_->cfg_.mrs_.end()) {
      DAQIRI_LOG_ERROR("Memory region '{}' not found in config", mr_name);
      return false;
    }

    auto cursor_it = mr_next_idx_.find(mr_name);
    if (cursor_it == mr_next_idx_.end()) {
      DAQIRI_LOG_ERROR("Memory region '{}' cursor not initialized", mr_name);
      return false;
    }

    if (cursor_it->second >= mr_it->second.num_bufs_) {
      DAQIRI_LOG_ERROR("Memory region '{}' depleted while assigning queue buffers", mr_name);
      return false;
    }

    auto alloc_it = owner_->ar_.find(mr_name);
    if (alloc_it == owner_->ar_.end() || alloc_it->second.ptr_ == nullptr) {
      DAQIRI_LOG_ERROR("Memory region '{}' was not allocated", mr_name);
      return false;
    }

    char* base = reinterpret_cast<char*>(alloc_it->second.ptr_);
    const size_t buf_idx = cursor_it->second++;
    void* seg_ptr = base + (buf_idx * mr_it->second.adj_size_);
    const size_t seg_cap = mr_it->second.buf_size_;

    auto mmap_it = mmaps_.find(mr_name);
    if (mmap_it == mmaps_.end()) {
      DAQIRI_LOG_ERROR("Memory region '{}' mmap is missing", mr_name);
      return false;
    }

    struct doca_buf* seg_buf = nullptr;
    doca_error_t status =
        doca_buf_inventory_buf_get_by_addr(buf_inv_, mmap_it->second, seg_ptr, seg_cap, &seg_buf);
    if (status != DOCA_SUCCESS) {
      log_doca_status("doca_buf_inventory_buf_get_by_addr failed", status);
      return false;
    }

    seg_ptrs.push_back(seg_ptr);
    seg_caps.push_back(seg_cap);
    seg_bufs.push_back(seg_buf);
  }

  if (seg_bufs.empty()) {
    DAQIRI_LOG_ERROR("Queue has no memory regions");
    return false;
  }

  *head_buf = seg_bufs[0];
  for (size_t i = 1; i < seg_bufs.size(); ++i) {
    doca_error_t status = doca_buf_chain_list(*head_buf, seg_bufs[i]);
    if (status != DOCA_SUCCESS) {
      log_doca_status("doca_buf_chain_list failed", status);
      return false;
    }
  }

  return true;
}

bool DocaEthMgr::Impl::init_tx_queue(uint16_t port_id, const TxQueueConfig& qcfg) {
  auto port_it = ports_.find(port_id);
  if (port_it == ports_.end()) {
    DAQIRI_LOG_ERROR("Invalid port {} in TX queue init", port_id);
    return false;
  }

  const uint32_t key = queue_key(port_id, static_cast<uint16_t>(qcfg.common_.id_));
  if (tx_queues_.find(key) != tx_queues_.end()) {
    DAQIRI_LOG_ERROR("Duplicate TX queue key {} for queue {}", key, qcfg.common_.name_);
    return false;
  }

  auto [insert_it, inserted] = tx_queues_.try_emplace(key);
  if (!inserted) {
    DAQIRI_LOG_ERROR("Failed to create TX queue state for queue {}", qcfg.common_.name_);
    return false;
  }

  auto& qstate = insert_it->second;
  qstate.key = key;
  qstate.port_id = port_id;
  qstate.queue_id = static_cast<uint16_t>(qcfg.common_.id_);
  qstate.batch_size = static_cast<uint32_t>(qcfg.common_.batch_size_);
  qstate.num_segs = static_cast<uint32_t>(qcfg.common_.mrs_.size());

  if (qstate.num_segs == 0 || qstate.num_segs > MAX_NUM_SEGS) {
    DAQIRI_LOG_ERROR("TX queue {} has invalid num_segs {}", qcfg.common_.name_, qstate.num_segs);
    return false;
  }

  uint32_t max_burst = std::max<uint32_t>(1, qstate.batch_size);
  doca_error_t status = doca_eth_txq_create(port_it->second.dev, max_burst, &qstate.txq);
  if (status != DOCA_SUCCESS) {
    log_doca_status("doca_eth_txq_create failed", status);
    return false;
  }

  status = doca_eth_txq_set_type(qstate.txq, DOCA_ETH_TXQ_TYPE_REGULAR);
  if (status != DOCA_SUCCESS) {
    log_doca_status("doca_eth_txq_set_type failed", status);
    return false;
  }

  status = doca_eth_txq_set_max_send_buf_list_len(qstate.txq, qstate.num_segs);
  if (status != DOCA_SUCCESS) {
    log_doca_status("doca_eth_txq_set_max_send_buf_list_len failed", status);
    return false;
  }

  const uint32_t task_send_budget = std::max<uint32_t>(max_burst, 1024U);
  status = doca_eth_txq_task_send_set_conf(
      qstate.txq, tx_send_task_success_cb, tx_send_task_error_cb, task_send_budget);
  if (status != DOCA_SUCCESS) {
    log_doca_status("doca_eth_txq_task_send_set_conf failed", status);
    return false;
  }

  qstate.ctx = doca_eth_txq_as_doca_ctx(qstate.txq);
  status = doca_pe_create(&qstate.pe);
  if (status != DOCA_SUCCESS) {
    log_doca_status("doca_pe_create failed (tx)", status);
    return false;
  }

  status = doca_pe_connect_ctx(qstate.pe, qstate.ctx);
  if (status != DOCA_SUCCESS) {
    log_doca_status("doca_pe_connect_ctx failed (tx)", status);
    return false;
  }

  status = doca_ctx_start(qstate.ctx);
  if (status != DOCA_SUCCESS) {
    log_doca_status("doca_ctx_start failed (tx)", status);
    return false;
  }

  status = doca_eth_txq_apply_queue_id(qstate.txq, qstate.queue_id);
  if (status != DOCA_SUCCESS) {
    log_doca_status("doca_eth_txq_apply_queue_id failed", status);
    return false;
  }

  size_t min_num_bufs = std::numeric_limits<size_t>::max();
  for (const auto& mr_name : qcfg.common_.mrs_) {
    min_num_bufs = std::min(min_num_bufs, owner_->cfg_.mrs_.at(mr_name).num_bufs_);
  }

  const size_t slot_count = std::min(min_num_bufs,
                                     std::max<size_t>(qstate.batch_size, 1024));
  if (slot_count < static_cast<size_t>(qstate.batch_size)) {
    DAQIRI_LOG_ERROR(
        "TX queue {} requires at least {} buffers but only {} available",
        qcfg.common_.name_,
        qstate.batch_size,
        slot_count);
    return false;
  }

  qstate.slots.resize(slot_count);
  for (size_t i = 0; i < slot_count; ++i) {
    auto& slot = qstate.slots[i];
    slot.queue = &qstate;
    slot.slot_id = static_cast<uint32_t>(i);

    if (!create_slot_chain(qcfg.common_.mrs_,
                           slot.seg_ptrs,
                           slot.seg_caps,
                           slot.seg_bufs,
                           &slot.head_buf)) {
      return false;
    }

    qstate.free_slots.push_back(static_cast<uint32_t>(i));
  }

  return true;
}

bool DocaEthMgr::Impl::submit_rx_slot(RxSlot& slot) {
  for (auto* seg_buf : slot.seg_bufs) { (void)doca_buf_reset_data_len(seg_buf); }

  union doca_data user_data{};
  user_data.ptr = &slot;

  struct doca_eth_rxq_task_recv* task = nullptr;
  doca_error_t status =
      doca_eth_rxq_task_recv_allocate_init(slot.queue->rxq, slot.head_buf, user_data, &task);
  if (status != DOCA_SUCCESS) {
    log_doca_status("doca_eth_rxq_task_recv_allocate_init failed", status);
    slot.state = RxSlot::State::ERROR;
    return false;
  }

  status = doca_task_submit(doca_eth_rxq_task_recv_as_doca_task(task));
  if (status != DOCA_SUCCESS) {
    log_doca_status("doca_task_submit failed (rx)", status);
    doca_task_free(doca_eth_rxq_task_recv_as_doca_task(task));
    slot.state = RxSlot::State::ERROR;
    return false;
  }

  slot.state = RxSlot::State::POSTED;
  return true;
}

bool DocaEthMgr::Impl::init_rx_queue(uint16_t port_id, const RxQueueConfig& qcfg) {
  auto port_it = ports_.find(port_id);
  if (port_it == ports_.end()) {
    DAQIRI_LOG_ERROR("Invalid port {} in RX queue init", port_id);
    return false;
  }

  const uint32_t key = queue_key(port_id, static_cast<uint16_t>(qcfg.common_.id_));
  if (rx_queues_.find(key) != rx_queues_.end()) {
    DAQIRI_LOG_ERROR("Duplicate RX queue key {} for queue {}", key, qcfg.common_.name_);
    return false;
  }

  auto [insert_it, inserted] = rx_queues_.try_emplace(key);
  if (!inserted) {
    DAQIRI_LOG_ERROR("Failed to create RX queue state for queue {}", qcfg.common_.name_);
    return false;
  }

  auto& qstate = insert_it->second;
  qstate.key = key;
  qstate.port_id = port_id;
  qstate.queue_id = static_cast<uint16_t>(qcfg.common_.id_);
  qstate.batch_size = static_cast<uint32_t>(qcfg.common_.batch_size_);
  qstate.num_segs = static_cast<uint32_t>(qcfg.common_.mrs_.size());

  if (qstate.num_segs == 0 || qstate.num_segs > MAX_NUM_SEGS) {
    DAQIRI_LOG_ERROR("RX queue {} has invalid num_segs {}", qcfg.common_.name_, qstate.num_segs);
    return false;
  }

  qstate.max_packet_size = 0;
  size_t min_num_bufs = std::numeric_limits<size_t>::max();
  for (const auto& mr_name : qcfg.common_.mrs_) {
    const auto& mr = owner_->cfg_.mrs_.at(mr_name);
    qstate.max_packet_size += static_cast<uint32_t>(mr.buf_size_);
    min_num_bufs = std::min(min_num_bufs, mr.num_bufs_);
  }

  uint32_t max_burst = std::max<uint32_t>(1, qstate.batch_size);
  doca_error_t status =
      doca_eth_rxq_create(port_it->second.dev, max_burst, qstate.max_packet_size, &qstate.rxq);
  if (status != DOCA_SUCCESS) {
    log_doca_status("doca_eth_rxq_create failed", status);
    return false;
  }

  status = doca_eth_rxq_set_type(qstate.rxq, DOCA_ETH_RXQ_TYPE_REGULAR);
  if (status != DOCA_SUCCESS) {
    log_doca_status("doca_eth_rxq_set_type failed", status);
    return false;
  }

  status = doca_eth_rxq_set_flow_tag(qstate.rxq, 1);
  if (status != DOCA_SUCCESS) {
    log_doca_status("doca_eth_rxq_set_flow_tag failed", status);
    return false;
  }

  status = doca_eth_rxq_set_max_recv_buf_list_len(qstate.rxq, qstate.num_segs);
  if (status != DOCA_SUCCESS) {
    log_doca_status("doca_eth_rxq_set_max_recv_buf_list_len failed", status);
    return false;
  }

  const size_t slot_count =
      std::min(min_num_bufs,
               std::max<size_t>(kMinRxSlotsPerQueue,
                                std::min<size_t>(kMaxRxSlotsPerQueue, qstate.batch_size)));
  const uint32_t task_count = static_cast<uint32_t>(std::max<size_t>(1, slot_count));

  status = doca_eth_rxq_task_recv_set_conf(
      qstate.rxq, rx_recv_task_success_cb, rx_recv_task_error_cb, task_count);
  if (status != DOCA_SUCCESS) {
    log_doca_status("doca_eth_rxq_task_recv_set_conf failed", status);
    return false;
  }

  qstate.ctx = doca_eth_rxq_as_doca_ctx(qstate.rxq);

  status = doca_pe_create(&qstate.pe);
  if (status != DOCA_SUCCESS) {
    log_doca_status("doca_pe_create failed (rx)", status);
    return false;
  }

  status = doca_pe_connect_ctx(qstate.pe, qstate.ctx);
  if (status != DOCA_SUCCESS) {
    log_doca_status("doca_pe_connect_ctx failed (rx)", status);
    return false;
  }

  status = doca_ctx_start(qstate.ctx);
  if (status != DOCA_SUCCESS) {
    log_doca_status("doca_ctx_start failed (rx)", status);
    return false;
  }

  status = doca_eth_rxq_apply_queue_id(qstate.rxq, qstate.queue_id);
  if (status != DOCA_SUCCESS) {
    log_doca_status("doca_eth_rxq_apply_queue_id failed", status);
    return false;
  }

  qstate.slots.resize(slot_count);
  for (size_t i = 0; i < slot_count; ++i) {
    auto& slot = qstate.slots[i];
    slot.queue = &qstate;
    slot.slot_id = static_cast<uint32_t>(i);

    if (!create_slot_chain(qcfg.common_.mrs_,
                           slot.seg_ptrs,
                           slot.seg_caps,
                           slot.seg_bufs,
                           &slot.head_buf)) {
      return false;
    }

    if (!submit_rx_slot(slot)) {
      DAQIRI_LOG_ERROR("Failed to submit initial RX task for queue {} slot {}",
                       qcfg.common_.name_,
                       i);
      return false;
    }
  }

  return true;
}

bool DocaEthMgr::Impl::init_tx_queues() {
  for (const auto& intf : owner_->cfg_.ifs_) {
    for (const auto& q : intf.tx_.queues_) {
      if (!init_tx_queue(intf.port_id_, q)) { return false; }
    }
  }
  return true;
}

bool DocaEthMgr::Impl::init_rx_queues() {
  for (const auto& intf : owner_->cfg_.ifs_) {
    for (const auto& q : intf.rx_.queues_) {
      if (!init_rx_queue(intf.port_id_, q)) { return false; }
    }
  }
  return true;
}

bool DocaEthMgr::Impl::init_flow() {
  bool has_rx = false;
  for (const auto& intf : owner_->cfg_.ifs_) {
    if (!intf.rx_.queues_.empty()) {
      has_rx = true;
      break;
    }
  }
  if (!has_rx) { return true; }

  struct doca_flow_cfg* flow_cfg = nullptr;
  doca_error_t status = doca_flow_cfg_create(&flow_cfg);
  if (status != DOCA_SUCCESS) {
    log_doca_status("doca_flow_cfg_create failed", status);
    return false;
  }

  status = doca_flow_cfg_set_pipe_queues(flow_cfg, 1);
  if (status != DOCA_SUCCESS) {
    log_doca_status("doca_flow_cfg_set_pipe_queues failed", status);
    (void)doca_flow_cfg_destroy(flow_cfg);
    return false;
  }

  status = doca_flow_cfg_set_mode_args(flow_cfg, "vnf");
  if (status != DOCA_SUCCESS) {
    log_doca_status("doca_flow_cfg_set_mode_args failed", status);
    (void)doca_flow_cfg_destroy(flow_cfg);
    return false;
  }

  status = doca_flow_cfg_set_nr_counters(flow_cfg, kMaxFlowCounters);
  if (status != DOCA_SUCCESS) {
    log_doca_status("doca_flow_cfg_set_nr_counters failed", status);
    (void)doca_flow_cfg_destroy(flow_cfg);
    return false;
  }

  status = doca_flow_init(flow_cfg);
  (void)doca_flow_cfg_destroy(flow_cfg);
  if (status != DOCA_SUCCESS) {
    log_doca_status("doca_flow_init failed", status);
    return false;
  }
  flow_initialized_ = true;

  for (const auto& intf : owner_->cfg_.ifs_) {
    if (intf.rx_.queues_.empty()) { continue; }

    auto port_it = ports_.find(intf.port_id_);
    if (port_it == ports_.end()) {
      DAQIRI_LOG_ERROR("Port {} missing in flow setup", intf.port_id_);
      return false;
    }

    struct doca_flow_port_cfg* port_cfg = nullptr;
    status = doca_flow_port_cfg_create(&port_cfg);
    if (status != DOCA_SUCCESS) {
      log_doca_status("doca_flow_port_cfg_create failed", status);
      return false;
    }

    status = doca_flow_port_cfg_set_port_id(port_cfg, intf.port_id_);
    if (status == DOCA_SUCCESS) {
      status = doca_flow_port_cfg_set_dev(port_cfg, port_it->second.dev);
    }
    if (status == DOCA_SUCCESS) {
      status = doca_flow_port_cfg_set_nr_resources(
          port_cfg, DOCA_FLOW_RESOURCE_RSS, static_cast<uint32_t>(intf.rx_.queues_.size()));
    }
    if (status != DOCA_SUCCESS) {
      log_doca_status("doca_flow_port_cfg setup failed", status);
      (void)doca_flow_port_cfg_destroy(port_cfg);
      return false;
    }

    status = doca_flow_port_start(port_cfg, &port_it->second.flow.flow_port);
    (void)doca_flow_port_cfg_destroy(port_cfg);
    if (status != DOCA_SUCCESS) {
      log_doca_status("doca_flow_port_start failed", status);
      return false;
    }

    std::vector<uint16_t> rss_queues;
    rss_queues.reserve(intf.rx_.queues_.size());
    for (const auto& q : intf.rx_.queues_) {
      rss_queues.push_back(static_cast<uint16_t>(q.common_.id_));
    }

    const uint16_t default_flow_id =
        intf.rx_.flows_.empty() ? 0 : static_cast<uint16_t>(intf.rx_.flows_.front().id_);
    port_it->second.flow.default_flow_id = default_flow_id;

    struct doca_flow_pipe_cfg* pipe_cfg = nullptr;
    status = doca_flow_pipe_cfg_create(&pipe_cfg, port_it->second.flow.flow_port);
    if (status != DOCA_SUCCESS) {
      log_doca_status("doca_flow_pipe_cfg_create failed", status);
      return false;
    }

    struct doca_flow_match pipe_match{};
    struct doca_flow_actions pipe_action{};
    pipe_action.meta.pkt_meta = DOCA_HTOBE32(default_flow_id);
    struct doca_flow_actions* pipe_actions[1] = {&pipe_action};

    status = doca_flow_pipe_cfg_set_name(pipe_cfg, "DAQIRI_DOCA_ETH_ROOT");
    if (status == DOCA_SUCCESS) {
      status = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_BASIC);
    }
    if (status == DOCA_SUCCESS) {
      status = doca_flow_pipe_cfg_set_is_root(pipe_cfg, true);
    }
    if (status == DOCA_SUCCESS) {
      status = doca_flow_pipe_cfg_set_match(pipe_cfg, &pipe_match, nullptr);
    }
    if (status == DOCA_SUCCESS) {
      status = doca_flow_pipe_cfg_set_actions(pipe_cfg, pipe_actions, nullptr, nullptr, 1);
    }
    if (status != DOCA_SUCCESS) {
      log_doca_status("doca_flow_pipe_cfg_set_* failed", status);
      (void)doca_flow_pipe_cfg_destroy(pipe_cfg);
      return false;
    }

    struct doca_flow_fwd fwd{};
    fwd.type = DOCA_FLOW_FWD_RSS;
    fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
    fwd.rss.outer_flags = DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_UDP;
    fwd.rss.queues_array = rss_queues.data();
    fwd.rss.nr_queues = static_cast<uint16_t>(rss_queues.size());

    struct doca_flow_fwd fwd_miss{};
    fwd_miss.type = DOCA_FLOW_FWD_DROP;

    status = doca_flow_pipe_create(
        pipe_cfg, &fwd, &fwd_miss, &port_it->second.flow.root_pipe);
    (void)doca_flow_pipe_cfg_destroy(pipe_cfg);
    if (status != DOCA_SUCCESS) {
      log_doca_status("doca_flow_pipe_create failed", status);
      return false;
    }

    struct doca_flow_match entry_match{};
    struct doca_flow_actions entry_action{};
    entry_action.meta.pkt_meta = DOCA_HTOBE32(default_flow_id);

    status = doca_flow_pipe_add_entry(0,
                                      port_it->second.flow.root_pipe,
                                      &entry_match,
                                      0,
                                      &entry_action,
                                      nullptr,
                                      nullptr,
                                      DOCA_FLOW_NO_WAIT,
                                      nullptr,
                                      &port_it->second.flow.default_entry);
    if (status != DOCA_SUCCESS) {
      log_doca_status("doca_flow_pipe_add_entry failed", status);
      return false;
    }

    status = doca_flow_entries_process(port_it->second.flow.flow_port, 0, 10000, 8);
    if (status != DOCA_SUCCESS) {
      log_doca_status("doca_flow_entries_process failed", status);
      return false;
    }

    if (doca_flow_pipe_entry_get_status(port_it->second.flow.default_entry) !=
        DOCA_FLOW_ENTRY_STATUS_SUCCESS) {
      DAQIRI_LOG_ERROR("doca_flow default entry did not reach SUCCESS state on port {}", intf.port_id_);
      return false;
    }
  }

  return true;
}

void DocaEthMgr::Impl::progress_rx_queue(RxQueueState& qstate) {
  if (qstate.pe == nullptr) { return; }
  for (int i = 0; i < 128; ++i) {
    if (doca_pe_progress(qstate.pe) == 0) { break; }
  }
}

void DocaEthMgr::Impl::progress_tx_queue(TxQueueState& qstate) {
  if (qstate.pe == nullptr) { return; }
  for (int i = 0; i < 64; ++i) {
    if (doca_pe_progress(qstate.pe) == 0) { break; }
  }
}

void DocaEthMgr::Impl::release_slot_buffers() {
  for (auto& kv : rx_queues_) {
    for (auto& slot : kv.second.slots) {
      for (auto*& seg_buf : slot.seg_bufs) {
        if (seg_buf != nullptr) {
          (void)doca_buf_dec_refcount(seg_buf, nullptr);
          seg_buf = nullptr;
        }
      }
      slot.head_buf = nullptr;
    }
  }

  for (auto& kv : tx_queues_) {
    for (auto& slot : kv.second.slots) {
      for (auto*& seg_buf : slot.seg_bufs) {
        if (seg_buf != nullptr) {
          (void)doca_buf_dec_refcount(seg_buf, nullptr);
          seg_buf = nullptr;
        }
      }
      slot.head_buf = nullptr;
    }
  }
}

void DocaEthMgr::Impl::shutdown() {
  for (auto& kv : rx_queues_) {
    auto& q = kv.second;
    if (q.ctx != nullptr) { (void)doca_ctx_stop(q.ctx); }
    if (q.rxq != nullptr) {
      (void)doca_eth_rxq_destroy(q.rxq);
      q.rxq = nullptr;
    }
    if (q.pe != nullptr) {
      (void)doca_pe_destroy(q.pe);
      q.pe = nullptr;
    }
  }

  for (auto& kv : tx_queues_) {
    auto& q = kv.second;
    if (q.ctx != nullptr) { (void)doca_ctx_stop(q.ctx); }
    if (q.txq != nullptr) {
      (void)doca_eth_txq_destroy(q.txq);
      q.txq = nullptr;
    }
    if (q.pe != nullptr) {
      (void)doca_pe_destroy(q.pe);
      q.pe = nullptr;
    }
  }

  for (auto& kv : ports_) {
    auto& flow = kv.second.flow;
    if (flow.root_pipe != nullptr) {
      doca_flow_pipe_destroy(flow.root_pipe);
      flow.root_pipe = nullptr;
      flow.default_entry = nullptr;
    }
    if (flow.flow_port != nullptr) {
      (void)doca_flow_port_stop(flow.flow_port);
      flow.flow_port = nullptr;
    }
  }

  if (flow_initialized_) {
    doca_flow_destroy();
    flow_initialized_ = false;
  }

  release_slot_buffers();
  rx_queues_.clear();
  tx_queues_.clear();

  if (buf_inv_ != nullptr) {
    (void)doca_buf_inventory_stop(buf_inv_);
    (void)doca_buf_inventory_destroy(buf_inv_);
    buf_inv_ = nullptr;
  }

  for (auto& kv : mmaps_) {
    if (kv.second != nullptr) {
      (void)doca_mmap_stop(kv.second);
      (void)doca_mmap_destroy(kv.second);
    }
  }
  mmaps_.clear();

  std::unordered_set<struct doca_dev*> closed;
  for (auto& kv : ports_) {
    if (kv.second.dev != nullptr && closed.insert(kv.second.dev).second) {
      (void)doca_dev_close(kv.second.dev);
    }
    kv.second.dev = nullptr;
  }
  ports_.clear();

  for (auto& mr : owner_->cfg_.mrs_) {
    if (!mr.second.owned_) { continue; }

    auto it = owner_->ar_.find(mr.first);
    if (it == owner_->ar_.end() || it->second.ptr_ == nullptr) { continue; }

    void* ptr = it->second.ptr_;
    switch (mr.second.kind_) {
      case MemoryKind::HOST:
        free(ptr);
        break;
      case MemoryKind::HOST_PINNED:
        (void)cudaFreeHost(ptr);
        break;
      case MemoryKind::HUGE:
        rte_free(ptr);
        break;
      case MemoryKind::DEVICE:
        (void)cuMemFree(reinterpret_cast<CUdeviceptr>(ptr));
        break;
      default:
        break;
    }
    it->second.ptr_ = nullptr;
  }

  owner_->ar_.clear();
  port_num_rx_queues_.clear();
  mr_next_idx_.clear();
  running_ = false;
}

DocaEthMgr::DocaEthMgr() : impl_(std::make_unique<Impl>(this)) {}

DocaEthMgr::~DocaEthMgr() {
  shutdown();
}

bool DocaEthMgr::set_config_and_initialize(const NetworkConfig& cfg) {
  num_init_++;

  if (initialized_) { return true; }

  DAQIRI_LOG_INFO("Initializing doca_eth manager");
  cfg_ = cfg;

  initialize();
  if (!initialized_) {
    DAQIRI_LOG_CRITICAL("Failed to initialize doca_eth manager");
    return false;
  }

  if (!validate_config()) {
    DAQIRI_LOG_CRITICAL("Config validation failed");
    shutdown();
    return false;
  }

  run();
  return true;
}

void DocaEthMgr::initialize() {
  for (size_t i = 0; i < cfg_.ifs_.size(); ++i) { cfg_.ifs_[i].port_id_ = static_cast<uint16_t>(i); }

  if (!impl_->init_eal()) { return; }
  if (!impl_->adjust_and_allocate_memory_regions()) { return; }
  if (!impl_->open_ports()) {
    impl_->shutdown();
    return;
  }
  if (!impl_->init_mmaps_and_inventory()) {
    impl_->shutdown();
    return;
  }
  if (!impl_->init_tx_queues()) {
    impl_->shutdown();
    return;
  }
  if (!impl_->init_rx_queues()) {
    impl_->shutdown();
    return;
  }
  if (!impl_->init_flow()) {
    impl_->shutdown();
    return;
  }

  impl_->port_num_rx_queues_.clear();
  for (const auto& intf : cfg_.ifs_) {
    impl_->port_num_rx_queues_[intf.port_id_] = static_cast<uint16_t>(intf.rx_.queues_.size());
  }

  impl_->running_ = true;
  initialized_ = true;
}

void DocaEthMgr::run() {
  DAQIRI_LOG_INFO("doca_eth manager initialized");
}

void* DocaEthMgr::get_packet_ptr(BurstParams* burst, int idx) {
  if (burst == nullptr || burst->pkts[0] == nullptr || idx < 0) { return nullptr; }
  return burst->pkts[0][idx];
}

uint32_t DocaEthMgr::get_packet_length(BurstParams* burst, int idx) {
  if (burst == nullptr || idx < 0) { return 0; }

  uint32_t total = 0;
  for (int seg = 0; seg < burst->hdr.hdr.num_segs && seg < MAX_NUM_SEGS; ++seg) {
    if (burst->pkt_lens[seg] == nullptr) { continue; }
    total += burst->pkt_lens[seg][idx];
  }
  return total;
}

void* DocaEthMgr::get_segment_packet_ptr(BurstParams* burst, int seg, int idx) {
  if (burst == nullptr || seg < 0 || seg >= MAX_NUM_SEGS || idx < 0) { return nullptr; }
  if (burst->pkts[seg] == nullptr) { return nullptr; }
  return burst->pkts[seg][idx];
}

uint32_t DocaEthMgr::get_segment_packet_length(BurstParams* burst, int seg, int idx) {
  if (burst == nullptr || seg < 0 || seg >= MAX_NUM_SEGS || idx < 0) { return 0; }
  if (burst->pkt_lens[seg] == nullptr) { return 0; }
  return burst->pkt_lens[seg][idx];
}

uint16_t DocaEthMgr::get_packet_flow_id(BurstParams* burst, int idx) {
  if (burst == nullptr || burst->pkt_extra_info == nullptr || idx < 0) { return 0; }
  const auto* flow_ids = reinterpret_cast<const uint16_t*>(burst->pkt_extra_info);
  return flow_ids[idx];
}

void* DocaEthMgr::get_packet_extra_info(BurstParams* burst, int idx) {
  if (burst == nullptr || burst->pkt_extra_info == nullptr || idx < 0) { return nullptr; }
  auto* flow_ids = reinterpret_cast<uint16_t*>(burst->pkt_extra_info);
  return &flow_ids[idx];
}

Status DocaEthMgr::get_tx_packet_burst(BurstParams* burst) {
  if (burst == nullptr) { return Status::NULL_PTR; }

  const uint32_t key = queue_key(burst->hdr.hdr.port_id, burst->hdr.hdr.q_id);
  auto q_it = impl_->tx_queues_.find(key);
  if (q_it == impl_->tx_queues_.end()) { return Status::INVALID_PARAMETER; }

  auto& qstate = q_it->second;
  impl_->progress_tx_queue(qstate);

  if (burst->hdr.hdr.num_pkts == 0) { burst->hdr.hdr.num_pkts = qstate.batch_size; }
  if (burst->hdr.hdr.num_segs == 0) { burst->hdr.hdr.num_segs = static_cast<int>(qstate.num_segs); }

  if (burst->hdr.hdr.num_segs != static_cast<int>(qstate.num_segs)) {
    DAQIRI_LOG_ERROR("TX burst num_segs {} does not match queue {} num_segs {}",
                     burst->hdr.hdr.num_segs,
                     key,
                     qstate.num_segs);
    return Status::INVALID_PARAMETER;
  }

  const int num_pkts = static_cast<int>(burst->hdr.hdr.num_pkts);
  if (num_pkts <= 0) { return Status::INVALID_PARAMETER; }

  auto tx_ctx = std::make_shared<TxBurstContext>();
  tx_ctx->key = key;
  tx_ctx->slot_ids.resize(num_pkts, -1);
  tx_ctx->tx_times_ns.resize(num_pkts, 0);

  {
    std::lock_guard<std::mutex> guard(qstate.lock);
    if (qstate.free_slots.size() < static_cast<size_t>(num_pkts)) { return Status::NO_FREE_PACKET_BUFFERS; }

    for (int i = 0; i < num_pkts; ++i) {
      tx_ctx->slot_ids[i] = static_cast<int32_t>(qstate.free_slots.front());
      qstate.free_slots.pop_front();
    }
  }

  for (int seg = 0; seg < burst->hdr.hdr.num_segs && seg < MAX_NUM_SEGS; ++seg) {
    burst->pkts[seg] = new void*[num_pkts]();
    burst->pkt_lens[seg] = new uint32_t[num_pkts]();
  }

  for (int i = 0; i < num_pkts; ++i) {
    auto& slot = qstate.slots[static_cast<size_t>(tx_ctx->slot_ids[i])];
    for (size_t seg = 0; seg < qstate.num_segs; ++seg) {
      burst->pkts[seg][i] = slot.seg_ptrs[seg];
      burst->pkt_lens[seg][i] = 0;
    }
  }

  burst->pkt_extra_info = nullptr;
  burst->hdr.hdr.burst_flags = kBurstFlagTxContext;
  burst->custom_pkt_data = std::static_pointer_cast<void>(tx_ctx);
  return Status::SUCCESS;
}

Status DocaEthMgr::set_eth_header(BurstParams* burst, int idx, char* dst_addr) {
  if (burst == nullptr || dst_addr == nullptr || burst->pkts[0] == nullptr) {
    return Status::INVALID_PARAMETER;
  }

  auto* pkt = reinterpret_cast<UDPIPV4Pkt*>(burst->pkts[0][idx]);
  std::memcpy(pkt->eth.h_dest, dst_addr, ETH_ALEN);
  pkt->eth.h_proto = htons(ETH_P_IP);
  return Status::SUCCESS;
}

Status DocaEthMgr::set_ipv4_header(BurstParams* burst,
                                   int idx,
                                   int ip_len,
                                   uint8_t proto,
                                   unsigned int src_host,
                                   unsigned int dst_host) {
  if (burst == nullptr || burst->pkts[0] == nullptr) { return Status::INVALID_PARAMETER; }

  auto* pkt = reinterpret_cast<UDPIPV4Pkt*>(burst->pkts[0][idx]);
  pkt->ip.version = 4;
  pkt->ip.ihl = 5;
  pkt->ip.protocol = proto;
  pkt->ip.tot_len = htons(static_cast<uint16_t>(sizeof(pkt->ip) + ip_len));
  pkt->ip.saddr = htonl(src_host);
  pkt->ip.daddr = htonl(dst_host);
  return Status::SUCCESS;
}

Status DocaEthMgr::set_udp_header(BurstParams* burst,
                                  int idx,
                                  int udp_len,
                                  uint16_t src_port,
                                  uint16_t dst_port) {
  if (burst == nullptr || burst->pkts[0] == nullptr) { return Status::INVALID_PARAMETER; }

  auto* pkt = reinterpret_cast<UDPIPV4Pkt*>(burst->pkts[0][idx]);
  pkt->udp.source = htons(src_port);
  pkt->udp.dest = htons(dst_port);
  pkt->udp.check = 0;
  pkt->udp.len = htons(static_cast<uint16_t>(udp_len + sizeof(pkt->udp)));
  return Status::SUCCESS;
}

Status DocaEthMgr::set_udp_payload(BurstParams* burst, int idx, void* data, int len) {
  if (burst == nullptr || burst->pkts[0] == nullptr || data == nullptr || len < 0) {
    return Status::INVALID_PARAMETER;
  }

  auto* pkt = reinterpret_cast<UDPIPV4Pkt*>(burst->pkts[0][idx]);
  std::memcpy(reinterpret_cast<void*>(&pkt->udp + 1), data, static_cast<size_t>(len));
  return Status::SUCCESS;
}

bool DocaEthMgr::is_tx_burst_available(BurstParams* burst) {
  if (burst == nullptr) { return false; }

  const uint32_t key = queue_key(burst->hdr.hdr.port_id, burst->hdr.hdr.q_id);
  auto q_it = impl_->tx_queues_.find(key);
  if (q_it == impl_->tx_queues_.end()) { return false; }

  auto& qstate = q_it->second;
  impl_->progress_tx_queue(qstate);

  const size_t needed = burst->hdr.hdr.num_pkts == 0
                            ? static_cast<size_t>(qstate.batch_size)
                            : static_cast<size_t>(burst->hdr.hdr.num_pkts);

  std::lock_guard<std::mutex> guard(qstate.lock);
  return qstate.free_slots.size() >= needed;
}

Status DocaEthMgr::set_packet_lengths(BurstParams* burst,
                                      int idx,
                                      const std::initializer_list<int>& lens) {
  if (burst == nullptr || idx < 0) { return Status::INVALID_PARAMETER; }
  if (static_cast<int>(lens.size()) != burst->hdr.hdr.num_segs) { return Status::INVALID_PARAMETER; }

  int seg = 0;
  for (const auto len : lens) {
    if (seg >= MAX_NUM_SEGS || burst->pkt_lens[seg] == nullptr || len < 0) {
      return Status::INVALID_PARAMETER;
    }
    burst->pkt_lens[seg][idx] = static_cast<uint32_t>(len);
    seg++;
  }
  return Status::SUCCESS;
}

void DocaEthMgr::release_rx_slot(BurstParams* burst, int pkt) {
  if (pkt < 0) { return; }
  auto ctx = get_rx_burst_context(burst);
  if (!ctx || pkt >= static_cast<int>(ctx->slot_ids.size())) { return; }

  const int32_t slot_id = ctx->slot_ids[pkt];
  if (slot_id < 0) { return; }

  auto q_it = impl_->rx_queues_.find(ctx->key);
  if (q_it == impl_->rx_queues_.end()) {
    ctx->slot_ids[pkt] = -1;
    return;
  }

  auto& qstate = q_it->second;
  auto& slot = qstate.slots[static_cast<size_t>(slot_id)];
  slot.state = RxSlot::State::IDLE;
  if (!impl_->submit_rx_slot(slot)) {
    DAQIRI_LOG_ERROR("Failed to re-submit RX task for port {} queue {} slot {}",
                     qstate.port_id,
                     qstate.queue_id,
                     slot_id);
  }

  ctx->slot_ids[pkt] = -1;
}

void DocaEthMgr::release_all_rx_slots(BurstParams* burst) {
  auto ctx = get_rx_burst_context(burst);
  if (!ctx) { return; }
  for (int i = 0; i < static_cast<int>(ctx->slot_ids.size()); ++i) { release_rx_slot(burst, i); }
}

void DocaEthMgr::release_tx_slot(BurstParams* burst, int pkt) {
  if (pkt < 0) { return; }
  auto ctx = get_tx_burst_context(burst);
  if (!ctx || pkt >= static_cast<int>(ctx->slot_ids.size())) { return; }

  const int32_t slot_id = ctx->slot_ids[pkt];
  if (slot_id < 0) { return; }

  auto q_it = impl_->tx_queues_.find(ctx->key);
  if (q_it != impl_->tx_queues_.end()) {
    auto& qstate = q_it->second;
    std::lock_guard<std::mutex> guard(qstate.lock);
    qstate.free_slots.push_back(static_cast<uint32_t>(slot_id));
  }

  ctx->slot_ids[pkt] = -1;
}

void DocaEthMgr::release_all_tx_slots(BurstParams* burst) {
  auto ctx = get_tx_burst_context(burst);
  if (!ctx) { return; }

  for (int i = 0; i < static_cast<int>(ctx->slot_ids.size()); ++i) { release_tx_slot(burst, i); }
}

void DocaEthMgr::free_packet_segment(BurstParams* burst, int seg, int pkt) {
  (void)seg;
  if (burst == nullptr) { return; }
  if ((burst->hdr.hdr.burst_flags & kBurstFlagRxContext) != 0U) {
    release_rx_slot(burst, pkt);
  } else if ((burst->hdr.hdr.burst_flags & kBurstFlagTxContext) != 0U) {
    release_tx_slot(burst, pkt);
  }
}

void DocaEthMgr::free_packet(BurstParams* burst, int pkt) {
  free_packet_segment(burst, 0, pkt);
}

void DocaEthMgr::free_all_segment_packets(BurstParams* burst, int seg) {
  if (seg != 0 || burst == nullptr) { return; }
  free_all_packets(burst);
}

void DocaEthMgr::free_all_packets(BurstParams* burst) {
  if (burst == nullptr) { return; }
  if ((burst->hdr.hdr.burst_flags & kBurstFlagTxContext) != 0U) {
    release_all_tx_slots(burst);
    return;
  }

  if ((burst->hdr.hdr.burst_flags & kBurstFlagRxContext) != 0U) { release_all_rx_slots(burst); }
}

void DocaEthMgr::free_rx_burst(BurstParams* burst) {
  if (burst == nullptr) { return; }
  release_all_rx_slots(burst);

  for (int seg = 0; seg < MAX_NUM_SEGS; ++seg) {
    delete[] burst->pkts[seg];
    burst->pkts[seg] = nullptr;
    delete[] burst->pkt_lens[seg];
    burst->pkt_lens[seg] = nullptr;
  }

  if (burst->pkt_extra_info != nullptr) {
    delete[] reinterpret_cast<uint16_t*>(burst->pkt_extra_info);
    burst->pkt_extra_info = nullptr;
  }

  delete burst;
}

void DocaEthMgr::free_tx_burst(BurstParams* burst) {
  if (burst == nullptr) { return; }
  release_all_tx_slots(burst);

  for (int seg = 0; seg < MAX_NUM_SEGS; ++seg) {
    delete[] burst->pkts[seg];
    burst->pkts[seg] = nullptr;
    delete[] burst->pkt_lens[seg];
    burst->pkt_lens[seg] = nullptr;
  }

  delete burst;
}

Status DocaEthMgr::set_packet_tx_time(BurstParams* burst, int idx, uint64_t time) {
  if (idx < 0) { return Status::INVALID_PARAMETER; }
  auto ctx = get_tx_burst_context(burst);
  if (!ctx || idx >= static_cast<int>(ctx->tx_times_ns.size())) { return Status::INVALID_PARAMETER; }
  ctx->tx_times_ns[idx] = time;
  return Status::SUCCESS;
}

Status DocaEthMgr::get_rx_burst(BurstParams** burst, int port, int q) {
  if (burst == nullptr) { return Status::NULL_PTR; }
  *burst = nullptr;

  const uint32_t key = queue_key(static_cast<uint16_t>(port), static_cast<uint16_t>(q));
  auto q_it = impl_->rx_queues_.find(key);
  if (q_it == impl_->rx_queues_.end()) { return Status::INVALID_PARAMETER; }

  auto& qstate = q_it->second;
  impl_->progress_rx_queue(qstate);

  auto port_it = impl_->ports_.find(static_cast<uint16_t>(port));
  if (port_it != impl_->ports_.end() && port_it->second.flow.drop_traffic) {
    std::vector<uint32_t> to_resubmit;
    {
      std::lock_guard<std::mutex> guard(qstate.lock);
      while (!qstate.completed_slots.empty()) {
        to_resubmit.push_back(qstate.completed_slots.front());
        qstate.completed_slots.pop_front();
      }
    }
    for (uint32_t sid : to_resubmit) {
      auto& slot = qstate.slots[sid];
      slot.state = RxSlot::State::IDLE;
      (void)impl_->submit_rx_slot(slot);
    }
    return Status::NOT_READY;
  }

  std::vector<uint32_t> selected_slots;
  selected_slots.reserve(qstate.batch_size);

  {
    std::lock_guard<std::mutex> guard(qstate.lock);
    while (!qstate.completed_slots.empty() && selected_slots.size() < qstate.batch_size) {
      selected_slots.push_back(qstate.completed_slots.front());
      qstate.completed_slots.pop_front();
    }
  }

  if (selected_slots.empty()) { return Status::NOT_READY; }

  auto* out = new BurstParams{};
  out->hdr.hdr.port_id = static_cast<uint16_t>(port);
  out->hdr.hdr.q_id = static_cast<uint16_t>(q);
  out->hdr.hdr.num_segs = static_cast<int>(qstate.num_segs);
  out->hdr.hdr.num_pkts = selected_slots.size();
  out->hdr.hdr.nbytes = 0;

  const int num_pkts = static_cast<int>(selected_slots.size());
  for (int seg = 0; seg < out->hdr.hdr.num_segs && seg < MAX_NUM_SEGS; ++seg) {
    out->pkts[seg] = new void*[num_pkts]();
    out->pkt_lens[seg] = new uint32_t[num_pkts]();
  }

  auto rx_ctx = std::make_shared<RxBurstContext>();
  rx_ctx->key = key;
  rx_ctx->slot_ids.resize(num_pkts, -1);
  rx_ctx->flow_ids.resize(num_pkts, 0);

  auto* flow_ids = new uint16_t[num_pkts]();

  for (int i = 0; i < num_pkts; ++i) {
    auto& slot = qstate.slots[selected_slots[static_cast<size_t>(i)]];
    slot.state = RxSlot::State::IN_APP;

    rx_ctx->slot_ids[i] = static_cast<int32_t>(slot.slot_id);
    rx_ctx->flow_ids[i] = slot.flow_id;
    flow_ids[i] = slot.flow_id;

    for (size_t seg = 0; seg < qstate.num_segs; ++seg) {
      out->pkts[seg][i] = slot.seg_ptrs[seg];
      out->pkt_lens[seg][i] = slot.seg_lens[seg];
    }

    out->hdr.hdr.nbytes += slot.total_len;
  }

  out->pkt_extra_info = reinterpret_cast<void**>(flow_ids);
  out->hdr.hdr.burst_flags = kBurstFlagRxContext;
  out->custom_pkt_data = std::static_pointer_cast<void>(rx_ctx);

  impl_->rx_packets_.fetch_add(out->hdr.hdr.num_pkts, std::memory_order_relaxed);
  impl_->rx_bytes_.fetch_add(out->hdr.hdr.nbytes, std::memory_order_relaxed);

  *burst = out;
  return Status::SUCCESS;
}

void DocaEthMgr::free_rx_metadata(BurstParams* burst) {
  free_rx_burst(burst);
}

void DocaEthMgr::free_tx_metadata(BurstParams* burst) {
  free_tx_burst(burst);
}

Status DocaEthMgr::get_tx_metadata_buffer(BurstParams** burst) {
  if (burst == nullptr) { return Status::NULL_PTR; }
  *burst = new BurstParams{};
  (*burst)->hdr.hdr.burst_flags = 0;
  return Status::SUCCESS;
}

Status DocaEthMgr::send_tx_burst(BurstParams* burst) {
  auto ctx = get_tx_burst_context(burst);
  if (!ctx) { return Status::INVALID_PARAMETER; }

  auto q_it = impl_->tx_queues_.find(ctx->key);
  if (q_it == impl_->tx_queues_.end()) {
    release_all_tx_slots(burst);
    free_tx_burst(burst);
    return Status::INVALID_PARAMETER;
  }

  auto& qstate = q_it->second;
  Status ret_status = Status::SUCCESS;
  std::vector<int> submitted_idx;
  submitted_idx.reserve(ctx->slot_ids.size());
  std::vector<uint64_t> pkt_totals(ctx->slot_ids.size(), 0);

  for (int i = 0; i < static_cast<int>(ctx->slot_ids.size()); ++i) {
    const int32_t slot_id = ctx->slot_ids[i];
    if (slot_id < 0) { continue; }

    auto& slot = qstate.slots[static_cast<size_t>(slot_id)];

    const uint64_t tx_time = ctx->tx_times_ns[static_cast<size_t>(i)];
    if (tx_time > 0) {
      const uint64_t cur = now_ns();
      if (tx_time > cur) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(tx_time - cur));
      }
    }

    uint64_t pkt_total = 0;
    for (size_t seg = 0; seg < qstate.num_segs; ++seg) {
      if (burst->pkt_lens[seg] == nullptr) {
        ret_status = Status::INVALID_PARAMETER;
        break;
      }

      const uint32_t len = burst->pkt_lens[seg][i];
      if (len > slot.seg_caps[seg]) {
        DAQIRI_LOG_ERROR("TX packet length {} exceeds segment capacity {} (port {} queue {})",
                         len,
                         slot.seg_caps[seg],
                         qstate.port_id,
                         qstate.queue_id);
        ret_status = Status::INVALID_PARAMETER;
        break;
      }

      slot.seg_lens[seg] = len;
      const auto status = doca_buf_set_data(slot.seg_bufs[seg], slot.seg_ptrs[seg], len);
      if (status != DOCA_SUCCESS) {
        log_doca_status("doca_buf_set_data failed", status);
        ret_status = Status::GENERIC_FAILURE;
        break;
      }

      pkt_total += len;
    }

    if (ret_status != Status::SUCCESS) {
      release_tx_slot(burst, i);
      continue;
    }

    union doca_data user_data{};
    user_data.ptr = &slot;

    struct doca_eth_txq_task_send* task_send = nullptr;
    auto status = doca_eth_txq_task_send_allocate_init(qstate.txq, slot.head_buf, user_data, &task_send);
    if (status != DOCA_SUCCESS) {
      log_doca_status("doca_eth_txq_task_send_allocate_init failed", status);
      ret_status = Status::NO_FREE_PACKET_BUFFERS;
      release_tx_slot(burst, i);
      continue;
    }

    slot.tx_status = 0;
    status = doca_task_submit(doca_eth_txq_task_send_as_doca_task(task_send));
    if (status != DOCA_SUCCESS) {
      log_doca_status("doca_task_submit failed (tx)", status);
      doca_task_free(doca_eth_txq_task_send_as_doca_task(task_send));
      ret_status = Status::GENERIC_FAILURE;
      release_tx_slot(burst, i);
      continue;
    }

    pkt_totals[static_cast<size_t>(i)] = pkt_total;
    submitted_idx.push_back(i);
  }

  bool pending = !submitted_idx.empty();
  while (pending) {
    pending = false;
    impl_->progress_tx_queue(qstate);
    for (int idx : submitted_idx) {
      const int32_t sid = ctx->slot_ids[static_cast<size_t>(idx)];
      if (sid < 0) { continue; }
      if (qstate.slots[static_cast<size_t>(sid)].tx_status == 0) {
        pending = true;
      }
    }
  }

  for (int idx : submitted_idx) {
    const int32_t sid = ctx->slot_ids[static_cast<size_t>(idx)];
    if (sid < 0) { continue; }
    auto& slot = qstate.slots[static_cast<size_t>(sid)];
    if (slot.tx_status != 1) {
      ret_status = Status::GENERIC_FAILURE;
    } else {
      impl_->tx_packets_.fetch_add(1, std::memory_order_relaxed);
      impl_->tx_bytes_.fetch_add(pkt_totals[static_cast<size_t>(idx)], std::memory_order_relaxed);
    }
    release_tx_slot(burst, idx);
  }

  free_tx_burst(burst);
  return ret_status;
}

Status DocaEthMgr::get_mac_addr(int port, char* mac) {
  if (mac == nullptr) { return Status::NULL_PTR; }

  auto it = impl_->ports_.find(static_cast<uint16_t>(port));
  if (it == impl_->ports_.end()) { return Status::INVALID_PARAMETER; }

  std::memcpy(mac, it->second.mac.data(), it->second.mac.size());
  return Status::SUCCESS;
}

Status DocaEthMgr::drop_all_traffic(int port) {
  auto it = impl_->ports_.find(static_cast<uint16_t>(port));
  if (it == impl_->ports_.end()) { return Status::INVALID_PARAMETER; }

  it->second.flow.drop_traffic = true;
  return Status::SUCCESS;
}

Status DocaEthMgr::allow_all_traffic(int port) {
  auto it = impl_->ports_.find(static_cast<uint16_t>(port));
  if (it == impl_->ports_.end()) { return Status::INVALID_PARAMETER; }

  it->second.flow.drop_traffic = false;
  return Status::SUCCESS;
}

void DocaEthMgr::shutdown() {
  if (num_init_ > 0) {
    num_init_--;
    if (num_init_ > 0) { return; }
  }

  impl_->shutdown();
  initialized_ = false;
}

void DocaEthMgr::print_stats() {
  DAQIRI_LOG_INFO("daqiri doca_eth manager stats");
  DAQIRI_LOG_INFO("  TX packets: {}", impl_->tx_packets_.load(std::memory_order_relaxed));
  DAQIRI_LOG_INFO("  TX bytes:   {}", impl_->tx_bytes_.load(std::memory_order_relaxed));
  DAQIRI_LOG_INFO("  RX packets: {}", impl_->rx_packets_.load(std::memory_order_relaxed));
  DAQIRI_LOG_INFO("  RX bytes:   {}", impl_->rx_bytes_.load(std::memory_order_relaxed));
}

uint64_t DocaEthMgr::get_burst_tot_byte(BurstParams* burst) {
  if (burst == nullptr) { return 0; }
  if (burst->hdr.hdr.nbytes != 0) { return burst->hdr.hdr.nbytes; }

  uint64_t total = 0;
  for (size_t i = 0; i < burst->hdr.hdr.num_pkts; ++i) {
    total += get_packet_length(burst, static_cast<int>(i));
  }
  return total;
}

BurstParams* DocaEthMgr::create_tx_burst_params() {
  return new BurstParams{};
}

bool DocaEthMgr::validate_config() const {
  return Manager::validate_config();
}

uint16_t DocaEthMgr::get_num_rx_queues(int port_id) const {
  const auto it = impl_->port_num_rx_queues_.find(port_id);
  if (it == impl_->port_num_rx_queues_.end()) { return 0; }
  return it->second;
}

}  // namespace daqiri
