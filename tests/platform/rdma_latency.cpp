// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
// Hardware-only RC SEND ping-pong: preallocated registered handles, zero-copy
// server echo, and payload verification after send completion.
#include <daqiri/daqiri.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <pthread.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <sched.h>
#include <stdexcept>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
void check(daqiri::Status s, const char* action) {
  if (s != daqiri::Status::SUCCESS)
    throw std::runtime_error(std::string(action) + " status=" + std::to_string(int(s)));
}
int main(int argc, char** argv) {
  if (argc != 10) {
    std::cerr << "config server bytes warmup samples app_cpu client_ip server_ip output\n";
    return 2;
  }
  try {
    const bool server = std::stoi(argv[2]);
    const int bytes = std::stoi(argv[3]), warmup = std::stoi(argv[4]), samples = std::stoi(argv[5]);
    const int cpu = std::stoi(argv[6]);
    if (bytes < 8 || warmup < 0 || samples <= 0 ||
        samples > std::numeric_limits<int>::max() - warmup || cpu < 0 || cpu >= CPU_SETSIZE) {
      throw std::runtime_error("invalid message size, sample counts, or CPU index");
    }
    // One ping is outstanding, and each buffer is reused after SEND completion.
    // An unsignaled first SEND cannot be released by a later request in this test.
    const char* signal_every = std::getenv("DAQIRI_RDMA_SEND_SIGNAL_EVERY");
    if (signal_every && std::string(signal_every) != "1") {
      throw std::runtime_error("rdma_latency requires DAQIRI_RDMA_SEND_SIGNAL_EVERY=1 (or unset)");
    }
    const int total = warmup + samples;
    daqiri::NetworkConfig config;
    check(daqiri::parse_network_config_from_yaml_file(argv[1], config), "parse config");
    for (const char* name : {"TX", "RX"}) {
      auto region = config.mrs_.find(name);
      if (region == config.mrs_.end() || region->second.kind_ == daqiri::MemoryKind::DEVICE ||
          region->second.buf_size_ < static_cast<uint32_t>(bytes) ||
          region->second.num_bufs_ <
              static_cast<uint32_t>(std::string(name) == "RX" ? std::min(total, 32) : 1)) {
        throw std::runtime_error(
            "TX/RX regions must be CPU-accessible and cover the message size and receive depth");
      }
    }
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(cpu, &cpus);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus)) return 3;
    check(daqiri::daqiri_init_from_yaml_file(argv[1]), "init");
    uintptr_t conn = 0;
    auto deadline = Clock::now() + std::chrono::seconds(20);
    while (Clock::now() < deadline) {
      auto status = server ? daqiri::rdma_get_server_conn_id(argv[8], 18491, &conn)
                           : daqiri::rdma_connect_to_server(argv[8], 18491, argv[7], &conn);
      if (status == daqiri::Status::SUCCESS) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!conn) throw std::runtime_error("connection timed out");
    uint64_t send_id = 1, recv_id = 1000000000;
    int recv_posted = 0, tx_completed = 0, rx_completed = 0;
    auto allocate = [&](daqiri::RDMAOpCode op) {
      auto* burst = daqiri::create_tx_burst_params();
      if (!burst) throw std::runtime_error("metadata allocation failed");
      const uint64_t wr = op == daqiri::RDMAOpCode::SEND ? send_id++ : recv_id++;
      check(daqiri::rdma_set_header(burst, op, conn, server, 1, wr,
                                    op == daqiri::RDMAOpCode::SEND ? "TX" : "RX"),
            "header");
      check(daqiri::get_tx_packet_burst(burst), "buffer allocation");
      check(daqiri::set_packet_lengths(burst, 0, {bytes}), "length");
      return burst;
    };
    std::vector<daqiri::BurstParams*> owned;
    auto post_receive = [&](daqiri::BurstParams* burst) {
      if (recv_posted < total) {
        check(daqiri::rdma_set_header(burst, daqiri::RDMAOpCode::RECEIVE, conn, server, 1,
                                      recv_id++, "RX"),
              "repost header");
        check(daqiri::set_packet_lengths(burst, 0, {bytes}), "repost length");
        check(daqiri::send_tx_burst(burst), "post receive");
        ++recv_posted;
      }
    };
    for (int i = 0; i < std::min(total, 32); ++i) {
      auto* burst = allocate(daqiri::RDMAOpCode::RECEIVE);
      owned.push_back(burst);
      post_receive(burst);
    }
    daqiri::BurstParams* reusable_send = nullptr;
    if (!server) {
      reusable_send = allocate(daqiri::RDMAOpCode::SEND);
      owned.push_back(reusable_send);
    }
    deadline = Clock::now() + std::chrono::seconds(60);
    uint32_t polls = 0;
    auto poll = [&]() -> daqiri::BurstParams* {
      daqiri::BurstParams* burst = nullptr;
      auto status = daqiri::get_rx_burst(&burst, conn, server);
      if (status != daqiri::Status::SUCCESS && status != daqiri::Status::NOT_READY)
        check(status, "poll");
      if (burst) check(burst->transport_hdr.status, "completion");
      if ((++polls & 16383u) == 0 && Clock::now() > deadline) {
        throw std::runtime_error("completion timeout");
      }
      return burst;
    };
    auto validate = [&](daqiri::BurstParams* burst, int seq) {
      if (daqiri::get_packet_length(burst, 0) != unsigned(bytes))
        throw std::runtime_error("wrong receive length");
      auto* ptr = static_cast<unsigned char*>(daqiri::get_packet_ptr(burst, 0));
      uint64_t actual;
      std::memcpy(&actual, ptr, sizeof(actual));
      if (actual != uint64_t(seq)) throw std::runtime_error("sequence mismatch");
      for (int i = 8; i < bytes; ++i)
        if (ptr[i] != static_cast<unsigned char>(seq + i))
          throw std::runtime_error("payload mismatch");
    };
    std::vector<double> latency;
    latency.reserve(samples);
    if (server) {
      while (rx_completed < total || tx_completed < total) {
        auto* completion = poll();
        if (!completion) continue;
        if (daqiri::rdma_get_opcode(completion) == daqiri::RDMAOpCode::SEND) {
          // Validate only after the NIC has released ownership. This keeps
          // verification and allocation out of this reply's critical path.
          validate(completion, tx_completed);
          ++tx_completed;
          post_receive(completion);
          continue;
        }
        if (daqiri::get_packet_length(completion, 0) != static_cast<uint32_t>(bytes)) {
          throw std::runtime_error("wrong server receive length");
        }
        ++rx_completed;
        // The receive buffer is registered for local SENDs too. Keep it out
        // of the RQ until SEND completion, including when the send is inline.
        check(daqiri::rdma_set_header(completion, daqiri::RDMAOpCode::SEND, conn, true, 1,
                                      send_id++, "RX"),
              "echo header");
        check(daqiri::send_tx_burst(completion), "echo");
      }
    } else {
      for (int seq = 0; seq < total; ++seq) {
        auto* send = reusable_send;
        check(daqiri::rdma_set_header(send, daqiri::RDMAOpCode::SEND, conn, false, 1, send_id++,
                                      "TX"),
              "reused SEND header");
        auto* ptr = static_cast<unsigned char*>(daqiri::get_packet_ptr(send, 0));
        for (int i = 8; i < bytes; ++i) ptr[i] = static_cast<unsigned char>(seq + i);
        uint64_t value = seq;
        std::memcpy(ptr, &value, sizeof(value));
        auto start = Clock::now();
        check(daqiri::send_tx_burst(send), "send");
        Clock::time_point arrived;
        bool received = false, transmitted = false;
        while (!received || !transmitted) {
          auto* completion = poll();
          if (!completion) continue;
          const bool is_receive =
              daqiri::rdma_get_opcode(completion) == daqiri::RDMAOpCode::RECEIVE;
          if (!is_receive) {
            ++tx_completed;
            transmitted = true;
          } else {
            arrived = Clock::now();
            validate(completion, seq);
            ++rx_completed;
            received = true;
          }
          if (is_receive) post_receive(completion);
        }
        if (seq >= warmup)
          latency.push_back(std::chrono::duration<double, std::micro>(arrived - start).count());
      }
      std::ofstream output(argv[9]);
      output << "rtt_us\n";
      for (auto value : latency) output << value << '\n';
      std::sort(latency.begin(), latency.end());
      auto percentile = [&](double p) {
        return latency[std::min(latency.size() - 1, size_t(p * (latency.size() - 1)))];
      };
      std::cout << "RESULT bytes=" << bytes << " samples=" << samples
                << " rtt_min_us=" << latency.front() << " rtt_median_us=" << percentile(.5)
                << " rtt_p95_us=" << percentile(.95) << " rtt_p99_us=" << percentile(.99)
                << " rtt_max_us=" << latency.back() << std::endl;
    }
    std::cout << "VERIFIED role=" << (server ? "server" : "client") << " tx=" << tx_completed
              << " rx=" << rx_completed << " payload_errors=0" << std::endl;
    std::cout << "ALLOCATIONS registered_handles=" << owned.size()
              << " timed_handle_allocations=0 server_payload_copies=0" << std::endl;
    for (auto* burst : owned) daqiri::free_tx_burst(burst);
    daqiri::print_stats();
    // Give the peer time to collect its final completion before teardown.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    daqiri::shutdown();
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL " << e.what() << std::endl;
    // A benchmark failure must not hang in connection teardown.
    std::_Exit(1);
  }
}
