// SPDX-License-Identifier: Apache-2.0
#include "mgbe_pmd.hpp"
#include <endian.h>
#include <array>
#include <cassert>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
constexpr uint32_t own = 1U << 31, first_last = (1U << 29) | (1U << 28);
void throws(const std::function<void()>& f) {
  bool caught = false;
  try {
    f();
  } catch (const std::exception&) {
    caught = true;
  }
  assert(caught);
}
struct Fixture {
  std::array<mgbe::Descriptor, 64> rx{}, tx{};
  std::array<uint8_t, 64 * 2048> rx_pool{}, tx_pool{};
  std::array<uint32_t, 1024> regs{};
  mgbe_pmd_status status{MGBE_PMD_RUNNING, 0, 1, 0};
  mgbe_pmd_info info{};
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
    for (auto& d : rx) d.word[3] = htole32(own);
  }
  mgbe::Queue::Mapping mapping() {
    return {rx.data(), tx.data(), rx_pool.data(), tx_pool.data(), regs.data(), &status, info};
  }
  void complete_rx(uint32_t slot, uint32_t length = 64, uint32_t flags = first_last) {
    rx[slot].word[3] = htole32(flags | length);
    regs[info.rx_completed_offset / 4] = htole32(slot << 16);
  }
};

void receive_ownership_and_wrap() {
  Fixture f;
  mgbe::Queue q(f.mapping());
  std::array<mgbe::Packet, 64> p;
  assert(q.receive(p.data(), p.size()) == 0);
  for (uint32_t i = 0; i < 64; ++i) f.complete_rx(i);
  assert(q.receive(p.data(), p.size()) == 64);
  assert(q.receive(p.data(), p.size()) == 0);
  auto stale = p[0];
  for (uint32_t i = 63; i > 0; --i) {
    q.release(p[i]);
    assert(!(le32toh(f.rx[i].word[3]) & own));
  }
  q.release(p[0]);
  for (const auto& d : f.rx) assert(le32toh(d.word[3]) & own);
  assert(le32toh(f.regs[f.info.rx_tail_offset / 4]) == f.info.rx_ring_dma + 64 * 16);
  throws([&] { q.release(p[0]); });
  // More than one hardware ring revolution, repeatedly wrapping both indices.
  for (uint32_t round = 0; round < 2000; ++round) {
    const uint32_t slot = round & 63;
    f.complete_rx(slot);
    assert(q.receive(p.data(), 1) == 1);
    assert(p[0].slot == slot);
    if (slot == 0) throws([&] { q.release(stale); });
    std::memset(p[0].data, round & 255, p[0].length);
    q.release(p[0]);
    assert(le32toh(f.rx[slot].word[0]) == f.info.rx_pool_dma + slot * 2048);
  }
  assert(q.counters().rx_packets == 2064);
}

void malformed_and_partial_frames() {
  Fixture f;
  mgbe::Queue q(f.mapping());
  std::array<mgbe::Packet, 8> p;
  f.complete_rx(0, 64, first_last | (1U << 15));
  f.complete_rx(1, 64, 1U << 29);  // Multi-descriptor packet: no last flag.
  f.complete_rx(2, 0, 1U << 30);   // Timestamp context, not a frame.
  f.complete_rx(3, 1519);
  f.complete_rx(4, 13);
  f.complete_rx(5, 128);
  assert(q.receive(p.data(), p.size()) == 1);
  assert(p[0].slot == 5 && p[0].length == 128);
  assert(q.counters().rx_errors == 5);
  assert(q.counters().rx_bytes == 128);
  q.release(p[0]);
  throws([&] { q.receive(nullptr, 1); });
}

void transmit_exhaustion_and_reuse() {
  Fixture f;
  mgbe::Queue q(f.mapping());
  throws([&] { q.submit(64); });
  for (uint32_t i = 0; i < 63; ++i) {
    auto* p = q.tx_buffer();
    assert(p == f.tx_pool.data() + i * 2048);
    // Reclaim must not overwrite the outstanding reserved slot.
    assert(q.tx_buffer() == p);
    std::memset(p, i, 64);
    q.submit(64);
    assert(le32toh(f.tx[i].word[0]) == f.info.tx_pool_dma + i * 2048);
    assert(le32toh(f.tx[i].word[3]) == (own | first_last | 64));
  }
  assert(q.tx_buffer() == nullptr);
  assert(q.reclaim() == 0);
  f.tx[1].word[3] = 0;  // Out-of-order completion cannot release slot zero.
  assert(q.reclaim() == 0);
  f.tx[0].word[3] = 0;
  assert(q.reclaim() == 2);
  assert(q.tx_buffer() == f.tx_pool.data() + 63 * 2048);
  throws([&] { q.submit(1519); });
  q.cancel_tx();
  throws([&] { q.submit(64); });
  assert(q.tx_buffer());
  q.submit(100);
  for (uint32_t i = 2; i < 64; ++i) f.tx[i].word[3] = 0;
  // Actual MGBE completion retains the frame length, including bits that
  // represent errors only on EQOS. Do not invent MGBE transmission errors.
  f.tx[3].word[3] = htole32(first_last | 1514);
  assert(q.reclaim() == 62);
  assert(q.counters().tx_packets == 64);
  assert(q.counters().tx_errors == 0);
  assert(q.counters().tx_bytes == 63 * 64 + 100);
  assert(q.tx_buffer() == f.tx_pool.data());
  q.cancel_tx();
}

void capabilities_and_faults() {
  Fixture f;
  auto m = f.mapping();
  m.info.ring_size = 65;
  throws([&] { mgbe::Queue q(m); });
  m = f.mapping();
  m.info.rx_tail_offset = 4096;
  throws([&] { mgbe::Queue q(m); });
  m = f.mapping();
  m.info.rx_ring_dma = 0xfffffff0ULL;
  throws([&] { mgbe::Queue q(m); });
  m = f.mapping();
  m.info.abi = 99;
  throws([&] { mgbe::Queue q(m); });
  mgbe::Queue q(f.mapping());
  f.complete_rx(0);
  mgbe::Packet p;
  assert(q.receive(&p, 1) == 1);
  f.status.state = MGBE_PMD_FAULT;
  throws([&] { q.receive(&p, 1); });
  throws([&] { q.release(p); });
  throws([&] { q.tx_buffer(); });
  throws([&] { q.reclaim(); });
  throws([&] { mgbe::Device d("/this/mgbe/device/does/not/exist"); });
}
}  // namespace

int main() {
  static_assert(sizeof(mgbe::Descriptor) == 16);
  static_assert(sizeof(mgbe_pmd_info) == 80);
  static_assert(sizeof(mgbe_pmd_config) == 16);
  receive_ownership_and_wrap();
  malformed_and_partial_frames();
  transmit_exhaustion_and_reuse();
  capabilities_and_faults();
  std::cout << "MGBE queue ownership, wraparound, exhaustion and fault tests passed\n";
}
