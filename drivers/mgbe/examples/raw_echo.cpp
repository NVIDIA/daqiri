// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#include <daqiri/daqiri.h>
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
  if (argc != 2) {
    std::cerr << "Usage: daqiri_mgbe_raw_echo config.yaml\n";
    return 2;
  }
  std::signal(SIGINT, stop);
  std::signal(SIGTERM, stop);
  daqiri::NetworkConfig config{};
  if (daqiri::parse_network_config_from_yaml_file(argv[1], config) != daqiri::Status::SUCCESS ||
      daqiri::daqiri_init(config) != daqiri::Status::SUCCESS)
    return 1;
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
      const auto& tx =
          *std::find_if(config.ifs_[port].tx_.queues_.begin(), config.ifs_[port].tx_.queues_.end(),
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
  bool failed = false;
  uint64_t echoed = 0;
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
            (input[20] & 0x3f) || input[21])
          continue;
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
  }
  if (daqiri::wait_for_tx_idle(1000) != daqiri::Status::SUCCESS) failed = true;
  daqiri::print_stats();
  daqiri::shutdown();
  std::cout << "Packets echoed: " << echoed << '\n';
  return failed ? 1 : 0;
}
