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
#include <map>
#include <set>

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
  DevicePort(const std::string& interface, uint32_t ring, uint32_t mtu, uint32_t queues)
      : device_("/dev/mgbe-pmd-" + interface, ring, mtu, queues) {}
  mgbe::Queue& queue(uint32_t id = 0) override {
    return device_.queue(id);
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
      config.common_.loopback_ != LoopbackType::DISABLED || config.ifs_.empty() ||
      config.ifs_.size() > 4)
    return reject("requires raw Ethernet, 1..4 interfaces and no internal loopback");
  if (!config.rx_meta_buffers_ || config.rx_meta_buffers_ > 4096 || !config.tx_meta_buffers_ ||
      config.tx_meta_buffers_ > 4096)
    return reject("metadata pool sizes must be 1..4096");
  std::set<std::string> interfaces, regions;
  for (const auto& port : config.ifs_) {
    if (port.address_.size() != 7 || port.address_.compare(0, 4, "mgbe") ||
        port.address_[4] < '0' || port.address_[4] > '3' || port.address_.substr(5) != "_0" ||
        !interfaces.insert(port.address_).second)
      return reject("requires unique MGBE netdevs, mgbe0_0 through mgbe3_0");
    const size_t n = port.rx_.queues_.size();
    if (!n || n > MGBE_PMD_MAX_QUEUES || port.tx_.queues_.size() != n || !port.rx_.flows_.empty() ||
        !port.tx_.flows_.empty() || !port.rx_.flex_items_.empty() ||
        !port.rx_.reorder_configs_.empty() || port.rx_.flow_isolation_ ||
        port.rx_.hardware_timestamps_ || port.tx_.accurate_send_)
      return reject(
          "requires 1..4 RX/TX pairs per port; flow rules, offloads and timestamps are "
          "unsupported");
    size_t ring = 0, capacity = 0;
    auto check = [&](const CommonQueueConfig& q, QueuePollMode mode, std::set<int>& ids) {
      if (q.id_ < 0 || size_t(q.id_) >= n || !ids.insert(q.id_).second || q.mrs_.size() != 1 ||
          !q.offloads_.empty() || q.split_boundary_ || q.extra_queue_config_)
        return false;
      if (mode == QueuePollMode::INDIRECT) {
        if (q.batch_size_ < 1 || q.batch_size_ > int(rx_batch) || q.cpu_core_.empty() ||
            q.cpu_core_.find_first_not_of("0123456789") != std::string::npos)
          return false;
        try {
          if (std::stoul(q.cpu_core_) >= CPU_SETSIZE) return false;
        } catch (...) {
          return false;
        }
      } else if (mode != QueuePollMode::DIRECT)
        return false;
      const auto found = config.mrs_.find(q.mrs_[0]);
      if (found == config.mrs_.end() || !regions.insert(q.mrs_[0]).second) return false;
      const auto& mr = found->second;
      if (mr.kind_ != MemoryKind::HOST || !mr.owned_ ||
          (mr.access_ & ~uint32_t(MEM_ACCESS_LOCAL)) || mr.buf_size_ < 82 || mr.buf_size_ > 9014 ||
          mr.num_bufs_ < 64 || mr.num_bufs_ > MGBE_PMD_MAX_RING ||
          (mr.num_bufs_ & (mr.num_bufs_ - 1)))
        return false;
      if (!ring) {
        ring = mr.num_bufs_;
        capacity = mr.buf_size_;
      }
      return ring == mr.num_bufs_ && capacity == mr.buf_size_;
    };
    std::set<int> rx_ids, tx_ids;
    for (const auto& q : port.rx_.queues_)
      if (!check(q.common_, q.poll_mode_, rx_ids) || q.cqe_compression_ ||
          q.timeout_us_ > uint64_t(INT64_MAX) / 1000)
        return reject(
            "invalid RX queues: unique IDs 0..N-1, distinct owned host pools, equal port "
            "ring/frame sizes");
    for (const auto& q : port.tx_.queues_)
      if (!check(q.common_, q.poll_mode_, tx_ids) || q.pacing_mbps_ ||
          q.buffer_mode_ != TxBufferMode::CYCLIC || q.max_inflight_sends_)
        return reject(
            "invalid TX queues: IDs/pools must match port shape; only cyclic unscheduled TX is "
            "supported");
  }
  if (regions.size() != config.mrs_.size()) return reject("unused memory regions are unsupported");
  return true;
}

struct MgbeEngine::Impl {
  struct Channel;
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
    Channel* channel = nullptr;
  };
  using RingPtr = std::unique_ptr<Ring, decltype(&Ring::free)>;
  static RingPtr ring(size_t capacity) {
    RingPtr result(Ring::create("mgbe", capacity + 1, RingMode::MPMC), &Ring::free);
    if (!result) throw std::bad_alloc();
    return result;
  }
  std::atomic<bool> stop{false}, fault{false};
  struct Channel {
    RingPtr rx_free{nullptr, &Ring::free};
    RingPtr rx_ready{nullptr, &Ring::free}, rx_returned{nullptr, &Ring::free};
    RingPtr tx_ready{nullptr, &Ring::free}, tx_free_slots{nullptr, &Ring::free};
    Impl& engine;
    std::atomic<bool>& fault;
    MgbePort* port = nullptr;
    uint32_t port_id = 0, queue_id = 0;
    Channel(Impl& owner) : engine(owner), fault(owner.fault) {}
    struct TxGuard {
      Channel& channel;
      bool locked;
      TxGuard(Channel& q, bool wait = false) : channel(q), locked(q.tx_indirect) {
        if (!q.tx_indirect) {
          do {
            locked = !q.tx_guard.test_and_set(std::memory_order_acquire);
            if (!locked && wait) std::this_thread::yield();
          } while (!locked && wait);
        }
      }
      ~TxGuard() {
        if (locked && !channel.tx_indirect) channel.tx_guard.clear(std::memory_order_release);
      }
      explicit operator bool() const {
        return locked;
      }
    };
    std::atomic_flag tx_guard = ATOMIC_FLAG_INIT;
    std::vector<Lease*> rx_direct_free;
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
    std::array<std::atomic<uint64_t>, 2> owners{};
    bool claim(bool tx) {
      if (tx ? tx_indirect : rx_indirect) return true;
      auto& owner = owners[tx ? 1 : 0];
      const uint64_t token = thread_token();
      uint64_t current = owner.load(std::memory_order_relaxed);
      return current == token || (current == 0 && owner.compare_exchange_strong(
                                                      current, token, std::memory_order_relaxed));
    }
    Lease* take(bool tx) {
      if (tx) return engine.take_tx();
      void* ptr = nullptr;
      if (rx_indirect) {
        if (!rx_free->dequeue(&ptr)) return nullptr;
      } else {
        if (rx_direct_free.empty()) return nullptr;
        ptr = rx_direct_free.back();
        rx_direct_free.pop_back();
      }
      auto* m = static_cast<Lease*>(ptr);
      engine.reset(*m, rx_batch_size, capacity);
      m->channel = this;
      m->burst.hdr.hdr.port_id = port_id;
      m->burst.hdr.hdr.q_id = queue_id;
      return m;
    }
    void put(Lease& m) {
      if (m.tx) {
        engine.put_tx(m);
        return;
      }
      m.state.store(State::FREE, std::memory_order_release);
      if (!rx_indirect)
        rx_direct_free.push_back(&m);
      else if (!rx_free->enqueue(&m))
        throw std::logic_error("MGBE RX metadata overflow");
    }
    bool valid_tx(Lease* m) const {
      return m && m->tx && m->burst.hdr.hdr.port_id == this->port_id &&
             m->burst.hdr.hdr.q_id == queue_id && m->burst.hdr.hdr.num_segs == 1 &&
             m->burst.hdr.hdr.num_pkts >= 1 && m->burst.hdr.hdr.num_pkts <= tx_batch_size;
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
      auto& q = port->queue(queue_id);
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
      auto& q = port->queue(queue_id);
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
  };
  std::vector<std::unique_ptr<MgbePort>> ports;
  std::vector<std::unique_ptr<Channel>> channels;
  std::array<std::array<Channel*, MGBE_PMD_MAX_QUEUES>, 4> table{};
  std::vector<std::unique_ptr<Lease>> storage;
  std::unordered_map<BurstParams*, Lease*> leases;
  RingPtr tx_free{nullptr, &Ring::free};
  std::vector<Lease*> tx_direct_free;
  std::vector<std::thread> workers;
  Channel* single_direct = nullptr;
  uint32_t max_tx_batch = 1, max_capacity = 0;
  void reset(Lease& m, uint32_t batch, uint32_t capacity) {
    m.burst = {};
    m.pointers.fill(nullptr);
    m.lengths.fill(0);
    m.held.fill(false);
    m.count = 0;
    m.channel = nullptr;
    m.burst.pkts[0] = m.pointers.data();
    m.burst.pkt_lens[0] = m.lengths.data();
    m.burst.hdr.hdr.num_segs = 1;
    m.burst.hdr.hdr.max_pkt = batch;
    m.burst.hdr.hdr.max_pkt_size = capacity;
    m.state.store(State::APP, std::memory_order_release);
  }
  Lease* find(BurstParams* burst) {
    if (!burst) return nullptr;
    const auto it = std::as_const(leases).find(burst);
    if (it == leases.end()) return nullptr;
    auto* m = it->second;
    if (m->state.load(std::memory_order_acquire) != State::APP) return nullptr;
    if (m->channel) return m->channel->claim(m->tx) ? m : nullptr;
    return !single_direct || single_direct->claim(true) ? m : nullptr;
  }
  Channel* lookup(int port, int queue) {
    if (port < 0 || port >= 4 || queue < 0 || queue >= int(MGBE_PMD_MAX_QUEUES)) return nullptr;
    return table[port][queue];
  }
  Channel* target(Lease* m) {
    if (!m || !m->tx) return nullptr;
    auto* q = lookup(m->burst.hdr.hdr.port_id, m->burst.hdr.hdr.q_id);
    if (!q || (m->channel && m->channel != q) || !q->valid_tx(m) || !q->claim(true)) return nullptr;
    return q;
  }
  Lease* take_tx() {
    void* ptr = nullptr;
    if (single_direct) {
      if (!single_direct->claim(true) || tx_direct_free.empty()) return nullptr;
      ptr = tx_direct_free.back();
      tx_direct_free.pop_back();
    } else if (!tx_free->dequeue(&ptr))
      return nullptr;
    auto* m = static_cast<Lease*>(ptr);
    reset(*m, max_tx_batch, max_capacity);
    return m;
  }
  void put_tx(Lease& m) {
    m.state.store(State::FREE, std::memory_order_release);
    if (single_direct)
      tx_direct_free.push_back(&m);
    else if (!tx_free->enqueue(&m))
      throw std::logic_error("MGBE TX metadata overflow");
  }
  uint8_t* writable(BurstParams* burst, int index, size_t bytes) {
    auto* m = find(burst);
    auto* q = m ? m->channel : nullptr;
    if (!q || !q->valid_tx(m) || (!q->tx_indirect && q->reserved != m) || index < 0 ||
        size_t(index) >= m->count || !m->held[index] || bytes > q->capacity)
      return nullptr;
    return m->packets[index].data;
  }
  struct Work {
    Channel* channel;
    bool rx, tx;
  };
  void start_worker(int cpu, std::vector<Work> work) {
    std::promise<void> ready;
    auto future = ready.get_future();
    workers.emplace_back([this, cpu, work = std::move(work), ready = std::move(ready)]() mutable {
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
        while (!stop.load(std::memory_order_acquire) && !fault.load(std::memory_order_acquire))
          for (const auto& item : work) {
            if (item.rx) item.channel->poll_rx();
            if (item.tx) item.channel->poll_tx();
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
                                                uint32_t mtu, uint32_t queues) {
  return std::make_unique<DevicePort>(interface, ring, mtu, queues);
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
  size_t count = 0;
  for (const auto& port : cfg_.ifs_) count += port.rx_.queues_.size();
  const size_t metadata = count * cfg_.rx_meta_buffers_ + cfg_.tx_meta_buffers_;
  s.storage.reserve(metadata);
  s.leases.reserve(metadata);
  s.channels.reserve(count);
  for (size_t port_id = 0; port_id < cfg_.ifs_.size(); ++port_id) {
    auto& port = cfg_.ifs_[port_id];
    port.port_id_ = port_id;
    const auto& mr = cfg_.mrs_.at(port.rx_.queues_[0].common_.mrs_[0]);
    const size_t n = port.rx_.queues_.size();
    s.ports.push_back(open_port(port.address_, mr.num_bufs_, mr.buf_size_ - 14, n));
    for (size_t id = 0; id < n; ++id) {
      const auto& rx = *std::find_if(port.rx_.queues_.begin(), port.rx_.queues_.end(),
                                     [&](const auto& q) { return q.common_.id_ == int(id); });
      const auto& tx = *std::find_if(port.tx_.queues_.begin(), port.tx_.queues_.end(),
                                     [&](const auto& q) { return q.common_.id_ == int(id); });
      auto channel = std::make_unique<Impl::Channel>(s);
      auto& q = *channel;
      q.port = s.ports.back().get();
      q.port_id = port_id;
      q.queue_id = id;
      q.capacity = mr.buf_size_;
      q.rx_indirect = rx.poll_mode_ == QueuePollMode::INDIRECT;
      q.tx_indirect = tx.poll_mode_ == QueuePollMode::INDIRECT;
      q.rx_batch_size = q.rx_indirect ? rx.common_.batch_size_ : rx_batch;
      q.tx_batch_size = q.tx_indirect ? tx.common_.batch_size_ : 1;
      q.rx_timeout = std::chrono::nanoseconds(rx.timeout_us_ * 1000);
      s.max_capacity = std::max(s.max_capacity, q.capacity);
      s.max_tx_batch = std::max(s.max_tx_batch, q.tx_batch_size);
      q.rx_direct_free.reserve(cfg_.rx_meta_buffers_);
      if (q.rx_indirect) {
        q.rx_free = Impl::ring(cfg_.rx_meta_buffers_);
        q.rx_ready = Impl::ring(cfg_.rx_meta_buffers_);
        q.rx_returned = Impl::ring(mr.num_bufs_);
        q.rx_slots.resize(mr.num_bufs_);
      }
      if (q.tx_indirect) {
        q.tx_ready = Impl::ring(cfg_.tx_meta_buffers_);
        q.tx_free_slots = Impl::ring(mr.num_bufs_);
        q.tx_slots.resize(mr.num_bufs_);
        q.inflight.resize(mr.num_bufs_);
        for (uint32_t i = 0; i < mr.num_bufs_; ++i) {
          q.tx_slots[i] = {i, q.port->queue(id).tx_buffer_at(i)};
          q.tx_free_slots->enqueue(&q.tx_slots[i]);
        }
      }
      s.table[port_id][id] = &q;
      s.channels.push_back(std::move(channel));
      for (size_t i = 0; i < cfg_.rx_meta_buffers_; ++i) {
        auto meta = std::make_unique<Impl::Lease>();
        meta->channel = &q;
        q.put(*meta);
        s.leases.emplace(&meta->burst, meta.get());
        s.storage.push_back(std::move(meta));
      }
    }
  }
  s.single_direct = count == 1 && !s.channels[0]->tx_indirect ? s.channels[0].get() : nullptr;
  if (s.single_direct)
    s.tx_direct_free.reserve(cfg_.tx_meta_buffers_);
  else
    s.tx_free = Impl::ring(cfg_.tx_meta_buffers_);
  for (size_t i = 0; i < cfg_.tx_meta_buffers_; ++i) {
    auto meta = std::make_unique<Impl::Lease>();
    meta->tx = true;
    s.put_tx(*meta);
    s.leases.emplace(&meta->burst, meta.get());
    s.storage.push_back(std::move(meta));
  }
  for (auto& port : s.ports) port->start();
  initialized_ = true;
  run();
}
void MgbeEngine::run() {
  auto& s = *impl_;
  if (!initialized_ || !s.workers.empty()) return;
  std::map<int, std::vector<Impl::Work>> groups;
  for (auto& q : s.channels) {
    const auto& port = cfg_.ifs_[q->port_id];
    const auto& rx =
        *std::find_if(port.rx_.queues_.begin(), port.rx_.queues_.end(),
                      [&](const auto& v) { return v.common_.id_ == int(q->queue_id); });
    const auto& tx =
        *std::find_if(port.tx_.queues_.begin(), port.tx_.queues_.end(),
                      [&](const auto& v) { return v.common_.id_ == int(q->queue_id); });
    const int rx_cpu = q->rx_indirect ? std::stoi(rx.common_.cpu_core_) : -1;
    const int tx_cpu = q->tx_indirect ? std::stoi(tx.common_.cpu_core_) : -1;
    if (rx_cpu >= 0) groups[rx_cpu].push_back({q.get(), true, tx_cpu == rx_cpu});
    if (tx_cpu >= 0 && tx_cpu != rx_cpu) groups[tx_cpu].push_back({q.get(), false, true});
  }
  try {
    for (auto& [cpu, work] : groups) s.start_worker(cpu, std::move(work));
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
  // All pollers stop before any port stops DMA and unmaps its memory.
  s.ports.clear();
  s.channels.clear();
  s.table = {};
  s.leases.clear();
  s.storage.clear();
  s.tx_free.reset();
  s.tx_direct_free.clear();
  s.single_direct = nullptr;
  s.max_tx_batch = 1;
  s.max_capacity = 0;
  s.stop = false;
  s.fault = false;
}
void MgbeEngine::print_stats() {
  if (!initialized_) return;
  for (const auto& q : impl_->channels)
    DAQIRI_LOG_INFO("MGBE port={} queue={} RX packets={} errors={}, completed TX packets={}",
                    q->port_id, q->queue_id, q->received.load(), q->rx_errors.load(),
                    q->completed.load());
}
BurstParams* MgbeEngine::create_tx_burst_params() {
  if (!initialized_ || impl_->fault.load(std::memory_order_acquire)) return nullptr;
  auto* meta = impl_->take_tx();
  return meta ? &meta->burst : nullptr;
}
Status MgbeEngine::get_rx_burst(BurstParams** burst, int port, int queue) {
  if (!burst) return Status::NULL_PTR;
  *burst = nullptr;
  if (!initialized_) return Status::NOT_READY;
  auto* selected = impl_->lookup(port, queue);
  if (!selected || !selected->claim(false)) return Status::INVALID_PARAMETER;
  auto& s = *selected;
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
    m->count = s.port->queue(s.queue_id).receive(m->packets.data(), rx_batch);
    if (!m->count) {
      s.put(*m);
      return Status::NOT_READY;
    }
    s.fill_rx(*m, 0);
    s.received.store(s.port->queue(s.queue_id).counters().rx_packets, std::memory_order_relaxed);
    s.rx_errors.store(s.port->queue(s.queue_id).counters().rx_errors, std::memory_order_relaxed);
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
  auto* m = impl_->find(burst);
  auto* selected = impl_->target(m);
  if (!selected || m->count) return false;
  auto& s = *selected;
  Impl::Channel::TxGuard guard(s);
  if (!guard) return false;
  if (s.tx_indirect) return s.tx_free_slots->count() >= m->burst.hdr.hdr.num_pkts;
  if (s.reserved) return false;
  try {
    const bool available = s.port->queue(s.queue_id).tx_buffer() != nullptr;
    if (available) s.port->queue(s.queue_id).cancel_tx();
    return available;
  } catch (...) {
    return false;
  }
}
Status MgbeEngine::get_tx_packet_burst(BurstParams* burst) {
  if (!burst) return Status::NULL_PTR;
  if (!initialized_) return Status::NOT_READY;
  if (impl_->fault.load(std::memory_order_acquire)) return Status::INTERNAL_ERROR;
  auto* m = impl_->find(burst);
  auto* selected = impl_->target(m);
  if (!selected) return Status::INVALID_PARAMETER;
  auto& s = *selected;
  Impl::Channel::TxGuard guard(s);
  if (!guard) return Status::NOT_READY;
  if (m->count || (!s.tx_indirect && s.reserved)) return Status::RESOURCE_IN_USE;
  try {
    const size_t n = m->burst.hdr.hdr.num_pkts;
    if (s.tx_indirect) {
      if (!s.tx_free_slots->dequeue_bulk(m->slots.data(), n)) return Status::NO_FREE_PACKET_BUFFERS;
      for (size_t i = 0; i < n; ++i) m->pointers[i] = static_cast<Impl::TxSlot*>(m->slots[i])->data;
    } else {
      m->pointers[0] = s.port->queue(s.queue_id).tx_buffer();
      if (!m->pointers[0]) return Status::NO_FREE_PACKET_BUFFERS;
      s.reserved = m;
    }
    m->channel = &s;
    m->burst.hdr.hdr.max_pkt = s.tx_batch_size;
    m->burst.hdr.hdr.max_pkt_size = s.capacity;
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
  if (impl_->fault.load(std::memory_order_acquire)) return Status::INTERNAL_ERROR;
  auto* m = impl_->find(burst);
  auto* selected = impl_->target(m);
  if (!selected || m->channel != selected) return Status::INVALID_PARAMETER;
  auto& s = *selected;
  Impl::Channel::TxGuard guard(s);
  if (!guard) return Status::NOT_READY;
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
      s.port->queue(s.queue_id).submit(m->lengths[0]);
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
  std::memcpy(p->eth.h_source,
              impl_->find(b)->channel->port->queue(impl_->find(b)->channel->queue_id).info().mac,
              6);
  p->eth.h_proto = htons(ETH_P_IP);
  return Status::SUCCESS;
}
Status MgbeEngine::set_ipv4_header(BurstParams* b, int i, int length, uint8_t protocol,
                                   unsigned int source, unsigned int destination) {
  auto* p = reinterpret_cast<UDPIPV4Pkt*>(impl_->writable(b, i, sizeof(UDPIPV4Pkt)));
  if (!p || length < int(sizeof(iphdr)) ||
      size_t(length) + sizeof(ethhdr) > impl_->find(b)->channel->capacity)
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
  if (!p || length < int(sizeof(udphdr)) || size_t(length) + 34 > impl_->find(b)->channel->capacity)
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
  auto* m = impl_->find(b);
  if (!m || !m->channel || i < 0 || size_t(i) >= m->count || !m->held[i]) return;
  auto& s = *m->channel;
  try {
    if (m->tx) {
      Impl::Channel::TxGuard guard(s, true);
      if (s.tx_indirect) {
        if (!s.tx_free_slots->enqueue(m->slots[i])) throw std::logic_error("TX release overflow");
      } else if (s.reserved == m) {
        s.port->queue(s.queue_id).cancel_tx();
        s.reserved = nullptr;
      }
      s.reservations.fetch_sub(1, std::memory_order_relaxed);
    } else if (s.rx_indirect) {
      if (!s.rx_returned->enqueue(&s.rx_slots[m->packets[i].slot]))
        throw std::logic_error("RX release overflow");
    } else {
      s.port->queue(s.queue_id).release(m->packets[i]);
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
  m->channel->put(*m);
}
void MgbeEngine::free_tx_burst(BurstParams* b) {
  auto* m = impl_->find(b);
  if (!m || !m->tx) return;
  free_all_packets(b);
  impl_->put_tx(*m);
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
  auto* q = impl_->lookup(port, 0);
  if (!initialized_ || !q) return Status::INVALID_PARAMETER;
  std::memcpy(mac, q->port->queue(0).info().mac, 6);
  return Status::SUCCESS;
}
Status MgbeEngine::wait_for_tx_idle(uint32_t timeout_ms) {
  if (!initialized_) return Status::NOT_READY;
  for (auto& q : impl_->channels)
    if (q->reservations.load()) return Status::RESOURCE_IN_USE;
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  try {
    do {
      if (impl_->fault.load(std::memory_order_acquire)) return Status::INTERNAL_ERROR;
      bool idle = true;
      for (auto& q : impl_->channels) {
        if (!q->tx_indirect) {
          Impl::Channel::TxGuard guard(*q);
          if (!guard) {
            idle = false;
            continue;
          }
          q->port->queue(q->queue_id).reclaim();
          q->completed.store(q->port->queue(q->queue_id).counters().tx_packets,
                             std::memory_order_release);
        }
        if (q->completed.load(std::memory_order_acquire) !=
            q->submitted.load(std::memory_order_acquire))
          idle = false;
      }
      if (idle) return Status::SUCCESS;
      std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < end);
  } catch (...) {
    return Status::INTERNAL_ERROR;
  }
  return Status::NOT_READY;
}
}  // namespace daqiri
