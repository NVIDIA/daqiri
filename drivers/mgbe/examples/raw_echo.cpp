// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#include <daqiri/daqiri.h>
#include "mgbe_tap_daqiri.hpp"
#include <memory>
#include <stdexcept>
#include <algorithm>
#include <array>
#include <csignal>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
volatile std::sig_atomic_t stopped = 0;
void stop(int) {
  stopped = 1;
}
}  // namespace
int main(int argc, char** argv) {
  if (argc < 2 || argc % 2) {
    std::cerr << "Usage: daqiri_mgbe_raw_echo config.yaml [--tap NAME] [--tap-cpu CORE] "
                 "[--udp-port PORT]\n";
    return 2;
  }
  bool initialized = false;
  try {
    std::string tap_name;
    int tap_cpu = 0, udp_port = 9000;
    for (int i = 2; i < argc; i += 2) {
      const std::string option = argv[i];
      if (option == "--tap")
        tap_name = argv[i + 1];
      else if (option == "--tap-cpu")
        tap_cpu = std::stoi(argv[i + 1]);
      else if (option == "--udp-port")
        udp_port = std::stoi(argv[i + 1]);
      else
        throw std::invalid_argument("unknown echo option");
    }
    if (udp_port < 1 || udp_port > 65535) throw std::invalid_argument("invalid UDP port");
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    daqiri::NetworkConfig config{};
    if (daqiri::parse_network_config_from_yaml_file(argv[1], config) != daqiri::Status::SUCCESS ||
        daqiri::daqiri_init(config) != daqiri::Status::SUCCESS)
      return 1;
    initialized = true;
    if (daqiri::get_engine_type() != daqiri::EngineType::MGBE) {
      daqiri::shutdown();
      return 1;
    }
    struct Endpoint {
      int port, queue;
      size_t tx_batch;
      std::array<char, 6> mac;
    };
    std::vector<Endpoint> endpoints;
    for (size_t port = 0; port < config.ifs_.size(); ++port) {
      for (const auto& rx : config.ifs_[port].rx_.queues_) {
        const auto& tx = *std::find_if(
            config.ifs_[port].tx_.queues_.begin(), config.ifs_[port].tx_.queues_.end(),
            [&](const auto& q) { return q.common_.id_ == rx.common_.id_; });
        Endpoint endpoint{int(port),
                          rx.common_.id_,
                          tx.poll_mode_ == daqiri::QueuePollMode::DIRECT
                              ? size_t(1)
                              : size_t(tx.common_.batch_size_),
                          {}};
        if (daqiri::get_mac_addr(port, endpoint.mac.data()) != daqiri::Status::SUCCESS) {
          daqiri::shutdown();
          return 1;
        }
        endpoints.push_back(endpoint);
      }
    }
    std::unique_ptr<mgbe::KernelTap> tap;
    if (!tap_name.empty()) {
      if (config.ifs_.size() != 1) throw std::invalid_argument("echo TAP requires one interface");
      const auto& mr = config.mrs_.at(config.ifs_[0].rx_.queues_[0].common_.mrs_[0]);
      mgbe::KernelTap::Options options;
      options.name = tap_name;
      options.worker_cpu = tap_cpu;
      options.mtu = mr.buf_size_ - 14;
      std::memcpy(options.mac.data(), endpoints[0].mac.data(), 6);
      tap = std::make_unique<mgbe::KernelTap>(options);
    }
    bool failed = false;
    uint64_t echoed = 0, kernel_submitted = 0, kernel_dropped = 0;
    std::cout << "{\"event\":\"ready\",\"engine\":\"mgbe\"}" << std::endl;
    while (!stopped && !failed) {
      for (const auto& endpoint : endpoints) {
        if (stopped || failed) break;
        daqiri::BurstParams* rx = nullptr;
        const auto status = daqiri::get_rx_burst(&rx, endpoint.port, endpoint.queue);
        if (status == daqiri::Status::NOT_READY) continue;
        if (status != daqiri::Status::SUCCESS) {
          failed = true;
          break;
        }
        std::array<int, 64> replies{};
        size_t count = 0;
        for (int i = 0; i < daqiri::get_num_packets(rx); ++i) {
          const auto length = daqiri::get_packet_length(rx, i);
          const auto* input = static_cast<const uint8_t*>(daqiri::get_packet_ptr(rx, i));
          if (length < sizeof(daqiri::UDPIPV4Pkt) || std::memcmp(input, endpoint.mac.data(), 6) ||
              input[12] != 8 || input[13] != 0 || input[14] != 0x45 || input[23] != 17 ||
              (input[20] & 0x3f) || input[21] ||
              (tap && ((uint16_t(input[36]) << 8) | input[37]) != udp_port)) {
            if (tap && !tap->send_to_kernel(input, length)) ++kernel_dropped;
            continue;
          }
          replies[count++] = i;
        }
        for (size_t next = 0; next < count && !failed;) {
          auto* tx = daqiri::create_tx_burst_params();
          if (!tx) {
            failed = true;
            break;
          }
          const size_t n = std::min(count - next, endpoint.tx_batch);
          daqiri::set_header(tx, endpoint.port, endpoint.queue, n, 1);
          if (!n || daqiri::get_tx_packet_burst(tx) != daqiri::Status::SUCCESS) {
            daqiri::free_all_packets_and_burst_tx(tx);
            failed = true;
            break;
          }
          for (size_t i = 0; i < n; ++i) {
            const int index = replies[next + i];
            const auto length = daqiri::get_packet_length(rx, index);
            if (daqiri::set_packet_lengths(tx, i, {int(length)}) != daqiri::Status::SUCCESS) {
              failed = true;
              break;
            }
            const auto* input = static_cast<const uint8_t*>(daqiri::get_packet_ptr(rx, index));
            auto* output = static_cast<uint8_t*>(daqiri::get_packet_ptr(tx, i));
            std::memcpy(output, input, length);
            for (int j = 0; j < 6; ++j) std::swap(output[j], output[j + 6]);
            for (int j = 0; j < 4; ++j) std::swap(output[26 + j], output[30 + j]);
            for (int j = 0; j < 2; ++j) std::swap(output[34 + j], output[36 + j]);
            // Swapping endpoints preserves the IPv4 and UDP checksum sums.
          }
          if (failed || daqiri::send_tx_burst(tx) != daqiri::Status::SUCCESS) {
            daqiri::free_all_packets_and_burst_tx(tx);
            failed = true;
            break;
          }
          next += n;
          echoed += n;
        }
        daqiri::free_all_packets_and_burst_rx(rx);
      }
      // Only this application TX owner accesses queue zero. At most one kernel
      // return frame per pass keeps exception traffic from monopolizing the loop.
      if (tap && !failed && mgbe::transmit_from_kernel(*tap, 0, 0)) ++kernel_submitted;
    }
    if (tap) {
      tap->stop();
      const auto stats = tap->stats();
      std::cout << "TAP enqueued=" << stats.to_kernel_enqueued
                << " delivered=" << stats.to_kernel_delivered << " kernel_TX=" << kernel_submitted
                << " dropped=" << kernel_dropped << " discarded_on_stop=" << stats.discarded_on_stop
                << '\n';
      if (stats.error || stats.from_kernel_delivered != kernel_submitted) failed = true;
    }
    if (daqiri::wait_for_tx_idle(1000) != daqiri::Status::SUCCESS) failed = true;
    daqiri::print_stats();
    daqiri::shutdown();
    initialized = false;
    std::cout << "Packets echoed: " << echoed << '\n';
    return failed ? 1 : 0;
  } catch (const std::exception& error) {
    if (initialized) daqiri::shutdown();
    std::cerr << error.what() << '\n';
    return 1;
  }
}
