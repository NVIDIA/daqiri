// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "daqiri_rdma_engine.h"
#include "src/rdma/cm_api.h"
#include "src/metrics.h"
#include <cstring>

namespace daqiri {
extern std::atomic<bool> rdma_force_quit;
namespace {
const void* caller_identity() {
  static thread_local const char identity = 0;
  return &identity;
}
size_t direct_hash(uintptr_t id) {
  return (id >> 4) ^ (id >> 16);
}
}  // namespace

void RdmaDirectState::close() {
  gate.fetch_or(kClosed, std::memory_order_acq_rel);
  while ((gate.load(std::memory_order_acquire) & ~kClosed) != 0) {
    std::this_thread::yield();
  }
}

RdmaDirectLease::RdmaDirectLease(RdmaDirectState* state) {
  uint64_t gate = state->gate.load(std::memory_order_relaxed);
  do {
    if (gate & RdmaDirectState::kClosed) {
      return;
    }
  } while (!state->gate.compare_exchange_weak(gate, gate + 1, std::memory_order_acquire,
                                              std::memory_order_relaxed));
  const void* me = caller_identity();
  const void* owner = state->owner.load(std::memory_order_relaxed);
  if (owner == nullptr) {
    state->owner.compare_exchange_strong(owner, me, std::memory_order_relaxed);
    owner = state->owner.load(std::memory_order_relaxed);
  }
  if (owner != me) {
    state->gate.fetch_sub(1, std::memory_order_release);
    return;
  }
  state_ = state;
}
RdmaDirectLease::~RdmaDirectLease() {
  if (state_) {
    state_->gate.fetch_sub(1, std::memory_order_release);
  }
}

RdmaDirectState* RdmaEngine::find_direct(uintptr_t id) const {
  const size_t start = direct_hash(id) & (kDirectConnections - 1);
  for (size_t offset = 0; offset < kDirectConnections; ++offset) {
    auto* state = direct_connections_[(start + offset) & (kDirectConnections - 1)].load(
        std::memory_order_acquire);
    if (!state || state->connection_id == id) {
      return state;
    }
  }
  return nullptr;
}

bool RdmaEngine::publish_direct(rdma_thread_params* params) {
  auto state = std::make_unique<RdmaDirectState>();
  state->connection_id = reinterpret_cast<uintptr_t>(params->client_id);
  state->params = params;
  const auto& intf = cfg_.ifs_[params->if_idx];
  state->counters = metrics::get_or_create_queue(
      "rdma", intf.name_.empty() ? intf.address_ : intf.name_, intf.port_id_,
      std::to_string(intf.tx_.queues_[params->queue_idx].common_.id_));
  state->regions.reserve(mrs_.size());
  for (auto& [name, region] : mrs_) {
    auto registration = region.ctx_mr_map_.find(params->pd);
    if (registration == region.ctx_mr_map_.end()) {
      continue;
    }
    auto pool = mem_pools_.find(name);
    state->regions.push_back({name, &region, pool == mem_pools_.end() ? nullptr : pool->second,
                              registration->second->lkey});
  }
  state->send.sg_list = &state->sge;
  state->send.num_sge = 1;
  state->send.opcode = IBV_WR_SEND;
  state->receive.sg_list = &state->sge;
  state->receive.num_sge = 1;
  const size_t start = direct_hash(state->connection_id) & (kDirectConnections - 1);
  for (size_t offset = 0; offset < kDirectConnections; ++offset) {
    auto& entry = direct_connections_[(start + offset) & (kDirectConnections - 1)];
    auto* old = entry.load(std::memory_order_relaxed);
    if (!old || old->connection_id == state->connection_id ||
        (old->gate.load(std::memory_order_acquire) & RdmaDirectState::kClosed)) {
      params->direct_state = state.get();
      direct_states_.push_back(std::move(state));
      entry.store(params->direct_state, std::memory_order_release);
      return true;
    }
  }
  DAQIRI_LOG_ERROR("Direct RDMA connection table is full");
  return false;
}

const RdmaDirectMr* RdmaEngine::direct_region(RdmaDirectState& state, BurstParams* burst) {
  auto& cache = rdma_burst_cache(burst);
  if (cache.magic == RdmaBurstCache::kMagic && cache.engine == this && cache.state == &state &&
      cache.region && cache.region->name == burst->transport_hdr.local_mr_name) {
    return cache.region;
  }
  for (const auto& region : state.regions) {
    if (region.name == burst->transport_hdr.local_mr_name) {
      const bool scheduled =
          cache.magic == RdmaBurstCache::kMagic && cache.engine == this && cache.scheduled;
      cache = {RdmaBurstCache::kMagic, this, &state, &region, scheduled};
      return &region;
    }
  }
  return nullptr;
}

Ring* RdmaEngine::burst_pool(BurstParams* burst) {
  const auto& cache = rdma_burst_cache(burst);
  if (cache.magic == RdmaBurstCache::kMagic && cache.engine == this && cache.region &&
      cache.region->name == burst->transport_hdr.local_mr_name) {
    return cache.region->pool;
  }
  auto pool = mem_pools_.find(burst->transport_hdr.local_mr_name);
  return pool == mem_pools_.end() ? nullptr : pool->second;
}

Status RdmaEngine::send_direct(RdmaDirectState& state, BurstParams* burst) {
  auto& params = *state.params;
  if (rdma_force_quit.load(std::memory_order_relaxed) || params.ready_to_exit.load()) {
    return Status::NOT_READY;
  }
  if (burst->transport_hdr.num_pkts != 1 || burst->transport_hdr.num_segs != 1 || !burst->pkts[0] ||
      !burst->pkt_lens[0]) {
    return Status::INVALID_PARAMETER;
  }
  const auto* region = direct_region(state, burst);
  if (!region) {
    return Status::INVALID_PARAMETER;
  }
  state.sge.addr = reinterpret_cast<uintptr_t>(burst->pkts[0][0]);
  state.sge.length = burst->pkt_lens[0][0];
  state.sge.lkey = region->lkey;
  if (state.sge.length > region->region->params_.buf_size_) {
    return Status::INVALID_PARAMETER;
  }
  if (burst->transport_hdr.opcode == RDMAOpCode::SEND) {
    if (state.sends_posted - state.sends_completed == RdmaDirectState::kRequests) {
      return Status::NO_SPACE_AVAILABLE;
    }
    const uint64_t tag = state.sends_posted + 1;
    auto& slot = state.sends[(tag - 1) & (RdmaDirectState::kRequests - 1)];
    state.send.wr_id = tag;
    const bool signal =
        params.send_signal_every == 1 ||
        params.sends_since_signal + 1 >= static_cast<uint64_t>(params.send_signal_every) ||
        state.sends_posted - state.sends_completed >= 15;
    state.send.send_flags = signal ? IBV_SEND_SIGNALED : 0;
    const auto kind = region->region->params_.kind_;
    if ((kind == MemoryKind::HUGE || kind == MemoryKind::HOST_PINNED) &&
        state.sge.length <= params.max_inline_data) {
      state.send.send_flags |= IBV_SEND_INLINE;
    }
    ibv_send_wr* bad = nullptr;
    const uint64_t time = rdma_tx_time(burst, 0);
    const int status = time ? verbs_->post_timed_send(params.client_id->qp, &state.send, time)
                            : verbs_->post_send(params.client_id->qp, &state.send, &bad);
    if (status) {
      metrics::add_dropped(state.counters, "tx_post_failure", 1);
      return Status::GENERIC_FAILURE;
    }
    slot = {burst, tag};
    state.sends_posted = tag;
    params.sends_since_signal = signal ? 0 : params.sends_since_signal + 1;
    if (state.send.send_flags & IBV_SEND_INLINE) {
      // This connection has one writer; atomic publication is only for stats readers.
      params.inline_wqes.store(params.inline_wqes.load(std::memory_order_relaxed) + 1,
                               std::memory_order_relaxed);
    }
  } else if (burst->transport_hdr.opcode == RDMAOpCode::RECEIVE) {
    const uint64_t tag = state.receives_posted + 1;
    auto& slot = state.receives[(tag - 1) & (RdmaDirectState::kRequests - 1)];
    if (slot.burst) {
      return Status::NO_SPACE_AVAILABLE;
    }
    state.receive.wr_id = tag;
    ibv_recv_wr* bad = nullptr;
    if (verbs_->post_recv(params.client_id->qp, &state.receive, &bad)) {
      metrics::add_dropped(state.counters, "rx_post_failure", 1);
      return Status::GENERIC_FAILURE;
    }
    slot = {burst, tag};
    state.receives_posted = tag;
    ++state.receive_count;
  } else {
    return Status::NOT_SUPPORTED;
  }
  return Status::SUCCESS;
}

Status RdmaEngine::poll_direct(RdmaDirectState& state, bool tx, bool server) {
  ibv_wc wc{};
  auto& params = *state.params;
  const int count = verbs_->poll_cq(tx ? params.qp_params.tx_cq : params.qp_params.rx_cq, 1, &wc);
  if (count <= 0) {
    return count == 0 ? Status::NOT_READY : Status::GENERIC_FAILURE;
  }
  const Status status = wc.status == IBV_WC_SUCCESS ? Status::SUCCESS : Status::GENERIC_FAILURE;
  auto deliver = [&](RdmaDirectState::Request& slot, bool transmit) {
    auto* burst = slot.burst;
    slot.burst = nullptr;
    burst->transport_hdr.status = status;
    burst->transport_hdr.tx = transmit;
    burst->transport_hdr.server = server;
    if (status == Status::SUCCESS) {
      if (transmit) {
        metrics::add_tx(state.counters, 1, burst->pkt_lens[0][0]);
      } else {
        burst->pkt_lens[0][0] = wc.byte_len;
        metrics::add_rx(state.counters, 1, wc.byte_len);
      }
    } else {
      metrics::add_dropped(state.counters, transmit ? "tx_completion_error" : "rx_completion_error",
                           1);
    }
    state.ready[state.ready_tail++ & (RdmaDirectState::kReady - 1)] = burst;
  };
  if (tx) {
    if (wc.wr_id <= state.sends_completed || wc.wr_id > state.sends_posted) {
      return Status::GENERIC_FAILURE;
    }
    // A signaled CQE also completes preceding unsignaled SENDs, using internal
    // posting order rather than ordering assumptions about application WR IDs.
    if (wc.wr_id - state.sends_completed >
        RdmaDirectState::kReady - (state.ready_tail - state.ready_head)) {
      return Status::GENERIC_FAILURE;
    }
    while (state.sends_completed < wc.wr_id) {
      auto& slot = state.sends[state.sends_completed & (RdmaDirectState::kRequests - 1)];
      if (!slot.burst || slot.tag != state.sends_completed + 1) {
        return Status::GENERIC_FAILURE;
      }
      deliver(slot, true);
      ++state.sends_completed;
    }
  } else {
    auto& slot = state.receives[(wc.wr_id - 1) & (RdmaDirectState::kRequests - 1)];
    if (!wc.wr_id || !slot.burst || slot.tag != wc.wr_id ||
        state.ready_tail - state.ready_head == RdmaDirectState::kReady) {
      return Status::GENERIC_FAILURE;
    }
    deliver(slot, false);
    --state.receive_count;
  }
  return Status::SUCCESS;
}

Status RdmaEngine::get_direct(RdmaDirectState& state, BurstParams** burst, bool server) {
  if (rdma_force_quit.load(std::memory_order_relaxed) || state.params->ready_to_exit.load()) {
    return Status::NOT_READY;
  }
  // Drain decoded completions first. Alternate CQ priority on each refill so
  // a continuously ready direction cannot starve the other. Inspect each needed
  // CQ at most once, stopping as soon as a completion is available.
  if (state.ready_head == state.ready_tail) {
    const bool tx_first = state.poll_tx_first;
    state.poll_tx_first = !tx_first;
    for (bool tx : {tx_first, !tx_first}) {
      if (tx ? state.sends_completed == state.sends_posted : state.receive_count == 0) {
        continue;
      }
      const auto status = poll_direct(state, tx, server);
      if (status == Status::GENERIC_FAILURE) {
        return status;
      }
      if (state.ready_head != state.ready_tail) {
        break;
      }
    }
    if (state.ready_head == state.ready_tail) {
      return Status::NOT_READY;
    }
  }
  *burst = state.ready[state.ready_head++ & (RdmaDirectState::kReady - 1)];
  return Status::SUCCESS;
}
}  // namespace daqiri
