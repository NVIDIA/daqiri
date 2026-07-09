/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "pcie_provider.h"

#include <cuda.h>
#include <cuda_runtime.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <utility>

#include <endian.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "The DAQIRI PCIe v1 shared-ring implementation requires a little-endian host"
#endif

namespace daqiri::pcie {
namespace {

template <typename T>
T make_ioctl_payload() {
  T value{};
  value.header.magic = DAQIRI_PCIE_ABI_MAGIC;
  value.header.version_major = DAQIRI_PCIE_ABI_VERSION_MAJOR;
  value.header.version_minor = DAQIRI_PCIE_ABI_VERSION_MINOR;
  value.header.struct_size = sizeof(T);
  return value;
}

std::string errno_message(const std::string& operation) {
  return operation + ": " + std::strerror(errno);
}

uint32_t entry_region(const daqiri_pcie_ring_entry& entry) {
  return le32toh(entry.region_id);
}

uint32_t entry_slot(const daqiri_pcie_ring_entry& entry) {
  return le32toh(entry.slot_id);
}

uint32_t entry_length(const daqiri_pcie_ring_entry& entry) {
  return le32toh(entry.length);
}

void set_entry_status(daqiri_pcie_ring_entry* entry, daqiri_pcie_completion_status status) {
  entry->status = htole16(static_cast<uint16_t>(status));
}

class MappedRing {
 public:
  bool bind(void* mapping, size_t mapping_bytes, const daqiri_pcie_ring_mapping& layout,
            std::string* error) {
    if (layout.depth == 0) {
      control_ = nullptr;
      entries_ = nullptr;
      depth_ = 0;
      mask_ = 0;
      corrupted_ = false;
      return true;
    }
    if ((layout.depth & (layout.depth - 1)) != 0) {
      *error = "driver returned a non-power-of-two ring depth";
      return false;
    }
    if (layout.control_offset > mapping_bytes ||
        sizeof(daqiri_pcie_ring_control) > mapping_bytes - layout.control_offset) {
      *error = "driver returned an out-of-range ring-control offset";
      return false;
    }
    if (layout.control_offset % alignof(daqiri_pcie_ring_control) != 0 ||
        layout.entries_offset % alignof(daqiri_pcie_ring_entry) != 0) {
      *error = "driver returned a misaligned coherent-ring offset";
      return false;
    }
    const size_t entries_bytes = static_cast<size_t>(layout.depth) * sizeof(daqiri_pcie_ring_entry);
    if (layout.entries_offset > mapping_bytes ||
        entries_bytes > mapping_bytes - layout.entries_offset) {
      *error = "driver returned an out-of-range ring-entry offset";
      return false;
    }

    auto* base = static_cast<uint8_t*>(mapping);
    control_ = reinterpret_cast<daqiri_pcie_ring_control*>(base + layout.control_offset);
    entries_ = reinterpret_cast<daqiri_pcie_ring_entry*>(base + layout.entries_offset);
    depth_ = layout.depth;
    mask_ = depth_ - 1;
    corrupted_ = false;
    if (control_->depth != depth_ || control_->mask != mask_) {
      *error = "driver ring metadata does not match CONFIGURE_QUEUES response";
      control_ = nullptr;
      entries_ = nullptr;
      depth_ = 0;
      return false;
    }
    return true;
  }

  bool push(const daqiri_pcie_ring_entry* values, size_t count) {
    if (count == 0) {
      return true;
    }
    if (control_ == nullptr || values == nullptr || count > depth_) {
      return false;
    }
    const uint64_t producer = load_relaxed(&control_->producer.value);
    const uint64_t consumer = load_acquire(&control_->consumer.value);
    if (producer - consumer > depth_) {
      corrupted_ = true;
      return false;
    }
    if (count > depth_ - (producer - consumer)) {
      return false;
    }
    for (size_t i = 0; i < count; ++i) {
      entries_[(producer + i) & mask_] = values[i];
    }
    store_release(&control_->producer.value, producer + count);
    return true;
  }

  size_t pop(daqiri_pcie_ring_entry* values, size_t capacity) {
    if (control_ == nullptr || values == nullptr || capacity == 0) {
      return 0;
    }
    const uint64_t consumer = load_relaxed(&control_->consumer.value);
    const uint64_t producer = load_acquire(&control_->producer.value);
    if (producer - consumer > depth_) {
      corrupted_ = true;
      return 0;
    }
    const size_t count = static_cast<size_t>(
        std::min<uint64_t>(producer - consumer, static_cast<uint64_t>(capacity)));
    for (size_t i = 0; i < count; ++i) {
      values[i] = entries_[(consumer + i) & mask_];
    }
    if (count != 0) {
      store_release(&control_->consumer.value, consumer + count);
    }
    return count;
  }

  bool corrupted() const {
    return corrupted_;
  }

 private:
  static uint64_t load_relaxed(const volatile uint64_t* value) {
    return __atomic_load_n(const_cast<const uint64_t*>(value), __ATOMIC_RELAXED);
  }
  static uint64_t load_acquire(const volatile uint64_t* value) {
    return __atomic_load_n(const_cast<const uint64_t*>(value), __ATOMIC_ACQUIRE);
  }
  static void store_release(volatile uint64_t* value, uint64_t next) {
    __atomic_store_n(const_cast<uint64_t*>(value), next, __ATOMIC_RELEASE);
  }

  daqiri_pcie_ring_control* control_ = nullptr;
  daqiri_pcie_ring_entry* entries_ = nullptr;
  uint64_t depth_ = 0;
  uint64_t mask_ = 0;
  bool corrupted_ = false;
};

std::string discover_device_path(const std::string& address) {
  if (address.rfind("/dev/", 0) == 0) {
    return address;
  }

  const std::string sysfs_char = "/sys/bus/pci/devices/" + address + "/daqiri_pcie/char";
  std::ifstream input(sysfs_char);
  std::string major_minor;
  if (input >> major_minor && !major_minor.empty()) {
    return "/dev/char/" + major_minor;
  }

  std::string suffix = address;
  std::replace(suffix.begin(), suffix.end(), ':', '_');
  std::replace(suffix.begin(), suffix.end(), '.', '_');
  return "/dev/daqiri-pcie-" + suffix;
}

class CharacterDeviceProvider final : public Provider {
 public:
  ~CharacterDeviceProvider() override {
    (void)stop(1000);
    if (mapping_ != nullptr) {
      munmap(mapping_, mapping_bytes_);
    }
    mapping_ = nullptr;
    for (auto it = registered_regions_.rbegin(); it != registered_regions_.rend(); ++it) {
      if (fd_ < 0) {
        break;
      }
      auto request = make_ioctl_payload<daqiri_pcie_ioctl_unregister_region>();
      request.region_id = *it;
      (void)::ioctl(fd_, DAQIRI_PCIE_IOCTL_UNREGISTER_REGION, &request);
    }
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  bool open(const std::string& device_address) override {
    if (fd_ >= 0) {
      set_error("character-device provider was opened more than once");
      return false;
    }
    device_path_ = discover_device_path(device_address);
    fd_ = ::open(device_path_.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
      set_error(errno_message("open(" + device_path_ +
                              ") failed; install/bind a conforming "
                              "daqiri_pcie kernel driver"));
      return false;
    }

    auto request = make_ioctl_payload<daqiri_pcie_ioctl_caps>();
    if (::ioctl(fd_, DAQIRI_PCIE_IOCTL_GET_CAPS, &request) != 0) {
      set_error(errno_message("DAQIRI_PCIE_IOCTL_GET_CAPS failed"));
      return false;
    }
    if (request.header.magic != DAQIRI_PCIE_ABI_MAGIC ||
        request.header.version_major != DAQIRI_PCIE_ABI_VERSION_MAJOR) {
      set_error("kernel driver uses an incompatible DAQIRI PCIe ABI");
      return false;
    }
    caps_.capabilities = request.capabilities;
    caps_.max_regions = request.max_regions;
    caps_.max_ring_depth = request.max_ring_depth;
    caps_.min_slot_alignment = request.min_slot_alignment;

    auto status = make_ioctl_payload<daqiri_pcie_ioctl_status>();
    if (::ioctl(fd_, DAQIRI_PCIE_IOCTL_GET_STATUS, &status) != 0) {
      set_error(errno_message("initial DAQIRI_PCIE_IOCTL_GET_STATUS failed"));
      return false;
    }
    if ((status.status_flags & DAQIRI_PCIE_STATUS_FLAG_FATAL) != 0) {
      set_error("FPGA driver is already in fatal status " + std::to_string(status.fatal_code));
      return false;
    }
    if ((status.status_flags & DAQIRI_PCIE_STATUS_FLAG_RUNNING) != 0) {
      set_error("FPGA device already has active queues; the character device must be exclusive");
      return false;
    }
    reset_count_ = status.reset_count;
    return true;
  }

  ProviderCaps capabilities() const override {
    return caps_;
  }

  bool register_region(RegionRegistration* region) override {
    if (region == nullptr || fd_ < 0 || region->dmabuf_fd < 0) {
      set_error("invalid DMA-BUF region registration");
      return false;
    }
    auto request = make_ioctl_payload<daqiri_pcie_ioctl_register_region>();
    request.dmabuf_fd = region->dmabuf_fd;
    request.direction = static_cast<uint32_t>(region->direction);
    request.bytes = region->bytes;
    request.slot_stride = region->slot_stride;
    request.slot_count = region->slot_count;
    if (::ioctl(fd_, DAQIRI_PCIE_IOCTL_REGISTER_REGION, &request) != 0) {
      set_error(errno_message("DAQIRI_PCIE_IOCTL_REGISTER_REGION failed"));
      return false;
    }
    region->region_id = request.region_id;
    registered_regions_.push_back(request.region_id);
    return true;
  }

  bool configure(const QueueConfiguration& config) override {
    if (fd_ < 0 || mapping_ != nullptr) {
      set_error("invalid character-device provider configure sequence");
      return false;
    }
    auto request = make_ioctl_payload<daqiri_pcie_ioctl_configure_queues>();
    request.epoch = config.epoch;
    std::copy(std::begin(config.depths), std::end(config.depths),
              std::begin(request.requested_depth));
    if (::ioctl(fd_, DAQIRI_PCIE_IOCTL_CONFIGURE_QUEUES, &request) != 0) {
      set_error(errno_message("DAQIRI_PCIE_IOCTL_CONFIGURE_QUEUES failed"));
      return false;
    }
    if (request.mmap_bytes == 0 || request.mmap_bytes > SIZE_MAX) {
      set_error("driver returned an invalid coherent-ring mmap size");
      return false;
    }
    mapping_bytes_ = static_cast<size_t>(request.mmap_bytes);
    mapping_ = mmap(nullptr, mapping_bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_,
                    static_cast<off_t>(request.mmap_offset));
    if (mapping_ == MAP_FAILED) {
      mapping_ = nullptr;
      set_error(errno_message("mmap of DAQIRI PCIe control rings failed"));
      return false;
    }
    for (size_t i = 0; i < rings_.size(); ++i) {
      if (!rings_[i].bind(mapping_, mapping_bytes_, request.rings[i], &error_)) {
        return false;
      }
      if (request.rings[i].depth != config.depths[i]) {
        set_error("driver changed a requested ring depth");
        return false;
      }
    }
    return true;
  }

  bool start(uint64_t epoch) override {
    auto request = make_ioctl_payload<daqiri_pcie_ioctl_start>();
    request.epoch = epoch;
    // An ioctl error cannot prove that hardware failed before starting DMA.
    // Keep the provider in the may-be-active state so teardown must issue STOP
    // (and RESET if needed) before unmapping any registration.
    started_ = true;
    if (fd_ < 0 || ::ioctl(fd_, DAQIRI_PCIE_IOCTL_START, &request) != 0) {
      set_error(errno_message("DAQIRI_PCIE_IOCTL_START failed"));
      return false;
    }
    return true;
  }

  bool post_rx_available(const daqiri_pcie_ring_entry* entries, size_t count) override {
    return rings_[DAQIRI_PCIE_RING_RX_AVAILABLE].push(entries, count);
  }

  bool post_tx_submission(const daqiri_pcie_ring_entry* entries, size_t count) override {
    return rings_[DAQIRI_PCIE_RING_TX_SUBMISSION].push(entries, count);
  }

  size_t poll_rx_completion(daqiri_pcie_ring_entry* entries, size_t capacity) override {
    return rings_[DAQIRI_PCIE_RING_RX_COMPLETION].pop(entries, capacity);
  }

  size_t poll_tx_completion(daqiri_pcie_ring_entry* entries, size_t capacity) override {
    return rings_[DAQIRI_PCIE_RING_TX_COMPLETION].pop(entries, capacity);
  }

  bool stop(uint32_t timeout_ms) override {
    if (!started_) {
      return true;
    }
    auto request = make_ioctl_payload<daqiri_pcie_ioctl_stop>();
    request.timeout_ms = timeout_ms;
    if (::ioctl(fd_, DAQIRI_PCIE_IOCTL_STOP, &request) != 0) {
      set_error(errno_message("DAQIRI_PCIE_IOCTL_STOP failed"));
      return false;
    }
    if (request.quiesced == 0) {
      set_error("FPGA DMA did not quiesce before the stop timeout");
      return false;
    }
    started_ = false;
    return true;
  }

  bool reset(uint64_t new_epoch) override {
    auto request = make_ioctl_payload<daqiri_pcie_ioctl_reset>();
    request.new_epoch = new_epoch;
    if (fd_ < 0 || ::ioctl(fd_, DAQIRI_PCIE_IOCTL_RESET, &request) != 0) {
      set_error(errno_message("DAQIRI_PCIE_IOCTL_RESET failed"));
      return false;
    }
    started_ = false;
    return true;
  }

  bool healthy() override {
    if (fd_ < 0 || !error_.empty()) {
      return false;
    }
    if (std::any_of(rings_.begin(), rings_.end(),
                    [](const MappedRing& ring) { return ring.corrupted(); })) {
      set_error("FPGA produced corrupt PCIe ring counters");
      return false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last_status_poll_ < std::chrono::milliseconds(100)) {
      return true;
    }
    last_status_poll_ = now;
    auto request = make_ioctl_payload<daqiri_pcie_ioctl_status>();
    if (::ioctl(fd_, DAQIRI_PCIE_IOCTL_GET_STATUS, &request) != 0) {
      set_error(errno_message("DAQIRI_PCIE_IOCTL_GET_STATUS failed"));
      return false;
    }
    if ((request.status_flags & DAQIRI_PCIE_STATUS_FLAG_FATAL) != 0) {
      set_error("FPGA driver reported fatal status " + std::to_string(request.fatal_code));
      return false;
    }
    if (request.reset_count != reset_count_) {
      set_error("FPGA device reset while queues were active");
      return false;
    }
    if (started_ && (request.status_flags & DAQIRI_PCIE_STATUS_FLAG_RUNNING) == 0) {
      set_error("FPGA queues stopped unexpectedly");
      return false;
    }
    return true;
  }

  std::string last_error() const override {
    return error_;
  }

 private:
  void set_error(std::string value) {
    error_ = std::move(value);
  }

  int fd_ = -1;
  std::string device_path_;
  ProviderCaps caps_{};
  void* mapping_ = nullptr;
  size_t mapping_bytes_ = 0;
  std::array<MappedRing, DAQIRI_PCIE_RING_COUNT> rings_{};
  std::vector<uint32_t> registered_regions_;
  bool started_ = false;
  uint64_t reset_count_ = 0;
  std::string error_;
  std::chrono::steady_clock::time_point last_status_poll_{};
};

class SoftwareRing {
 public:
  bool configure(uint32_t depth) {
    if (depth == 0) {
      entries_.clear();
      mask_ = 0;
      producer_ = 0;
      consumer_ = 0;
      return true;
    }
    if ((depth & (depth - 1)) != 0) {
      return false;
    }
    entries_.assign(depth, {});
    mask_ = depth - 1;
    producer_ = 0;
    consumer_ = 0;
    return true;
  }

  bool enabled() const {
    return !entries_.empty();
  }
  size_t available() const {
    return static_cast<size_t>(producer_ - consumer_);
  }
  size_t free_count() const {
    return entries_.size() - available();
  }

  bool push(const daqiri_pcie_ring_entry* values, size_t count) {
    if (count == 0) {
      return true;
    }
    if (values == nullptr || count > free_count()) {
      return false;
    }
    for (size_t i = 0; i < count; ++i) {
      entries_[(producer_ + i) & mask_] = values[i];
    }
    std::atomic_thread_fence(std::memory_order_release);
    producer_ += count;
    return true;
  }

  size_t pop(daqiri_pcie_ring_entry* values, size_t capacity) {
    if (values == nullptr || capacity == 0) {
      return 0;
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    const size_t count = std::min(capacity, available());
    for (size_t i = 0; i < count; ++i) {
      values[i] = entries_[(consumer_ + i) & mask_];
    }
    consumer_ += count;
    return count;
  }

 private:
  std::vector<daqiri_pcie_ring_entry> entries_;
  uint64_t producer_ = 0;
  uint64_t consumer_ = 0;
  uint64_t mask_ = 0;
};

class SoftwareLoopbackProvider final : public Provider {
 public:
  bool open(const std::string& device_address) override {
    (void)device_address;
    std::lock_guard<std::mutex> lock(mutex_);
    // Internal deterministic verification hook, intentionally not part of the
    // YAML/public API. Supported one-shot faults: stale_epoch, bad_length, and
    // device_reset (unset, empty, or "none" leaves loopback fault-free).
    if (const char* value = std::getenv("DAQIRI_PCIE_LOOPBACK_FAULT")) {
      const std::string fault(value);
      if (fault == "stale_epoch") {
        fault_ = FaultInjection::STALE_EPOCH;
      } else if (fault == "bad_length") {
        fault_ = FaultInjection::BAD_LENGTH;
      } else if (fault == "device_reset") {
        fault_ = FaultInjection::DEVICE_RESET;
      } else if (!fault.empty() && fault != "none") {
        error_ = "unknown DAQIRI_PCIE_LOOPBACK_FAULT value '" + fault + "'";
        return false;
      }
    }
    opened_ = true;
    return true;
  }

  ProviderCaps capabilities() const override {
    ProviderCaps result;
    result.capabilities =
        DAQIRI_PCIE_CAP_DMABUF_PCIE | DAQIRI_PCIE_CAP_DMA_FENCE | DAQIRI_PCIE_CAP_DEVICE_RESET;
    result.max_regions = 2;
    result.max_ring_depth = 1U << 30;
    result.min_slot_alignment = 256;
    return result;
  }

  bool register_region(RegionRegistration* region) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!opened_ || region == nullptr || region->gpu_base == nullptr || region->slot_stride == 0 ||
        region->slot_count == 0) {
      error_ = "invalid software-loopback region registration";
      return false;
    }
    region->region_id = next_region_id_++;
    regions_[region->region_id] = *region;
    if (region->direction == DAQIRI_PCIE_DIRECTION_RX) {
      rx_region_id_ = region->region_id;
    } else {
      tx_region_id_ = region->region_id;
    }
    return true;
  }

  bool configure(const QueueConfiguration& config) override {
    std::lock_guard<std::mutex> lock(mutex_);
    epoch_ = config.epoch;
    for (size_t i = 0; i < rings_.size(); ++i) {
      if (!rings_[i].configure(config.depths[i])) {
        error_ = "software-loopback ring depth is not a power of two";
        return false;
      }
    }
    return true;
  }

  bool start(uint64_t epoch) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!opened_ || epoch != epoch_) {
      error_ = "software-loopback start epoch does not match configured epoch";
      return false;
    }
    running_ = true;
    return true;
  }

  bool post_rx_available(const daqiri_pcie_ring_entry* entries, size_t count) override {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_or_configured() && rings_[DAQIRI_PCIE_RING_RX_AVAILABLE].push(entries, count);
  }

  bool post_tx_submission(const daqiri_pcie_ring_entry* entries, size_t count) override {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_ && rings_[DAQIRI_PCIE_RING_TX_SUBMISSION].push(entries, count);
  }

  size_t poll_rx_completion(daqiri_pcie_ring_entry* entries, size_t capacity) override {
    std::lock_guard<std::mutex> lock(mutex_);
    progress();
    return rings_[DAQIRI_PCIE_RING_RX_COMPLETION].pop(entries, capacity);
  }

  size_t poll_tx_completion(daqiri_pcie_ring_entry* entries, size_t capacity) override {
    std::lock_guard<std::mutex> lock(mutex_);
    progress();
    return rings_[DAQIRI_PCIE_RING_TX_COMPLETION].pop(entries, capacity);
  }

  bool stop(uint32_t timeout_ms) override {
    (void)timeout_ms;
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    return true;
  }

  bool reset(uint64_t new_epoch) override {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    epoch_ = new_epoch;
    return true;
  }

  bool healthy() override {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_.empty();
  }

  std::string last_error() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
  }

 private:
  enum class FaultInjection { NONE, STALE_EPOCH, BAD_LENGTH, DEVICE_RESET };

  bool running_or_configured() const {
    return running_ || epoch_ != 0;
  }

  const RegionRegistration* find_region(uint32_t id) const {
    auto it = regions_.find(id);
    return it == regions_.end() ? nullptr : &it->second;
  }

  void progress() {
    if (!running_) {
      return;
    }
    auto& submissions = rings_[DAQIRI_PCIE_RING_TX_SUBMISSION];
    auto& tx_completions = rings_[DAQIRI_PCIE_RING_TX_COMPLETION];
    auto& rx_available = rings_[DAQIRI_PCIE_RING_RX_AVAILABLE];
    auto& rx_completions = rings_[DAQIRI_PCIE_RING_RX_COMPLETION];

    while (submissions.available() != 0 && tx_completions.free_count() != 0) {
      const bool has_rx = rx_available.enabled() && rx_completions.enabled();
      if (has_rx && (rx_available.available() == 0 || rx_completions.free_count() == 0)) {
        break;
      }

      daqiri_pcie_ring_entry tx{};
      if (submissions.pop(&tx, 1) != 1) {
        break;
      }
      daqiri_pcie_ring_entry tx_completion = tx;
      set_entry_status(&tx_completion, DAQIRI_PCIE_COMPLETION_OK);

      if (has_rx) {
        daqiri_pcie_ring_entry rx{};
        (void)rx_available.pop(&rx, 1);
        daqiri_pcie_ring_entry rx_completion = rx;
        const auto* tx_region = find_region(entry_region(tx));
        const auto* rx_region = find_region(entry_region(rx));
        const uint32_t tx_slot = entry_slot(tx);
        const uint32_t rx_slot = entry_slot(rx);
        const uint32_t length = entry_length(tx);
        daqiri_pcie_completion_status status = DAQIRI_PCIE_COMPLETION_OK;

        if (tx_region == nullptr || rx_region == nullptr ||
            tx_region->direction != DAQIRI_PCIE_DIRECTION_TX ||
            rx_region->direction != DAQIRI_PCIE_DIRECTION_RX || tx_slot >= tx_region->slot_count ||
            rx_slot >= rx_region->slot_count) {
          status = DAQIRI_PCIE_COMPLETION_BAD_DESCRIPTOR;
        } else if (length > tx_region->slot_stride || length > rx_region->slot_stride) {
          status = DAQIRI_PCIE_COMPLETION_LENGTH_ERROR;
        } else if (length != 0) {
          auto* src = static_cast<uint8_t*>(tx_region->gpu_base) +
                      static_cast<size_t>(tx_slot) * tx_region->slot_stride;
          auto* dst = static_cast<uint8_t*>(rx_region->gpu_base) +
                      static_cast<size_t>(rx_slot) * rx_region->slot_stride;
          cudaError_t copy_result = cudaSuccess;
          if (tx_region->gpu_device == rx_region->gpu_device) {
            static thread_local int selected_loopback_device = -1;
            if (selected_loopback_device != rx_region->gpu_device) {
              copy_result = cudaSetDevice(rx_region->gpu_device);
              if (copy_result == cudaSuccess) {
                selected_loopback_device = rx_region->gpu_device;
              }
            }
            if (copy_result == cudaSuccess) {
              copy_result = cudaMemcpy(dst, src, length, cudaMemcpyDeviceToDevice);
            }
          } else {
            copy_result =
                cudaMemcpyPeer(dst, rx_region->gpu_device, src, tx_region->gpu_device, length);
          }
          if (copy_result != cudaSuccess) {
            status = DAQIRI_PCIE_COMPLETION_INTERNAL_ERROR;
            (void)cudaGetLastError();
          }
        }
        rx_completion.length = htole32(length);
        set_entry_status(&rx_completion, status);
        if (status != DAQIRI_PCIE_COMPLETION_OK) {
          set_entry_status(&tx_completion, status);
        }
        if (!fault_injected_) {
          if (fault_ == FaultInjection::STALE_EPOCH) {
            rx_completion.epoch = htole64(le64toh(rx_completion.epoch) ^ UINT64_C(1));
            fault_injected_ = true;
          } else if (fault_ == FaultInjection::BAD_LENGTH) {
            rx_completion.length = htole32(UINT32_MAX);
            fault_injected_ = true;
          } else if (fault_ == FaultInjection::DEVICE_RESET) {
            set_entry_status(&rx_completion, DAQIRI_PCIE_COMPLETION_DEVICE_RESET);
            fault_injected_ = true;
          }
        }
        (void)rx_completions.push(&rx_completion, 1);
      }
      if (!has_rx && !fault_injected_) {
        if (fault_ == FaultInjection::STALE_EPOCH) {
          tx_completion.epoch = htole64(le64toh(tx_completion.epoch) ^ UINT64_C(1));
          fault_injected_ = true;
        } else if (fault_ == FaultInjection::BAD_LENGTH) {
          tx_completion.length = htole32(UINT32_MAX);
          fault_injected_ = true;
        } else if (fault_ == FaultInjection::DEVICE_RESET) {
          set_entry_status(&tx_completion, DAQIRI_PCIE_COMPLETION_DEVICE_RESET);
          fault_injected_ = true;
        }
      }
      (void)tx_completions.push(&tx_completion, 1);
    }
  }

  mutable std::mutex mutex_;
  bool opened_ = false;
  bool running_ = false;
  uint64_t epoch_ = 0;
  uint32_t next_region_id_ = 1;
  uint32_t rx_region_id_ = 0;
  uint32_t tx_region_id_ = 0;
  FaultInjection fault_ = FaultInjection::NONE;
  bool fault_injected_ = false;
  std::unordered_map<uint32_t, RegionRegistration> regions_;
  std::array<SoftwareRing, DAQIRI_PCIE_RING_COUNT> rings_{};
  std::string error_;
};

}  // namespace

std::unique_ptr<Provider> make_character_device_provider() {
  return std::make_unique<CharacterDeviceProvider>();
}

std::unique_ptr<Provider> make_software_loopback_provider() {
  return std::make_unique<SoftwareLoopbackProvider>();
}

}  // namespace daqiri::pcie
