// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <infiniband/verbs.h>
#include <daqiri/types.h>

namespace daqiri {
class RdmaEngine;
class Ring;
struct rdma_thread_params;
struct rdma_mr_params;
namespace metrics {
struct CounterSet;
}

struct RdmaDirectMr {
  std::string name;
  rdma_mr_params* region = nullptr;
  Ring* pool = nullptr;
  uint32_t lkey = 0;
};

// A connection incarnation is retained until engine destruction. Closing the
// gate prevents new readers and lets CM teardown wait for the current reader;
// a reused server slot cannot invalidate an old cached handle.
struct RdmaDirectState {
  static constexpr uint32_t kRequests = 1024;
  static constexpr uint32_t kReady = 2 * kRequests;
  static constexpr uint64_t kClosed = uint64_t{1} << 63;
  struct Request {
    BurstParams* burst = nullptr;
    uint64_t tag = 0;
  };
  uintptr_t connection_id = 0;
  rdma_thread_params* params = nullptr;
  std::atomic<uint64_t> gate{0};
  std::atomic<const void*> owner{nullptr};
  std::vector<RdmaDirectMr> regions;
  std::shared_ptr<metrics::CounterSet> counters;
  std::array<Request, kRequests> sends{};
  std::array<Request, kRequests> receives{};
  uint64_t sends_posted = 0;
  uint64_t sends_completed = 0;
  uint64_t receives_posted = 0;
  uint32_t receive_count = 0;
  bool poll_tx_first = false;
  std::array<BurstParams*, kReady> ready{};
  uint64_t ready_head = 0;
  uint64_t ready_tail = 0;
  ibv_sge sge{};
  ibv_send_wr send{};
  ibv_recv_wr receive{};

  void close();
};

// Engine-private cache in the unused tail of the existing 256-byte header.
// The public transport header, packet pointers, and application WR ID retain
// their existing layout and meaning.
struct RdmaBurstCache {
  static constexpr uint64_t kMagic = 0x52444d4143414348ULL;
  uint64_t magic = 0;
  const RdmaEngine* engine = nullptr;
  RdmaDirectState* state = nullptr;
  const RdmaDirectMr* region = nullptr;
};
static_assert(sizeof(BurstTransportHeader) <=
              ADV_NETWORK_HEADER_SIZE_BYTES - sizeof(RdmaBurstCache));
inline RdmaBurstCache& rdma_burst_cache(BurstParams* burst) {
  return *reinterpret_cast<RdmaBurstCache*>(reinterpret_cast<uint8_t*>(burst) +
                                            ADV_NETWORK_HEADER_SIZE_BYTES - sizeof(RdmaBurstCache));
}

class RdmaDirectLease {
 public:
  explicit RdmaDirectLease(RdmaDirectState* state);
  ~RdmaDirectLease();
  RdmaDirectLease(const RdmaDirectLease&) = delete;
  RdmaDirectLease& operator=(const RdmaDirectLease&) = delete;
  explicit operator bool() const {
    return state_ != nullptr;
  }

 private:
  RdmaDirectState* state_ = nullptr;
};
}  // namespace daqiri
