// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <arpa/inet.h>
#include <chrono>
#include <future>
#include <mutex>
#include <vector>
#include <cerrno>
#include <deque>
#include <iostream>
#include <sched.h>
#include <stdexcept>
#include <thread>

#include "src/engines/rdma/daqiri_rdma_engine.h"
#include "src/rdma/cm_api.h"

namespace daqiri {
extern std::atomic<bool> rdma_force_quit;
namespace {
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

struct FakeVerbs {
  ibv_cq rx{}, tx{};
  ibv_cq_ex rx_ex{};
  ibv_wc current_wc{};
  static constexpr uint64_t kTimestamp = 1780000000123456789ULL;
  bool timestamps = false, realtime_supported = true, fail_create_cq_ex = false;
  uint64_t rx_cq_flags = 0;
  int poll_error = 0;
  unsigned ended_polls = 0;
  ibv_cq_ex timed_rx{}, timed_tx{};
  ibv_qp qp{};
  ibv_mr grh{};
  ibv_ah ah{};
  unsigned destroyed_ahs = 0, deregistered_grhs = 0;
  std::deque<ibv_wc> rx_completions, tx_completions;
  std::atomic<unsigned> rx_polls{0}, tx_polls{0}, sends{0}, receives{0};
  std::mutex mutex;
  bool auto_complete = false;
  uint64_t last_send_id = 0, last_receive_id = 0;
  unsigned last_flags = 0;
  int send_result = 0, recv_result = 0;
  uint32_t requested_inline = 0;
  int created_cqs = 0, destroyed_cqs = 0, fail_cq = -1;
  bool fail_qp = false;
  bool wallclock_requested = false, timed_qp = false;
  uint32_t requested_send_wr = 0;
  std::vector<uint64_t> send_times;
  rdma_event_channel channel{};
  rdma_cm_id client{};
  rdma_cm_event event{};
  sockaddr_in local{};
  int events = 0, pending_events = 0, ids_destroyed = 0, channels_destroyed = 0;
  int connects = 0, accepts = 0, rejects = 0;
  bool fail_connect = false, unexpected_event = false;
  static FakeVerbs* current;
  static rdma_event_channel* create_channel() {
    return &current->channel;
  }
  static void destroy_channel(rdma_event_channel*) {
    ++current->channels_destroyed;
  }
  static int create_id(rdma_event_channel*, rdma_cm_id** id, void*, rdma_port_space) {
    *id = &current->client;
    return 0;
  }
  static int destroy_id(rdma_cm_id* id) {
    require(!id->qp && current->pending_events == 0, "CM ID destroyed before QP/event cleanup");
    ++current->ids_destroyed;
    return 0;
  }
  static int resolve_addr(rdma_cm_id*, sockaddr*, sockaddr*, int) {
    return 0;
  }
  static int resolve_route(rdma_cm_id*, int) {
    return 0;
  }
  static sockaddr* local_addr(rdma_cm_id*) {
    return reinterpret_cast<sockaddr*>(&current->local);
  }
  static int get_event(rdma_event_channel*, rdma_cm_event** event) {
    const int n = current->events++;
    current->event.event = n == 0                      ? RDMA_CM_EVENT_ADDR_RESOLVED
                           : n == 1                    ? RDMA_CM_EVENT_ROUTE_RESOLVED
                           : current->unexpected_event ? RDMA_CM_EVENT_REJECTED
                                                       : RDMA_CM_EVENT_ESTABLISHED;
    *event = &current->event;
    ++current->pending_events;
    return 0;
  }
  static int ack(rdma_cm_event*) {
    --current->pending_events;
    return 0;
  }
  static const char* event_str(rdma_cm_event_type) {
    return "test event";
  }
  static int connect(rdma_cm_id*, rdma_conn_param*) {
    ++current->connects;
    return current->fail_connect ? -1 : 0;
  }
  static int accept(rdma_cm_id*, rdma_conn_param*) {
    ++current->accepts;
    return -1;
  }
  static int reject(rdma_cm_id*, const void*, uint8_t) {
    ++current->rejects;
    return 0;
  }

  static ibv_cq* create_cq(ibv_context*, int, void*, ibv_comp_channel*, int) {
    const int n = current->created_cqs++;
    if (n == current->fail_cq) return nullptr;
    return n % 2 == 0 ? &current->rx : &current->tx;
  }
  static ibv_cq_ex* create_cq_ex(ibv_context*, ibv_cq_init_attr_ex* attr) {
    if (current->fail_create_cq_ex) {
      errno = EOPNOTSUPP;
      return nullptr;
    }
    current->wallclock_requested = !(attr->wc_flags & IBV_WC_EX_WITH_COMPLETION_TIMESTAMP);
    const int n = current->created_cqs++;
    if (n == current->fail_cq) return nullptr;
    if (attr->wc_flags & IBV_WC_EX_WITH_BYTE_LEN) {
      current->timestamps = true;
      current->rx_cq_flags = attr->wc_flags;
    }
    if (attr->wc_flags & IBV_WC_EX_WITH_COMPLETION_TIMESTAMP) return &current->rx_ex;
    return n % 2 == 0 ? &current->timed_rx : &current->timed_tx;
  }
  static int destroy_cq(ibv_cq*) {
    ++current->destroyed_cqs;
    return 0;
  }
  static void destroy_qp(rdma_cm_id* id) {
    id->qp = nullptr;
  }
  static int start_poll(ibv_cq_ex* cq, ibv_poll_cq_attr*) {
    if (current->poll_error) return current->poll_error;
    if (poll_cq(&current->rx, 1, &current->current_wc) == 0) return ENOENT;
    cq->wr_id = current->current_wc.wr_id;
    cq->status = current->current_wc.status;
    return 0;
  }
  static void end_poll(ibv_cq_ex*) {
    ++current->ended_polls;
  }
  static ibv_wc_opcode read_opcode(ibv_cq_ex*) {
    require(current->current_wc.status == IBV_WC_SUCCESS, "read opcode on failed CQE");
    return current->current_wc.opcode;
  }
  static unsigned int read_wc_flags(ibv_cq_ex*) {
    require(current->current_wc.status == IBV_WC_SUCCESS, "read flags on failed CQE");
    return current->current_wc.wc_flags;
  }
  static uint32_t read_imm_data(ibv_cq_ex*) {
    require(current->current_wc.status == IBV_WC_SUCCESS, "read immediate on failed CQE");
    return current->current_wc.imm_data;
  }
  static uint32_t read_byte_len(ibv_cq_ex*) {
    require(current->current_wc.status == IBV_WC_SUCCESS, "read length on failed CQE");
    return current->current_wc.byte_len;
  }
  static uint64_t read_timestamp(ibv_cq_ex*) {
    require(current->current_wc.status == IBV_WC_SUCCESS, "read timestamp on failed CQE");
    const uint64_t timestamp = kTimestamp + current->current_wc.wr_id;
    // Match mlx5: WALLCLOCK alone on a real-time-capable device exposes packed
    // seconds/nanoseconds; requesting raw ticks too selects provider conversion.
    if (current->realtime_supported &&
        !(current->rx_cq_flags & IBV_WC_EX_WITH_COMPLETION_TIMESTAMP)) {
      return ((timestamp / 1000000000ULL) << 32) | (timestamp % 1000000000ULL);
    }
    return timestamp;
  }
  static int create_timed_qp(rdma_cm_id* id, ibv_pd* pd, ibv_qp_init_attr* attr) {
    require(attr->recv_cq == ibv_cq_ex_to_cq(&current->timed_rx) &&
                attr->send_cq == ibv_cq_ex_to_cq(&current->timed_tx),
            "timed QP requires matching wall-clock RX and TX CQs");
    current->timed_qp = true;
    return create_qp(id, pd, attr);
  }
  static int create_qp(rdma_cm_id* id, ibv_pd*, ibv_qp_init_attr* attr) {
    if (current->fail_qp) return -1;
    if (!current->timed_qp && current->timestamps && current->realtime_supported &&
        !(current->rx_cq_flags & IBV_WC_EX_WITH_COMPLETION_TIMESTAMP))
      return -1;
    current->requested_inline = attr->cap.max_inline_data;
    current->requested_send_wr = attr->cap.max_send_wr;
    // The provider can return more capacity than requested.
    attr->cap.max_inline_data = attr->cap.max_inline_data ? 160 : 0;
    current->qp.qp_type = attr->qp_type;
    id->qp = &current->qp;
    return 0;
  }
  static int poll_cq(ibv_cq* cq, int, ibv_wc* wc) {
    std::lock_guard<std::mutex> lock(current->mutex);
    const bool rx = cq == &current->rx || cq == ibv_cq_ex_to_cq(&current->timed_rx);
    ++(rx ? current->rx_polls : current->tx_polls);
    auto& completions = rx ? current->rx_completions : current->tx_completions;
    if (completions.empty()) return 0;
    *wc = completions.front();
    completions.pop_front();
    return 1;
  }
  static int post_send(ibv_qp* qp, ibv_send_wr* wr, ibv_send_wr**) {
    std::lock_guard<std::mutex> lock(current->mutex);
    const bool one_sided = wr->opcode == IBV_WR_RDMA_READ || wr->opcode == IBV_WR_RDMA_WRITE ||
                           wr->opcode == IBV_WR_RDMA_WRITE_WITH_IMM;
    require((one_sided || wr->opcode == IBV_WR_SEND) &&
                ((wr->num_sge == 1 && wr->sg_list) ||
                 (wr->opcode == IBV_WR_RDMA_WRITE_WITH_IMM && wr->num_sge == 0 && !wr->sg_list)),
            "incorrect SQ encoding");
    if (one_sided) {
      require(wr->wr.rdma.remote_addr >= 4096 && wr->wr.rdma.rkey == 456 &&
                  bool(wr->send_flags & IBV_SEND_FENCE) == (qp->qp_type == IBV_QPT_RC),
              "one-sided descriptor or fence lost");
    }
    if (qp->qp_type == IBV_QPT_UD) {
      require(wr->wr.ud.ah == &current->ah && wr->wr.ud.remote_qpn == 321 &&
                  wr->wr.ud.remote_qkey == 654 && !(wr->send_flags & IBV_SEND_FENCE),
              "UD SEND lost its destination or used an RC fence");
    }
    if (current->send_result) {
      return current->send_result;
    }
    current->last_send_id = wr->wr_id;
    if (current->auto_complete && (wr->send_flags & IBV_SEND_SIGNALED)) {
      ibv_wc wc{};
      wc.wr_id = wr->wr_id;
      wc.status = IBV_WC_SUCCESS;
      wc.opcode = IBV_WC_SEND;
      current->tx_completions.push_back(wc);
    }
    current->last_flags = wr->send_flags;
    ++current->sends;
    return 0;
  }
  static int post_timed_send(ibv_qp* qp, ibv_send_wr* wr, uint64_t time) {
    const int result = post_send(qp, wr, nullptr);
    if (!result) current->send_times.push_back(time);
    return result;
  }
  static int post_recv(ibv_qp* qp, ibv_recv_wr* wr, ibv_recv_wr**) {
    std::lock_guard<std::mutex> lock(current->mutex);
    const bool ud = qp->qp_type == IBV_QPT_UD;
    require((ud && wr->num_sge == 2 && wr->sg_list[0].length == sizeof(ibv_grh)) ||
                (wr->num_sge == 1 && wr->sg_list) || (wr->num_sge == 0 && !wr->sg_list),
            "incorrect RECEIVE encoding");
    if (current->recv_result) {
      return current->recv_result;
    }
    current->last_receive_id = wr->wr_id;
    if (current->auto_complete) {
      ibv_wc wc{};
      wc.wr_id = wr->wr_id;
      wc.status = IBV_WC_SUCCESS;
      wc.opcode = IBV_WC_RECV;
      wc.byte_len = ud            ? sizeof(ibv_grh) + wr->sg_list[1].length
                    : wr->num_sge ? wr->sg_list->length
                                  : 0;
      current->rx_completions.push_back(wc);
    }
    ++current->receives;
    return 0;
  }
  static int modify_qp(ibv_qp*, ibv_qp_attr*, int) {
    return 0;
  }
  static int query_qp(ibv_qp*, ibv_qp_attr*, int, ibv_qp_init_attr*) {
    return -1;
  }
  static ibv_mr* reg_mr(ibv_pd*, void*, size_t, int) {
    return &current->grh;
  }
  static int dereg_mr(ibv_mr*) {
    ++current->deregistered_grhs;
    return 0;
  }
  static int query_port(ibv_context*, uint8_t, ibv_port_attr* attr) {
    attr->active_mtu = IBV_MTU_1024;
    return 0;
  }
  static ibv_ah* create_ah(ibv_pd*, ibv_ah_attr*) {
    return &current->ah;
  }
  static int destroy_ah(ibv_ah*) {
    ++current->destroyed_ahs;
    return 0;
  }
  static const char* wc_status_str(ibv_wc_status) {
    return "test completion";
  }
};
FakeVerbs* FakeVerbs::current = nullptr;
}  // namespace

// Inject the verbs table to exercise production posting, completion, and
// connection launch paths without a NIC, GPU, or CUDA context.
class RdmaEnginePollingTestPeer {
  RdmaEngine engine;
  rdma_cm_id listener{};
  rdma_thread_params& params;
  FakeVerbs fake;
  detail::RdmaCmApi api{};
  rdma_cm_id id{};
  ibv_context context{};
  ibv_pd pd{};
  ibv_mr mr{};
  Ring* tx_ring = nullptr;
  Ring* rx_ring = nullptr;

 public:
  RdmaEnginePollingTestPeer(bool direct_rx, bool direct_tx, bool accurate_send = false,
                            bool timestamps = false, bool realtime_supported = true,
                            RDMATransportMode transport = RDMATransportMode::RC)
      : params(engine.server_q_params_.try_emplace(&listener, 1).first->second[0]) {
    FakeVerbs::current = &fake;
    api.cm_create_event_channel = &FakeVerbs::create_channel;
    api.cm_destroy_event_channel = &FakeVerbs::destroy_channel;
    api.cm_create_id = &FakeVerbs::create_id;
    api.cm_destroy_id = &FakeVerbs::destroy_id;
    api.cm_resolve_addr = &FakeVerbs::resolve_addr;
    api.cm_resolve_route = &FakeVerbs::resolve_route;
    api.cm_get_local_addr = &FakeVerbs::local_addr;
    api.cm_get_cm_event = &FakeVerbs::get_event;
    api.cm_ack_cm_event = &FakeVerbs::ack;
    api.cm_event_str = &FakeVerbs::event_str;
    api.cm_connect = &FakeVerbs::connect;
    api.cm_accept = &FakeVerbs::accept;
    api.cm_reject = &FakeVerbs::reject;
    fake.realtime_supported = realtime_supported;
    api.create_cq = &FakeVerbs::create_cq;
    api.create_cq_ex = &FakeVerbs::create_cq_ex;
    api.start_poll = &FakeVerbs::start_poll;
    api.end_poll = &FakeVerbs::end_poll;
    api.wc_read_opcode = &FakeVerbs::read_opcode;
    api.wc_read_byte_len = &FakeVerbs::read_byte_len;
    api.wc_read_wc_flags = &FakeVerbs::read_wc_flags;
    api.wc_read_imm_data = &FakeVerbs::read_imm_data;
    api.wc_read_completion_wallclock_ns = &FakeVerbs::read_timestamp;
    api.create_timed_qp = &FakeVerbs::create_timed_qp;
    api.cm_create_qp = &FakeVerbs::create_qp;
    api.destroy_cq = &FakeVerbs::destroy_cq;
    api.cm_destroy_qp = &FakeVerbs::destroy_qp;
    api.poll_cq = &FakeVerbs::poll_cq;
    api.post_send = &FakeVerbs::post_send;
    api.post_timed_send = &FakeVerbs::post_timed_send;
    api.post_recv = &FakeVerbs::post_recv;
    api.modify_qp = &FakeVerbs::modify_qp;
    api.query_qp = &FakeVerbs::query_qp;
    api.wc_status_str = &FakeVerbs::wc_status_str;
    api.reg_mr = &FakeVerbs::reg_mr;
    api.dereg_mr = &FakeVerbs::dereg_mr;
    api.query_port = &FakeVerbs::query_port;
    api.create_ah = &FakeVerbs::create_ah;
    api.destroy_ah = &FakeVerbs::destroy_ah;
    engine.verbs_ = &api;
    engine.cfg_.ifs_.resize(1);
    auto& intf = engine.cfg_.ifs_[0];
    intf.rx_.hardware_timestamps_ = timestamps;
    intf.tx_.accurate_send_ = accurate_send;
    intf.rdma_.xmode_ = transport;
    intf.rx_.queues_.resize(1);
    intf.tx_.queues_.resize(1);
    intf.rx_.queues_[0].poll_mode_ = direct_rx ? QueuePollMode::DIRECT : QueuePollMode::INDIRECT;
    intf.tx_.queues_[0].poll_mode_ = direct_tx ? QueuePollMode::DIRECT : QueuePollMode::INDIRECT;
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    require(sched_getaffinity(0, sizeof(allowed), &allowed) == 0, "cannot query CPU affinity");
    int cpu = 0;
    while (cpu < CPU_SETSIZE && !CPU_ISSET(cpu, &allowed)) ++cpu;
    intf.rx_.queues_[0].common_.cpu_core_ = std::to_string(cpu);
    intf.tx_.queues_[0].common_.cpu_core_ = std::to_string(cpu);
    tx_ring = Ring::create("rdma_test_tx", 1024, RingMode::MPMC);
    rx_ring = Ring::create("rdma_test_rx", 1024, RingMode::MPMC);
    require(tx_ring && rx_ring, "cannot allocate rings");
    engine.tx_rings_.push(tx_ring);
    engine.rx_rings_.push(rx_ring);
    id.verbs = &context;
    params.client_id = &id;
    params.pd = &pd;
    params.if_idx = params.queue_idx = 0;
    mr.lkey = 123;
    engine.pd_map_[&context] = &pd;
    auto& memory = engine.mrs_["data"];
    memory.params_.kind_ = MemoryKind::HOST_PINNED;
    memory.params_.buf_size_ = 4096;
    memory.params_.access_ = MEM_ACCESS_LOCAL;
    memory.ptr_ = reinterpret_cast<void*>(1);
    memory.params_.adj_size_ = UINT64_MAX - 1;
    memory.params_.num_bufs_ = 1;
    memory.ctx_mr_map_[&pd] = &mr;
    require(engine.setup_thread_params(&params, false) == 0, "QP setup failed");
    if (transport == RDMATransportMode::UD) {
      rdma_ud_param peer{};
      peer.qp_num = 321;
      peer.qkey = 654;
      require(engine.setup_ud_peer(&params, peer), "UD destination setup failed");
    }
    require(fake.requested_inline == (direct_tx ? 128u : 0u), "incorrect inline request");
    require(fake.timestamps == timestamps, "timestamp CQ selection incorrect");
    require(
        !timestamps || accurate_send || (fake.rx_cq_flags & IBV_WC_EX_WITH_COMPLETION_TIMESTAMP),
        "timestamp-enabled RX must retain the legacy TX CQ's device-clock format");
    require(fake.timed_qp == accurate_send && fake.wallclock_requested == accurate_send &&
                fake.requested_send_wr == (accurate_send ? 2048u : 1024u) && fake.created_cqs == 2,
            "accurate SEND did not request a wall-clock CQ and WAIT capacity");
  }

  ~RdmaEnginePollingTestPeer() {
    params.ready_to_exit.store(true);
    for (auto& [id, thread] : engine.worker_threads_) {
      if (thread.joinable()) thread.join();
    }
    engine.worker_threads_.clear();
    engine.connection_params_.clear();
    engine.mrs_.clear();
    engine.pd_map_.clear();
    engine.rx_rings_map_.clear();
    engine.tx_rings_map_.clear();
    // The fixture owns these rings and the fake CM channel. Failure cleanup
    // returns the rings to the engine's available queues, so detach them before
    // freeing the fixture storage and destroying the engine.
    while (!engine.tx_rings_.empty()) engine.tx_rings_.pop();
    while (!engine.rx_rings_.empty()) engine.rx_rings_.pop();
    engine.cm_event_channel_ = nullptr;
    engine.initialized_ = false;
    Ring::free(tx_ring);
    Ring::free(rx_ring);
  }

  void timestamp_errors() {
    if (!fake.timestamps) return;
    ibv_wc wc{};
    uint64_t timestamp_ns = 1;
    const auto ends = fake.ended_polls;
    require(engine.poll_rx_completion(&params, &wc, &timestamp_ns) == 0 &&
                timestamp_ns == UINT64_MAX && fake.ended_polls == ends,
            "empty extended CQ was not handled correctly");
    fake.poll_error = EIO;
    require(engine.poll_rx_completion(&params, &wc, &timestamp_ns) < 0 && fake.ended_polls == ends,
            "extended CQ poll error was ignored or ended an inactive poll");
    fake.poll_error = 0;
    fake.fail_create_cq_ex = true;
    rdma_thread_params unsupported{};
    unsupported.client_id = &id;
    unsupported.if_idx = unsupported.queue_idx = 0;
    require(engine.setup_thread_params(&unsupported, false) != 0,
            "unsupported hardware timestamps silently fell back to ordinary CQ");
    fake.fail_create_cq_ex = false;
    BurstParams burst{};
    require(engine.get_packet_rx_timestamp(nullptr, 0, &timestamp_ns) == Status::NULL_PTR &&
                engine.get_packet_rx_timestamp(&burst, 0, nullptr) == Status::NULL_PTR,
            "timestamp getter did not validate null pointers");
    require(engine.get_packet_rx_timestamp(&burst, 0, &timestamp_ns) == Status::INVALID_PARAMETER,
            "timestamp getter accepted an empty burst");
  }

  void transport_operations(bool server) {
    engine.launch_worker(&params, server);
    params.ready_to_exit.store(true);
    for (auto& [id, thread] : engine.worker_threads_) thread.join();
    engine.worker_threads_.clear();
    params.ready_to_exit.store(false);
    fake.auto_complete = true;
    std::array<char, 2048> bytes{};
    void* pointer = bytes.data();
    uint32_t length = 64;
    for (auto op : {RDMAOpCode::SEND, RDMAOpCode::RECEIVE, RDMAOpCode::RDMA_READ,
                    RDMAOpCode::RDMA_WRITE, RDMAOpCode::RDMA_WRITE_IMM}) {
      BurstParams burst{};
      engine.rdma_set_header(&burst, op, reinterpret_cast<uintptr_t>(&id), server, 1, 9, "data");
      burst.pkts[0] = &pointer;
      burst.pkt_lens[0] = &length;
      burst.transport_hdr.raddr = reinterpret_cast<void*>(4096);
      burst.transport_hdr.dst_key = 456;
      burst.transport_hdr.remote_length = 4096;
      burst.transport_hdr.remote_access = MEM_ACCESS_RDMA_READ | MEM_ACCESS_RDMA_WRITE;
      const bool supported = op == RDMAOpCode::SEND || op == RDMAOpCode::RECEIVE ||
                             (fake.qp.qp_type == IBV_QPT_UC &&
                              (op == RDMAOpCode::RDMA_WRITE || op == RDMAOpCode::RDMA_WRITE_IMM));
      if (!supported) {
        // Keep this stack-backed request caller-owned on synchronous rejection.
        const bool direct = params.direct_tx;
        params.direct_tx = true;
        require(engine.send_tx_burst(&burst) == Status::NOT_SUPPORTED,
                "unsupported transport operation accepted");
        params.direct_tx = direct;
        continue;
      }
      require(engine.send_tx_burst(&burst) == Status::SUCCESS, "supported operation rejected");
      BurstParams* pending = nullptr;
      while (tx_ring->dequeue(reinterpret_cast<void**>(&pending)))
        require(engine.post_burst(&params, pending) == Status::SUCCESS, "transport post failed");
      engine.poll_completions(server, &params, !params.direct_rx, !params.direct_tx);
      BurstParams* result = nullptr;
      require(engine.get_rx_burst(&result, reinterpret_cast<uintptr_t>(&id), server) ==
                      Status::SUCCESS &&
                  result == &burst && length == 64 &&
                  result->transport_hdr.status == Status::SUCCESS,
              "transport completion lost payload length or ownership");
    }
    if (fake.qp.qp_type == IBV_QPT_UD) {
      BurstParams burst{};
      engine.rdma_set_header(&burst, RDMAOpCode::SEND, reinterpret_cast<uintptr_t>(&id), server, 1,
                             10, "data");
      burst.pkts[0] = &pointer;
      burst.pkt_lens[0] = &length;
      length = params.ud_mtu + 1;
      params.direct_tx = true;
      const auto posted = fake.sends.load();
      require(engine.send_tx_burst(&burst) == Status::INVALID_PARAMETER && fake.sends == posted,
              "oversize UD SEND reached the NIC");
    }
    if (params.direct_state) params.direct_state->close();
    engine.destroy_thread_params(&params);
    require(!params.ud_ah && !params.ud_grh_mr &&
                fake.destroyed_ahs == (fake.qp.qp_type == IBV_QPT_UD ? 1u : 0u) &&
                fake.deregistered_grhs == fake.destroyed_ahs && engine.tx_rings_.size() == 1 &&
                engine.rx_rings_.size() == 1,
            "transport teardown leaked AHs, registrations, or queue reservations");
  }

  void check_timestamp(BurstParams* burst, bool receive, uint64_t wr_id) {
    uint64_t timestamp_ns = 1;
    const auto status = engine.get_packet_rx_timestamp(burst, 0, &timestamp_ns);
    const bool supported =
        fake.timestamps && receive && burst->transport_hdr.status == Status::SUCCESS;
    require(status == (supported ? Status::SUCCESS : Status::NOT_SUPPORTED),
            "incorrect timestamp availability");
    require(timestamp_ns == (supported ? FakeVerbs::kTimestamp + wr_id : 0),
            "hardware timestamp was lost, changed, or reused from an earlier completion");
    require(
        engine.get_packet_rx_timestamp(burst, -1, &timestamp_ns) == Status::INVALID_PARAMETER &&
            engine.get_packet_rx_timestamp(burst, 1, &timestamp_ns) == Status::INVALID_PARAMETER,
        "timestamp getter accepted an out-of-range index");
  }

  void batched_timestamps() {
    if (params.direct_rx && params.direct_tx) return;
    std::array<BurstParams, 2> bursts{};
    std::array<uint32_t, 2> lengths{512, 512};
    for (unsigned i = 0; i < 2; ++i) {
      auto& burst = bursts[i];
      require(engine.rdma_set_header(&burst, RDMAOpCode::RECEIVE, reinterpret_cast<uintptr_t>(&id),
                                     false, 1, 500 + i, "data") == Status::SUCCESS,
              "receive header failed");
      burst.pkt_lens[0] = &lengths[i];
      params.outstanding_receive_wr_ids[500 + i] = {&burst, 0};
      params.pending_receives[&burst] = 1;
      ibv_wc wc{};
      wc.wr_id = 500 + i;
      wc.status = IBV_WC_SUCCESS;
      wc.opcode = IBV_WC_RECV;
      wc.byte_len = 37 + i;
      fake.rx_completions.push_back(wc);
    }
    engine.poll_completions(false, &params, true, false);
    for (unsigned i = 0; i < 2; ++i) {
      check_timestamp(&bursts[i], true, 500 + i);
      require(lengths[i] == 37 + i, "receive length changed");
      BurstParams* result = nullptr;
      require(rx_ring->dequeue(reinterpret_cast<void**>(&result)) && result == &bursts[i],
              "completion lost its burst");
    }
    require(
        engine.rdma_set_header(&bursts[0], RDMAOpCode::RECEIVE, reinterpret_cast<uintptr_t>(&id),
                               false, 1, 600, "data") == Status::SUCCESS,
        "reused receive header failed");
    uint64_t timestamp_ns = 1;
    require(engine.get_packet_rx_timestamp(&bursts[0], 0, &timestamp_ns) == Status::NOT_SUPPORTED &&
                timestamp_ns == 0,
            "reused burst retained an old timestamp");
  }

  void setup_failures() {
    engine.destroy_thread_params(&params);
    params.client_id = nullptr;
    engine.initialized_ = true;
    engine.cfg_.ifs_[0].address_ = "127.0.0.1";
    engine.cfg_.ifs_[0].port_id_ = 0;
    engine.pd_params_[&listener].if_idx = 0;
    fake.client.verbs = &context;
    fake.local.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &fake.local.sin_addr);
    // Each phase is retried more often than there are queue slots. Neither
    // client reservations nor server slots may be consumed by a failed setup.
    for (bool server : {false, true}) {
      for (int phase = 0; phase < 5; ++phase) {
        for (int retry = 0; retry < 3; ++retry) {
          fake.created_cqs = fake.destroyed_cqs = 0;
          fake.fail_cq = phase < 2 ? phase : -1;
          fake.fail_qp = phase == 2;
          fake.fail_connect = phase == 3;
          fake.unexpected_event = phase == 4;
          fake.events = fake.connects = fake.accepts = fake.rejects = 0;
          const int destroyed = fake.ids_destroyed;
          const int channels = fake.channels_destroyed;
          if (server) {
            rdma_cm_event event{};
            event.event = RDMA_CM_EVENT_CONNECT_REQUEST;
            event.id = &fake.client;
            event.listen_id = &listener;
            ++fake.pending_events;
            engine.handle_connect_request(&event);
            require(!params.client_id && !params.worker_started && fake.rejects == 1 &&
                        fake.accepts == (phase >= 3 ? 1 : 0),
                    "failed server setup accepted work or retained its queue slot");
          } else {
            uintptr_t conn_id = 123;
            require(engine.rdma_connect_to_server("127.0.0.2", 18515, &conn_id) ==
                            Status::CONNECT_FAILURE &&
                        conn_id == 0 && engine.client_q_params_.empty() &&
                        fake.connects == (phase >= 3 ? 1 : 0) &&
                        fake.channels_destroyed == channels + 1,
                    "failed client setup connected or leaked its reservation/channel");
          }
          require(fake.ids_destroyed == destroyed + 1 && fake.pending_events == 0 &&
                      fake.destroyed_cqs == fake.created_cqs - (phase < 2 ? 1 : 0) &&
                      engine.tx_rings_.size() == 1 && engine.rx_rings_.size() == 1 &&
                      engine.tx_rings_map_.empty() && engine.rx_rings_map_.empty() &&
                      engine.connection_params_.empty() && engine.worker_threads_.empty(),
                  "failed setup leaked CQ/ring/CM resources or published a connection");
        }
      }
    }
    engine.initialized_ = false;
  }

  void receive_ring_pressure(bool server) {
    BurstParams burst{};
    std::array<uint64_t, kRdmaPacketMetadataBytes / sizeof(uint64_t)> metadata{};
    char byte = 0;
    void* pointer = &byte;
    require(engine.rdma_set_header(&burst, RDMAOpCode::RECEIVE, reinterpret_cast<uintptr_t>(&id),
                                   server, 1, 900, "data") == Status::SUCCESS,
            "header failed");
    burst.pkts[0] = &pointer;
    burst.pkt_lens[0] = reinterpret_cast<uint32_t*>(metadata.data());
    require(engine.post_burst(&params, &burst) == Status::SUCCESS, "receive posting failed");
    int sentinel = 0;
    while (rx_ring->enqueue(&sentinel)) {
    }
    ibv_wc wc{};
    wc.wr_id = fake.last_receive_id;
    wc.status = IBV_WC_SUCCESS;
    wc.opcode = IBV_WC_RECV_RDMA_WITH_IMM;
    wc.wc_flags = IBV_WC_WITH_IMM;
    wc.imm_data = htonl(123);
    wc.byte_len = 64;
    fake.rx_completions.push_back(wc);
    engine.poll_completions(server, &params, true, false);
    require(params.pending_rx_burst == &burst && params.outstanding_receive_wr_ids.empty(),
            "a full completion ring discarded the notification");
    engine.poll_completions(server, &params, true, false);
    require(params.pending_rx_burst == &burst, "retry lost a blocked notification");
    void* entry = nullptr;
    while (rx_ring->dequeue(&entry)) require(entry == &sentinel, "ring contents changed");
    engine.poll_completions(server, &params, true, false);
    BurstParams* result = nullptr;
    uint32_t immediate = 0, length = 0;
    require(!params.pending_rx_burst && rx_ring->dequeue(reinterpret_cast<void**>(&result)) &&
                result == &burst &&
                engine.rdma_get_immediate_data(result, &immediate, &length) == Status::SUCCESS &&
                immediate == 123 && length == 64 && rx_ring->count() == 0,
            "completion ring did not recover with exactly one notification");
    check_timestamp(result, true, wc.wr_id);
  }

  void batched_receive_pressure(bool server, int pressured_pool, bool receive_error) {
    require(!params.direct_rx, "batched receive test needs indirect RX");
    engine.connection_params_[&id] = &params;
    constexpr size_t count = 3;
    const size_t metadata_count = pressured_pool == 0 ? 2 : 4;
    const size_t pointer_count = pressured_pool == 1 ? 2 : 4;
    const size_t length_count = pressured_pool == 2 ? 2 : 4;
    engine.tx_meta = ObjectPool::create("pressure_metadata", metadata_count, sizeof(BurstParams));
    engine.tx_burst_pool_ =
        ObjectPool::create("pressure_pointers", pointer_count, count * sizeof(void*));
    engine.pkt_len_pool_ =
        ObjectPool::create("pressure_lengths", length_count, kRdmaPacketMetadataBytes);
    auto* packets = Ring::create("pressure_packets", 4, RingMode::MPMC);
    std::array<std::array<char, 64>, count> storage{};
    for (auto& packet : storage)
      require(packets->enqueue(packet.data()), "packet pool setup failed");
    engine.mem_pools_["data"] = packets;
    auto* parent = engine.create_tx_burst_params();
    require(
        parent &&
            engine.rdma_set_header(parent, RDMAOpCode::RECEIVE, reinterpret_cast<uintptr_t>(&id),
                                   server, count, 700, "data") == Status::SUCCESS &&
            engine.get_tx_packet_burst(parent) == Status::SUCCESS,
        "batch allocation failed");
    for (size_t p = 0; p < count; ++p) parent->pkt_lens[0][p] = 0;
    int sentinel = 0;
    size_t queued_count = 0;
    if (pressured_pool == 3)
      while (tx_ring->enqueue(&sentinel)) ++queued_count;
    const auto submitted = engine.send_tx_burst(parent);
    if (pressured_pool >= 0 && pressured_pool <= 3) {
      require(submitted == Status::NO_SPACE_AVAILABLE && fake.receives == 0 &&
                  tx_ring->count() == queued_count && params.queued_receive_completions.empty() &&
                  packets->count() == count && engine.tx_meta->avail_count() == metadata_count &&
                  engine.tx_burst_pool_->avail_count() == pointer_count &&
                  engine.pkt_len_pool_->avail_count() == length_count,
              "failed reservation accepted work or leaked a partially reserved batch");
      void* entry = nullptr;
      while (tx_ring->dequeue(&entry)) require(entry == &sentinel, "overflow changed queued work");
      // A rejected batch must return enough storage to retry with a smaller receive.
      parent = engine.create_tx_burst_params();
      require(
          parent &&
              engine.rdma_set_header(parent, RDMAOpCode::RECEIVE, reinterpret_cast<uintptr_t>(&id),
                                     server, 1, 700, "data") == Status::SUCCESS &&
              engine.get_tx_packet_burst(parent) == Status::SUCCESS &&
              engine.send_tx_burst(parent) == Status::SUCCESS,
          "receive could not recover after reservation rejection");
      BurstParams* queued = nullptr;
      require(tx_ring->dequeue(reinterpret_cast<void**>(&queued)) && queued == parent,
              "recovery submission was lost");
      engine.free_tx_burst(parent);
    } else {
      require(submitted == Status::SUCCESS && engine.tx_meta->avail_count() == 0 &&
                  engine.tx_burst_pool_->avail_count() == 0 &&
                  engine.pkt_len_pool_->avail_count() == 0,
              "test did not exhaust every shared pool with an accepted batch");
      if (pressured_pool == 4) {
        require(engine.destroy_thread_params(&params) == 0,
                "queued reservation cancellation failed");
      } else {
        BurstParams* queued = nullptr;
        require(tx_ring->dequeue(reinterpret_cast<void**>(&queued)) && queued == parent &&
                    engine.post_burst(&params, queued) == Status::SUCCESS,
                "accepted receive batch did not post");
        if (pressured_pool == 5) {
          require(engine.destroy_thread_params(&params) == 0,
                  "posted reservation cancellation failed");
        } else {
          const uint64_t first_id = fake.last_receive_id - count + 1;
          for (size_t p = 0; p < count; ++p) {
            ibv_wc wc{};
            wc.wr_id = first_id + p;
            wc.status = receive_error && p == 1 ? IBV_WC_LOC_PROT_ERR : IBV_WC_SUCCESS;
            wc.opcode = IBV_WC_RECV_RDMA_WITH_IMM;
            wc.wc_flags = IBV_WC_WITH_IMM;
            wc.imm_data = htonl(100 + p);
            wc.byte_len = 64 + p;
            fake.rx_completions.push_back(wc);
          }
          engine.poll_completions(server, &params, true, false);
          require(params.outstanding_receive_wr_ids.empty() && params.pending_receives.empty() &&
                      rx_ring->count() == count,
                  "accepted receive batch stalled when all shared metadata was reserved");
          for (size_t p = 0; p < count; ++p) {
            BurstParams* result = nullptr;
            require(rx_ring->dequeue(reinterpret_cast<void**>(&result)) && result != parent &&
                        result->transport_hdr.wr_id == 700 + p &&
                        result->transport_hdr.num_pkts == 1 &&
                        result->pkts[0][0] == storage[p].data() && result->pkt_lens[0][0] == 0,
                    "split receive ownership or application ID was lost under pressure");
            const bool failed = receive_error && p == 1;
            require(result->transport_hdr.status ==
                        (failed ? Status::GENERIC_FAILURE : Status::SUCCESS),
                    "saved receive CQE lost its error status");
            check_timestamp(result, !failed, first_id + p);
            if (!failed) {
              uint32_t immediate = 0, written = 0;
              require(
                  engine.rdma_get_immediate_data(result, &immediate, &written) == Status::SUCCESS &&
                      immediate == 100 + p && written == 64 + p,
                  "saved CQE lost its immediate or write length");
            }
            engine.free_tx_burst(result);
          }
        }
      }
    }
    require(params.queued_receive_completions.empty() &&
                params.outstanding_receive_wr_ids.empty() && params.pending_receives.empty() &&
                rx_ring->count() == 0 && packets->count() == count &&
                engine.tx_meta->avail_count() == metadata_count &&
                engine.tx_burst_pool_->avail_count() == pointer_count &&
                engine.pkt_len_pool_->avail_count() == length_count,
            "pool-pressure recovery leaked or duplicated a notification or buffer");
    engine.mem_pools_.erase("data");
    Ring::free(packets);
    ObjectPool::free(engine.tx_meta);
    ObjectPool::free(engine.tx_burst_pool_);
    ObjectPool::free(engine.pkt_len_pool_);
    engine.tx_meta = engine.tx_burst_pool_ = engine.pkt_len_pool_ = nullptr;
  }

  void indirect_overflow(bool server) {
    std::array<char, 4096> registered_bytes{};
    require(!params.direct_tx, "overflow test requires indirect TX");
    const auto connection = reinterpret_cast<uintptr_t>(&id);
    engine.connection_params_[&id] = &params;
    engine.tx_meta = ObjectPool::create("overflow_metadata", 1, sizeof(BurstParams));
    engine.tx_burst_pool_ = ObjectPool::create("overflow_pointers", 1, sizeof(void*));
    engine.pkt_len_pool_ = ObjectPool::create("overflow_lengths", 1, sizeof(uint32_t));
    auto* packets = Ring::create("overflow_packets", 2, RingMode::MPMC);
    engine.mem_pools_["data"] = packets;
    require(engine.tx_meta && engine.tx_burst_pool_ && engine.pkt_len_pool_ && packets,
            "cannot allocate overflow test pools");
    require(packets->enqueue(registered_bytes.data()), "cannot populate overflow packet pool");
    RDMAMemoryRegion remote{4096, 4096, 456};
    int sentinel = 0;
    unsigned queued = 0;
    while (tx_ring->enqueue(&sentinel)) ++queued;
    require(queued != 0, "cannot fill work ring");

    auto allocate = [&](RDMAOpCode opcode, uint64_t wr_id) {
      auto* burst = engine.create_tx_burst_params();
      require(burst != nullptr, "overflow drained the metadata pool");
      require(engine.rdma_set_header(burst, opcode, connection, server, 1, wr_id, "data") ==
                  Status::SUCCESS,
              "overflow test header failed");
      require(engine.get_tx_packet_burst(burst) == Status::SUCCESS,
              "overflow drained a packet or metadata pool");
      burst->pkt_lens[0][0] = 64;
      if (opcode == RDMAOpCode::RDMA_READ || opcode == RDMAOpCode::RDMA_WRITE ||
          opcode == RDMAOpCode::RDMA_WRITE_IMM) {
        require(engine.rdma_set_remote_memory(burst, remote, 512) == Status::SUCCESS,
                "overflow test remote target failed");
      }
      return burst;
    };
    auto reclaimed = [&] {
      require(engine.tx_meta->avail_count() == 1 && engine.tx_burst_pool_->avail_count() == 1 &&
                  engine.pkt_len_pool_->avail_count() == 1 && packets->count() == 1,
              "indirect overflow leaked a burst, packet, pointer array, or length array");
    };
    // One slot in each pool makes any missing free visible on the first failure.
    // Repeated reallocation verifies that rejection never drains any pool.
    for (auto opcode : {RDMAOpCode::SEND, RDMAOpCode::RECEIVE, RDMAOpCode::RDMA_READ,
                        RDMAOpCode::RDMA_WRITE, RDMAOpCode::RDMA_WRITE_IMM}) {
      if (opcode == RDMAOpCode::RECEIVE && params.direct_rx) continue;
      for (uint64_t wr_id = 0; wr_id < 3; ++wr_id) {
        auto* burst = allocate(opcode, wr_id);
        require(engine.send_tx_burst(burst) == Status::NO_SPACE_AVAILABLE,
                "full indirect work ring accepted a burst");
        // NO_SPACE_AVAILABLE consumes the burst; do not access or free it here.
        reclaimed();
      }
    }
    require(fake.sends == 0 && fake.receives == 0 && rx_ring->count() == 0 &&
                tx_ring->count() == queued,
            "overflow posted work or returned a completion for a freed request");
    void* entry = nullptr;
    while (tx_ring->dequeue(&entry)) require(entry == &sentinel, "overflow changed queued work");

    fake.auto_complete = true;
    auto* burst = allocate(RDMAOpCode::SEND, 100);
    require(engine.send_tx_burst(burst) == Status::SUCCESS,
            "submission did not recover after work-ring overflow");
    BurstParams* pending = nullptr;
    require(tx_ring->dequeue(reinterpret_cast<void**>(&pending)) && pending == burst &&
                engine.post_burst(&params, pending) == Status::SUCCESS,
            "recovered work-ring submission was lost");
    engine.poll_completions(server, &params, false, true);
    BurstParams* completed = nullptr;
    require(engine.get_rx_burst(&completed, connection, server) == Status::SUCCESS &&
                completed == burst && completed->transport_hdr.status == Status::SUCCESS,
            "recovered submission did not complete with its original burst");
    engine.free_tx_burst(completed);
    reclaimed();
    engine.mem_pools_.erase("data");
    Ring::free(packets);
    ObjectPool::free(engine.tx_meta);
    ObjectPool::free(engine.tx_burst_pool_);
    ObjectPool::free(engine.pkt_len_pool_);
    engine.tx_meta = engine.tx_burst_pool_ = engine.pkt_len_pool_ = nullptr;
  }

  void immediate_timestamps(bool server) {
    engine.launch_worker(&params, server);
    params.ready_to_exit.store(true);
    for (auto& [id, thread] : engine.worker_threads_) thread.join();
    engine.worker_threads_.clear();
    params.ready_to_exit.store(false);
    BurstParams burst{};
    std::array<uint64_t, kRdmaPacketMetadataBytes / sizeof(uint64_t)> metadata{};
    auto* length = reinterpret_cast<uint32_t*>(metadata.data());
    char data[512]{};
    void* pointer = data;
    for (int outcome : {0, 1, 2}) {
      require(engine.rdma_set_header(&burst, RDMAOpCode::RECEIVE, reinterpret_cast<uintptr_t>(&id),
                                     server, 1, 777, "data") == Status::SUCCESS,
              "header failed");
      burst.pkts[0] = &pointer;
      burst.pkt_lens[0] = length;
      *length = 0;
      uint64_t timestamp = 1;
      require(engine.get_packet_rx_timestamp(&burst, 0, &timestamp) == Status::NOT_SUPPORTED &&
                  timestamp == 0,
              "reused notification retained a timestamp");
      require(engine.send_tx_burst(&burst) == Status::SUCCESS, "receive credit failed");
      BurstParams* pending = nullptr;
      while (tx_ring->dequeue(reinterpret_cast<void**>(&pending)))
        require(engine.post_burst(&params, pending) == Status::SUCCESS, "receive posting failed");
      ibv_wc wc{};
      wc.wr_id = fake.last_receive_id;
      wc.status = outcome == 2 ? IBV_WC_LOC_PROT_ERR : IBV_WC_SUCCESS;
      wc.opcode = IBV_WC_RECV_RDMA_WITH_IMM;
      wc.wc_flags = outcome == 1 ? 0 : IBV_WC_WITH_IMM;
      wc.imm_data = htonl(0x12345678u);
      wc.byte_len = 4096;
      fake.rx_completions.push_back(wc);
      engine.poll_completions(server, &params, !params.direct_rx, false);
      BurstParams* result = nullptr;
      require(engine.get_rx_burst(&result, reinterpret_cast<uintptr_t>(&id), server) ==
                      Status::SUCCESS &&
                  result == &burst && *length == 0 && !result->transport_hdr.tx,
              "notification lost its credit or fabricated local payload");
      require(result->transport_hdr.status == (outcome ? Status::GENERIC_FAILURE : Status::SUCCESS),
              "notification completion status changed");
      check_timestamp(result, true, wc.wr_id);
      uint32_t immediate = 0;
      require(engine.rdma_get_immediate_data(result, &immediate) ==
                  (outcome == 2   ? Status::NOT_SUPPORTED
                   : outcome == 1 ? Status::GENERIC_FAILURE
                                  : Status::SUCCESS),
              "immediate getter accepted a failed notification");
      if (!outcome) {
        require(immediate == 0x12345678u, "immediate byte order changed");
        uint32_t written = 0;
        require(engine.rdma_get_immediate_data(result, &immediate, &written) == Status::SUCCESS &&
                    written == 4096,
                "notification write length was confused with local packet length");
      }
    }
  }

  void timed(bool server, bool enabled, RDMAOpCode operation, bool empty = false) {
    engine.launch_worker(&params, server);
    params.ready_to_exit.store(true);
    for (auto& [id, thread] : engine.worker_threads_) thread.join();
    engine.worker_threads_.clear();
    params.ready_to_exit.store(false);
    std::array<uint64_t, kRdmaPacketMetadataBytes / sizeof(uint64_t)> metadata{};
    std::array<char, 512> data{};
    std::array<void*, 2> pointers{data.data(), data.data()};
    auto* lengths = reinterpret_cast<uint32_t*>(metadata.data());
    lengths[0] = lengths[1] = empty ? 0 : 64;
    BurstParams burst{};
    auto prepare = [&](RDMAOpCode opcode = RDMAOpCode::INVALID, int count = 1) {
      if (opcode == RDMAOpCode::INVALID) opcode = operation;
      engine.rdma_set_header(&burst, opcode, reinterpret_cast<uintptr_t>(&id), server, count, 123,
                             "data");
      burst.pkts[0] = pointers.data();
      burst.pkt_lens[0] = lengths;
      burst.transport_hdr.raddr = reinterpret_cast<void*>(4096);
      burst.transport_hdr.dst_key = 456;
      burst.transport_hdr.remote_length = 4096;
      burst.transport_hdr.remote_access = MEM_ACCESS_RDMA_READ | MEM_ACCESS_RDMA_WRITE;
    };
    prepare();
    require(engine.set_packet_tx_time(nullptr, 0, 1) == Status::INVALID_PARAMETER &&
                engine.set_packet_tx_time(&burst, -1, 1) == Status::INVALID_PARAMETER &&
                engine.set_packet_tx_time(&burst, 1, 1) == Status::INVALID_PARAMETER,
            "invalid timestamp arguments accepted");
    require(engine.set_packet_tx_time(&burst, 0, 0) ==
                (enabled ? Status::SUCCESS : Status::NOT_SUPPORTED),
            "accurate_send option not honored");
    if (!enabled) return;
    const uint64_t when = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count() +
                          100000000ULL;
    require(
        engine.set_packet_tx_time(&burst, 0, when + 4100000000ULL) == Status::INVALID_PARAMETER &&
            engine.set_packet_tx_time(&burst, 0, when - 4200000000ULL) == Status::INVALID_PARAMETER,
        "setter accepted targets outside the cyclic half-window");
    auto submit = [&] {
      require(engine.send_tx_burst(&burst) == Status::SUCCESS, "timed SEND failed");
      BurstParams* pending = nullptr;
      while (tx_ring->dequeue(reinterpret_cast<void**>(&pending))) {
        require(engine.post_burst(&params, pending) == Status::SUCCESS, "worker timed SEND failed");
      }
      // One SEND CQE reclaims the operation; no WAIT success CQE is expected.
      for (size_t p = 0; p < burst.transport_hdr.num_pkts; ++p) {
        ibv_wc wc{};
        wc.wr_id = fake.last_send_id - burst.transport_hdr.num_pkts + 1 + p;
        wc.status = IBV_WC_SUCCESS;
        wc.opcode = IBV_WC_SEND;
        fake.tx_completions.push_back(wc);
      }
      engine.poll_completions(server, &params, false, !params.direct_tx);
      {
        BurstParams* result = nullptr;
        require(engine.get_rx_burst(&result, reinterpret_cast<uintptr_t>(&id), server) ==
                        Status::SUCCESS &&
                    result == &burst,
                "scheduled SEND lost its completion");
      }
    };
    require(engine.set_packet_tx_time(&burst, 0, when) == Status::SUCCESS,
            "timestamp assignment failed");
    submit();
    require(fake.send_times == std::vector<uint64_t>{when}, "timestamp was not posted intact");
    // Header reuse must discard old timestamps, including a cached direct MR.
    prepare();
    submit();
    require(fake.send_times.size() == 1, "reused burst retained a timestamp");
    require(engine.set_packet_tx_time(&burst, 0, when) == Status::SUCCESS &&
                engine.set_packet_tx_time(&burst, 0, 0) == Status::SUCCESS,
            "timestamp clearing failed");
    submit();
    require(fake.send_times.size() == 1, "zero timestamp emitted a WAIT");
    if (!params.direct_tx) {
      prepare(operation, 2);
      require(engine.set_packet_tx_time(&burst, 1, when + 1) == Status::SUCCESS &&
                  rdma_tx_time(&burst, 0) == 0,
              "setting one packet did not initialize the other timestamps");
      submit();
      require(fake.send_times.back() == when + 1 && fake.send_times.size() == 2,
              "indirect per-packet timestamp lost");
    } else {
      prepare();
      engine.set_packet_tx_time(&burst, 0, when);
      fake.send_result = ENOMEM;
      require(engine.send_tx_burst(&burst) == Status::GENERIC_FAILURE &&
                  fake.send_times.size() == 1 && burst.pkts[0] == pointers.data(),
              "failed timed post consumed ownership or committed the WAIT");
      fake.send_result = 0;
      submit();
      require(fake.send_times.back() == when, "retry lost its timestamp");
    }
    prepare(RDMAOpCode::RECEIVE);
    require(engine.set_packet_tx_time(&burst, 0, when) == Status::INVALID_PARAMETER,
            "RECEIVE accepted a TX timestamp");
    prepare();
    params.ready_to_exit.store(true);
    require(engine.set_packet_tx_time(&burst, 0, when) == Status::NOT_READY,
            "disconnected connection accepted scheduling");
  }

  static void configuration() {
    const std::string text = R"yaml(
daqiri:
  cfg:
    version: 1
    stream_type: socket
    master_core: 0
    memory_regions:
      - {name: data, kind: host_pinned, affinity: 0, num_bufs: 16, buf_size: 512}
    interfaces:
      - name: roce
        address: 127.0.0.1
        socket_config: {mode: client, local_addr: 'roce://127.0.0.1'}
        roce_config: {transport_mode: RC}
        rx:
          queues: [{name: rx, id: 0, poll_mode: direct}]
        tx:
          queues: [{name: tx, id: 0, poll_mode: direct}]
)yaml";
    for (bool rx : {false, true}) {
      for (bool tx : {false, true}) {
        auto yaml = YAML::Load(text);
        auto intf = yaml["daqiri"]["cfg"]["interfaces"][0];
        for (const auto& direction : {std::make_pair("rx", rx), std::make_pair("tx", tx)}) {
          if (!direction.second) {
            auto queue = intf[direction.first]["queues"][0];
            queue["poll_mode"] = "indirect";
            queue["cpu_core"] = "0";
            queue["batch_size"] = 1;
          }
        }
        NetworkConfig config;
        require(parse_network_config_from_yaml_string(YAML::Dump(yaml), config) == Status::SUCCESS,
                "RDMA poll modes rejected by parser");
        require(validate_network_config(config), "RDMA poll modes rejected by shared validation");
      }
    }
    for (const auto& direction : {"rx", "tx"}) {
      for (const auto& field : {"cpu_core", "batch_size"}) {
        auto yaml = YAML::Load(text);
        yaml["daqiri"]["cfg"]["interfaces"][0][direction]["queues"][0][field] = 1;
        NetworkConfig config;
        require(parse_network_config_from_yaml_string(YAML::Dump(yaml), config) != Status::SUCCESS,
                "direct RDMA queue accepted a forbidden worker field");
      }
    }
    auto yaml = YAML::Load(text);
    yaml["daqiri"]["cfg"]["interfaces"][0]["socket_config"]["local_addr"] = "udp://127.0.0.1:9999";
    yaml["daqiri"]["cfg"]["interfaces"][0].remove("roce_config");
    NetworkConfig config;
    require(parse_network_config_from_yaml_string(YAML::Dump(yaml), config) != Status::SUCCESS,
            "UDP unexpectedly accepted direct polling");
  }

  void concurrent_worker(bool server) {
    fake.auto_complete = true;
    engine.launch_worker(&params, server);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!params.worker_started.load() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    require(params.worker_started.load(), "concurrent worker failed to start");
    constexpr size_t count = 128;
    std::array<BurstParams, 2 * count> bursts{};
    std::array<void*, 2 * count> pointers{};
    std::array<uint32_t, 2 * count> lengths{};
    std::array<bool, 2 * count> seen{};
    std::array<uint64_t, 2 * count> data{};
    size_t completed = 0;
    for (size_t i = 0; i < bursts.size(); ++i) {
      const bool receive = i % 2 == 0;
      auto& burst = bursts[i];
      pointers[i] = &data[i];
      lengths[i] = sizeof(uint64_t);
      engine.rdma_set_header(&burst, receive ? RDMAOpCode::RECEIVE : RDMAOpCode::SEND,
                             reinterpret_cast<uintptr_t>(&id), server, 1, 10000 + i, "data");
      burst.pkts[0] = &pointers[i];
      burst.pkt_lens[0] = &lengths[i];
      require(engine.send_tx_burst(&burst) == Status::SUCCESS, "concurrent submission failed");
      BurstParams* result = nullptr;
      while (engine.get_rx_burst(&result, reinterpret_cast<uintptr_t>(&id), server) ==
             Status::SUCCESS) {
        const auto index = result->transport_hdr.wr_id - 10000;
        require(index < bursts.size() && !seen[index] && result == &bursts[index],
                "concurrent completion lost ownership or was returned twice");
        require(result->transport_hdr.status == Status::SUCCESS, "concurrent completion failed");
        seen[index] = true;
        ++completed;
      }
    }
    while (completed < bursts.size() && std::chrono::steady_clock::now() < deadline) {
      BurstParams* result = nullptr;
      if (engine.get_rx_burst(&result, reinterpret_cast<uintptr_t>(&id), server) !=
          Status::SUCCESS) {
        std::this_thread::yield();
        continue;
      }
      const auto index = result->transport_hdr.wr_id - 10000;
      require(index < bursts.size() && !seen[index] && result == &bursts[index],
              "concurrent completion lost ownership or was returned twice");
      require(result->transport_hdr.status == Status::SUCCESS, "concurrent completion failed");
      seen[index] = true;
      ++completed;
    }
    require(completed == bursts.size() && fake.sends == count && fake.receives == count,
            "caller and live worker did not complete all mixed-mode work");
    params.ready_to_exit.store(true);
    for (auto& [id, thread] : engine.worker_threads_) thread.join();
    engine.worker_threads_.clear();
  }

  void fully_direct(bool server) {
    auto* state = params.direct_state;
    require(state != nullptr && engine.worker_threads_.empty(), "missing fully direct state");
    const auto polls_before = fake.rx_polls.load() + fake.tx_polls.load();
    BurstParams* empty = reinterpret_cast<BurstParams*>(1);
    require(engine.get_rx_burst(&empty, reinterpret_cast<uintptr_t>(&id), server) ==
                    Status::NOT_READY &&
                empty == nullptr && fake.rx_polls.load() + fake.tx_polls.load() == polls_before,
            "direct empty request tables polled a CQ");
    fake.auto_complete = true;
    BurstParams send{}, receive{};
    std::array<char, 512> data{};
    void* pointer = data.data();
    uint32_t length = 64;
    auto prepare = [&](BurstParams& burst, RDMAOpCode opcode, uint64_t external_id) {
      engine.rdma_set_header(&burst, opcode, reinterpret_cast<uintptr_t>(&id), server, 1,
                             external_id, "data");
      burst.pkts[0] = &pointer;
      burst.pkt_lens[0] = &length;
    };
    auto complete = [&](BurstParams& expected) {
      BurstParams* result = nullptr;
      require(engine.get_rx_burst(&result, reinterpret_cast<uintptr_t>(&id), server) ==
                      Status::SUCCESS &&
                  result == &expected,
              "direct completion not returned");
      check_timestamp(result, !result->transport_hdr.tx, fake.current_wc.wr_id);
      require(rx_ring->count() == 0 && params.outstanding_send_wr_ids.empty() &&
                  params.outstanding_receive_wr_ids.empty(),
              "direct path used a handoff ring or request map");
    };
    // Reuse the same caller-owned handles after completion, wrap both hardware
    // request tables, and preserve deliberately nonmonotonic application IDs.
    for (uint64_t i = 0; i < RdmaDirectState::kRequests + 17; ++i) {
      prepare(send, RDMAOpCode::SEND, 100000 - i);
      require(engine.send_tx_burst(&send) == Status::SUCCESS, "direct SEND failed");
      require(fake.last_flags & IBV_SEND_INLINE, "direct small SEND not inline");
      complete(send);
      require(send.transport_hdr.wr_id == 100000 - i, "application SEND ID changed");
      prepare(receive, RDMAOpCode::RECEIVE, i % 13);
      require(engine.send_tx_burst(&receive) == Status::SUCCESS, "direct RECEIVE failed");
      complete(receive);
      require(receive.transport_hdr.wr_id == i % 13, "application RECEIVE ID changed");
    }
    // RX stays ready across every call by reposting each returned receive.
    // SEND must still complete promptly, even beyond the request-table capacity.
    // Run the mirror case too: a busy TX CQ must not starve receives.
    for (bool busy_tx : {false, true}) {
      auto& busy = busy_tx ? send : receive;
      auto& target = busy_tx ? receive : send;
      const auto busy_opcode = busy_tx ? RDMAOpCode::SEND : RDMAOpCode::RECEIVE;
      const auto target_opcode = busy_tx ? RDMAOpCode::RECEIVE : RDMAOpCode::SEND;
      prepare(busy, busy_opcode, 1);
      require(engine.send_tx_burst(&busy) == Status::SUCCESS, "busy CQ posting failed");
      for (uint64_t i = 0; i < RdmaDirectState::kRequests + 17; ++i) {
        prepare(target, target_opcode, 2 + i);
        require(engine.send_tx_burst(&target) == Status::SUCCESS, "target CQ posting failed");
        bool completed = false;
        for (int poll = 0; poll < 2 && !completed; ++poll) {
          BurstParams* result = nullptr;
          require(engine.get_rx_burst(&result, reinterpret_cast<uintptr_t>(&id), server) ==
                          Status::SUCCESS &&
                      (result == &busy || result == &target) &&
                      result->transport_hdr.status == Status::SUCCESS,
                  "busy CQ progress failed");
          if (result == &target) {
            completed = true;
          } else {
            prepare(busy, busy_opcode, 1);
            require(engine.send_tx_burst(&busy) == Status::SUCCESS, "busy CQ repost failed");
          }
        }
        require(completed, "continuously ready CQ starved the other direction");
      }
      complete(busy);
      require(state->receive_count == 0 && state->sends_posted == state->sends_completed &&
                  state->ready_head == state->ready_tail,
              "fair CQ polling lost completions or request credits");
    }
    for (auto kind :
         {MemoryKind::HOST_PINNED, MemoryKind::HUGE, MemoryKind::HOST, MemoryKind::DEVICE}) {
      engine.mrs_["data"].params_.kind_ = kind;
      for (uint32_t size : {160u, 161u}) {
        length = size;
        prepare(send, RDMAOpCode::SEND, 99);
        require(engine.send_tx_burst(&send) == Status::SUCCESS, "direct boundary SEND failed");
        const bool expected =
            size <= 160 && (kind == MemoryKind::HOST_PINNED || kind == MemoryKind::HUGE);
        require(bool(fake.last_flags & IBV_SEND_INLINE) == expected,
                "direct inline boundary wrong");
        complete(send);
      }
    }
    engine.mrs_["data"].params_.kind_ = MemoryKind::HOST_PINNED;
    fake.auto_complete = false;
    for (auto opcode : {RDMAOpCode::SEND, RDMAOpCode::RECEIVE}) {
      auto& burst = opcode == RDMAOpCode::SEND ? send : receive;
      prepare(burst, opcode, 123);
      require(engine.send_tx_burst(&burst) == Status::SUCCESS, "error test posting failed");
      ibv_wc wc{};
      wc.wr_id = opcode == RDMAOpCode::SEND ? fake.last_send_id : fake.last_receive_id;
      wc.status = IBV_WC_LOC_PROT_ERR;
      wc.opcode = IBV_WC_RDMA_READ;
      (opcode == RDMAOpCode::SEND ? fake.tx_completions : fake.rx_completions).push_back(wc);
      complete(burst);
      require(burst.transport_hdr.status == Status::GENERIC_FAILURE,
              "direct failed CQE lost error status");
    }
    // Fill both fixed tables, retain ownership on backpressure, then reclaim
    // every slot. Application IDs are unrelated to the internal CQE tags.
    {
      constexpr size_t capacity = RdmaDirectState::kRequests;
      std::vector<BurstParams> work(capacity + 1);
      std::vector<void*> pointers(capacity + 1, data.data());
      std::vector<uint32_t> lengths(capacity + 1, 64);
      for (auto opcode : {RDMAOpCode::SEND, RDMAOpCode::RECEIVE}) {
        const uint64_t first_tag =
            opcode == RDMAOpCode::SEND ? state->sends_posted + 1 : state->receives_posted + 1;
        for (size_t i = 0; i <= capacity; ++i) {
          engine.rdma_set_header(&work[i], opcode, reinterpret_cast<uintptr_t>(&id), server, 1,
                                 capacity - i, "data");
          work[i].pkts[0] = &pointers[i];
          work[i].pkt_lens[0] = &lengths[i];
          const auto status = engine.send_tx_burst(&work[i]);
          require(status == (i < capacity ? Status::SUCCESS : Status::NO_SPACE_AVAILABLE),
                  "direct queue credit limit was not enforced");
          require(work[i].pkts[0] == &pointers[i], "backpressure consumed caller ownership");
        }
        for (size_t i = 0; i < capacity; ++i) {
          ibv_wc wc{};
          wc.wr_id = first_tag + i;
          wc.status = IBV_WC_SUCCESS;
          wc.opcode = opcode == RDMAOpCode::SEND ? IBV_WC_SEND : IBV_WC_RECV;
          wc.byte_len = 64;
          (opcode == RDMAOpCode::SEND ? fake.tx_completions : fake.rx_completions).push_back(wc);
          complete(work[i]);
          require(work[i].transport_hdr.wr_id == capacity - i,
                  "credit reclamation changed caller ID");
        }
      }
      require(state->receive_count == 0 && state->sends_posted == state->sends_completed,
              "full tables did not reclaim all credits");
    }
    // One signaled CQE must return the preceding unsignaled SENDs through the
    // local completion array without a producer/consumer ring.
    {
      std::array<BurstParams, 16> work{};
      params.send_signal_every = 16;
      for (size_t i = 0; i < work.size(); ++i) {
        prepare(work[i], RDMAOpCode::SEND, 1000 - i);
        require(engine.send_tx_burst(&work[i]) == Status::SUCCESS, "unsignaled posting failed");
        require(bool(fake.last_flags & IBV_SEND_SIGNALED) == (i == 15),
                "incorrect signaling cadence");
      }
      ibv_wc wc{};
      wc.wr_id = fake.last_send_id;
      wc.status = IBV_WC_SUCCESS;
      wc.opcode = IBV_WC_SEND;
      fake.tx_completions.push_back(wc);
      for (auto& burst : work) complete(burst);
      params.send_signal_every = 1;
    }
    fake.send_result = ENOMEM;
    prepare(send, RDMAOpCode::SEND, 456);
    require(engine.send_tx_burst(&send) == Status::GENERIC_FAILURE && send.pkts[0] == &pointer,
            "direct failed post consumed caller ownership");
    fake.send_result = 0;
    send.transport_hdr.num_pkts = 2;
    require(engine.send_tx_burst(&send) == Status::INVALID_PARAMETER,
            "direct accepted multiple packets");
    prepare(send, RDMAOpCode::SEND, 789);
    auto other = std::async(std::launch::async, [&] {
      BurstParams* result = nullptr;
      return engine.get_rx_burst(&result, reinterpret_cast<uintptr_t>(&id), server);
    });
    require(other.get() == Status::NOT_READY, "direct connection accepted a second owner thread");
    std::future<void> closer;
    {
      RdmaDirectLease active(state);
      require(bool(active), "could not acquire direct lifetime guard");
      closer = std::async(std::launch::async, [&] { state->close(); });
      while (!(state->gate.load() & RdmaDirectState::kClosed)) std::this_thread::yield();
      require(closer.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout,
              "disconnect did not wait for active direct reader");
    }
    closer.get();
    require(engine.send_tx_burst(&send) == Status::NOT_READY,
            "closed direct connection accepted work");
    // Reuse the control-plane slot while retaining the closed incarnation.
    rdma_cm_id next = id;
    params.client_id = &next;
    require(engine.publish_direct(&params), "could not republish reused server slot");
    require(engine.send_tx_burst(&send) == Status::NOT_READY,
            "old cached handle entered reused connection");
    params.client_id = &id;
    params.direct_state->close();
  }

  void run(bool direct_rx, bool direct_tx, bool server) {
    engine.launch_worker(&params, server);
    // Repeated launch events must not create a second worker or reset state.
    engine.launch_worker(&params, server);
    if (direct_rx && direct_tx) {
      require(engine.worker_threads_.empty(), "fully direct connection spawned a worker");
      require(params.worker_started.load(), "direct connection was not published ready");
    } else {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (!params.worker_started.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
      }
      require(params.worker_started.load(), "indirect worker did not start");
      require(engine.worker_threads_.size() == 1, "incorrect number of workers");
    }

    if (server) {
      engine.server_str_to_id_["127.0.0.1:9999"] = &listener;
      uintptr_t connection = 0;
      require(engine.rdma_get_server_conn_id("127.0.0.1", 9999, &connection) == Status::SUCCESS &&
                  connection == reinterpret_cast<uintptr_t>(&id),
              "server connection was not discoverable");
    }

    if (direct_rx && direct_tx) {
      fully_direct(server);
      return;
    }

    // Pause the worker for deterministic posting and completion assertions.
    params.ready_to_exit.store(true);
    for (auto& [id, thread] : engine.worker_threads_) thread.join();
    engine.worker_threads_.clear();
    params.ready_to_exit.store(false);
    fake.rx_polls.store(0);
    fake.tx_polls.store(0);
    BurstParams* result = reinterpret_cast<BurstParams*>(1);
    require(
        engine.get_rx_burst(&result, reinterpret_cast<uintptr_t>(&id), server) == Status::NOT_READY,
        "empty completion poll should return NOT_READY");
    require(result == nullptr, "empty completion poll left a stale pointer");
    require((fake.rx_polls > 0) == direct_rx && (fake.tx_polls > 0) == direct_tx,
            "caller polled an indirect CQ or failed to poll a direct CQ");

    BurstParams send{}, receive{};
    std::array<char, 512> bytes{};
    void* send_ptr = bytes.data();
    void* recv_ptr = bytes.data();
    uint32_t send_len = 64, recv_len = 512;
    engine.rdma_set_header(&send, RDMAOpCode::SEND, reinterpret_cast<uintptr_t>(&id), server, 1, 10,
                           "data");
    engine.rdma_set_header(&receive, RDMAOpCode::RECEIVE, reinterpret_cast<uintptr_t>(&id), server,
                           1, 20, "data");
    send.pkts[0] = &send_ptr;
    send.pkt_lens[0] = &send_len;
    receive.pkts[0] = &recv_ptr;
    receive.pkt_lens[0] = &recv_len;
    require(engine.send_tx_burst(&send) == Status::SUCCESS, "SEND submission failed");
    require(engine.send_tx_burst(&receive) == Status::SUCCESS, "RECEIVE submission failed");
    require(fake.sends == static_cast<unsigned>(direct_tx), "SEND did not follow TX mode");
    require(fake.receives == static_cast<unsigned>(direct_rx), "RECEIVE did not follow RX mode");
    // Drive one worker iteration explicitly: only indirect operations enter its ring.
    BurstParams* pending = nullptr;
    while (tx_ring->dequeue(reinterpret_cast<void**>(&pending))) {
      require(engine.post_burst(&params, pending) == Status::SUCCESS, "worker posting failed");
    }
    require(fake.sends == 1 && fake.receives == 1, "work was lost or posted twice");
    require(bool(fake.last_flags & IBV_SEND_INLINE) == direct_tx,
            "small SEND inline policy incorrect");
    require(fake.last_flags & IBV_SEND_SIGNALED, "inline SEND lost completion signaling");

    ibv_wc rx_wc{};
    rx_wc.status = IBV_WC_SUCCESS;
    rx_wc.opcode = IBV_WC_RECV;
    rx_wc.wr_id = fake.last_receive_id;
    rx_wc.byte_len = 37;
    fake.rx_completions.push_back(rx_wc);
    ibv_wc tx_wc{};
    tx_wc.status = IBV_WC_SUCCESS;
    tx_wc.opcode = IBV_WC_SEND;
    tx_wc.wr_id = fake.last_send_id;
    fake.tx_completions.push_back(tx_wc);
    // Worker touches only indirect CQs; caller must finish the direct ones.
    engine.poll_completions(server, &params, !direct_rx, !direct_tx);
    bool got_tx = false, got_rx = false;
    for (int i = 0; i < 2; ++i) {
      require(
          engine.get_rx_burst(&result, reinterpret_cast<uintptr_t>(&id), server) == Status::SUCCESS,
          "completion missing");
      if (result == &send) {
        require(result->transport_hdr.tx, "TX completion mislabeled");
        check_timestamp(result, false, 10);
        got_tx = true;
      } else if (result == &receive) {
        require(!result->transport_hdr.tx && recv_len == 37,
                "RX completion length/ownership incorrect");
        check_timestamp(result, true, fake.last_receive_id);
        got_rx = true;
      } else {
        require(false, "unexpected completion object");
      }
    }
    require(got_tx && got_rx && params.outstanding_send_wr_ids.empty() &&
                params.outstanding_receive_wr_ids.empty(),
            "completion credits not reclaimed");

    send.transport_hdr.wr_id = 30;
    receive.transport_hdr.wr_id = 40;
    params.outstanding_send_wr_ids.posted(&send, 0);
    params.outstanding_receive_wr_ids[40] = {&receive, 0};
    params.pending_receives[&receive] = 1;
    tx_wc.wr_id = params.outstanding_send_wr_ids.next_id() - 1;
    tx_wc.status = IBV_WC_LOC_PROT_ERR;
    tx_wc.opcode = IBV_WC_RDMA_READ;
    rx_wc.wr_id = 40;
    rx_wc.status = IBV_WC_LOC_PROT_ERR;
    rx_wc.opcode = IBV_WC_RDMA_READ;
    fake.tx_completions.push_back(tx_wc);
    fake.rx_completions.push_back(rx_wc);
    engine.poll_completions(server, &params, !direct_rx, !direct_tx);
    for (int i = 0; i < 2; ++i) {
      require(engine.get_rx_burst(&result, reinterpret_cast<uintptr_t>(&id), server) ==
                      Status::SUCCESS &&
                  result->transport_hdr.status == Status::GENERIC_FAILURE,
              "failed work request did not return its buffer with error status");
      check_timestamp(result, result == &receive, 40);
    }
    require(params.outstanding_send_wr_ids.empty() && params.outstanding_receive_wr_ids.empty(),
            "failed work request leaked completion credits");

    if (direct_tx) {
      // Test the provider-returned boundary and CPU/GPU memory selection.
      for (auto kind :
           {MemoryKind::HOST_PINNED, MemoryKind::HUGE, MemoryKind::HOST, MemoryKind::DEVICE}) {
        engine.mrs_["data"].params_.kind_ = kind;
        for (uint32_t length : {160u, 161u}) {
          send_len = length;
          ++send.transport_hdr.wr_id;
          require(engine.send_tx_burst(&send) == Status::SUCCESS, "boundary SEND failed");
          const bool expected =
              length <= 160 && (kind == MemoryKind::HOST_PINNED || kind == MemoryKind::HUGE);
          require(bool(fake.last_flags & IBV_SEND_INLINE) == expected,
                  "inline capacity/memory-kind selection wrong");
          params.outstanding_send_wr_ids.clear();
        }
      }
      const unsigned posted = fake.sends;
      send.transport_hdr.num_pkts = 2;
      require(engine.send_tx_burst(&send) == Status::INVALID_PARAMETER && fake.sends == posted,
              "direct TX accepted a multi-packet burst");
    }
    send.transport_hdr.num_pkts = 1;
    if (direct_tx) {
      fake.send_result = ENOMEM;
      require(engine.send_tx_burst(&send) == Status::GENERIC_FAILURE && send.pkts[0] == &send_ptr &&
                  params.outstanding_send_wr_ids.empty(),
              "failed direct SEND consumed caller ownership");
      fake.send_result = 0;
    }
    if (direct_rx) {
      fake.recv_result = ENOMEM;
      require(engine.send_tx_burst(&receive) == Status::GENERIC_FAILURE &&
                  receive.pkts[0] == &recv_ptr && params.outstanding_receive_wr_ids.empty(),
              "failed direct RECEIVE consumed caller ownership");
      fake.recv_result = 0;
    }
    params.ready_to_exit.store(true);
    const unsigned sends = fake.sends;
    require(engine.send_tx_burst(&send) == Status::NOT_READY && fake.sends == sends,
            "disconnected connection accepted work");
    require(
        engine.get_rx_burst(&result, reinterpret_cast<uintptr_t>(&id), server) == Status::NOT_READY,
        "disconnected connection polled a CQ");
  }
};
}  // namespace daqiri

int main() {
  try {
    daqiri::RdmaEnginePollingTestPeer::configuration();
    for (auto transport : {daqiri::RDMATransportMode::UC, daqiri::RDMATransportMode::UD}) {
      for (bool rx : {false, true}) {
        for (bool tx : {false, true}) {
          for (bool server : {false, true}) {
            daqiri::RdmaEnginePollingTestPeer test(rx, tx, false, true, true, transport);
            test.transport_operations(server);
          }
        }
      }
    }
    for (bool rx : {false, true}) {
      for (bool tx : {false, true}) {
        for (bool enabled : {false, true}) {
          daqiri::RdmaEnginePollingTestPeer failures(rx, tx, enabled);
          failures.setup_failures();
        }
        for (bool server : {false, true}) {
          if (!rx) {
            {
              daqiri::RdmaEnginePollingTestPeer full_ring(rx, tx, true, true);
              full_ring.receive_ring_pressure(server);
            }
            for (int pool : {-1, 0, 1, 2, 3, 4, 5}) {
              for (bool timestamps : {false, true}) {
                for (bool accurate : {false, true}) {
                  for (bool error : {false, true}) {
                    daqiri::RdmaEnginePollingTestPeer pressure(rx, tx, accurate, timestamps);
                    pressure.batched_receive_pressure(server, pool, error);
                  }
                }
              }
            }
          }
          if (!tx) {
            daqiri::RdmaEnginePollingTestPeer overflow(rx, tx);
            overflow.indirect_overflow(server);
          }
          for (bool enabled : {false, true}) {
            for (auto opcode :
                 {daqiri::RDMAOpCode::SEND, daqiri::RDMAOpCode::RDMA_READ,
                  daqiri::RDMAOpCode::RDMA_WRITE, daqiri::RDMAOpCode::RDMA_WRITE_IMM}) {
              daqiri::RdmaEnginePollingTestPeer timed(rx, tx, enabled);
              timed.timed(server, enabled, opcode);
            }
            daqiri::RdmaEnginePollingTestPeer empty(rx, tx, enabled);
            empty.timed(server, enabled, daqiri::RDMAOpCode::RDMA_WRITE_IMM, true);
          }
        }
      }
    }
    for (bool accurate : {false, true}) {
      for (bool realtime_supported : {false, true}) {
        if (accurate && !realtime_supported) continue;
        for (bool timestamps : {false, true}) {
          for (bool rx : {false, true}) {
            for (bool tx : {false, true}) {
              for (bool server : {false, true}) {
                {
                  daqiri::RdmaEnginePollingTestPeer notification(rx, tx, accurate, timestamps,
                                                                 realtime_supported);
                  notification.immediate_timestamps(server);
                }
                daqiri::RdmaEnginePollingTestPeer test(rx, tx, accurate, timestamps,
                                                       realtime_supported);
                test.timestamp_errors();
                test.batched_timestamps();
                test.run(rx, tx, server);
                if (!rx || !tx) {
                  daqiri::RdmaEnginePollingTestPeer concurrent(rx, tx, accurate, timestamps,
                                                               realtime_supported);
                  concurrent.concurrent_worker(server);
                }
              }
            }
          }
        }
      }
    }
    std::cout << "PASS: RDMA direct/indirect RX/TX, inline selection, and completions\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
