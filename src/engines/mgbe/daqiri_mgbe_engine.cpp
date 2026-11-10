// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#include "daqiri_mgbe_engine.h"
#include "mgbe_pmd.hpp"
#include "src/daqiri_ring.h"
#include <pthread.h>
#include <future>
#include <system_error>
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
#include <utility>

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
  if (rx.common_.id_ != 0 || tx.common_.id_ != 0 || rx.cqe_compression_ || tx.pacing_mbps_ ||
      tx.buffer_mode_ != TxBufferMode::CYCLIC || tx.max_inflight_sends_)
    return reject("requires queue zero, cyclic TX and no pacing/compression");
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
  for (const auto* q : {&rx.common_, &tx.common_}) {
    const bool indirect = q == &rx.common_ ? rx.poll_mode_ == QueuePollMode::INDIRECT
                                           : tx.poll_mode_ == QueuePollMode::INDIRECT;
    if (indirect) {
      if (q->batch_size_ < 1 || q->batch_size_ > int(rx_batch) || q->cpu_core_.empty() ||
          q->cpu_core_.find_first_not_of("0123456789") != std::string::npos)
        return reject("indirect queues require batch_size 1..64 and a numeric cpu_core");
      try {
        if (std::stoul(q->cpu_core_) >= CPU_SETSIZE)
          return reject("worker CPU exceeds CPU_SETSIZE");
      } catch (...) {
        return reject("invalid worker CPU");
      }
    }
  }
  if (rx.timeout_us_ > uint64_t(INT64_MAX) / 1000)
    return reject("RX timeout exceeds clock duration range");
  const auto& r = config.mrs_.at(rx.common_.mrs_[0]);
  const auto& t = config.mrs_.at(tx.common_.mrs_[0]);
  if (config.mrs_.size() != 2 || r.name_ == t.name_ || r.buf_size_ != t.buf_size_ ||
      r.num_bufs_ != t.num_bufs_)
    return reject("requires distinct RX/TX regions with equal frame capacities and slot counts");
  return true;
}

struct MgbeEngine::Impl {
  enum class State { FREE, APP, WORKER };
  struct TxSlot {
    uint32_t index;
    uint8_t* data;
  };
  struct RxReturn {
    mgbe::Packet packet;
  };
  struct Lease {
    BurstParams burst{};
    std::array<void*, rx_batch> pointers{};
    std::array<uint32_t, rx_batch> lengths{};
    std::array<mgbe::Packet, rx_batch> packets{};
    std::array<void*, rx_batch> slots{};
    std::array<bool, rx_batch> held{};
    size_t count = 0;
    std::atomic<State> state{State::FREE};
    bool tx = false;
  };
  using RingPtr = std::unique_ptr<Ring, decltype(&Ring::free)>;
  static RingPtr ring(size_t capacity) {
    RingPtr result(Ring::create("mgbe", capacity + 1, RingMode::MPMC), &Ring::free);
    if (!result) throw std::bad_alloc();
    return result;
  }
  RingPtr rx_free{nullptr, &Ring::free}, tx_free{nullptr, &Ring::free};
  RingPtr rx_ready{nullptr, &Ring::free}, rx_returned{nullptr, &Ring::free};
  RingPtr tx_ready{nullptr, &Ring::free}, tx_free_slots{nullptr, &Ring::free};
  std::unique_ptr<MgbePort> port;
  std::vector<std::unique_ptr<Lease>> storage;
  std::vector<Lease*> rx_direct_free, tx_direct_free;
  std::unordered_map<BurstParams*, Lease*> leases;
  std::vector<TxSlot> tx_slots;
  std::vector<RxReturn> rx_slots;
  std::vector<TxSlot*> inflight;
  uint64_t posted = 0, cleaned = 0;
  Lease *reserved = nullptr, *rx_pending = nullptr, *tx_pending = nullptr;
  size_t tx_offset = 0;
  std::chrono::steady_clock::time_point rx_since;
  std::chrono::nanoseconds rx_timeout{0};
  uint32_t capacity = 0, rx_batch_size = rx_batch, tx_batch_size = 1;
  bool rx_indirect = false, tx_indirect = false;
  alignas(64) std::atomic<uint64_t> submitted{0};
  alignas(64) std::atomic<uint64_t> completed{0};
  alignas(64) std::atomic<uint64_t> received{0}, rx_errors{0};
  uint64_t rx_seen = 0, rx_error_seen = 0;
  std::atomic<uint32_t> reservations{0};
  std::atomic<uint64_t> owner{0};
  std::atomic<bool> stop{false}, fault{false};
  std::vector<std::thread> workers;
  bool claim(bool tx) {
    if (tx ? tx_indirect : rx_indirect) return true;
    const uint64_t token = thread_token();
    uint64_t current = owner.load(std::memory_order_relaxed);
    return current == token || (current == 0 && owner.compare_exchange_strong(
                                                    current, token, std::memory_order_relaxed));
  }
  Lease* find(BurstParams* burst) {
    if (!port || !burst) return nullptr;
    const auto it = std::as_const(leases).find(burst);
    if (it == leases.end()) return nullptr;
    auto* m = it->second;
    return claim(m->tx) && m->state.load(std::memory_order_acquire) == State::APP ? m : nullptr;
  }
  Lease* take(bool tx) {
    void* ptr = nullptr;
    if (tx ? tx_indirect : rx_indirect) {
      if (!(tx ? tx_free : rx_free)->dequeue(&ptr)) return nullptr;
    } else {
      auto& free = tx ? tx_direct_free : rx_direct_free;
      if (free.empty()) return nullptr;
      ptr = free.back();
      free.pop_back();
    }
    auto* meta = static_cast<Lease*>(ptr);
    meta->burst = {};
    meta->pointers.fill(nullptr);
    meta->lengths.fill(0);
    meta->held.fill(false);
    meta->count = 0;
    meta->burst.pkts[0] = meta->pointers.data();
    meta->burst.pkt_lens[0] = meta->lengths.data();
    meta->burst.hdr.hdr.num_segs = 1;
    meta->burst.hdr.hdr.max_pkt = tx ? tx_batch_size : rx_batch_size;
    meta->burst.hdr.hdr.max_pkt_size = capacity;
    meta->state.store(State::APP, std::memory_order_release);
    return meta;
  }
  void put(Lease& meta) {
    meta.state.store(State::FREE, std::memory_order_release);
    if (!(meta.tx ? tx_indirect : rx_indirect)) {
      (meta.tx ? tx_direct_free : rx_direct_free).push_back(&meta);
    } else if (!(meta.tx ? tx_free : rx_free)->enqueue(&meta))
      throw std::logic_error("MGBE metadata pool overflow");
  }
  bool valid_tx(Lease* m) const {
    return m && m->tx && m->burst.hdr.hdr.port_id == 0 && m->burst.hdr.hdr.q_id == 0 &&
           m->burst.hdr.hdr.num_segs == 1 && m->burst.hdr.hdr.num_pkts >= 1 &&
           m->burst.hdr.hdr.num_pkts <= tx_batch_size;
  }
  uint8_t* writable(BurstParams* burst, int index, size_t bytes) {
    auto* m = find(burst);
    if (!valid_tx(m) || (!tx_indirect && reserved != m) || index < 0 || size_t(index) >= m->count ||
        !m->held[index] || bytes > capacity)
      return nullptr;
    return m->packets[index].data;
  }
  void fill_rx(Lease& m, size_t start) {
    m.burst.hdr.hdr.num_pkts = m.count;
    for (size_t i = start; i < m.count; ++i) {
      m.held[i] = true;
      m.pointers[i] = m.packets[i].data;
      m.lengths[i] = m.packets[i].length;
      m.burst.hdr.hdr.nbytes += m.lengths[i];
      if (rx_indirect) rx_slots[m.packets[i].slot].packet = m.packets[i];
    }
  }
  void poll_rx() {
    auto& q = port->queue();
    // Return objects are independent of burst metadata, which the application
    // can recycle before this worker rearms the hardware slots.
    std::array<mgbe::Packet, rx_batch> released;
    size_t n = 0;
    void* ptr = nullptr;
    while (n < released.size() && rx_returned->dequeue(&ptr))
      released[n++] = static_cast<RxReturn*>(ptr)->packet;
    if (n) q.release(released.data(), n);
    if (!rx_pending) rx_pending = take(false);
    if (!rx_pending) {
      // Still observe link/session faults when all metadata is application-owned.
      q.receive(nullptr, 0);
      return;
    }
    auto& m = *rx_pending;
    const size_t previous = m.count;
    m.count += q.receive(m.packets.data() + previous, rx_batch_size - previous);
    if (!previous && m.count) rx_since = std::chrono::steady_clock::now();
    fill_rx(m, previous);
    const auto& c = q.counters();
    if (rx_seen != c.rx_packets) {
      rx_seen = c.rx_packets;
      received.store(rx_seen, std::memory_order_relaxed);
    }
    if (rx_error_seen != c.rx_errors) {
      rx_error_seen = c.rx_errors;
      rx_errors.store(rx_error_seen, std::memory_order_relaxed);
    }
    if (m.count &&
        (m.count == rx_batch_size ||
         (rx_timeout.count() && std::chrono::steady_clock::now() - rx_since >= rx_timeout))) {
      if (!rx_ready->enqueue(&m)) throw std::logic_error("MGBE RX handoff overflow");
      rx_pending = nullptr;
    }
  }
  void poll_tx() {
    auto& q = port->queue();
    const size_t done = q.reclaim();
    std::array<void*, rx_batch> returned;
    for (size_t i = 0; i < done;) {
      const size_t n = std::min(returned.size(), done - i);
      for (size_t j = 0; j < n; ++j) returned[j] = inflight[cleaned++ % inflight.size()];
      if (!tx_free_slots->enqueue_bulk(returned.data(), n))
        throw std::logic_error("MGBE TX slot pool overflow");
      i += n;
    }
    if (done) completed.fetch_add(done, std::memory_order_release);
    // Bound each pass so a shared RX/TX worker services both directions fairly.
    size_t budget = rx_batch;
    while (budget--) {
      if (!tx_pending) {
        void* ptr = nullptr;
        if (!tx_ready->dequeue(&ptr)) break;
        tx_pending = static_cast<Lease*>(ptr);
        tx_offset = 0;
      }
      auto& m = *tx_pending;
      auto* slot = static_cast<TxSlot*>(m.slots[tx_offset]);
      if (!q.submit_buffer(slot->index, m.lengths[tx_offset], false)) break;
      inflight[posted++ % inflight.size()] = slot;
      if (++tx_offset == m.count) {
        put(m);  // Data slots stay owned until their DMA completions above.
        tx_pending = nullptr;
      }
    }
    q.flush_tx();  // One doorbell for all descriptors posted in this pass.
  }
  void start_worker(int cpu, bool rx, bool tx) {
    std::promise<void> ready;
    auto future = ready.get_future();
    workers.emplace_back([this, cpu, rx, tx, ready = std::move(ready)]() mutable {
      cpu_set_t set;
      CPU_ZERO(&set);
      CPU_SET(cpu, &set);
      const int error = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
      if (error) {
        ready.set_exception(std::make_exception_ptr(
            std::system_error(error, std::generic_category(), "pin MGBE worker")));
        return;
      }
      ready.set_value();
      try {
        while (!stop.load(std::memory_order_acquire) && !fault.load(std::memory_order_acquire)) {
          if (rx) poll_rx();
          if (tx) poll_tx();
        }
      } catch (const std::exception& error) {
        fault.store(true, std::memory_order_release);
        DAQIRI_LOG_ERROR("MGBE worker fault; restart session: {}", error.what());
      }
    });
    future.get();
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
  const auto& rx = cfg_.ifs_[0].rx_.queues_[0];
  const auto& tx = cfg_.ifs_[0].tx_.queues_[0];
  s.rx_indirect = rx.poll_mode_ == QueuePollMode::INDIRECT;
  s.tx_indirect = tx.poll_mode_ == QueuePollMode::INDIRECT;
  s.rx_batch_size = s.rx_indirect ? rx.common_.batch_size_ : rx_batch;
  s.tx_batch_size = s.tx_indirect ? tx.common_.batch_size_ : 1;
  s.rx_timeout = std::chrono::nanoseconds(rx.timeout_us_ * 1000);
  s.rx_direct_free.reserve(cfg_.rx_meta_buffers_);
  s.tx_direct_free.reserve(cfg_.tx_meta_buffers_);
  if (s.rx_indirect) {
    s.rx_free = Impl::ring(cfg_.rx_meta_buffers_);
    s.rx_ready = Impl::ring(cfg_.rx_meta_buffers_);
    s.rx_returned = Impl::ring(mr.num_bufs_);
    s.rx_slots.resize(mr.num_bufs_);
  }
  if (s.tx_indirect) {
    s.tx_free = Impl::ring(cfg_.tx_meta_buffers_);
    s.tx_ready = Impl::ring(cfg_.tx_meta_buffers_);
    s.tx_free_slots = Impl::ring(mr.num_bufs_);
    s.tx_slots.resize(mr.num_bufs_);
    s.inflight.resize(mr.num_bufs_);
    for (uint32_t i = 0; i < mr.num_bufs_; ++i) {
      s.tx_slots[i] = {i, s.port->queue().tx_buffer_at(i)};
      s.tx_free_slots->enqueue(&s.tx_slots[i]);
    }
  }
  for (size_t i = 0; i < total; ++i) {
    auto meta = std::make_unique<Impl::Lease>();
    meta->tx = i >= cfg_.rx_meta_buffers_;
    s.put(*meta);
    s.leases.emplace(&meta->burst, meta.get());
    s.storage.push_back(std::move(meta));
  }
  s.port->start();
  initialized_ = true;
  run();
}
void MgbeEngine::run() {
  auto& s = *impl_;
  if (!initialized_ || !s.workers.empty()) return;
  const auto& port = cfg_.ifs_[0];
  try {
    const int rx_cpu = s.rx_indirect ? std::stoi(port.rx_.queues_[0].common_.cpu_core_) : -1;
    const int tx_cpu = s.tx_indirect ? std::stoi(port.tx_.queues_[0].common_.cpu_core_) : -1;
    if (rx_cpu >= 0) s.start_worker(rx_cpu, true, tx_cpu == rx_cpu);
    if (tx_cpu >= 0 && tx_cpu != rx_cpu) s.start_worker(tx_cpu, false, true);
  } catch (...) {
    shutdown();
    throw;
  }
}
void MgbeEngine::shutdown() {
  initialized_ = false;
  auto& s = *impl_;
  s.stop.store(true, std::memory_order_release);
  for (auto& worker : s.workers)
    if (worker.joinable()) worker.join();
  s.workers.clear();
  // Join pollers, then stop DMA before destroying handoffs or unmapping pools.
  s.port.reset();
  s.reserved = s.rx_pending = s.tx_pending = nullptr;
  s.leases.clear();
  s.storage.clear();
  s.rx_direct_free.clear();
  s.tx_direct_free.clear();
  s.rx_free.reset();
  s.tx_free.reset();
  s.rx_ready.reset();
  s.tx_ready.reset();
  s.rx_returned.reset();
  s.tx_free_slots.reset();
  s.tx_slots.clear();
  s.rx_slots.clear();
  s.inflight.clear();
  s.posted = s.cleaned = 0;
  s.submitted = 0;
  s.completed = 0;
  s.received = 0;
  s.rx_errors = 0;
  s.rx_seen = s.rx_error_seen = 0;
  s.reservations = 0;
  s.owner = 0;
  s.fault = false;
  s.stop = false;
}
void MgbeEngine::print_stats() {
  if (!initialized_) return;
  auto& s = *impl_;
  DAQIRI_LOG_INFO("MGBE RX packets={} errors={}, completed TX packets={}", s.received.load(),
                  s.rx_errors.load(), s.completed.load());
}
BurstParams* MgbeEngine::create_tx_burst_params() {
  if (!initialized_ || impl_->fault.load(std::memory_order_acquire) || !impl_->claim(true))
    return nullptr;
  auto* meta = impl_->take(true);
  return meta ? &meta->burst : nullptr;
}
Status MgbeEngine::get_rx_burst(BurstParams** burst, int port, int queue) {
  if (!burst) return Status::NULL_PTR;
  *burst = nullptr;
  if (!initialized_) return Status::NOT_READY;
  auto& s = *impl_;
  if (port != 0 || queue != 0 || !s.claim(false)) return Status::INVALID_PARAMETER;
  if (s.fault.load(std::memory_order_acquire)) return Status::INTERNAL_ERROR;
  if (s.rx_indirect) {
    void* ptr = nullptr;
    if (!s.rx_ready->dequeue(&ptr)) return Status::NOT_READY;
    *burst = &static_cast<Impl::Lease*>(ptr)->burst;
    return Status::SUCCESS;
  }
  auto* m = s.take(false);
  if (!m) return Status::NO_FREE_BURST_BUFFERS;
  try {
    m->count = s.port->queue().receive(m->packets.data(), rx_batch);
    if (!m->count) {
      s.put(*m);
      return Status::NOT_READY;
    }
    s.fill_rx(*m, 0);
    s.received.store(s.port->queue().counters().rx_packets, std::memory_order_relaxed);
    s.rx_errors.store(s.port->queue().counters().rx_errors, std::memory_order_relaxed);
    *burst = &m->burst;
    return Status::SUCCESS;
  } catch (const std::exception& error) {
    DAQIRI_LOG_ERROR("MGBE receive failed: {}", error.what());
    s.put(*m);
    return Status::INTERNAL_ERROR;
  }
}
bool MgbeEngine::is_tx_burst_available(BurstParams* burst) {
  if (!initialized_ || impl_->fault.load(std::memory_order_acquire)) return false;
  auto& s = *impl_;
  auto* m = s.find(burst);
  if (!s.valid_tx(m) || m->count) return false;
  if (s.tx_indirect) return s.tx_free_slots->count() >= m->burst.hdr.hdr.num_pkts;
  if (s.reserved) return false;
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
  if (s.fault.load(std::memory_order_acquire)) return Status::INTERNAL_ERROR;
  auto* m = s.find(burst);
  if (!s.valid_tx(m)) return Status::INVALID_PARAMETER;
  if (m->count || (!s.tx_indirect && s.reserved)) return Status::RESOURCE_IN_USE;
  try {
    const size_t n = m->burst.hdr.hdr.num_pkts;
    if (s.tx_indirect) {
      if (!s.tx_free_slots->dequeue_bulk(m->slots.data(), n)) return Status::NO_FREE_PACKET_BUFFERS;
      for (size_t i = 0; i < n; ++i) m->pointers[i] = static_cast<Impl::TxSlot*>(m->slots[i])->data;
    } else {
      m->pointers[0] = s.port->queue().tx_buffer();
      if (!m->pointers[0]) return Status::NO_FREE_PACKET_BUFFERS;
      s.reserved = m;
    }
    m->count = n;
    for (size_t i = 0; i < n; ++i) {
      m->held[i] = true;
      m->packets[i].data = static_cast<uint8_t*>(m->pointers[i]);
    }
    s.reservations.fetch_add(n, std::memory_order_relaxed);
    return Status::SUCCESS;
  } catch (...) {
    return Status::INTERNAL_ERROR;
  }
}
Status MgbeEngine::send_tx_burst(BurstParams* burst) {
  if (!burst) return Status::NULL_PTR;
  if (!initialized_) return Status::NOT_READY;
  auto& s = *impl_;
  if (s.fault.load(std::memory_order_acquire)) return Status::INTERNAL_ERROR;
  auto* m = s.find(burst);
  if (!s.valid_tx(m) || m->count != m->burst.hdr.hdr.num_pkts ||
      (!s.tx_indirect && s.reserved != m))
    return Status::INVALID_PARAMETER;
  for (size_t i = 0; i < m->count; ++i) {
    if (!m->held[i] || m->pointers[i] != m->packets[i].data || m->lengths[i] < 14 ||
        m->lengths[i] > s.capacity)
      return Status::INVALID_PARAMETER;
  }
  try {
    if (s.tx_indirect) {
      const size_t n = m->count;
      m->state.store(Impl::State::WORKER, std::memory_order_release);
      // Capacity covers every metadata lease, so a correctly owned lease always fits.
      if (!s.tx_ready->enqueue(m)) {
        m->state.store(Impl::State::APP, std::memory_order_release);
        return Status::NO_FREE_BURST_BUFFERS;
      }
      s.reservations.fetch_sub(n, std::memory_order_relaxed);
      s.submitted.fetch_add(n, std::memory_order_release);
    } else {
      s.port->queue().submit(m->lengths[0]);
      s.submitted.fetch_add(1, std::memory_order_release);
      s.reservations.fetch_sub(1, std::memory_order_relaxed);
      s.reserved = nullptr;
      s.put(*m);
    }
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
  auto* m = impl_->find(b);
  b->hdr.hdr.nbytes = b->hdr.hdr.nbytes - m->lengths[i] + *lens.begin();
  m->lengths[i] = *lens.begin();
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
  if (!m || i < 0 || size_t(i) >= m->count || !m->held[i]) return;
  try {
    if (m->tx) {
      if (s.tx_indirect) {
        if (!s.tx_free_slots->enqueue(m->slots[i])) throw std::logic_error("TX release overflow");
      } else if (s.reserved == m) {
        s.port->queue().cancel_tx();
        s.reserved = nullptr;
      }
      s.reservations.fetch_sub(1, std::memory_order_relaxed);
    } else if (s.rx_indirect) {
      if (!s.rx_returned->enqueue(&s.rx_slots[m->packets[i].slot]))
        throw std::logic_error("RX release overflow");
    } else {
      s.port->queue().release(m->packets[i]);
    }
  } catch (...) {
    s.fault.store(true, std::memory_order_release);  // Reclaim the session at shutdown.
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
  if (!m || m->tx) return;
  free_all_packets(b);
  impl_->put(*m);
}
void MgbeEngine::free_tx_burst(BurstParams* b) {
  auto* m = impl_->find(b);
  if (!m || !m->tx) return;
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
  if (!impl_->claim(true)) return Status::INVALID_PARAMETER;
  if (impl_->reservations.load()) return Status::RESOURCE_IN_USE;
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  try {
    do {
      if (impl_->fault.load(std::memory_order_acquire)) return Status::INTERNAL_ERROR;
      if (!impl_->tx_indirect) {
        impl_->port->queue().reclaim();
        impl_->completed.store(impl_->port->queue().counters().tx_packets,
                               std::memory_order_release);
      }
      if (impl_->completed.load(std::memory_order_acquire) ==
          impl_->submitted.load(std::memory_order_acquire))
        return Status::SUCCESS;
      if (impl_->tx_indirect) std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < end);
  } catch (...) {
    return Status::INTERNAL_ERROR;
  }
  return Status::NOT_READY;
}
}  // namespace daqiri
