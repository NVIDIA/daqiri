// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "mgbe_pmd_uapi.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mgbe {
struct alignas(16) Descriptor {
  uint32_t word[4];
};
struct Packet {
  uint8_t* data;
  uint32_t length;
  uint32_t slot;
  uint64_t ticket;
};
struct Counters {
  uint64_t rx_packets = 0, rx_bytes = 0, rx_errors = 0;
  uint64_t tx_packets = 0, tx_bytes = 0, tx_errors = 0;
};

// One thread owns a Queue. Returned buffers must not outlive it. TX submissions
// use separate registered TX slots; callers write directly into tx_buffer().
class Queue {
 public:
  struct Mapping {
    Descriptor* rx;
    Descriptor* tx;
    uint8_t* rx_pool;
    uint8_t* tx_pool;
    volatile uint32_t* regs;
    const mgbe_pmd_status* status;
    mgbe_pmd_info info;
  };
  explicit Queue(Mapping mapping);
  size_t receive(Packet* packets, size_t capacity);
  void release(const Packet& packet);
  void release(const Packet* packets, size_t count);
  // nullptr when every TX slot is outstanding. A slot is reserved until submit
  // or cancel_tx(), so holding it across receive/reclaim is safe.
  uint8_t* tx_buffer();
  void cancel_tx();
  bool submit(uint32_t length, bool flush = true);
  void flush_tx();
  size_t reclaim();
  const Counters& counters() const {
    return counters_;
  }
  const mgbe_pmd_info& info() const {
    return m_.info;
  }

 private:
  void check_running() const;
  void rearm();
  void mark_released(const Packet& packet);
  Mapping m_;
  uint32_t mask_;
  uint64_t rx_next_ = 0, rx_rearm_ = 0, tx_next_ = 0, tx_clean_ = 0;
  bool tx_reserved_ = false;
  bool tx_pending_ = false;
  std::vector<uint8_t> rx_state_;
  std::vector<uint64_t> rx_ticket_;
  std::vector<uint32_t> tx_lengths_;
  Counters counters_;
};

class Device {
 public:
  Device(const std::string& path, uint32_t ring_size = 1024, uint32_t mtu = 1500);
  ~Device();
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;
  void start();
  void enable_mac_loopback();
  void stop();
  mgbe_pmd_stats hardware_stats();
  Queue& queue() {
    return *queue_;
  }
  const mgbe_pmd_status& status() const {
    return *status_;
  }

 private:
  int fd_ = -1;
  struct Region {
    void* ptr;
    size_t size;
  };
  std::vector<Region> regions_;
  std::unique_ptr<Queue> queue_;
  const mgbe_pmd_status* status_ = nullptr;
  void cleanup() noexcept;
};
}  // namespace mgbe
