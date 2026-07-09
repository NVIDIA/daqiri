/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <daqiri/pcie_abi.h>

namespace daqiri::pcie {

struct ProviderCaps {
  uint64_t capabilities = 0;
  uint32_t max_regions = 0;
  uint32_t max_ring_depth = 0;
  uint32_t min_slot_alignment = 256;
};

struct RegionRegistration {
  uint32_t region_id = 0;
  daqiri_pcie_direction direction = DAQIRI_PCIE_DIRECTION_RX;
  int dmabuf_fd = -1;
  void* gpu_base = nullptr;  // Used only by the software-loopback provider.
  size_t bytes = 0;
  uint32_t slot_stride = 0;
  uint32_t slot_count = 0;
  int gpu_device = -1;
};

struct QueueConfiguration {
  uint64_t epoch = 0;
  uint32_t depths[DAQIRI_PCIE_RING_COUNT]{};
};

class Provider {
 public:
  virtual ~Provider() = default;

  virtual bool open(const std::string& device_address) = 0;
  virtual ProviderCaps capabilities() const = 0;
  virtual bool register_region(RegionRegistration* region) = 0;
  virtual bool configure(const QueueConfiguration& config) = 0;
  virtual bool start(uint64_t epoch) = 0;

  virtual bool post_rx_available(const daqiri_pcie_ring_entry* entries, size_t count) = 0;
  virtual bool post_tx_submission(const daqiri_pcie_ring_entry* entries, size_t count) = 0;
  virtual size_t poll_rx_completion(daqiri_pcie_ring_entry* entries, size_t capacity) = 0;
  virtual size_t poll_tx_completion(daqiri_pcie_ring_entry* entries, size_t capacity) = 0;

  virtual bool stop(uint32_t timeout_ms) = 0;
  // A successful reset synchronously stops all DMA before returning and
  // invalidates descriptors from the old epoch.
  virtual bool reset(uint64_t new_epoch) = 0;
  virtual bool healthy() = 0;
  virtual std::string last_error() const = 0;
};

std::unique_ptr<Provider> make_character_device_provider();
std::unique_ptr<Provider> make_software_loopback_provider();

}  // namespace daqiri::pcie
