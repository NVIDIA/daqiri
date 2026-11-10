// SPDX-License-Identifier: Apache-2.0
#include "mgbe_pmd.hpp"
#include <endian.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace mgbe {
namespace {
// Bit definitions follow NVIDIA's MIT-licensed nvethernetrm hw_desc.h.
constexpr uint32_t own = 1U << 31, context = 1U << 30;
constexpr uint32_t first = 1U << 29, last = 1U << 28;
constexpr uint32_t rx_error = 1U << 15, rx_context_available = 1U << 27;
uint32_t load(const uint32_t* p) {
  return le32toh(__atomic_load_n(p, __ATOMIC_RELAXED));
}
void store(uint32_t* p, uint32_t v) {
  __atomic_store_n(p, htole32(v), __ATOMIC_RELAXED);
}
void dma_read_barrier() {
#if defined(__aarch64__)
  asm volatile("dmb oshld" ::: "memory");
#else
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}
void dma_write_barrier() {
#if defined(__aarch64__)
  asm volatile("dmb oshst" ::: "memory");
#else
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}
void doorbell(volatile uint32_t* regs, uint32_t offset, uint64_t address) {
  dma_write_barrier();
  regs[offset / 4] = htole32(static_cast<uint32_t>(address));
  // Order Device-memory writes between bursts. Kernel teardown performs a
  // register readback and reset before it releases DMA mappings.
  dma_write_barrier();
}
[[noreturn]] void sysfail(const char* operation) {
  throw std::system_error(errno, std::generic_category(), operation);
}
}  // namespace

Queue::Queue(Mapping mapping) : m_(mapping), mask_(mapping.info.ring_size - 1) {
  const auto& i = m_.info;
  if (i.abi != MGBE_PMD_ABI || i.ring_size < 64 || i.ring_size > MGBE_PMD_MAX_RING ||
      (i.ring_size & (i.ring_size - 1)) || (i.slot_size != 2048 && i.slot_size != 16384) ||
      i.mtu < 68 || i.mtu > 9000 || i.mtu + 18 > i.slot_size || !m_.rx || !m_.tx || !m_.rx_pool ||
      !m_.tx_pool || !m_.regs || !m_.status) {
    throw std::invalid_argument("invalid MGBE mapping/capabilities");
  }
  for (auto offset : {i.rx_tail_offset, i.tx_tail_offset, i.rx_completed_offset}) {
    if ((offset & 3) || i.reg_size < 4 || offset > i.reg_size - 4)
      throw std::invalid_argument("invalid MGBE register offset");
  }
  // Hardware tail registers carry only the low 32 bits. A ring cannot cross a
  // 4-GiB boundary, including its one-past-end initial RX tail address.
  for (uint64_t base : {i.rx_ring_dma, i.tx_ring_dma}) {
    if ((base & 15) || (base & 0xffffffffULL) + uint64_t(i.ring_size) * 16 > 0xffffffffULL)
      throw std::invalid_argument("DMA ring crosses tail-pointer address boundary");
  }
  rx_state_.resize(i.ring_size);
  rx_ticket_.resize(i.ring_size);
  tx_lengths_.resize(i.ring_size);
}

void Queue::check_running() const {
  if (__atomic_load_n(&m_.status->state, __ATOMIC_ACQUIRE) != MGBE_PMD_RUNNING)
    throw std::runtime_error("MGBE queue is stopped or faulted; close and reopen the device");
}

size_t Queue::receive(Packet* packets, size_t capacity) {
  check_running();
  if (capacity && !packets) throw std::invalid_argument("null RX output");
  size_t count = 0;
  // Thor's vendor path bounds polling by the RX descriptor writeback index as
  // well as OWN. Preserve both checks rather than assuming a generic DWMAC.
  const uint32_t completed = (le32toh(m_.regs[m_.info.rx_completed_offset / 4]) >> 16) & mask_;
  dma_read_barrier();
  uint32_t budget = ((completed - (rx_next_ & mask_)) & mask_) + 1;
  while (count < capacity && budget-- && rx_next_ - rx_rearm_ < m_.info.ring_size) {
    const uint32_t slot = rx_next_ & mask_;
    auto& d = m_.rx[slot];
    const uint32_t flags = load(&d.word[3]);
    if (flags & own) break;
    dma_read_barrier();
    const uint32_t length = flags & 0x7fffU;
    rx_ticket_[slot] = rx_next_++;
    // Timestamp context descriptors are unsupported in v1. Drain them safely;
    // never expose them as packet buffers. Disable timestamping in the shim.
    if ((flags & (context | rx_error | rx_context_available)) ||
        (flags & (first | last)) != (first | last) || length < 14 || length > m_.info.mtu + 18 ||
        length > m_.info.slot_size) {
      ++counters_.rx_errors;
      rx_state_[slot] = 2;
      rearm();
      continue;
    }
    rx_state_[slot] = 1;
    packets[count++] = {m_.rx_pool + size_t(slot) * m_.info.slot_size, length, slot,
                        rx_ticket_[slot]};
    ++counters_.rx_packets;
    counters_.rx_bytes += length;
  }
  return count;
}

void Queue::release(const Packet& p) {
  check_running();
  mark_released(p);
  rearm();
}
void Queue::mark_released(const Packet& p) {
  if (p.slot > mask_ || rx_state_[p.slot] != 1 || rx_ticket_[p.slot] != p.ticket ||
      p.data != m_.rx_pool + size_t(p.slot) * m_.info.slot_size)
    throw std::invalid_argument("stale, duplicate or foreign RX release");
  rx_state_[p.slot] = 2;
}
void Queue::release(const Packet* packets, size_t count) {
  check_running();
  if (count && !packets) throw std::invalid_argument("null RX release list");
  for (size_t i = 0; i < count; ++i) mark_released(packets[i]);
  rearm();
}

void Queue::rearm() {
  const auto initial = rx_rearm_;
  while (rx_rearm_ != rx_next_ && rx_state_[rx_rearm_ & mask_] == 2) {
    const uint32_t slot = rx_rearm_ & mask_;
    auto& d = m_.rx[slot];
    const auto addr = m_.info.rx_pool_dma + uint64_t(slot) * m_.info.slot_size;
    store(&d.word[0], addr);
    store(&d.word[1], addr >> 32);
    store(&d.word[2], 0);
    dma_write_barrier();
    store(&d.word[3], own);  // IOC deliberately clear: userspace polls.
    rx_state_[slot] = 0;
    ++rx_rearm_;
  }
  if (initial != rx_rearm_)
    doorbell(m_.regs, m_.info.rx_tail_offset,
             // NVIDIA's cyclic RX path always kicks with one-past-the-ring;
             // OWN, rather than a moving tail index, bounds available credits.
             m_.info.rx_ring_dma + uint64_t(m_.info.ring_size) * sizeof(Descriptor));
}

uint8_t* Queue::tx_buffer() {
  check_running();
  reclaim();
  // Retain one unused descriptor to distinguish full and empty hardware rings.
  if (tx_next_ - tx_clean_ >= mask_) return nullptr;
  tx_reserved_ = true;
  return m_.tx_pool + size_t(tx_next_ & mask_) * m_.info.slot_size;
}
void Queue::cancel_tx() {
  tx_reserved_ = false;
}
bool Queue::submit(uint32_t length, bool flush) {
  if (!tx_reserved_) throw std::logic_error("reserve a TX buffer before submission");
  const bool result = submit_buffer(tx_next_ & mask_, length, flush);
  if (result) tx_reserved_ = false;
  return result;
}
uint8_t* Queue::tx_buffer_at(uint32_t slot) const {
  if (slot > mask_) throw std::invalid_argument("invalid TX pool slot");
  return m_.tx_pool + size_t(slot) * m_.info.slot_size;
}
bool Queue::submit_buffer(uint32_t buffer_slot, uint32_t length, bool flush) {
  check_running();
  if (buffer_slot > mask_) throw std::invalid_argument("invalid TX pool slot");
  if (length < 14 || length > m_.info.mtu + 18 || length > 0x3fffU)
    throw std::invalid_argument("TX frame exceeds MTU/buffer limit");
  if (tx_next_ - tx_clean_ >= mask_) return false;
  const uint32_t slot = tx_next_ & mask_;
  auto& d = m_.tx[slot];
  if (load(&d.word[3]) & own) throw std::logic_error("TX slot is still hardware-owned");
  const auto addr = m_.info.tx_pool_dma + uint64_t(buffer_slot) * m_.info.slot_size;
  store(&d.word[0], addr);
  store(&d.word[1], addr >> 32);
  store(&d.word[2], length);
  store(&d.word[3], first | last | length);
  dma_write_barrier();
  store(&d.word[3], own | first | last | length);
  tx_lengths_[slot] = length;
  ++tx_next_;
  tx_pending_ = true;
  if (flush) flush_tx();
  return true;
}
void Queue::flush_tx() {
  check_running();
  if (!tx_pending_) return;
  doorbell(m_.regs, m_.info.tx_tail_offset,
           m_.info.tx_ring_dma + (tx_next_ & mask_) * sizeof(Descriptor));
  tx_pending_ = false;
}
size_t Queue::reclaim() {
  check_running();
  size_t count = 0;
  while (tx_clean_ != tx_next_) {
    const uint32_t slot = tx_clean_ & mask_;
    const uint32_t flags = load(&m_.tx[slot].word[3]);
    if (flags & own) break;
    dma_read_barrier();
    // MGBE retains length bits here. NVIDIA only interprets TDES3_ES_BITS
    // for EQOS, not MGBE (osi_dma_txrx.c: process_last_desc). Completion
    // establishes buffer reuse; receiver/MMC counters establish delivery.
    ++counters_.tx_packets;
    counters_.tx_bytes += tx_lengths_[slot];
    store(&m_.tx[slot].word[3], 0);
    ++tx_clean_;
    ++count;
  }
  return count;
}

Device::Device(const std::string& path, uint32_t ring_size, uint32_t mtu, uint32_t queue_count) {
  try {
    if (!queue_count || queue_count > MGBE_PMD_MAX_QUEUES)
      throw std::invalid_argument("MGBE queue count must be 1..4");
    fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) sysfail("open MGBE control device");
    mgbe_pmd_config config{MGBE_PMD_ABI, ring_size, mtu, queue_count};
    if (ioctl(fd_, MGBE_PMD_IOC_CONFIGURE, &config)) sysfail("configure MGBE");
    mgbe_pmd_info info{};
    if (ioctl(fd_, MGBE_PMD_IOC_INFO, &info)) sysfail("query MGBE capabilities");
    if (info.abi != MGBE_PMD_ABI || info.ring_size != ring_size || info.mtu != mtu ||
        info.queue_count != queue_count || info.max_queues < queue_count ||
        info.max_queues > MGBE_PMD_MAX_QUEUES || info.channel || info.flags ||
        info.reg_size != 4096 || (info.slot_size != 2048 && info.slot_size != 16384) ||
        ring_size > MGBE_PMD_MAX_RING)
      throw std::runtime_error("incompatible MGBE control ABI");
    const auto page = sysconf(_SC_PAGESIZE);
    if (page != 4096) throw std::runtime_error("prototype requires 4-KiB system pages");
    auto map = [&](uint32_t region, size_t bytes, bool write = true) {
      const size_t size = (bytes + page - 1) & ~(size_t(page) - 1);
      void* p = mmap(nullptr, size, PROT_READ | (write ? PROT_WRITE : 0), MAP_SHARED, fd_,
                     off_t(region) * page);
      if (p == MAP_FAILED) sysfail("map MGBE region");
      regions_.push_back({p, size});
      return p;
    };
    status_ = static_cast<mgbe_pmd_status*>(map(MGBE_PMD_STATUS, page, false));
    auto* regs = static_cast<volatile uint32_t*>(map(MGBE_PMD_REGS, info.reg_size));
    queues_.reserve(queue_count);
    for (uint32_t channel = 0; channel < queue_count; ++channel) {
      mgbe_pmd_info query{};
      query.abi = MGBE_PMD_ABI;
      query.channel = channel;
      if (ioctl(fd_, MGBE_PMD_IOC_QUEUE_INFO, &query)) sysfail("query MGBE queue");
      if (query.abi != MGBE_PMD_ABI || query.channel != channel ||
          query.queue_count != queue_count || query.ring_size != ring_size || query.mtu != mtu ||
          query.slot_size != info.slot_size || query.reg_size != info.reg_size ||
          query.max_queues != info.max_queues || query.flags)
        throw std::runtime_error("incompatible MGBE queue ABI");
      Queue::Mapping m{};
      m.info = query;
      m.status = status_;
      const uint32_t base = channel * MGBE_PMD_REGIONS;
      m.rx = static_cast<Descriptor*>(map(base + MGBE_PMD_RX_RING, ring_size * sizeof(Descriptor)));
      m.tx = static_cast<Descriptor*>(map(base + MGBE_PMD_TX_RING, ring_size * sizeof(Descriptor)));
      m.rx_pool =
          static_cast<uint8_t*>(map(base + MGBE_PMD_RX_POOL, size_t(ring_size) * info.slot_size));
      m.tx_pool =
          static_cast<uint8_t*>(map(base + MGBE_PMD_TX_POOL, size_t(ring_size) * info.slot_size));
      m.regs = regs;
      queues_.push_back(std::make_unique<Queue>(m));
    }
  } catch (...) {
    cleanup();
    throw;
  }
}
Device::~Device() {
  cleanup();
}
void Device::start() {
  if (ioctl(fd_, MGBE_PMD_IOC_START)) sysfail("start MGBE");
}
void Device::enable_mac_loopback() {
  if (ioctl(fd_, MGBE_PMD_IOC_MAC_LOOPBACK)) sysfail("enable MGBE internal MAC loopback");
}
void Device::stop() {
  if (ioctl(fd_, MGBE_PMD_IOC_STOP)) sysfail("stop MGBE");
}
mgbe_pmd_stats Device::hardware_stats() {
  mgbe_pmd_stats stats{};
  if (ioctl(fd_, MGBE_PMD_IOC_STATS, &stats)) sysfail("read MGBE hardware statistics");
  return stats;
}
mgbe_pmd_rss Device::rss() {
  mgbe_pmd_rss result{};
  if (ioctl(fd_, MGBE_PMD_IOC_GET_RSS, &result)) sysfail("read MGBE RSS");
  return result;
}
void Device::configure_rss(const mgbe_pmd_rss& rss) {
  if (ioctl(fd_, MGBE_PMD_IOC_SET_RSS, &rss)) sysfail("configure MGBE RSS");
}
void Device::cleanup() noexcept {
  if (fd_ >= 0) ioctl(fd_, MGBE_PMD_IOC_STOP);
  queues_.clear();
  for (auto it = regions_.rbegin(); it != regions_.rend(); ++it) munmap(it->ptr, it->size);
  regions_.clear();
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}
}  // namespace mgbe
