// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// SPDX-License-Identifier: Apache-2.0

// Raw ibverbs hardware-loopback byte integrity for scatter RX and gathered TX.
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

uint32_t checksum_sum(const unsigned char* bytes, size_t length, uint32_t sum = 0) {
  for (size_t i = 0; i < length; i += 2) {
    sum += static_cast<uint32_t>(bytes[i]) << 8;
    if (i + 1 < length) sum += bytes[i + 1];
  }
  while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
  return sum;
}

void run_shared(const std::vector<int>& sizes, bool endpoint, daqiri::EndpointId destination,
                int port, const std::array<char, 6>& mac) {
  constexpr int frame_size = kPrefix + kPayload;
  const int segments = sizes.size();
  auto get = [&]() {
    auto* burst = daqiri::create_tx_burst_params();
    require(burst != nullptr, "Shared TX metadata exhausted");
    daqiri::set_header(burst, port, kQueue, 1, segments);
    return burst;
  };
  auto transmit = [&](daqiri::BurstParams*& burst) {
    const auto status =
        endpoint ? daqiri::send_tx_burst(destination, kQueue, burst) : daqiri::send_tx_burst(burst);
    if (status == daqiri::Status::SUCCESS || status == daqiri::Status::NO_SPACE_AVAILABLE)
      burst = nullptr;
    require(status == daqiri::Status::SUCCESS, "Shared TX submission failed");
    require(daqiri::wait_for_tx_idle(2000) == daqiri::Status::SUCCESS,
            "Shared TX completion timeout");
  };
  auto receive = [&](const std::vector<unsigned char>& expected, unsigned variant) {
    RxLoan incoming;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (daqiri::get_rx_burst(&incoming.burst, port, kQueue) != daqiri::Status::SUCCESS) {
      require(std::chrono::steady_clock::now() < deadline, "Shared RX timeout");
      std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
    std::vector<unsigned char> received;
    for (int seg = 0; seg < incoming.burst->hdr.hdr.num_segs; ++seg) {
      const auto bytes = incoming.burst->pkt_lens[seg][0];
      const size_t offset = received.size();
      received.resize(offset + bytes);
      if (seg == 0)
        std::memcpy(received.data() + offset, incoming.burst->pkts[seg][0], bytes);
      else
        cuda_check(cudaMemcpy(received.data() + offset, incoming.burst->pkts[seg][0], bytes,
                              cudaMemcpyDeviceToHost));
    }
    require(received.size() == expected.size(), "Shared RX frame size differs");
    require(std::equal(received.begin() + kHeader, received.end(), expected.begin() + kHeader),
            "Private header or shared payload differs");
    require(received[36] * 256 + received[37] == kUdp, "Shared UDP destination differs");
    if (!endpoint) require(received[29] == variant, "Private source IP was not independent");
    require(checksum_sum(received.data() + 14, 20) == 0xffff, "Shared IPv4 checksum differs");
    const auto sum = checksum_sum(received.data() + 26, 8, 17 + frame_size - 34);
    require(checksum_sum(received.data() + 34, frame_size - 34, sum) == 0xffff,
            "Shared gathered UDP checksum differs");
  };
  auto headers = [&](daqiri::BurstParams* burst, unsigned variant,
                     std::vector<unsigned char>& expected) {
    int offset = endpoint ? kHeader : 0;
    for (int seg = 0; seg + 1 < segments; ++seg) {
      if (endpoint || seg != 0) {
        std::memset(burst->pkts[seg][0], variant + seg, sizes[seg]);
        std::fill_n(expected.begin() + offset, sizes[seg], variant + seg);
      }
      offset += sizes[seg];
    }
    if (!endpoint) {
      auto* ptr = static_cast<uint8_t*>(burst->pkts[0][0]);
      require(daqiri::set_eth_header(burst, 0, const_cast<char*>(mac.data())) ==
                  daqiri::Status::SUCCESS,
              "Private Ethernet header rejected");
      std::memcpy(ptr + 6, mac.data(), mac.size());
      require(daqiri::set_ipv4_header(burst, 0, frame_size - 14, 17, htonl(0xc0000200 | variant),
                                      htonl(0xc0000202)) == daqiri::Status::SUCCESS,
              "Private IP header rejected");
      ptr[22] = 64;
      require(
          daqiri::set_udp_header(burst, 0, frame_size - 34, kUdp, kUdp) == daqiri::Status::SUCCESS,
          "Private UDP header rejected");
    }
  };
  for (int iteration = 0; iteration < 300; ++iteration) {
    TxLoan original{get()}, second;
    require(daqiri::get_tx_packet_burst(original.burst) == daqiri::Status::SUCCESS,
            "Shared acquisition failed");
    original.acquired = true;
    std::vector<unsigned char> expected(frame_size);
    for (int i = kHeader; i < frame_size; ++i)
      expected[i] = static_cast<unsigned char>(i * 37 + iteration);
    int offset = endpoint ? kHeader : 0;
    for (int seg = 0; seg < segments; ++seg) {
      original.burst->pkt_lens[seg][0] = sizes[seg];
      if (seg + 1 == segments)
        cuda_check(cudaMemcpy(original.burst->pkts[seg][0], expected.data() + offset, sizes[seg],
                              cudaMemcpyHostToDevice));
      else
        std::memcpy(original.burst->pkts[seg][0], expected.data() + offset, sizes[seg]);
      offset += sizes[seg];
    }
    headers(original.burst, 1, expected);
    auto* payload = original.burst->pkts[segments - 1][0];
    require(daqiri::clone_tx_burst(original.burst, 1u << (segments - 1), &second.burst) ==
                daqiri::Status::SUCCESS,
            "Shared clone failed");
    second.acquired = true;
    require(second.burst->pkts[segments - 1][0] == payload, "Payload was copied");
    for (int seg = 0; seg + 1 < segments; ++seg)
      require(second.burst->pkts[seg][0] != original.burst->pkts[seg][0], "Private segments alias");
    if (iteration % 2 == 0) {
      transmit(original.burst);
      receive(expected, 1);
      TxLoan blocked{get()};
      require(daqiri::get_tx_packet_burst(blocked.burst) == daqiri::Status::NO_FREE_PACKET_BUFFERS,
              "Original completion freed unsent clone payload");
    } else {
      daqiri::free_all_packets_and_burst_tx(original.burst);
      original.burst = nullptr;
    }
    // Clone a clone after the original has completed/cancelled; private pool
    // capacity two proves no extra payload/header is allocated behind the API.
    TxLoan third;
    require(daqiri::clone_tx_burst(second.burst, 1u << (segments - 1), &third.burst) ==
                daqiri::Status::SUCCESS,
            "Clone of clone failed");
    third.acquired = true;
    daqiri::free_all_packets_and_burst_tx(second.burst);
    second.burst = nullptr;
    headers(third.burst, 2, expected);
    transmit(third.burst);
    receive(expected, 2);
  }
  // Deletion must wait for an unsubmitted clone, even with no outstanding sends.
  TxLoan held{get()};
  require(daqiri::get_tx_packet_burst(held.burst) == daqiri::Status::SUCCESS,
          "Deletion hold allocation failed");
  held.acquired = true;
  daqiri::ResourceOpId operation = 0;
  require(daqiri::delete_tx_queue_async(port, kQueue, &operation) == daqiri::Status::SUCCESS,
          "Queue deletion request failed");
  daqiri::ResourceOpResult result;
  require(daqiri::poll_resource_op(&result) == daqiri::Status::NOT_READY,
          "Queue deleted caller-owned storage");
  require(daqiri::delete_memory_region_async("tx-" + std::to_string(segments - 1), &operation) ==
              daqiri::Status::RESOURCE_IN_USE,
          "MR deleted while clone pinned queue");
  daqiri::free_all_packets_and_burst_tx(held.burst);
  held.burst = nullptr;
  require(daqiri::poll_resource_op(&result) == daqiri::Status::SUCCESS &&
              result.state_ == daqiri::ResourceState::REMOVED,
          "Queue deletion did not finish after release");
  std::cout << "PASS: shared TX private headers, GPU payload lifetime, clone of clone, "
               "cancellation, checksums, and draining\n";
}

void run(int regions, int frame_size, bool direct, int tx_regions = 0, bool tx_direct = false,
         bool endpoint = false, bool shared = false) {
  require(regions >= 1 && regions <= 5, "RX region count must be 1..5");
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
  const std::string null_layout = setting("DAQIRI_PLATFORM_NULL_LAYOUT", "");
  std::vector<bool> null_segments(regions, false);
  if (!null_layout.empty()) {
    if (null_layout == "all") {
      require(regions == 1, "Discard-only layout requires one region");
      sizes = {frame_size};
      null_segments[0] = true;
    } else {
      require(regions >= 2, "Null scatter requires multiple regions");
      if (null_layout == "head" || null_layout == "head-tail") null_segments[0] = true;
      if (null_layout == "middle") null_segments[1] = true;
      if (null_layout == "tail" || null_layout == "head-tail") null_segments[regions - 1] = true;
    }
  }
  const int gpu_start = regions == 2 ? 1 : 2;
  std::vector<int> tx_sizes;
  if (tx_regions == 0) {
    tx_sizes = frame_size > kPrefix ? std::vector<int>{kPrefix, frame_size - kPrefix}
                                    : std::vector<int>{frame_size};
  } else {
    require(tx_regions >= 1 && tx_regions <= 5, "TX region count must be 1..5");
    require(frame_size == kPrefix + kPayload, "TX matrix requires the full frame size");
    tx_sizes = tx_regions == 1   ? std::vector<int>{endpoint ? frame_size - kHeader : frame_size}
               : tx_regions == 2 ? std::vector<int>{endpoint ? 1000 : kPrefix, kPayload}
               : tx_regions == 3
                   ? std::vector<int>{endpoint ? 19 : kHeader, endpoint ? 981 : 1000, kPayload}
               : tx_regions == 4
                   ? std::vector<int>{endpoint ? 19 : kHeader, 499, endpoint ? 482 : 501, kPayload}
                   : std::vector<int>{kHeader, 249, 250, 501, kPayload};
  }
  daqiri::NetworkConfig config{};
  config.common_.version = 1;
  config.common_.dir = daqiri::Direction::TX_RX;
  config.common_.stream_type = daqiri::StreamType::RAW;
  config.common_.engine = daqiri::EngineType::IBVERBS;
  config.common_.loopback_ = daqiri::LoopbackType::LOOPBACK_TYPE_HW;
  config.common_.master_core_ = std::stoi(setting("DAQIRI_PLATFORM_MASTER_CPU", "3"));
  config.tx_meta_buffers_ = config.rx_meta_buffers_ = 256;
  daqiri::InterfaceConfig intf{};
  intf.name_ = "loopback";
  intf.address_ = setting("DAQIRI_PLATFORM_IBVERBS_BDF");
  intf.rx_.flow_isolation_ = true;
  daqiri::TxQueueConfig tx{};
  tx.common_.id_ = kQueue;
  if (shared) {
    tx.buffer_mode_ = daqiri::TxBufferMode::SHARED;
    tx.max_inflight_sends_ = 8;
  }
  if (!tx_direct) {
    tx.common_.cpu_core_ = setting("DAQIRI_PLATFORM_TX_CPU", "1");
    tx.common_.batch_size_ = 1;
  }
  tx.poll_mode_ = tx_direct ? daqiri::QueuePollMode::DIRECT : daqiri::QueuePollMode::INDIRECT;
  for (size_t seg = 0; seg < tx_sizes.size(); ++seg) {
    const auto name = "tx-" + std::to_string(seg);
    const bool device = (seg > 0 || shared) && seg == tx_sizes.size() - 1;
    add_region(config, name, tx_sizes[seg],
               device ? daqiri::MemoryKind::DEVICE : daqiri::MemoryKind::HUGE, device ? gpu : numa);
    if (shared) config.mrs_[name].num_bufs_ = device ? 1 : 2;
    tx.common_.mrs_.push_back(name);
  }
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
    if (null_segments[seg]) {
      auto& region = config.mrs_.at(name);
      region.kind_ = daqiri::MemoryKind::NULL_MR;
      // These fields must never affect allocation, registration, or queue depth.
      region.buf_size_ = static_cast<size_t>(-1);
      region.num_bufs_ = 0;
      region.affinity_ = UINT16_MAX;
      region.access_ = UINT32_MAX;
      region.owned_ = false;
    }
    rx.common_.mrs_.push_back(name);
    rx.common_.mr_lengths_.push_back(null_segments[seg] && seg + 1 < regions ? sizes[seg] : 0);
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
  if (tx_regions > daqiri::MAX_NUM_SEGS) {
    require(initialized != daqiri::Status::SUCCESS, "Oversized TX region list accepted");
    std::cout << "PASS: oversized TX region list rejected\n";
    return;
  }
  if (regions > daqiri::MAX_NUM_SEGS) {
    require(initialized != daqiri::Status::SUCCESS, "Oversized region list accepted");
    std::cout << "PASS: oversized region list rejected\n";
    return;
  }
  require(initialized == daqiri::Status::SUCCESS, "daqiri_init failed");
  Session session;
  const int port = daqiri::get_port_id("loopback");
  require(port >= 0, "Missing port");
  daqiri::EndpointId destination = daqiri::INVALID_ENDPOINT_ID;
  if (endpoint) {
    daqiri::RawUdpEndpointConfig target;
    target.name_ = "gathered";
    target.interface_ = "loopback";
    target.dst_mac_ = mac_text;
    target.src_ipv4_ = "192.0.2.1";
    target.dst_ipv4_ = "192.0.2.2";
    target.src_port_ = target.dst_port_ = kUdp;
    target.mtu_ = frame_size;
    require(daqiri::add_endpoint(target, &destination) == daqiri::Status::SUCCESS,
            "Endpoint creation failed");
  }

  if (shared) {
    require(!tx_direct && regions == 2 && (endpoint || tx_regions > 1),
            "Invalid shared TX test layout");
    run_shared(tx_sizes, endpoint, destination, port, mac);
    return;
  }

  // TX cases cross slot and SQ wraparound with distinct bytes per packet.
  const int packets = tx_regions || !null_layout.empty() ? 1100 : 8;
  for (int packet = 0; packet < packets; ++packet) {
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
    int tx_offset = endpoint ? kHeader : 0;
    for (size_t seg = 0; seg < tx_sizes.size(); ++seg) {
      void* ptr = daqiri::get_segment_packet_ptr(outgoing.burst, seg, 0);
      if (seg > 0 && seg == tx_sizes.size() - 1) {
        cuda_check(
            cudaMemcpy(ptr, expected.data() + tx_offset, tx_sizes[seg], cudaMemcpyHostToDevice));
      } else {
        std::memcpy(ptr, expected.data() + tx_offset, tx_sizes[seg]);
      }
      outgoing.burst->pkt_lens[seg][0] = tx_sizes[seg];
      tx_offset += tx_sizes[seg];
    }
    require(tx_offset == frame_size, "TX segments do not cover the frame");
    if (!endpoint) {
      auto* prefix = static_cast<unsigned char*>(outgoing.burst->pkts[0][0]);
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
    }
    const auto sent = endpoint ? daqiri::send_tx_burst(destination, kQueue, outgoing.burst)
                               : daqiri::send_tx_burst(outgoing.burst);
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
    std::vector<unsigned char> received(frame_size);
    for (int seg = 0; seg < regions; ++seg) {
      require(incoming.burst->pkts[seg] && incoming.burst->pkt_lens[seg], "Missing segment arrays");
      const int length = std::min(sizes[seg], frame_size - offset);
      require(daqiri::get_segment_packet_length(incoming.burst, seg, 0) == length,
              "Incorrect segment length: " + std::to_string(seg));
      void* ptr = daqiri::get_segment_packet_ptr(incoming.burst, seg, 0);
      if (null_segments[seg]) {
        require(ptr == nullptr, "Discarded scatter element has a backing pointer");
        offset += length;
        continue;
      }
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
      std::copy(actual.begin(), actual.end(), received.begin() + offset);
      offset += length;
    }
    require(offset == frame_size, "RX segment lengths do not cover the frame");
    if (tx_regions) {
      require(received[16] * 256 + received[17] == frame_size - 14, "Incorrect IPv4 length");
      require(received[38] * 256 + received[39] == frame_size - 34, "Incorrect UDP length");
      require(received[36] * 256 + received[37] == kUdp, "Incorrect UDP destination");
      require(checksum_sum(received.data() + 14, 20) == 0xffff, "Incorrect IPv4 checksum");
      uint32_t sum = checksum_sum(received.data() + 26, 8, 17 + frame_size - 34);
      require(checksum_sum(received.data() + 34, frame_size - 34, sum) == 0xffff,
              "Incorrect UDP checksum over gathered segments");
    }
  }
  if (null_layout == "all") {
    auto completed = [&](daqiri::ResourceOpId operation) {
      daqiri::ResourceOpResult result{};
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (daqiri::poll_resource_op(&result) != daqiri::Status::SUCCESS) {
        require(std::chrono::steady_clock::now() < deadline, "Resource operation timed out");
        std::this_thread::sleep_for(std::chrono::microseconds(50));
      }
      require(result.op_id_ == operation && result.status_ == daqiri::Status::SUCCESS,
              "Null resource operation failed");
    };
    daqiri::MemoryRegionConfig runtime{};
    runtime.name_ = "runtime-discard";
    runtime.kind_ = daqiri::MemoryKind::NULL_MR;
    daqiri::ResourceOpId operation = 0;
    require(daqiri::add_memory_region_async(runtime, &operation) == daqiri::Status::SUCCESS,
            "Runtime null MR addition rejected");
    completed(operation);
    daqiri::RxQueueConfig runtime_queue{};
    runtime_queue.common_.id_ = 2;
    runtime_queue.common_.mrs_ = {runtime.name_};
    runtime_queue.poll_mode_ = daqiri::QueuePollMode::DIRECT;
    require(daqiri::add_rx_queue_async(port, runtime_queue, &operation) == daqiri::Status::SUCCESS,
            "Runtime null RX queue addition rejected");
    completed(operation);
    require(daqiri::delete_memory_region_async(runtime.name_, &operation) ==
                daqiri::Status::RESOURCE_IN_USE,
            "Deleted a null MR while its queue was active");
    require(daqiri::delete_rx_queue_async(port, 2, &operation) == daqiri::Status::SUCCESS,
            "Runtime null RX queue deletion rejected");
    completed(operation);
    require(
        daqiri::delete_memory_region_async(runtime.name_, &operation) == daqiri::Status::SUCCESS,
        "Runtime null MR deletion rejected");
    completed(operation);
  }
  std::cout << "PASS: "
            << (null_layout.empty() ? "exact CPU/GPU byte integrity"
                                    : "null scatter discard and retained byte integrity")
            << " across " << packets << " packets\n";
}
}  // namespace

int main(int argc, char** argv) {
  try {
    require(argc == 4 || argc == 7 || argc == 8,
            "Usage: multi_region_rx REGIONS FRAME_SIZE direct|indirect "
            "[TX_REGIONS direct|indirect raw|endpoint]");
    const std::string mode = argv[3];
    require(mode == "direct" || mode == "indirect", "Invalid polling mode");
    const std::string tx_mode = argc >= 7 ? argv[5] : "indirect";
    const std::string headers = argc >= 7 ? argv[6] : "raw";
    require(tx_mode == "direct" || tx_mode == "indirect", "Invalid TX polling mode");
    require(headers == "raw" || headers == "endpoint", "Invalid TX header mode");
    run(std::stoi(argv[1]), std::stoi(argv[2]), mode == "direct",
        argc >= 7 ? std::stoi(argv[4]) : 0, tx_mode == "direct", headers == "endpoint",
        argc == 8 && std::string(argv[7]) == "shared");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
