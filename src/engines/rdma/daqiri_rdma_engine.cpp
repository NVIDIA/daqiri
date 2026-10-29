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

#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <mqueue.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <map>
#include "src/daqiri_ring.h"
#include "src/daqiri_pool.h"
#include "src/metrics.h"
#include "daqiri_rdma_engine.h"
#include "rdma_queue_index.h"

/* The ordering of most RDMA/CM setup follows the ordering specified here:
   https://man7.org/linux/man-pages/man7/rdma_cm.7.html
   Applications exchange memory descriptors through their own control channel.
*/

#include "src/rdma/cm_api.h"

namespace daqiri {
std::atomic<bool> rdma_force_quit = false;

namespace {

constexpr std::array<detail::RdmaClientQueueAssignment, 1> kQueueOnInterfaceZero{{{0, 0}}};
static_assert(detail::first_available_rdma_client_queue(1, 1, kQueueOnInterfaceZero) == 0);
static_assert(detail::first_available_rdma_client_queue(0, 2, kQueueOnInterfaceZero) == 1);

constexpr std::array<detail::RdmaClientQueueAssignment, 2> kQueuesZeroAndTwo{{{0, 0}, {0, 2}}};
static_assert(detail::first_available_rdma_client_queue(0, 3, kQueuesZeroAndTwo) == 1);

constexpr std::array<detail::RdmaClientQueueAssignment, 2> kTwoQueuesOnInterfaceZero{
    {{0, 0}, {0, 1}}};
static_assert(detail::first_available_rdma_client_queue(0, 2, kTwoQueuesOnInterfaceZero) == -1);

void reset_rdma_burst_metadata(BurstParams* burst) {
  if (burst == nullptr) { return; }

  burst->transport_hdr = BurstTransportHeader{};
  burst->pkts.fill(nullptr);
  burst->pkt_lens.fill(nullptr);
  burst->pkt_extra_info = nullptr;
  burst->event = nullptr;
}

std::shared_ptr<metrics::CounterSet> get_rdma_metrics(const NetworkConfig& cfg,
                                                      const rdma_thread_params& params) {
  if (params.if_idx < 0 || params.if_idx >= static_cast<int>(cfg.ifs_.size())) { return nullptr; }

  const auto& intf = cfg.ifs_[params.if_idx];
  auto queue_id = params.queue_idx;
  if (params.queue_idx >= 0 && params.queue_idx < static_cast<int>(intf.tx_.queues_.size())) {
    queue_id = intf.tx_.queues_[params.queue_idx].common_.id_;
  }

  return metrics::get_or_create_queue("rdma",
                                      intf.name_.empty() ? intf.address_ : intf.name_,
                                      intf.port_id_,
                                      std::to_string(queue_id));
}

}  // namespace

bool RdmaEngine::set_config_and_initialize(const NetworkConfig& cfg) {
  if (initialized_) {
    DAQIRI_LOG_ERROR("RDMA engine is already initialized; call shutdown() first");
    return false;
  }

  DAQIRI_LOG_INFO("Setting up RDMA engine");
  rdma_force_quit.store(false, std::memory_order_relaxed);
  cfg_ = cfg;
  if (!validate_config()) {
    DAQIRI_LOG_CRITICAL("Config validation failed");
    return false;
  }
  initialize();

  return initialized_;
}

// Common DAQIRI functions
Status RdmaEngine::set_packet_lengths(BurstParams* burst, int idx,
                                   const std::initializer_list<int>& lens) {
  assert(lens.size() == 1);  // Split not supported yet
  burst->pkt_lens[0][idx] = lens.begin()[0];
  return Status::SUCCESS;
}

void* RdmaEngine::get_segment_packet_ptr(BurstParams* burst, int seg, int idx) {
  return burst->pkts[seg][idx];
}

void* RdmaEngine::get_packet_ptr(BurstParams* burst, int idx) {
  return burst->pkts[0][idx];
}

uint32_t RdmaEngine::get_segment_packet_length(BurstParams* burst, int seg, int idx) {
  return burst->pkt_lens[seg][idx];
}

uint32_t RdmaEngine::get_packet_length(BurstParams* burst, int idx) {
  return burst->pkt_lens[0][idx];
}

Status RdmaEngine::set_eth_header(BurstParams* burst, int idx, char* dst_addr) {
  DAQIRI_LOG_CRITICAL("Cannot set Ethernet header in RDMA mode");
  return Status::NOT_SUPPORTED;
}

Status RdmaEngine::set_ipv4_header(BurstParams* burst, int idx, int ip_len, uint8_t proto,
                                unsigned int src_host, unsigned int dst_host) {
  DAQIRI_LOG_CRITICAL("Cannot set IPv4 header in RDMA mode");
  return Status::NOT_SUPPORTED;
}

Status RdmaEngine::set_udp_header(BurstParams* burst, int idx, int udp_len, uint16_t src_port,
                               uint16_t dst_port) {
  DAQIRI_LOG_CRITICAL("Cannot set UDP header in RDMA mode");
  return Status::NOT_SUPPORTED;
}

Status RdmaEngine::set_udp_payload(BurstParams* burst, int idx, void* data, int len) {
  memcpy(burst->pkts[0][idx], data, len);
  return Status::SUCCESS;
}

uint64_t RdmaEngine::get_burst_tot_byte(BurstParams* burst) {
  return 0;
}

BurstParams* RdmaEngine::create_tx_burst_params() {
  BurstParams* burst = nullptr;
  if (!tx_meta->get(reinterpret_cast<void**>(&burst))) {
    DAQIRI_LOG_CRITICAL("Failed to get RDMA TX meta descriptor");
    return nullptr;
  }

  reset_rdma_burst_metadata(burst);
  rdma_burst_cache(burst) = {};
  return burst;
}

bool RdmaEngine::get_ip_from_interface(const std::string_view& if_name, sockaddr_in& addr) {
  struct ifaddrs *ifaddr, *ifa;
  bool found = false;

  // Initialize the sockaddr_in structure
  memset(&addr, 0, sizeof(sockaddr_in));
  addr.sin_family = AF_INET;

  // Get the list of network interfaces
  if (getifaddrs(&ifaddr) == -1) {
    DAQIRI_LOG_CRITICAL("Failed to get a list of interfaces");
    return false;
  }

  // Loop through the list of interfaces
  for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == nullptr) {
      DAQIRI_LOG_CRITICAL("Interface {} has no address", ifa->ifa_name);
      continue;
    }

    if (ifa->ifa_addr->sa_family != AF_INET) {
      continue;  // Only IPv4 for now
    }

    // Check if the interface name matches
    if (if_name == ifa->ifa_name) {
      struct sockaddr_in* in_addr = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
      addr.sin_addr = in_addr->sin_addr;

      found = true;
      break;
    }
  }

  freeifaddrs(ifaddr);
  return found;
}

inline bool RdmaEngine::ack_event(rdma_cm_event* cm_event) {
  int ret = verbs_->cm_ack_cm_event(cm_event);
  if (ret != 0) {
    DAQIRI_LOG_ERROR("Failed to acknowledge CM event: {}", ret);
    return false;
  } else {
    DAQIRI_LOG_INFO("Acknowledged CM event: {}", verbs_->cm_event_str(cm_event->event));
  }
  return true;
}

void RdmaEngine::launch_worker(rdma_thread_params* params, bool is_server) {
  if (params == nullptr || params->client_id == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(threads_mutex_);
  if (connection_params_.find(params->client_id) != connection_params_.end()) {
    return;
  }
  initialize_progress(is_server, params);
  connection_params_[params->client_id] = params;
  if (params->direct_rx && params->direct_tx) {
    if (!publish_direct(params)) {
      connection_params_.erase(params->client_id);
      params->ready_to_exit.store(true);
      return;
    }
    params->worker_started.store(true);
    return;
  }
  params->worker_started.store(false);
  worker_threads_[params->client_id] =
      std::thread(&RdmaEngine::rdma_thread, this, is_server, params);
}

int RdmaEngine::mr_access_to_ibv(uint32_t access) {
  int ibv_access = 0;

  if (access & MEM_ACCESS_LOCAL) { ibv_access |= IBV_ACCESS_LOCAL_WRITE; }
  if (access & MEM_ACCESS_RDMA_READ) { ibv_access |= IBV_ACCESS_REMOTE_READ; }
  if (access & MEM_ACCESS_RDMA_WRITE) {
    ibv_access |= IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE;
  }
  // IBV_ACCESS_RELAXED_ORDERING is an rdma-core enum value, not a macro, so an
  // #ifdef guard compiled it out. Keep it opt-in per memory region: it has caused
  // local protection errors on some aarch64/Tegra platforms.
  if (access & MEM_ACCESS_RELAXED_ORDERING) {
    ibv_access |= IBV_ACCESS_RELAXED_ORDERING;
  }

  return ibv_access;
}

int RdmaEngine::rdma_register_mr(const MemoryRegionConfig& mr, void* ptr) {
  rdma_mr_params params{};
  params.params_ = mr;
  params.ptr_ = ptr;
  DAQIRI_LOG_INFO("Registering MR {} with ibverbs {}", mr.name_, pd_map_.size());
  // For now register the MR with every PD we have
  for (const auto& pd : pd_map_) {
    if (pd.second != nullptr) {
      int access = mr_access_to_ibv(mr.access_);
      params.ctx_mr_map_[pd.second] =
          verbs_->reg_mr(pd.second, ptr, mr.adj_size_ * mr.num_bufs_, access);
      if (params.ctx_mr_map_[pd.second] == nullptr &&
          (access & IBV_ACCESS_RELAXED_ORDERING) != 0) {
        const int fallback_access = access & ~IBV_ACCESS_RELAXED_ORDERING;
        DAQIRI_LOG_WARN(
            "Failed to register MR {} with relaxed ordering on PD {}; retrying without it",
            mr.name_,
            (void*)pd.second);
        params.ctx_mr_map_[pd.second] =
            verbs_->reg_mr(pd.second, ptr, mr.adj_size_ * mr.num_bufs_, fallback_access);
        access = fallback_access;
      }
      if (params.ctx_mr_map_[pd.second] == nullptr) {
        DAQIRI_LOG_CRITICAL("Failed to register MR {} on PD {}", mr.name_, (void*)pd.second);
        for (auto& [registered_pd, registered_mr] : params.ctx_mr_map_) {
          (void)registered_pd;
          if (registered_mr != nullptr) {
            verbs_->dereg_mr(registered_mr);
          }
        }
        return -1;
      } else {
        DAQIRI_LOG_INFO(
            "Successfully registered MR {} with {} bytes on PD {} ptr {}-{} lkey {} access {}",
            mr.name_,
            mr.buf_size_ * mr.num_bufs_,
            (void*)pd.second,
            (void*)ptr,
            (void*)((uint8_t*)ptr + mr.adj_size_ * mr.num_bufs_),
            params.ctx_mr_map_[pd.second]->lkey,
            access);
      }
    }
  }

  mrs_[mr.name_] = params;

  return 0;
}

int RdmaEngine::rdma_register_cfg_mrs() {
  DAQIRI_LOG_INFO("Registering memory regions");

  if (allocate_memory_regions() != Status::SUCCESS) {
    DAQIRI_LOG_CRITICAL("Failed to allocate memory");
    return -1;
  }

  for (const auto& mr : cfg_.mrs_) {
    // Allocate 1 more than the user specifies because the ring usable capacity is size - 1.
    // Multi-producer/consumer: app threads and the RDMA worker both touch this buffer pool.
    auto ring = daqiri::Ring::create(mr.second.name_.c_str(), mr.second.num_bufs_ + 1,
                                     daqiri::RingMode::MPMC,
                                     daqiri::detail::numa_node_for_cpu(cfg_.common_.master_core_));

    if (ring == nullptr) {
      DAQIRI_LOG_CRITICAL("Failed to create ring for MR {}", mr.second.name_);
      return -1;
    }

    mem_pools_[mr.second.name_] = ring;
    DAQIRI_LOG_INFO("Created mempool for MR {}", mr.second.name_);

    if (populate_pool(ring, mr.second.name_) != Status::SUCCESS) {
      DAQIRI_LOG_CRITICAL("Failed to populate pool for MR {}", mr.second.name_);
      return -1;
    }

    int ret = rdma_register_mr(mr.second, ar_[mr.second.name_].ptr_);
    if (ret < 0) {
      DAQIRI_LOG_CRITICAL("Failed to register MR {} with {} bytes at {}",
                            mr.second.name_,
                            mr.second.buf_size_ * mr.second.num_bufs_,
                            (void*)ar_[mr.second.name_].ptr_);
      return ret;
    }
  }

  return 0;
}

Status RdmaEngine::wait_on_key_xchg() {
  return Status::SUCCESS;
}

/**
 * Set up all parameters needed for a newly-connected client
 */
int RdmaEngine::setup_thread_params(rdma_thread_params* params, bool is_server) {
  // A connection uses paired RX/TX queue indices.
  rdma_qp_params qp_params{};
  const auto& intf = cfg_.ifs_[params->if_idx];
  if (params->queue_idx >= static_cast<int>(intf.tx_.queues_.size())) {
    DAQIRI_LOG_ERROR("RDMA connections require a paired TX queue");
    return -1;
  }
  params->direct_state = nullptr;
  params->direct_tx = intf.tx_.queues_[params->queue_idx].poll_mode_ == QueuePollMode::DIRECT;
  params->direct_rx = params->queue_idx < static_cast<int>(intf.rx_.queues_.size()) &&
                      intf.rx_.queues_[params->queue_idx].poll_mode_ == QueuePollMode::DIRECT;
  const int port_id = cfg_.ifs_[static_cast<int>(params->if_idx)].port_id_;

  DAQIRI_LOG_INFO("Creating RX and TX CQs for client ID {}", (void*)params->client_id);
  qp_params.rx_cq = verbs_->create_cq(params->client_id->verbs, MAX_CQ, nullptr, nullptr, 0);
  if (qp_params.rx_cq == nullptr) {
    DAQIRI_LOG_CRITICAL("Failed to create RX queue pair! {}", strerror(errno));
    return -1;
  }

  qp_params.tx_cq = verbs_->create_cq(params->client_id->verbs, MAX_CQ, nullptr, nullptr, 0);
  if (qp_params.tx_cq == nullptr) {
    DAQIRI_LOG_CRITICAL("Failed to create TX queue pair! {}", strerror(errno));
    return -1;
  } else {
    DAQIRI_LOG_INFO("Successfully created TX CQ for client ID {}", (void*)params->client_id);
  }

  memset(&qp_params.qp_attr, 0, sizeof(qp_params.qp_attr));
  qp_params.qp_attr.cap.max_recv_sge = 1;  // No header-data split in RDMA right now
  qp_params.qp_attr.cap.max_recv_wr = MAX_OUSTANDING_WR;
  qp_params.qp_attr.cap.max_send_sge = 1;
  qp_params.qp_attr.cap.max_send_wr = MAX_OUSTANDING_WR;
  if (params->direct_tx) {
    qp_params.qp_attr.cap.max_inline_data = 128;
  }

  if (cfg_.ifs_[static_cast<int>(params->if_idx)].rdma_.xmode_ == RDMATransportMode::RC) {
    qp_params.qp_attr.qp_type = IBV_QPT_RC;
  } else if (cfg_.ifs_[static_cast<int>(params->if_idx)].rdma_.xmode_ == RDMATransportMode::UC) {
    qp_params.qp_attr.qp_type = IBV_QPT_UC;
  } else if (cfg_.ifs_[static_cast<int>(params->if_idx)].rdma_.xmode_ == RDMATransportMode::UD) {
    qp_params.qp_attr.qp_type = IBV_QPT_UD;
  } else {
    DAQIRI_LOG_ERROR("RDMA transport mode {} not supported!",
                     static_cast<int>(cfg_.ifs_[static_cast<int>(params->if_idx)].rdma_.xmode_));
    return -1;
  }

  // Share the CQ between TX and RX
  qp_params.qp_attr.recv_cq = qp_params.rx_cq;
  qp_params.qp_attr.send_cq = qp_params.tx_cq;

  DAQIRI_LOG_INFO("Creating QP for client ID {} with PD {}", (void*)params->client_id,
                  (void*)params->pd);
  int ret = verbs_->cm_create_qp(params->client_id, params->pd, &qp_params.qp_attr);
  if (ret != 0) {
    DAQIRI_LOG_CRITICAL("Failed to create QP: {}", strerror(errno));
    return -1;
  }

  if (qp_params.qp_attr.qp_type == IBV_QPT_RC) {
    ibv_qp_attr access{};
    access.qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
    if (verbs_->modify_qp(params->client_id->qp, &access, IBV_QP_ACCESS_FLAGS) != 0) {
      DAQIRI_LOG_ERROR("Failed to enable RDMA READ/WRITE on QP: {}", strerror(errno));
      verbs_->cm_destroy_qp(params->client_id);
      verbs_->destroy_cq(qp_params.rx_cq);
      verbs_->destroy_cq(qp_params.tx_cq);
      return -1;
    }
  }

  params->max_inline_data = qp_params.qp_attr.cap.max_inline_data;
  params->inline_wqes = 0;
  // qp_params.rx_ring = rx_ring;
  qp_params.tx_ring = tx_rings_.front();
  tx_rings_.pop();
  tx_rings_map_[params->client_id] = qp_params.tx_ring;

  qp_params.rx_ring = rx_rings_.front();
  rx_rings_.pop();
  rx_rings_map_[params->client_id] = qp_params.rx_ring;

  params->qp_params = qp_params;
  DAQIRI_LOG_INFO("Successfully configured queues for client {}", (void*)params->client_id);

  return 0;
}

int RdmaEngine::destroy_thread_params(rdma_thread_params* params) {
  DAQIRI_LOG_INFO("Destroying thread params for client {}", (void*)params->client_id);
  // First destroy the QP
  if (params->client_id->qp) {
    verbs_->cm_destroy_qp(params->client_id);
  }

  // Then destroy the CQs
  if (params->qp_params.tx_cq) {
    verbs_->destroy_cq(params->qp_params.tx_cq);
    params->qp_params.tx_cq = nullptr;
  }

  if (params->qp_params.rx_cq) {
    verbs_->destroy_cq(params->qp_params.rx_cq);
    params->qp_params.rx_cq = nullptr;
  }

  client_params_mutex_.lock();
  client_q_params_.erase(params->client_id);
  client_params_mutex_.unlock();

  return 0;
}

inline int RdmaEngine::set_affinity(int cpu_core) {
  // Set the CPU affinity of our thread
  cpu_set_t cpuset;

  CPU_ZERO(&cpuset);
  CPU_SET(cpu_core, &cpuset);
  if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) != 0) {
    DAQIRI_LOG_CRITICAL("Failed to set thread affinity to core {}", cpu_core);
    return -1;
  }

  return 0;
}

Status RdmaEngine::send_tx_burst(BurstParams* burst) {
  if (burst == nullptr) {
    return Status::INVALID_PARAMETER;
  }

  daqiri::Ring* ring;

  const auto conn_id = get_connection_id(burst);
  auto& cache = rdma_burst_cache(burst);
  auto* direct = cache.magic == RdmaBurstCache::kMagic && cache.engine == this && cache.state &&
                         cache.state->connection_id == conn_id
                     ? cache.state
                     : find_direct(conn_id);
  if (direct) {
    RdmaDirectLease lease(direct);
    return lease ? send_direct(*direct, burst) : Status::NOT_READY;
  }
  std::unique_lock<std::mutex> registry_lock(threads_mutex_);
  const auto connection = connection_params_.find(reinterpret_cast<struct rdma_cm_id*>(conn_id));
  if (connection == connection_params_.end()) {
    return Status::NOT_READY;
  }
  auto* params = connection->second;
  std::unique_lock<std::mutex> progress_lock(params->progress_mutex);
  registry_lock.unlock();
  if (rdma_force_quit.load() || params->ready_to_exit.load()) {
    return Status::NOT_READY;
  }
  const auto opcode = burst->transport_hdr.opcode;
  if (opcode != RDMAOpCode::SEND && opcode != RDMAOpCode::RECEIVE &&
      opcode != RDMAOpCode::RDMA_READ && opcode != RDMAOpCode::RDMA_WRITE) {
    return Status::NOT_SUPPORTED;
  }
  if (opcode == RDMAOpCode::RDMA_READ || opcode == RDMAOpCode::RDMA_WRITE) {
    const auto status = validate_one_sided(*params, burst);
    if (status != Status::SUCCESS) {
      return status;
    }
  }
  const bool receive = burst->transport_hdr.opcode == RDMAOpCode::RECEIVE;
  if (receive ? params->direct_rx : params->direct_tx) {
    if (burst->transport_hdr.num_pkts != 1 || burst->transport_hdr.num_segs != 1) {
      return Status::INVALID_PARAMETER;
    }
    poll_completions(burst->transport_hdr.server, params, params->direct_rx, params->direct_tx);
    return post_burst(params, burst);
  }
  ring = params->qp_params.tx_ring;

  if (!ring->enqueue(reinterpret_cast<void*>(burst))) {
    auto counters = get_rdma_metrics(cfg_, *params);
    const auto dropped_packets =
        burst->transport_hdr.num_pkts > 0 ? burst->transport_hdr.num_pkts : 1;
    metrics::add_dropped(counters, "tx_enqueue_failure", dropped_packets);
    free_tx_burst(burst);
    DAQIRI_LOG_CRITICAL("Failed to enqueue TX work");
    return Status::NO_SPACE_AVAILABLE;
  }

  return Status::SUCCESS;
}

RDMAOpCode RdmaEngine::ibv_opcode_to_daqiri_opcode(ibv_wc_opcode opcode) {
  switch (opcode) {
    case IBV_WC_SEND:
      return RDMAOpCode::SEND;
    case IBV_WC_RECV:
      return RDMAOpCode::RECEIVE;
    case IBV_WC_RDMA_WRITE:
      return RDMAOpCode::RDMA_WRITE;
    case IBV_WC_RDMA_READ:
      return RDMAOpCode::RDMA_READ;
    default:
      return RDMAOpCode::INVALID;
  }
}

// Configure connection progress before publishing it to callers or workers.
void RdmaEngine::initialize_progress(bool is_server, rdma_thread_params* tparams) {
  tparams->send_signal_every = 1;
  if (const char* env = std::getenv("DAQIRI_RDMA_SEND_SIGNAL_EVERY")) {
    const long parsed = strtol(env, nullptr, 10);
    if (parsed > 0) {
      tparams->send_signal_every = static_cast<int>(parsed);
    }
  }
  tparams->sends_since_signal = 0;
  // rdma_cm leaves min_rnr_timer at 0, which the IB spec defines as 655.36 ms,
  // and sets rnr_retry to 7 (infinite). So a single RECV-queue underrun -- which
  // happens intermittently on fast small messages, where the sender briefly
  // outruns the responder's RECV reposting -- stalls the sender for 655 ms with
  // no CQE error and no drop. That is the intermittent small-message throughput
  // cliff (#126). Lower min_rnr_timer (as responder) so a transient underrun
  // costs sub-millisecond, not ~0.65 s. Env-overridable for tuning; the default
  // 12 == 0.64 ms is comfortably above the peer's RECV-repost latency, so it
  // avoids an RNR-retry storm while keeping any stall negligible.
  if (tparams->client_id != nullptr && tparams->client_id->qp != nullptr) {
    int rnr_timer = 12;  // IB min_rnr_timer encoding: 12 == 0.64 ms
    if (const char* env = std::getenv("DAQIRI_RDMA_MIN_RNR_TIMER")) {
      char* end = nullptr;
      const long parsed = strtol(env, &end, 10);
      // Require the whole string to parse as a valid 0-31 code. A bare strtol
      // would map non-numeric input (e.g. "abc") to 0, which is itself a valid
      // code meaning 655.36 ms -- silently reintroducing the very stall this
      // override exists to prevent. Reject and keep the default instead.
      if (end != env && *end == '\0' && parsed >= 0 && parsed <= 31) {
        rnr_timer = static_cast<int>(parsed);
      } else {
        DAQIRI_LOG_WARN(
            "Ignoring invalid DAQIRI_RDMA_MIN_RNR_TIMER='{}' (expected integer 0-31); "
            "using default {}",
            env, rnr_timer);
      }
    }
    struct ibv_qp_attr mod = {};
    mod.min_rnr_timer = static_cast<uint8_t>(rnr_timer);
    if (verbs_->modify_qp(tparams->client_id->qp, &mod, IBV_QP_MIN_RNR_TIMER) != 0) {
      DAQIRI_LOG_WARN("Failed to set min_rnr_timer on {} QP: {}", is_server ? "server" : "client",
                      strerror(errno));
    }

    // Confirm the negotiated retry/timeout attributes after the override.
    struct ibv_qp_attr qattr;
    struct ibv_qp_init_attr qinit;
    const int qmask = IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MIN_RNR_TIMER;
    if (verbs_->query_qp(tparams->client_id->qp, &qattr, qmask, &qinit) == 0) {
      const double ack_us = qattr.timeout == 0 ? 0.0 : 4.096 * (1U << qattr.timeout);
      DAQIRI_LOG_INFO(
          "[rdma-qp {}] timeout={} (local-ack ~{} us; 0=infinite) retry_cnt={} "
          "rnr_retry={} min_rnr_timer={}",
          is_server ? "server" : "client", static_cast<int>(qattr.timeout), ack_us,
          static_cast<int>(qattr.retry_cnt), static_cast<int>(qattr.rnr_retry),
          static_cast<int>(qattr.min_rnr_timer));
    }
  }
}

void RdmaEngine::poll_completions(bool is_server, rdma_thread_params* tparams, bool poll_rx,
                                  bool poll_tx) {
  struct ibv_wc wc;
  int num_comp;
  BurstParams* msg;
  auto* rx_ring = tparams->qp_params.rx_ring;
  auto& outstanding_send_wr_ids = tparams->outstanding_send_wr_ids;
  auto& outstanding_receive_wr_ids = tparams->outstanding_receive_wr_ids;
  auto counters = get_rdma_metrics(cfg_, *tparams);
  auto packet_length_for_wr = [](BurstParams* burst, uint64_t wr_id) -> uint64_t {
    if (burst == nullptr || burst->pkt_lens[0] == nullptr) {
      return 0;
    }
    if (wr_id < burst->transport_hdr.wr_id) {
      return 0;
    }
    const auto idx = static_cast<size_t>(wr_id - burst->transport_hdr.wr_id);
    if (idx >= burst->transport_hdr.num_pkts) {
      return 0;
    }
    return burst->pkt_lens[0][idx];
  };

  // Check RQ first to reduce latency
  for (int polled = 0; poll_rx && polled < MAX_CQ; ++polled) {
    num_comp = verbs_->poll_cq(tparams->qp_params.rx_cq, 1, &wc);
    if (num_comp <= 0) {
      if (num_comp < 0) {
        DAQIRI_LOG_ERROR("RDMA CQ polling failed: {}", num_comp);
      }
      break;
    }
    DAQIRI_LOG_DEBUG("GOT RX COMPLETION on connection {} queue {} wrid {}",
                     (void*)tparams->client_id, tparams->queue_idx, (int64_t)wc.wr_id);
    if (wc.status != IBV_WC_SUCCESS) {
      // WR_FLUSH_ERR / RETRY_EXC_ERR are the expected statuses for in-flight
      // WRs when the QP moves to ERR state during teardown (peer disconnect or
      // local shutdown). Demote to DEBUG only while shutting down, so a clean
      // teardown doesn't spam while an unexpected mid-run flush (e.g. abrupt
      // peer disconnect before CM teardown is observed) still surfaces.
      const bool shutting_down = rdma_force_quit.load() || tparams->ready_to_exit.load();
      const bool benign_teardown =
          shutting_down && (wc.status == IBV_WC_WR_FLUSH_ERR || wc.status == IBV_WC_RETRY_EXC_ERR);
      if (benign_teardown) {
        DAQIRI_LOG_DEBUG("RX CQ teardown status {} ({}) for WRID {} opcode {}",
                         verbs_->wc_status_str(wc.status), (int)wc.status, (int64_t)wc.wr_id,
                         (int)wc.opcode);
      } else {
        DAQIRI_LOG_ERROR("CQ error {} ({}) for WRID {} and opcode {}",
                         verbs_->wc_status_str(wc.status), (int)wc.status, (int64_t)wc.wr_id,
                         (int)wc.opcode);
      }
      metrics::add_dropped(counters, "rx_completion_error", 1);
    }

    if (wc.opcode == IBV_WC_RECV || wc.status != IBV_WC_SUCCESS) {
      auto it = outstanding_receive_wr_ids.find(wc.wr_id);
      if (it == outstanding_receive_wr_ids.end()) {
        DAQIRI_LOG_CRITICAL("WR ID {} not found in outstanding RECEIVE WR IDs", wc.wr_id);
        metrics::add_dropped(counters, "rx_unknown_wr_id", 1);
        continue;
      }

      msg = it->second;
      const auto packet = wc.wr_id - msg->transport_hdr.wr_id;
      if (wc.status == IBV_WC_SUCCESS && msg->pkt_lens[0] != nullptr &&
          packet < msg->transport_hdr.num_pkts) {
        msg->pkt_lens[0][packet] = wc.byte_len;
      }
      if (wc.status == IBV_WC_SUCCESS) {
        metrics::add_rx(counters, 1, static_cast<uint64_t>(wc.byte_len));
      }

      const auto conn_id = get_connection_id(msg);
      const auto expected_conn_id = reinterpret_cast<uintptr_t>(tparams->client_id);
      if (conn_id != expected_conn_id) {
        DAQIRI_LOG_CRITICAL("Wrong connection ID in receive completion {}: {} != {}", wc.wr_id,
                            conn_id, expected_conn_id);
      }

      outstanding_receive_wr_ids.erase(it);
    } else {
      msg = create_burst_params();
    }
    const bool msg_owns_packets = wc.opcode == IBV_WC_RECV || wc.status != IBV_WC_SUCCESS;

    // Only populate a header to indicate which burst needs to be freed
    // msg->transport_hdr.opcode  = ibv_opcode_to_daqiri_opcode(wc.opcode);
    msg->transport_hdr.status =
        wc.status == IBV_WC_SUCCESS ? Status::SUCCESS : Status::GENERIC_FAILURE;
    // set_connection_id(msg, reinterpret_cast<uintptr_t>(tparams->client_id));
    msg->transport_hdr.server = is_server;
    msg->transport_hdr.tx = false;
    // msg->transport_hdr.wr_id   = wc.wr_id;

    if (!rx_ring->enqueue(reinterpret_cast<void*>(msg))) {
      DAQIRI_LOG_CRITICAL("Failed to enqueue RX completion message");
      metrics::add_dropped(counters, "rx_completion_enqueue_failure", 1);
      if (msg_owns_packets) {
        free_tx_burst(msg);
      } else {
        free_tx_metadata(msg);
      }
      return;
    }
  }

  auto complete_send_wrs = [&](uint64_t wr_id, Status status) -> bool {
    const auto last =
        std::find(tparams->send_wr_order.begin(), tparams->send_wr_order.end(), wr_id);
    if (last == tparams->send_wr_order.end()) {
      DAQIRI_LOG_CRITICAL("WR ID {} not found in outstanding SQ work", wr_id);
      metrics::add_dropped(counters, "tx_unknown_wr_id", 1);
      return true;
    }
    const size_t count = std::distance(tparams->send_wr_order.begin(), last) + 1;
    for (size_t i = 0; i < count; ++i) {
      const auto completed_it = outstanding_send_wr_ids.find(tparams->send_wr_order.front());
      tparams->send_wr_order.pop_front();
      msg = completed_it->second;
      if (status == Status::SUCCESS) {
        metrics::add_tx(counters, 1, packet_length_for_wr(msg, completed_it->first));
      }

      const auto conn_id = get_connection_id(msg);
      const auto expected_conn_id = reinterpret_cast<uintptr_t>(tparams->client_id);
      if (conn_id != expected_conn_id) {
        DAQIRI_LOG_CRITICAL("Wrong connection ID in send completion {}: {} != {}",
                            completed_it->first, conn_id, expected_conn_id);
      }

      outstanding_send_wr_ids.erase(completed_it);

      msg->transport_hdr.tx = true;
      msg->transport_hdr.status = status;
      msg->transport_hdr.server = is_server;

      if (!rx_ring->enqueue(reinterpret_cast<void*>(msg))) {
        DAQIRI_LOG_CRITICAL("Failed to enqueue RX completion message");
        metrics::add_dropped(counters, "tx_completion_enqueue_failure", 1);
        free_tx_burst(msg);
        return false;
      }
    }

    return true;
  };

  // Check TX CQ for completion
  for (int polled = 0; poll_tx && polled < MAX_CQ; ++polled) {
    num_comp = verbs_->poll_cq(tparams->qp_params.tx_cq, 1, &wc);
    if (num_comp <= 0) {
      if (num_comp < 0) {
        DAQIRI_LOG_ERROR("RDMA CQ polling failed: {}", num_comp);
      }
      break;
    }
    DAQIRI_LOG_DEBUG("GOT TX COMPLETION on connection {} queue {} wrid {}",
                     (void*)tparams->client_id, tparams->queue_idx, (int64_t)wc.wr_id);
    if (wc.status != IBV_WC_SUCCESS) {
      const bool shutting_down = rdma_force_quit.load() || tparams->ready_to_exit.load();
      const bool benign_teardown =
          shutting_down && (wc.status == IBV_WC_WR_FLUSH_ERR || wc.status == IBV_WC_RETRY_EXC_ERR);
      if (benign_teardown) {
        DAQIRI_LOG_DEBUG("TX CQ teardown status on {}: {} ({}) for WRID {} opcode {}",
                         is_server ? "server" : "client", verbs_->wc_status_str(wc.status),
                         (int)wc.status, (int64_t)wc.wr_id, (int)wc.opcode);
      } else {
        DAQIRI_LOG_ERROR("CQ error on {}: {} ({}) for WRID {} and opcode {}",
                         is_server ? "server" : "client", verbs_->wc_status_str(wc.status),
                         (int)wc.status, (int64_t)wc.wr_id, (int)wc.opcode);
      }
      metrics::add_dropped(counters, "tx_completion_error", 1);
      // On error, only wr_id and status are guaranteed valid; opcode is not.
      if (!complete_send_wrs(wc.wr_id, Status::GENERIC_FAILURE)) {
        return;
      }
      continue;
    }

    if (wc.opcode == IBV_WC_SEND || wc.opcode == IBV_WC_RDMA_READ ||
        wc.opcode == IBV_WC_RDMA_WRITE) {
      if (!complete_send_wrs(wc.wr_id, Status::SUCCESS)) {
        return;
      }
      continue;
    } else {
      msg = create_burst_params();
    }

    // Only populate a header to indicate which burst needs to be freed
    // msg->transport_hdr.opcode  = ibv_opcode_to_daqiri_opcode(wc.opcode);
    msg->transport_hdr.tx = true;
    msg->transport_hdr.status =
        wc.status == IBV_WC_SUCCESS ? Status::SUCCESS : Status::GENERIC_FAILURE;
    // set_connection_id(msg, reinterpret_cast<uintptr_t>(tparams->client_id));
    msg->transport_hdr.server = is_server;
    // msg->transport_hdr.wr_id   = wc.wr_id;

    if (!rx_ring->enqueue(reinterpret_cast<void*>(msg))) {
      DAQIRI_LOG_CRITICAL("Failed to enqueue RX completion message");
      metrics::add_dropped(counters, "tx_completion_enqueue_failure", 1);
      free_tx_metadata(msg);
      return;
    }
  }
}

Status RdmaEngine::post_burst(rdma_thread_params* tparams, BurstParams* burst) {
  auto counters = get_rdma_metrics(cfg_, *tparams);
  auto& outstanding_send_wr_ids = tparams->outstanding_send_wr_ids;
  auto& outstanding_receive_wr_ids = tparams->outstanding_receive_wr_ids;
  auto& sends_since_signal = tparams->sends_since_signal;
  const int send_signal_every = tparams->send_signal_every;
  const bool caller_owned =
      burst->transport_hdr.opcode == RDMAOpCode::RECEIVE ? tparams->direct_rx : tparams->direct_tx;
  const auto local_mr = mrs_.find(std::string(burst->transport_hdr.local_mr_name));
  if (local_mr == mrs_.end()) {
    DAQIRI_LOG_CRITICAL("Couldn't find MR with name {} in registry",
                        burst->transport_hdr.local_mr_name);
    metrics::add_dropped(counters, "missing_memory_region", burst->transport_hdr.num_pkts);
    if (!caller_owned) {
      free_tx_burst(burst);
    }
    return Status::INVALID_PARAMETER;
  }

  switch (burst->transport_hdr.opcode) {
    case RDMAOpCode::SEND: {
      // Get lkey for this PD
      auto pd = pd_map_.find(tparams->client_id->verbs);
      if (pd == pd_map_.end()) {
        DAQIRI_LOG_CRITICAL("Couldn't find PD for client");
        metrics::add_dropped(counters, "missing_protection_domain", burst->transport_hdr.num_pkts);
        if (!caller_owned) {
          free_tx_burst(burst);
        }
        return Status::INVALID_PARAMETER;
      }

      // Get lkey for this MR
      auto lkey = local_mr->second.ctx_mr_map_.find(pd->second);
      if (lkey == local_mr->second.ctx_mr_map_.end()) {
        DAQIRI_LOG_CRITICAL("Couldn't find MR with name {} in registry",
                            burst->transport_hdr.local_mr_name);
        metrics::add_dropped(counters, "missing_memory_key", burst->transport_hdr.num_pkts);
        if (!caller_owned) {
          free_tx_burst(burst);
        }
        return Status::INVALID_PARAMETER;
      }

      bool posted_any = false;
      for (int p = 0; p < burst->transport_hdr.num_pkts; p++) {
        ibv_send_wr wr;
        ibv_send_wr* bad_wr;
        ibv_sge sge;

        memset(&wr, 0, sizeof(wr));
        sge.addr = (uint64_t)burst->pkts[0][p];
        sge.length = (uint32_t)burst->pkt_lens[0][p];
        sge.lkey = lkey->second->lkey;
        wr.wr_id = burst->transport_hdr.wr_id + p;  // Auto-increment wr_id to be unique
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.opcode = IBV_WR_SEND;
        if (outstanding_send_wr_ids.count(wr.wr_id)) {
          if (!posted_any && !caller_owned) {
            free_tx_burst(burst);
          }
          return Status::INVALID_PARAMETER;
        }
        // Keep signaled completions frequent enough for callers that use SEND
        // completions as their credit to refill the outstanding window.
        const bool signal_send = send_signal_every == 1 ||
                                 sends_since_signal + 1 >= send_signal_every ||
                                 outstanding_send_wr_ids.size() >= 15;
        wr.send_flags = signal_send ? IBV_SEND_SIGNALED : 0;
        if (tparams->qp_params.qp_attr.qp_type == IBV_QPT_RC) {
          wr.send_flags |= IBV_SEND_FENCE;
        }
        const auto kind = local_mr->second.params_.kind_;
        if (tparams->direct_tx && burst->transport_hdr.num_pkts == 1 &&
            burst->transport_hdr.num_segs == 1 &&
            (kind == MemoryKind::HUGE || kind == MemoryKind::HOST_PINNED) &&
            sge.length <= tparams->max_inline_data) {
          wr.send_flags |= IBV_SEND_INLINE;
        }

        int ret = verbs_->post_send(tparams->client_id->qp, &wr, &bad_wr);
        if (ret != 0) {
          DAQIRI_LOG_CRITICAL("Failed to post SEND request, errno: {}", strerror(errno));
          metrics::add_dropped(counters, "tx_post_failure", 1);
          if (!posted_any) {
            if (!caller_owned) {
              free_tx_burst(burst);
            }
          }
          return Status::GENERIC_FAILURE;
        }
        posted_any = true;
        if (wr.send_flags & IBV_SEND_INLINE) {
          ++tparams->inline_wqes;
        }
        if (signal_send) {
          sends_since_signal = 0;
        } else {
          sends_since_signal++;
        }

        outstanding_send_wr_ids[burst->transport_hdr.wr_id + p] = burst;
        tparams->send_wr_order.push_back(wr.wr_id);
      }

      break;
    }
    case RDMAOpCode::RECEIVE: {
      // Get lkey for this PD
      auto pd = pd_map_.find(tparams->client_id->verbs);
      if (pd == pd_map_.end()) {
        DAQIRI_LOG_CRITICAL("Couldn't find PD for client");
        metrics::add_dropped(counters, "missing_protection_domain", burst->transport_hdr.num_pkts);
        if (!caller_owned) {
          free_tx_burst(burst);
        }
        return Status::INVALID_PARAMETER;
      }

      // Get lkey for this MR
      auto lkey = local_mr->second.ctx_mr_map_.find(pd->second);
      if (lkey == local_mr->second.ctx_mr_map_.end()) {
        DAQIRI_LOG_CRITICAL("Couldn't find MR with name {} in registry",
                            burst->transport_hdr.local_mr_name);
        metrics::add_dropped(counters, "missing_memory_key", burst->transport_hdr.num_pkts);
        if (!caller_owned) {
          free_tx_burst(burst);
        }
        return Status::INVALID_PARAMETER;
      }

      bool posted_any = false;
      for (int p = 0; p < burst->transport_hdr.num_pkts; p++) {
        struct ibv_recv_wr recv_wr;
        struct ibv_sge sge;
        struct ibv_recv_wr* bad_wr = NULL;
        int ret;

        // Prepare Scatter/Gather Entry
        memset(&sge, 0, sizeof(sge));
        sge.addr = (uintptr_t)(uint64_t)burst->pkts[0][p];
        sge.length = (uint32_t)burst->pkt_lens[0][p];
        sge.lkey = lkey->second->lkey;

        // Prepare Receive Work Request
        memset(&recv_wr, 0, sizeof(recv_wr));
        recv_wr.wr_id = burst->transport_hdr.wr_id + p;
        recv_wr.next = NULL;
        recv_wr.sg_list = &sge;
        recv_wr.num_sge = 1;

        // Post the receive request
        ret = verbs_->post_recv(tparams->client_id->qp, &recv_wr, &bad_wr);
        if (ret) {
          DAQIRI_LOG_CRITICAL("ibv_post_recv failed: {}", strerror(errno));
          metrics::add_dropped(counters, "rx_post_failure", 1);
          if (!posted_any) {
            if (!caller_owned) {
              free_tx_burst(burst);
            }
          }
          return Status::GENERIC_FAILURE;
        }
        posted_any = true;

        outstanding_receive_wr_ids[burst->transport_hdr.wr_id + p] = burst;
      }
      break;
    }
    case RDMAOpCode::RDMA_WRITE:
    case RDMAOpCode::RDMA_READ: {
      auto fail = [&](Status status) {
        if (!caller_owned) {
          // The worker owns an accepted operation; return the original buffer
          // exactly once even when posting fails, with no invented CQE.
          burst->transport_hdr.status = status;
          burst->transport_hdr.tx = true;
          if (!tparams->qp_params.rx_ring->enqueue(burst)) {
            free_tx_burst(burst);
          }
        }
        return status;
      };
      auto status = validate_one_sided(*tparams, burst);
      if (status != Status::SUCCESS) {
        return fail(status);
      }
      if (outstanding_send_wr_ids.size() >= MAX_OUSTANDING_WR) {
        return fail(Status::NO_SPACE_AVAILABLE);
      }
      if (outstanding_send_wr_ids.count(burst->transport_hdr.wr_id)) {
        return fail(Status::INVALID_PARAMETER);
      }
      const auto registration = local_mr->second.ctx_mr_map_.find(tparams->pd);
      if (registration == local_mr->second.ctx_mr_map_.end()) {
        return fail(Status::INVALID_PARAMETER);
      }
      ibv_sge sge{};
      sge.addr = reinterpret_cast<uintptr_t>(burst->pkts[0][0]);
      sge.length = burst->pkt_lens[0][0];
      sge.lkey = registration->second->lkey;
      ibv_send_wr wr{};
      wr.wr_id = burst->transport_hdr.wr_id;
      wr.sg_list = &sge;
      wr.num_sge = 1;
      wr.opcode = burst->transport_hdr.opcode == RDMAOpCode::RDMA_READ ? IBV_WR_RDMA_READ
                                                                       : IBV_WR_RDMA_WRITE;
      // Fence against earlier READs before returning preceding SQ buffers.
      wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_FENCE;
      wr.wr.rdma.remote_addr = reinterpret_cast<uintptr_t>(burst->transport_hdr.raddr);
      wr.wr.rdma.rkey = static_cast<uint32_t>(burst->transport_hdr.dst_key);
      ibv_send_wr* bad = nullptr;
      if (verbs_->post_send(tparams->client_id->qp, &wr, &bad)) {
        metrics::add_dropped(counters, "tx_post_failure", 1);
        return fail(Status::GENERIC_FAILURE);
      }
      outstanding_send_wr_ids.emplace(wr.wr_id, burst);
      tparams->send_wr_order.push_back(wr.wr_id);
      sends_since_signal = 0;
      break;
    }
    default:
      return Status::NOT_SUPPORTED;
  }

  return Status::SUCCESS;
}

void RdmaEngine::rdma_thread(bool is_server, rdma_thread_params* tparams) {
  const auto& intf = cfg_.ifs_[tparams->if_idx];
  const auto& common = tparams->direct_tx ? intf.rx_.queues_[tparams->queue_idx].common_
                                          : intf.tx_.queues_[tparams->queue_idx].common_;
  const long cpu_core = strtol(common.cpu_core_.c_str(), nullptr, 10);
  if (set_affinity(cpu_core) != 0) {
    DAQIRI_LOG_CRITICAL("Failed to set RDMA core affinity");
    return;
  }
  tparams->worker_started.store(true);
  while (!rdma_force_quit.load() && !tparams->ready_to_exit.load()) {
    std::lock_guard<std::mutex> lock(tparams->progress_mutex);
    poll_completions(is_server, tparams, !tparams->direct_rx, !tparams->direct_tx);
    BurstParams* burst = nullptr;
    if (tparams->qp_params.tx_ring->dequeue(reinterpret_cast<void**>(&burst))) {
      post_burst(tparams, burst);
    }
  }
}

Status RdmaEngine::rdma_connect_to_server(const std::string& dst_addr, uint16_t dst_port,
                                       uintptr_t* conn_id) {
  return rdma_connect_to_server(dst_addr, dst_port, "", conn_id);
}

RDMAOpCode RdmaEngine::rdma_get_opcode(BurstParams* burst) {
  return burst->transport_hdr.opcode;
}

Status RdmaEngine::rdma_set_header(BurstParams* burst, RDMAOpCode op_code, uintptr_t conn_id,
                                bool is_server, int num_pkts, uint64_t wr_id,
                                const std::string& local_mr_name) {
  if (!burst || num_pkts <= 0 || num_pkts > MAX_RDMA_BATCH || local_mr_name.empty() ||
      local_mr_name.size() >= sizeof(burst->transport_hdr.local_mr_name) ||
      local_mr_name.find('\0') != std::string::npos) {
    return Status::INVALID_PARAMETER;
  }
  if (op_code != RDMAOpCode::SEND && op_code != RDMAOpCode::RECEIVE &&
      op_code != RDMAOpCode::RDMA_READ && op_code != RDMAOpCode::RDMA_WRITE) {
    return Status::NOT_SUPPORTED;
  }
  if ((op_code == RDMAOpCode::RDMA_READ || op_code == RDMAOpCode::RDMA_WRITE) && num_pkts != 1) {
    return Status::INVALID_PARAMETER;
  }
  burst->transport_hdr.raddr = nullptr;
  burst->transport_hdr.dst_key = 0;
  burst->transport_hdr.remote_length = 0;
  burst->transport_hdr.remote_access = 0;
  burst->transport_hdr.opcode = op_code;
  set_connection_id(burst, conn_id);
  burst->transport_hdr.server = is_server;
  burst->transport_hdr.num_pkts = num_pkts;
  burst->transport_hdr.num_segs = 1;
  burst->transport_hdr.wr_id = wr_id;
  auto& cache = rdma_burst_cache(burst);
  if (cache.magic != RdmaBurstCache::kMagic || cache.engine != this || !cache.state ||
      cache.state->connection_id != conn_id || !cache.region ||
      cache.region->name != local_mr_name) {
    cache = {};
    cache.magic = RdmaBurstCache::kMagic;
    cache.engine = this;
    cache.state = find_direct(conn_id);
    if (cache.state) {
      for (const auto& region : cache.state->regions) {
        if (region.name == local_mr_name) {
          cache.region = &region;
          break;
        }
      }
    }
  }
  snprintf(burst->transport_hdr.local_mr_name,
           sizeof(burst->transport_hdr.local_mr_name),
           "%s",
           local_mr_name.c_str());
  return Status::SUCCESS;
}

Status RdmaEngine::rdma_get_local_memory_region(uintptr_t conn_id, const std::string& name,
                                                RdmaMemoryRegion* region) {
  if (!region) {
    return Status::INVALID_PARAMETER;
  }
  *region = {};
  std::unique_lock<std::mutex> registry_lock(threads_mutex_);
  const auto connection = connection_params_.find(reinterpret_cast<rdma_cm_id*>(conn_id));
  if (connection == connection_params_.end()) {
    return Status::NOT_READY;
  }
  auto& params = *connection->second;
  std::lock_guard<std::mutex> progress_lock(params.progress_mutex);
  if (params.ready_to_exit.load() || rdma_force_quit.load()) {
    return Status::NOT_READY;
  }
  if (params.qp_params.qp_attr.qp_type != IBV_QPT_RC) {
    return Status::NOT_SUPPORTED;
  }
  const auto local = mrs_.find(name);
  if (local == mrs_.end()) {
    return Status::INVALID_PARAMETER;
  }
  const auto registration = local->second.ctx_mr_map_.find(params.pd);
  if (registration == local->second.ctx_mr_map_.end() || !registration->second) {
    return Status::INVALID_PARAMETER;
  }
  const auto* mr = registration->second;
  *region = {reinterpret_cast<uintptr_t>(mr->addr), mr->length, mr->rkey,
             local->second.params_.access_};
  return Status::SUCCESS;
}

Status RdmaEngine::rdma_set_remote_memory(BurstParams* burst, const RdmaMemoryRegion& region,
                                          uint64_t offset) {
  if (!burst) {
    return Status::INVALID_PARAMETER;
  }
  auto& hdr = burst->transport_hdr;
  // Clear the old target even if the replacement is invalid.
  hdr.raddr = nullptr;
  hdr.dst_key = hdr.remote_length = hdr.remote_access = 0;
  if (hdr.opcode != RDMAOpCode::RDMA_READ && hdr.opcode != RDMAOpCode::RDMA_WRITE) {
    return Status::NOT_SUPPORTED;
  }
  const uint32_t required =
      hdr.opcode == RDMAOpCode::RDMA_READ ? MEM_ACCESS_RDMA_READ : MEM_ACCESS_RDMA_WRITE;
  if (!region.address || !region.length || offset >= region.length ||
      region.length > UINT64_MAX - region.address || !(region.access & required)) {
    return Status::INVALID_PARAMETER;
  }
  hdr.raddr = reinterpret_cast<void*>(region.address + offset);
  hdr.dst_key = region.rkey;
  hdr.remote_length = region.length - offset;
  hdr.remote_access = region.access;
  return Status::SUCCESS;
}

Status RdmaEngine::validate_one_sided(const rdma_thread_params& params, BurstParams* burst) const {
  if (params.qp_params.qp_attr.qp_type != IBV_QPT_RC) {
    return Status::NOT_SUPPORTED;
  }
  const auto& hdr = burst->transport_hdr;
  if (hdr.num_pkts != 1 || hdr.num_segs != 1 || !burst->pkts[0] || !burst->pkt_lens[0] ||
      !burst->pkts[0][0] || !hdr.raddr || hdr.dst_key > UINT32_MAX ||
      !memchr(hdr.local_mr_name, '\0', sizeof(hdr.local_mr_name))) {
    return Status::INVALID_PARAMETER;
  }
  const auto local = mrs_.find(hdr.local_mr_name);
  if (local == mrs_.end()) {
    return Status::INVALID_PARAMETER;
  }
  const auto registration = local->second.ctx_mr_map_.find(params.pd);
  if (registration == local->second.ctx_mr_map_.end() || !registration->second) {
    return Status::INVALID_PARAMETER;
  }
  const auto* mr = registration->second;
  const uint64_t address = reinterpret_cast<uintptr_t>(burst->pkts[0][0]);
  const uint64_t base = reinterpret_cast<uintptr_t>(mr->addr);
  const uint64_t length = burst->pkt_lens[0][0];
  const bool read = hdr.opcode == RDMAOpCode::RDMA_READ;
  if (!length || length > local->second.params_.buf_size_ || address < base ||
      address - base > mr->length || length > mr->length - (address - base) ||
      length > hdr.remote_length || length > UINT64_MAX - reinterpret_cast<uintptr_t>(hdr.raddr) ||
      !(hdr.remote_access & (read ? MEM_ACCESS_RDMA_READ : MEM_ACCESS_RDMA_WRITE)) ||
      (read && !(local->second.params_.access_ & (MEM_ACCESS_LOCAL | MEM_ACCESS_RDMA_WRITE)))) {
    return Status::INVALID_PARAMETER;
  }
  return Status::SUCCESS;
}

Status RdmaEngine::rdma_get_server_conn_id(const std::string& server_addr, uint16_t server_port,
                                           uintptr_t* conn_id) {
  const auto iter = server_str_to_id_.find(server_addr + ":" + std::to_string(server_port));
  if (iter == server_str_to_id_.end()) {
    return Status::NOT_READY;
  }

  const auto server_id = iter->second;
  const auto server_params = server_q_params_.find(server_id);
  if (server_params == server_q_params_.end()) {
    return Status::NOT_READY;
  }

  bool waiting_for_client = true;
  for (size_t i = 0; i < server_params->second.size(); i++) {
    if (server_params->second[i].client_id == nullptr) {
      continue;
    }
    waiting_for_client = false;

    if (server_params->second[i].active) {
      continue;
    }

    auto* client_id = server_params->second[i].client_id;
    if (!server_params->second[i].worker_started.load()) {
      continue;
    }

    std::lock_guard<std::mutex> lock(threads_mutex_);
    if (connection_params_.find(client_id) == connection_params_.end() ||
        tx_rings_map_.find(client_id) == tx_rings_map_.end() ||
        rx_rings_map_.find(client_id) == rx_rings_map_.end()) {
      continue;
    }

    *conn_id = reinterpret_cast<uintptr_t>(client_id);
    server_params->second[i].active = true;
    DAQIRI_LOG_INFO("Found available queue ID for server {}:{} with cm_id {}", server_addr,
                    server_port, (void*)client_id);
    return Status::SUCCESS;
  }

  if (waiting_for_client) {
    DAQIRI_LOG_DEBUG("No client connected yet for server {}:{}", server_addr, server_port);
    return Status::NOT_READY;
  }

  DAQIRI_LOG_WARN("No free queue slots for server {}:{}. Close at least one connection first.",
                  server_addr, server_port);
  return Status::NO_SPACE_AVAILABLE;
}

Status RdmaEngine::rdma_connect_to_server(const std::string& dst_addr, uint16_t dst_port,
                                          const std::string& src_addr, uintptr_t* conn_id) {
  struct rdma_cm_id* cm_id = nullptr;
  struct rdma_event_channel* ec = nullptr;
  struct rdma_cm_event* event = nullptr;
  struct rdma_conn_param conn_param = {};

  if (!initialized_) {
    DAQIRI_LOG_WARN("RDMA engine not initialized yet. Not trying to connect to server");
    return Status::NOT_READY;
  }

  DAQIRI_LOG_INFO("Connecting to server {} on port {}", dst_addr, dst_port);

  *conn_id = 0;

  ec = verbs_->cm_create_event_channel();
  if (ec == nullptr) {
    DAQIRI_LOG_CRITICAL("Failed to create a CM channel: {}", strerror(errno));
    return Status::CONNECT_FAILURE;
  }

  // Create RDMA id
  if (verbs_->cm_create_id(ec, &cm_id, nullptr, RDMA_PS_TCP)) {
    DAQIRI_LOG_CRITICAL("Failed to create ID");
    verbs_->cm_destroy_event_channel(ec);
    return Status::CONNECT_FAILURE;
  } else {
    DAQIRI_LOG_INFO("Created ID for client: {}", (void*)cm_id);
  }

  // Always bind to the source address, even if not explicitly provided
  struct sockaddr_in src_addr_in;
  memset(&src_addr_in, 0, sizeof(src_addr_in));

  if (!src_addr.empty()) {
    src_addr_in.sin_family = AF_INET;
    src_addr_in.sin_port = 0;  //
    DAQIRI_LOG_INFO("Using provided source address: {}", src_addr);
    inet_pton(AF_INET, src_addr.c_str(), &src_addr_in.sin_addr);
  }

  // Set up server address
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(dst_port);
  if (inet_pton(AF_INET, dst_addr.c_str(), &addr.sin_addr) != 1) {
    DAQIRI_LOG_CRITICAL("Failed to convert IP address: {}", dst_addr);
    return Status::CONNECT_FAILURE;
  }

  struct sockaddr* src_addr_p = src_addr.empty() ? nullptr : (struct sockaddr*)&src_addr_in;

  DAQIRI_LOG_INFO("Client resolving server address: {}", dst_addr);
  // Resolve the server's address
  if (verbs_->cm_resolve_addr(cm_id, src_addr_p, (struct sockaddr*)&addr, 2000)) {
    DAQIRI_LOG_CRITICAL("Failed to resolve address");
    verbs_->cm_destroy_event_channel(ec);
    return Status::CONNECT_FAILURE;
  }

  // Wait for address resolution event
  if (verbs_->cm_get_cm_event(ec, &event)) {
    DAQIRI_LOG_CRITICAL("Failed to get CM event");
    verbs_->cm_destroy_event_channel(ec);
    return Status::CONNECT_FAILURE;
  }

  if (event->event != RDMA_CM_EVENT_ADDR_RESOLVED) {
    if (event->event == RDMA_CM_EVENT_ADDR_ERROR) {
      DAQIRI_LOG_CRITICAL("Failed to resolve address");
    } else {
      DAQIRI_LOG_CRITICAL("Unexpected event from rdma_resolve_addr: {}", (int)event->event);
    }
    verbs_->cm_destroy_event_channel(ec);
    return Status::CONNECT_FAILURE;
  } else {
    DAQIRI_LOG_INFO("Client resolved server address: {}", (void*)cm_id);
  }

  verbs_->cm_ack_cm_event(event);

  // Resolve route to server
  if (verbs_->cm_resolve_route(cm_id, 2000)) {
    DAQIRI_LOG_CRITICAL("Failed to resolve route");
    verbs_->cm_destroy_event_channel(ec);
    return Status::CONNECT_FAILURE;
  }

  // Wait for route resolution event
  if (verbs_->cm_get_cm_event(ec, &event)) {
    DAQIRI_LOG_CRITICAL("Failed to get CM event");
    verbs_->cm_destroy_event_channel(ec);
    return Status::CONNECT_FAILURE;
  }

  if (event->event != RDMA_CM_EVENT_ROUTE_RESOLVED) {
    if (event->event == RDMA_CM_EVENT_REJECTED) {
      DAQIRI_LOG_WARN(
          "Server rejected connection. Check if server is running and accepting connections");
    } else {
      DAQIRI_LOG_CRITICAL("Unexpected event: {}", (int)event->event);
    }

    verbs_->cm_destroy_event_channel(ec);
    return Status::CONNECT_FAILURE;
  } else {
    DAQIRI_LOG_INFO("Client {} resolved route for server", (void*)cm_id);
    verbs_->cm_ack_cm_event(event);
  }

  struct sockaddr* source_addr = verbs_->cm_get_local_addr(cm_id);

  // Convert sockaddr to string
  char source_addr_str[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &source_addr->sa_data[2], source_addr_str, INET_ADDRSTRLEN);
  DAQIRI_LOG_INFO("Client source address: {}", source_addr_str);

  int client_port = -1;
  for (const auto& port : cfg_.ifs_) {
    if (port.address_ == std::string(source_addr_str)) {
      client_port = port.port_id_;
      break;
    }
  }

  if (client_port < 0) {
    DAQIRI_LOG_ERROR("No configured interface matches source address {}", source_addr_str);
    verbs_->cm_destroy_id(cm_id);
    verbs_->cm_destroy_event_channel(ec);
    return Status::INVALID_PARAMETER;
  }

  rdma_thread_params* params = nullptr;
  bool queue_exhausted = false;
  {
    std::lock_guard<std::mutex> lock(client_params_mutex_);
    std::vector<detail::RdmaClientQueueAssignment> assignments;
    assignments.reserve(client_q_params_.size());
    for (const auto& entry : client_q_params_) {
      assignments.push_back({entry.second.if_idx, entry.second.queue_idx});
    }

    // queue_idx addresses the queue vector on one interface, so reserve the
    // lowest free index on that interface rather than using the global client count.
    const auto queue_count = cfg_.ifs_[client_port].tx_.queues_.size();
    const int queue_idx =
        detail::first_available_rdma_client_queue(client_port, queue_count, assignments);
    queue_exhausted = queue_idx < 0;
    if (!queue_exhausted) {
      auto [iter, inserted] = client_q_params_.try_emplace(cm_id);
      if (inserted) {
        params = &iter->second;
        params->client_id = cm_id;
        params->pd = pd_map_[cm_id->verbs];
        params->if_idx = client_port;
        params->queue_idx = queue_idx;
      }
    }
  }

  if (params == nullptr) {
    if (queue_exhausted) {
      DAQIRI_LOG_ERROR("No available client TX queue for source address {}", source_addr_str);
    } else {
      DAQIRI_LOG_CRITICAL("Failed to insert client params into map");
    }
    verbs_->cm_destroy_id(cm_id);
    verbs_->cm_destroy_event_channel(ec);
    return queue_exhausted ? Status::NO_SPACE_AVAILABLE : Status::CONNECT_FAILURE;
  }
  setup_thread_params(params, false);

  // Set up connection parameters
  memset(&conn_param, 0, sizeof(conn_param));
  conn_param.responder_resources = 1;
  conn_param.initiator_depth = 1;
  conn_param.rnr_retry_count = 7;

  // Connect to server
  if (verbs_->cm_connect(cm_id, &conn_param) != 0) {
    DAQIRI_LOG_CRITICAL("Failed to connect to server");
    destroy_thread_params(params);
    verbs_->cm_destroy_event_channel(ec);
    return Status::CONNECT_FAILURE;
  } else {
    DAQIRI_LOG_INFO("Client {} connected to server", (void*)cm_id);
  }

  // Wait for connection established event
  if (verbs_->cm_get_cm_event(ec, &event)) {
    DAQIRI_LOG_CRITICAL("Failed to get CM event");
    destroy_thread_params(params);
    verbs_->cm_destroy_event_channel(ec);
    return Status::CONNECT_FAILURE;
  }

  if (event->event != RDMA_CM_EVENT_ESTABLISHED) {
    if (event->event == RDMA_CM_EVENT_REJECTED) {
      DAQIRI_LOG_WARN(
          "Server rejected connection. Check if server is running and accepting connections");
    } else {
      DAQIRI_LOG_CRITICAL("Unexpected event: {}", (int)event->event);
    }
    verbs_->cm_ack_cm_event(event);
    verbs_->cm_destroy_event_channel(ec);
    destroy_thread_params(params);
    return Status::CONNECT_FAILURE;
  } else {
    DAQIRI_LOG_INFO("Client {} established connection to server", (void*)cm_id);
  }

  verbs_->cm_ack_cm_event(event);

  DAQIRI_LOG_INFO("Launching client thread for {}", (void*)cm_id);

  // Store the connection ID for later use
  launch_worker(params, false);

  *conn_id = reinterpret_cast<uintptr_t>(cm_id);
  DAQIRI_LOG_INFO("Successfully connected to server {} on port {}", dst_addr, dst_port);

  // rdma_destroy_event_channel(ec);

  return Status::SUCCESS;
}

Status RdmaEngine::rdma_get_port_queue(uintptr_t conn_id, uint16_t* port, uint16_t* queue) {
  // Look up connection ID to get port/q
  auto iter = client_q_params_.find(reinterpret_cast<struct rdma_cm_id*>(conn_id));
  if (iter == client_q_params_.end()) {
    DAQIRI_LOG_CRITICAL("Failed to find client params for connection ID {}", conn_id);
    return Status::INVALID_PARAMETER;
  }

  *port = iter->second.if_idx;
  *queue = iter->second.queue_idx;

  return Status::SUCCESS;
}

Status RdmaEngine::get_tx_packet_burst(BurstParams* burst) {
  if (burst == nullptr) { return Status::INVALID_PARAMETER; }

  // RDMA isn't allowing split segments yet
  assert(burst->transport_hdr.num_segs == 1);
  assert(burst->transport_hdr.num_pkts <= MAX_RDMA_BATCH);
  auto* pool = burst_pool(burst);
  if (pool == nullptr) {
    DAQIRI_LOG_ERROR("Failed to look up burst pool name for MR {}",
                       burst->transport_hdr.local_mr_name);
    return Status::INVALID_PARAMETER;
  }

  if (!tx_burst_pool_->get(reinterpret_cast<void**>(&burst->pkts[0]))) {
    DAQIRI_LOG_ERROR("Failed to get packet from pool");
    return Status::NO_FREE_BURST_BUFFERS;
  }

  const unsigned num_pkts = static_cast<unsigned>(burst->transport_hdr.num_pkts);
  const unsigned rx = pool->dequeue_bulk(reinterpret_cast<void**>(burst->pkts[0]), num_pkts);
  if (rx != num_pkts) {
    DAQIRI_LOG_ERROR("Asked for {} packets, got {}", num_pkts, rx);
    tx_burst_pool_->put(reinterpret_cast<void*>(burst->pkts[0]));
    burst->pkts[0] = nullptr;
    return Status::NO_FREE_BURST_BUFFERS;
  }

  // Allocate packet length buffer
  if (!pkt_len_pool_->get(reinterpret_cast<void**>(&burst->pkt_lens[0]))) {
    DAQIRI_LOG_ERROR("Failed to get packet length buffer");
    pool->enqueue_bulk(reinterpret_cast<void**>(burst->pkts[0]), num_pkts);
    tx_burst_pool_->put(reinterpret_cast<void*>(burst->pkts[0]));
    burst->pkts[0] = nullptr;
    return Status::NO_FREE_PACKET_BUFFERS;
  }

  return Status::SUCCESS;
}

bool RdmaEngine::is_tx_burst_available(BurstParams* burst) {
  auto* pool = burst_pool(burst);
  if (pool == nullptr) {
    DAQIRI_LOG_ERROR("Failed to look up burst pool name for MR {}",
                       burst->transport_hdr.local_mr_name);
    return false;
  }

  if (pool->count() < burst->transport_hdr.num_pkts) {
    return false;
  }

  return true;
}

void RdmaEngine::run() {
  int ret;

  DAQIRI_LOG_INFO("Starting RDMA CM main thread");

  if (set_affinity(cfg_.common_.master_core_) != 0) {
    DAQIRI_LOG_CRITICAL("Failed to set master core affinity");
    return;
  }

  // Create a channel for events. Only the master thread reads from this channel
  cm_event_channel_ = verbs_->cm_create_event_channel();
  if (cm_event_channel_ == nullptr) {
    DAQIRI_LOG_CRITICAL("Failed to create a CM channel: {}", strerror(errno));
    return;
  }

  // Set the channel to non-blocking so we can check for SIGINT
  int flags = fcntl(cm_event_channel_->fd, F_GETFL);
  fcntl(cm_event_channel_->fd, F_SETFL, flags | O_NONBLOCK);

  // Start an RDMA server on each server interface
  for (const auto& intf : cfg_.ifs_) {
    if (intf.rdma_.mode_ != RDMAMode::SERVER) {
      continue;
    }

    // Initialize all the setup before the main loop
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(intf.rdma_.port_);  // Make sure port is set

    // Convert IP address string to network address
    if (inet_pton(AF_INET, intf.address_.c_str(), &server_addr.sin_addr) != 1) {
      DAQIRI_LOG_ERROR("Failed to convert IP address: {}", intf.address_);
      return;
    }

    DAQIRI_LOG_INFO("Successfully created CM event channel");

    struct rdma_cm_id* s_id;
    ret = verbs_->cm_create_id(cm_event_channel_, &s_id, nullptr, RDMA_PS_TCP);
    if (ret != 0) {
      DAQIRI_LOG_CRITICAL("Failed to create RDMA server ID {}", strerror(errno));
      return;
    }

    pd_params_[s_id] = {};
    pd_params_[s_id].server_id = s_id;
    pd_params_[s_id].if_idx = intf.port_id_;

    // try_emplace constructs the vector in place via vector(size_t n), which
    // value-initialises n elements without ever moving them. We can't use the
    // operator[] + resize() pattern any more: rdma_thread_params now holds an
    // std::atomic<bool> and is therefore non-movable, and libstdc++'s resize()
    // instantiates the move-if-noexcept path even when starting from empty.
    server_q_params_.try_emplace(s_id, intf.rx_.queues_.size());

    server_str_to_id_[intf.address_ + ":" + std::to_string(intf.rdma_.port_)] = s_id;

    DAQIRI_LOG_INFO("Created RDMA server on {}:{} successfully with listener_id {}", intf.address_,
                    intf.rdma_.port_, (void*)s_id);

    ret = verbs_->cm_bind_addr(s_id, reinterpret_cast<struct sockaddr*>(&server_addr));
    if (ret != 0) {
      DAQIRI_LOG_CRITICAL("Failed to bind for RDMA server: {}", strerror(errno));
      return;
    }

    ret = verbs_->cm_listen(s_id, intf.rx_.queues_.size());
    if (ret != 0) {
      DAQIRI_LOG_CRITICAL("Failed to listen for RDMA server: {}", strerror(errno));
      return;
    }

    DAQIRI_LOG_INFO("RDMA server successfully started on {} queues", intf.rx_.queues_.size());
  }

  DAQIRI_LOG_INFO("Entering CM main loop");

  // Our master thread's job is to wait on connections from clients, set up all the needed
  // information for them (QPs, MRs, etc), and spawn helper threads to monitor both TX and RX
  struct rdma_cm_event* cm_event = nullptr;
  while (!rdma_force_quit.load()) {
    ret = verbs_->cm_get_cm_event(cm_event_channel_, &cm_event);
    if (ret != 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // Sleep for 500 microseconds and try again
        usleep(500);
        continue;
      }

      DAQIRI_LOG_INFO("Failed to get CM event: {}", strerror(errno));
      continue;
    }

    DAQIRI_LOG_INFO("Received CM event: {}", verbs_->cm_event_str(cm_event->event));

    if (cm_event->status != 0) {
      DAQIRI_LOG_ERROR("Error status received in CM event {}: {}",
                       verbs_->cm_event_str(cm_event->event), cm_event->status);
      if (verbs_->cm_ack_cm_event(cm_event) == 0) {
        DAQIRI_LOG_INFO("Acknowledged CM event: {}", verbs_->cm_event_str(cm_event->event));
      } else {
        DAQIRI_LOG_CRITICAL("Failed to acknowledge CM event {}: {}",
                            verbs_->cm_event_str(cm_event->event), strerror(errno));
      }

      continue;
    }

    switch (cm_event->event) {
      case RDMA_CM_EVENT_CONNECT_REQUEST: {
        DAQIRI_LOG_INFO("Received new connection request for client ID {} on listener {}",
                        (void*)cm_event->id, (void*)cm_event->listen_id);

        const auto listen_iter = pd_params_.find(cm_event->listen_id);
        if (listen_iter == pd_params_.end()) {
          DAQIRI_LOG_ERROR("Failed to find server ID for {}", (void*)cm_event->listen_id);
          break;
        } else {
          const auto server_iter = server_q_params_.find(cm_event->listen_id);
          if (server_iter == server_q_params_.end()) {
            DAQIRI_LOG_ERROR("Failed to find server ID for {}", (void*)cm_event->listen_id);
            break;
          }

          bool duplicate_client = false;
          for (const auto& slot : server_iter->second) {
            if (cm_event->id == slot.client_id) {
              DAQIRI_LOG_CRITICAL("Client ID {} already exists in server ID {}",
                                  (void*)cm_event->id, (void*)listen_iter->second.server_id);
              verbs_->cm_reject(cm_event->id, nullptr, 0);
              duplicate_client = true;
              break;
            }
          }
          if (duplicate_client) {
            break;
          }

          // A slot is free only when no client occupies it. Testing !active
          // would reuse a slot that was already accepted but not yet claimed by
          // the app (client_id set, active still false), clobbering the first
          // connection's params/rings while its worker is still running.
          int queue_idx = -1;
          for (int i = 0; i < server_iter->second.size(); i++) {
            if (server_iter->second[i].client_id == nullptr) {
              queue_idx = i;
              break;
            }
          }

          if (queue_idx == -1) {
            DAQIRI_LOG_CRITICAL(
                "No free queues on server ID {}. Close at least one connection first.",
                (void*)cm_event->listen_id);
            verbs_->cm_reject(cm_event->id, nullptr, 0);
            break;
          }

          auto& params = server_iter->second[queue_idx];
          params.active = false;
          params.ready_to_exit.store(false);
          params.worker_started.store(false);
          params.client_id = cm_event->id;
          params.pd = pd_map_[cm_event->id->verbs];
          // Use the listener's cfg-interface index, not cm_event->id->port_num
          // (IB hardware port, different domain than cfg_.ifs_ indices)
          params.if_idx = listen_iter->second.if_idx;
          params.queue_idx = queue_idx;

          setup_thread_params(&params, true);
          DAQIRI_LOG_INFO("Configured queues for client {}", (void*)cm_event->id);

          ack_event(cm_event);

          struct rdma_conn_param conn_param = {};
          conn_param.responder_resources = 1;
          conn_param.initiator_depth = 1;
          conn_param.rnr_retry_count = 7;

          if (verbs_->cm_accept(params.client_id, &conn_param) != 0) {
            DAQIRI_LOG_CRITICAL("Failed to accept connection: {}", strerror(errno));
            verbs_->cm_reject(params.client_id, nullptr, 0);
            continue;
          }

          DAQIRI_LOG_INFO("Server accepted connection for client ID {}", (void*)params.client_id);
          launch_worker(&params, true);
        }

        break;
      }
      case RDMA_CM_EVENT_ESTABLISHED: {
        bool found = false;
        DAQIRI_LOG_INFO("Received established event for client ID {}", (void*)cm_event->id);

        for (auto& sp : server_q_params_) {
          for (auto& thread_params : sp.second) {
            if (thread_params.client_id == cm_event->id) {
              DAQIRI_LOG_INFO("Client ID {} is on server ID {}", (void*)cm_event->id,
                              (void*)sp.first);
              found = true;
              launch_worker(&thread_params, true);
              break;
            }
          }
          if (found) {
            break;
          }
        }

        if (!found) {
          DAQIRI_LOG_CRITICAL("Received established event for unknown client ID {}",
                              (void*)cm_event->id);
          break;
        }

        ack_event(cm_event);

        break;
      }
      case RDMA_CM_EVENT_DISCONNECTED: {
        DAQIRI_LOG_INFO("Received disconnected event for client ID {}", (void*)cm_event->id);

        // Avoid the deadlock that print_stats() exposes:
        // (1) signal the worker via ready_to_exit;
        // (2) move its thread handle out of worker_threads_ under threads_mutex_;
        // (3) release the mutex, then join (so a concurrent print_stats(),
        //     which also takes threads_mutex_, doesn't sit behind a join that
        //     can only finish once we've signalled exit);
        // (4) only after the worker has actually stopped, push its rings back
        //     to the pool and clear the slot. The original code returned the
        //     rings after the join too, and the worker may still be polling
        //     tparams->qp_params.{tx,rx}_ring up until the moment it exits.
        bool found = false;
        std::thread worker_to_join;
        rdma_thread_params* tparams_to_clear = nullptr;
        for (auto& sp : server_q_params_) {
          for (auto& thread_params : sp.second) {
            if (thread_params.client_id == cm_event->id) {
              thread_params.ready_to_exit.store(true);
              {
                std::lock_guard<std::mutex> lock(threads_mutex_);
                connection_params_.erase(cm_event->id);
                auto it = worker_threads_.find(cm_event->id);
                if (it != worker_threads_.end()) {
                  worker_to_join = std::move(it->second);
                  worker_threads_.erase(it);
                }
              }
              tparams_to_clear = &thread_params;
              found = true;
              break;
            }
          }
          if (found) {
            break;
          }
        }

        if (worker_to_join.joinable()) {
          worker_to_join.join();
        }

        if (tparams_to_clear != nullptr) {
          if (tparams_to_clear->direct_state) {
            tparams_to_clear->direct_state->close();
          }
          std::lock_guard<std::mutex> progress_lock(tparams_to_clear->progress_mutex);
          tparams_to_clear->outstanding_send_wr_ids.clear();
          tparams_to_clear->send_wr_order.clear();
          tparams_to_clear->outstanding_receive_wr_ids.clear();
          if (tparams_to_clear->qp_params.tx_ring != nullptr) {
            tx_rings_.push(tparams_to_clear->qp_params.tx_ring);
            tx_rings_map_.erase(cm_event->id);
          }
          if (tparams_to_clear->qp_params.rx_ring != nullptr) {
            rx_rings_.push(tparams_to_clear->qp_params.rx_ring);
            rx_rings_map_.erase(cm_event->id);
          }
          tparams_to_clear->client_id = nullptr;
          tparams_to_clear->active = false;
          tparams_to_clear->worker_started.store(false);
          DAQIRI_LOG_INFO("Joined and removed client thread for ID {}", (void*)cm_event->id);
        } else {
          DAQIRI_LOG_CRITICAL("Received disconnected event for unknown client ID {}",
                              (void*)cm_event->id);
        }

        ack_event(cm_event);
        break;
      }
      default: {
        DAQIRI_LOG_INFO("Cannot handle event type {}", verbs_->cm_event_str(cm_event->event));
        ack_event(cm_event);
        break;
      }
    }
  }

  // Join any client threads we had spawned
  DAQIRI_LOG_INFO("Waiting for server TX/RX threads to complete");
  for (auto& thread : worker_threads_) {
    thread.second.join();
  }

  DAQIRI_LOG_INFO("Finished cleaning up TX/RX workers");
}

int RdmaEngine::setup_pools_and_rings() {
  // RX rings
  DAQIRI_LOG_INFO("Setting up TX/RX per-queue rings");

  // Pin the rings/pools to the master core's NUMA node, mirroring DPDK's use of
  // rte_socket_id() for these allocations. -1 (libnuma absent) -> first-touch.
  const int numa_node = daqiri::detail::numa_node_for_cpu(cfg_.common_.master_core_);

  // Each connection ring is single-producer/single-consumer by design: one
  // rdma_thread owns the engine side and one application thread owns the API
  // side for a given conn_id. If multi-threaded app access per conn_id is ever
  // allowed, these rings must go back to MP/MC-safe flags.
  for (int i = 0; i < MAX_RDMA_CONNECTIONS; i++) {
    std::string ring_name = "RX_RING_" + std::to_string(i);
    DAQIRI_LOG_DEBUG("Setting up RX ring {}", ring_name);
    daqiri::Ring* ring =
        daqiri::Ring::create(ring_name.c_str(), 2048, daqiri::RingMode::SPSC, numa_node);
    if (ring == nullptr) {
      DAQIRI_LOG_CRITICAL("Failed to allocate RX ring {}!", ring_name);
      return -1;
    }
    rx_rings_.push(ring);

    ring_name = "TX_RING_" + std::to_string(i);
    DAQIRI_LOG_DEBUG("Setting up TX ring {}", ring_name);
    ring = daqiri::Ring::create(ring_name.c_str(), 2048, daqiri::RingMode::SPSC, numa_node);
    if (ring == nullptr) {
      DAQIRI_LOG_CRITICAL("Failed to allocate TX ring {}!", ring_name);
      return -1;
    }
    tx_rings_.push(ring);
  }

  // Packet length buffers
  DAQIRI_LOG_DEBUG("Setting up RX meta pool");
  pkt_len_pool_ = daqiri::ObjectPool::create("PKT_LEN_POOL", (1U << 11) - 1U,
                                             sizeof(uint32_t) * MAX_RDMA_BATCH, numa_node);
  if (pkt_len_pool_ == nullptr) {
    DAQIRI_LOG_CRITICAL("Failed to allocate packet length pool!");
    return -1;
  }

  DAQIRI_LOG_DEBUG("Setting up RX meta pool");
  rx_meta =
      daqiri::ObjectPool::create("RX_META_POOL", (1U << 11) - 1U, sizeof(BurstParams), numa_node);
  if (rx_meta == nullptr) {
    DAQIRI_LOG_CRITICAL("Failed to allocate RX meta pool!");
    return -1;
  }

  DAQIRI_LOG_DEBUG("Setting up TX meta pool");
  tx_meta =
      daqiri::ObjectPool::create("TX_META_POOL", (1U << 11) - 1U, sizeof(BurstParams), numa_node);
  if (tx_meta == nullptr) {
    DAQIRI_LOG_CRITICAL("Failed to allocate TX meta pool!");
    return -1;
  }

  tx_burst_pool_ = daqiri::ObjectPool::create("TX_BURST_POOL", (1U << 11) - 1U,
                                              sizeof(void*) * MAX_RDMA_BATCH, numa_node);
  if (tx_burst_pool_ == nullptr) {
    DAQIRI_LOG_CRITICAL("Failed to allocate TX burst pool!");
    return -1;
  } else {
    DAQIRI_LOG_INFO("TX burst pool allocated at {}", (void*)tx_burst_pool_);
  }

  return 0;
}

void RdmaEngine::free_rx_burst(BurstParams* burst) {
  if (burst == nullptr) { return; }
  rx_meta->put(burst);
}

void RdmaEngine::free_tx_burst(BurstParams* burst) {
  if (burst == nullptr) { return; }

  auto* pool = burst_pool(burst);
  if (burst->transport_hdr.num_pkts > 0 && burst->pkts[0] != nullptr && pool != nullptr) {
    unsigned ret =
        pool->enqueue_bulk(reinterpret_cast<void**>(burst->pkts[0]), burst->transport_hdr.num_pkts);
    if (ret != burst->transport_hdr.num_pkts) {
      DAQIRI_LOG_CRITICAL(
          "Asked to free {} packets, only enqueued {}", burst->transport_hdr.num_pkts, ret);
    }
  }

  if (burst->pkts[0] != nullptr) {
    tx_burst_pool_->put(reinterpret_cast<void*>(burst->pkts[0]));
  }
  if (burst->pkt_lens[0] != nullptr) {
    pkt_len_pool_->put(reinterpret_cast<void*>(burst->pkt_lens[0]));
  }
  reset_rdma_burst_metadata(burst);
  tx_meta->put(burst);
}

void RdmaEngine::initialize() {
  std::string load_error;
  verbs_ = detail::load_cm_api(load_error);
  if (verbs_ == nullptr) {
    DAQIRI_LOG_ERROR("Unable to load rdma runtime: {}", load_error);
    return;
  }

  bool server = false;
  bool client = false;

  // Populate pd_map_ with all IB devices and create PDs
  int num_ib_devices = 0;
  ibv_context** ib_devices = verbs_->cm_get_devices(&num_ib_devices);
  if (ib_devices == nullptr || num_ib_devices == 0) {
    DAQIRI_LOG_CRITICAL("No RDMA-capable devices found!");
    return;
  }

  DAQIRI_LOG_INFO("Found {} RDMA-capable devices", num_ib_devices);

  for (int i = 0; i < num_ib_devices; i++) {
    struct ibv_context* device = ib_devices[i];
    DAQIRI_LOG_INFO("Creating PD for device {}", (void*)device);

    // Get device attributes to determine number of ports
    struct ibv_device_attr device_attr;
    if (verbs_->query_device(device, &device_attr) != 0) {
      DAQIRI_LOG_ERROR("Failed to query device attributes for device {}", (void*)device);
      continue;
    }

    struct ibv_pd* pd = verbs_->alloc_pd(device);
    if (pd == nullptr) {
      DAQIRI_LOG_ERROR("Failed to allocate PD for device {}", (void*)device);
      continue;
    }
    DAQIRI_LOG_INFO("Created PD {} for device {}", (void*)pd, (void*)device);
    pd_map_[device] = pd;
  }

  // Assign each interface a port id. The RDMA engine drives the NIC through
  // libibverbs directly, so no DPDK EAL initialization is needed -- the rings
  // and pools are the libdpdk-free daqiri::Ring / daqiri::ObjectPool.
  int if_num = 0;
  for (auto& intf : cfg_.ifs_) {
    intf.port_id_ = if_num++;
  }

  // Set up memory region sizes (round buf_size up to the RDMA/GPU alignment).
  for (auto& mr : cfg_.mrs_) {
    const size_t rdma_alignment = std::max<size_t>(get_alignment(mr.second.kind_), GPU_PAGE_SIZE);
    mr.second.adj_size_ = (mr.second.buf_size_ + rdma_alignment - 1) & ~(rdma_alignment - 1);
  }

  for (const auto& intf : cfg_.ifs_) {
    if (intf.rdma_.mode_ == RDMAMode::SERVER) {
      server = true;
    } else {
      client = true;
    }
  }

  if (rdma_register_cfg_mrs() < 0) {
    DAQIRI_LOG_ERROR("Failed to register MRs");
    return;
  }

  if (setup_pools_and_rings() != 0) {
    DAQIRI_LOG_CRITICAL("Failed to setup pools and rings");
    return;
  }

  main_thread_ = std::thread(&RdmaEngine::run, this);

  DAQIRI_LOG_INFO("RDMA engine initialized");
  initialized_ = true;
}

void RdmaEngine::shutdown() {
  // Idempotency guard: shutdown() runs explicitly from the caller AND again
  // from ~RdmaEngine / ~SocketEngine during C++ __cxa_finalize. By the second call
  // the spdlog default logger (a function-local static created lazily on the
  // first DAQIRI_LOG_INFO) has already been destroyed, so any logging here
  // crashes inside spdlog::sink_it_. Skip the whole body on subsequent calls.
  if (!initialized_) {
    return;
  }
  DAQIRI_LOG_INFO("RDMA engine shutting down");
  rdma_force_quit.store(true);

  DAQIRI_LOG_INFO("Waiting for main thread to complete");
  main_thread_.join();
  {
    std::lock_guard<std::mutex> registry_lock(threads_mutex_);
    for (const auto& [id, params] : connection_params_) {
      (void)id;
      std::lock_guard<std::mutex> progress_lock(params->progress_mutex);
      params->ready_to_exit.store(true);
      if (params->direct_state) {
        params->direct_state->close();
      }
    }
    connection_params_.clear();
  }

  // Release the rings and pools (plain heap allocations now, no EAL memzones).
  while (!rx_rings_.empty()) {
    auto ring = rx_rings_.front();
    rx_rings_.pop();
    if (ring != nullptr) {
      daqiri::Ring::free(ring);
    }
  }
  while (!tx_rings_.empty()) {
    auto ring = tx_rings_.front();
    tx_rings_.pop();
    if (ring != nullptr) {
      daqiri::Ring::free(ring);
    }
  }
  rx_rings_map_.clear();
  tx_rings_map_.clear();

  if (pkt_len_pool_ != nullptr) {
    daqiri::ObjectPool::free(pkt_len_pool_);
    pkt_len_pool_ = nullptr;
  }
  if (rx_meta != nullptr) {
    daqiri::ObjectPool::free(rx_meta);
    rx_meta = nullptr;
  }
  if (tx_meta != nullptr) {
    daqiri::ObjectPool::free(tx_meta);
    tx_meta = nullptr;
  }
  if (tx_burst_pool_ != nullptr) {
    daqiri::ObjectPool::free(tx_burst_pool_);
    tx_burst_pool_ = nullptr;
  }
  for (auto& entry : mem_pools_) {
    if (entry.second != nullptr) {
      daqiri::Ring::free(entry.second);
    }
  }
  mem_pools_.clear();

  initialized_ = false;
}

void RdmaEngine::init_client() {
  // TODO: Implement client initialization
  // return Status::SUCCESS;
}

Status RdmaEngine::get_rx_burst(BurstParams** burst, uintptr_t conn_id, bool server) {
  if (burst == nullptr) {
    return Status::INVALID_PARAMETER;
  }
  *burst = nullptr;
  if (auto* direct = find_direct(conn_id)) {
    RdmaDirectLease lease(direct);
    return lease ? get_direct(*direct, burst, server) : Status::NOT_READY;
  }
  std::unique_lock<std::mutex> registry_lock(threads_mutex_);
  const auto connection = connection_params_.find(reinterpret_cast<struct rdma_cm_id*>(conn_id));
  if (connection == connection_params_.end()) {
    return Status::NOT_READY;
  }
  auto* params = connection->second;
  std::unique_lock<std::mutex> progress_lock(params->progress_mutex);
  registry_lock.unlock();
  if (rdma_force_quit.load() || params->ready_to_exit.load()) {
    return Status::NOT_READY;
  }
  poll_completions(server, params, params->direct_rx, params->direct_tx);
  if (!params->qp_params.rx_ring->dequeue(reinterpret_cast<void**>(burst))) {
    return Status::NOT_READY;
  }

  return Status::SUCCESS;
}

void RdmaEngine::free_rx_metadata(BurstParams* burst) {
  if (burst == nullptr) { return; }
  reset_rdma_burst_metadata(burst);
  rx_meta->put(burst);
}

void RdmaEngine::free_tx_metadata(BurstParams* burst) {
  if (burst == nullptr) { return; }
  reset_rdma_burst_metadata(burst);
  tx_meta->put(burst);
}

void RdmaEngine::print_stats() {
  DAQIRI_LOG_INFO("daqiri RDMA engine stats");

  size_t total_server_queues = 0;
  size_t server_queues_with_client = 0;
  size_t active_server_queues = 0;

  for (const auto& server_entry : server_q_params_) {
    const auto& qparams = server_entry.second;
    total_server_queues += qparams.size();

    for (const auto& q : qparams) {
      if (q.client_id != nullptr) {
        server_queues_with_client++;
      }
      if (q.active) {
        active_server_queues++;
      }
    }
  }

  size_t active_client_queues = 0;
  for (const auto& client_entry : client_q_params_) {
    if (client_entry.second.active) {
      active_client_queues++;
    }
  }

  size_t tx_ring_entries = 0;
  size_t rx_ring_entries = 0;
  for (const auto& it : tx_rings_map_) {
    if (it.second != nullptr) {
      tx_ring_entries += (it.second)->count();
    }
  }
  for (const auto& it : rx_rings_map_) {
    if (it.second != nullptr) {
      rx_ring_entries += (it.second)->count();
    }
  }

  size_t worker_threads = 0;
  {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    worker_threads = worker_threads_.size();
    for (const auto& [id, params] : connection_params_) {
      std::lock_guard<std::mutex> progress_lock(params->progress_mutex);
      DAQIRI_LOG_INFO("  conn={} rx_poll={} tx_poll={} max_inline_data={} inline_wqes={}",
                      (void*)id, params->direct_rx ? "direct" : "indirect",
                      params->direct_tx ? "direct" : "indirect", params->max_inline_data,
                      params->inline_wqes.load());
    }
  }

  DAQIRI_LOG_INFO("  initialized={} stop_requested={}", initialized_,
                  rdma_force_quit.load() ? "true" : "false");
  DAQIRI_LOG_INFO(
      "  listeners={} server_queues_total={} server_queues_with_client={} "
      "server_queues_active={}",
      server_q_params_.size(), total_server_queues, server_queues_with_client,
      active_server_queues);
  DAQIRI_LOG_INFO("  client_connections={} active_client_connections={}", client_q_params_.size(),
                  active_client_queues);
  DAQIRI_LOG_INFO("  worker_threads={} pds={} registered_mrs={} remote_mrs={} endpoints={}",
                  worker_threads, pd_map_.size(), mrs_.size(), remote_mrs_.size(),
                  endpoints_.size());
  DAQIRI_LOG_INFO("  tx_rings={} tx_ring_pending={} rx_rings={} rx_ring_pending={}",
                  tx_rings_map_.size(), tx_ring_entries, rx_rings_map_.size(), rx_ring_entries);

  if (tx_burst_pool_ != nullptr) {
    DAQIRI_LOG_INFO("  tx_burst_pool avail={} in_use={}", tx_burst_pool_->avail_count(),
                    tx_burst_pool_->in_use_count());
  }
  if (rx_meta != nullptr) {
    DAQIRI_LOG_INFO("  rx_meta_pool avail={} in_use={}", rx_meta->avail_count(),
                    rx_meta->in_use_count());
  }
  if (tx_meta != nullptr) {
    DAQIRI_LOG_INFO("  tx_meta_pool avail={} in_use={}", tx_meta->avail_count(),
                    tx_meta->in_use_count());
  }
  if (pkt_len_pool_ != nullptr) {
    DAQIRI_LOG_INFO("  pkt_len_pool avail={} in_use={}", pkt_len_pool_->avail_count(),
                    pkt_len_pool_->in_use_count());
  }
}

// Also need destructor implementation
RdmaEngine::~RdmaEngine() {
  for (auto& state : direct_states_) {
    state->close();
  }
  // Ensure all threads are joined
  {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    for (auto& thread_pair : worker_threads_) {
      if (thread_pair.second.joinable()) {
        thread_pair.second.join();
      }
    }
    worker_threads_.clear();
  }

  // Destroy event channel
  if (cm_event_channel_ != nullptr) {
    verbs_->cm_destroy_event_channel(cm_event_channel_);
    cm_event_channel_ = nullptr;
  }

  // Registrations hold references to their PDs and backing allocations. Drop
  // them before deallocating PDs; Engine's base destructor frees the buffers.
  for (auto& [name, params] : mrs_) {
    (void)name;
    for (auto& [pd, mr] : params.ctx_mr_map_) {
      (void)pd;
      if (mr != nullptr) {
        verbs_->dereg_mr(mr);
      }
    }
    params.ctx_mr_map_.clear();
  }
  mrs_.clear();

  // Deallocate PDs
  for (auto& entry : pd_map_) {
    if (entry.second != nullptr) {
      verbs_->dealloc_pd(entry.second);
    }
  }
  pd_map_.clear();

  // Covers the partial-init / no-shutdown path. shutdown() already releases and
  // nulls these on the normal path; the null/empty checks make this idempotent.
  while (!rx_rings_.empty()) {
    auto ring = rx_rings_.front();
    rx_rings_.pop();
    daqiri::Ring::free(ring);
  }
  while (!tx_rings_.empty()) {
    auto ring = tx_rings_.front();
    tx_rings_.pop();
    daqiri::Ring::free(ring);
  }
  if (pkt_len_pool_ != nullptr) {
    daqiri::ObjectPool::free(pkt_len_pool_);
    pkt_len_pool_ = nullptr;
  }
  if (rx_meta != nullptr) {
    daqiri::ObjectPool::free(rx_meta);
    rx_meta = nullptr;
  }
  if (tx_meta != nullptr) {
    daqiri::ObjectPool::free(tx_meta);
    tx_meta = nullptr;
  }
  if (tx_burst_pool_ != nullptr) {
    daqiri::ObjectPool::free(tx_burst_pool_);
    tx_burst_pool_ = nullptr;
  }
  for (auto& entry : mem_pools_) {
    if (entry.second != nullptr) {
      daqiri::Ring::free(entry.second);
    }
  }
  rx_rings_map_.clear();
  tx_rings_map_.clear();
  mem_pools_.clear();
}

// RDMA-specific functions that were declared but not implemented
Status RdmaEngine::register_mr(std::string name, int intf, void* addr, size_t len, int flags) {
  // TODO: Implement register_mr
  return Status::SUCCESS;
}
}  // namespace daqiri
