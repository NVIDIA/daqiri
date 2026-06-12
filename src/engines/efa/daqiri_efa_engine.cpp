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

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <cuda_runtime.h>
#include <rte_common.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>

#include "daqiri_efa_engine.h"
#include "src/dpdk_log.h"

// EFA SRD on the plain `efa` fabric performs its own segmentation/reassembly,
// so a multi-megabyte message is a single fi_send. We still target libfabric
// 1.18 as the minimum API level for solid CUDA FI_HMEM support.
#ifndef DAQIRI_EFA_FI_VERSION
#define DAQIRI_EFA_FI_VERSION FI_VERSION(1, 18)
#endif

namespace daqiri {

namespace {

void reset_efa_burst_metadata(BurstParams* burst) {
  if (burst == nullptr) {
    return;
  }
  burst->transport_hdr = BurstTransportHeader{};
  burst->pkts.fill(nullptr);
  burst->pkt_lens.fill(nullptr);
  burst->pkt_extra_info = nullptr;
  burst->event = nullptr;
}

// Blocking write/read of an exact byte count over the bootstrap TCP socket.
bool write_all(int fd, const void* buf, size_t len) {
  const auto* p = static_cast<const uint8_t*>(buf);
  size_t off = 0;
  while (off < len) {
    const ssize_t n = ::send(fd, p + off, len - off, 0);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) {
        continue;
      }
      return false;
    }
    off += static_cast<size_t>(n);
  }
  return true;
}

bool read_all(int fd, void* buf, size_t len) {
  auto* p = static_cast<uint8_t*>(buf);
  size_t off = 0;
  while (off < len) {
    const ssize_t n = ::recv(fd, p + off, len - off, 0);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) {
        continue;
      }
      return false;
    }
    off += static_cast<size_t>(n);
  }
  return true;
}

}  // namespace

EfaEngine::~EfaEngine() {
  shutdown();
}

bool EfaEngine::set_config_and_initialize(const NetworkConfig& cfg) {
  DAQIRI_LOG_INFO("Setting up EFA (libfabric) engine");
  cfg_ = cfg;
  initialize();
  return initialized_;
}

// ---------------------------------------------------------------------------
// libfabric init / teardown
// ---------------------------------------------------------------------------

bool EfaEngine::ofi_init() {
  struct fi_info* hints = fi_allocinfo();
  if (hints == nullptr) {
    DAQIRI_LOG_CRITICAL("fi_allocinfo failed");
    return false;
  }

  hints->fabric_attr->prov_name = strdup("efa");
  hints->ep_attr->type = FI_EP_RDM;  // SRD reliable datagram
  hints->caps = FI_MSG | FI_HMEM;    // two-sided messaging + GPU memory
  hints->mode = 0;
  // Request the modes EFA advertises plus FI_MR_LOCAL so we can pass our own
  // descriptors on send/recv (required for HMEM buffers). The provider keeps
  // whatever subset it accepts; we read it back below rather than assume.
  hints->domain_attr->mr_mode =
      FI_MR_LOCAL | FI_MR_VIRT_ADDR | FI_MR_ALLOCATED | FI_MR_PROV_KEY | FI_MR_HMEM;
  hints->domain_attr->threading = FI_THREAD_SAFE;
  hints->domain_attr->resource_mgmt = FI_RM_ENABLED;
  hints->tx_attr->msg_order = FI_ORDER_SAS;
  hints->rx_attr->msg_order = FI_ORDER_SAS;

  int rc = fi_getinfo(DAQIRI_EFA_FI_VERSION, nullptr, nullptr, 0, hints, &fi_);
  fi_freeinfo(hints);
  if (rc != 0 || fi_ == nullptr) {
    DAQIRI_LOG_CRITICAL(
        "fi_getinfo for provider 'efa' failed: {} ({}). No EFA device found. "
        "Check that the efa kernel driver is loaded, an EFA-enabled ENI is "
        "attached, and libfabric was built with the EFA provider and CUDA "
        "support (run `fi_info -p efa` to verify).",
        rc, fi_strerror(-rc));
    return false;
  }

  DAQIRI_LOG_INFO(
      "Selected EFA fabric '{}' domain '{}' mr_mode=0x{:x} max_msg_size={} "
      "tx_size={} rx_size={}",
      fi_->fabric_attr->prov_name ? fi_->fabric_attr->prov_name : "?",
      fi_->domain_attr->name ? fi_->domain_attr->name : "?", fi_->domain_attr->mr_mode,
      fi_->ep_attr->max_msg_size, fi_->tx_attr->size, fi_->rx_attr->size);

  if ((fi_->caps & FI_HMEM) == 0) {
    DAQIRI_LOG_WARN(
        "EFA provider reports no FI_HMEM support; GPU (device) memory regions "
        "will fail to register. Rebuild libfabric --with-cuda.");
  }

  if (fi_fabric(fi_->fabric_attr, &fabric_, nullptr) != 0) {
    DAQIRI_LOG_CRITICAL("fi_fabric failed");
    return false;
  }
  if (fi_domain(fabric_, fi_, &domain_, nullptr) != 0) {
    DAQIRI_LOG_CRITICAL("fi_domain failed");
    return false;
  }

  struct fi_cq_attr cq_attr;
  memset(&cq_attr, 0, sizeof(cq_attr));
  cq_attr.format = FI_CQ_FORMAT_DATA;  // carries op_context + len + flags
  cq_attr.wait_obj = FI_WAIT_NONE;     // busy-poll hot path
  cq_attr.size = CQ_DEPTH;
  if (fi_cq_open(domain_, &cq_attr, &txcq_, nullptr) != 0 ||
      fi_cq_open(domain_, &cq_attr, &rxcq_, nullptr) != 0) {
    DAQIRI_LOG_CRITICAL("fi_cq_open failed");
    return false;
  }

  struct fi_av_attr av_attr;
  memset(&av_attr, 0, sizeof(av_attr));
  av_attr.type = FI_AV_TABLE;
  av_attr.count = MAX_EFA_CONNECTIONS;
  if (fi_av_open(domain_, &av_attr, &av_, nullptr) != 0) {
    DAQIRI_LOG_CRITICAL("fi_av_open failed");
    return false;
  }

  if (fi_endpoint(domain_, fi_, &ep_, nullptr) != 0) {
    DAQIRI_LOG_CRITICAL("fi_endpoint failed");
    return false;
  }
  if (fi_ep_bind(ep_, &txcq_->fid, FI_TRANSMIT) != 0 ||
      fi_ep_bind(ep_, &rxcq_->fid, FI_RECV) != 0 || fi_ep_bind(ep_, &av_->fid, 0) != 0) {
    DAQIRI_LOG_CRITICAL("fi_ep_bind failed");
    return false;
  }
  if (fi_enable(ep_) != 0) {
    DAQIRI_LOG_CRITICAL("fi_enable failed");
    return false;
  }

  DAQIRI_LOG_INFO("libfabric EFA endpoint enabled");
  return true;
}

void EfaEngine::ofi_teardown() {
  if (ep_ != nullptr) {
    fi_close(&ep_->fid);
    ep_ = nullptr;
  }
  for (auto& entry : mrs_) {
    if (entry.second.mr != nullptr) {
      fi_close(&entry.second.mr->fid);
      entry.second.mr = nullptr;
    }
  }
  if (txcq_ != nullptr) {
    fi_close(&txcq_->fid);
    txcq_ = nullptr;
  }
  if (rxcq_ != nullptr) {
    fi_close(&rxcq_->fid);
    rxcq_ = nullptr;
  }
  if (av_ != nullptr) {
    fi_close(&av_->fid);
    av_ = nullptr;
  }
  if (domain_ != nullptr) {
    fi_close(&domain_->fid);
    domain_ = nullptr;
  }
  if (fabric_ != nullptr) {
    fi_close(&fabric_->fid);
    fabric_ = nullptr;
  }
  if (fi_ != nullptr) {
    fi_freeinfo(fi_);
    fi_ = nullptr;
  }
}

int EfaEngine::register_mr(const MemoryRegionConfig& mr, void* ptr) {
  efa_mr_params params;
  params.params_ = mr;
  params.ptr_ = ptr;

  struct fi_mr_attr attr;
  memset(&attr, 0, sizeof(attr));
  struct iovec iov;
  iov.iov_base = ptr;
  iov.iov_len = mr.adj_size_ * mr.num_bufs_;
  attr.mr_iov = &iov;
  attr.iov_count = 1;
  attr.access = FI_SEND | FI_RECV | FI_REMOTE_READ | FI_REMOTE_WRITE;

  if (mr.kind_ == MemoryKind::DEVICE) {
    attr.iface = FI_HMEM_CUDA;
    attr.device.cuda = mr.affinity_;
  } else {
    attr.iface = FI_HMEM_SYSTEM;
  }

  // Path B (iovec/GDRCopy). dma-buf registration (Path A) is a follow-up; it
  // requires the CUDA driver API and libfabric's FI_MR_DMABUF flag.
  int rc = fi_mr_regattr(domain_, &attr, 0, &params.mr);
  if (rc != 0 || params.mr == nullptr) {
    DAQIRI_LOG_CRITICAL("fi_mr_regattr failed for MR {} ({} bytes, iface {}): {}", mr.name_,
                        iov.iov_len, static_cast<int>(attr.iface), fi_strerror(-rc));
    return -1;
  }
  params.desc = fi_mr_desc(params.mr);

  mrs_[mr.name_] = params;
  DAQIRI_LOG_INFO("Registered EFA MR {} ({} bytes) iface={} desc={}", mr.name_, iov.iov_len,
                  static_cast<int>(attr.iface), params.desc);
  return 0;
}

void* EfaEngine::mr_desc_for_name(const std::string& name) {
  auto it = mrs_.find(name);
  return it == mrs_.end() ? nullptr : it->second.desc;
}

int EfaEngine::register_cfg_mrs() {
  if (allocate_memory_regions() != Status::SUCCESS) {
    DAQIRI_LOG_CRITICAL("Failed to allocate memory regions");
    return -1;
  }

  for (const auto& mr : cfg_.mrs_) {
    auto ring = rte_ring_create(mr.second.name_.c_str(), rte_align32pow2(mr.second.num_bufs_ + 1),
                                rte_socket_id(), 0);
    if (ring == nullptr) {
      DAQIRI_LOG_CRITICAL("Failed to create ring for MR {}", mr.second.name_);
      return -1;
    }
    mem_pools_[mr.second.name_] = ring;

    if (populate_pool(ring, mr.second.name_) != Status::SUCCESS) {
      DAQIRI_LOG_CRITICAL("Failed to populate pool for MR {}", mr.second.name_);
      return -1;
    }

    if (register_mr(mr.second, ar_[mr.second.name_].ptr_) < 0) {
      return -1;
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// DPDK ring/pool plumbing (same shape as RdmaEngine)
// ---------------------------------------------------------------------------

int EfaEngine::setup_pools_and_rings() {
  for (int i = 0; i < MAX_EFA_CONNECTIONS; i++) {
    std::string name = "EFA_RX_RING_" + std::to_string(i);
    auto* ring =
        rte_ring_create(name.c_str(), 2048, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (ring == nullptr) {
      DAQIRI_LOG_CRITICAL("Failed to allocate {}", name);
      return -1;
    }
    rx_rings_.push(ring);

    name = "EFA_TX_RING_" + std::to_string(i);
    ring = rte_ring_create(name.c_str(), 2048, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (ring == nullptr) {
      DAQIRI_LOG_CRITICAL("Failed to allocate {}", name);
      return -1;
    }
    tx_rings_.push(ring);
  }

  pkt_len_pool_ =
      rte_mempool_create("EFA_PKT_LEN_POOL", (1U << 11) - 1U, sizeof(uint32_t) * MAX_EFA_BATCH, 0,
                         0, nullptr, nullptr, nullptr, nullptr, rte_socket_id(), 0);
  rx_meta_ = rte_mempool_create("EFA_RX_META_POOL", (1U << 11) - 1U, sizeof(BurstParams), 0, 0,
                                nullptr, nullptr, nullptr, nullptr, rte_socket_id(), 0);
  tx_meta_ = rte_mempool_create("EFA_TX_META_POOL", (1U << 11) - 1U, sizeof(BurstParams), 0, 0,
                                nullptr, nullptr, nullptr, nullptr, rte_socket_id(), 0);
  tx_burst_pool_ =
      rte_mempool_create("EFA_TX_BURST_POOL", (1U << 11) - 1U, sizeof(void*) * MAX_EFA_BATCH, 0, 0,
                         nullptr, nullptr, nullptr, nullptr, rte_socket_id(), 0);
  if (pkt_len_pool_ == nullptr || rx_meta_ == nullptr || tx_meta_ == nullptr ||
      tx_burst_pool_ == nullptr) {
    DAQIRI_LOG_CRITICAL("Failed to allocate EFA meta/burst pools");
    return -1;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

void EfaEngine::initialize() {
  struct EalCleanupGuard {
    EfaEngine* mgr;
    ~EalCleanupGuard() {
      if (!mgr->initialized_ && mgr->eal_initialized_) {
        rte_eal_cleanup();
        mgr->eal_initialized_ = false;
      }
    }
  } cleanup_guard{this};

  if (!check_hugepage_availability()) {
    DAQIRI_LOG_CRITICAL("Aborting before rte_eal_init() to keep /dev/hugepages clean");
    return;
  }

  // Build minimal EAL args: master core first, then every configured queue core.
  std::string cores = std::to_string(cfg_.common_.master_core_) + ",";
  int if_num = 0;
  for (auto& intf : cfg_.ifs_) {
    for (const auto& q : intf.rx_.queues_) {
      cores += q.common_.cpu_core_ + ",";
    }
    for (const auto& q : intf.tx_.queues_) {
      cores += q.common_.cpu_core_ + ",";
    }
    intf.port_id_ = if_num++;
  }
  cores = cores.substr(0, cores.size() - 1);

  const std::string prefix = generate_random_string(10);
  std::vector<std::string> args = {"daqiri_efa", "--file-prefix=" + prefix, "-l", cores,
                                   "--no-pci"};
  std::vector<char*> argv;
  for (auto& a : args) {
    argv.push_back(const_cast<char*>(a.c_str()));
  }

  if (rte_eal_init(static_cast<int>(argv.size()), argv.data()) < 0) {
    DAQIRI_LOG_CRITICAL("rte_eal_init failed: {}", rte_errno);
    return;
  }
  eal_initialized_ = true;

  for (auto& mr : cfg_.mrs_) {
    const size_t align = std::max<size_t>(get_alignment(mr.second.kind_), GPU_PAGE_SIZE);
    mr.second.adj_size_ = RTE_ALIGN_CEIL(mr.second.buf_size_, align);
  }

  if (!ofi_init()) {
    return;
  }
  if (register_cfg_mrs() < 0) {
    return;
  }
  if (setup_pools_and_rings() != 0) {
    return;
  }

  main_thread_ = std::thread(&EfaEngine::run, this);

  DAQIRI_LOG_INFO("EFA engine initialized");
  initialized_ = true;
}

// ---------------------------------------------------------------------------
// Bootstrap address exchange + connection setup
// ---------------------------------------------------------------------------

Status EfaEngine::exchange_addr(int boot_fd, fi_addr_t* peer) {
  // Local EFA address (opaque, variable length).
  char laddr[MAX_EFA_ADDR_LEN];
  size_t llen = sizeof(laddr);
  int rc = fi_getname(&ep_->fid, laddr, &llen);
  if (rc != 0) {
    DAQIRI_LOG_CRITICAL("fi_getname failed: {}", fi_strerror(-rc));
    return Status::CONNECT_FAILURE;
  }

  const uint32_t llen_net = htonl(static_cast<uint32_t>(llen));
  if (!write_all(boot_fd, &llen_net, sizeof(llen_net)) || !write_all(boot_fd, laddr, llen)) {
    DAQIRI_LOG_CRITICAL("Failed to send local EFA address over bootstrap socket");
    return Status::CONNECT_FAILURE;
  }

  uint32_t plen_net = 0;
  if (!read_all(boot_fd, &plen_net, sizeof(plen_net))) {
    DAQIRI_LOG_CRITICAL("Failed to read peer EFA address length");
    return Status::CONNECT_FAILURE;
  }
  const uint32_t plen = ntohl(plen_net);
  if (plen == 0 || plen > MAX_EFA_ADDR_LEN) {
    DAQIRI_LOG_CRITICAL("Invalid peer EFA address length {}", plen);
    return Status::CONNECT_FAILURE;
  }

  char paddr[MAX_EFA_ADDR_LEN];
  if (!read_all(boot_fd, paddr, plen)) {
    DAQIRI_LOG_CRITICAL("Failed to read peer EFA address");
    return Status::CONNECT_FAILURE;
  }

  if (fi_av_insert(av_, paddr, 1, peer, 0, nullptr) != 1) {
    DAQIRI_LOG_CRITICAL("fi_av_insert failed for peer EFA address");
    return Status::CONNECT_FAILURE;
  }
  return Status::SUCCESS;
}

efa_conn* EfaEngine::make_connection(int boot_fd, int if_idx, bool server, fi_addr_t peer) {
  std::lock_guard<std::mutex> lock(conns_mutex_);
  if (tx_rings_.empty() || rx_rings_.empty()) {
    DAQIRI_LOG_CRITICAL("Out of EFA TX/RX rings (max {} connections)", MAX_EFA_CONNECTIONS);
    return nullptr;
  }

  auto conn = std::make_unique<efa_conn>();
  conn->peer = peer;
  conn->boot_fd = boot_fd;
  conn->if_idx = if_idx;
  conn->server = server;
  conn->tx_ring = tx_rings_.front();
  tx_rings_.pop();
  conn->rx_ring = rx_rings_.front();
  rx_rings_.pop();

  efa_conn* raw = conn.get();
  const auto id = reinterpret_cast<uintptr_t>(raw);
  tx_rings_map_[id] = conn->tx_ring;
  rx_rings_map_[id] = conn->rx_ring;
  conns_.push_back(std::move(conn));
  return raw;
}

Status EfaEngine::rdma_connect_to_server(const std::string& dst_addr, uint16_t dst_port,
                                         uintptr_t* conn_id) {
  return rdma_connect_to_server(dst_addr, dst_port, "", conn_id);
}

Status EfaEngine::rdma_connect_to_server(const std::string& dst_addr, uint16_t dst_port,
                                         const std::string& src_addr, uintptr_t* conn_id) {
  (void)src_addr;
  if (!initialized_) {
    DAQIRI_LOG_WARN("EFA engine not initialized; cannot connect");
    return Status::NOT_READY;
  }
  *conn_id = 0;

  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    DAQIRI_LOG_CRITICAL("Failed to create bootstrap socket: {}", strerror(errno));
    return Status::CONNECT_FAILURE;
  }

  sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(dst_port);
  if (inet_pton(AF_INET, dst_addr.c_str(), &addr.sin_addr) != 1) {
    DAQIRI_LOG_CRITICAL("Invalid server address {}", dst_addr);
    ::close(fd);
    return Status::CONNECT_FAILURE;
  }

  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    DAQIRI_LOG_CRITICAL("Bootstrap connect to {}:{} failed: {}", dst_addr, dst_port,
                        strerror(errno));
    ::close(fd);
    return Status::CONNECT_FAILURE;
  }

  fi_addr_t peer = FI_ADDR_UNSPEC;
  if (exchange_addr(fd, &peer) != Status::SUCCESS) {
    ::close(fd);
    return Status::CONNECT_FAILURE;
  }

  // Map the client's source interface to a configured port index (best effort).
  int if_idx = 0;
  efa_conn* conn = make_connection(fd, if_idx, /*server=*/false, peer);
  if (conn == nullptr) {
    ::close(fd);
    return Status::CONNECT_FAILURE;
  }

  *conn_id = reinterpret_cast<uintptr_t>(conn);
  DAQIRI_LOG_INFO("EFA client connected to {}:{} (conn_id {:#x})", dst_addr, dst_port, *conn_id);
  return Status::SUCCESS;
}

Status EfaEngine::rdma_get_server_conn_id(const std::string& server_addr, uint16_t server_port,
                                          uintptr_t* conn_id) {
  const std::string key = server_addr + ":" + std::to_string(server_port);
  std::lock_guard<std::mutex> lock(conns_mutex_);
  auto it = server_conns_.find(key);
  if (it == server_conns_.end() || it->second.empty()) {
    DAQIRI_LOG_WARN("No EFA client has connected to server {} yet", key);
    return Status::NO_SPACE_AVAILABLE;
  }
  // Hand back the oldest unclaimed connection (front of the list).
  efa_conn* conn = it->second.front();
  it->second.erase(it->second.begin());
  *conn_id = reinterpret_cast<uintptr_t>(conn);
  return Status::SUCCESS;
}

Status EfaEngine::rdma_get_port_queue(uintptr_t conn_id, uint16_t* port, uint16_t* queue) {
  auto* conn = reinterpret_cast<efa_conn*>(conn_id);
  if (conn == nullptr) {
    return Status::INVALID_PARAMETER;
  }
  *port = static_cast<uint16_t>(conn->if_idx);
  *queue = 0;
  return Status::SUCCESS;
}

// ---------------------------------------------------------------------------
// Bootstrap listener (server) + progress loop
// ---------------------------------------------------------------------------

void EfaEngine::run() {
  // Stand up a TCP bootstrap listener per server interface.
  for (const auto& intf : cfg_.ifs_) {
    if (intf.rdma_.mode_ != RDMAMode::SERVER) {
      continue;
    }

    int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
      DAQIRI_LOG_CRITICAL("Failed to create EFA bootstrap listen socket: {}", strerror(errno));
      continue;
    }
    int yes = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(intf.rdma_.port_);
    if (inet_pton(AF_INET, intf.address_.c_str(), &addr.sin_addr) != 1) {
      DAQIRI_LOG_ERROR("Invalid server address {}", intf.address_);
      ::close(lfd);
      continue;
    }
    if (::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(lfd, MAX_EFA_CONNECTIONS) != 0) {
      DAQIRI_LOG_CRITICAL("EFA bootstrap bind/listen on {}:{} failed: {}", intf.address_,
                          intf.rdma_.port_, strerror(errno));
      ::close(lfd);
      continue;
    }

    // Non-blocking so the accept loop can observe force_quit_.
    int flags = fcntl(lfd, F_GETFL);
    fcntl(lfd, F_SETFL, flags | O_NONBLOCK);
    listen_fds_.push_back(lfd);
    DAQIRI_LOG_INFO("EFA bootstrap listening on {}:{}", intf.address_, intf.rdma_.port_);
  }

  // Build a fast lookup from listen fd -> (server key, if_idx).
  std::unordered_map<int, std::pair<std::string, int>> listener_meta;
  {
    size_t li = 0;
    for (int idx = 0; idx < static_cast<int>(cfg_.ifs_.size()); ++idx) {
      const auto& intf = cfg_.ifs_[idx];
      if (intf.rdma_.mode_ != RDMAMode::SERVER) {
        continue;
      }
      if (li < listen_fds_.size()) {
        listener_meta[listen_fds_[li]] = {intf.address_ + ":" + std::to_string(intf.rdma_.port_),
                                          idx};
        ++li;
      }
    }
  }

  // Single progress thread: services all connections' TX rings and drains the
  // shared CQs. A connectionless EFA endpoint shares one tx/rx CQ pair across
  // peers, so polling must be centralized (unlike RdmaEngine's per-QP threads).
  std::thread progress([this]() {
    set_affinity(cfg_.common_.master_core_);
    std::unordered_map<BurstParams*, int> pending;  // burst -> outstanding ops
    while (!force_quit_.load()) {
      // 1. Post any queued TX/RECV work across all connections.
      std::vector<efa_conn*> snapshot;
      {
        std::lock_guard<std::mutex> lock(conns_mutex_);
        snapshot.reserve(conns_.size());
        for (auto& c : conns_) {
          snapshot.push_back(c.get());
        }
      }

      for (efa_conn* conn : snapshot) {
        BurstParams* burst = nullptr;
        if (rte_ring_dequeue(conn->tx_ring, reinterpret_cast<void**>(&burst)) != 0) {
          continue;
        }

        void* desc = mr_desc_for_name(burst->transport_hdr.local_mr_name);
        const int num_pkts = static_cast<int>(burst->transport_hdr.num_pkts);
        const bool is_send = burst->transport_hdr.opcode == RDMAOpCode::SEND;
        int posted = 0;
        for (int p = 0; p < num_pkts; ++p) {
          auto* ctx = new efa_op_ctx{burst, p, is_send, conn};
          void* buf = burst->pkts[0][p];
          const size_t len = burst->pkt_lens[0][p];
          ssize_t rc;
          do {
            rc = is_send ? fi_send(ep_, buf, len, desc, conn->peer, ctx)
                         : fi_recv(ep_, buf, len, desc, FI_ADDR_UNSPEC, ctx);
            if (rc == -FI_EAGAIN) {
              poll_cq(txcq_, conn, pending);
              poll_cq(rxcq_, conn, pending);
            }
          } while (rc == -FI_EAGAIN && !force_quit_.load());
          if (rc != 0) {
            DAQIRI_LOG_ERROR("fi_{} failed: {}", is_send ? "send" : "recv", fi_strerror(-rc));
            delete ctx;
            break;
          }
          ++posted;
        }
        if (posted > 0) {
          pending[burst] = posted;
        }
      }

      // 2. Reap completions and route finished bursts to their RX ring.
      poll_cq(txcq_, nullptr, pending);
      poll_cq(rxcq_, nullptr, pending);
    }
  });

  // Accept loop on the bootstrap listeners.
  while (!force_quit_.load()) {
    bool any = false;
    for (int lfd : listen_fds_) {
      sockaddr_in caddr;
      socklen_t clen = sizeof(caddr);
      int cfd = ::accept(lfd, reinterpret_cast<sockaddr*>(&caddr), &clen);
      if (cfd < 0) {
        continue;
      }
      any = true;

      const auto meta = listener_meta.find(lfd);
      const int if_idx = meta == listener_meta.end() ? 0 : meta->second.second;

      fi_addr_t peer = FI_ADDR_UNSPEC;
      if (exchange_addr(cfd, &peer) != Status::SUCCESS) {
        ::close(cfd);
        continue;
      }
      efa_conn* conn = make_connection(cfd, if_idx, /*server=*/true, peer);
      if (conn == nullptr) {
        ::close(cfd);
        continue;
      }
      if (meta != listener_meta.end()) {
        std::lock_guard<std::mutex> lock(conns_mutex_);
        server_conns_[meta->second.first].push_back(conn);
      }
      DAQIRI_LOG_INFO("EFA server accepted bootstrap connection on if {}", if_idx);
    }
    if (!any) {
      usleep(500);
    }
  }

  if (progress.joinable()) {
    progress.join();
  }
  DAQIRI_LOG_INFO("EFA run loop exiting");
}

void EfaEngine::poll_cq(struct fid_cq* cq, efa_conn* /*conn*/,
                        std::unordered_map<BurstParams*, int>& pending) {
  struct fi_cq_data_entry comp[CQ_POLL_BATCH];
  ssize_t n = fi_cq_read(cq, comp, CQ_POLL_BATCH);
  if (n == -FI_EAGAIN) {
    return;
  }
  if (n == -FI_EAVAIL) {
    struct fi_cq_err_entry err;
    memset(&err, 0, sizeof(err));
    fi_cq_readerr(cq, &err, 0);
    auto* ctx = static_cast<efa_op_ctx*>(err.op_context);
    DAQIRI_LOG_ERROR("EFA completion error: {} ({})", err.err,
                     fi_cq_strerror(cq, err.prov_errno, err.err_data, nullptr, 0));
    if (ctx != nullptr) {
      auto it = pending.find(ctx->burst);
      if (it != pending.end() && --it->second <= 0) {
        pending.erase(it);
      }
      delete ctx;
    }
    return;
  }
  if (n < 0) {
    DAQIRI_LOG_ERROR("fi_cq_read failed: {}", fi_strerror(static_cast<int>(-n)));
    return;
  }

  for (ssize_t i = 0; i < n; ++i) {
    auto* ctx = static_cast<efa_op_ctx*>(comp[i].op_context);
    if (ctx == nullptr) {
      continue;
    }
    BurstParams* burst = ctx->burst;
    efa_conn* owner = ctx->conn;
    const bool is_tx = ctx->is_tx;
    delete ctx;

    auto it = pending.find(burst);
    if (it == pending.end()) {
      continue;
    }
    if (--it->second > 0) {
      continue;
    }  // burst not fully complete yet
    pending.erase(it);

    burst->transport_hdr.tx = is_tx;
    burst->transport_hdr.server = owner->server;
    burst->transport_hdr.status = Status::SUCCESS;
    if (is_tx) {
      tx_pkts_.fetch_add(burst->transport_hdr.num_pkts);
    } else {
      rx_pkts_.fetch_add(burst->transport_hdr.num_pkts);
    }
    if (rte_ring_enqueue(owner->rx_ring, burst) != 0) {
      DAQIRI_LOG_ERROR("Failed to enqueue EFA completion burst");
      if (is_tx) {
        free_tx_burst(burst);
      } else {
        free_rx_burst(burst);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Burst / packet API (same shape as RdmaEngine)
// ---------------------------------------------------------------------------

void* EfaEngine::get_packet_ptr(BurstParams* burst, int idx) {
  return burst->pkts[0][idx];
}
void* EfaEngine::get_segment_packet_ptr(BurstParams* burst, int seg, int idx) {
  return burst->pkts[seg][idx];
}
uint32_t EfaEngine::get_packet_length(BurstParams* burst, int idx) {
  return burst->pkt_lens[0][idx];
}
uint32_t EfaEngine::get_segment_packet_length(BurstParams* burst, int seg, int idx) {
  return burst->pkt_lens[seg][idx];
}

Status EfaEngine::set_packet_lengths(BurstParams* burst, int idx,
                                     const std::initializer_list<int>& lens) {
  assert(lens.size() == 1);  // no header-data split on EFA yet
  burst->pkt_lens[0][idx] = lens.begin()[0];
  return Status::SUCCESS;
}

Status EfaEngine::set_eth_header(BurstParams*, int, char*) {
  DAQIRI_LOG_CRITICAL("Cannot set Ethernet header in EFA mode");
  return Status::NOT_SUPPORTED;
}
Status EfaEngine::set_ipv4_header(BurstParams*, int, int, uint8_t, unsigned int, unsigned int) {
  DAQIRI_LOG_CRITICAL("Cannot set IPv4 header in EFA mode");
  return Status::NOT_SUPPORTED;
}
Status EfaEngine::set_udp_header(BurstParams*, int, int, uint16_t, uint16_t) {
  DAQIRI_LOG_CRITICAL("Cannot set UDP header in EFA mode");
  return Status::NOT_SUPPORTED;
}
Status EfaEngine::set_udp_payload(BurstParams* burst, int idx, void* data, int len) {
  rte_memcpy(burst->pkts[0][idx], data, len);
  return Status::SUCCESS;
}

uint64_t EfaEngine::get_burst_tot_byte(BurstParams*) {
  return 0;
}

BurstParams* EfaEngine::create_tx_burst_params() {
  BurstParams* burst = nullptr;
  if (rte_mempool_get(tx_meta_, reinterpret_cast<void**>(&burst)) != 0) {
    DAQIRI_LOG_CRITICAL("Failed to get EFA TX meta descriptor");
    return nullptr;
  }
  reset_efa_burst_metadata(burst);
  return burst;
}

Status EfaEngine::get_tx_packet_burst(BurstParams* burst) {
  if (burst == nullptr) {
    return Status::INVALID_PARAMETER;
  }
  assert(burst->transport_hdr.num_segs == 1);
  assert(burst->transport_hdr.num_pkts <= MAX_EFA_BATCH);

  auto burst_pool = mem_pools_.find(burst->transport_hdr.local_mr_name);
  if (burst_pool == mem_pools_.end()) {
    DAQIRI_LOG_ERROR("Failed to look up burst pool for MR {}", burst->transport_hdr.local_mr_name);
    return Status::INVALID_PARAMETER;
  }

  if (rte_mempool_get(tx_burst_pool_, reinterpret_cast<void**>(&burst->pkts[0])) != 0) {
    return Status::NO_FREE_BURST_BUFFERS;
  }

  const unsigned num_pkts = static_cast<unsigned>(burst->transport_hdr.num_pkts);
  const unsigned got = rte_ring_dequeue_bulk(
      burst_pool->second, reinterpret_cast<void**>(burst->pkts[0]), num_pkts, nullptr);
  if (got != num_pkts) {
    rte_mempool_put(tx_burst_pool_, reinterpret_cast<void*>(burst->pkts[0]));
    burst->pkts[0] = nullptr;
    return Status::NO_FREE_BURST_BUFFERS;
  }

  if (rte_mempool_get(pkt_len_pool_, reinterpret_cast<void**>(&burst->pkt_lens[0])) != 0) {
    rte_ring_enqueue_bulk(burst_pool->second, reinterpret_cast<void**>(burst->pkts[0]), num_pkts,
                          nullptr);
    rte_mempool_put(tx_burst_pool_, reinterpret_cast<void*>(burst->pkts[0]));
    burst->pkts[0] = nullptr;
    return Status::NO_FREE_PACKET_BUFFERS;
  }
  return Status::SUCCESS;
}

bool EfaEngine::is_tx_burst_available(BurstParams* burst) {
  auto burst_pool = mem_pools_.find(burst->transport_hdr.local_mr_name);
  if (burst_pool == mem_pools_.end()) {
    return false;
  }
  return rte_ring_count(burst_pool->second) >= burst->transport_hdr.num_pkts;
}

Status EfaEngine::send_tx_burst(BurstParams* burst) {
  if (burst == nullptr) {
    return Status::INVALID_PARAMETER;
  }

  const auto conn_id = get_connection_id(burst);
  struct rte_ring* ring = nullptr;
  {
    std::lock_guard<std::mutex> lock(conns_mutex_);
    auto it = tx_rings_map_.find(conn_id);
    if (it == tx_rings_map_.end()) {
      DAQIRI_LOG_ERROR("Invalid connection ID in send_tx_burst: {:#x}", conn_id);
      free_tx_burst(burst);
      return Status::INVALID_PARAMETER;
    }
    ring = it->second;
  }

  if (rte_ring_enqueue(ring, burst) != 0) {
    free_tx_burst(burst);
    return Status::NO_SPACE_AVAILABLE;
  }
  return Status::SUCCESS;
}

Status EfaEngine::get_rx_burst(BurstParams** burst, uintptr_t conn_id, bool server) {
  (void)server;
  struct rte_ring* ring = nullptr;
  {
    std::lock_guard<std::mutex> lock(conns_mutex_);
    auto it = rx_rings_map_.find(conn_id);
    if (it == rx_rings_map_.end()) {
      DAQIRI_LOG_CRITICAL("No EFA RX ring for conn_id {:#x}", conn_id);
      return Status::INVALID_PARAMETER;
    }
    ring = it->second;
  }
  if (rte_ring_dequeue(ring, reinterpret_cast<void**>(burst)) != 0) {
    return Status::NOT_READY;
  }
  return Status::SUCCESS;
}

RDMAOpCode EfaEngine::rdma_get_opcode(BurstParams* burst) {
  return burst->transport_hdr.opcode;
}

Status EfaEngine::rdma_set_header(BurstParams* burst, RDMAOpCode op_code, uintptr_t conn_id,
                                  bool is_server, int num_pkts, uint64_t wr_id,
                                  const std::string& local_mr_name) {
  burst->transport_hdr.opcode = op_code;
  set_connection_id(burst, conn_id);
  burst->transport_hdr.server = is_server;
  burst->transport_hdr.num_pkts = num_pkts;
  burst->transport_hdr.num_segs = 1;
  burst->transport_hdr.wr_id = wr_id;
  snprintf(burst->transport_hdr.local_mr_name, sizeof(burst->transport_hdr.local_mr_name), "%s",
           local_mr_name.c_str());
  return Status::SUCCESS;
}

// ---------------------------------------------------------------------------
// Free / metadata
// ---------------------------------------------------------------------------

void EfaEngine::free_rx_burst(BurstParams* burst) {
  if (burst == nullptr) {
    return;
  }
  rte_mempool_put(rx_meta_, burst);
}

void EfaEngine::free_tx_burst(BurstParams* burst) {
  if (burst == nullptr) {
    return;
  }
  auto burst_pool = mem_pools_.find(burst->transport_hdr.local_mr_name);
  if (burst->transport_hdr.num_pkts > 0 && burst->pkts[0] != nullptr &&
      burst_pool != mem_pools_.end()) {
    rte_ring_enqueue_bulk(burst_pool->second, reinterpret_cast<void**>(burst->pkts[0]),
                          burst->transport_hdr.num_pkts, nullptr);
  }
  if (burst->pkts[0] != nullptr) {
    rte_mempool_put(tx_burst_pool_, burst->pkts[0]);
  }
  if (burst->pkt_lens[0] != nullptr) {
    rte_mempool_put(pkt_len_pool_, burst->pkt_lens[0]);
  }
  reset_efa_burst_metadata(burst);
  rte_mempool_put(tx_meta_, burst);
}

void EfaEngine::free_rx_metadata(BurstParams* burst) {
  if (burst == nullptr) {
    return;
  }
  reset_efa_burst_metadata(burst);
  rte_mempool_put(rx_meta_, burst);
}

void EfaEngine::free_tx_metadata(BurstParams* burst) {
  if (burst == nullptr) {
    return;
  }
  reset_efa_burst_metadata(burst);
  rte_mempool_put(tx_meta_, burst);
}

Status EfaEngine::get_tx_metadata_buffer(BurstParams** burst) {
  if (burst == nullptr) {
    return Status::INVALID_PARAMETER;
  }
  *burst = create_tx_burst_params();
  return *burst == nullptr ? Status::NO_FREE_BURST_BUFFERS : Status::SUCCESS;
}

// ---------------------------------------------------------------------------
// Misc
// ---------------------------------------------------------------------------

int EfaEngine::set_affinity(int cpu_core) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(cpu_core, &cpuset);
  return pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
}

void EfaEngine::print_stats() {
  DAQIRI_LOG_INFO("daqiri EFA (libfabric) engine stats");
  DAQIRI_LOG_INFO("  initialized={} connections={} registered_mrs={}", initialized_, conns_.size(),
                  mrs_.size());
  DAQIRI_LOG_INFO("  tx_pkts={} rx_pkts={}", tx_pkts_.load(), rx_pkts_.load());
}

void EfaEngine::shutdown() {
  if (!initialized_) {
    return;
  }
  DAQIRI_LOG_INFO("EFA engine shutting down");
  force_quit_.store(true);

  if (main_thread_.joinable()) {
    main_thread_.join();
  }
  for (auto& t : worker_threads_) {
    if (t.second.joinable()) {
      t.second.join();
    }
  }
  worker_threads_.clear();

  for (int lfd : listen_fds_) {
    if (lfd >= 0) {
      ::close(lfd);
    }
  }
  listen_fds_.clear();

  {
    std::lock_guard<std::mutex> lock(conns_mutex_);
    for (auto& c : conns_) {
      if (c->boot_fd >= 0) {
        ::close(c->boot_fd);
      }
    }
    conns_.clear();
    tx_rings_map_.clear();
    rx_rings_map_.clear();
    server_conns_.clear();
  }

  ofi_teardown();

  // Release DPDK resources before EAL cleanup.
  while (!rx_rings_.empty()) {
    if (rx_rings_.front() != nullptr) {
      rte_ring_free(rx_rings_.front());
    }
    rx_rings_.pop();
  }
  while (!tx_rings_.empty()) {
    if (tx_rings_.front() != nullptr) {
      rte_ring_free(tx_rings_.front());
    }
    tx_rings_.pop();
  }
  for (auto& e : mem_pools_) {
    if (e.second != nullptr) {
      rte_ring_free(e.second);
    }
  }
  mem_pools_.clear();
  if (pkt_len_pool_ != nullptr) {
    rte_mempool_free(pkt_len_pool_);
    pkt_len_pool_ = nullptr;
  }
  if (rx_meta_ != nullptr) {
    rte_mempool_free(rx_meta_);
    rx_meta_ = nullptr;
  }
  if (tx_meta_ != nullptr) {
    rte_mempool_free(tx_meta_);
    tx_meta_ = nullptr;
  }
  if (tx_burst_pool_ != nullptr) {
    rte_mempool_free(tx_burst_pool_);
    tx_burst_pool_ = nullptr;
  }

  if (eal_initialized_) {
    rte_eal_cleanup();
    eal_initialized_ = false;
  }
  initialized_ = false;
}

}  // namespace daqiri
