// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <iostream>
#include <stdexcept>
#include "src/engines/ibverbs/daqiri_ibverbs_engine.h"
#include "src/rdma/raw_api.h"

namespace daqiri {
namespace {
void require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
}  // namespace

class IbverbsEngineRxTestPeer {
 public:
  static void geometry() {
    IbverbsEngine engine;
    detail::RawVerbsApi verbs{};
    verbs.dv_devx_alloc_uar = [](ibv_context*, uint32_t) -> mlx5dv_devx_uar* { return nullptr; };
    engine.verbs_ = &verbs;
    MemoryRegionConfig discard{};
    discard.kind_ = MemoryKind::NULL_MR;
    engine.cfg_.mrs_["discard"] = discard;
    IbvRxQueue q;
    q.mr_names = {"discard"};
    q.regions.resize(1);
    q.batch_size = 513;
    require(engine.create_regular_rq(q) == Status::GENERIC_FAILURE,
            "geometry test unexpectedly reached hardware");
    require(q.num_wqe == 1024, "discard-only RQ cannot fill a non-power-of-two burst");
    q.batch_size = 32769;
    require(engine.create_regular_rq(q) == Status::INVALID_PARAMETER,
            "accepted a zero-timeout burst larger than the RQ limit");
    engine.cfg_.mrs_["discard"].kind_ = MemoryKind::HOST;
    engine.cfg_.mrs_["discard"].num_bufs_ = 64;
    q.batch_size = 128;
    require(engine.create_regular_rq(q) == Status::INVALID_PARAMETER,
            "accepted a burst larger than retained storage");
    q.timeout_us = 1;
    require(engine.create_regular_rq(q) == Status::GENERIC_FAILURE && q.num_wqe == 64,
            "partial-burst timeout did not allow a smaller RQ");
    q.timeout_us = 0;
    q.poll_mode = QueuePollMode::DIRECT;
    require(engine.create_regular_rq(q) == Status::GENERIC_FAILURE,
            "direct polling must allow partial bursts without a timeout");
    engine.verbs_ = nullptr;
  }

  static void failed_add_cleanup() {
    IbverbsEngine engine;
    engine.cfg_.ifs_.resize(2);
    RxQueueConfig original_rx{};
    TxQueueConfig original_tx{};
    original_rx.common_.id_ = original_tx.common_.id_ = 0;
    engine.cfg_.ifs_[0].rx_.queues_ = {original_rx};
    engine.cfg_.ifs_[0].tx_.queues_ = {original_tx};
    InterfaceConfig interface{};
    interface.port_id_ = 1;
    RxQueueConfig rx{};
    TxQueueConfig tx{};
    rx.common_.id_ = tx.common_.id_ = 2;
    MemoryRegionConfig discard{};
    discard.kind_ = MemoryKind::NULL_MR;
    engine.cfg_.mrs_["discard"] = discard;
    rx.common_.mrs_ = {"discard", "discard"};
    tx.common_.mrs_ = {"discard"};
    IbvRxQueue rq;
    IbvTxQueue tq;
    require(engine.setup_rx_queue(rq, interface, rx) == Status::INVALID_PARAMETER,
            "invalid RX configuration accepted");
    require(engine.setup_tx_queue(tq, interface, tx) == Status::INVALID_PARAMETER,
            "invalid TX configuration accepted");
    require(rq.port_id == 1 && rq.queue_id == 2 && tq.port_id == 1 && tq.queue_id == 2,
            "failed setup did not retain its own identity");
    engine.destroy_rx_queue(rq);
    engine.destroy_tx_queue(tq);
    IbvRxQueue never_started_rx;
    IbvTxQueue never_started_tx;
    engine.destroy_rx_queue(never_started_rx);
    engine.destroy_tx_queue(never_started_tx);
    require(
        engine.cfg_.ifs_[0].rx_.queues_.size() == 1 && engine.cfg_.ifs_[0].tx_.queues_.size() == 1,
        "failed add cleanup erased interface 0 queue 0");
  }

  static void error_credits(bool direct, bool error_first) {
    IbverbsEngine engine;
    engine.max_batch_ = 8;
    engine.cfg_.common_.master_core_ = -1;
    engine.cfg_.rx_meta_buffers_ = engine.cfg_.tx_meta_buffers_ = 8;
    require(engine.initialize_burst_pools(), "metadata pools failed");
    IbvRxQueue q;
    q.striding = false;
    q.poll_mode = direct ? QueuePollMode::DIRECT : QueuePollMode::INDIRECT;
    if (!direct) {
      q.ring = Ring::create("rx_error_test", 8, RingMode::SPSC);
      require(q.ring != nullptr, "RX handoff ring failed");
    }
    q.num_wqe = 4;
    q.strides_per_wqe = 1;
    q.batch_size = 4;
    q.num_segs = 1;
    q.rq_pi = 4;
    q.freed_strides = std::make_unique<std::atomic<uint32_t>[]>(4);
    for (size_t i = 0; i < 4; ++i) q.freed_strides[i].store(0);
    std::array<uint8_t, 256> data{};
    q.regions.push_back({data.data(), 123, 64, 16});
    alignas(64) std::array<mlx5_cqe64, 8> cq{};
    for (auto& cqe : cq) cqe.op_own = MLX5_CQE_INVALID << 4;
    uint32_t cq_db = 0;
    std::array<uint32_t, 2> rq_db{};
    q.rq_dbr = rq_db.data();
    q.dv_cq.buf = cq.data();
    q.dv_cq.cqe_cnt = cq.size();
    q.dv_cq.cqe_size = sizeof(mlx5_cqe64);
    q.dv_cq.dbrec = &cq_db;
    const uint16_t error_slot = error_first ? 0 : 1;
    const uint16_t packet_slot = error_first ? 1 : 0;
    auto* error = reinterpret_cast<mlx5_err_cqe*>(&cq[error_slot]);
    error->syndrome = MLX5_CQE_SYNDROME_LOCAL_LENGTH_ERR;
    error->wqe_counter = htobe16(error_slot);
    error->op_own = MLX5_CQE_RESP_ERR << 4;
    cq[packet_slot].op_own = MLX5_CQE_RESP_SEND << 4;
    cq[packet_slot].wqe_counter = htobe16(packet_slot);
    cq[packet_slot].byte_cnt = htobe32(16);
    BurstParams* burst = nullptr;
    require(engine.rx_poll_queue(&q, direct ? &burst : nullptr) == Status::SUCCESS,
            "an error prevented a valid packet from being polled");
    if (!direct) {
      require(q.ring->dequeue(reinterpret_cast<void**>(&burst)),
              "error did not publish a zero-timeout partial batch");
    }
    require(burst != nullptr, "valid packet was not returned");
    require(q.dbg_err == 1 && q.rq_pi == (error_first ? 5U : 4U),
            "error WQE credit did not respect cyclic refill order");
    require(
        q.outstanding_strides.load() == 1 && burst->pkts[0][0] == data.data() + 64 * packet_slot,
        "error changed ownership of the successful packet");
    require(q.cq_ci == 2 && be32toh(cq_db) == 2, "CQ credits were not published");
    engine.release_strides(q, packet_slot, 1);
    engine.devx_advance_producer(q);
    require(q.rq_pi == 6 && be32toh(rq_db[0]) == 6,
            "freeing the valid packet did not repost both consumed WQEs");
    engine.rx_meta_pool_->put(burst);
    for (uint16_t i = 2; i < 4; ++i) {
      auto* failed = reinterpret_cast<mlx5_err_cqe*>(&cq[i]);
      failed->syndrome = MLX5_CQE_SYNDROME_LOCAL_LENGTH_ERR;
      failed->wqe_counter = htobe16(i);
      failed->op_own = MLX5_CQE_RESP_ERR << 4;
    }
    burst = nullptr;
    require(engine.rx_poll_queue(&q, direct ? &burst : nullptr) ==
                    (direct ? Status::NOT_READY : Status::SUCCESS) &&
                burst == nullptr,
            "error-only completions returned a packet");
    if (!direct) require(q.ring->count() == 0, "error-only completions published a burst");
    require(q.rq_pi == 8 && q.outstanding_strides.load() == 0,
            "consecutive errors leaked credits or underflowed caller ownership");
    engine.destroy_rx_queue(q);
  }
};
}  // namespace daqiri

int main() {
  try {
    daqiri::IbverbsEngineRxTestPeer::geometry();
    daqiri::IbverbsEngineRxTestPeer::failed_add_cleanup();
    for (bool direct : {false, true}) {
      for (bool error_first : {false, true}) {
        daqiri::IbverbsEngineRxTestPeer::error_credits(direct, error_first);
      }
    }
    std::cout << "PASS: RX capacity, failed-add cleanup, and error credit recovery\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
