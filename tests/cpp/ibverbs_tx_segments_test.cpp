// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <cstring>
#include <endian.h>
#include <iostream>
#include <stdexcept>
#include <string>

#include "src/engines/ibverbs/daqiri_ibverbs_engine.h"

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
      BurstParams* second = nullptr;
      require(engine.clone_tx_burst(original, mask, &second) == Status::SUCCESS, "clone failed");
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
        post(second);
        const auto last = post(third);
        complete(last);  // Retire both sends with one cumulative completion.
      }
      require(q.shared_tx->idle_storage(), "storage reference leaked");
      require(q.completed_tail.load() == q.alloc_head, "send credits leaked");
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
    require(engine.send_tx_burst(a) == Status::SUCCESS, "first admission failed");
    require(
        engine.clone_tx_burst(a, mask, &invalid) == Status::INVALID_PARAMETER && invalid == nullptr,
        "accepted burst remained cloneable");
    require(engine.send_tx_burst(b) == Status::SUCCESS, "second admission failed");
    require(engine.send_tx_burst(c) == Status::NO_SPACE_AVAILABLE, "send-credit limit ignored");
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
