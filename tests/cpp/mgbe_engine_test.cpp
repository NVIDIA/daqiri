// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#include "src/engines/mgbe/daqiri_mgbe_engine.h"
#include "mgbe_pmd.hpp"
#include <daqiri/daqiri.h>
#include <endian.h>
#include <array>
#include <cassert>
#include <cstring>
#include <functional>
#include <thread>

using namespace daqiri;
namespace {
constexpr uint32_t own = 1U << 31, first_last = (1U << 29) | (1U << 28);
struct Fixture {
  std::array<mgbe::Descriptor, 64> rx{}, tx{};
  std::array<uint8_t, 64 * 2048> rx_pool{}, tx_pool{};
  std::array<uint32_t, 1024> regs{};
  mgbe_pmd_status status{MGBE_PMD_RUNNING, 0, 1, 0};
  mgbe_pmd_info info{};
  int opened = 0, closed = 0;
  bool fail_start = false;
  Fixture() {
    info.abi = MGBE_PMD_ABI;
    info.ring_size = 64;
    info.mtu = 1500;
    info.slot_size = 2048;
    info.reg_size = 4096;
    info.rx_tail_offset = 0x12c;
    info.tx_tail_offset = 0x124;
    info.rx_completed_offset = 0x17c;
    info.rx_ring_dma = 0x10000;
    info.tx_ring_dma = 0x20000;
    info.rx_pool_dma = 0x30000;
    info.tx_pool_dma = 0x60000;
    info.mac[0] = 2;
    info.mac[5] = 7;
    for (auto& d : rx) d.word[3] = htole32(own);
  }
  mgbe::Queue::Mapping mapping() {
    return {rx.data(), tx.data(), rx_pool.data(), tx_pool.data(), regs.data(), &status, info};
  }
  void receive(size_t slot) {
    rx[slot].word[3] = htole32(first_last | 60);
    regs[info.rx_completed_offset / 4] = htole32(slot << 16);
    std::memset(rx_pool.data() + slot * 2048, int(slot + 1), 60);
  }
};
class FakePort : public MgbePort {
 public:
  explicit FakePort(Fixture& f) : f_(f), queue_(f.mapping()) {
    ++f_.opened;
  }
  ~FakePort() override {
    ++f_.closed;
  }
  mgbe::Queue& queue() override {
    return queue_;
  }
  void start() override {
    if (f_.fail_start) throw std::runtime_error("simulated link fault");
  }

 private:
  Fixture& f_;
  mgbe::Queue queue_;
};
class TestEngine : public MgbeEngine {
 public:
  explicit TestEngine(Fixture& f) : f_(f) {}

 protected:
  std::unique_ptr<MgbePort> open_port(const std::string&, uint32_t ring, uint32_t mtu) override {
    assert(ring == 64 && mtu == 1500);
    return std::make_unique<FakePort>(f_);
  }

 private:
  Fixture& f_;
};
NetworkConfig config() {
  NetworkConfig cfg{};
  assert(parse_network_config_from_yaml_file(MGBE_TEST_CONFIG, cfg) == Status::SUCCESS);
  for (auto& [name, mr] : cfg.mrs_) mr.num_bufs_ = 64;
  cfg.rx_meta_buffers_ = cfg.tx_meta_buffers_ = 2;
  assert(validate_network_config(cfg));
  return cfg;
}
BurstParams* tx_burst(TestEngine& engine) {
  auto* b = engine.create_tx_burst_params();
  assert(b);
  set_header(b, 0, 0, 1, 1);
  assert(engine.get_tx_packet_burst(b) == Status::SUCCESS);
  return b;
}
void validation() {
  assert(config_engine_from_string("mgbe", StreamType::RAW) == EngineType::MGBE);
  assert(engine_type_from_string("mgbe") == EngineType::MGBE);
  assert(engine_type_to_string(EngineType::MGBE) == "mgbe");
  assert(!engine_type_supports_stream_type(EngineType::MGBE, StreamType::SOCKET));
  const std::function<void(NetworkConfig&)> invalid[] = {
      [](auto& c) { c.common_.stream_type = StreamType::SOCKET; },
      [](auto& c) { c.common_.loopback_ = LoopbackType::LOOPBACK_TYPE_SW; },
      [](auto& c) { c.ifs_.push_back(c.ifs_[0]); },
      [](auto& c) { c.ifs_[0].address_ = "../../not-a-netdev"; },
      [](auto& c) { c.ifs_[0].rx_.hardware_timestamps_ = true; },
      [](auto& c) { c.ifs_[0].rx_.flow_isolation_ = true; },
      [](auto& c) { c.ifs_[0].rx_.queues_[0].poll_mode_ = QueuePollMode::INDIRECT; },
      [](auto& c) { c.ifs_[0].tx_.queues_[0].common_.id_ = 1; },
      [](auto& c) { c.ifs_[0].tx_.queues_[0].common_.offloads_.push_back("tx_eth_src"); },
      [](auto& c) { c.ifs_[0].tx_.queues_[0].pacing_mbps_ = 1; },
      [](auto& c) { c.ifs_[0].tx_.accurate_send_ = true; },
      [](auto& c) { c.mrs_.at("RX").kind_ = MemoryKind::DEVICE; },
      [](auto& c) { c.mrs_.at("RX").owned_ = false; },
      [](auto& c) { c.mrs_.at("RX").num_bufs_ = 63; },
      [](auto& c) { c.mrs_.at("RX").buf_size_ = 9015; },
      [](auto& c) { c.rx_meta_buffers_ = 0; },
  };
  for (const auto& change : invalid) {
    auto cfg = config();
    change(cfg);
    Fixture f;
    TestEngine engine(f);
    assert(!engine.set_config_and_initialize(cfg));
    assert(f.opened == 0);  // Validation happens before device/DMA acquisition.
  }
}
void rx_ownership() {
  Fixture f;
  TestEngine engine(f);
  assert(engine.set_config_and_initialize(config()));
  BurstParams* first = nullptr;
  assert(engine.get_rx_burst(&first, 0, 0) == Status::NOT_READY && !first);
  f.receive(0);
  assert(engine.get_rx_burst(&first, 0, 0) == Status::SUCCESS);
  assert(engine.get_packet_length(first, 0) == 60);
  assert(engine.get_packet_ptr(first, 0) == f.rx_pool.data());
  assert(!engine.get_packet_ptr(first, -1) && !engine.get_packet_ptr(first, 1));
  f.receive(1);
  BurstParams* second = nullptr;
  assert(engine.get_rx_burst(&second, 0, 0) == Status::SUCCESS);
  BurstParams* extra = nullptr;
  assert(engine.get_rx_burst(&extra, 0, 0) == Status::NO_FREE_BURST_BUFFERS);
  engine.free_rx_burst(second);
  assert(!(le32toh(f.rx[1].word[3]) & own));  // Earlier held buffer blocks rearm.
  assert(static_cast<uint8_t*>(engine.get_packet_ptr(first, 0))[0] == 1);
  engine.free_all_packets(first);
  engine.free_rx_burst(first);
  engine.free_rx_burst(first);  // Duplicate release must not duplicate metadata.
  assert((le32toh(f.rx[0].word[3]) & own) && (le32toh(f.rx[1].word[3]) & own));
  f.receive(2);
  assert(engine.get_rx_burst(&first, 0, 0) == Status::SUCCESS);
  uint64_t timestamp = 1;
  assert(engine.get_packet_rx_timestamp(first, 0, &timestamp) == Status::NOT_SUPPORTED &&
         !timestamp);
  engine.free_rx_metadata(first);
  Status foreign = Status::SUCCESS;
  std::thread other([&] {
    BurstParams* b = nullptr;
    foreign = engine.get_rx_burst(&b, 0, 0);
  });
  other.join();
  assert(foreign == Status::INVALID_PARAMETER);
  f.status.state = MGBE_PMD_FAULT;
  assert(engine.get_rx_burst(&first, 0, 0) == Status::INTERNAL_ERROR && !first);
  engine.shutdown();
  assert(f.closed == 1 && !engine.is_initialized());
}
void tx_ownership_and_headers() {
  Fixture f;
  TestEngine engine(f);
  assert(engine.set_config_and_initialize(config()));
  auto* first = tx_burst(engine);
  auto* second = engine.create_tx_burst_params();
  assert(second && !engine.create_tx_burst_params());
  set_header(second, 0, 0, 1, 1);
  assert(engine.get_tx_packet_burst(second) == Status::RESOURCE_IN_USE);
  assert(engine.set_packet_lengths(first, 0, {1515}) == Status::INVALID_PARAMETER);
  assert(engine.set_packet_lengths(first, 0, {60}) == Status::SUCCESS);
  char mac[6] = {2, 0, 0, 0, 0, 8};
  assert(engine.set_eth_header(first, 0, mac) == Status::SUCCESS);
  assert(engine.set_ipv4_header(first, 0, 46, IPPROTO_UDP, htonl(0xc0000201), htonl(0xc0000202)) ==
         Status::SUCCESS);
  assert(engine.set_udp_header(first, 0, 26, 9000, 9001) == Status::SUCCESS);
  std::array<uint8_t, 18> payload{};
  payload.fill(0xa5);
  assert(engine.set_udp_payload(first, 0, payload.data(), payload.size()) == Status::SUCCESS);
  auto* frame = static_cast<uint8_t*>(engine.get_packet_ptr(first, 0));
  assert(frame == f.tx_pool.data() && frame[6] == 2 && frame[11] == 7);
  uint32_t sum = 0;
  for (int i = 14; i < 34; i += 2) sum += (uint16_t(frame[i]) << 8) | frame[i + 1];
  while (sum >> 16) sum = (sum & 0xffffU) + (sum >> 16);
  assert(sum == 0xffff && frame[42] == 0xa5);
  assert(engine.send_tx_burst(first) == Status::SUCCESS);
  assert(!engine.get_packet_ptr(first, 0));
  assert(le32toh(f.tx[0].word[3]) & own);
  assert(engine.wait_for_tx_idle(0) == Status::NOT_READY);
  assert(engine.get_tx_packet_burst(second) == Status::SUCCESS);
  assert(engine.get_packet_ptr(second, 0) == f.tx_pool.data() + 2048);
  engine.free_tx_metadata(second);  // Cancel without submitting its descriptor.
  f.tx[0].word[3] &= htole32(~own);
  assert(engine.wait_for_tx_idle(0) == Status::SUCCESS);
  // Exhaust the real mock TX ring: hardware-owned buffers cannot be reused.
  for (int i = 0; i < 63; ++i) {
    auto* b = tx_burst(engine);
    assert(engine.set_packet_lengths(b, 0, {60}) == Status::SUCCESS);
    assert(engine.send_tx_burst(b) == Status::SUCCESS);
  }
  auto* full = engine.create_tx_burst_params();
  set_header(full, 0, 0, 1, 1);
  assert(engine.get_tx_packet_burst(full) == Status::NO_FREE_PACKET_BUFFERS);
  engine.free_tx_burst(full);
}
void partial_failure() {
  Fixture f;
  f.fail_start = true;
  TestEngine engine(f);
  assert(!engine.set_config_and_initialize(config()));
  assert(f.opened == 1 && f.closed == 1);
  f.fail_start = false;
  assert(engine.set_config_and_initialize(config()));
  engine.shutdown();
  assert(f.closed == 2);
}
}  // namespace
int main() {
  validation();
  rx_ownership();
  tx_ownership_and_headers();
  partial_failure();
}
