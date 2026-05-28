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
#include <cstdlib>
#include <map>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_errno.h>
#include <rte_mbuf.h>
#include "src/dpdk_log.h"
#include "daqiri_rdma_mgr.h"

/* The ordering of most RDMA/CM setup follows the ordering specified here:
   https://man7.org/linux/man-pages/man7/rdma_cm.7.html
   The exception is that there is no standard way to pass around keys, so we use standard
   sends and receives.
*/

namespace daqiri {
std::atomic<bool> rdma_force_quit = false;

bool RdmaMgr::set_config_and_initialize(const NetworkConfig& cfg) {
  DAQIRI_LOG_INFO("Setting up RDMA manager");
  cfg_ = cfg;
  initialize();

  return initialized_;
}

// Common DAQIRI functions
Status RdmaMgr::set_packet_lengths(BurstParams* burst, int idx,
                                   const std::initializer_list<int>& lens) {
  assert(lens.size() == 1);  // Split not supported yet
  burst->pkt_lens[0][idx] = lens.begin()[0];
  return Status::SUCCESS;
}

void* RdmaMgr::get_segment_packet_ptr(BurstParams* burst, int seg, int idx) {
  return burst->pkts[seg][idx];
}

void* RdmaMgr::get_packet_ptr(BurstParams* burst, int idx) {
  return burst->pkts[0][idx];
}

uint32_t RdmaMgr::get_segment_packet_length(BurstParams* burst, int seg, int idx) {
  return burst->pkt_lens[seg][idx];
}

uint32_t RdmaMgr::get_packet_length(BurstParams* burst, int idx) {
  return burst->pkt_lens[0][idx];
}

Status RdmaMgr::set_eth_header(BurstParams* burst, int idx, char* dst_addr) {
  DAQIRI_LOG_CRITICAL("Cannot set Ethernet header in RDMA mode");
  return Status::NOT_SUPPORTED;
}

Status RdmaMgr::set_ipv4_header(BurstParams* burst, int idx, int ip_len, uint8_t proto,
                                unsigned int src_host, unsigned int dst_host) {
  DAQIRI_LOG_CRITICAL("Cannot set IPv4 header in RDMA mode");
  return Status::NOT_SUPPORTED;
}

Status RdmaMgr::set_udp_header(BurstParams* burst, int idx, int udp_len, uint16_t src_port,
                               uint16_t dst_port) {
  DAQIRI_LOG_CRITICAL("Cannot set UDP header in RDMA mode");
  return Status::NOT_SUPPORTED;
}

Status RdmaMgr::set_udp_payload(BurstParams* burst, int idx, void* data, int len) {
  rte_memcpy(burst->pkts[0][idx], data, len);
  return Status::SUCCESS;
}

uint64_t RdmaMgr::get_burst_tot_byte(BurstParams* burst) {
  return 0;
}

BurstParams* RdmaMgr::create_tx_burst_params() {
  return new BurstParams();
}

bool RdmaMgr::get_ip_from_interface(const std::string_view& if_name, sockaddr_in& addr) {
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

inline bool RdmaMgr::ack_event(rdma_cm_event* cm_event) {
  int ret = rdma_ack_cm_event(cm_event);
  if (ret != 0) {
    DAQIRI_LOG_ERROR("Failed to acknowledge CM event: {}", ret);
    return false;
  } else {
    DAQIRI_LOG_INFO("Acknowledged CM event: {}", rdma_event_str(cm_event->event));
  }
  return true;
}

int RdmaMgr::mr_access_to_ibv(uint32_t access) {
  int ibv_access = 0;

  if (access & MEM_ACCESS_LOCAL) { ibv_access |= IBV_ACCESS_LOCAL_WRITE; }
  if (access & MEM_ACCESS_RDMA_READ) { ibv_access |= IBV_ACCESS_REMOTE_READ; }
  if (access & MEM_ACCESS_RDMA_WRITE) { ibv_access |= IBV_ACCESS_REMOTE_WRITE; }
  ibv_access |= IBV_ACCESS_RELAXED_ORDERING;

  return ibv_access;
}

int RdmaMgr::rdma_register_mr(const MemoryRegionConfig& mr, void* ptr) {
  rdma_mr_params params{};
  params.params_ = mr;
  params.ptr_ = ptr;
  DAQIRI_LOG_INFO("Registering MR {} with ibverbs {}", mr.name_, pd_map_.size());
  // For now register the MR with every PD we have
  for (const auto& pd : pd_map_) {
    if (pd.second != nullptr) {
      int access = mr_access_to_ibv(mr.access_);
      params.ctx_mr_map_[pd.second] =
          ibv_reg_mr(pd.second, ptr, mr.adj_size_ * mr.num_bufs_, access);
      if (params.ctx_mr_map_[pd.second] == nullptr &&
          (access & IBV_ACCESS_RELAXED_ORDERING) != 0) {
        const int fallback_access = access & ~IBV_ACCESS_RELAXED_ORDERING;
        DAQIRI_LOG_WARN(
            "Failed to register MR {} with relaxed ordering on PD {}; retrying without it",
            mr.name_,
            (void*)pd.second);
        params.ctx_mr_map_[pd.second] =
            ibv_reg_mr(pd.second, ptr, mr.adj_size_ * mr.num_bufs_, fallback_access);
        access = fallback_access;
      }
      if (params.ctx_mr_map_[pd.second] == nullptr) {
        DAQIRI_LOG_CRITICAL("Failed to register MR {} on PD {}", mr.name_, (void*)pd.second);
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

int RdmaMgr::rdma_register_cfg_mrs() {
  DAQIRI_LOG_INFO("Registering memory regions");

  if (allocate_memory_regions() != Status::SUCCESS) {
    DAQIRI_LOG_CRITICAL("Failed to allocate memory");
    return -1;
  }

  for (const auto& mr : cfg_.mrs_) {
    // Allocate 1 more than the user specifies because DPDK can give a capacity of size - 1
    auto ring = rte_ring_create(
        mr.second.name_.c_str(), rte_align32pow2(mr.second.num_bufs_ + 1), rte_socket_id(), 0);

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

Status RdmaMgr::wait_on_key_xchg() {
  return Status::SUCCESS;
}

/**
 * Set up all parameters needed for a newly-connected client
 */
int RdmaMgr::setup_thread_params(rdma_thread_params* params, bool is_server) {
  // RX/TX queues should be symmetric with RDMA
  rdma_qp_params qp_params;
  const int port_id = cfg_.ifs_[static_cast<int>(params->if_idx)].port_id_;

  DAQIRI_LOG_INFO("Creating RX and TX CQs for client ID {}", (void*)params->client_id);
  qp_params.rx_cq = ibv_create_cq(params->client_id->verbs, MAX_CQ, nullptr, nullptr, 0);
  if (qp_params.rx_cq == nullptr) {
    DAQIRI_LOG_CRITICAL("Failed to create RX queue pair! {}", strerror(errno));
    return -1;
  }

  qp_params.tx_cq = ibv_create_cq(params->client_id->verbs, MAX_CQ, nullptr, nullptr, 0);
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

  DAQIRI_LOG_INFO(
      "Creating QP for client ID {} with PD {}", (void*)params->client_id, (void*)params->pd);
  int ret = rdma_create_qp(params->client_id, params->pd, &qp_params.qp_attr);
  if (ret != 0) {
    DAQIRI_LOG_CRITICAL("Failed to create QP: {}", strerror(errno));
    return -1;
  }

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

int RdmaMgr::destroy_thread_params(rdma_thread_params* params) {
  DAQIRI_LOG_INFO("Destroying thread params for client {}", (void*)params->client_id);
  // First destroy the QP
  if (params->client_id->qp) { rdma_destroy_qp(params->client_id); }

  // Then destroy the CQs
  if (params->qp_params.tx_cq) {
    ibv_destroy_cq(params->qp_params.tx_cq);
    params->qp_params.tx_cq = nullptr;
  }

  if (params->qp_params.rx_cq) {
    ibv_destroy_cq(params->qp_params.rx_cq);
    params->qp_params.rx_cq = nullptr;
  }

  client_params_mutex_.lock();
  client_q_params_.erase(params->client_id);
  client_params_mutex_.unlock();

  return 0;
}

inline int RdmaMgr::set_affinity(int cpu_core) {
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

Status RdmaMgr::send_tx_burst(BurstParams* burst) {
  struct rte_ring* ring;

  const auto conn_id = get_connection_id(burst);
  auto ri = tx_rings_map_.find(reinterpret_cast<struct rdma_cm_id*>(conn_id));
  if (ri == tx_rings_map_.end()) {
    DAQIRI_LOG_ERROR("Invalid server connection ID in send_tx_burst: {}", conn_id);
    return Status::INVALID_PARAMETER;
  }

  ring = ri->second;

  if (rte_ring_enqueue(ring, reinterpret_cast<void*>(burst)) != 0) {
    free_tx_burst(burst);
    free_tx_metadata(burst);
    DAQIRI_LOG_CRITICAL("Failed to enqueue TX work");
    return Status::NO_SPACE_AVAILABLE;
  }

  return Status::SUCCESS;
}

RDMAOpCode RdmaMgr::ibv_opcode_to_daqiri_opcode(ibv_wc_opcode opcode) {
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

/**
 * Worker thread for a client or server. Each thread handles one queue pair.
 */
void RdmaMgr::rdma_thread(bool is_server, rdma_thread_params* tparams) {
  struct ibv_wc wc;
  int num_comp;
  BurstParams* msg;
  int send_signal_every = 1;
  if (const char* env = std::getenv("DAQIRI_RDMA_SEND_SIGNAL_EVERY")) {
    const long parsed = strtol(env, nullptr, 10);
    if (parsed > 0) { send_signal_every = static_cast<int>(parsed); }
  }
  uint64_t sends_since_signal = 0;
  const auto& qref = cfg_.ifs_[tparams->if_idx].tx_.queues_[tparams->queue_idx];
  const long cpu_core = strtol(qref.common_.cpu_core_.c_str(), NULL, 10);
  struct rte_ring* tx_ring = tparams->qp_params.tx_ring;
  struct rte_ring* rx_ring = tparams->qp_params.rx_ring;
  std::map<uint64_t, BurstParams*> outstanding_send_wr_ids;
  std::unordered_map<uint64_t, BurstParams*> outstanding_receive_wr_ids;

  if (set_affinity(cpu_core) != 0) {
    DAQIRI_LOG_CRITICAL("Failed to set RDMA core affinity");
    return;
  }

  DAQIRI_LOG_INFO("Affined {} RDMA thread to core {}", is_server ? "Server" : "Client", cpu_core);

  // Main TX loop. Wait for send requests from the transmitters to arrive for sending. Also
  // periodically poll the CQ.
  while (!rdma_force_quit.load()) {
    // Check RQ first to reduce latency
    while (true) {
      num_comp = ibv_poll_cq(tparams->qp_params.rx_cq, 1, &wc);
      if (num_comp == 0) { break; }
      DAQIRI_LOG_DEBUG("GOT RX COMPLETION in thread {} core {} wrid {}",
                         (void*)tparams->client_id,
                         cpu_core,
                         (int64_t)wc.wr_id);
      if (wc.status != IBV_WC_SUCCESS) {
        DAQIRI_LOG_ERROR("CQ error {} for WRID {} and opcode {}",
                           (int)wc.status,
                           (int64_t)wc.wr_id,
                           (int)wc.opcode);
        continue;
      }

      if (wc.opcode == IBV_WC_RECV) {
        auto it = outstanding_receive_wr_ids.find(wc.wr_id);
        if (it == outstanding_receive_wr_ids.end()) {
          DAQIRI_LOG_CRITICAL("WR ID {} not found in outstanding RECEIVE WR IDs", wc.wr_id);
          continue;
        }

        msg = it->second;

        const auto conn_id = get_connection_id(msg);
        const auto expected_conn_id = reinterpret_cast<uintptr_t>(tparams->client_id);
        if (conn_id != expected_conn_id) {
          DAQIRI_LOG_CRITICAL("Wrong connection ID in receive completion {}: {} != {}",
                                wc.wr_id,
                                conn_id,
                                expected_conn_id);
        }

        outstanding_receive_wr_ids.erase(it);
      } else {
        msg = create_burst_params();
      }
      const bool msg_owns_packets = wc.opcode == IBV_WC_RECV;

      // Only populate a header to indicate which burst needs to be freed
      // msg->transport_hdr.opcode  = ibv_opcode_to_daqiri_opcode(wc.opcode);
      msg->transport_hdr.status =
          wc.status == IBV_WC_SUCCESS ? Status::SUCCESS : Status::GENERIC_FAILURE;
      // set_connection_id(msg, reinterpret_cast<uintptr_t>(tparams->client_id));
      msg->transport_hdr.server = is_server;
      msg->transport_hdr.tx = false;
      // msg->transport_hdr.wr_id   = wc.wr_id;

      if (rte_ring_enqueue(rx_ring, reinterpret_cast<void*>(msg)) != 0) {
        DAQIRI_LOG_CRITICAL("Failed to enqueue RX completion message");
        if (msg_owns_packets) {
          free_tx_burst(msg);
        } else {
          free_tx_metadata(msg);
        }
        return;
      }
    }

    // Check TX CQ for completion
    while (true) {
      num_comp = ibv_poll_cq(tparams->qp_params.tx_cq, 1, &wc);
      if (num_comp == 0) { break; }
      DAQIRI_LOG_DEBUG("GOT TX COMPLETION in thread {} core {} wrid {}",
                         (void*)tparams->client_id,
                         cpu_core,
                         (int64_t)wc.wr_id);
      if (wc.status != IBV_WC_SUCCESS) {
        DAQIRI_LOG_ERROR("CQ error on {}: {} ({}) for WRID {} and opcode {}",
                           is_server ? "server" : "client",
                           ibv_wc_status_str(wc.status),
                           (int)wc.status,
                           (int64_t)wc.wr_id,
                           (int)wc.opcode);
        continue;
      }

      if (wc.opcode == IBV_WC_SEND) {
        auto it = outstanding_send_wr_ids.find(wc.wr_id);
        if (it == outstanding_send_wr_ids.end()) {
          DAQIRI_LOG_CRITICAL("WR ID {} not found in outstanding SEND WR IDs", wc.wr_id);
          continue;
        }

        auto completed_end = outstanding_send_wr_ids.upper_bound(wc.wr_id);
        for (auto completed_it = outstanding_send_wr_ids.begin(); completed_it != completed_end;) {
          msg = completed_it->second;

          const auto conn_id = get_connection_id(msg);
          const auto expected_conn_id = reinterpret_cast<uintptr_t>(tparams->client_id);
          if (conn_id != expected_conn_id) {
            DAQIRI_LOG_CRITICAL("Wrong connection ID in send completion {}: {} != {}",
                                  completed_it->first,
                                  conn_id,
                                  expected_conn_id);
          }

          completed_it = outstanding_send_wr_ids.erase(completed_it);

          msg->transport_hdr.tx = true;
          msg->transport_hdr.status =
              wc.status == IBV_WC_SUCCESS ? Status::SUCCESS : Status::GENERIC_FAILURE;
          msg->transport_hdr.server = is_server;

          if (rte_ring_enqueue(rx_ring, reinterpret_cast<void*>(msg)) != 0) {
            DAQIRI_LOG_CRITICAL("Failed to enqueue RX completion message");
            free_tx_burst(msg);
            return;
          }
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

      if (rte_ring_enqueue(rx_ring, reinterpret_cast<void*>(msg)) != 0) {
        DAQIRI_LOG_CRITICAL("Failed to enqueue RX completion message");
        free_tx_metadata(msg);
        return;
      }
    }

    // Now handle any incoming messages
    BurstParams* burst;

    // ssize_t bytes = mq_receive(tparams.tx_mq, reinterpret_cast<char*>(&burst), sizeof(burst),
    // nullptr);
    if (rte_ring_dequeue(tx_ring, reinterpret_cast<void**>(&burst)) != 0) { continue; }

    const auto local_mr = mrs_.find(std::string(burst->transport_hdr.local_mr_name));
    if (local_mr == mrs_.end()) {
      DAQIRI_LOG_CRITICAL("Couldn't find MR with name {} in registry",
                            burst->transport_hdr.local_mr_name);
      free_tx_burst(burst);
      continue;
    }

    switch (burst->transport_hdr.opcode) {
      case RDMAOpCode::SEND: {
        // Get lkey for this PD
        auto pd = pd_map_.find(tparams->client_id->verbs);
        if (pd == pd_map_.end()) {
          DAQIRI_LOG_CRITICAL("Couldn't find PD for client");
          free_tx_burst(burst);
          continue;
        }

        // Get lkey for this MR
        auto lkey = local_mr->second.ctx_mr_map_.find(pd->second);
        if (lkey == local_mr->second.ctx_mr_map_.end()) {
          DAQIRI_LOG_CRITICAL("Couldn't find MR with name {} in registry",
                                burst->transport_hdr.local_mr_name);
          free_tx_burst(burst);
          continue;
        }

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
          // Keep signaled completions frequent enough for callers that use SEND
          // completions as their credit to refill the outstanding window.
          const bool signal_send = send_signal_every == 1 ||
                                   sends_since_signal + 1 >= send_signal_every ||
                                   outstanding_send_wr_ids.size() >= 15;
          wr.send_flags = signal_send ? IBV_SEND_SIGNALED : 0;

          int ret = ibv_post_send(tparams->client_id->qp, &wr, &bad_wr);
          if (ret != 0) {
            DAQIRI_LOG_CRITICAL("Failed to post SEND request, errno: {}", strerror(errno));
            free_tx_burst(burst);
            continue;
          }
          if (signal_send) {
            sends_since_signal = 0;
          } else {
            sends_since_signal++;
          }

          outstanding_send_wr_ids[burst->transport_hdr.wr_id + p] = burst;
        }

        break;
      }
      case RDMAOpCode::RECEIVE: {
        // Get lkey for this PD
        auto pd = pd_map_.find(tparams->client_id->verbs);
        if (pd == pd_map_.end()) {
          DAQIRI_LOG_CRITICAL("Couldn't find PD for client");
          free_tx_burst(burst);
          continue;
        }

        // Get lkey for this MR
        auto lkey = local_mr->second.ctx_mr_map_.find(pd->second);
        if (lkey == local_mr->second.ctx_mr_map_.end()) {
          DAQIRI_LOG_CRITICAL("Couldn't find MR with name {} in registry",
                                burst->transport_hdr.local_mr_name);
          free_tx_burst(burst);
          continue;
        }

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
          ret = ibv_post_recv(tparams->client_id->qp, &recv_wr, &bad_wr);
          if (ret) {
            DAQIRI_LOG_CRITICAL("ibv_post_recv failed: {}", strerror(errno));
            free_tx_burst(burst);
            continue;
          }

          outstanding_receive_wr_ids[burst->transport_hdr.wr_id + p] = burst;
        }
        break;
      }
      case RDMAOpCode::RDMA_WRITE:
        [[fallthrough]];
      case RDMAOpCode::RDMA_WRITE_IMM: {
        break;
      }
      case RDMAOpCode::RDMA_READ: {
        break;
      }
    }
  }

  DAQIRI_LOG_INFO("{} RDMA thread exiting on core {}", is_server ? "Server" : "Client", cpu_core);
}

Status RdmaMgr::rdma_connect_to_server(const std::string& dst_addr, uint16_t dst_port,
                                       uintptr_t* conn_id) {
  return rdma_connect_to_server(dst_addr, dst_port, "", conn_id);
}

RDMAOpCode RdmaMgr::rdma_get_opcode(BurstParams* burst) {
  return burst->transport_hdr.opcode;
}

Status RdmaMgr::rdma_set_header(BurstParams* burst, RDMAOpCode op_code, uintptr_t conn_id,
                                bool is_server, int num_pkts, uint64_t wr_id,
                                const std::string& local_mr_name) {
  burst->transport_hdr.opcode = op_code;
  set_connection_id(burst, conn_id);
  burst->transport_hdr.server = is_server;
  burst->transport_hdr.num_pkts = num_pkts;
  burst->transport_hdr.num_segs = 1;
  burst->transport_hdr.wr_id = wr_id;
  snprintf(burst->transport_hdr.local_mr_name,
           sizeof(burst->transport_hdr.local_mr_name),
           "%s",
           local_mr_name.c_str());
  return Status::SUCCESS;
}

Status RdmaMgr::rdma_get_server_conn_id(const std::string& server_addr, uint16_t server_port,
                                        uintptr_t* conn_id) {
  const auto iter = server_str_to_id_.find(server_addr + ":" + std::to_string(server_port));
  if (iter == server_str_to_id_.end()) {
    DAQIRI_LOG_CRITICAL("Couldn't find server params for address {}", server_addr);
    return Status::INVALID_PARAMETER;
  }

  // Now that we have the server's listening ID, we need to find the next queue ID that's not
  // already in use
  const auto server_id = iter->second;
  const auto server_params = server_q_params_.find(server_id);
  if (server_params == server_q_params_.end()) {
    DAQIRI_LOG_CRITICAL("Couldn't find server params for address {}", server_addr);
    return Status::INVALID_PARAMETER;
  }

  // Find the next queue ID that's not already in use
  for (size_t i = 0; i < server_params->second.size(); i++) {
    if (server_params->second[i].client_id != nullptr && !server_params->second[i].active) {
      auto* client_id = server_params->second[i].client_id;
      std::lock_guard<std::mutex> lock(threads_mutex_);
      if (worker_threads_.find(client_id) == worker_threads_.end() ||
          tx_rings_map_.find(client_id) == tx_rings_map_.end() ||
          rx_rings_map_.find(client_id) == rx_rings_map_.end()) {
        continue;
      }

      *conn_id = reinterpret_cast<uintptr_t>(client_id);
      server_params->second[i].active = true;
      DAQIRI_LOG_INFO("Found available queue ID for server {}:{} with cm_id {}",
                        server_addr,
                        server_port,
                        (void*)client_id);
      return Status::SUCCESS;
    }
  }

  DAQIRI_LOG_CRITICAL(
    "Couldn't find an available queue ID for server {}:{}. "
    "Maybe no clients have tried to connect yet?", server_addr, server_port);
  return Status::NO_SPACE_AVAILABLE;
}

Status RdmaMgr::rdma_connect_to_server(const std::string& dst_addr, uint16_t dst_port,
                                       const std::string& src_addr, uintptr_t* conn_id) {
  struct rdma_cm_id* cm_id = nullptr;
  struct rdma_event_channel* ec = nullptr;
  struct rdma_cm_event* event = nullptr;
  struct rdma_conn_param conn_param = {};

  if (!initialized_) {
    DAQIRI_LOG_WARN("RDMA manager not initialized yet. Not trying to connect to server");
    return Status::NOT_READY;
  }

  DAQIRI_LOG_INFO("Connecting to server {} on port {}", dst_addr, dst_port);

  *conn_id = 0;

  ec = rdma_create_event_channel();
  if (ec == nullptr) {
    DAQIRI_LOG_CRITICAL("Failed to create a CM channel: {}", strerror(errno));
    return Status::CONNECT_FAILURE;
  }

  // Create RDMA id
  if (rdma_create_id(ec, &cm_id, nullptr, RDMA_PS_TCP)) {
    DAQIRI_LOG_CRITICAL("Failed to create ID");
    rdma_destroy_event_channel(ec);
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
  if (rdma_resolve_addr(cm_id, src_addr_p, (struct sockaddr*)&addr, 2000)) {
    DAQIRI_LOG_CRITICAL("Failed to resolve address");
    rdma_destroy_event_channel(ec);
    return Status::CONNECT_FAILURE;
  }

  // Wait for address resolution event
  if (rdma_get_cm_event(ec, &event)) {
    DAQIRI_LOG_CRITICAL("Failed to get CM event");
    rdma_destroy_event_channel(ec);
    return Status::CONNECT_FAILURE;
  }

  if (event->event != RDMA_CM_EVENT_ADDR_RESOLVED) {
    if (event->event == RDMA_CM_EVENT_ADDR_ERROR) {
      DAQIRI_LOG_CRITICAL("Failed to resolve address");
    } else {
      DAQIRI_LOG_CRITICAL("Unexpected event from rdma_resolve_addr: {}", (int)event->event);
    }
    rdma_destroy_event_channel(ec);
    return Status::CONNECT_FAILURE;
  } else {
    DAQIRI_LOG_INFO("Client resolved server address: {}", (void*)cm_id);
  }

  rdma_ack_cm_event(event);

  // Resolve route to server
  if (rdma_resolve_route(cm_id, 2000)) {
    DAQIRI_LOG_CRITICAL("Failed to resolve route");
    rdma_destroy_event_channel(ec);
    return Status::CONNECT_FAILURE;
  }

  // Wait for route resolution event
  if (rdma_get_cm_event(ec, &event)) {
    DAQIRI_LOG_CRITICAL("Failed to get CM event");
    rdma_destroy_event_channel(ec);
    return Status::CONNECT_FAILURE;
  }

  if (event->event != RDMA_CM_EVENT_ROUTE_RESOLVED) {
    if (event->event == RDMA_CM_EVENT_REJECTED) {
      DAQIRI_LOG_WARN(
          "Server rejected connection. Check if server is running and accepting connections");
    } else {
      DAQIRI_LOG_CRITICAL("Unexpected event: {}", (int)event->event);
    }

    rdma_destroy_event_channel(ec);
    return Status::CONNECT_FAILURE;
  } else {
    DAQIRI_LOG_INFO("Client {} resolved route for server", (void*)cm_id);
    rdma_ack_cm_event(event);
  }

  struct sockaddr* source_addr = rdma_get_local_addr(cm_id);

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
    rdma_destroy_id(cm_id);
    rdma_destroy_event_channel(ec);
    return Status::INVALID_PARAMETER;
  }

  // Construct the params directly in the map using try_emplace
  client_params_mutex_.lock();
  auto [iter, inserted] = client_q_params_.try_emplace(cm_id);

  if (!inserted) {
    client_params_mutex_.unlock();
    DAQIRI_LOG_CRITICAL("Failed to insert client params into map");
    return Status::CONNECT_FAILURE;
  }

  auto& params = iter->second;
  params.client_id = cm_id;
  params.pd = pd_map_[cm_id->verbs];
  params.if_idx = client_port;
  params.queue_idx = client_q_params_.size() - 1;
  client_params_mutex_.unlock();
  setup_thread_params(&params, false);

  // Set up connection parameters
  memset(&conn_param, 0, sizeof(conn_param));
  conn_param.responder_resources = 1;
  conn_param.initiator_depth = 1;
  conn_param.rnr_retry_count = 7;

  // Connect to server
  if (rdma_connect(cm_id, &conn_param) != 0) {
    DAQIRI_LOG_CRITICAL("Failed to connect to server");
    destroy_thread_params(&params);
    rdma_destroy_event_channel(ec);
    return Status::CONNECT_FAILURE;
  } else {
    DAQIRI_LOG_INFO("Client {} connected to server", (void*)cm_id);
  }

  // Wait for connection established event
  if (rdma_get_cm_event(ec, &event)) {
    DAQIRI_LOG_CRITICAL("Failed to get CM event");
    destroy_thread_params(&params);
    rdma_destroy_event_channel(ec);
    return Status::CONNECT_FAILURE;
  }

  if (event->event != RDMA_CM_EVENT_ESTABLISHED) {
    if (event->event == RDMA_CM_EVENT_REJECTED) {
      DAQIRI_LOG_WARN(
          "Server rejected connection. Check if server is running and accepting connections");
    } else {
      DAQIRI_LOG_CRITICAL("Unexpected event: {}", (int)event->event);
    }
    rdma_ack_cm_event(event);
    rdma_destroy_event_channel(ec);
    destroy_thread_params(&params);
    return Status::CONNECT_FAILURE;
  } else {
    DAQIRI_LOG_INFO("Client {} established connection to server", (void*)cm_id);
  }

  rdma_ack_cm_event(event);

  DAQIRI_LOG_INFO("Launching client thread for {}", (void*)cm_id);

  // Store the connection ID for later use
  threads_mutex_.lock();
  worker_threads_[cm_id] = std::thread(&RdmaMgr::rdma_thread, this, false, &params);
  threads_mutex_.unlock();

  *conn_id = reinterpret_cast<uintptr_t>(cm_id);
  DAQIRI_LOG_INFO("Successfully connected to server {} on port {}", dst_addr, dst_port);

  // rdma_destroy_event_channel(ec);

  return Status::SUCCESS;
}

Status RdmaMgr::rdma_get_port_queue(uintptr_t conn_id, uint16_t* port, uint16_t* queue) {
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

Status RdmaMgr::get_tx_packet_burst(BurstParams* burst) {
  // RDMA isn't allowing split segments yet
  assert(burst->transport_hdr.num_segs == 1);
  assert(burst->transport_hdr.num_pkts <= MAX_RDMA_BATCH);
  auto burst_pool = mem_pools_.find(burst->transport_hdr.local_mr_name);
  if (burst_pool == mem_pools_.end()) {
    DAQIRI_LOG_ERROR("Failed to look up burst pool name for MR {}",
                       burst->transport_hdr.local_mr_name);
    return Status::INVALID_PARAMETER;
  }

  if (rte_mempool_get(tx_burst_pool_, reinterpret_cast<void**>(&burst->pkts[0])) != 0) {
    DAQIRI_LOG_ERROR("Failed to get packet from pool");
    return Status::NO_FREE_BURST_BUFFERS;
  }

  const unsigned num_pkts = static_cast<unsigned>(burst->transport_hdr.num_pkts);
  const unsigned rx =
      rte_ring_dequeue_bulk(burst_pool->second, reinterpret_cast<void**>(burst->pkts[0]), num_pkts, nullptr);
  if (rx != num_pkts) {
    DAQIRI_LOG_ERROR("Asked for {} packets, got {}", num_pkts, rx);
    rte_mempool_put(tx_burst_pool_, reinterpret_cast<void*>(burst->pkts[0]));
    burst->pkts[0] = nullptr;
    return Status::NO_FREE_BURST_BUFFERS;
  }

  // Allocate packet length buffer
  if (rte_mempool_get(pkt_len_pool_, reinterpret_cast<void**>(&burst->pkt_lens[0])) != 0) {
    DAQIRI_LOG_ERROR("Failed to get packet length buffer");
    rte_ring_enqueue_bulk(
        burst_pool->second, reinterpret_cast<void**>(burst->pkts[0]), num_pkts, nullptr);
    rte_mempool_put(tx_burst_pool_, reinterpret_cast<void*>(burst->pkts[0]));
    burst->pkts[0] = nullptr;
    return Status::NO_FREE_PACKET_BUFFERS;
  }

  return Status::SUCCESS;
}

bool RdmaMgr::is_tx_burst_available(BurstParams* burst) {
  auto burst_pool = mem_pools_.find(burst->transport_hdr.local_mr_name);
  if (burst_pool == mem_pools_.end()) {
    DAQIRI_LOG_ERROR("Failed to look up burst pool name for MR {}",
                       burst->transport_hdr.local_mr_name);
    return false;
  }

  if (rte_ring_count(burst_pool->second) < burst->transport_hdr.num_pkts) { return false; }

  return true;
}

void RdmaMgr::run() {
  int ret;

  DAQIRI_LOG_INFO("Starting RDMA CM main thread");

  if (set_affinity(cfg_.common_.master_core_) != 0) {
    DAQIRI_LOG_CRITICAL("Failed to set master core affinity");
    return;
  }

  // Create a channel for events. Only the master thread reads from this channel
  cm_event_channel_ = rdma_create_event_channel();
  if (cm_event_channel_ == nullptr) {
    DAQIRI_LOG_CRITICAL("Failed to create a CM channel: {}", strerror(errno));
    return;
  }

  // Set the channel to non-blocking so we can check for SIGINT
  int flags = fcntl(cm_event_channel_->fd, F_GETFL);
  fcntl(cm_event_channel_->fd, F_SETFL, flags | O_NONBLOCK);

  // Start an RDMA server on each server interface
  for (const auto& intf : cfg_.ifs_) {
    if (intf.rdma_.mode_ != RDMAMode::SERVER) { continue; }

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
    ret = rdma_create_id(cm_event_channel_, &s_id, nullptr, RDMA_PS_TCP);
    if (ret != 0) {
      DAQIRI_LOG_CRITICAL("Failed to create RDMA server ID {}", strerror(errno));
      return;
    }

    pd_params_[s_id] = {};
    pd_params_[s_id].server_id = s_id;
    pd_params_[s_id].if_idx = intf.port_id_;

    auto& vec = server_q_params_[s_id];
    vec.resize(intf.rx_.queues_.size());

    DAQIRI_LOG_INFO("Created RDMA server on {}:{} successfully with listener_id {}",
                      intf.address_,
                      intf.rdma_.port_,
                      (void*)s_id);

    ret = rdma_bind_addr(s_id, reinterpret_cast<struct sockaddr*>(&server_addr));
    if (ret != 0) {
      DAQIRI_LOG_CRITICAL("Failed to bind for RDMA server: {}", strerror(errno));
      return;
    }

    ret = rdma_listen(s_id, intf.rx_.queues_.size());
    if (ret != 0) {
      DAQIRI_LOG_CRITICAL("Failed to listen for RDMA server: {}", strerror(errno));
      return;
    }

    server_str_to_id_[intf.address_ + ":" + std::to_string(intf.rdma_.port_)] = s_id;

    DAQIRI_LOG_INFO("RDMA server successfully started on {} queues", intf.rx_.queues_.size());
  }

  DAQIRI_LOG_INFO("Entering CM main loop");

  // Our master thread's job is to wait on connections from clients, set up all the needed
  // information for them (QPs, MRs, etc), and spawn helper threads to monitor both TX and RX
  struct rdma_cm_event* cm_event = nullptr;
  while (!rdma_force_quit.load()) {
    ret = rdma_get_cm_event(cm_event_channel_, &cm_event);
    if (ret != 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // Sleep for 500 microseconds and try again
        usleep(500);
        continue;
      }

      DAQIRI_LOG_INFO("Failed to get CM event: {}", strerror(errno));
      continue;
    }

    DAQIRI_LOG_INFO("Received CM event: {}", rdma_event_str(cm_event->event));

    if (cm_event->status != 0) {
      DAQIRI_LOG_ERROR("Error status received in CM event {}: {}",
                         rdma_event_str(cm_event->event),
                         cm_event->status);
      if (rdma_ack_cm_event(cm_event) == 0) {
        DAQIRI_LOG_INFO("Acknowledged CM event: {}", rdma_event_str(cm_event->event));
      } else {
        DAQIRI_LOG_CRITICAL("Failed to acknowledge CM event {}: {}",
                              rdma_event_str(cm_event->event),
                              strerror(errno));
      }

      continue;
    }

    switch (cm_event->event) {
      case RDMA_CM_EVENT_CONNECT_REQUEST: {
        DAQIRI_LOG_INFO("Received new connection request for client ID {} on listener {}",
                          (void*)cm_event->id,
                          (void*)cm_event->listen_id);

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

          // Find first inactive queue
          int queue_idx = -1;
          for (int i = 0; i < server_iter->second.size(); i++) {
            if (!server_iter->second[i].active) { queue_idx = i; }

            // Also check if the client ID already exists
            if (cm_event->id == server_iter->second[i].client_id) {
              DAQIRI_LOG_CRITICAL("Client ID {} already exists in server ID {}",
                                    (void*)cm_event->id,
                                    (void*)listen_iter->second.server_id);
              rdma_reject(cm_event->id, nullptr, 0);
              break;
            }
          }

          if (queue_idx == -1) {
            DAQIRI_LOG_CRITICAL(
                "No free queues on server ID {}. Close at least one connection first.",
                (void*)cm_event->listen_id);
            rdma_reject(cm_event->id, nullptr, 0);
            break;
          }

          auto& params = server_iter->second[queue_idx];
          params.active = false;
          params.client_id = cm_event->id;
          params.pd = pd_map_[cm_event->id->verbs];
          params.if_idx = cm_event->id->port_num;
          params.queue_idx = queue_idx;

          setup_thread_params(&params, true);
          DAQIRI_LOG_INFO("Configured queues for client {}. Launching thread",
                            (void*)cm_event->id);

          ack_event(cm_event);

          struct rdma_conn_param conn_param = {};
          conn_param.responder_resources = 1;
          conn_param.initiator_depth = 1;
          conn_param.rnr_retry_count = 7;

          if (rdma_accept(params.client_id, &conn_param) != 0) {
            DAQIRI_LOG_CRITICAL("Failed to accept connection: {}", strerror(errno));
            rdma_reject(params.client_id, nullptr, 0);
            continue;
          } else {
            DAQIRI_LOG_INFO("Server accepted connection for client ID {}",
                              (void*)params.client_id);
          }
        }

        break;
      }
      case RDMA_CM_EVENT_ESTABLISHED: {
        bool found = false;
        DAQIRI_LOG_INFO("Received established event for client ID {}", (void*)cm_event->id);

        // Find which server the client is on
        for (auto& sp : server_q_params_) {
          for (auto& thread_params : sp.second) {
            if (thread_params.client_id == cm_event->id) {
              DAQIRI_LOG_INFO(
                  "Client ID {} is on server ID {}", (void*)cm_event->id, (void*)sp.first);
              found = true;

              DAQIRI_LOG_INFO("Connection established. Launching server thread for client {}",
                                (void*)cm_event->id);

              threads_mutex_.lock();
              worker_threads_[cm_event->id] =
                  std::thread(&RdmaMgr::rdma_thread, this, true, &thread_params);
              threads_mutex_.unlock();
              found = true;
              break;
            }
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

        bool found = false;
        for (auto& sp : server_q_params_) {
          for (auto& thread_params : sp.second) {
            if (thread_params.client_id == cm_event->id) {
              threads_mutex_.lock();
              worker_threads_[cm_event->id].join();
              worker_threads_.erase(cm_event->id);
              threads_mutex_.unlock();

              // Return the TX and RX rings to the pool
              if (thread_params.qp_params.tx_ring != nullptr) {
                tx_rings_.push(thread_params.qp_params.tx_ring);
                tx_rings_map_.erase(cm_event->id);
              }

              if (thread_params.qp_params.rx_ring != nullptr) {
                rx_rings_.push(thread_params.qp_params.rx_ring);
                rx_rings_map_.erase(cm_event->id);
              }

              thread_params.client_id = nullptr;
              thread_params.active = false;
              DAQIRI_LOG_INFO("Joined and removed client thread for ID {}", (void*)cm_event->id);
              found = true;
              break;
            }
          }
        }

        if (!found) {
          DAQIRI_LOG_CRITICAL("Received disconnected event for unknown client ID {}",
                                (void*)cm_event->id);
        }

        ack_event(cm_event);
        break;
      }
      default: {
        DAQIRI_LOG_INFO("Cannot handle event type {}", rdma_event_str(cm_event->event));
        ack_event(cm_event);
        break;
      }
    }
  }

  // Join any client threads we had spawned
  DAQIRI_LOG_INFO("Waiting for server TX/RX threads to complete");
  for (auto& thread : worker_threads_) { thread.second.join(); }

  DAQIRI_LOG_INFO("Finished cleaning up TX/RX workers");
}

int RdmaMgr::setup_pools_and_rings() {
  // RX rings
  DAQIRI_LOG_INFO("Setting up TX/RX per-queue rings");

  // Each connection ring is single-producer/single-consumer by design: one
  // rdma_thread owns the manager side and one application thread owns the API
  // side for a given conn_id. If multi-threaded app access per conn_id is ever
  // allowed, these rings must go back to MP/MC-safe flags.
  for (int i = 0; i < MAX_RDMA_CONNECTIONS; i++) {
    std::string ring_name = "RX_RING_" + std::to_string(i);
    DAQIRI_LOG_DEBUG("Setting up RX ring {}", ring_name);
    struct rte_ring* ring =
        rte_ring_create(ring_name.c_str(), 2048, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (ring == nullptr) {
      DAQIRI_LOG_CRITICAL("Failed to allocate RX ring {}!", ring_name);
      return -1;
    }
    rx_rings_.push(ring);

    ring_name = "TX_RING_" + std::to_string(i);
    DAQIRI_LOG_DEBUG("Setting up TX ring {}", ring_name);
    ring = rte_ring_create(
        ring_name.c_str(), 2048, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (ring == nullptr) {
      DAQIRI_LOG_CRITICAL("Failed to allocate TX ring {}!", ring_name);
      return -1;
    }
    tx_rings_.push(ring);
  }

  // Packet length buffers
  DAQIRI_LOG_DEBUG("Setting up RX meta pool");
  pkt_len_pool_ = rte_mempool_create("PKT_LEN_POOL",
                                     (1U << 11) - 1U,
                                     sizeof(uint32_t) * MAX_RDMA_BATCH,
                                     0,
                                     0,
                                     nullptr,
                                     nullptr,
                                     nullptr,
                                     nullptr,
                                     rte_socket_id(),
                                     0);
  if (pkt_len_pool_ == nullptr) {
    DAQIRI_LOG_CRITICAL("Failed to allocate packet length pool!");
    return -1;
  }

  DAQIRI_LOG_DEBUG("Setting up RX meta pool");
  rx_meta = rte_mempool_create("RX_META_POOL",
                               (1U << 11) - 1U,
                               sizeof(BurstParams),
                               0,
                               0,
                               nullptr,
                               nullptr,
                               nullptr,
                               nullptr,
                               rte_socket_id(),
                               0);
  if (rx_meta == nullptr) {
    DAQIRI_LOG_CRITICAL("Failed to allocate RX meta pool!");
    return -1;
  }

  DAQIRI_LOG_DEBUG("Setting up TX meta pool");
  tx_meta = rte_mempool_create("TX_META_POOL",
                               (1U << 11) - 1U,
                               sizeof(BurstParams),
                               0,
                               0,
                               nullptr,
                               nullptr,
                               nullptr,
                               nullptr,
                               rte_socket_id(),
                               0);
  if (tx_meta == nullptr) {
    DAQIRI_LOG_CRITICAL("Failed to allocate TX meta pool!");
    return -1;
  }

  tx_burst_pool_ = rte_mempool_create("TX_BURST_POOL",
                                      (1U << 11) - 1U,
                                      sizeof(void*) * MAX_RDMA_BATCH,
                                      0,
                                      0,
                                      nullptr,
                                      nullptr,
                                      nullptr,
                                      nullptr,
                                      rte_socket_id(),
                                      0);
  if (tx_burst_pool_ == nullptr) {
    DAQIRI_LOG_CRITICAL("Failed to allocate TX burst pool!");
    return -1;
  } else {
    DAQIRI_LOG_INFO("TX burst pool allocated at {}", (void*)tx_burst_pool_);
  }

  return 0;
}

void RdmaMgr::free_rx_burst(BurstParams* burst) {
  rte_mempool_put(rx_meta, burst);
}

void RdmaMgr::free_tx_burst(BurstParams* burst) {
  auto burst_pool = mem_pools_.find(burst->transport_hdr.local_mr_name);
  if (burst_pool != mem_pools_.end()) {
    int ret = rte_ring_enqueue_bulk(burst_pool->second,
                                    reinterpret_cast<void**>(burst->pkts[0]),
                                    burst->transport_hdr.num_pkts,
                                    nullptr);
    if (ret != burst->transport_hdr.num_pkts) {
      DAQIRI_LOG_CRITICAL(
          "Asked to free {} packets, only enqueued {}", burst->transport_hdr.num_pkts, ret);
    }
  }

  rte_mempool_put(tx_burst_pool_, (void*)burst->pkts[0]);
  rte_mempool_put(pkt_len_pool_, (void*)burst->pkt_lens[0]);
  burst->transport_hdr.num_pkts = 0;
  rte_mempool_put(tx_meta, burst);
}

void RdmaMgr::initialize() {
  bool server = false;
  bool client = false;
  int ret;

  // Cleanup-on-failure guard: mirrors DpdkMgr::initialize(). Any return path
  // that does not set initialized_ = true triggers rte_eal_cleanup() and
  // unlinks our --file-prefix=<...>map_* files so the next run can start.
  struct EalCleanupGuard {
    RdmaMgr* mgr;
    ~EalCleanupGuard() {
      if (!mgr->initialized_) { mgr->cleanup_eal(); }
    }
  } cleanup_guard{this};

  if (!check_hugepage_availability()) {
    DAQIRI_LOG_CRITICAL("Aborting before rte_eal_init() to keep /dev/hugepages clean");
    return;
  }

  /* Initialize DPDK params */
  constexpr int max_nargs = 32;
  constexpr int max_arg_size = 64;
  char** _argv;
  _argv = (char**)malloc(sizeof(char*) * max_nargs);
  if (_argv == nullptr) {
    DAQIRI_LOG_CRITICAL("Failed to allocate memory for DPDK arguments");
    return;
  }

  for (int i = 0; i < max_nargs; i++) {
    _argv[i] = (char*)malloc(max_arg_size);
    if (_argv[i] == nullptr) {
      DAQIRI_LOG_CRITICAL("Failed to allocate memory for DPDK argument {}", i);
      for (int j = 0; j < i; j++) {
        free(_argv[j]);
      }
      free(_argv);
      return;
    }
  }

  int arg = 0;
  std::string cores = std::to_string(cfg_.common_.master_core_) + ",";  // Master core must be first
  std::set<std::string> ifs;

  // Populate pd_map_ with all IB devices and create PDs
  int num_ib_devices;
  ibv_context** ib_devices = rdma_get_devices(&num_ib_devices);
  if (num_ib_devices == 0) {
    DAQIRI_LOG_CRITICAL("No RDMA-capable devices found!");
    return;
  }

  DAQIRI_LOG_INFO("Found {} RDMA-capable devices", num_ib_devices);

  for (int i = 0; i < num_ib_devices; i++) {
    struct ibv_context* device = ib_devices[i];
    DAQIRI_LOG_INFO("Creating PD for device {}", (void*)device);

    // Get device attributes to determine number of ports
    struct ibv_device_attr device_attr;
    if (ibv_query_device(device, &device_attr) != 0) {
      DAQIRI_LOG_ERROR("Failed to query device attributes for device {}", (void*)device);
      continue;
    }

    struct ibv_pd* pd = ibv_alloc_pd(device);
    if (pd == nullptr) {
      DAQIRI_LOG_ERROR("Failed to allocate PD for device {}", (void*)device);
      continue;
    }
    DAQIRI_LOG_INFO("Created PD {} for device {}", (void*)pd, (void*)device);
    pd_map_[device] = pd;
  }

  // Get GPU PCIe BDFs since they're needed to pass to DPDK
  int if_num = 0;
  for (auto& intf : cfg_.ifs_) {
    ifs.emplace(intf.address_);
    for (const auto& q : intf.rx_.queues_) { cores += q.common_.cpu_core_ + ","; }
    for (const auto& q : intf.tx_.queues_) { cores += q.common_.cpu_core_ + ","; }
    intf.port_id_ = if_num++;
  }

  cores = cores.substr(0, cores.size() - 1);

  strncpy(_argv[arg++], "daqiri_operator", max_arg_size - 1);
  eal_file_prefix_ = generate_random_string(10);
  strncpy(_argv[arg++],
          (std::string("--file-prefix=") + eal_file_prefix_).c_str(),
          max_arg_size - 1);
  strncpy(_argv[arg++], "-l", max_arg_size - 1);
  strncpy(_argv[arg++], cores.c_str(), max_arg_size - 1);

  if (cfg_.debug_) {
    DAQIRI_LOG_INFO(
        "Setting DPDK log level to: {}",
        DpdkLogLevel::to_description_string(DpdkLogLevel::from_ano_log_level(cfg_.log_level_)));

    DpdkLogLevelCommandBuilder cmd(cfg_.log_level_);
    for (auto& c : cmd.get_cmd_flags_strings()) {
      strncpy(_argv[arg++], c.c_str(), max_arg_size - 1);
    }
  }

  _argv[arg] = nullptr;
  std::string dpdk_args = "";
  for (int ac = 0; ac < arg; ac++) { dpdk_args += std::string(_argv[ac]) + " "; }

  DAQIRI_LOG_INFO("DPDK EAL arguments: {}", dpdk_args);

  ret = rte_eal_init(arg, _argv);
  if (ret < 0) {
    DAQIRI_LOG_CRITICAL("Invalid EAL arguments: {}", rte_errno);
    return;
  }
  eal_initialized_ = true;

  // Set up memory region sizes
  for (auto& mr : cfg_.mrs_) {
    const size_t rdma_alignment = std::max<size_t>(get_alignment(mr.second.kind_), GPU_PAGE_SIZE);
    mr.second.adj_size_ = RTE_ALIGN_CEIL(mr.second.buf_size_, rdma_alignment);
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

  main_thread_ = std::thread(&RdmaMgr::run, this);

  DAQIRI_LOG_INFO("RDMA manager initialized");
  initialized_ = true;
}

void RdmaMgr::shutdown() {
  // Idempotency guard: shutdown() runs explicitly from the caller AND again
  // from ~RdmaMgr / ~SocketMgr during C++ __cxa_finalize. By the second call
  // the spdlog default logger (a function-local static created lazily on the
  // first DAQIRI_LOG_INFO) has already been destroyed, so any logging here
  // crashes inside spdlog::sink_it_. Skip the whole body on subsequent calls.
  if (!initialized_) { return; }
  DAQIRI_LOG_INFO("RDMA manager shutting down");
  rdma_force_quit.store(true);

  DAQIRI_LOG_INFO("Waiting for main thread to complete");
  main_thread_.join();

  // Release DPDK resources BEFORE rte_eal_cleanup(). Pointers owned by EAL
  // memzones become invalid once cleanup runs, so the destructor must not
  // touch them after this point.
  while (!rx_rings_.empty()) {
    auto ring = rx_rings_.front();
    rx_rings_.pop();
    if (ring != nullptr) { rte_ring_free(ring); }
  }
  while (!tx_rings_.empty()) {
    auto ring = tx_rings_.front();
    tx_rings_.pop();
    if (ring != nullptr) { rte_ring_free(ring); }
  }
  rx_rings_map_.clear();
  tx_rings_map_.clear();

  if (pkt_len_pool_ != nullptr) { rte_mempool_free(pkt_len_pool_); pkt_len_pool_ = nullptr; }
  if (rx_meta != nullptr) { rte_mempool_free(rx_meta); rx_meta = nullptr; }
  if (tx_meta != nullptr) { rte_mempool_free(tx_meta); tx_meta = nullptr; }
  if (tx_burst_pool_ != nullptr) { rte_mempool_free(tx_burst_pool_); tx_burst_pool_ = nullptr; }
  for (auto& entry : mem_pools_) {
    if (entry.second != nullptr) { rte_ring_free(entry.second); }
  }
  mem_pools_.clear();

  cleanup_eal();
  initialized_ = false;
}

void RdmaMgr::init_client() {
  // TODO: Implement client initialization
  // return Status::SUCCESS;
}

Status RdmaMgr::get_rx_burst(BurstParams** burst, uintptr_t conn_id, bool server) {
  if (server) {
    const auto ring = rx_rings_map_[reinterpret_cast<struct rdma_cm_id*>(conn_id)];
    if (ring == nullptr) {
      DAQIRI_LOG_CRITICAL("No server RX ring found for conn_id {:#x}", conn_id);
      return Status::INVALID_PARAMETER;
    }

    if (rte_ring_dequeue(ring, reinterpret_cast<void**>(burst)) != 0) { return Status::NOT_READY; }

    return Status::SUCCESS;
  } else {
    const auto ring = rx_rings_map_[reinterpret_cast<struct rdma_cm_id*>(conn_id)];
    if (ring == nullptr) {
      DAQIRI_LOG_CRITICAL("No client RX ring found for conn_id {:#x}", conn_id);
      return Status::INVALID_PARAMETER;
    }

    if (rte_ring_dequeue(ring, reinterpret_cast<void**>(burst)) != 0) { return Status::NOT_READY; }

    return Status::SUCCESS;
  }
}

void RdmaMgr::free_rx_metadata(BurstParams* burst) {
  rte_mempool_put(rx_meta, burst);
}

void RdmaMgr::free_tx_metadata(BurstParams* burst) {
  rte_mempool_put(tx_meta, burst);
}

Status RdmaMgr::get_tx_metadata_buffer(BurstParams** burst) {
  // TODO: Implement get_tx_meta_buf
  return Status::SUCCESS;
}

void RdmaMgr::print_stats() {
  DAQIRI_LOG_INFO("daqiri RDMA manager stats");

  size_t total_server_queues = 0;
  size_t server_queues_with_client = 0;
  size_t active_server_queues = 0;

  for (const auto& server_entry : server_q_params_) {
    const auto& qparams = server_entry.second;
    total_server_queues += qparams.size();

    for (const auto& q : qparams) {
      if (q.client_id != nullptr) { server_queues_with_client++; }
      if (q.active) { active_server_queues++; }
    }
  }

  size_t active_client_queues = 0;
  for (const auto& client_entry : client_q_params_) {
    if (client_entry.second.active) { active_client_queues++; }
  }

  size_t tx_ring_entries = 0;
  size_t rx_ring_entries = 0;
  for (const auto& it : tx_rings_map_) {
    if (it.second != nullptr) { tx_ring_entries += rte_ring_count(it.second); }
  }
  for (const auto& it : rx_rings_map_) {
    if (it.second != nullptr) { rx_ring_entries += rte_ring_count(it.second); }
  }

  size_t worker_threads = 0;
  {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    worker_threads = worker_threads_.size();
  }

  DAQIRI_LOG_INFO("  initialized={} stop_requested={}",
                    initialized_,
                    rdma_force_quit.load() ? "true" : "false");
  DAQIRI_LOG_INFO("  listeners={} server_queues_total={} server_queues_with_client={} "
                  "server_queues_active={}",
                    server_q_params_.size(),
                    total_server_queues,
                    server_queues_with_client,
                    active_server_queues);
  DAQIRI_LOG_INFO("  client_connections={} active_client_connections={}",
                    client_q_params_.size(),
                    active_client_queues);
  DAQIRI_LOG_INFO("  worker_threads={} pds={} registered_mrs={} remote_mrs={} endpoints={}",
                    worker_threads,
                    pd_map_.size(),
                    mrs_.size(),
                    remote_mrs_.size(),
                    endpoints_.size());
  DAQIRI_LOG_INFO("  tx_rings={} tx_ring_pending={} rx_rings={} rx_ring_pending={}",
                    tx_rings_map_.size(),
                    tx_ring_entries,
                    rx_rings_map_.size(),
                    rx_ring_entries);

  if (tx_burst_pool_ != nullptr) {
    DAQIRI_LOG_INFO("  tx_burst_pool avail={} in_use={}",
                      rte_mempool_avail_count(tx_burst_pool_),
                      rte_mempool_in_use_count(tx_burst_pool_));
  }
  if (rx_meta != nullptr) {
    DAQIRI_LOG_INFO("  rx_meta_pool avail={} in_use={}",
                      rte_mempool_avail_count(rx_meta),
                      rte_mempool_in_use_count(rx_meta));
  }
  if (tx_meta != nullptr) {
    DAQIRI_LOG_INFO("  tx_meta_pool avail={} in_use={}",
                      rte_mempool_avail_count(tx_meta),
                      rte_mempool_in_use_count(tx_meta));
  }
  if (pkt_len_pool_ != nullptr) {
    DAQIRI_LOG_INFO("  pkt_len_pool avail={} in_use={}",
                      rte_mempool_avail_count(pkt_len_pool_),
                      rte_mempool_in_use_count(pkt_len_pool_));
  }
}

// Also need destructor implementation
RdmaMgr::~RdmaMgr() {
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
    rdma_destroy_event_channel(cm_event_channel_);
    cm_event_channel_ = nullptr;
  }

  // Deallocate PDs
  for (auto& entry : pd_map_) {
    if (entry.second != nullptr) {
      ibv_dealloc_pd(entry.second);
    }
  }
  pd_map_.clear();

  // shutdown() releases DPDK rings/mempools BEFORE rte_eal_cleanup(); the
  // block below covers only the partial-init / no-shutdown path, where EAL
  // is still alive and these pointers are still valid.
  if (eal_initialized_) {
    while (!rx_rings_.empty()) {
      auto ring = rx_rings_.front();
      rx_rings_.pop();
      rte_ring_free(ring);
    }
    while (!tx_rings_.empty()) {
      auto ring = tx_rings_.front();
      tx_rings_.pop();
      rte_ring_free(ring);
    }
    if (pkt_len_pool_ != nullptr) { rte_mempool_free(pkt_len_pool_); pkt_len_pool_ = nullptr; }
    if (rx_meta != nullptr) { rte_mempool_free(rx_meta); rx_meta = nullptr; }
    if (tx_meta != nullptr) { rte_mempool_free(tx_meta); tx_meta = nullptr; }
    if (tx_burst_pool_ != nullptr) { rte_mempool_free(tx_burst_pool_); tx_burst_pool_ = nullptr; }
    for (auto& entry : mem_pools_) {
      if (entry.second != nullptr) { rte_ring_free(entry.second); }
    }
  }
  rx_rings_map_.clear();
  tx_rings_map_.clear();
  mem_pools_.clear();

  // Final safety net: if shutdown() never ran (process exiting from a partial
  // init), still release EAL and unlink any leftover hugepage files.
  cleanup_eal();
}

// RDMA-specific functions that were declared but not implemented
Status RdmaMgr::register_mr(std::string name, int intf, void* addr, size_t len, int flags) {
  // TODO: Implement register_mr
  return Status::SUCCESS;
}
}  // namespace daqiri
