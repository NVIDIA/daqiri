// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace mgbe {
// One worker, separate RX and TX chunks. Native driver / zero-copy only.
class AfXdp {
 public:
  AfXdp(const std::string& interface, const std::string& program, uint32_t local_ip,
        uint16_t local_port, uint32_t busy_poll_us);
  ~AfXdp();
  AfXdp(const AfXdp&) = delete;
  AfXdp& operator=(const AfXdp&) = delete;
  bool receive(const uint8_t*& data, uint32_t& length);
  void release();
  uint8_t* tx_buffer();
  void cancel_tx();
  bool submit(uint32_t length);
  void reclaim();
  struct Frame {
    const uint8_t* data;
    uint32_t length;
  };
  struct Stats {
    uint64_t dropped, invalid_rx, invalid_tx, ring_full, fill_empty, tx_empty;
  };
  uint32_t receive_burst(Frame* frames, uint32_t capacity);
  void release_burst();
  uint32_t reserve_tx_burst(uint8_t** frames, uint32_t count);
  bool submit_tx_burst(uint32_t length);
  uint64_t outstanding_tx() const;
  uint64_t completed_tx() const;
  Stats statistics() const;
  const uint8_t* mac() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace mgbe
