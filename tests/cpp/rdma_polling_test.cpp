// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
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
namespace {
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

struct FakeVerbs {
  ibv_cq rx{}, tx{};
  ibv_cq_ex rx_ex{};
  ibv_wc current_wc{};
  static constexpr uint64_t kTimestamp = 1780000000123456789ULL;
  bool timestamps = false;
  bool fail_create_cq_ex = false;
  int poll_error = 0;
  unsigned ended_polls = 0;
  ibv_qp qp{};
  std::deque<ibv_wc> rx_completions, tx_completions;
  std::atomic<unsigned> rx_polls{0}, tx_polls{0}, sends{0}, receives{0};
  std::mutex mutex;
  bool auto_complete = false;
  uint64_t last_send_id = 0, last_receive_id = 0;
  unsigned last_flags = 0;
  int send_result = 0, recv_result = 0;
  uint32_t requested_inline = 0;
  int created_cqs = 0;
  static FakeVerbs* current;

  static ibv_cq* create_cq(ibv_context*, int, void*, ibv_comp_channel*, int) {
    return current->created_cqs++ == 0 ? &current->rx : &current->tx;
  }
  static ibv_cq_ex* create_cq_ex(ibv_context*, ibv_cq_init_attr_ex* attr) {
    require(
        attr->wc_flags == (IBV_WC_EX_WITH_BYTE_LEN | IBV_WC_EX_WITH_COMPLETION_TIMESTAMP_WALLCLOCK),
        "RX CQ did not request hardware wall-clock timestamps and byte lengths");
    if (current->fail_create_cq_ex) {
      errno = EOPNOTSUPP;
      return nullptr;
    }
    ++current->created_cqs;
    current->timestamps = true;
    return &current->rx_ex;
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
  static uint32_t read_byte_len(ibv_cq_ex*) {
    require(current->current_wc.status == IBV_WC_SUCCESS, "read length on failed CQE");
    return current->current_wc.byte_len;
  }
  static uint64_t read_timestamp(ibv_cq_ex*) {
    require(current->current_wc.status == IBV_WC_SUCCESS, "read timestamp on failed CQE");
    return kTimestamp + current->current_wc.wr_id;
  }
  static int create_qp(rdma_cm_id* id, ibv_pd*, ibv_qp_init_attr* attr) {
    current->requested_inline = attr->cap.max_inline_data;
    // The provider can return more capacity than requested.
    attr->cap.max_inline_data = attr->cap.max_inline_data ? 160 : 0;
    id->qp = &current->qp;
    return 0;
  }
  static int poll_cq(ibv_cq* cq, int, ibv_wc* wc) {
    std::lock_guard<std::mutex> lock(current->mutex);
    const bool rx = cq == &current->rx;
    ++(rx ? current->rx_polls : current->tx_polls);
    auto& completions = rx ? current->rx_completions : current->tx_completions;
    if (completions.empty()) return 0;
    *wc = completions.front();
    completions.pop_front();
    return 1;
  }
  static int post_send(ibv_qp*, ibv_send_wr* wr, ibv_send_wr**) {
    std::lock_guard<std::mutex> lock(current->mutex);
    require(wr->opcode == IBV_WR_SEND && wr->num_sge == 1, "incorrect SEND encoding");
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
  static int post_recv(ibv_qp*, ibv_recv_wr* wr, ibv_recv_wr**) {
    std::lock_guard<std::mutex> lock(current->mutex);
    require(wr->num_sge == 1, "incorrect RECEIVE encoding");
    if (current->recv_result) {
      return current->recv_result;
    }
    current->last_receive_id = wr->wr_id;
    if (current->auto_complete) {
      ibv_wc wc{};
      wc.wr_id = wr->wr_id;
      wc.status = IBV_WC_SUCCESS;
      wc.opcode = IBV_WC_RECV;
      wc.byte_len = wr->sg_list->length;
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
  RdmaEnginePollingTestPeer(bool direct_rx, bool direct_tx, bool timestamps = false)
      : params(engine.server_q_params_.try_emplace(&listener, 1).first->second[0]) {
    FakeVerbs::current = &fake;
    api.create_cq = &FakeVerbs::create_cq;
    api.create_cq_ex = &FakeVerbs::create_cq_ex;
    api.start_poll = &FakeVerbs::start_poll;
    api.end_poll = &FakeVerbs::end_poll;
    api.wc_read_opcode = &FakeVerbs::read_opcode;
    api.wc_read_byte_len = &FakeVerbs::read_byte_len;
    api.wc_read_completion_wallclock_ns = &FakeVerbs::read_timestamp;
    api.cm_create_qp = &FakeVerbs::create_qp;
    api.poll_cq = &FakeVerbs::poll_cq;
    api.post_send = &FakeVerbs::post_send;
    api.post_recv = &FakeVerbs::post_recv;
    api.modify_qp = &FakeVerbs::modify_qp;
    api.query_qp = &FakeVerbs::query_qp;
    api.wc_status_str = &FakeVerbs::wc_status_str;
    engine.verbs_ = &api;
    engine.cfg_.ifs_.resize(1);
    auto& intf = engine.cfg_.ifs_[0];
    intf.rx_.hardware_timestamps_ = timestamps;
    intf.rdma_.xmode_ = RDMATransportMode::RC;
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
    memory.ctx_mr_map_[&pd] = &mr;
    require(engine.setup_thread_params(&params, false) == 0, "QP setup failed");
    require(fake.requested_inline == (direct_tx ? 128u : 0u), "incorrect inline request");
    require(fake.timestamps == timestamps, "timestamp CQ selection incorrect");
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
    BurstParams burst{};
    std::array<uint64_t, 2> timestamps{};
    std::array<uint32_t, 2> lengths{512, 512};
    require(engine.rdma_set_header(&burst, RDMAOpCode::RECEIVE, reinterpret_cast<uintptr_t>(&id),
                                   false, 2, 500, "data") == Status::SUCCESS,
            "batched RECEIVE header failed");
    burst.pkt_lens[0] = lengths.data();
    rdma_burst_cache(&burst).rx_timestamps = timestamps.data();
    rdma_clear_rx_timestamps(&burst);
    for (unsigned i = 0; i < 2; ++i) {
      params.outstanding_receive_wr_ids[500 + i] = &burst;
      ibv_wc wc{};
      wc.wr_id = 500 + i;
      wc.status = IBV_WC_SUCCESS;
      wc.opcode = IBV_WC_RECV;
      wc.byte_len = 37 + i;
      fake.rx_completions.push_back(wc);
    }
    engine.poll_completions(false, &params, true, false);
    for (unsigned i = 0; i < 2; ++i) {
      uint64_t timestamp_ns = 1;
      require(engine.get_packet_rx_timestamp(&burst, i, &timestamp_ns) ==
                      (fake.timestamps ? Status::SUCCESS : Status::NOT_SUPPORTED) &&
                  timestamp_ns == (fake.timestamps ? FakeVerbs::kTimestamp + 500 + i : 0) &&
                  lengths[i] == 37 + i,
              "batched CQE timestamp/length did not use the packet's WR ID");
      BurstParams* result = nullptr;
      require(rx_ring->dequeue(reinterpret_cast<void**>(&result)) && result == &burst,
              "batched completion lost its burst");
    }
    require(engine.rdma_set_header(&burst, RDMAOpCode::RECEIVE, reinterpret_cast<uintptr_t>(&id),
                                   false, 2, 600, "data") == Status::SUCCESS,
            "reused batched RECEIVE header failed");
    uint64_t timestamp_ns = 1;
    require(engine.get_packet_rx_timestamp(&burst, 1, &timestamp_ns) == Status::NOT_SUPPORTED &&
                timestamp_ns == 0,
            "reused burst retained an old timestamp");
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
    rx_wc.wr_id = 20;
    rx_wc.byte_len = 37;
    fake.rx_completions.push_back(rx_wc);
    ibv_wc tx_wc{};
    tx_wc.status = IBV_WC_SUCCESS;
    tx_wc.opcode = IBV_WC_SEND;
    tx_wc.wr_id = 10;
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
        check_timestamp(result, true, 20);
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
    params.outstanding_send_wr_ids[30] = &send;
    params.outstanding_receive_wr_ids[40] = &receive;
    tx_wc.wr_id = 30;
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
    for (bool timestamps : {false, true}) {
      for (bool rx : {false, true}) {
        for (bool tx : {false, true}) {
          for (bool server : {false, true}) {
            daqiri::RdmaEnginePollingTestPeer test(rx, tx, timestamps);
            test.timestamp_errors();
            test.batched_timestamps();
            test.run(rx, tx, server);
            if (!rx || !tx) {
              daqiri::RdmaEnginePollingTestPeer concurrent(rx, tx, timestamps);
              concurrent.concurrent_worker(server);
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
