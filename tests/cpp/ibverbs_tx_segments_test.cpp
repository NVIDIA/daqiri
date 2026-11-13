// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <cerrno>
#include <cstring>
#include <endian.h>
#include <iostream>
#include <stdexcept>
#include <string>

#include "src/engines/ibverbs/daqiri_ibverbs_engine.h"
#include "src/rdma/raw_api.h"

namespace daqiri {
namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}  // namespace

// Exercise the production WQE builders and completion poller against host SQ,
// CQ, and doorbell memory. No NIC, GPU, CUDA context, or verbs calls are needed.
class IbverbsEngineTxTestPeer {
 public:
  static void tx_timestamps(int segments, bool endpoint, bool direct, bool scheduled, bool realtime,
                            bool gpu = false) {
    IbverbsEngine engine;
    engine.max_batch_ = 4;
    engine.cfg_.rx_meta_buffers_ = engine.cfg_.tx_meta_buffers_ = 8;
    require(engine.initialize_burst_pools(), "timestamp metadata setup failed");
    detail::RawVerbsApi verbs{};
    verbs.dv_get_clock_info = [](ibv_context*, mlx5dv_clock_info*) { return 0; };
    verbs.dv_ts_to_ns = [](mlx5dv_clock_info*, uint64_t ticks) { return ticks + 1000; };
    engine.verbs_ = &verbs;
    IbvTxQueue q;
    q.port_id = q.queue_id = 0;
    q.state = ResourceState::ACTIVE;
    q.poll_mode = direct ? QueuePollMode::DIRECT : QueuePollMode::INDIRECT;
    q.accurate_send = realtime;
    q.send_scheduling = scheduled;
    q.empw_enabled = true;  // Requests must bypass packing, even on a capable NIC.
    q.num_slots = 8;
    q.num_segs = segments;
    q.send_ring = Ring::create("timestamp_test", 8, RingMode::SPSC);
    engine.tx_queue_table_[0][0].store(&q);
    alignas(64) std::array<uint8_t, 32 * 64> sq{};
    alignas(64) std::array<uint8_t, 512> bf{};
    alignas(64) std::array<mlx5_cqe64, 8> cq{};
    std::array<uint32_t, 2> sq_db{};
    uint32_t cq_db = 0;
    for (auto& cqe : cq) cqe.op_own = MLX5_CQE_INVALID << 4;
    q.dv_qp.sq.buf = sq.data();
    q.dv_qp.sq.wqe_cnt = 32;
    q.dv_qp.sq.stride = 64;
    q.dv_qp.dbrec = sq_db.data();
    q.dv_qp.bf.reg = bf.data();
    q.dv_qp.bf.size = 256;
    if (gpu) {
      // Identity-address mock mappings; the production descriptor builder and
      // credit/completion logic run without registering CUDA or touching a NIC.
      q.gpu_uar_mapping = std::make_shared<GpuTxMapping>();
      q.gpu_record_mapping = std::make_shared<GpuTxMapping>();
    }
    q.dv_txcq.buf = cq.data();
    q.dv_txcq.cqe_cnt = cq.size();
    q.dv_txcq.cqe_size = sizeof(mlx5_cqe64);
    q.dv_txcq.dbrec = &cq_db;
    q.sq_capacity_wqebbs = 32;
    q.sq_pi = q.sq_completed = 65534;  // Exercise SQ, CQ, and 16-bit counter wraps.
    q.wqe_slot_cum.resize(32);
    q.wqe_wqebb_cum.resize(32);
    q.wqe_timestamp_ids.resize(32);
    std::array<std::array<uint8_t, 8 * 128>, MAX_NUM_SEGS> storage{};
    for (int s = 0; s < segments; ++s)
      q.regions.push_back({storage[s].data(), static_cast<uint32_t>(0x100 + s), 128});
    auto allocate = [&]() {
      auto* burst = engine.create_tx_burst_params();
      require(burst != nullptr, "timestamp burst allocation failed");
      burst->hdr.hdr.port_id = burst->hdr.hdr.q_id = 0;
      burst->hdr.hdr.num_pkts = direct ? 1 : 3;
      require(engine.get_tx_packet_burst(burst) == Status::SUCCESS, "packet allocation failed");
      for (int s = 0; s < segments; ++s)
        for (size_t i = 0; i < burst->hdr.hdr.num_pkts; ++i) burst->pkt_lens[s][i] = 42;
      return burst;
    };
    auto inject_completion = [&](uint64_t wqe, uint64_t raw, bool error = false) {
      const auto ci = q.tx_cq_ci;
      auto& cqe = cq[ci & 7];
      cqe.op_own = ((error ? MLX5_CQE_REQ_ERR : MLX5_CQE_REQ) << 4) | ((ci & 8) ? 1 : 0);
      cqe.wqe_counter = htobe16(static_cast<uint16_t>(wqe));
      cqe.timestamp = htobe64(raw);
    };
    for (unsigned iteration = 0; iteration < 40; ++iteration) {
      auto* burst = allocate();
      const int packets = burst->hdr.hdr.num_pkts;
      std::array<TxTimestampId, 3> ids{};
      uint64_t ns = 999;
      for (int i = 0; i < packets; ++i) {
        if (i == 1) continue;  // An untracked packet between two tracked sends.
        require(engine.request_packet_tx_timestamp(burst, i, &ids[i]) == Status::SUCCESS,
                "timestamp request failed");
        require(engine.get_packet_tx_timestamp(ids[i], &ns) == Status::NOT_READY && ns == 0,
                "unposted packet has a timestamp");
      }
      TxTimestampId duplicate = 999;
      require(engine.request_packet_tx_timestamp(burst, 0, &duplicate) == Status::ALREADY_EXISTS &&
                  duplicate == 0,
              "duplicate request accepted");
      if (scheduled)
        require(engine.set_packet_tx_time(burst, 0, 123456789) == Status::SUCCESS,
                "schedule assignment failed");
      if (endpoint) {
        IbverbsEngine::EndpointFastPath destination;
        destination.mtu = 1500;
        require(engine.prepare_endpoint_tx_burst(q, destination, burst) == Status::SUCCESS,
                "timestamp endpoint preparation failed");
      }
      const uint32_t blocks = endpoint ? (segments == 4 ? 3 : 2) : (segments > 2 ? 2 : 1);
      require(engine.tx_burst_wqebbs(q, burst) == blocks * packets + (scheduled ? 1 : 0),
              "timestamp requests did not bypass eMPW SQ accounting");
      const uint64_t first_send = q.sq_pi + (scheduled ? 1 : 0);
      // Release while in flight must not affect a later request or transmission.
      if (iteration == 1)
        require(engine.release_tx_timestamp(ids[0]) == Status::SUCCESS,
                "pending timestamp release failed");
      if (gpu) {
        const auto before_sq_db = sq_db;
        const auto before_bf = bf;
        GpuTxSubmission submission{};
        engine.post_tx_burst(q, burst, &submission);
        q.direct_pending = nullptr;
        engine.tx_meta_pool_->put(burst);
        require(sq_db == before_sq_db && bf == before_bf,
                "CPU published a GPU-owned packet before it was ready");
        require(submission.record == reinterpret_cast<uint64_t>(&sq_db[MLX5_SND_DBR]) &&
                    submission.doorbell ==
                        reinterpret_cast<uint64_t>(bf.data()) + (q.bf_offset ^ q.dv_qp.bf.size) &&
                    be32toh(submission.record_value) == (q.sq_pi & 0xffff),
                "incorrect GPU doorbell descriptor");
        const uint32_t words[2] = {htobe32(static_cast<uint32_t>(q.sq_pi << 8)),
                                   htobe32(q.sqn << 8)};
        uint64_t expected_db = 0;
        memcpy(&expected_db, words, sizeof(expected_db));
        require(submission.doorbell_value == expected_db, "wrong GPU doorbell WQE index/QPN");
        require(!engine.emit_direct_drain_nop(q) && engine.direct_tx_has_capacity(q, 2),
                "prepared GPU sends must block CPU publication but allow preparation ahead");
        const auto handle = gpu_tx_queue(submission);
        const auto ticket = gpu_tx_ticket(submission);
        require(handle.doorbell == reinterpret_cast<uint64_t>(bf.data()) &&
                    handle.record == submission.record &&
                    handle.doorbell + ticket.doorbell_offset == submission.doorbell &&
                    ticket.doorbell_value == submission.doorbell_value &&
                    ticket.record_value == submission.record_value,
                "cached GPU queue/ticket conversion failed");
        // Stand in for the GPU's ordered DBREC/MMIO stores, then inject its CQE.
        *reinterpret_cast<uint32_t*>(submission.record) = submission.record_value;
        *reinterpret_cast<uint64_t*>(submission.doorbell) = submission.doorbell_value;
      } else {
        require(engine.send_tx_burst(burst) == Status::SUCCESS, "timestamp send failed");
      }
      if (!direct) {
        void* raw = nullptr;
        require(q.send_ring->dequeue(&raw), "missing timestamp handoff");
        engine.post_tx_burst(q, static_cast<BurstParams*>(raw));
        engine.tx_meta_pool_->put(raw);
      }
      // The production send path has already recycled the burst metadata.
      for (int i = 0; i < packets; ++i) {
        if (i == 1) continue;
        const uint64_t wqe = first_send + blocks * i;
        const auto* ctrl = reinterpret_cast<mlx5_wqe_ctrl_seg*>(sq.data() + (wqe & 31) * 64);
        require((be32toh(ctrl->opmod_idx_opcode) & 0xff) == MLX5_OPCODE_SEND &&
                    ctrl->fm_ce_se == MLX5_WQE_CTRL_CQ_UPDATE,
                "timestamp packet did not get its own signaled SEND");
        const uint64_t raw = realtime ? (uint64_t{100} << 32) | (42 + i) : 100 + i;
        inject_completion(wqe, raw);
        if (!direct) engine.poll_tx_completions(q);
        const auto status = engine.get_packet_tx_timestamp(ids[i], &ns);
        if (iteration == 1 && i == 0) {
          // Explicitly drain a direct CQ when querying a released ID cannot poll it.
          if (direct) engine.poll_tx_completions(q);
          require(status == Status::INVALID_PARAMETER && ns == 0, "released ID resurrected");
        } else {
          require(status == Status::SUCCESS && ns == (realtime ? 100000000042ULL + i : 1100 + i),
                  "CQE timestamp conversion/correlation failed");
          uint64_t repeated = 0;
          require(engine.get_packet_tx_timestamp(ids[i], &repeated) == Status::SUCCESS &&
                      repeated == ns,
                  "timestamp read consumed the result");
          require(engine.release_tx_timestamp(ids[i]) == Status::SUCCESS, "release failed");
        }
      }
      require(q.completed_tail.load() == q.alloc_head, "timestamp sends leaked credits");
    }
    if (gpu) {
      auto* untracked = allocate();
      const auto send = q.sq_pi;
      GpuTxSubmission submission{};
      engine.post_tx_burst(q, untracked, &submission);
      q.direct_pending = nullptr;
      engine.tx_meta_pool_->put(untracked);
      const auto* ctrl = reinterpret_cast<mlx5_wqe_ctrl_seg*>(sq.data() + (send & 31) * 64);
      require(ctrl->fm_ce_se == MLX5_WQE_CTRL_CQ_UPDATE,
              "GPU submission without a timestamp must still signal completion");
      inject_completion(send, 123);
      engine.poll_tx_completions(q);
      require(engine.direct_tx_has_capacity(q, 2) && q.gpu_pending_wqebb <= q.sq_completed,
              "GPU completion did not unblock direct queue allocation");
      // Prepare a whole bounded window before any GPU publication/completion.
      const auto db_before = sq_db;
      const auto bf_before = bf;
      std::array<uint64_t, 8> sends{};
      for (size_t i = 0; i < sends.size(); ++i) {
        auto* ahead = allocate();
        sends[i] = q.sq_pi;
        engine.post_tx_burst(q, ahead, &submission);
        q.direct_pending = nullptr;
        engine.tx_meta_pool_->put(ahead);
      }
      require(sq_db == db_before && bf == bf_before && !engine.direct_tx_has_capacity(q, 2) &&
                  !engine.emit_direct_drain_nop(q),
              "prepared window published early or exceeded its packet credits");
      inject_completion(sends[0], 123);
      engine.poll_tx_completions(q);
      auto* cpu = allocate();
      const auto pi_before = q.sq_pi;
      require(engine.send_tx_burst(cpu) == Status::NOT_READY && q.direct_pending == cpu &&
                  q.sq_pi == pi_before && sq_db == db_before && bf == bf_before,
              "CPU SEND published an unfinished GPU window or consumed its caller's burst");
      engine.free_all_packets(cpu);
      engine.free_tx_burst(cpu);
      for (size_t i = 1; i < sends.size(); ++i) {
        inject_completion(sends[i], 123 + i);
        engine.poll_tx_completions(q);
      }
      require(q.alloc_head == q.completed_tail.load() && engine.direct_tx_has_capacity(q, 2),
              "prepared window did not restore packet/SQ credits");
    }
    // Cancellation, capacity exhaustion, and invalid arguments never yield success.
    auto* burst = allocate();
    TxTimestampId id = 999;
    require(engine.request_packet_tx_timestamp(nullptr, 0, &id) == Status::NULL_PTR && id == 0,
            "null burst accepted");
    require(
        engine.request_packet_tx_timestamp(burst, -1, &id) == Status::INVALID_PARAMETER && id == 0,
        "negative packet index accepted");
    require(engine.request_packet_tx_timestamp(burst, 0, nullptr) == Status::NULL_PTR,
            "null ID output accepted");
    require(engine.get_packet_tx_timestamp(0, nullptr) == Status::NULL_PTR,
            "null timestamp output accepted");
    if (!realtime) {
      verbs.dv_get_clock_info = [](ibv_context*, mlx5dv_clock_info*) { return EOPNOTSUPP; };
      require(engine.request_packet_tx_timestamp(burst, 0, &id) == Status::NOT_SUPPORTED,
              "unavailable clock accepted");
      verbs.dv_get_clock_info = [](ibv_context*, mlx5dv_clock_info*) { return 0; };
    }
    for (size_t i = 0; i < IbverbsEngine::kMaxTxTimestampRequests; ++i)
      engine.tx_timestamp_requests_.emplace(1000000 + i, IbverbsEngine::TxTimestampRequest{&q});
    require(
        engine.request_packet_tx_timestamp(burst, 0, &id) == Status::NO_SPACE_AVAILABLE && id == 0,
        "unbounded timestamp requests");
    engine.tx_timestamp_requests_.clear();
    require(engine.request_packet_tx_timestamp(burst, 0, &id) == Status::SUCCESS,
            "request capacity did not recover");
    engine.free_all_packets(burst);
    engine.free_tx_burst(burst);
    uint64_t ns = 999;
    require(engine.get_packet_tx_timestamp(id, &ns) == Status::GENERIC_FAILURE && ns == 0,
            "cancelled packet has a timestamp");
    require(engine.release_tx_timestamp(id) == Status::SUCCESS, "cancelled release failed");
    // A CQE error also fails requests which have not produced their CQEs yet.
    burst = allocate();
    require(engine.request_packet_tx_timestamp(burst, 0, &id) == Status::SUCCESS,
            "error-case request failed");
    engine.fail_queue_tx_timestamps(q);
    require(engine.get_packet_tx_timestamp(id, &ns) == Status::GENERIC_FAILURE,
            "queue removal left request pending");
    engine.free_all_packets(burst);
    engine.free_tx_burst(burst);
    require(engine.release_tx_timestamp(id) == Status::SUCCESS, "failed release failed");
    burst = allocate();
    require(engine.request_packet_tx_timestamp(burst, 0, &id) == Status::SUCCESS,
            "CQ-error request failed");
    const auto error_wqe = q.sq_pi;
    require(engine.send_tx_burst(burst) == Status::SUCCESS, "CQ-error send failed");
    if (!direct) {
      void* raw = nullptr;
      require(q.send_ring->dequeue(&raw), "missing CQ-error handoff");
      engine.post_tx_burst(q, static_cast<BurstParams*>(raw));
      engine.tx_meta_pool_->put(raw);
    }
    inject_completion(error_wqe, (uint64_t{100} << 32) | 42, true);
    engine.poll_tx_completions(q);
    require(engine.get_packet_tx_timestamp(id, &ns) == Status::GENERIC_FAILURE && ns == 0,
            "error CQE was reported as a successful transmit");
    require(engine.release_tx_timestamp(id) == Status::SUCCESS, "CQ-error release failed");
    engine.tx_queue_table_[0][0].store(nullptr);
    Ring::free(q.send_ring);
    q.send_ring = nullptr;
  }

  static void gathered_send(int segments, bool endpoint, bool direct, bool wait) {
    IbverbsEngine engine;
    IbvTxQueue q;
    q.port_id = q.queue_id = 0;
    constexpr uint32_t kBlocks = 16;
    constexpr uint32_t kBlockBytes = 64;
    alignas(64) std::array<uint8_t, kBlocks * kBlockBytes> sq;
    alignas(64) std::array<uint8_t, 512> bf{};
    alignas(64) std::array<mlx5_cqe64, 4> cq{};
    std::array<uint32_t, 2> sq_db{};
    uint32_t cq_db = 0;
    sq.fill(0xa5);
    for (auto& cqe : cq) cqe.op_own = MLX5_CQE_INVALID << 4;
    q.num_segs = segments;
    q.poll_mode = direct ? QueuePollMode::DIRECT : QueuePollMode::INDIRECT;
    q.sqn = 0x1234;
    // Cross both SQ wraparound and the CQE's 16-bit WQE counter wraparound.
    const uint64_t start = wait ? 65534 : 65535;
    q.sq_pi = q.sq_completed = start;
    q.sq_capacity_wqebbs = kBlocks;
    q.dv_qp.sq.buf = sq.data();
    q.dv_qp.sq.wqe_cnt = kBlocks;
    q.dv_qp.sq.stride = kBlockBytes;
    q.dv_qp.dbrec = sq_db.data();
    q.dv_qp.bf.reg = bf.data();
    q.dv_qp.bf.size = bf.size() / 2;
    q.dv_txcq.buf = cq.data();
    q.dv_txcq.cqe_cnt = cq.size();
    q.dv_txcq.cqe_size = sizeof(mlx5_cqe64);
    q.dv_txcq.dbrec = &cq_db;
    q.wqe_slot_cum.resize(kBlocks);
    q.wqe_wqebb_cum.resize(kBlocks);

    BurstParams burst{};
    burst.hdr.hdr.num_pkts = direct ? 1 : 2;
    burst.hdr.hdr.num_segs = segments;
    std::array<std::array<void*, 2>, MAX_NUM_SEGS> pointers{};
    std::array<std::array<uint32_t, 2>, MAX_NUM_SEGS> lengths{};
    uint64_t expected_bytes = 0;
    for (int s = 0; s < segments; ++s) {
      q.regions.push_back({nullptr, static_cast<uint32_t>(0x100 + s), 256});
      burst.pkts[s] = pointers[s].data();
      burst.pkt_lens[s] = lengths[s].data();
      for (size_t packet = 0; packet < burst.hdr.hdr.num_pkts; ++packet) {
        pointers[s][packet] = reinterpret_cast<void*>(0x100000 + s * 4096 + packet * 256);
        lengths[s][packet] = 42 + s * 7 + packet;
        expected_bytes += lengths[s][packet];
      }
    }
    if (endpoint) {
      IbverbsEngine::EndpointFastPath destination;
      destination.mtu = 1500;
      destination.header_template.ip.version = 4;
      destination.header_template.ip.ihl = 5;
      destination.header_template.ip.protocol = IPPROTO_UDP;
      destination.header_template.udp.dest = htobe16(12345);
      require(engine.prepare_endpoint_tx_burst(q, destination, &burst) == Status::SUCCESS,
              "endpoint preparation failed");
    }

    const uint32_t blocks_per_send = endpoint ? (segments == 4 ? 3 : 2) : (segments > 2 ? 2 : 1);
    const uint64_t expected_blocks = blocks_per_send * burst.hdr.hdr.num_pkts;
    require(engine.tx_burst_wqebbs(q, &burst) == expected_blocks, "incorrect SQ reservation");
    require(engine.get_burst_tot_byte(&burst) == expected_bytes,
            "segments missing from byte count");
    q.sq_capacity_wqebbs = expected_blocks;
    q.sq_completed = start - 1;
    require(!engine.tx_sq_has_space(q, expected_blocks),
            "accepted a burst with too few SQ credits");
    q.sq_capacity_wqebbs = kBlocks;
    q.sq_completed = start;
    if (wait) engine.emit_wait_wqe(q, 1'000'000'042ULL);
    engine.post_tx_burst(q, &burst);

    const uint64_t first_send = start + (wait ? 1 : 0);
    const uint64_t end = first_send + expected_blocks;
    require(q.sq_pi == end, "incorrect SQ producer progress");
    require(q.slots_posted == burst.hdr.hdr.num_pkts, "segments counted as packet slots");
    require(be32toh(sq_db[MLX5_SND_DBR]) == (end & 0xffff), "incorrect SQ doorbell");
    for (size_t packet = 0; packet < burst.hdr.hdr.num_pkts; ++packet) {
      const uint64_t wqe_start = first_send + packet * blocks_per_send;
      const uint32_t index = wqe_start & (kBlocks - 1);
      alignas(16) std::array<uint8_t, 192> wqe{};
      for (uint32_t block = 0; block < blocks_per_send; ++block) {
        std::memcpy(wqe.data() + block * kBlockBytes,
                    sq.data() + ((index + block) & (kBlocks - 1)) * kBlockBytes, kBlockBytes);
      }
      const auto* ctrl = reinterpret_cast<const mlx5_wqe_ctrl_seg*>(wqe.data());
      require((be32toh(ctrl->opmod_idx_opcode) & 0xff) == MLX5_OPCODE_SEND, "SEND overwritten");
      require(
          (be32toh(ctrl->qpn_ds) & 0x3f) == static_cast<uint32_t>(segments + (endpoint ? 5 : 2)),
          "incorrect encoded WQE length");
      const size_t data_offset = endpoint ? 80 : 32;
      const auto* data = reinterpret_cast<const mlx5_wqe_data_seg*>(wqe.data() + data_offset);
      for (int s = 0; s < segments; ++s) {
        require(be32toh(data[s].byte_count) == lengths[s][packet], "incorrect segment length");
        require(be32toh(data[s].lkey) == q.regions[s].lkey, "incorrect segment lkey");
        require(be64toh(data[s].addr) == reinterpret_cast<uint64_t>(pointers[s][packet]),
                "incorrect segment address/order");
      }
      if (endpoint) {
        UDPIPV4Pkt header;
        std::memcpy(&header, wqe.data() + 16 + offsetof(mlx5_wqe_eth_seg, inline_hdr_start),
                    sizeof(header));
        uint32_t payload = 0;
        for (int s = 0; s < segments; ++s) payload += lengths[s][packet];
        require(be16toh(header.udp.len) == payload + 8, "endpoint UDP length misses segments");
        require(be16toh(header.ip.tot_len) == payload + 28, "endpoint IP length misses segments");
        require(be16toh(header.udp.dest) == 12345, "endpoint header overwritten");
      }
      require(q.wqe_slot_cum[index] == packet + 1, "incorrect cumulative packet mapping");
      require(q.wqe_wqebb_cum[index] == wqe_start + blocks_per_send,
              "completion mapping misses WQE continuation blocks");
    }
    // A CQE for the final SEND must reclaim earlier unsignaled sends and WAITs.
    cq[0].op_own = 0;
    cq[0].wqe_counter = htobe16(static_cast<uint16_t>(end - blocks_per_send));
    engine.poll_tx_completions(q);
    require(q.completed_tail.load() == burst.hdr.hdr.num_pkts, "incorrect completed packet count");
    require(q.sq_completed == end, "completion failed to reclaim all SQ blocks");
    require(be32toh(cq_db) == 1, "incorrect CQ doorbell");
    engine.poll_tx_completions(q);
    require(q.completed_tail.load() == burst.hdr.hdr.num_pkts, "completion reclaimed twice");
  }

  static void shared_pool() {
    std::array<uint8_t, 256> storage{};
    detail::SharedTxPool pool;
    require(pool.initialize(storage.data(), 128, 64, 2, true, -1), "pool initialization failed");
    auto* first = pool.acquire();
    auto* second = pool.acquire();
    require(first && second && first != second && pool.acquire() == nullptr,
            "independent slot exhaustion failed");
    first->references.store(UINT32_MAX);
    require(!first->retain(), "reference count overflowed");
    first->references.store(1);
    require(first->retain(), "retain failed");
    first->release();
    require(pool.available() == 0, "retained slot recycled early");
    second->release();
    require(pool.available() == 1, "old retained slot blocked unrelated free slot");
    first->release();
    require(pool.available() == 2, "last reference did not recycle slot");
    // App cancellation and completion may race on different references to one
    // slot. The last release is the only publisher to the MPMC free pool.
    for (int i = 0; i < 500; ++i) {
      auto* buffer = pool.acquire();
      require(buffer && buffer->retain(), "concurrent release setup failed");
      std::thread worker([buffer] { buffer->release(); });
      buffer->release();
      worker.join();
      require(pool.available() == 2, "concurrent last release lost or duplicated slot");
    }
  }

  static void shared_lifetime(int segments, bool endpoint) {
    IbverbsEngine engine;
    engine.max_batch_ = 8;
    engine.cfg_.common_.master_core_ = 0;
    engine.cfg_.tx_meta_buffers_ = engine.cfg_.rx_meta_buffers_ = 16;
    require(engine.initialize_burst_pools(), "metadata setup failed");
    IbvTxQueue q;
    q.port_id = q.queue_id = 0;
    q.num_segs = segments;
    q.num_slots = 4;
    q.state = ResourceState::ACTIVE;
    q.shared_tx = std::make_unique<detail::SharedTxState>();
    q.accurate_send = true;
    q.shared_tx->ledger.resize(4);
    q.send_ring = Ring::create("shared_test", 8, RingMode::SPSC);
    engine.tx_queue_table_[0][0].store(&q);
    std::array<std::array<uint8_t, 4 * 128>, MAX_NUM_SEGS> storage{};
    for (int seg = 0; seg < segments; ++seg) {
      auto pool = std::make_unique<detail::SharedTxPool>();
      const unsigned count = seg == segments - 1 ? 1 : 4;
      require(pool->initialize(storage[seg].data(), 128, 128, count, true, -1),
              "pool setup failed");
      q.shared_tx->pools.push_back(std::move(pool));
      q.regions.push_back({storage[seg].data(), static_cast<uint32_t>(0x100 + seg), 128});
    }
    alignas(64) std::array<uint8_t, 32 * 64> sq{};
    alignas(64) std::array<uint8_t, 512> bf{};
    alignas(64) std::array<mlx5_cqe64, 8> cq{};
    std::array<uint32_t, 2> sq_db{};
    uint32_t cq_db = 0;
    for (auto& cqe : cq) cqe.op_own = MLX5_CQE_INVALID << 4;
    q.dv_qp.sq.buf = sq.data();
    q.dv_qp.sq.wqe_cnt = 32;
    q.dv_qp.sq.stride = 64;
    q.dv_qp.dbrec = sq_db.data();
    q.dv_qp.bf.reg = bf.data();
    q.dv_qp.bf.size = 256;
    q.dv_txcq.buf = cq.data();
    q.dv_txcq.cqe_cnt = 8;
    q.dv_txcq.cqe_size = sizeof(mlx5_cqe64);
    q.dv_txcq.dbrec = &cq_db;
    q.sq_capacity_wqebbs = 32;
    q.wqe_slot_cum.resize(32);
    q.wqe_wqebb_cum.resize(32);
    q.wqe_timestamp_ids.resize(32);
    IbverbsEngine::EndpointFastPath destination;
    destination.mtu = 1500;
    auto allocate = [&]() {
      auto* burst = engine.create_tx_burst_params();
      require(burst != nullptr, "metadata allocation failed");
      burst->hdr.hdr.port_id = 0;
      burst->hdr.hdr.q_id = 0;
      burst->hdr.hdr.num_pkts = 1;
      return burst;
    };
    auto post = [&](BurstParams* burst) {
      if (endpoint)
        require(engine.prepare_endpoint_tx_burst(q, destination, burst) == Status::SUCCESS,
                "endpoint preparation failed");
      require(engine.send_tx_burst(burst) == Status::SUCCESS, "shared submission failed");
      void* raw = nullptr;
      require(q.send_ring->dequeue(&raw) && raw == burst, "handoff ownership lost");
      engine.post_tx_burst(q, burst);
      engine.tx_meta_pool_->put(burst);  // Production worker recycles before completion.
      const uint32_t blocks = endpoint ? (segments == 4 ? 3 : 2) : (segments > 2 ? 2 : 1);
      return q.sq_pi - blocks;
    };
    auto complete = [&](uint64_t wqe) {
      const uint32_t ci = q.tx_cq_ci;
      cq[ci & 7].op_own = (ci & 8) ? 1 : 0;
      cq[ci & 7].wqe_counter = htobe16(static_cast<uint16_t>(wqe));
      cq[ci & 7].timestamp = htobe64((uint64_t{100} << 32) | 42);
      engine.poll_tx_completions(q);
    };
    const uint32_t mask = 1u << (segments - 1);
    // Retargeting must not let a cyclic queue consume shared storage or recycle
    // its metadata without releasing the source queue's references.
    {
      IbvTxQueue cyclic;
      cyclic.port_id = 0;
      cyclic.queue_id = 1;
      cyclic.num_segs = segments;
      cyclic.num_slots = 4;
      cyclic.state = ResourceState::ACTIVE;
      cyclic.send_ring = Ring::create("cyclic_test", 8, RingMode::SPSC);
      require(cyclic.send_ring != nullptr, "cyclic handoff setup failed");
      engine.tx_queue_table_[0][1].store(&cyclic);
      for (bool direct : {false, true}) {
        cyclic.poll_mode = direct ? QueuePollMode::DIRECT : QueuePollMode::INDIRECT;
        auto* original = allocate();
        require(engine.get_tx_packet_burst(original) == Status::SUCCESS,
                "wrong-queue source allocation failed");
        for (int seg = 0; seg < segments; ++seg) original->pkt_lens[seg][0] = 42;
        BurstParams* clone = nullptr;
        require(engine.clone_tx_burst(original, mask, &clone) == Status::SUCCESS,
                "wrong-queue clone failed");
        const auto metadata_available = engine.tx_meta_pool_->avail_count();
        for (auto* burst : {original, clone}) {
          set_header(burst, 0, 1, 1, segments);
          const auto flags = burst->hdr.hdr.burst_flags;
          std::array<void*, MAX_NUM_SEGS> pointers{};
          for (int seg = 0; seg < segments; ++seg) pointers[seg] = burst->pkts[seg][0];
          require(engine.send_tx_burst(burst) == Status::INVALID_PARAMETER,
                  "cyclic queue accepted shared storage");
          require(burst->hdr.hdr.burst_flags == flags &&
                      engine.tx_meta_pool_->avail_count() == metadata_available,
                  "wrong-queue rejection consumed ownership or metadata");
          for (int seg = 0; seg < segments; ++seg)
            require(burst->pkts[seg][0] == pointers[seg] && burst->pkt_lens[seg][0] == 42,
                    "wrong-queue rejection changed packet storage");
        }
        void* raw = nullptr;
        require(!cyclic.send_ring->dequeue(&raw) && cyclic.alloc_head == 0 &&
                    cyclic.slots_posted == 0 && cyclic.direct_pending == nullptr,
                "wrong-queue rejection changed cyclic queue state");
        require(!q.shared_tx->idle_storage() && q.alloc_head == 0,
                "wrong-queue rejection changed source ownership");
        engine.free_tx_burst(original);
        require(q.shared_tx->pools.back()->available() == 0,
                "cancelling source freed rejected clone's payload");
        engine.free_tx_burst(clone);
        require(q.shared_tx->idle_storage(), "wrong-queue cancellation leaked storage");
      }
      engine.tx_queue_table_[0][1].store(nullptr);
      Ring::free(cyclic.send_ring);
      cyclic.send_ring = nullptr;
    }
    for (unsigned iteration = 0; iteration < 40; ++iteration) {
      auto* original = allocate();
      require(engine.get_tx_packet_burst(original) == Status::SUCCESS, "shared allocation failed");
      for (int seg = 0; seg < segments; ++seg) {
        original->pkt_lens[seg][0] = seg == 0 ? 42 : 32;
        std::memset(original->pkts[seg][0], 0x30 + seg, original->pkt_lens[seg][0]);
      }
      auto* payload = original->pkts[segments - 1][0];
      TxTimestampId original_id = 0, clone_id = 0;
      require(engine.request_packet_tx_timestamp(original, 0, &original_id) == Status::SUCCESS,
              "shared timestamp request failed");
      BurstParams* second = nullptr;
      require(engine.clone_tx_burst(original, mask, &second) == Status::SUCCESS, "clone failed");
      require(engine.request_packet_tx_timestamp(second, 0, &clone_id) == Status::SUCCESS &&
                  clone_id != original_id,
              "clone inherited source timestamp request");
      require(second->pkts[segments - 1][0] == payload, "clone copied shared payload");
      for (int seg = 0; seg + 1 < segments; ++seg) {
        require(second->pkts[seg][0] != original->pkts[seg][0], "private header aliases source");
        require(std::memcmp(second->pkts[seg][0], original->pkts[seg][0],
                            original->pkt_lens[seg][0]) == 0,
                "private header copy differs");
        *static_cast<uint8_t*>(second->pkts[seg][0]) = 0x80;
        require(*static_cast<uint8_t*>(original->pkts[seg][0]) == 0x30 + seg,
                "private header edit changed source");
      }
      require(engine.set_udp_header(segments == 1 ? original : second, 0, 32, 1, 2) ==
                  (segments == 1 ? Status::RESOURCE_IN_USE : Status::SUCCESS),
              "header immutability guard failed");
      if (iteration % 2 == 0) {
        const auto first_wqe = post(original);
        complete(first_wqe);
        require(q.shared_tx->pools.back()->available() == 0,
                "original completion freed clone payload");
        auto* blocked = allocate();
        require(engine.get_tx_packet_burst_checked(blocked) == Status::NO_FREE_PACKET_BUFFERS,
                "reallocated retained payload");
        engine.free_tx_burst(blocked);
        BurstParams* third = nullptr;
        require(engine.clone_tx_burst(second, mask, &third) == Status::SUCCESS,
                "clone of clone failed after source metadata recycling");
        engine.free_all_packets(second);
        engine.free_tx_burst(second);
        complete(post(third));
      } else {
        // Cancellation of the oldest allocation cannot roll back newer slots.
        engine.free_all_packets(original);
        engine.free_tx_metadata(original);
        BurstParams* third = nullptr;
        require(engine.clone_tx_burst(second, mask, &third) == Status::SUCCESS,
                "clone of retained storage failed");
        const auto second_wqe = post(second);
        const auto last = post(third);
        complete(second_wqe);  // A requested packet's timestamp needs its own CQE.
        complete(last);        // Retire the remaining send and its shared references.
      }
      require(q.shared_tx->idle_storage(), "storage reference leaked");
      require(q.completed_tail.load() == q.alloc_head, "send credits leaked");
      uint64_t ns = 999;
      require(engine.get_packet_tx_timestamp(original_id, &ns) ==
                      (iteration % 2 == 0 ? Status::SUCCESS : Status::GENERIC_FAILURE) &&
                  ns == (iteration % 2 == 0 ? 100000000042ULL : 0),
              "shared source timestamp did not survive recycling/cancellation");
      require(engine.get_packet_tx_timestamp(clone_id, &ns) ==
                      (iteration % 2 == 0 ? Status::GENERIC_FAILURE : Status::SUCCESS) &&
                  ns == (iteration % 2 == 0 ? 0 : 100000000042ULL),
              "shared clone timestamp correlated with wrong send");
      require(engine.release_tx_timestamp(original_id) == Status::SUCCESS &&
                  engine.release_tx_timestamp(clone_id) == Status::SUCCESS,
              "shared timestamp release failed");
    }
    // A failed clone unwinds every allocation/retain, including private headers.
    auto* original = allocate();
    require(engine.get_tx_packet_burst(original) == Status::SUCCESS, "allocation failed");
    for (int seg = 0; seg < segments; ++seg) original->pkt_lens[seg][0] = 42;
    BurstParams* invalid = reinterpret_cast<BurstParams*>(1);
    require(engine.clone_tx_burst(original, 0, &invalid) == Status::INVALID_PARAMETER &&
                invalid == nullptr,
            "invalid mask changed source");
    auto* owner = q.shared_tx->pools.back()->acquire();
    require(owner == nullptr, "shared payload pool has more than one slot");
    if (segments > 1) {
      std::vector<detail::SharedTxBuffer*> held;
      while ((owner = q.shared_tx->pools[segments - 2]->acquire()) != nullptr)
        held.push_back(owner);
      require(engine.clone_tx_burst(original, mask, &invalid) == Status::NO_FREE_PACKET_BUFFERS &&
                  invalid == nullptr,
              "clone exhaustion did not roll back");
      for (auto* buffer : held) buffer->release();
    }
    original->pkts[0][0] = static_cast<uint8_t*>(original->pkts[0][0]) + 1;
    require(engine.send_tx_burst(original) == Status::INVALID_PARAMETER,
            "foreign pointer accepted");
    original->pkts[0][0] = static_cast<uint8_t*>(original->pkts[0][0]) - 1;
    // A queue entering DRAINING rejects clones while cancellation still works.
    q.state = ResourceState::DRAINING;
    require(engine.clone_tx_burst(original, mask, &invalid) == Status::INVALID_PARAMETER,
            "draining queue accepted clone");
    require(!q.shared_tx->idle_storage(), "unsent storage did not pin queue");
    engine.free_all_packets(original);
    engine.free_tx_burst(original);
    require(q.shared_tx->idle_storage(), "cancel in draining state leaked");
    q.state = ResourceState::ACTIVE;
    q.num_slots = 2;
    q.shared_tx->ledger.resize(2);
    auto* a = allocate();
    require(engine.get_tx_packet_burst(a) == Status::SUCCESS, "admission test allocation failed");
    for (int seg = 0; seg < segments; ++seg) a->pkt_lens[seg][0] = 42;
    BurstParams *b = nullptr, *c = nullptr;
    require(engine.clone_tx_burst(a, mask, &b) == Status::SUCCESS, "admission clone failed");
    require(engine.clone_tx_burst(a, mask, &c) == Status::SUCCESS, "second admission clone failed");
    TxTimestampId rejected_id = 0;
    require(engine.request_packet_tx_timestamp(c, 0, &rejected_id) == Status::SUCCESS,
            "admission timestamp request failed");
    require(engine.send_tx_burst(a) == Status::SUCCESS, "first admission failed");
    require(
        engine.clone_tx_burst(a, mask, &invalid) == Status::INVALID_PARAMETER && invalid == nullptr,
        "accepted burst remained cloneable");
    require(engine.send_tx_burst(b) == Status::SUCCESS, "second admission failed");
    require(engine.send_tx_burst(c) == Status::NO_SPACE_AVAILABLE, "send-credit limit ignored");
    uint64_t rejected_ns = 999;
    require(engine.get_packet_tx_timestamp(rejected_id, &rejected_ns) == Status::GENERIC_FAILURE &&
                rejected_ns == 0,
            "admission rejection left a timestamp pending");
    require(engine.release_tx_timestamp(rejected_id) == Status::SUCCESS,
            "rejected request release failed");
    require(q.shared_tx->pools.back()->available() == 0, "rejection freed admitted payload");
    void* raw = nullptr;
    require(q.send_ring->dequeue(&raw), "missing admitted send");
    engine.post_tx_burst(q, static_cast<BurstParams*>(raw));
    engine.tx_meta_pool_->put(raw);
    require(q.send_ring->dequeue(&raw), "missing second admitted send");
    const auto error_wqe = q.sq_pi;
    engine.post_tx_burst(q, static_cast<BurstParams*>(raw));
    engine.tx_meta_pool_->put(raw);
    const uint64_t completed_before_error = q.completed_tail.load();
    const uint32_t ci = q.tx_cq_ci;
    cq[ci & 7].op_own = (MLX5_CQE_REQ_ERR << 4) | ((ci & 8) ? 1 : 0);
    cq[ci & 7].wqe_counter = htobe16(static_cast<uint16_t>(error_wqe));
    engine.poll_tx_completions(q);
    require(q.shared_tx->failed.load() && q.completed_tail.load() == completed_before_error,
            "error CQE incorrectly retired an outstanding prefix");
    require(q.shared_tx->pools.back()->available() == 0, "error CQE released DMA-visible storage");
    q.shared_tx->retire(q.slots_posted);  // Model successful QP destruction.
    require(q.shared_tx->idle_storage(), "post-quiescence cleanup leaked references");
    // Whole-burst cloning must preserve one reference per packet/segment.
    q.shared_tx->failed = false;
    engine.force_quit_ = false;
    q.num_slots = 4;
    q.shared_tx->ledger.resize(4);
    q.shared_tx->pools.back() = std::make_unique<detail::SharedTxPool>();
    require(
        q.shared_tx->pools.back()->initialize(storage[segments - 1].data(), 128, 128, 2, true, -1),
        "batch payload pool setup failed");
    auto* batch = allocate();
    batch->hdr.hdr.num_pkts = 2;
    require(engine.get_tx_packet_burst(batch) == Status::SUCCESS, "batch acquisition failed");
    for (int seg = 0; seg < segments; ++seg)
      for (int packet = 0; packet < 2; ++packet) batch->pkt_lens[seg][packet] = 42;
    BurstParams* batch_clone = nullptr;
    require(engine.clone_tx_burst(batch, mask, &batch_clone) == Status::SUCCESS,
            "whole batch clone failed");
    complete(post(batch));
    require(q.shared_tx->pools.back()->available() == 0,
            "batch source completion freed clone storage");
    complete(post(batch_clone));
    require(q.shared_tx->idle_storage(), "batch completion leaked storage");
    auto* all_shared = allocate();
    require(engine.get_tx_packet_burst(all_shared) == Status::SUCCESS,
            "all-shared allocation failed");
    for (int seg = 0; seg < segments; ++seg) all_shared->pkt_lens[seg][0] = 42;
    BurstParams* all_clone = nullptr;
    require(engine.clone_tx_burst(all_shared, (1u << segments) - 1, &all_clone) == Status::SUCCESS,
            "multiple shared-segment mask failed");
    for (int seg = 0; seg < segments; ++seg)
      require(all_clone->pkts[seg][0] == all_shared->pkts[seg][0], "selected segment was copied");
    require(engine.set_udp_header(all_clone, 0, 32, 1, 2) == Status::RESOURCE_IN_USE,
            "shared header segment remained writable");
    engine.free_tx_metadata(all_shared);
    engine.free_tx_metadata(all_clone);
    require(q.shared_tx->idle_storage(), "metadata-only cancellation leaked shared references");
    if (segments > 1) {
      q.shared_tx->pools.back() = std::make_unique<detail::SharedTxPool>();
      require(q.shared_tx->pools.back()->initialize(storage[segments - 1].data(), 128, 128, 2,
                                                    false, -1),
              "GPU-kind pool setup failed");
      auto* device_source = allocate();
      require(engine.get_tx_packet_burst(device_source) == Status::SUCCESS,
              "GPU-kind acquisition failed");
      for (int seg = 0; seg < segments; ++seg) device_source->pkt_lens[seg][0] = 42;
      require(engine.clone_tx_burst(device_source, 1u, &invalid) == Status::NOT_SUPPORTED &&
                  invalid == nullptr,
              "private GPU copy was not rejected transactionally");
      engine.free_tx_metadata(device_source);
      require(q.shared_tx->idle_storage(), "unsupported GPU copy leaked storage");
    }
    engine.tx_queue_table_[0][0].store(nullptr);
    Ring::free(q.send_ring);
    q.send_ring = nullptr;
  }

  static void shared_configuration() {
    NetworkConfig cfg{};
    cfg.common_.stream_type = StreamType::RAW;
    cfg.common_.engine_type = EngineType::IBVERBS;
    MemoryRegionConfig region{};
    region.name_ = "data";
    region.kind_ = MemoryKind::HOST;
    region.buf_size_ = 256;
    region.num_bufs_ = 1;
    cfg.mrs_[region.name_] = region;
    InterfaceConfig intf{};
    TxQueueConfig tx{};
    tx.common_.name_ = "shared";
    tx.common_.id_ = 0;
    tx.common_.cpu_core_ = "0";
    tx.common_.batch_size_ = 1;
    tx.common_.mrs_ = {"data"};
    tx.buffer_mode_ = TxBufferMode::SHARED;
    intf.tx_.queues_ = {tx};
    cfg.ifs_ = {intf};
    require(validate_network_config(cfg), "valid shared queue rejected");
    auto& q = cfg.ifs_[0].tx_.queues_[0];
    q.poll_mode_ = QueuePollMode::DIRECT;
    require(!validate_network_config(cfg), "direct shared queue accepted");
    q.poll_mode_ = QueuePollMode::INDIRECT;
    cfg.common_.engine_type = EngineType::SOCKET;
    require(!validate_network_config(cfg), "socket shared queue accepted");
    cfg.common_.engine_type = EngineType::IBVERBS;
    q.common_.mrs_.push_back("data");
    require(!validate_network_config(cfg), "repeated shared MR accepted");
    q.common_.mrs_.resize(1);
    q.common_.batch_size_ = 3;
    require(!validate_network_config(cfg), "shared batch exceeds default send credits");
    q.common_.batch_size_ = 1;
    q.buffer_mode_ = TxBufferMode::CYCLIC;
    q.max_inflight_sends_ = 2;
    require(!validate_network_config(cfg), "cyclic queue accepted shared send-credit field");
  }

  static void validation() {
    IbverbsEngine engine;
    IbvTxQueue q;
    q.port_id = q.queue_id = 0;
    q.num_segs = 3;
    q.regions.resize(3);
    for (auto& region : q.regions) region.slot_size = 65536;
    BurstParams burst{};
    burst.hdr.hdr.num_pkts = 1;
    burst.hdr.hdr.num_segs = 3;
    GpuTxSubmission gpu_submission{};
    gpu_submission.doorbell = 123;
    require(engine.prepare_tx_burst_gpu(nullptr, &gpu_submission) == Status::NULL_PTR &&
                gpu_submission.doorbell == 0,
            "null GPU preparation did not clear its output");
    require(engine.prepare_tx_burst_gpu(&burst, nullptr) == Status::NULL_PTR,
            "null GPU descriptor accepted");
    gpu_submission.doorbell = 123;
    require(engine.prepare_tx_burst_gpu(&burst, &gpu_submission) == Status::INVALID_PARAMETER &&
                gpu_submission.doorbell == 0,
            "unknown GPU queue accepted");
    q.state = ResourceState::ACTIVE;
    engine.tx_queue_table_[0][0].store(&q);
    require(engine.prepare_tx_burst_gpu(&burst, &gpu_submission) == Status::NOT_SUPPORTED &&
                q.direct_pending == nullptr,
            "GPU submission accepted an indirect queue");
    q.poll_mode = QueuePollMode::DIRECT;
    require(engine.prepare_tx_burst_gpu(&burst, &gpu_submission) == Status::INVALID_PARAMETER &&
                q.direct_pending == nullptr && q.sq_pi == 0,
            "GPU submission accepted an unallocated burst");
    engine.tx_queue_table_[0][0].store(nullptr);
    std::array<void*, 3> pointers{reinterpret_cast<void*>(0x1000), reinterpret_cast<void*>(0x2000),
                                  reinterpret_cast<void*>(0x3000)};
    std::array<uint32_t, 3> lengths{16, 16, 16};
    for (int s = 0; s < 3; ++s) {
      burst.pkts[s] = &pointers[s];
      burst.pkt_lens[s] = &lengths[s];
    }
    IbverbsEngine::EndpointFastPath endpoint;
    endpoint.mtu = 80;
    require(engine.prepare_endpoint_tx_burst(q, endpoint, &burst) == Status::INVALID_PARAMETER,
            "endpoint ignored aggregate MTU limit");
    require(burst.hdr.hdr.burst_flags == 0, "failed validation changed ownership state");
    endpoint.mtu = 100000;
    lengths = {40000, 40000, 40000};
    require(engine.prepare_endpoint_tx_burst(q, endpoint, &burst) == Status::INVALID_PARAMETER,
            "endpoint ignored aggregate IPv4/UDP limit");
    lengths = {16, 16, 65537};
    require(engine.prepare_endpoint_tx_burst(q, endpoint, &burst) == Status::INVALID_PARAMETER,
            "endpoint ignored an individual segment capacity");
    lengths = {16, 16, 16};
    burst.hdr.hdr.num_segs = 2;
    require(engine.prepare_endpoint_tx_burst(q, endpoint, &burst) == Status::INVALID_PARAMETER,
            "endpoint accepted a segment-count mismatch");

    InterfaceConfig interface;
    TxQueueConfig config;
    config.common_.mrs_ = {"a", "b", "c", "d", "e"};
    require(engine.setup_tx_queue(q, interface, config) == Status::INVALID_PARAMETER,
            "oversized TX region list accepted");
  }
};
}  // namespace daqiri

int main() {
  try {
    for (int segments : {1, 4})
      for (bool scheduled : {false, true})
        for (bool realtime : {false, true})
          daqiri::IbverbsEngineTxTestPeer::tx_timestamps(segments, false, true, scheduled, realtime,
                                                         true);
    for (int segments : {1, 4})
      for (bool endpoint : {false, true})
        for (bool direct : {false, true})
          for (bool scheduled : {false, true})
            for (bool realtime : {false, true})
              daqiri::IbverbsEngineTxTestPeer::tx_timestamps(segments, endpoint, direct, scheduled,
                                                             realtime);
    for (int segments = 1; segments <= daqiri::MAX_NUM_SEGS; ++segments) {
      for (bool endpoint : {false, true}) {
        for (bool direct : {false, true}) {
          for (bool wait : {false, true}) {
            daqiri::IbverbsEngineTxTestPeer::gathered_send(segments, endpoint, direct, wait);
          }
        }
      }
    }
    daqiri::IbverbsEngineTxTestPeer::shared_configuration();
    daqiri::IbverbsEngineTxTestPeer::shared_pool();
    for (int segments = 1; segments <= daqiri::MAX_NUM_SEGS; ++segments)
      for (bool endpoint : {false, true})
        daqiri::IbverbsEngineTxTestPeer::shared_lifetime(segments, endpoint);
    daqiri::IbverbsEngineTxTestPeer::validation();
    std::cout << "PASS: multi-segment TX encoding, wraparound, completion, and validation\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
