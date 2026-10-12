// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// SPDX-License-Identifier: Apache-2.0

// Raw ibverbs hardware-loopback regression for CPU/CPU/GPU scatter RX (#337).
#include <daqiri/daqiri.h>
#include <cuda_runtime_api.h>
#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr int kHeader = 42, kPrefix = 1042, kPayload = 4100, kSlots = 256;
constexpr uint16_t kQueue = 1, kUdp = 7796;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void cuda_check(cudaError_t status) {
  require(status == cudaSuccess, cudaGetErrorString(status));
}

std::string setting(const char* name, const char* fallback = nullptr) {
  const char* value = std::getenv(name);
  if (value && *value) return value;
  require(fallback != nullptr, std::string("Missing setting: ") + name);
  return fallback;
}

void add_region(daqiri::NetworkConfig& config, const std::string& name, int size,
                daqiri::MemoryKind kind, int affinity) {
  daqiri::MemoryRegionConfig region{};
  region.name_ = name;
  region.kind_ = kind;
  region.affinity_ = affinity;
  region.buf_size_ = size;
  region.num_bufs_ = kSlots;
  region.owned_ = true;
  config.mrs_.emplace(name, region);
}

struct Session {
  ~Session() {
    daqiri::shutdown();
  }
};

struct TxLoan {
  daqiri::BurstParams* burst = nullptr;
  bool acquired = false;
  ~TxLoan() {
    if (!burst) return;
    if (acquired)
      daqiri::free_all_packets_and_burst_tx(burst);
    else
      daqiri::free_tx_metadata(burst);
  }
};

struct RxLoan {
  daqiri::BurstParams* burst = nullptr;
  ~RxLoan() {
    if (burst) daqiri::free_all_packets_and_burst_rx(burst);
  }
};

void run(int regions, int frame_size, bool direct) {
  require(regions >= 2 && regions <= 5, "RX region count must be 2..5");
  require(frame_size >= 64 && frame_size <= kPrefix + kPayload, "Invalid frame size");
  const int gpu = std::stoi(setting("DAQIRI_PLATFORM_GPU", "0"));
  const int numa = std::stoi(setting("DAQIRI_PLATFORM_NUMA", "0"));
  cuda_check(cudaSetDevice(gpu));
  const auto mac_text = setting("DAQIRI_PLATFORM_RX_MAC");
  std::array<unsigned int, 6> mac_bytes{};
  require(std::sscanf(mac_text.c_str(), "%x:%x:%x:%x:%x:%x", &mac_bytes[0], &mac_bytes[1],
                      &mac_bytes[2], &mac_bytes[3], &mac_bytes[4], &mac_bytes[5]) == 6,
          "Invalid MAC");
  std::array<char, 6> mac{};
  for (size_t i = 0; i < mac.size(); ++i) {
    require(mac_bytes[i] <= 255, "Invalid MAC byte");
    mac[i] = static_cast<char>(mac_bytes[i]);
  }

  std::vector<int> sizes = regions == 2   ? std::vector<int>{kPrefix, kPayload}
                           : regions == 3 ? std::vector<int>{kHeader, 1000, kPayload}
                           : regions == 4 ? std::vector<int>{kHeader, 1000, 2000, 2100}
                                          : std::vector<int>{kHeader, 1000, 1000, 1000, 2100};
  const int gpu_start = regions == 2 ? 1 : 2;
  daqiri::NetworkConfig config{};
  config.common_.version = 1;
  config.common_.dir = daqiri::Direction::TX_RX;
  config.common_.stream_type = daqiri::StreamType::RAW;
  config.common_.engine = daqiri::EngineType::IBVERBS;
  config.common_.loopback_ = daqiri::LoopbackType::LOOPBACK_TYPE_HW;
  config.common_.master_core_ = std::stoi(setting("DAQIRI_PLATFORM_MASTER_CPU", "3"));
  config.tx_meta_buffers_ = config.rx_meta_buffers_ = 256;
  add_region(config, "tx-prefix", kPrefix, daqiri::MemoryKind::HUGE, numa);
  add_region(config, "tx-gpu", kPayload, daqiri::MemoryKind::DEVICE, gpu);
  daqiri::InterfaceConfig intf{};
  intf.name_ = "loopback";
  intf.address_ = setting("DAQIRI_PLATFORM_IBVERBS_BDF");
  intf.rx_.flow_isolation_ = true;
  daqiri::TxQueueConfig tx{};
  tx.common_.id_ = kQueue;
  tx.common_.cpu_core_ = setting("DAQIRI_PLATFORM_TX_CPU", "1");
  tx.common_.batch_size_ = 1;
  tx.common_.mrs_ = {"tx-prefix"};
  if (frame_size > kPrefix) tx.common_.mrs_.push_back("tx-gpu");
  tx.poll_mode_ = daqiri::QueuePollMode::INDIRECT;
  intf.tx_.queues_.push_back(tx);
  daqiri::RxQueueConfig rx{};
  rx.common_.id_ = kQueue;
  if (!direct) {
    rx.common_.cpu_core_ = setting("DAQIRI_PLATFORM_RX_CPU", "5");
    rx.common_.batch_size_ = 1;
  }
  rx.poll_mode_ = direct ? daqiri::QueuePollMode::DIRECT : daqiri::QueuePollMode::INDIRECT;
  for (int seg = 0; seg < regions; ++seg) {
    const auto name = "rx-" + std::to_string(seg);
    add_region(config, name, sizes[seg],
               seg < gpu_start ? daqiri::MemoryKind::HUGE : daqiri::MemoryKind::DEVICE,
               seg < gpu_start ? numa : gpu);
    rx.common_.mrs_.push_back(name);
  }
  intf.rx_.queues_.push_back(rx);
  daqiri::FlowConfig flow{};
  flow.name_ = "scatter";
  flow.id_ = 1;
  flow.action_.type_ = daqiri::FlowType::QUEUE;
  flow.action_.id_ = kQueue;
  flow.match_.udp_dst_ = kUdp;
  intf.rx_.flows_.push_back(flow);
  config.ifs_ = {intf};
  const auto initialized = daqiri::daqiri_init(config);
  if (regions > daqiri::MAX_NUM_SEGS) {
    require(initialized != daqiri::Status::SUCCESS, "Oversized region list accepted");
    std::cout << "PASS: oversized region list rejected\n";
    return;
  }
  require(initialized == daqiri::Status::SUCCESS, "daqiri_init failed");
  Session session;
  const int port = daqiri::get_port_id("loopback");
  require(port >= 0, "Missing port");

  // Exercise several packet slots, with a distinct pattern per packet.
  for (int packet = 0; packet < 8; ++packet) {
    std::vector<unsigned char> expected(frame_size);
    for (int i = kHeader; i < frame_size; ++i) {
      expected[i] = static_cast<unsigned char>(i * 37 + packet * 13);
    }
    TxLoan outgoing{daqiri::create_tx_burst_params()};
    require(outgoing.burst != nullptr, "TX metadata allocation failed");
    daqiri::set_header(outgoing.burst, port, kQueue, 1, tx.common_.mrs_.size());
    require(daqiri::get_tx_packet_burst(outgoing.burst) == daqiri::Status::SUCCESS,
            "TX packet acquisition failed");
    outgoing.acquired = true;
    auto* prefix =
        static_cast<unsigned char*>(daqiri::get_segment_packet_ptr(outgoing.burst, 0, 0));
    std::memcpy(prefix, expected.data(), std::min(frame_size, kPrefix));
    if (frame_size > kPrefix) {
      cuda_check(cudaMemcpy(daqiri::get_segment_packet_ptr(outgoing.burst, 1, 0),
                            expected.data() + kPrefix, frame_size - kPrefix,
                            cudaMemcpyHostToDevice));
    }
    require(daqiri::set_eth_header(outgoing.burst, 0, mac.data()) == daqiri::Status::SUCCESS,
            "Ethernet header failed");
    std::memcpy(prefix + 6, mac.data(), mac.size());
    require(daqiri::set_ipv4_header(outgoing.burst, 0, frame_size - 14, 17, htonl(0xc0000201),
                                    htonl(0xc0000202)) == daqiri::Status::SUCCESS,
            "IPv4 header failed");
    prefix[14 + 8] = 64;
    require(daqiri::set_udp_header(outgoing.burst, 0, frame_size - 34, kUdp, kUdp) ==
                daqiri::Status::SUCCESS,
            "UDP header failed");
    const auto lengths =
        frame_size > kPrefix
            ? daqiri::set_packet_lengths(outgoing.burst, 0, {kPrefix, frame_size - kPrefix})
            : daqiri::set_packet_lengths(outgoing.burst, 0, {frame_size});
    require(lengths == daqiri::Status::SUCCESS, "TX lengths failed");
    const auto sent = daqiri::send_tx_burst(outgoing.burst);
    if (sent == daqiri::Status::SUCCESS || sent == daqiri::Status::NO_SPACE_AVAILABLE) {
      outgoing.burst = nullptr;
    }
    require(sent == daqiri::Status::SUCCESS, "TX submission failed");

    RxLoan incoming;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (daqiri::get_rx_burst(&incoming.burst, port, kQueue) != daqiri::Status::SUCCESS) {
      require(std::chrono::steady_clock::now() < deadline, "RX timeout");
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    require(incoming.burst && daqiri::get_num_packets(incoming.burst) == 1, "Invalid RX burst");
    require(incoming.burst->hdr.hdr.num_segs == regions, "Incorrect RX segment count");
    int offset = 0;
    for (int seg = 0; seg < regions; ++seg) {
      require(incoming.burst->pkts[seg] && incoming.burst->pkt_lens[seg], "Missing segment arrays");
      const int length = std::min(sizes[seg], frame_size - offset);
      require(daqiri::get_segment_packet_length(incoming.burst, seg, 0) == length,
              "Incorrect segment length: " + std::to_string(seg));
      void* ptr = daqiri::get_segment_packet_ptr(incoming.burst, seg, 0);
      require(ptr != nullptr, "Missing segment buffer");
      std::vector<unsigned char> actual(length);
      if (seg >= gpu_start) {
        cudaPointerAttributes attributes{};
        cuda_check(cudaPointerGetAttributes(&attributes, ptr));
        require(attributes.type == cudaMemoryTypeDevice && attributes.device == gpu,
                "RX segment is not on the configured GPU");
        if (length) cuda_check(cudaMemcpy(actual.data(), ptr, length, cudaMemcpyDeviceToHost));
      } else if (length) {
        std::memcpy(actual.data(), ptr, length);
      }
      // NIC checksum offload changes network headers; compare all application bytes.
      const int skip = std::min(length, std::max(0, kHeader - offset));
      require(std::equal(actual.begin() + skip, actual.end(), expected.begin() + offset + skip),
              "Incorrect bytes in segment: " + std::to_string(seg));
      offset += length;
    }
    require(offset == frame_size, "RX segment lengths do not cover the frame");
  }
  std::cout << "PASS: exact CPU/GPU byte integrity across 8 packets\n";
}
}  // namespace

int main(int argc, char** argv) {
  try {
    require(argc == 4, "Usage: multi_region_rx REGIONS FRAME_SIZE direct|indirect");
    const std::string mode = argv[3];
    require(mode == "direct" || mode == "indirect", "Invalid polling mode");
    run(std::stoi(argv[1]), std::stoi(argv[2]), mode == "direct");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
