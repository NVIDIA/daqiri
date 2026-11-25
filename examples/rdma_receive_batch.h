// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <daqiri/daqiri.h>

namespace daqiri::bench {

struct RdmaReceiveBatch {
  size_t completed = 0;
  size_t reposted = 0;
  size_t valid = 0;
};

// A transport-only receiver finishes reading each completion in this call.
// Workload examples use the regular API so GPU events retain buffer ownership.
inline Status recycle_rdma_receives(uintptr_t connection, uint32_t bytes, bool repost,
                                    RdmaReceiveBatch& result) {
  std::array<BurstParams*, 32> bursts;
  const auto status =
      rdma_poll_receive_batch(connection, bursts.data(), bursts.size(), &result.completed);
  if (!result.completed) return status;

  size_t recyclable = 0;
  for (size_t i = 0; i < result.completed; ++i) {
    auto* burst = bursts[i];
    if (burst->transport_hdr.status == Status::SUCCESS &&
        burst->transport_hdr.opcode == RDMAOpCode::RECEIVE && burst->pkt_lens[0][0] == bytes) {
      ++result.valid;
      if (repost) {
        bursts[recyclable++] = burst;
        continue;
      }
    }
    free_tx_burst(burst);
  }
  if (!recyclable) return status;
  const auto post_status =
      rdma_repost_receive_batch(connection, bursts.data(), recyclable, &result.reposted);
  for (size_t i = result.reposted; i < recyclable; ++i) free_tx_burst(bursts[i]);
  return status == Status::SUCCESS ? post_status : status;
}
}  // namespace daqiri::bench
