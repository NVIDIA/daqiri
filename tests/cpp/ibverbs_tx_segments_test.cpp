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

  static void validation() {
    IbverbsEngine engine;
    IbvTxQueue q;
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
    daqiri::IbverbsEngineTxTestPeer::validation();
    std::cout << "PASS: multi-segment TX encoding, wraparound, completion, and validation\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
