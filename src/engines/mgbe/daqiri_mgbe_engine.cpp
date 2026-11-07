// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#include "daqiri_mgbe_engine.h"
#include "mgbe_pmd.hpp"
#include <daqiri/logging.hpp>
#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace daqiri {
namespace {
constexpr size_t rx_batch = 64;
uint64_t thread_token() {
  static std::atomic<uint64_t> next{1};
  thread_local const uint64_t token = next.fetch_add(1, std::memory_order_relaxed);
  return token;
}
class DevicePort final : public MgbePort {
 public:
  DevicePort(const std::string& interface, uint32_t ring, uint32_t mtu)
      : device_("/dev/mgbe-pmd-" + interface, ring, mtu) {}
  mgbe::Queue& queue() override {
    return device_.queue();
  }
  void start() override {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!__atomic_load_n(&device_.status().link, __ATOMIC_ACQUIRE) &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    device_.start();
  }

 private:
  mgbe::Device device_;
};
uint16_t checksum(const void* bytes, size_t length) {
  const auto* p = static_cast<const uint8_t*>(bytes);
  uint32_t sum = 0;
  for (size_t i = 0; i < length; i += 2) sum += (uint16_t(p[i]) << 8) | p[i + 1];
  while (sum >> 16) sum = (sum & 0xffffU) + (sum >> 16);
  return htons(static_cast<uint16_t>(~sum));
}
}  // namespace

bool validate_mgbe_config(const NetworkConfig& config) {
  auto reject = [](const char* reason) {
    DAQIRI_LOG_ERROR("MGBE configuration: {}", reason);
    return false;
  };
  if (config.common_.stream_type != StreamType::RAW ||
      config.common_.loopback_ != LoopbackType::DISABLED || config.ifs_.size() != 1)
    return reject("requires raw Ethernet, one interface and no internal loopback");
  if (!config.rx_meta_buffers_ || config.rx_meta_buffers_ > 4096 || !config.tx_meta_buffers_ ||
      config.tx_meta_buffers_ > 4096)
    return reject("RX/TX metadata pool sizes must be 1..4096");
  const auto& port = config.ifs_.front();
  if (port.address_.size() != 7 || port.address_.compare(0, 4, "mgbe") || port.address_[4] < '0' ||
      port.address_[4] > '3' || port.address_.substr(5) != "_0")
    return reject("address must be an MGBE netdev name, mgbe0_0 through mgbe3_0");
  if (port.rx_.queues_.size() != 1 || port.tx_.queues_.size() != 1 || !port.rx_.flows_.empty() ||
      !port.tx_.flows_.empty() || !port.rx_.flex_items_.empty() ||
      !port.rx_.reorder_configs_.empty() || port.rx_.flow_isolation_ ||
      port.rx_.hardware_timestamps_ || port.tx_.accurate_send_)
    return reject("requires one RX/TX pair without flows, isolation, timestamps or reorder");
  const auto& rx = port.rx_.queues_[0];
  const auto& tx = port.tx_.queues_[0];
  if (rx.poll_mode_ != QueuePollMode::DIRECT || tx.poll_mode_ != QueuePollMode::DIRECT ||
      rx.common_.id_ != 0 || tx.common_.id_ != 0 || rx.cqe_compression_ || tx.pacing_mbps_ ||
      tx.buffer_mode_ != TxBufferMode::CYCLIC || tx.max_inflight_sends_)
    return reject("requires direct queue zero, cyclic TX and no pacing/compression");
  for (const auto* q : {&rx.common_, &tx.common_}) {
    if (q->mrs_.size() != 1 || !q->offloads_.empty() || q->split_boundary_ ||
        q->extra_queue_config_)
      return reject("each queue requires one host region without split or offloads");
    const auto it = config.mrs_.find(q->mrs_[0]);
    if (it == config.mrs_.end()) return reject("unknown queue memory region");
    const auto& mr = it->second;
    if (mr.kind_ != MemoryKind::HOST || !mr.owned_ || (mr.access_ & ~uint32_t(MEM_ACCESS_LOCAL)) ||
        mr.buf_size_ < 82 || mr.buf_size_ > 9014 || mr.num_bufs_ < 64 ||
        mr.num_bufs_ > MGBE_PMD_MAX_RING || (mr.num_bufs_ & (mr.num_bufs_ - 1)))
      return reject(
          "regions must be owned host DMA pools: 82..9014 bytes, 64..4096 power-of-two slots");
  }
  const auto& r = config.mrs_.at(rx.common_.mrs_[0]);
  const auto& t = config.mrs_.at(tx.common_.mrs_[0]);
  if (config.mrs_.size() != 2 || r.name_ == t.name_ || r.buf_size_ != t.buf_size_ ||
      r.num_bufs_ != t.num_bufs_)
    return reject("requires distinct RX/TX regions with equal frame capacities and slot counts");
  return true;
}

struct MgbeEngine::Impl {
  struct Lease {
    BurstParams burst{};
    std::array<void*, rx_batch> pointers{};
    std::array<uint32_t, rx_batch> lengths{};
    std::array<mgbe::Packet, rx_batch> packets{};
    std::array<bool, rx_batch> held{};
    size_t count = 0;
    bool active = false, tx = false;
  };
  std::unique_ptr<MgbePort> port;
  std::vector<std::unique_ptr<Lease>> storage;
  std::vector<Lease*> rx_free, tx_free;
  std::unordered_map<BurstParams*, Lease*> leases;
  Lease* reserved = nullptr;
  uint32_t capacity = 0;
  uint64_t submitted = 0;
  std::atomic<uint64_t> owner{0};
  bool claim() {
    const uint64_t token = thread_token();
    uint64_t current = owner.load(std::memory_order_relaxed);
    return current == token || (current == 0 && owner.compare_exchange_strong(
                                                    current, token, std::memory_order_relaxed));
  }
  Lease* find(BurstParams* burst) {
    if (!port || !claim()) return nullptr;
    auto it = leases.find(burst);
    return it != leases.end() && it->second->active ? it->second : nullptr;
  }
  Lease* take(bool tx) {
    auto& free = tx ? tx_free : rx_free;
    if (free.empty()) return nullptr;
    auto* meta = free.back();
    free.pop_back();
    meta->burst = {};
    meta->pointers.fill(nullptr);
    meta->lengths.fill(0);
    meta->held.fill(false);
    meta->count = 0;
    meta->active = true;
    meta->burst.pkts[0] = meta->pointers.data();
    meta->burst.pkt_lens[0] = meta->lengths.data();
    meta->burst.hdr.hdr.num_segs = 1;
    meta->burst.hdr.hdr.max_pkt = tx ? 1 : rx_batch;
    meta->burst.hdr.hdr.max_pkt_size = capacity;
    return meta;
  }
  void put(Lease& meta) {
    meta.active = false;
    (meta.tx ? tx_free : rx_free).push_back(&meta);
  }
  bool valid_tx(Lease* m) const {
    return m && m->tx && m->burst.hdr.hdr.port_id == 0 && m->burst.hdr.hdr.q_id == 0 &&
           m->burst.hdr.hdr.num_segs == 1 && m->burst.hdr.hdr.num_pkts == 1;
  }
  uint8_t* writable(BurstParams* burst, int index, size_t bytes) {
    auto* m = find(burst);
    if (!valid_tx(m) || reserved != m || index != 0 || !m->held[0] || bytes > capacity)
      return nullptr;
    return static_cast<uint8_t*>(m->packets[0].data);
  }
};

MgbeEngine::MgbeEngine() : impl_(std::make_unique<Impl>()) {}
MgbeEngine::~MgbeEngine() {
  shutdown();
}
std::unique_ptr<MgbePort> MgbeEngine::open_port(const std::string& interface, uint32_t ring,
                                                uint32_t mtu) {
  return std::make_unique<DevicePort>(interface, ring, mtu);
}
bool MgbeEngine::validate_config() const {
  return cfg_.common_.engine_type == EngineType::MGBE && validate_network_config(cfg_);
}
bool MgbeEngine::set_config_and_initialize(const NetworkConfig& config) {
  if (initialized_) return false;
  cfg_ = config;
  try {
    initialize();
    return initialized_;
  } catch (const std::exception& error) {
    DAQIRI_LOG_ERROR("MGBE initialization failed: {}", error.what());
    shutdown();
    return false;
  }
}
void MgbeEngine::initialize() {
  if (initialized_ || !validate_config() || !external_mrs_.empty())
    throw std::invalid_argument("invalid MGBE configuration or external memory binding");
  auto& s = *impl_;
  cfg_.ifs_[0].port_id_ = 0;
  const auto& mr = cfg_.mrs_.at(cfg_.ifs_[0].rx_.queues_[0].common_.mrs_[0]);
  s.capacity = mr.buf_size_;
  s.port = open_port(cfg_.ifs_[0].address_, mr.num_bufs_, mr.buf_size_ - 14);
  const size_t total = size_t(cfg_.rx_meta_buffers_) + cfg_.tx_meta_buffers_;
  s.storage.reserve(total);
  s.leases.reserve(total);
  s.rx_free.reserve(cfg_.rx_meta_buffers_);
  s.tx_free.reserve(cfg_.tx_meta_buffers_);
  for (size_t i = 0; i < total; ++i) {
    auto meta = std::make_unique<Impl::Lease>();
    meta->tx = i >= cfg_.rx_meta_buffers_;
    (meta->tx ? s.tx_free : s.rx_free).push_back(meta.get());
    s.leases.emplace(&meta->burst, meta.get());
    s.storage.push_back(std::move(meta));
  }
  s.port->start();
  initialized_ = true;
}
void MgbeEngine::run() {}  // Both queues are polled directly by their one owner.
void MgbeEngine::shutdown() {
  initialized_ = false;
  // The port destructor stops DMA before unmapping any buffers or registers.
  impl_->port.reset();
  impl_->reserved = nullptr;
  impl_->leases.clear();
  impl_->rx_free.clear();
  impl_->tx_free.clear();
  impl_->storage.clear();
  impl_->submitted = 0;
  impl_->owner.store(0, std::memory_order_relaxed);
}
void MgbeEngine::print_stats() {
  if (!initialized_ || !impl_->claim()) return;
  const auto& c = impl_->port->queue().counters();
  DAQIRI_LOG_INFO("MGBE RX packets={} errors={}, completed TX packets={}", c.rx_packets,
                  c.rx_errors, c.tx_packets);
}
BurstParams* MgbeEngine::create_tx_burst_params() {
  if (!initialized_ || !impl_->claim()) return nullptr;
  auto* meta = impl_->take(true);
  return meta ? &meta->burst : nullptr;
}
Status MgbeEngine::get_rx_burst(BurstParams** burst, int port, int queue) {
  if (!burst) return Status::NULL_PTR;
  *burst = nullptr;
  if (!initialized_) return Status::NOT_READY;
  auto& s = *impl_;
  if (port != 0 || queue != 0 || !s.claim()) return Status::INVALID_PARAMETER;
  auto* m = s.take(false);
  if (!m) return Status::NO_FREE_BURST_BUFFERS;
  try {
    s.port->queue().reclaim();
    m->count = s.port->queue().receive(m->packets.data(), rx_batch);
    if (!m->count) {
      s.put(*m);
      return Status::NOT_READY;
    }
    m->burst.hdr.hdr.num_pkts = m->count;
    for (size_t i = 0; i < m->count; ++i) {
      m->held[i] = true;
      m->pointers[i] = m->packets[i].data;
      m->lengths[i] = m->packets[i].length;
      m->burst.hdr.hdr.nbytes += m->lengths[i];
    }
    *burst = &m->burst;
    return Status::SUCCESS;
  } catch (const std::exception& error) {
    DAQIRI_LOG_ERROR("MGBE receive failed: {}", error.what());
    s.put(*m);
    return Status::INTERNAL_ERROR;
  }
}
bool MgbeEngine::is_tx_burst_available(BurstParams* burst) {
  if (!initialized_ || !impl_->claim()) return false;
  auto& s = *impl_;
  if (!s.valid_tx(s.find(burst)) || s.reserved) return false;
  try {
    const bool available = s.port->queue().tx_buffer() != nullptr;
    if (available) s.port->queue().cancel_tx();
    return available;
  } catch (...) {
    return false;
  }
}
Status MgbeEngine::get_tx_packet_burst(BurstParams* burst) {
  if (!burst) return Status::NULL_PTR;
  if (!initialized_) return Status::NOT_READY;
  auto& s = *impl_;
  auto* m = s.find(burst);
  if (!s.claim() || !s.valid_tx(m)) return Status::INVALID_PARAMETER;
  if (s.reserved) return Status::RESOURCE_IN_USE;
  try {
    auto* ptr = s.port->queue().tx_buffer();
    if (!ptr) return Status::NO_FREE_PACKET_BUFFERS;
    s.reserved = m;
    m->count = 1;
    m->held[0] = true;
    m->pointers[0] = ptr;
    m->packets[0].data = ptr;
    return Status::SUCCESS;
  } catch (...) {
    return Status::INTERNAL_ERROR;
  }
}
Status MgbeEngine::send_tx_burst(BurstParams* burst) {
  if (!burst) return Status::NULL_PTR;
  if (!initialized_) return Status::NOT_READY;
  auto& s = *impl_;
  auto* m = s.find(burst);
  if (!s.claim() || !s.valid_tx(m) || s.reserved != m || !m->held[0] ||
      m->pointers[0] != m->packets[0].data || m->lengths[0] < 14 || m->lengths[0] > s.capacity)
    return Status::INVALID_PARAMETER;
  try {
    s.port->queue().submit(m->lengths[0]);
    ++s.submitted;
    s.reserved = nullptr;
    m->held[0] = false;
    m->pointers[0] = nullptr;
    s.put(*m);  // Hardware owns data until the driver's completion reclamation.
    return Status::SUCCESS;
  } catch (...) {
    return Status::INTERNAL_ERROR;
  }
}
void* MgbeEngine::get_packet_ptr(BurstParams* burst, int index) {
  auto* m = impl_->find(burst);
  return m && index >= 0 && size_t(index) < m->count && m->held[index] ? m->pointers[index]
                                                                       : nullptr;
}
uint32_t MgbeEngine::get_packet_length(BurstParams* burst, int index) {
  auto* m = impl_->find(burst);
  return get_packet_ptr(burst, index) ? m->lengths[index] : 0;
}
void* MgbeEngine::get_segment_packet_ptr(BurstParams* b, int s, int i) {
  return s == 0 ? get_packet_ptr(b, i) : nullptr;
}
uint32_t MgbeEngine::get_segment_packet_length(BurstParams* b, int s, int i) {
  return s == 0 ? get_packet_length(b, i) : 0;
}
FlowId MgbeEngine::get_packet_flow_id(BurstParams*, int) {
  return 0;
}
void* MgbeEngine::get_packet_extra_info(BurstParams*, int) {
  return nullptr;
}
Status MgbeEngine::get_packet_rx_timestamp(BurstParams* b, int i, uint64_t* time) {
  if (!time) return Status::NULL_PTR;
  *time = 0;
  return get_packet_ptr(b, i) ? Status::NOT_SUPPORTED : Status::INVALID_PARAMETER;
}
Status MgbeEngine::set_packet_tx_time(BurstParams*, int, uint64_t) {
  return Status::NOT_SUPPORTED;
}
Status MgbeEngine::set_packet_lengths(BurstParams* b, int i,
                                      const std::initializer_list<int>& lens) {
  if (lens.size() != 1 || *lens.begin() < 14 || !impl_->writable(b, i, size_t(*lens.begin())))
    return Status::INVALID_PARAMETER;
  b->hdr.hdr.nbytes = *lens.begin();
  impl_->find(b)->lengths[i] = *lens.begin();
  return Status::SUCCESS;
}
Status MgbeEngine::set_eth_header(BurstParams* b, int i, char* destination) {
  if (!destination) return Status::NULL_PTR;
  auto* p = reinterpret_cast<UDPIPV4Pkt*>(impl_->writable(b, i, sizeof(UDPIPV4Pkt)));
  if (!p) return Status::INVALID_PARAMETER;
  std::memcpy(p->eth.h_dest, destination, 6);
  std::memcpy(p->eth.h_source, impl_->port->queue().info().mac, 6);
  p->eth.h_proto = htons(ETH_P_IP);
  return Status::SUCCESS;
}
Status MgbeEngine::set_ipv4_header(BurstParams* b, int i, int length, uint8_t protocol,
                                   unsigned int source, unsigned int destination) {
  auto* p = reinterpret_cast<UDPIPV4Pkt*>(impl_->writable(b, i, sizeof(UDPIPV4Pkt)));
  if (!p || length < int(sizeof(iphdr)) || size_t(length) + sizeof(ethhdr) > impl_->capacity)
    return Status::INVALID_PARAMETER;
  p->ip = {};
  p->ip.version = 4;
  p->ip.ihl = 5;
  p->ip.ttl = 64;
  p->ip.tot_len = htons(length);
  p->ip.protocol = protocol;
  p->ip.saddr = source;
  p->ip.daddr = destination;
  p->ip.check = checksum(&p->ip, sizeof(p->ip));
  return Status::SUCCESS;
}
Status MgbeEngine::set_udp_header(BurstParams* b, int i, int length, uint16_t source,
                                  uint16_t destination) {
  auto* p = reinterpret_cast<UDPIPV4Pkt*>(impl_->writable(b, i, sizeof(UDPIPV4Pkt)));
  if (!p || length < int(sizeof(udphdr)) || size_t(length) + 34 > impl_->capacity)
    return Status::INVALID_PARAMETER;
  p->udp = {};
  p->udp.source = htons(source);
  p->udp.dest = htons(destination);
  p->udp.len = htons(length);
  return Status::SUCCESS;
}
Status MgbeEngine::set_udp_payload(BurstParams* b, int i, void* data, int length) {
  if (length < 0 || (!data && length)) return Status::INVALID_PARAMETER;
  auto* p = impl_->writable(b, i, sizeof(UDPIPV4Pkt) + size_t(length));
  if (!p) return Status::INVALID_PARAMETER;
  if (length) std::memcpy(p + sizeof(UDPIPV4Pkt), data, length);
  return Status::SUCCESS;
}
void MgbeEngine::free_packet(BurstParams* b, int i) {
  auto& s = *impl_;
  auto* m = s.find(b);
  if (!m || !s.claim() || i < 0 || size_t(i) >= m->count || !m->held[i]) return;
  try {
    if (m->tx) {
      if (s.reserved == m) {
        s.port->queue().cancel_tx();
        s.reserved = nullptr;
      }
    } else {
      s.port->queue().release(m->packets[i]);
    }
  } catch (...) { /* A faulted session is reclaimed at shutdown. */
  }
  m->held[i] = false;
  m->pointers[i] = nullptr;
}
void MgbeEngine::free_all_packets(BurstParams* b) {
  auto* m = impl_->find(b);
  if (m)
    for (size_t i = 0; i < m->count; ++i) free_packet(b, i);
}
void MgbeEngine::free_packet_segment(BurstParams* b, int s, int i) {
  if (s == 0) free_packet(b, i);
}
void MgbeEngine::free_all_segment_packets(BurstParams* b, int s) {
  if (s == 0) free_all_packets(b);
}
void MgbeEngine::free_rx_burst(BurstParams* b) {
  auto* m = impl_->find(b);
  if (!m || m->tx || !impl_->claim()) return;
  free_all_packets(b);
  impl_->put(*m);
}
void MgbeEngine::free_tx_burst(BurstParams* b) {
  auto* m = impl_->find(b);
  if (!m || !m->tx || !impl_->claim()) return;
  free_all_packets(b);
  impl_->put(*m);
}
void MgbeEngine::free_rx_metadata(BurstParams* b) {
  free_rx_burst(b);
}
void MgbeEngine::free_tx_metadata(BurstParams* b) {
  free_tx_burst(b);
}
uint64_t MgbeEngine::get_burst_tot_byte(BurstParams* b) {
  auto* m = impl_->find(b);
  uint64_t total = 0;
  if (m)
    for (size_t i = 0; i < m->count; ++i)
      if (m->held[i]) total += m->lengths[i];
  return total;
}
Status MgbeEngine::get_mac_addr(int port, char* mac) {
  if (!mac) return Status::NULL_PTR;
  if (!initialized_ || port != 0) return Status::INVALID_PARAMETER;
  std::memcpy(mac, impl_->port->queue().info().mac, 6);
  return Status::SUCCESS;
}
Status MgbeEngine::wait_for_tx_idle(uint32_t timeout_ms) {
  if (!initialized_) return Status::NOT_READY;
  if (!impl_->claim()) return Status::INVALID_PARAMETER;
  if (impl_->reserved) return Status::RESOURCE_IN_USE;
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  try {
    do {
      impl_->port->queue().reclaim();
      if (impl_->port->queue().counters().tx_packets == impl_->submitted) return Status::SUCCESS;
    } while (std::chrono::steady_clock::now() < end);
  } catch (...) {
    return Status::INTERNAL_ERROR;
  }
  return Status::NOT_READY;
}
}  // namespace daqiri
