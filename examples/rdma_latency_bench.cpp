// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "rdma_pingpong.h"

#include <yaml-cpp/yaml.h>
#include <pthread.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
  using daqiri::Status;
  using Clock = std::chrono::steady_clock;
  using namespace std::chrono_literals;
  if (argc != 6) {
    std::cerr << "Usage: daqiri_bench_rdma_latency CONFIG server|client WARMUP SAMPLES CSV\n";
    return 2;
  }
  bool initialized = false;
  try {
    const std::string role = argv[2];
    const bool server = role == "server";
    if (!server && role != "client") throw std::runtime_error("invalid RoCE role");
    const int warmup = std::stoi(argv[3]);
    const int samples = std::stoi(argv[4]);
    if (warmup < 0 || samples < 1) throw std::runtime_error("invalid RoCE probe counts");
    const auto config = YAML::LoadFile(argv[1])["rdma_bench_" + role];
    const auto server_address = config["server_address"].as<std::string>();
    const auto client_address = config["client_address"].as<std::string>();
    const auto port = config["server_port"].as<uint16_t>();
    const auto bytes = config["message_size"].as<int>();
    const auto credits = config["rx_depth"].as<int>(1);
    const auto cpu = config["cpu_core"].as<int>();
    const auto priority = config["realtime_priority"].as<int>(80);
    if (bytes < 8 || credits < 1 || credits > 1024 || cpu < 0 || cpu >= CPU_SETSIZE ||
        priority < 1 || priority > 98)
      throw std::runtime_error("invalid RoCE latency configuration");
    daqiri::NetworkConfig network;
    if (daqiri::parse_network_config_from_yaml_file(argv[1], network) != Status::SUCCESS ||
        network.ifs_.size() != 1)
      throw std::runtime_error("RoCE latency expects one interface per role");
    const auto& interface = network.ifs_.front();
    if (interface.rdma_.xmode_ != daqiri::RDMATransportMode::RC ||
        interface.rx_.queues_.size() != 1 || interface.tx_.queues_.size() != 1 ||
        interface.rx_.queues_.front().poll_mode_ != daqiri::QueuePollMode::DIRECT ||
        interface.tx_.queues_.front().poll_mode_ != daqiri::QueuePollMode::DIRECT)
      throw std::runtime_error("RoCE latency requires direct RC RX and TX queues");
    const std::string suffix = server ? "SERVER" : "CLIENT";
    const auto tx_name = "DATA_TX_GPU_" + suffix;
    const auto rx_name = "DATA_RX_GPU_" + suffix;
    const auto& tx = network.mrs_.at(tx_name);
    const auto& rx = network.mrs_.at(rx_name);
    const bool opaque = config["opaque_payload"].as<bool>(false) ||
                        tx.kind_ == daqiri::MemoryKind::DEVICE ||
                        rx.kind_ == daqiri::MemoryKind::DEVICE;
    if (tx.kind_ == daqiri::MemoryKind::DEVICE && !(tx.access_ & daqiri::MEM_INIT_ZERO))
      throw std::runtime_error("device TX requires zero_init");
    if (daqiri::daqiri_init(network) != Status::SUCCESS)
      throw std::runtime_error("DAQIRI initialization failed");
    initialized = true;

    uintptr_t connection = 0;
    const auto deadline = Clock::now() + 10s;
    while (!connection) {
      const auto status =
          server
              ? daqiri::rdma_get_server_conn_id(server_address, port, &connection)
              : daqiri::rdma_connect_to_server(server_address, port, client_address, &connection);
      if (status == Status::SUCCESS && connection) break;
      if (Clock::now() >= deadline) throw std::runtime_error("RoCE connection timed out");
      std::this_thread::sleep_for(1ms);
    }
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    CPU_SET(cpu, &affinity);
    if (pthread_setaffinity_np(pthread_self(), sizeof(affinity), &affinity))
      throw std::runtime_error("could not pin RoCE latency thread");
    std::vector<uint64_t> measurements;
    if (!server) measurements.reserve(samples);
    {
      daqiri::bench::RdmaPingPong ping_pong(connection, server, bytes, credits, opaque,
                                            2'000'000'000, tx_name, rx_name);
      sched_param sched{};
      sched.sched_priority = priority;
      if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sched))
        throw std::runtime_error("could not set FIFO priority for RoCE latency");
      int policy = 0;
      if (pthread_getschedparam(pthread_self(), &policy, &sched) || policy != SCHED_FIFO ||
          sched.sched_priority != priority)
        throw std::runtime_error("could not verify FIFO priority for RoCE latency");
      std::cerr << "Latency " << role << " on CPU " << cpu << " running SCHED_FIFO priority "
                << priority << '\n';
      const uint64_t total = static_cast<uint64_t>(warmup) + samples;
      for (uint64_t sequence = 0; sequence < total; ++sequence) {
        const auto rtt = ping_pong.probe(!server, sequence, sequence + 1 < total);
        if (!server && sequence >= static_cast<uint64_t>(warmup)) measurements.push_back(rtt);
      }
    }
    if (!server) {
      std::ofstream out(argv[5]);
      out << "sequence,software_ns\n";
      for (size_t i = 0; i < measurements.size(); ++i) out << i << ',' << measurements[i] << '\n';
      out.close();
      if (!out) throw std::runtime_error("could not save RoCE latency probes");
      std::sort(measurements.begin(), measurements.end());
      std::cout << "RoCE RTT (us), " << bytes << " B, " << samples << " samples\n"
                << "Min  Median  P99  Max\n"
                << measurements.front() / 1000.0 << "  "
                << (measurements[samples / 2] + measurements[(samples - 1) / 2]) / 2000.0 << "  "
                << measurements[(static_cast<size_t>(samples) * 99 + 99) / 100 - 1] / 1000.0 << "  "
                << measurements.back() / 1000.0 << '\n';
    }
    daqiri::print_stats();
    daqiri::shutdown();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "RoCE latency: " << error.what() << '\n';
    if (initialized) daqiri::shutdown();
    return 1;
  }
}
