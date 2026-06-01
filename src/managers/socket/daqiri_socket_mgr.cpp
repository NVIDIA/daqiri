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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "daqiri_socket_mgr.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <vector>

#include <daqiri/logging.hpp>

#if DAQIRI_MGR_RDMA
#include "src/managers/rdma/daqiri_rdma_mgr.h"
#endif

namespace daqiri {

namespace {

constexpr size_t kMaxUdpPayloadBytes = 65507;

bool parse_ipv4_addr(const std::string& ip, uint16_t port, sockaddr_in* addr) {
  if (addr == nullptr) { return false; }

  std::memset(addr, 0, sizeof(sockaddr_in));
  addr->sin_family = AF_INET;
  addr->sin_port = htons(port);

  if (inet_pton(AF_INET, ip.c_str(), &addr->sin_addr) != 1) {
    DAQIRI_LOG_ERROR("Invalid IPv4 address '{}'", ip);
    return false;
  }

  return true;
}

std::string sockaddr_to_ip(const sockaddr_in& addr) {
  char ip_buf[INET_ADDRSTRLEN] = {0};
  if (inet_ntop(AF_INET, &addr.sin_addr, ip_buf, sizeof(ip_buf)) == nullptr) {
    return "";
  }
  return std::string(ip_buf);
}

}  // namespace

SocketMgr::~SocketMgr() {
  shutdown();
}

bool SocketMgr::is_roce_protocol() const {
  return cfg_.common_.stream_type == StreamType::SOCKET && cfg_.common_.protocol == SocketProtocol::ROCE;
}

Status SocketMgr::roce_not_initialized(const char* op_name) const {
  DAQIRI_LOG_ERROR("{} is only supported when protocol=roce and RoCE manager is initialized", op_name);
  return Status::NOT_SUPPORTED;
}

bool SocketMgr::set_config_and_initialize(const NetworkConfig& cfg) {
  cfg_ = cfg;

  for (size_t i = 0; i < cfg_.ifs_.size(); ++i) {
    cfg_.ifs_[i].port_id_ = static_cast<uint16_t>(i);
  }

  if (is_roce_protocol()) {
#if DAQIRI_MGR_RDMA
    roce_mgr_ = std::make_unique<RdmaMgr>();
    initialized_ = roce_mgr_->set_config_and_initialize(cfg_);
    return initialized_;
#else
    DAQIRI_LOG_ERROR("Socket backend built without RDMA support; protocol=roce is unavailable");
    initialized_ = false;
    return false;
#endif
  }

  initialize();
  return initialized_;
}

void SocketMgr::initialize() {
  if (is_roce_protocol()) {
#if DAQIRI_MGR_RDMA
    if (roce_mgr_ != nullptr && !roce_mgr_->is_initialized()) {
      roce_mgr_->initialize();
      initialized_ = roce_mgr_->is_initialized();
      return;
    }
#endif
    initialized_ = roce_mgr_ != nullptr;
    return;
  }

  running_.store(true);
  initialized_ = false;

  try {
    endpoints_.clear();
    default_connection_by_port_.clear();
    server_connections_.clear();
    endpoints_.reserve(cfg_.ifs_.size());

    for (size_t i = 0; i < cfg_.ifs_.size(); ++i) {
      const auto& if_cfg = cfg_.ifs_[i];
      auto ep = std::make_unique<EndpointState>();
      ep->if_index = static_cast<int>(i);
      ep->port = cfg_.ifs_[i].port_id_;
      ep->address = if_cfg.address_;
      ep->socket_cfg = if_cfg.socket_;
      ep->rx_queue = select_queue_id(if_cfg.rx_.queues_);
      ep->tx_queue = select_queue_id(if_cfg.tx_.queues_);
      ep->tx_batch_size = select_batch_size(if_cfg.tx_.queues_);
      ep->max_packet_size = static_cast<size_t>(std::max(1, select_max_packet_size(if_cfg)));
      ep->rx_queue_state = get_or_create_rx_queue(ep->port, ep->rx_queue);
      endpoints_.push_back(std::move(ep));
    }

    for (auto& ep : endpoints_) {
      if (cfg_.common_.protocol == SocketProtocol::TCP) {
        setup_tcp_endpoint(*ep);
      } else if (cfg_.common_.protocol == SocketProtocol::UDP) {
        setup_udp_endpoint(*ep);
      } else {
        DAQIRI_LOG_ERROR("Unsupported socket protocol '{}'", socket_protocol_to_string(cfg_.common_.protocol));
        shutdown();
        return;
      }
    }

    initialized_ = true;
    DAQIRI_LOG_INFO("Socket manager initialized for protocol '{}'", socket_protocol_to_string(cfg_.common_.protocol));
  } catch (const std::exception& e) {
    DAQIRI_LOG_ERROR("Socket manager initialization failed: {}", e.what());
    shutdown();
  }
}

void SocketMgr::run() {
  if (is_roce_protocol()) {
#if DAQIRI_MGR_RDMA
    if (roce_mgr_ != nullptr) { roce_mgr_->run(); }
#endif
    return;
  }
}

void* SocketMgr::get_packet_ptr(BurstParams* burst, int idx) {
  return get_segment_packet_ptr(burst, 0, idx);
}

uint32_t SocketMgr::get_packet_length(BurstParams* burst, int idx) {
  return get_segment_packet_length(burst, 0, idx);
}

void* SocketMgr::get_segment_packet_ptr(BurstParams* burst, int seg, int idx) {
  if (burst == nullptr || seg < 0 || seg >= MAX_NUM_SEGS || burst->pkts[seg] == nullptr || idx < 0 ||
      idx >= static_cast<int>(burst->hdr.hdr.num_pkts)) {
    return nullptr;
  }
  return burst->pkts[seg][idx];
}

uint32_t SocketMgr::get_segment_packet_length(BurstParams* burst, int seg, int idx) {
  if (burst == nullptr || seg < 0 || seg >= MAX_NUM_SEGS || burst->pkt_lens[seg] == nullptr || idx < 0 ||
      idx >= static_cast<int>(burst->hdr.hdr.num_pkts)) {
    return 0;
  }
  return burst->pkt_lens[seg][idx];
}

uint16_t SocketMgr::get_packet_flow_id(BurstParams* burst, int idx) {
  return 0;
}

Status SocketMgr::get_packet_rx_timestamp(BurstParams* burst, int idx, uint64_t* timestamp_ns) {
  (void)burst;
  (void)idx;
  (void)timestamp_ns;
  return Status::NOT_SUPPORTED;
}

void* SocketMgr::get_packet_extra_info(BurstParams* burst, int idx) {
  return nullptr;
}

Status SocketMgr::get_tx_packet_burst(BurstParams* burst) {
  if (is_roce_protocol()) {
#if DAQIRI_MGR_RDMA
    return roce_mgr_ != nullptr ? roce_mgr_->get_tx_packet_burst(burst) : roce_not_initialized("get_tx_packet_burst");
#else
    return roce_not_initialized("get_tx_packet_burst");
#endif
  }

  if (burst == nullptr) { return Status::INVALID_PARAMETER; }

  const auto* ep = endpoint_for_port(burst->hdr.hdr.port_id);
  const auto num_pkts = burst->hdr.hdr.num_pkts > 0 ? burst->hdr.hdr.num_pkts : (ep ? ep->tx_batch_size : 1);
  const auto pkt_size = ep ? ep->max_packet_size : static_cast<size_t>(65536);

  if (num_pkts == 0) {
    DAQIRI_LOG_ERROR("TX burst requested with zero packets");
    return Status::INVALID_PARAMETER;
  }

  burst->hdr.hdr.num_pkts = num_pkts;
  burst->hdr.hdr.num_segs = 1;

  burst->pkts[0] = new void*[num_pkts]();
  burst->pkt_lens[0] = new uint32_t[num_pkts]();

  for (size_t i = 0; i < num_pkts; ++i) {
    burst->pkts[0][i] = static_cast<void*>(new uint8_t[pkt_size]);
    burst->pkt_lens[0][i] = 0;
  }

  return Status::SUCCESS;
}

Status SocketMgr::set_eth_header(BurstParams* burst, int idx, char* dst_addr) {
  if (is_roce_protocol()) {
#if DAQIRI_MGR_RDMA
    return roce_mgr_ != nullptr ? roce_mgr_->set_eth_header(burst, idx, dst_addr)
                                : roce_not_initialized("set_eth_header");
#else
    return roce_not_initialized("set_eth_header");
#endif
  }

  DAQIRI_LOG_ERROR(
      "set_eth_header is not supported for stream_type=socket with protocol={}; "
      "kernel sockets own L2/L3/L4 headers",
      socket_protocol_to_string(cfg_.common_.protocol));
  return Status::NOT_SUPPORTED;
}

Status SocketMgr::set_ipv4_header(BurstParams* burst, int idx, int ip_len, uint8_t proto,
                                  unsigned int src_host, unsigned int dst_host) {
  if (is_roce_protocol()) {
#if DAQIRI_MGR_RDMA
    return roce_mgr_ != nullptr ? roce_mgr_->set_ipv4_header(burst, idx, ip_len, proto, src_host, dst_host)
                                : roce_not_initialized("set_ipv4_header");
#else
    return roce_not_initialized("set_ipv4_header");
#endif
  }

  DAQIRI_LOG_ERROR(
      "set_ipv4_header is not supported for stream_type=socket with protocol={}; "
      "kernel sockets own L2/L3/L4 headers",
      socket_protocol_to_string(cfg_.common_.protocol));
  return Status::NOT_SUPPORTED;
}

Status SocketMgr::set_udp_header(BurstParams* burst, int idx, int udp_len, uint16_t src_port,
                                 uint16_t dst_port) {
  if (is_roce_protocol()) {
#if DAQIRI_MGR_RDMA
    return roce_mgr_ != nullptr ? roce_mgr_->set_udp_header(burst, idx, udp_len, src_port, dst_port)
                                : roce_not_initialized("set_udp_header");
#else
    return roce_not_initialized("set_udp_header");
#endif
  }

  DAQIRI_LOG_ERROR(
      "set_udp_header is not supported for stream_type=socket with protocol={}; "
      "kernel sockets own L2/L3/L4 headers",
      socket_protocol_to_string(cfg_.common_.protocol));
  return Status::NOT_SUPPORTED;
}

Status SocketMgr::set_udp_payload(BurstParams* burst, int idx, void* data, int len) {
  if (burst == nullptr || burst->pkts[0] == nullptr || idx < 0 ||
      idx >= static_cast<int>(burst->hdr.hdr.num_pkts) || data == nullptr || len < 0) {
    return Status::INVALID_PARAMETER;
  }

  std::memcpy(burst->pkts[0][idx], data, static_cast<size_t>(len));
  return Status::SUCCESS;
}

bool SocketMgr::is_tx_burst_available(BurstParams* burst) {
  if (is_roce_protocol()) {
#if DAQIRI_MGR_RDMA
    return roce_mgr_ != nullptr && roce_mgr_->is_tx_burst_available(burst);
#else
    return false;
#endif
  }

  return true;
}

Status SocketMgr::set_packet_lengths(BurstParams* burst, int idx,
                                     const std::initializer_list<int>& lens) {
  if (is_roce_protocol()) {
#if DAQIRI_MGR_RDMA
    return roce_mgr_ != nullptr ? roce_mgr_->set_packet_lengths(burst, idx, lens)
                                : roce_not_initialized("set_packet_lengths");
#else
    return roce_not_initialized("set_packet_lengths");
#endif
  }

  if (burst == nullptr || burst->pkt_lens[0] == nullptr || idx < 0 ||
      idx >= static_cast<int>(burst->hdr.hdr.num_pkts) || lens.size() == 0) {
    return Status::INVALID_PARAMETER;
  }

  burst->pkt_lens[0][idx] = static_cast<uint32_t>(*(lens.begin()));
  return Status::SUCCESS;
}

void SocketMgr::free_all_segment_packets(BurstParams* burst, int seg) {
  if (burst == nullptr || seg < 0 || seg >= MAX_NUM_SEGS || burst->pkts[seg] == nullptr) { return; }
  for (size_t i = 0; i < burst->hdr.hdr.num_pkts; ++i) {
    auto* pkt = reinterpret_cast<uint8_t*>(burst->pkts[seg][i]);
    delete[] pkt;
    burst->pkts[seg][i] = nullptr;
  }
}

void SocketMgr::free_all_packets(BurstParams* burst) {
  if (burst == nullptr) { return; }
  const int num_segs = std::clamp(burst->hdr.hdr.num_segs, 0, MAX_NUM_SEGS);
  for (int seg = 0; seg < num_segs; ++seg) {
    free_all_segment_packets(burst, seg);
  }
}

void SocketMgr::free_packet_segment(BurstParams* burst, int seg, int pkt) {
  if (burst == nullptr || seg < 0 || seg >= MAX_NUM_SEGS || burst->pkts[seg] == nullptr || pkt < 0 ||
      pkt >= static_cast<int>(burst->hdr.hdr.num_pkts)) {
    return;
  }
  auto* data = reinterpret_cast<uint8_t*>(burst->pkts[seg][pkt]);
  delete[] data;
  burst->pkts[seg][pkt] = nullptr;
}

void SocketMgr::free_packet(BurstParams* burst, int pkt) {
  free_packet_segment(burst, 0, pkt);
}

void SocketMgr::free_packet_arrays(BurstParams* burst) {
  if (burst == nullptr) { return; }
  for (int seg = 0; seg < MAX_NUM_SEGS; ++seg) {
    delete[] burst->pkts[seg];
    burst->pkts[seg] = nullptr;
    delete[] burst->pkt_lens[seg];
    burst->pkt_lens[seg] = nullptr;
  }
}

void SocketMgr::free_rx_burst(BurstParams* burst) {
  if (is_roce_protocol()) {
#if DAQIRI_MGR_RDMA
    if (roce_mgr_ != nullptr) {
      roce_mgr_->free_rx_burst(burst);
      return;
    }
#endif
    return;
  }

  free_packet_arrays(burst);
  delete burst;
}

void SocketMgr::free_tx_burst(BurstParams* burst) {
  if (is_roce_protocol()) {
#if DAQIRI_MGR_RDMA
    if (roce_mgr_ != nullptr) {
      roce_mgr_->free_tx_burst(burst);
      return;
    }
#endif
    return;
  }

  free_packet_arrays(burst);
  delete burst;
}

Status SocketMgr::set_packet_tx_time(BurstParams* burst, int idx, uint64_t time) {
  return Status::NOT_SUPPORTED;
}

void SocketMgr::close_fd(int& fd) {
  if (fd >= 0) {
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
    fd = -1;
  }
}

void SocketMgr::close_all_connections() {
  std::vector<std::shared_ptr<ConnectionState>> conn_copy;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (auto& kv : connections_) {
      conn_copy.push_back(kv.second);
    }
    connections_.clear();
    default_connection_by_port_.clear();
    server_connections_.clear();
  }

  for (auto& conn : conn_copy) {
    if (conn == nullptr) { continue; }
    conn->running.store(false);
    close_fd(conn->fd);
  }

  for (auto& conn : conn_copy) {
    if (conn != nullptr && conn->rx_thread.joinable()) {
      conn->rx_thread.join();
    }
  }
}

void SocketMgr::clear_rx_queues() {
  for (auto& ep : endpoints_) {
    if (ep == nullptr || ep->rx_queue_state == nullptr) { continue; }
    std::lock_guard<std::mutex> lock(ep->rx_queue_state->mutex);
    while (!ep->rx_queue_state->bursts.empty()) {
      auto* burst = ep->rx_queue_state->bursts.front();
      ep->rx_queue_state->bursts.pop();
      if (burst != nullptr) {
        free_all_packets(burst);
        free_rx_burst(burst);
      }
    }
  }
}

void SocketMgr::shutdown() {
  // Idempotency guard: shutdown() may be invoked a second time via
  // ~SocketMgr during C++ __cxa_finalize, after the spdlog default logger
  // has been destroyed. Any DAQIRI_LOG_INFO from the cascade (here, the
  // RoCE branch, or the manager method this delegates to) would then crash
  // inside spdlog::sink_it_. Skip the whole body on subsequent calls.
  //
  // The guard checks BOTH flags because initialize() sets initialized_=false
  // and running_=true before running setup, then sets initialized_=true on
  // success. If setup throws, the catch block calls shutdown() with
  // initialized_=false and running_=true to clean up any threads/sockets
  // that were spawned partway. Guarding on initialized_ alone would skip
  // that cleanup. Both flags are only cleared together after a successful
  // shutdown() body, so the post-shutdown re-entry from __cxa_finalize is
  // the only case where the guard fires.
  if (!initialized_ && !running_.load()) { return; }
  if (is_roce_protocol()) {
#if DAQIRI_MGR_RDMA
    if (roce_mgr_ != nullptr) {
      roce_mgr_->shutdown();
      initialized_ = false;
    }
#endif
    return;
  }

  running_.store(false);

  for (auto& ep : endpoints_) {
    if (ep == nullptr) { continue; }
    ep->accept_running.store(false);
    close_fd(ep->listen_fd);
  }

  close_all_connections();

  for (auto& ep : endpoints_) {
    if (ep == nullptr) { continue; }
    if (ep->accept_thread.joinable()) { ep->accept_thread.join(); }
    if (ep->io_thread.joinable()) { ep->io_thread.join(); }
  }

  clear_rx_queues();
  endpoints_.clear();
  initialized_ = false;
}

void SocketMgr::print_stats() {
  if (is_roce_protocol()) {
#if DAQIRI_MGR_RDMA
    if (roce_mgr_ != nullptr) {
      roce_mgr_->print_stats();
      return;
    }
#endif
  }

  DAQIRI_LOG_INFO("Socket manager stats: tx_pkts={} tx_bytes={} rx_pkts={} rx_bytes={}",
                  tx_pkts_.load(),
                  tx_bytes_.load(),
                  rx_pkts_.load(),
                  rx_bytes_.load());
}

uint64_t SocketMgr::get_burst_tot_byte(BurstParams* burst) {
  if (burst == nullptr || burst->pkt_lens[0] == nullptr) { return 0; }
  uint64_t total = 0;
  for (size_t i = 0; i < burst->hdr.hdr.num_pkts; ++i) {
    total += burst->pkt_lens[0][i];
  }
  return total;
}

BurstParams* SocketMgr::create_tx_burst_params() {
  return new BurstParams{};
}

Status SocketMgr::pop_rx_burst(const std::shared_ptr<RxQueueState>& qstate, BurstParams** burst) {
  if (burst == nullptr || qstate == nullptr) { return Status::INVALID_PARAMETER; }

  std::lock_guard<std::mutex> lock(qstate->mutex);
  if (qstate->bursts.empty()) {
    *burst = nullptr;
    return Status::NULL_PTR;
  }

  *burst = qstate->bursts.front();
  qstate->bursts.pop();
  return Status::SUCCESS;
}

void SocketMgr::push_rx_burst(const std::shared_ptr<RxQueueState>& qstate, BurstParams* burst) {
  if (qstate == nullptr || burst == nullptr) { return; }
  std::lock_guard<std::mutex> lock(qstate->mutex);
  qstate->bursts.push(burst);
}

Status SocketMgr::get_rx_burst(BurstParams** burst, int port, int q) {
  if (is_roce_protocol()) {
#if DAQIRI_MGR_RDMA
    return roce_mgr_ != nullptr ? roce_mgr_->get_rx_burst(burst, port, q)
                                : roce_not_initialized("get_rx_burst");
#else
    return roce_not_initialized("get_rx_burst");
#endif
  }

  for (auto& ep : endpoints_) {
    if (ep != nullptr && ep->port == static_cast<uint16_t>(port) && ep->rx_queue == static_cast<uint16_t>(q)) {
      return pop_rx_burst(ep->rx_queue_state, burst);
    }
  }

  if (burst != nullptr) { *burst = nullptr; }
  return Status::INVALID_PARAMETER;
}

Status SocketMgr::get_rx_burst(BurstParams** burst, uintptr_t conn_id, bool server) {
  if (is_roce_protocol()) {
#if DAQIRI_MGR_RDMA
    return roce_mgr_ != nullptr ? roce_mgr_->get_rx_burst(burst, conn_id, server)
                                : roce_not_initialized("get_rx_burst");
#else
    return roce_not_initialized("get_rx_burst");
#endif
  }

  std::shared_ptr<ConnectionState> conn;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = connections_.find(conn_id);
    if (it == connections_.end()) {
      if (burst != nullptr) { *burst = nullptr; }
      return Status::INVALID_PARAMETER;
    }
    conn = it->second;
  }

  return pop_rx_burst(conn->rx_queue, burst);
}

void SocketMgr::free_rx_metadata(BurstParams* burst) {
  if (is_roce_protocol()) {
#if DAQIRI_MGR_RDMA
    if (roce_mgr_ != nullptr) {
      roce_mgr_->free_rx_metadata(burst);
      return;
    }
#endif
    return;
  }

  free_packet_arrays(burst);
  delete burst;
}

void SocketMgr::free_tx_metadata(BurstParams* burst) {
  if (is_roce_protocol()) {
#if DAQIRI_MGR_RDMA
    if (roce_mgr_ != nullptr) {
      roce_mgr_->free_tx_metadata(burst);
      return;
    }
#endif
    return;
  }

  free_packet_arrays(burst);
  delete burst;
}

Status SocketMgr::get_tx_metadata_buffer(BurstParams** burst) {
  if (is_roce_protocol()) {
#if DAQIRI_MGR_RDMA
    return roce_mgr_ != nullptr ? roce_mgr_->get_tx_metadata_buffer(burst)
                                : roce_not_initialized("get_tx_metadata_buffer");
#else
    return roce_not_initialized("get_tx_metadata_buffer");
#endif
  }

  if (burst == nullptr) { return Status::INVALID_PARAMETER; }
  *burst = create_tx_burst_params();
  return Status::SUCCESS;
}

SocketMgr::EndpointState* SocketMgr::endpoint_for_port(uint16_t port) {
  for (auto& ep : endpoints_) {
    if (ep != nullptr && ep->port == port) { return ep.get(); }
  }
  return nullptr;
}

const SocketMgr::EndpointState* SocketMgr::endpoint_for_port(uint16_t port) const {
  for (const auto& ep : endpoints_) {
    if (ep != nullptr && ep->port == port) { return ep.get(); }
  }
  return nullptr;
}

bool SocketMgr::send_tcp_burst(int fd, BurstParams* burst, size_t* sent_pkts,
                               uint64_t* sent_bytes) {
  if (sent_pkts != nullptr) { *sent_pkts = 0; }
  if (sent_bytes != nullptr) { *sent_bytes = 0; }
  if (burst == nullptr || burst->pkts[0] == nullptr || burst->pkt_lens[0] == nullptr) { return false; }

  const size_t num_pkts = burst->hdr.hdr.num_pkts;
  if (num_pkts == 0) { return true; }

  std::vector<iovec> iovs;
  iovs.reserve(num_pkts);
  size_t zero_len_pkts = 0;
  for (size_t i = 0; i < num_pkts; ++i) {
    const auto len = static_cast<size_t>(burst->pkt_lens[0][i]);
    if (len == 0) {
      ++zero_len_pkts;
      continue;
    }
    iovec iov{};
    iov.iov_base = burst->pkts[0][i];
    iov.iov_len = len;
    iovs.push_back(iov);
  }
  if (sent_pkts != nullptr) { *sent_pkts += zero_len_pkts; }
  if (iovs.empty()) { return true; }

  size_t iov_idx = 0;
  size_t iov_off = 0;
  constexpr size_t kMaxIovPerWrite = 1024;

  while (iov_idx < iovs.size()) {
    const size_t batch_n = std::min(kMaxIovPerWrite, iovs.size() - iov_idx);
    std::vector<iovec> batch(batch_n);
    for (size_t j = 0; j < batch_n; ++j) {
      batch[j] = iovs[iov_idx + j];
    }
    if (iov_off > 0) {
      auto* ptr = static_cast<uint8_t*>(batch[0].iov_base);
      batch[0].iov_base = ptr + iov_off;
      batch[0].iov_len -= iov_off;
    }

    const ssize_t sent = ::writev(fd, batch.data(), static_cast<int>(batch_n));
    if (sent < 0) {
      if (errno == EINTR) { continue; }
      DAQIRI_LOG_ERROR("TCP writev failed: {}", strerror(errno));
      return false;
    }
    if (sent == 0) {
      DAQIRI_LOG_ERROR("TCP writev returned zero without progress");
      return false;
    }

    size_t remaining = static_cast<size_t>(sent);
    if (sent_bytes != nullptr) { *sent_bytes += remaining; }

    while (remaining > 0 && iov_idx < iovs.size()) {
      const size_t cur_remaining = iovs[iov_idx].iov_len - iov_off;
      if (remaining < cur_remaining) {
        iov_off += remaining;
        remaining = 0;
      } else {
        remaining -= cur_remaining;
        iov_off = 0;
        ++iov_idx;
        if (sent_pkts != nullptr) { ++(*sent_pkts); }
      }
    }
  }

  return true;
}

bool SocketMgr::send_udp_burst(EndpointState& ep, BurstParams* burst, size_t* sent_pkts,
                               uint64_t* sent_bytes) {
  if (sent_pkts != nullptr) { *sent_pkts = 0; }
  if (sent_bytes != nullptr) { *sent_bytes = 0; }
  if (burst == nullptr || burst->pkts[0] == nullptr || burst->pkt_lens[0] == nullptr) { return false; }

  const size_t num_pkts = burst->hdr.hdr.num_pkts;
  if (num_pkts == 0) { return true; }

  bool use_sendto = false;
  sockaddr_in peer{};
  if (ep.socket_cfg.mode_ == SocketMode::SERVER) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!ep.udp_peer_valid) {
      DAQIRI_LOG_ERROR("UDP server has no learned peer yet; cannot transmit");
      return false;
    }
    peer = ep.udp_peer_addr;
    use_sendto = true;
  }

  for (size_t i = 0; i < num_pkts; ++i) {
    const auto len = static_cast<size_t>(burst->pkt_lens[0][i]);
    if (len > kMaxUdpPayloadBytes) {
      DAQIRI_LOG_ERROR("UDP payload length {} exceeds maximum {} bytes", len, kMaxUdpPayloadBytes);
      return false;
    }
  }

  std::vector<mmsghdr> msgs(num_pkts);
  std::vector<iovec> iovs(num_pkts);
  std::vector<sockaddr_in> peers;
  if (use_sendto) { peers.assign(num_pkts, peer); }

  for (size_t i = 0; i < num_pkts; ++i) {
    iovs[i].iov_base = burst->pkts[0][i];
    iovs[i].iov_len = static_cast<size_t>(burst->pkt_lens[0][i]);

    std::memset(&msgs[i], 0, sizeof(mmsghdr));
    msgs[i].msg_hdr.msg_iov = &iovs[i];
    msgs[i].msg_hdr.msg_iovlen = 1;
    if (use_sendto) {
      msgs[i].msg_hdr.msg_name = &peers[i];
      msgs[i].msg_hdr.msg_namelen = sizeof(sockaddr_in);
    }
  }

  size_t offset = 0;
  while (offset < num_pkts) {
    auto* batch = msgs.data() + offset;
    const auto remaining = static_cast<unsigned int>(num_pkts - offset);
    const int sent = ::sendmmsg(ep.udp_fd, batch, remaining, 0);
    if (sent < 0) {
      if (errno == EINTR) { continue; }
      DAQIRI_LOG_ERROR("UDP sendmmsg failed: {}", strerror(errno));
      return false;
    }
    if (sent == 0) {
      DAQIRI_LOG_ERROR("UDP sendmmsg returned zero without progress");
      return false;
    }

    for (int i = 0; i < sent; ++i) {
      if (sent_pkts != nullptr) { ++(*sent_pkts); }
      if (sent_bytes != nullptr) { *sent_bytes += static_cast<uint64_t>(batch[i].msg_len); }
    }

    offset += static_cast<size_t>(sent);
  }

  return true;
}

Status SocketMgr::send_tx_burst(BurstParams* burst) {
  if (is_roce_protocol()) {
#if DAQIRI_MGR_RDMA
    return roce_mgr_ != nullptr ? roce_mgr_->send_tx_burst(burst) : roce_not_initialized("send_tx_burst");
#else
    return roce_not_initialized("send_tx_burst");
#endif
  }

  if (burst == nullptr || burst->pkts[0] == nullptr || burst->pkt_lens[0] == nullptr) {
    return Status::INVALID_PARAMETER;
  }

  EndpointState* ep = endpoint_for_port(burst->hdr.hdr.port_id);
  if (ep == nullptr) {
    DAQIRI_LOG_ERROR("No socket endpoint configured for port {}", burst->hdr.hdr.port_id);
    free_all_packets(burst);
    free_tx_burst(burst);
    return Status::INVALID_PARAMETER;
  }

  std::shared_ptr<ConnectionState> conn;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);

    auto requested_id = get_connection_id(burst);
    if (requested_id != 0) {
      auto it = connections_.find(requested_id);
      if (it != connections_.end()) { conn = it->second; }
    }

    if (conn == nullptr) {
      auto dit = default_connection_by_port_.find(ep->port);
      if (dit != default_connection_by_port_.end()) {
        auto cit = connections_.find(dit->second);
        if (cit != connections_.end()) { conn = cit->second; }
      }
    }
  }

  Status status = Status::SUCCESS;

  if (cfg_.common_.protocol == SocketProtocol::UDP) {
    size_t sent_pkts = 0;
    uint64_t sent_bytes = 0;
    if (!send_udp_burst(*ep, burst, &sent_pkts, &sent_bytes)) {
      status = Status::CONNECT_FAILURE;
    }
    if (sent_pkts > 0) { tx_pkts_.fetch_add(sent_pkts); }
    if (sent_bytes > 0) { tx_bytes_.fetch_add(sent_bytes); }
  } else if (cfg_.common_.protocol == SocketProtocol::TCP) {
    if (conn == nullptr || !conn->running.load()) {
      DAQIRI_LOG_ERROR("No active TCP connection for port {}", ep->port);
      status = Status::CONNECT_FAILURE;
    } else {
      size_t sent_pkts = 0;
      uint64_t sent_bytes = 0;
      if (!send_tcp_burst(conn->fd, burst, &sent_pkts, &sent_bytes)) {
        status = Status::CONNECT_FAILURE;
      }
      if (sent_pkts > 0) { tx_pkts_.fetch_add(sent_pkts); }
      if (sent_bytes > 0) { tx_bytes_.fetch_add(sent_bytes); }
    }
  }

  free_all_packets(burst);
  free_tx_burst(burst);
  return status;
}

Status SocketMgr::get_mac_addr(int port, char* mac) {
  if (mac == nullptr) { return Status::INVALID_PARAMETER; }
  return Status::NOT_SUPPORTED;
}

bool SocketMgr::validate_config() const {
  if (is_roce_protocol()) {
#if DAQIRI_MGR_RDMA
    return roce_mgr_ != nullptr ? roce_mgr_->validate_config() : false;
#else
    return false;
#endif
  }

  if (cfg_.common_.stream_type != StreamType::SOCKET) {
    DAQIRI_LOG_ERROR("Socket manager requires stream_type=socket");
    return false;
  }

  if (cfg_.common_.protocol != SocketProtocol::TCP && cfg_.common_.protocol != SocketProtocol::UDP) {
    DAQIRI_LOG_ERROR("Socket manager supports tcp/udp directly, or roce via delegation");
    return false;
  }

  return true;
}

std::string SocketMgr::endpoint_key(const std::string& ip, uint16_t port) const {
  return ip + ":" + std::to_string(port);
}

uintptr_t SocketMgr::next_conn_id() {
  return next_conn_id_.fetch_add(1);
}

std::shared_ptr<SocketMgr::RxQueueState> SocketMgr::get_or_create_rx_queue(uint16_t port,
                                                                            uint16_t queue) {
  for (auto& ep : endpoints_) {
    if (ep != nullptr && ep->port == port && ep->rx_queue == queue && ep->rx_queue_state != nullptr) {
      return ep->rx_queue_state;
    }
  }

  auto qstate = std::make_shared<RxQueueState>();
  qstate->port = port;
  qstate->queue = queue;
  return qstate;
}

int SocketMgr::select_max_packet_size(const InterfaceConfig& if_cfg) const {
  int max_size = static_cast<int>(if_cfg.socket_.max_payload_size_);

  auto apply_queue_mr = [&](const auto& queues) {
    for (const auto& queue : queues) {
      for (const auto& mr_name : queue.common_.mrs_) {
        auto mr_it = cfg_.mrs_.find(mr_name);
        if (mr_it == cfg_.mrs_.end()) { continue; }
        max_size = std::max(max_size, static_cast<int>(mr_it->second.buf_size_));
      }
    }
  };

  apply_queue_mr(if_cfg.rx_.queues_);
  apply_queue_mr(if_cfg.tx_.queues_);

  if (max_size <= 0) { max_size = 65536; }
  return max_size;
}

uint16_t SocketMgr::select_queue_id(const std::vector<RxQueueConfig>& queues) const {
  if (queues.empty()) { return 0; }
  return static_cast<uint16_t>(queues.front().common_.id_);
}

uint16_t SocketMgr::select_queue_id(const std::vector<TxQueueConfig>& queues) const {
  if (queues.empty()) { return 0; }
  return static_cast<uint16_t>(queues.front().common_.id_);
}

uint32_t SocketMgr::select_batch_size(const std::vector<TxQueueConfig>& queues) const {
  if (queues.empty()) { return 1; }
  return static_cast<uint32_t>(std::max(1, queues.front().common_.batch_size_));
}

std::shared_ptr<SocketMgr::ConnectionState> SocketMgr::register_connection(
    int fd, uint16_t port, uint16_t queue, int if_index, bool server_side, bool is_udp,
    const std::shared_ptr<RxQueueState>& rx_queue, bool start_rx_thread) {
  auto conn = std::make_shared<ConnectionState>();
  conn->conn_id = next_conn_id();
  conn->fd = fd;
  conn->port = port;
  conn->queue = queue;
  conn->if_index = if_index;
  conn->server_side = server_side;
  conn->is_udp = is_udp;
  conn->rx_queue = rx_queue;
  conn->running.store(start_rx_thread);

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    connections_[conn->conn_id] = conn;
  }

  if (start_rx_thread) {
    conn->rx_thread = std::thread(&SocketMgr::tcp_rx_loop, this, conn);
  }

  return conn;
}

std::shared_ptr<SocketMgr::ConnectionState> SocketMgr::create_tcp_client_connection(
    EndpointState& ep, const std::string& dst_addr, uint16_t dst_port, const std::string& src_addr,
    uint16_t src_port, bool set_as_primary) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    DAQIRI_LOG_ERROR("Failed to create TCP socket: {}", strerror(errno));
    return nullptr;
  }

  int yes = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

  if (!src_addr.empty() || src_port != 0) {
    const std::string bind_ip = src_addr.empty() ? std::string("0.0.0.0") : src_addr;
    sockaddr_in bind_addr{};
    if (!parse_ipv4_addr(bind_ip, src_port, &bind_addr)) {
      close_fd(fd);
      return nullptr;
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
      DAQIRI_LOG_ERROR("TCP client bind failed for {}:{}: {}", bind_ip, src_port, strerror(errno));
      close_fd(fd);
      return nullptr;
    }
  }

  sockaddr_in dst{};
  if (!parse_ipv4_addr(dst_addr, dst_port, &dst)) {
    close_fd(fd);
    return nullptr;
  }

  const int retry_s = std::max(1, ep.socket_cfg.retry_connect_s_);
  while (running_.load()) {
    if (::connect(fd, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) == 0) {
      auto conn = register_connection(
          fd, ep.port, ep.rx_queue, ep.if_index, false, false, ep.rx_queue_state, true);
      if (set_as_primary) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        default_connection_by_port_[ep.port] = conn->conn_id;
        ep.primary_conn_id = conn->conn_id;
      }
      DAQIRI_LOG_INFO("Connected TCP client {}:{} on port {}", dst_addr, dst_port, ep.port);
      return conn;
    }

    DAQIRI_LOG_WARN("TCP connect to {}:{} failed: {}. Retrying in {}s",
                    dst_addr,
                    dst_port,
                    strerror(errno),
                    retry_s);
    std::this_thread::sleep_for(std::chrono::seconds(retry_s));
  }

  close_fd(fd);
  return nullptr;
}

void SocketMgr::setup_tcp_endpoint(EndpointState& ep) {
  if (ep.socket_cfg.mode_ == SocketMode::CLIENT) {
    const auto src_ip = ep.socket_cfg.local_ip_;
    const auto src_port = ep.socket_cfg.local_port_;
    auto conn = create_tcp_client_connection(ep,
                                             ep.socket_cfg.remote_ip_,
                                             ep.socket_cfg.remote_port_,
                                             src_ip,
                                             src_port,
                                             true);
    if (conn == nullptr) {
      throw std::runtime_error("failed to establish configured TCP client connection");
    }
    return;
  }

  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    throw std::runtime_error("failed to create TCP listen socket");
  }

  int yes = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  sockaddr_in bind_addr{};
  if (!parse_ipv4_addr(ep.socket_cfg.local_ip_, ep.socket_cfg.local_port_, &bind_addr)) {
    close_fd(fd);
    throw std::runtime_error("invalid TCP server bind address");
  }

  if (::bind(fd, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
    const auto err = std::string(strerror(errno));
    close_fd(fd);
    throw std::runtime_error("failed to bind TCP listen socket: " + err);
  }

  if (::listen(fd, 128) != 0) {
    const auto err = std::string(strerror(errno));
    close_fd(fd);
    throw std::runtime_error("failed to listen on TCP socket: " + err);
  }

  ep.listen_fd = fd;
  ep.accept_running.store(true);
  ep.accept_thread = std::thread(&SocketMgr::tcp_accept_loop, this, ep.if_index);

  DAQIRI_LOG_INFO("TCP server listening on {}:{} (port={})",
                  ep.socket_cfg.local_ip_,
                  ep.socket_cfg.local_port_,
                  ep.port);
}

void SocketMgr::setup_udp_endpoint(EndpointState& ep) {
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) { throw std::runtime_error("failed to create UDP socket"); }

  int yes = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  if (ep.socket_cfg.mode_ == SocketMode::SERVER) {
    sockaddr_in bind_addr{};
    if (!parse_ipv4_addr(ep.socket_cfg.local_ip_, ep.socket_cfg.local_port_, &bind_addr)) {
      close_fd(fd);
      throw std::runtime_error("invalid UDP server bind address");
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
      const auto err = std::string(strerror(errno));
      close_fd(fd);
      throw std::runtime_error("failed to bind UDP server socket: " + err);
    }
  } else {
    if (!ep.socket_cfg.local_ip_.empty() || ep.socket_cfg.local_port_ != 0) {
      const std::string bind_ip = ep.socket_cfg.local_ip_.empty() ? std::string("0.0.0.0") : ep.socket_cfg.local_ip_;
      sockaddr_in bind_addr{};
      if (!parse_ipv4_addr(bind_ip, ep.socket_cfg.local_port_, &bind_addr)) {
        close_fd(fd);
        throw std::runtime_error("invalid UDP client bind address");
      }
      if (::bind(fd, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
        const auto err = std::string(strerror(errno));
        close_fd(fd);
        throw std::runtime_error("failed to bind UDP client socket: " + err);
      }
    }

    sockaddr_in dst_addr{};
    if (!parse_ipv4_addr(ep.socket_cfg.remote_ip_, ep.socket_cfg.remote_port_, &dst_addr)) {
      close_fd(fd);
      throw std::runtime_error("invalid UDP client destination address");
    }

    if (::connect(fd, reinterpret_cast<sockaddr*>(&dst_addr), sizeof(dst_addr)) != 0) {
      const auto err = std::string(strerror(errno));
      close_fd(fd);
      throw std::runtime_error("failed to connect UDP client socket: " + err);
    }
  }

  ep.udp_fd = fd;

  auto conn = register_connection(
      fd, ep.port, ep.rx_queue, ep.if_index, ep.socket_cfg.mode_ == SocketMode::SERVER, true,
      ep.rx_queue_state, false);
  ep.primary_conn_id = conn->conn_id;

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    default_connection_by_port_[ep.port] = conn->conn_id;
    if (ep.socket_cfg.mode_ == SocketMode::SERVER) {
      server_connections_[endpoint_key(ep.address, ep.socket_cfg.local_port_)].push_back(conn->conn_id);
      server_connections_[endpoint_key(ep.socket_cfg.local_ip_, ep.socket_cfg.local_port_)].push_back(conn->conn_id);
    }
  }

  ep.io_thread = std::thread(&SocketMgr::udp_rx_loop, this, ep.if_index);

  DAQIRI_LOG_INFO("UDP {} on {}:{} (port={})",
                  ep.socket_cfg.mode_ == SocketMode::SERVER ? "server" : "client",
                  ep.socket_cfg.mode_ == SocketMode::SERVER ? ep.socket_cfg.local_ip_ : ep.socket_cfg.remote_ip_,
                  ep.socket_cfg.mode_ == SocketMode::SERVER ? ep.socket_cfg.local_port_ : ep.socket_cfg.remote_port_,
                  ep.port);
}

void SocketMgr::tcp_accept_loop(int if_index) {
  if (if_index < 0 || if_index >= static_cast<int>(endpoints_.size())) { return; }
  auto* ep = endpoints_[if_index].get();
  if (ep == nullptr) { return; }

  while (running_.load() && ep->accept_running.load()) {
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    int fd = ::accept(ep->listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
    if (fd < 0) {
      if (!running_.load() || !ep->accept_running.load()) { break; }
      if (errno == EINTR) { continue; }
      DAQIRI_LOG_WARN("TCP accept failed on {}:{}: {}",
                      ep->socket_cfg.local_ip_,
                      ep->socket_cfg.local_port_,
                      strerror(errno));
      continue;
    }

    int yes = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

    auto conn = register_connection(
        fd, ep->port, ep->rx_queue, ep->if_index, true, false, ep->rx_queue_state, true);

    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (ep->primary_conn_id == 0) {
        ep->primary_conn_id = conn->conn_id;
        default_connection_by_port_[ep->port] = conn->conn_id;
      }
      server_connections_[endpoint_key(ep->address, ep->socket_cfg.local_port_)].push_back(conn->conn_id);
      server_connections_[endpoint_key(ep->socket_cfg.local_ip_, ep->socket_cfg.local_port_)].push_back(conn->conn_id);
    }

    DAQIRI_LOG_INFO("Accepted TCP connection from {}:{} mapped to port={} queue={} conn_id={}",
                    sockaddr_to_ip(client_addr),
                    ntohs(client_addr.sin_port),
                    ep->port,
                    ep->rx_queue,
                    conn->conn_id);
  }
}

void SocketMgr::tcp_rx_loop(std::shared_ptr<ConnectionState> conn) {
  if (conn == nullptr) { return; }

  size_t max_size = 65536;
  if (const auto* ep = endpoint_for_port(conn->port)) { max_size = ep->max_packet_size; }

  std::vector<uint8_t> tmp(max_size);

  while (running_.load() && conn->running.load()) {
    const ssize_t rx = ::recv(conn->fd, tmp.data(), tmp.size(), 0);
    if (rx == 0) {
      break;
    }
    if (rx < 0) {
      if (errno == EINTR) { continue; }
      if (!running_.load()) { break; }
      DAQIRI_LOG_WARN("TCP recv failed on conn_id={}: {}", conn->conn_id, strerror(errno));
      break;
    }

    // BNO-style behavior: each recv() chunk is surfaced as one DAQIRI packet.
    auto* burst = create_tx_burst_params();
    burst->hdr.hdr.port_id = conn->port;
    burst->hdr.hdr.q_id = conn->queue;
    burst->hdr.hdr.num_pkts = 1;
    burst->hdr.hdr.num_segs = 1;
    burst->pkts[0] = new void*[1];
    burst->pkt_lens[0] = new uint32_t[1];
    auto* payload = new uint8_t[static_cast<size_t>(rx)];
    std::memcpy(payload, tmp.data(), static_cast<size_t>(rx));
    burst->pkts[0][0] = payload;
    burst->pkt_lens[0][0] = static_cast<uint32_t>(rx);
    set_connection_id(burst, conn->conn_id);

    push_rx_burst(conn->rx_queue, burst);
    rx_pkts_.fetch_add(1);
    rx_bytes_.fetch_add(static_cast<uint64_t>(rx));
  }

  conn->running.store(false);
  close_fd(conn->fd);
}

void SocketMgr::udp_rx_loop(int if_index) {
  if (if_index < 0 || if_index >= static_cast<int>(endpoints_.size())) { return; }
  auto* ep = endpoints_[if_index].get();
  if (ep == nullptr) { return; }

  if (ep->udp_fd < 0) { return; }

  constexpr size_t kUdpRxBatch = 32;
  std::vector<uint8_t> rx_storage(ep->max_packet_size * kUdpRxBatch);
  std::vector<mmsghdr> msgs(kUdpRxBatch);
  std::vector<iovec> iovs(kUdpRxBatch);
  std::vector<sockaddr_in> peers(kUdpRxBatch);

  for (size_t i = 0; i < kUdpRxBatch; ++i) {
    iovs[i].iov_base = rx_storage.data() + (i * ep->max_packet_size);
    iovs[i].iov_len = ep->max_packet_size;
    std::memset(&msgs[i], 0, sizeof(mmsghdr));
    msgs[i].msg_hdr.msg_iov = &iovs[i];
    msgs[i].msg_hdr.msg_iovlen = 1;
    msgs[i].msg_hdr.msg_name = &peers[i];
    msgs[i].msg_hdr.msg_namelen = sizeof(sockaddr_in);
  }

  while (running_.load()) {
    for (size_t i = 0; i < kUdpRxBatch; ++i) {
      msgs[i].msg_hdr.msg_namelen = sizeof(sockaddr_in);
    }

    const int received = ::recvmmsg(ep->udp_fd, msgs.data(), static_cast<unsigned int>(kUdpRxBatch), 0, nullptr);
    if (received < 0) {
      if (errno == EINTR) { continue; }
      if (!running_.load()) { break; }
      DAQIRI_LOG_WARN("UDP recvmmsg failed on port {}: {}", ep->port, strerror(errno));
      continue;
    }
    if (received == 0) { continue; }

    if (ep->socket_cfg.mode_ == SocketMode::SERVER) {
      std::lock_guard<std::mutex> lock(state_mutex_);
      ep->udp_peer_addr = peers[static_cast<size_t>(received - 1)];
      ep->udp_peer_valid = true;
    }

    for (int i = 0; i < received; ++i) {
      const auto rx = static_cast<size_t>(msgs[static_cast<size_t>(i)].msg_len);
      auto* burst = create_tx_burst_params();
      burst->hdr.hdr.port_id = ep->port;
      burst->hdr.hdr.q_id = ep->rx_queue;
      burst->hdr.hdr.num_pkts = 1;
      burst->hdr.hdr.num_segs = 1;
      burst->pkts[0] = new void*[1];
      burst->pkt_lens[0] = new uint32_t[1];

      auto* payload = new uint8_t[rx];
      std::memcpy(payload, iovs[static_cast<size_t>(i)].iov_base, rx);
      burst->pkts[0][0] = payload;
      burst->pkt_lens[0][0] = static_cast<uint32_t>(rx);
      set_connection_id(burst, ep->primary_conn_id);

      push_rx_burst(ep->rx_queue_state, burst);
      rx_pkts_.fetch_add(1);
      rx_bytes_.fetch_add(static_cast<uint64_t>(rx));
    }
  }
}

Status SocketMgr::socket_connect_to_server(const std::string& dst_addr, uint16_t dst_port,
                                           uintptr_t* conn_id) {
  return socket_connect_to_server(dst_addr, dst_port, "", conn_id);
}

Status SocketMgr::socket_connect_to_server(const std::string& dst_addr, uint16_t dst_port,
                                           const std::string& src_addr, uintptr_t* conn_id) {
  if (is_roce_protocol()) {
    return rdma_connect_to_server(dst_addr, dst_port, src_addr, conn_id);
  }

  if (conn_id == nullptr) { return Status::INVALID_PARAMETER; }

  for (auto& ep : endpoints_) {
    if (ep == nullptr || ep->socket_cfg.mode_ != SocketMode::CLIENT) { continue; }

    if (cfg_.common_.protocol == SocketProtocol::TCP) {
      if (ep->primary_conn_id != 0 && ep->socket_cfg.remote_ip_ == dst_addr &&
          ep->socket_cfg.remote_port_ == dst_port &&
          (src_addr.empty() || src_addr == ep->socket_cfg.local_ip_)) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        const auto it = connections_.find(ep->primary_conn_id);
        if (it != connections_.end() && it->second != nullptr && it->second->running.load()) {
          *conn_id = ep->primary_conn_id;
          return Status::SUCCESS;
        }
      }

      auto conn = create_tcp_client_connection(*ep, dst_addr, dst_port, src_addr, 0, true);
      if (conn == nullptr) { return Status::CONNECT_FAILURE; }
      *conn_id = conn->conn_id;
      return Status::SUCCESS;
    }

    if (cfg_.common_.protocol == SocketProtocol::UDP) {
      sockaddr_in dst{};
      if (!parse_ipv4_addr(dst_addr, dst_port, &dst)) { return Status::INVALID_PARAMETER; }
      if (::connect(ep->udp_fd, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) != 0) {
        DAQIRI_LOG_ERROR("UDP connect failed for {}:{}: {}", dst_addr, dst_port, strerror(errno));
        return Status::CONNECT_FAILURE;
      }
      ep->socket_cfg.remote_ip_ = dst_addr;
      ep->socket_cfg.remote_port_ = dst_port;
      *conn_id = ep->primary_conn_id;
      return Status::SUCCESS;
    }
  }

  return Status::INVALID_PARAMETER;
}

Status SocketMgr::socket_get_port_queue(uintptr_t conn_id, uint16_t* port, uint16_t* queue) {
  if (is_roce_protocol()) {
    return rdma_get_port_queue(conn_id, port, queue);
  }

  if (port == nullptr || queue == nullptr) { return Status::INVALID_PARAMETER; }

  std::lock_guard<std::mutex> lock(state_mutex_);
  auto it = connections_.find(conn_id);
  if (it == connections_.end()) { return Status::INVALID_PARAMETER; }

  *port = it->second->port;
  *queue = it->second->queue;
  return Status::SUCCESS;
}

Status SocketMgr::socket_get_server_conn_id(const std::string& server_addr, uint16_t server_port,
                                            uintptr_t* conn_id) {
  if (is_roce_protocol()) {
    return rdma_get_server_conn_id(server_addr, server_port, conn_id);
  }

  if (conn_id == nullptr) { return Status::INVALID_PARAMETER; }

  std::lock_guard<std::mutex> lock(state_mutex_);
  auto it = server_connections_.find(endpoint_key(server_addr, server_port));
  if (it == server_connections_.end() || it->second.empty()) { return Status::NULL_PTR; }

  for (auto maybe_conn : it->second) {
    if (connections_.find(maybe_conn) != connections_.end()) {
      *conn_id = maybe_conn;
      return Status::SUCCESS;
    }
  }

  return Status::NULL_PTR;
}

Status SocketMgr::rdma_connect_to_server(const std::string& dst_addr, uint16_t dst_port,
                                         uintptr_t* conn_id) {
  if (!is_roce_protocol() || roce_mgr_ == nullptr) { return roce_not_initialized("rdma_connect_to_server"); }
  return roce_mgr_->rdma_connect_to_server(dst_addr, dst_port, conn_id);
}

Status SocketMgr::rdma_connect_to_server(const std::string& dst_addr, uint16_t dst_port,
                                         const std::string& src_addr, uintptr_t* conn_id) {
  if (!is_roce_protocol() || roce_mgr_ == nullptr) { return roce_not_initialized("rdma_connect_to_server"); }
  return roce_mgr_->rdma_connect_to_server(dst_addr, dst_port, src_addr, conn_id);
}

Status SocketMgr::rdma_get_port_queue(uintptr_t conn_id, uint16_t* port, uint16_t* queue) {
  if (!is_roce_protocol() || roce_mgr_ == nullptr) { return roce_not_initialized("rdma_get_port_queue"); }
  return roce_mgr_->rdma_get_port_queue(conn_id, port, queue);
}

Status SocketMgr::rdma_get_server_conn_id(const std::string& server_addr, uint16_t server_port,
                                          uintptr_t* conn_id) {
  if (!is_roce_protocol() || roce_mgr_ == nullptr) { return roce_not_initialized("rdma_get_server_conn_id"); }
  return roce_mgr_->rdma_get_server_conn_id(server_addr, server_port, conn_id);
}

Status SocketMgr::rdma_set_header(BurstParams* burst, RDMAOpCode op_code, uintptr_t conn_id,
                                  bool is_server, int num_pkts, uint64_t wr_id,
                                  const std::string& local_mr_name) {
  if (!is_roce_protocol() || roce_mgr_ == nullptr) { return roce_not_initialized("rdma_set_header"); }
  return roce_mgr_->rdma_set_header(
      burst, op_code, conn_id, is_server, num_pkts, wr_id, local_mr_name);
}

RDMAOpCode SocketMgr::rdma_get_opcode(BurstParams* burst) {
  if (!is_roce_protocol() || roce_mgr_ == nullptr) {
    DAQIRI_LOG_ERROR("rdma_get_opcode is only valid with protocol=roce");
    return RDMAOpCode::INVALID;
  }
  return roce_mgr_->rdma_get_opcode(burst);
}

}  // namespace daqiri
