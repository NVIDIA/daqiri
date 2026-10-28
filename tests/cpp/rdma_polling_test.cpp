// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <chrono>
#include <mutex>
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
  ibv_qp qp{};
  std::deque<ibv_wc> rx_completions, tx_completions;
  std::atomic<unsigned> rx_polls{0}, tx_polls{0}, sends{0}, receives{0};
  std::mutex mutex;
  bool auto_complete = false;
  unsigned last_flags = 0;
  int send_result = 0, recv_result = 0;
  uint32_t requested_inline = 0;
  int created_cqs = 0;
  static FakeVerbs* current;

  static ibv_cq* create_cq(ibv_context*, int, void*, ibv_comp_channel*, int) {
    return current->created_cqs++ == 0 ? &current->rx : &current->tx;
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
  RdmaEnginePollingTestPeer(bool direct_rx, bool direct_tx)
      : params(engine.server_q_params_.try_emplace(&listener, 1).first->second[0]) {
    FakeVerbs::current = &fake;
    api.create_cq = &FakeVerbs::create_cq;
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
    memory.ctx_mr_map_[&pd] = &mr;
    require(engine.setup_thread_params(&params, false) == 0, "QP setup failed");
    require(fake.requested_inline == (direct_tx ? 128u : 0u), "incorrect inline request");
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
        got_tx = true;
      } else if (result == &receive) {
        require(!result->transport_hdr.tx && recv_len == 37,
                "RX completion length/ownership incorrect");
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
    for (bool rx : {false, true}) {
      for (bool tx : {false, true}) {
        for (bool server : {false, true}) {
          daqiri::RdmaEnginePollingTestPeer test(rx, tx);
          test.run(rx, tx, server);
          if (!rx || !tx) {
            daqiri::RdmaEnginePollingTestPeer concurrent(rx, tx);
            concurrent.concurrent_worker(server);
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
