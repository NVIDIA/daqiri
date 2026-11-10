// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
// Explicit hardware correctness check. Never registered with CTest.
#include "src/engines/mgbe/daqiri_mgbe_engine.h"
#include "mgbe_pmd.hpp"
#include <daqiri/daqiri.h>
#include <array>
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool pass, const char* operation) {
  if (!pass) throw std::runtime_error(operation);
}
class LoopbackPort final : public daqiri::MgbePort {
 public:
  LoopbackPort(const std::string& interface, uint32_t ring, uint32_t mtu, uint32_t queues,
               uint32_t target)
      : device_("/dev/mgbe-pmd-" + interface, ring, mtu, queues), target_(target) {}
  mgbe::Queue& queue(uint32_t id = 0) override {
    return device_.queue(id);
  }
  void start() override {
    if (device_.queue_count() > 1) {
      auto rss = device_.rss();
      for (auto& entry : rss.table) entry = target_;
      device_.configure_rss(rss);
    }
    device_.enable_mac_loopback();
    device_.start();
  }

  void diagnose() {
    const auto& status = device_.status();
    std::cerr << "DMA session state=" << __atomic_load_n(&status.state, __ATOMIC_ACQUIRE)
              << " fault=" << status.fault << " link=" << status.link
              << " DMA status=" << status.dma_status << std::endl;
    const auto mmc = device_.hardware_stats();
    std::cerr << "MAC TX=" << mmc.tx_frames << " RX=" << mmc.rx_frames
              << " CRC=" << mmc.rx_crc_errors << " FIFO=" << mmc.rx_fifo_overflows << std::endl;
  }

 private:
  mgbe::Device device_;
  uint32_t target_;
};
class ProbeEngine final : public daqiri::MgbeEngine {
 public:
  explicit ProbeEngine(uint32_t target) : target_(target) {}
  void diagnose() {
    print_stats();
    for (auto* port : ports_) port->diagnose();
  }

 protected:
  std::unique_ptr<daqiri::MgbePort> open_port(const std::string& interface, uint32_t ring,
                                              uint32_t mtu, uint32_t queues) override {
    auto port = std::make_unique<LoopbackPort>(interface, ring, mtu, queues, target_);
    ports_.push_back(port.get());
    return port;
  }

 private:
  uint32_t target_;
  std::vector<LoopbackPort*> ports_;
};
void check(const daqiri::NetworkConfig& base, size_t frame_size, uint32_t target) {
  auto config = base;
  for (auto& [name, region] : config.mrs_) {
    region.num_bufs_ = 64;
    region.buf_size_ = frame_size <= 1514 ? 1514 : frame_size;
  }
  ProbeEngine engine(target);
  require(engine.set_config_and_initialize(config), "MGBE DMA initialization");
  for (size_t port = 0; port < config.ifs_.size(); ++port) {
    std::array<char, 6> mac{};
    require(engine.get_mac_addr(port, mac.data()) == daqiri::Status::SUCCESS, "read MAC");
    std::array<uint8_t, 9014> expected{};
    for (const auto& queue : config.ifs_[port].tx_.queues_) {
      const size_t batch =
          queue.poll_mode_ == daqiri::QueuePollMode::DIRECT ? 1 : queue.common_.batch_size_;
      auto pattern = [&](uint32_t sequence) {
        for (size_t byte = 0; byte < frame_size; ++byte)
          expected[byte] =
              uint8_t((byte * 131U) ^ sequence ^ (sequence >> 8) ^ (queue.common_.id_ << 4) ^ port);
        std::memcpy(expected.data(), mac.data(), 6);
        std::memcpy(expected.data() + 6, mac.data(), 6);
        auto* headers = reinterpret_cast<daqiri::UDPIPV4Pkt*>(expected.data());
        headers->eth.h_proto = htons(ETH_P_IP);
        headers->ip = {};
        headers->ip.version = 4;
        headers->ip.ihl = 5;
        headers->ip.ttl = 64;
        headers->ip.protocol = IPPROTO_UDP;
        headers->ip.tot_len = htons(frame_size - 14);
        headers->ip.saddr = htonl(0xc0000202);
        headers->ip.daddr = htonl(0xc0000201);
        uint32_t sum = 0;
        for (size_t i = 14; i < 34; i += 2) sum += (uint16_t(expected[i]) << 8) | expected[i + 1];
        while (sum >> 16) sum = (sum & 0xffffU) + (sum >> 16);
        headers->ip.check = htons(uint16_t(~sum));
        headers->udp = {};
        headers->udp.source = htons(10000 + queue.common_.id_);
        headers->udp.dest = htons(9000);
        headers->udp.len = htons(frame_size - 34);
      };
      for (uint32_t sequence = 0; sequence < 256; sequence += batch) {
        auto* tx = engine.create_tx_burst_params();
        require(tx != nullptr, "allocate TX metadata");
        daqiri::set_header(tx, port, queue.common_.id_, batch, 1);
        require(engine.get_tx_packet_burst(tx) == daqiri::Status::SUCCESS, "reserve TX burst");
        for (size_t i = 0; i < batch; ++i) {
          require(engine.set_packet_lengths(tx, i, {int(frame_size)}) == daqiri::Status::SUCCESS,
                  "set TX length");
          pattern(sequence + i);
          std::memcpy(engine.get_packet_ptr(tx, i), expected.data(), frame_size);
        }
        require(engine.send_tx_burst(tx) == daqiri::Status::SUCCESS, "post TX burst");
        size_t received = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (received < batch) {
          daqiri::BurstParams* rx = nullptr;
          daqiri::Status status;
          do {
            status = engine.get_rx_burst(&rx, port, target);
            if (status != daqiri::Status::NOT_READY) break;
          } while (std::chrono::steady_clock::now() < deadline);
          if (status != daqiri::Status::SUCCESS) {
            std::cerr << "RX timeout/error: port=" << port << " TX queue=" << queue.common_.id_
                      << " RX queue=" << target << " sequence=" << sequence
                      << " status=" << int(status) << std::endl;
            engine.diagnose();
            for (size_t q = 0; q < config.ifs_[port].rx_.queues_.size(); ++q) {
              daqiri::BurstParams* misplaced = nullptr;
              if (engine.get_rx_burst(&misplaced, port, q) == daqiri::Status::SUCCESS) {
                std::cerr << "Unexpected packets on RX queue " << q << ": "
                          << misplaced->hdr.hdr.num_pkts << std::endl;
                engine.free_rx_burst(misplaced);
              }
            }
          }
          require(status == daqiri::Status::SUCCESS && rx->hdr.hdr.num_pkts &&
                      received + rx->hdr.hdr.num_pkts <= batch,
                  "DMA RX completion");
          require(rx->hdr.hdr.port_id == port && rx->hdr.hdr.q_id == target,
                  "DMA queue correlation");
          for (size_t i = 0; i < rx->hdr.hdr.num_pkts; ++i) {
            pattern(sequence + received++);
            require(engine.get_packet_length(rx, i) == frame_size, "DMA RX length");
            const auto* actual = static_cast<const uint8_t*>(engine.get_packet_ptr(rx, i));
            if (std::memcmp(actual, expected.data(), frame_size)) {
              size_t offset = 0;
              while (offset < frame_size && actual[offset] == expected[offset]) ++offset;
              std::cerr << "Payload mismatch: port=" << port << " TX=" << queue.common_.id_
                        << " RX=" << target << " sequence=" << sequence + received - 1
                        << " byte=" << offset << " actual=" << unsigned(actual[offset])
                        << " expected=" << unsigned(expected[offset]) << std::endl;
              engine.diagnose();
              throw std::runtime_error("DMA payload mismatch");
            }
          }
          engine.free_rx_burst(rx);
        }
        require(engine.wait_for_tx_idle(1000) == daqiri::Status::SUCCESS, "drain TX batch");
      }
    }
  }
  require(engine.wait_for_tx_idle(1000) == daqiri::Status::SUCCESS, "drain TX");
  engine.shutdown();
}
}  // namespace
int main(int argc, char** argv) {
  if (argc < 2 || argc > 3) {
    std::cerr << "Usage: daqiri_mgbe_dma_test config.yaml [worker_cpu=8] (exclusive hardware MAC "
                 "loopback)\n";
    return 2;
  }
  try {
    daqiri::NetworkConfig config{};
    require(daqiri::parse_network_config_from_yaml_file(argv[1], config) == daqiri::Status::SUCCESS,
            "parse configuration");
    const std::string cpu = argc == 3 ? argv[2] : "8";
    // Exercise both modes and mixed directions through the actual adapter.
    for (int modes = 0; modes < 4; ++modes) {
      for (auto& port : config.ifs_) {
        for (auto& rx : port.rx_.queues_) {
          rx.poll_mode_ =
              modes & 1 ? daqiri::QueuePollMode::INDIRECT : daqiri::QueuePollMode::DIRECT;
          rx.common_.cpu_core_ = modes & 1 ? cpu : "";
          rx.common_.batch_size_ = modes & 1 ? 32 : 0;
          rx.timeout_us_ = modes & 1 ? 100 : 0;
        }
        for (auto& tx : port.tx_.queues_) {
          tx.poll_mode_ =
              modes & 2 ? daqiri::QueuePollMode::INDIRECT : daqiri::QueuePollMode::DIRECT;
          tx.common_.cpu_core_ = modes & 2 ? cpu : "";
          tx.common_.batch_size_ = modes & 2 ? 32 : 0;
        }
      }
      for (size_t frame : {size_t(60), size_t(1514), size_t(8192)}) {
        for (size_t target = 0; target < config.ifs_[0].rx_.queues_.size(); ++target) {
          check(config, frame, target);
          std::cout << "DMA check passed: modes=" << modes << " frame=" << frame << " RX=" << target
                    << std::endl;
        }
      }
    }
    std::cout << "MGBE DMA payload, ring reuse and session recovery checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
