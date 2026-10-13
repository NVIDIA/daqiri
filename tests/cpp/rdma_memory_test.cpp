// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#include "src/engines/rdma/daqiri_rdma_engine.h"

#include <cstdlib>
#include <iostream>
#include <cstring>
#include <deque>
#include <chrono>
#include <sched.h>
#include "src/daqiri_ring.h"

namespace daqiri {
class RdmaMemoryTestPeer {
 public:
  static void register_memory(RdmaEngine& engine, rdma_cm_id* connection, ibv_pd* pd, ibv_mr* mr) {
    engine.tx_rings_map_[connection] = nullptr;
    engine.pd_map_[connection->verbs] = pd;
    rdma_mr_params params{};
    params.ctx_mr_map_[pd] = mr;
    params.ptr_ = mr->addr;
    params.params_.buf_size_ = mr->length;
    params.params_.adj_size_ = mr->length;
    params.params_.num_bufs_ = 1;
    params.params_.access_ = MEM_ACCESS_LOCAL;
    engine.mrs_["memory"] = params;
  }
  static void release_synthetic_resources(RdmaEngine& engine) {
    // Stack-backed registrations and PDs are owned by the test, not rdma-core.
    engine.mrs_.clear();
    engine.pd_map_.clear();
    engine.tx_rings_map_.clear();
  }
  static void setup_worker(RdmaEngine& engine, rdma_thread_params& params) {
    engine.cfg_.ifs_.resize(1);
    engine.cfg_.ifs_[0].tx_.queues_.resize(1);
    engine.cfg_.ifs_[0].tx_.queues_[0].common_.cpu_core_ = std::to_string(sched_getcpu());
    engine.tx_rings_map_[params.client_id] = params.qp_params.tx_ring;
  }
  static void worker(RdmaEngine& engine, rdma_thread_params& params) {
    engine.rdma_thread(false, &params);
  }
  static int access(RdmaEngine& engine, uint32_t flags) {
    return engine.mr_access_to_ibv(flags);
  }
};
}  // namespace daqiri

extern "C" int ibv_modify_qp(ibv_qp*, ibv_qp_attr*, int) {
  return 0;
}
extern "C" int ibv_query_qp(ibv_qp*, ibv_qp_attr*, int, ibv_qp_init_attr*) {
  return ENOSYS;
}

namespace {
void require(bool condition) {
  if (!condition) {
    std::cerr << "RDMA memory assertion failed\n";
    std::exit(1);
  }
}

struct SimulatedVerbs {
  std::deque<ibv_wc> completions;
  std::vector<ibv_send_wr> posted;
  int fail_packet = -1;
  bool cq_error = false;
  uint32_t remote_key = 0;
};
int poll_cq(ibv_cq* cq, int, ibv_wc* wc) {
  auto* provider = static_cast<SimulatedVerbs*>(cq->cq_context);
  if (provider == nullptr || provider->completions.empty()) return 0;
  *wc = provider->completions.front();
  provider->completions.pop_front();
  return 1;
}
int post_send(ibv_qp* qp, ibv_send_wr* wr, ibv_send_wr** bad_wr) {
  auto* provider = static_cast<SimulatedVerbs*>(qp->qp_context);
  if (static_cast<int>(provider->posted.size()) == provider->fail_packet) {
    *bad_wr = wr;
    return EIO;
  }
  require(wr->num_sge == 1 && wr->sg_list->lkey == 17);
  require(wr->wr.rdma.rkey == provider->remote_key);
  require((wr->send_flags & (IBV_SEND_SIGNALED | IBV_SEND_FENCE)) ==
          (IBV_SEND_SIGNALED | IBV_SEND_FENCE));
  auto* local = reinterpret_cast<void*>(wr->sg_list->addr);
  auto* remote = reinterpret_cast<void*>(wr->wr.rdma.remote_addr);
  ibv_wc completion{};
  completion.wr_id = wr->wr_id;
  completion.status = provider->cq_error ? IBV_WC_REM_ACCESS_ERR : IBV_WC_SUCCESS;
  if (wr->opcode == IBV_WR_RDMA_READ) {
    if (!provider->cq_error) std::memcpy(local, remote, wr->sg_list->length);
    completion.opcode = IBV_WC_RDMA_READ;
  } else {
    require(wr->opcode == IBV_WR_RDMA_WRITE);
    if (!provider->cq_error) std::memcpy(remote, local, wr->sg_list->length);
    completion.opcode = IBV_WC_RDMA_WRITE;
  }
  // Error CQEs need not contain a usable opcode.
  if (provider->cq_error) completion.opcode = IBV_WC_RECV;
  provider->posted.push_back(*wr);
  provider->completions.push_back(completion);
  return 0;
}

void test_worker(daqiri::RDMAOpCode opcode, int fail_packet = -1, bool cq_error = false) {
  using namespace daqiri;
  RdmaEngine engine;
  SimulatedVerbs provider;
  provider.remote_key = 0x1234;
  provider.fail_packet = fail_packet;
  provider.cq_error = cq_error;
  ibv_context context{};
  context.ops.poll_cq = poll_cq;
  context.ops.post_send = post_send;
  ibv_pd pd{};
  ibv_qp qp{};
  qp.context = &context;
  qp.qp_type = IBV_QPT_RC;
  qp.qp_context = &provider;
  rdma_cm_id connection{};
  connection.verbs = &context;
  connection.qp = &qp;
  char local[128], remote[128];
  std::memset(local, opcode == RDMAOpCode::RDMA_WRITE ? 0x5a : 0, sizeof(local));
  std::memset(remote, opcode == RDMAOpCode::RDMA_READ ? 0x5a : 0, sizeof(remote));
  ibv_mr mr{};
  mr.addr = local;
  mr.length = sizeof(local);
  mr.lkey = 17;
  RdmaMemoryTestPeer::register_memory(engine, &connection, &pd, &mr);
  ibv_cq tx_cq{}, rx_cq{};
  tx_cq.context = rx_cq.context = &context;
  tx_cq.cq_context = &provider;
  rdma_thread_params params{};
  params.client_id = &connection;
  params.if_idx = params.queue_idx = 0;
  params.qp_params.tx_cq = &tx_cq;
  params.qp_params.rx_cq = &rx_cq;
  params.qp_params.qp_attr.qp_type = IBV_QPT_RC;
  params.qp_params.tx_ring = Ring::create("test_tx", 8, RingMode::SPSC);
  params.qp_params.rx_ring = Ring::create("test_rx", 8, RingMode::SPSC);
  require(params.qp_params.tx_ring && params.qp_params.rx_ring);
  RdmaMemoryTestPeer::setup_worker(engine, params);
  BurstParams burst{};
  void* packets[]{local, local + 32};
  uint32_t lengths[]{32, 64};
  const auto conn_id = reinterpret_cast<uintptr_t>(&connection);
  require(engine.rdma_set_header(&burst, opcode, conn_id, false, 2, 999, "memory") ==
          Status::SUCCESS);
  burst.pkts[0] = packets;
  burst.pkt_lens[0] = lengths;
  RDMAMemoryRegion region{reinterpret_cast<uintptr_t>(remote), sizeof(remote), provider.remote_key};
  require(engine.rdma_set_remote_memory(&burst, region, 16) == Status::SUCCESS);
  std::thread worker([&] { RdmaMemoryTestPeer::worker(engine, params); });
  require(engine.send_tx_burst(&burst) == Status::SUCCESS);
  BurstParams* completed = nullptr;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!params.qp_params.rx_ring->dequeue(reinterpret_cast<void**>(&completed)) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  params.ready_to_exit.store(true);
  worker.join();
  require(completed == &burst);
  require(burst.transport_hdr.opcode == opcode && burst.transport_hdr.wr_id == 999);
  require(burst.transport_hdr.tx);
  require(burst.transport_hdr.status ==
          (fail_packet >= 0 || cq_error ? Status::GENERIC_FAILURE : Status::SUCCESS));
  require(params.qp_params.rx_ring->count() == 0);
  if (fail_packet < 0 && !cq_error) {
    require(std::memcmp(local, remote + 16, 96) == 0);
    require(provider.posted.size() == 2);
    require(provider.posted[0].wr.rdma.remote_addr == region.address + 16);
    require(provider.posted[1].wr.rdma.remote_addr == region.address + 48);
  }
  RdmaMemoryTestPeer::release_synthetic_resources(engine);
  Ring::free(params.qp_params.tx_ring);
  Ring::free(params.qp_params.rx_ring);
}
}  // namespace

int main() {
  using namespace daqiri;
  RdmaEngine engine;
  ibv_context context{};
  ibv_pd pd{};
  rdma_cm_id connection{};
  connection.verbs = &context;
  char memory[128]{};
  ibv_mr mr{};
  mr.addr = memory;
  mr.length = sizeof(memory);
  mr.rkey = 0x12345678;
  RdmaMemoryTestPeer::register_memory(engine, &connection, &pd, &mr);
  const auto conn_id = reinterpret_cast<uintptr_t>(&connection);
  RDMAMemoryRegion exported;
  require(engine.rdma_get_memory_region(conn_id, "memory", &exported) == Status::SUCCESS);
  require(exported.address == reinterpret_cast<uintptr_t>(memory));
  require(exported.length == 128 && exported.rkey == mr.rkey);
  require(engine.rdma_get_memory_region(conn_id, "missing", &exported) ==
          Status::INVALID_PARAMETER);
  require(engine.rdma_get_memory_region(123, "memory", &exported) == Status::INVALID_PARAMETER);
  require(engine.rdma_get_memory_region(conn_id, "memory", nullptr) == Status::INVALID_PARAMETER);
  require((RdmaMemoryTestPeer::access(engine, MEM_ACCESS_RDMA_WRITE) & IBV_ACCESS_LOCAL_WRITE) !=
          0);
  require((RdmaMemoryTestPeer::access(engine, MEM_ACCESS_RDMA_READ) & IBV_ACCESS_REMOTE_READ) != 0);

  BurstParams burst{};
  uint32_t lengths[]{32, 64};
  burst.pkt_lens[0] = lengths;
  for (auto op : {RDMAOpCode::RDMA_READ, RDMAOpCode::RDMA_WRITE}) {
    require(engine.rdma_set_header(&burst, op, conn_id, false, 2, 42, "memory") == Status::SUCCESS);
    require(engine.rdma_set_remote_memory(&burst, exported, 32) == Status::SUCCESS);
    require(burst.transport_hdr.raddr == memory + 32 && burst.transport_hdr.dst_key == mr.rkey);
    require(engine.rdma_set_remote_memory(&burst, exported, 33) == Status::INVALID_PARAMETER);
    require(engine.rdma_set_remote_memory(&burst, exported, UINT64_MAX) ==
            Status::INVALID_PARAMETER);
    require(burst.transport_hdr.wr_id == 42 && burst.transport_hdr.opcode == op);
    require(engine.set_packet_lengths(&burst, 0, {32}) == Status::SUCCESS);
    require(burst.transport_hdr.raddr == nullptr);
    require(engine.rdma_set_remote_memory(&burst, exported, 32) == Status::SUCCESS);
  }
  RDMAMemoryRegion overflowing{UINT64_MAX - 16, 128, mr.rkey};
  require(engine.rdma_set_remote_memory(&burst, overflowing, 0) == Status::INVALID_PARAMETER);
  require(engine.rdma_set_remote_memory(nullptr, exported, 0) == Status::INVALID_PARAMETER);
  require(engine.rdma_set_header(&burst, RDMAOpCode::RDMA_READ_IMM, conn_id, false, 2, 42,
                                 "memory") == Status::NOT_SUPPORTED);
  require(engine.rdma_set_header(&burst, RDMAOpCode::RDMA_WRITE_IMM, conn_id, false, 2, 42,
                                 "memory") == Status::NOT_SUPPORTED);
  require(engine.rdma_set_header(&burst, RDMAOpCode::SEND, conn_id, false, 0, 42, "memory") ==
          Status::INVALID_PARAMETER);
  require(engine.rdma_set_header(&burst, RDMAOpCode::SEND, conn_id, false, 2, UINT64_MAX,
                                 "memory") == Status::INVALID_PARAMETER);
  require(engine.rdma_set_header(&burst, RDMAOpCode::SEND, conn_id, false, 2, 42, "memory") ==
          Status::SUCCESS);
  require(engine.rdma_set_remote_memory(&burst, exported, 0) == Status::NOT_SUPPORTED);
  RdmaMemoryTestPeer::release_synthetic_resources(engine);
  test_worker(RDMAOpCode::RDMA_READ);
  test_worker(RDMAOpCode::RDMA_WRITE);
  test_worker(RDMAOpCode::RDMA_READ, 0);
  test_worker(RDMAOpCode::RDMA_WRITE, 1);
  test_worker(RDMAOpCode::RDMA_READ, -1, true);
  return 0;
}
