// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace mgbe {

// Optional Ethernet exception path. No NIC queues or DMA buffers are accessed
// by the worker. Applications explicitly forward unwanted RX frames and submit
// returned frames through their usual TX owner. Neither direction allocates.
class KernelTap {
 public:
  struct Options {
    std::string name;
    std::array<uint8_t, 6> mac{};
    uint32_t mtu = 1500;
    uint32_t queue_size = 256;  // power of two, 2..4096, per direction
    int worker_cpu = 0;
  };
  struct Stats {
    uint64_t to_kernel_enqueued, to_kernel_delivered, to_kernel_dropped;
    uint64_t from_kernel_received, from_kernel_delivered, invalid_frames;
    uint64_t discarded_on_stop;
    int worker_cpu, worker_policy, error;
  };

  // Creates a new, nonpersistent TAP with the supplied MAC/MTU, disables
  // checksum/GSO offloads, and brings it up. Existing interfaces are refused.
  // Assign IP addresses/routes using normal Linux tools after construction.
  // Requires CAP_NET_ADMIN. Failure and destruction close/remove the TAP.
  explicit KernelTap(const Options& options);
  ~KernelTap();
  KernelTap(const KernelTap&) = delete;
  KernelTap& operator=(const KernelTap&) = delete;

  // Copy before returning: the application can immediately release its DMA RX
  // buffer, including on a full queue (false). Multiple producers are allowed.
  bool send_to_kernel(const void* frame, size_t length);
  // The hint may race with other consumers; receive_from_kernel() is definitive.
  bool has_tx() const;
  // Caller buffer must hold max_frame_size(). Short frames are zero-padded to
  // 60 bytes for NIC submission. Multiple consumers are allowed, but every NIC
  // TX queue still has exactly one owner. False means no frame was dequeued.
  bool receive_from_kernel(void* frame, size_t capacity, size_t& length);
  size_t max_frame_size() const;
  const std::string& name() const;
  Stats stats() const;
  // Quiesce application producers/consumers first. Bounded shutdown drops any
  // remaining exception frames and reports them in discarded_on_stop.
  void stop();

 private:
  friend struct KernelTapTestAccess;
  // Adopt an already configured packet FD for hardware-free socketpair tests.
  KernelTap(const Options& options, int packet_fd);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace mgbe
