// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <daqiri/daqiri.h>
#include <endian.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <time.h>
#include <utility>

namespace daqiri::bench {

// Stop-and-wait RC over caller-owned direct queues. A completed SEND returns
// the same storage for the next probe. Device payloads stay opaque.
class RdmaPingPong {
 public:
  RdmaPingPong(uintptr_t connection, bool server, int bytes, int receive_depth, bool opaque,
               uint64_t timeout_ns, const std::string& tx_region, const std::string& rx_region)
      : connection_(connection),
        server_(server),
        bytes_(bytes),
        opaque_(opaque),
        timeout_ns_(timeout_ns),
        tx_(allocate(RDMAOpCode::SEND, tx_region), free_tx_burst),
        pending_rx_(nullptr, free_tx_burst) {
    if (!opaque_ && bytes_ < static_cast<int>(sizeof(uint64_t)))
      throw std::runtime_error("RoCE latency probes need room for a token");
    for (int i = 0; i < receive_depth; ++i) {
      Owner receive(allocate(RDMAOpCode::RECEIVE, rx_region), free_tx_burst);
      post(receive);
    }
  }

  uint64_t probe(bool origin, uint64_t sequence, bool repost) {
    const auto token = htobe64(sequence);
    if (!opaque_) std::memcpy(tx_->pkts[0][0], &token, sizeof(token));
    const auto deadline = now() + timeout_ns_;
    uint64_t start = 0;
    if (origin) {
      start = now();
      post(tx_);
    }
    Owner receive = next(deadline, false);
    const uint64_t finish = origin ? now() : 0;
    if (!origin) post(tx_);

    if (receive->transport_hdr.opcode != RDMAOpCode::RECEIVE ||
        receive->pkt_lens[0][0] != static_cast<uint32_t>(bytes_) ||
        (!opaque_ && std::memcmp(receive->pkts[0][0], &token, sizeof(token)) != 0)) {
      throw std::runtime_error("RoCE ping-pong received a mismatched probe");
    }
    if (repost) post(receive);
    if (!tx_) tx_ = next(deadline, true);
    return origin ? finish - start : 0;
  }

 private:
  using Owner = std::unique_ptr<BurstParams, decltype(&free_tx_burst)>;

  static uint64_t now() {
    timespec stamp{};
    clock_gettime(CLOCK_MONOTONIC, &stamp);
    return static_cast<uint64_t>(stamp.tv_sec) * 1000000000 + stamp.tv_nsec;
  }

  BurstParams* allocate(RDMAOpCode op, const std::string& region) {
    auto* burst = create_tx_burst_params();
    if (!burst) throw std::runtime_error("RoCE ping-pong metadata unavailable");
    if (rdma_set_header(burst, op, connection_, server_, 1, 0, region) != Status::SUCCESS ||
        get_tx_packet_burst(burst) != Status::SUCCESS) {
      free_tx_metadata(burst);
      throw std::runtime_error("RoCE ping-pong buffer allocation failed");
    }
    burst->pkt_lens[0][0] = bytes_;
    return burst;
  }

  static void post(Owner& burst) {
    if (send_tx_burst(burst.get()) != Status::SUCCESS)
      throw std::runtime_error("RoCE ping-pong post failed");
    burst.release();
  }

  Owner next(uint64_t deadline, bool send) {
    if (!send && pending_rx_) return std::move(pending_rx_);
    // A clock read every 64 empty polls catches a stalled peer without adding
    // a clock read to each iteration of the timed completion poll.
    do {
      for (int i = 0; i < 64; ++i) {
        BurstParams* burst = nullptr;
        const auto status = get_rx_burst(&burst, connection_, server_);
        if (status == Status::NOT_READY) continue;
        Owner completion(burst, free_tx_burst);
        if (status != Status::SUCCESS || !burst || burst->transport_hdr.status != Status::SUCCESS) {
          throw std::runtime_error("RoCE ping-pong completion failed");
        }
        if (burst->transport_hdr.tx == send) return completion;
        if (!send && !tx_ && burst->transport_hdr.opcode == RDMAOpCode::SEND) {
          tx_ = std::move(completion);
          continue;
        }
        // After an echo, the next request can arrive before the local SEND
        // CQE. Keep its buffer caller-owned until the next reply is prepared.
        if (send && !pending_rx_ && burst->transport_hdr.opcode == RDMAOpCode::RECEIVE) {
          pending_rx_ = std::move(completion);
          continue;
        }
        throw std::runtime_error("RoCE ping-pong received an unexpected completion");
      }
    } while (now() < deadline);
    throw std::runtime_error("RoCE ping-pong timed out");
  }

  uintptr_t connection_;
  bool server_;
  int bytes_;
  bool opaque_;
  uint64_t timeout_ns_;
  Owner tx_;
  Owner pending_rx_;
};
}  // namespace daqiri::bench
