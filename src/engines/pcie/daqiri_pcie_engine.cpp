/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "daqiri_pcie_engine.h"

#include "pcie_provider.h"

#include <cuda.h>
#include <cuda_runtime.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>

#include <endian.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <daqiri/logging.hpp>

namespace daqiri {
namespace {

constexpr uint32_t kPcieSlotAlignment = 256;
constexpr size_t kCompletionPollBatch = 64;
constexpr uint32_t kStopTimeoutMs = 2000;

enum class SlotOwner : uint8_t { FREE, DEVICE, PENDING, APPLICATION, REPOST };

size_t align_up(size_t value, size_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

uint32_t ring_depth_for(size_t slots) {
  if (slots == 0 || slots > (UINT32_MAX / 2U)) {
    return 0;
  }
  uint32_t depth = 1;
  while (depth < slots) {
    depth <<= 1U;
  }
  return std::max<uint32_t>(depth, 2);
}

uint64_t make_epoch() {
  std::random_device random;
  const auto now =
      static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  uint64_t epoch =
      (static_cast<uint64_t>(random()) << 32U) ^ random() ^ now ^ static_cast<uint64_t>(getpid());
  return epoch == 0 ? 1 : epoch;
}

daqiri_pcie_ring_entry make_entry(uint64_t epoch, uint64_t sequence, uint32_t region, uint32_t slot,
                                  uint32_t length) {
  daqiri_pcie_ring_entry entry{};
  entry.epoch = htole64(epoch);
  entry.sequence = htole64(sequence);
  entry.region_id = htole32(region);
  entry.slot_id = htole32(slot);
  entry.length = htole32(length);
  entry.status = htole16(DAQIRI_PCIE_COMPLETION_OK);
  return entry;
}

uint64_t completion_epoch(const daqiri_pcie_ring_entry& entry) {
  return le64toh(entry.epoch);
}

uint64_t completion_sequence(const daqiri_pcie_ring_entry& entry) {
  return le64toh(entry.sequence);
}

uint32_t completion_region(const daqiri_pcie_ring_entry& entry) {
  return le32toh(entry.region_id);
}

uint32_t completion_slot(const daqiri_pcie_ring_entry& entry) {
  return le32toh(entry.slot_id);
}

uint32_t completion_length(const daqiri_pcie_ring_entry& entry) {
  return le32toh(entry.length);
}

uint16_t completion_status(const daqiri_pcie_ring_entry& entry) {
  return le16toh(entry.status);
}

bool socket_configured(const SocketConfig& config) {
  return config.mode_ != SocketMode::INVALID || !config.local_addr_.empty() ||
         !config.remote_addr_.empty() || !config.local_ip_.empty() || !config.remote_ip_.empty() ||
         config.local_port_ != 0 || config.remote_port_ != 0 || config.max_payload_size_ != 0 ||
         config.max_burst_interval_ms_ != 0 || config.min_ipg_ns_ != 0;
}

std::string cuda_error(CUresult result) {
  const char* name = nullptr;
  const char* text = nullptr;
  (void)cuGetErrorName(result, &name);
  (void)cuGetErrorString(result, &text);
  return std::string(name == nullptr ? "CUDA_ERROR" : name) + ": " +
         (text == nullptr ? "unknown CUDA driver error" : text);
}

}  // namespace

struct PcieEngine::QueueState {
  InterfaceState* interface = nullptr;
  uint16_t queue_id = 0;
  bool rx = false;
  uint64_t next_sequence = 1;
  std::string mr_name;
  pcie::RegionRegistration region{};
  size_t payload_size = 0;
  uint32_t batch_size = 1;
  uint64_t timeout_us = 0;
  int worker_cpu = -1;
  int gpu = -1;
  bool needs_cuda_flush = false;

  std::mutex slot_mutex;
  std::vector<SlotOwner> owners;
  std::vector<uint64_t> expected_sequence;
  std::vector<uint32_t> expected_length;
  std::deque<uint32_t> free_slots;
  std::deque<uint32_t> deferred_slots;

  std::vector<daqiri_pcie_ring_entry> pending;
  std::chrono::steady_clock::time_point pending_since{};
  std::mutex ready_mutex;
  std::deque<BurstParams*> ready;

  std::thread worker;
};

struct PcieEngine::InterfaceState {
  uint16_t port = 0;
  std::string name;
  std::string address;
  bool loopback = false;
  uint64_t epoch = 0;
  std::unique_ptr<pcie::Provider> provider;
  std::mutex provider_mutex;
  bool provider_started = false;
  bool quiesce_failed = false;
  std::vector<int> dmabuf_fds;
  std::vector<std::unique_ptr<QueueState>> rx_queues;
  std::vector<std::unique_ptr<QueueState>> tx_queues;
  std::atomic<bool> active{true};
};

struct PcieEngine::BurstStorage {
  QueueState* queue = nullptr;
  bool rx = false;
  bool submitted = false;
  std::vector<uint32_t> slots;
  std::vector<void*> pointers;
  std::vector<uint32_t> lengths;
  std::vector<uint8_t> released;
  std::mutex mutex;
};

PcieEngine::PcieEngine() = default;

PcieEngine::~PcieEngine() {
  shutdown();
}

PcieEngine::InterfaceState* PcieEngine::find_interface(uint16_t port) {
  for (auto& state : interfaces_) {
    if (state != nullptr && state->port == port) {
      return state.get();
    }
  }
  return nullptr;
}

const PcieEngine::InterfaceState* PcieEngine::find_interface(uint16_t port) const {
  for (const auto& state : interfaces_) {
    if (state != nullptr && state->port == port) {
      return state.get();
    }
  }
  return nullptr;
}

PcieEngine::QueueState* PcieEngine::find_queue(InterfaceState& state, bool rx, uint16_t queue_id) {
  auto& queues = rx ? state.rx_queues : state.tx_queues;
  for (auto& queue : queues) {
    if (queue != nullptr && queue->queue_id == queue_id) {
      return queue.get();
    }
  }
  return nullptr;
}

PcieEngine::BurstStorage* PcieEngine::burst_storage(BurstParams* burst) {
  return burst == nullptr ? nullptr : static_cast<BurstStorage*>(burst->custom_pkt_data.get());
}

const PcieEngine::BurstStorage* PcieEngine::burst_storage(const BurstParams* burst) {
  return burst == nullptr ? nullptr
                          : static_cast<const BurstStorage*>(burst->custom_pkt_data.get());
}

bool PcieEngine::set_config_and_initialize(const NetworkConfig& cfg) {
  if (initialized_ || !interfaces_.empty()) {
    DAQIRI_LOG_ERROR("PCIe engine cannot be initialized more than once");
    return false;
  }
  cfg_ = cfg;
  for (size_t i = 0; i < cfg_.ifs_.size(); ++i) {
    cfg_.ifs_[i].port_id_ = static_cast<uint16_t>(i);
  }
  for (auto& item : cfg_.mrs_) {
    item.second.adj_size_ = align_up(item.second.buf_size_, kPcieSlotAlignment);
  }
  initialize();
  return initialized_;
}

void PcieEngine::initialize() {
  initialized_ = false;
  healthy_.store(true);
  if (!validate_config()) {
    return;
  }

  if (allocate_memory_regions() != Status::SUCCESS) {
    DAQIRI_LOG_ERROR("PCIe engine failed to allocate GPU memory regions");
    return;
  }

  interfaces_.reserve(cfg_.ifs_.size());
  for (const auto& config : cfg_.ifs_) {
    auto state = std::make_unique<InterfaceState>();
    state->port = config.port_id_;
    state->name = config.name_;
    state->address = config.address_;
    state->loopback = cfg_.common_.loopback_ == LoopbackType::LOOPBACK_TYPE_SW;
    state->epoch = make_epoch();
    if (!initialize_interface(*state, config)) {
      interfaces_.push_back(std::move(state));
      shutdown();
      return;
    }
    interfaces_.push_back(std::move(state));
  }

  running_.store(true, std::memory_order_release);
  accepting_tx_.store(true, std::memory_order_release);
  for (auto& state : interfaces_) {
    for (auto& queue : state->rx_queues) {
      queue->worker = std::thread(&PcieEngine::rx_worker_loop, this, queue.get());
    }
    for (auto& queue : state->tx_queues) {
      queue->worker = std::thread(&PcieEngine::tx_worker_loop, this, queue.get());
    }
  }
  init_rx_core_q_map();
  initialized_ = true;
  DAQIRI_LOG_INFO("PCIe stream initialized with {} 3rd-party device interface(s)",
                  interfaces_.size());
}

void PcieEngine::run() {}

bool PcieEngine::initialize_interface(InterfaceState& state, const InterfaceConfig& config) {
  for (const auto& config_queue : config.rx_.queues_) {
    auto queue = std::make_unique<QueueState>();
    queue->interface = &state;
    queue->queue_id = config_queue.common_.id_;
    queue->rx = true;
    queue->mr_name = config_queue.common_.mrs_.front();
    queue->batch_size = static_cast<uint32_t>(config_queue.common_.batch_size_);
    queue->timeout_us = config_queue.timeout_us_;
    queue->worker_cpu = std::strtol(config_queue.common_.cpu_core_.c_str(), nullptr, 10);
    state.rx_queues.push_back(std::move(queue));
  }
  for (const auto& config_queue : config.tx_.queues_) {
    auto queue = std::make_unique<QueueState>();
    queue->interface = &state;
    queue->queue_id = config_queue.common_.id_;
    queue->rx = false;
    queue->mr_name = config_queue.common_.mrs_.front();
    queue->batch_size = static_cast<uint32_t>(config_queue.common_.batch_size_);
    queue->worker_cpu = std::strtol(config_queue.common_.cpu_core_.c_str(), nullptr, 10);
    state.tx_queues.push_back(std::move(queue));
  }

  state.provider = state.loopback ? pcie::make_software_loopback_provider()
                                  : pcie::make_character_device_provider();
  if (!state.provider->open(state.address)) {
    DAQIRI_LOG_ERROR("PCIe provider for interface '{}' failed to open: {}", state.name,
                     state.provider->last_error());
    return false;
  }
  const auto caps = state.provider->capabilities();
  const uint32_t region_count =
      static_cast<uint32_t>(state.rx_queues.size() + state.tx_queues.size());
  if ((caps.capabilities & DAQIRI_PCIE_CAP_DMA_FENCE) == 0 ||
      (!state.loopback && (caps.capabilities & DAQIRI_PCIE_CAP_DMABUF_PCIE) == 0)) {
    DAQIRI_LOG_ERROR("PCIe provider '{}' lacks a required DMA-BUF or DMA-fence capability",
                     state.name);
    return false;
  }
  const bool requires_multi_queue = state.rx_queues.size() > 1 || state.tx_queues.size() > 1;
  if (caps.max_regions < region_count || caps.max_queues < region_count ||
      (requires_multi_queue && (caps.capabilities & DAQIRI_PCIE_CAP_MULTI_QUEUE) == 0) ||
      caps.min_slot_alignment == 0 || caps.min_slot_alignment > kPcieSlotAlignment ||
      (kPcieSlotAlignment % caps.min_slot_alignment) != 0) {
    DAQIRI_LOG_ERROR("PCIe provider '{}' cannot register {} regions with {}-byte slot alignment",
                     state.name, region_count, kPcieSlotAlignment);
    return false;
  }

  for (auto& queue : state.rx_queues) {
    if (!register_region(state, *queue) || !initialize_cuda_ordering(*queue)) {
      return false;
    }
  }
  for (auto& queue : state.tx_queues) {
    if (!register_region(state, *queue)) {
      return false;
    }
  }

  pcie::QueueConfiguration queue_config{};
  queue_config.epoch = state.epoch;
  auto configure_queue = [&](const QueueState& queue) {
    const uint32_t depth = ring_depth_for(queue.region.slot_count);
    if (depth == 0) {
      DAQIRI_LOG_ERROR("PCIe queue {} region for '{}' has too many slots", queue.queue_id,
                       state.name);
      return false;
    }
    if (depth > caps.max_ring_depth || depth < 2) {
      DAQIRI_LOG_ERROR("PCIe ring depth {} is unsupported by provider '{}'", depth, state.name);
      return false;
    }
    queue_config.queues.push_back({queue.queue_id,
                                   queue.rx ? DAQIRI_PCIE_DIRECTION_RX : DAQIRI_PCIE_DIRECTION_TX,
                                   queue.region.region_id, depth});
    return true;
  };
  for (const auto& queue : state.rx_queues) {
    if (!configure_queue(*queue)) {
      return false;
    }
  }
  for (const auto& queue : state.tx_queues) {
    if (!configure_queue(*queue)) {
      return false;
    }
  }
  if (!state.provider->configure(queue_config)) {
    DAQIRI_LOG_ERROR("PCIe queue configuration failed for '{}': {}", state.name,
                     state.provider->last_error());
    return false;
  }
  for (auto& queue : state.rx_queues) {
    if (!post_initial_rx_slots(*queue)) {
      return false;
    }
  }
  // Treat every START attempt as potentially active: an ioctl can fail after
  // hardware has begun DMA. Shutdown must quiesce/reset even on an error return
  // before it releases rings, DMA-BUFs, or GPU allocations.
  state.provider_started = true;
  if (!state.provider->start(state.epoch)) {
    DAQIRI_LOG_ERROR("PCIe provider start failed for '{}': {}", state.name,
                     state.provider->last_error());
    return false;
  }
  return true;
}

bool PcieEngine::register_region(InterfaceState& state, QueueState& queue) {
  const std::string& mr_name = queue.mr_name;
  auto config_it = cfg_.mrs_.find(mr_name);
  auto allocation_it = ar_.find(mr_name);
  if (config_it == cfg_.mrs_.end() || allocation_it == ar_.end()) {
    return false;
  }
  const auto& config = config_it->second;
  auto& allocation = allocation_it->second;

  pcie::RegionRegistration region{};
  region.direction = queue.rx ? DAQIRI_PCIE_DIRECTION_RX : DAQIRI_PCIE_DIRECTION_TX;
  region.gpu_base = allocation.ptr_;
  region.bytes = allocation.size_;
  region.slot_stride = static_cast<uint32_t>(config.adj_size_);
  region.slot_count = static_cast<uint32_t>(config.num_bufs_);
  region.gpu_device = config.affinity_;

  const CUdeviceptr pointer = reinterpret_cast<CUdeviceptr>(allocation.ptr_);
  if (!state.loopback) {
    if (cudaSetDevice(config.affinity_) != cudaSuccess) {
      DAQIRI_LOG_ERROR("Cannot select GPU {} for PCIe MR '{}'", config.affinity_, mr_name);
      return false;
    }
    CUdevice device = 0;
    if (cuDeviceGet(&device, config.affinity_) != CUDA_SUCCESS) {
      return false;
    }
    int dmabuf_supported = 0;
    int allocation_gpudirect_capable = 0;
    CUresult result =
        cuDeviceGetAttribute(&dmabuf_supported, CU_DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED, device);
    if (result == CUDA_SUCCESS) {
      result = cuPointerGetAttribute(&allocation_gpudirect_capable,
                                     CU_POINTER_ATTRIBUTE_IS_GPU_DIRECT_RDMA_CAPABLE, pointer);
    }
    if (result != CUDA_SUCCESS || dmabuf_supported == 0 || allocation_gpudirect_capable == 0) {
      DAQIRI_LOG_ERROR("PCIe MR '{}' is not GPUDirect-capable DMA-BUF memory: {}", mr_name,
                       cuda_error(result));
      return false;
    }

    unsigned int sync_memops = 1;
    result = cuPointerSetAttribute(&sync_memops, CU_POINTER_ATTRIBUTE_SYNC_MEMOPS, pointer);
    if (result != CUDA_SUCCESS) {
      DAQIRI_LOG_ERROR("SYNC_MEMOPS failed for PCIe MR '{}': {}", mr_name, cuda_error(result));
      return false;
    }

    int fd = -1;
    // The exported dma-buf is attached to the emulated BF3 PCI function below;
    // the NVIDIA exporter then supplies that PCI device's peer-DMA mapping via
    // dma_buf_map_attachment().  Do not force the CUDA 13 PCIe mapping flag:
    // CUDA returns CUDA_ERROR_NOT_SUPPORTED for it on H100 even though ordinary
    // device-memory dma-buf export and GPUDirect RDMA are supported.
    result = cuMemGetHandleForAddressRange(&fd, pointer, allocation.size_,
                                           CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD, 0);
    if (result != CUDA_SUCCESS) {
      DAQIRI_LOG_ERROR("BAR1 DMA-BUF export failed for PCIe MR '{}': {}", mr_name,
                       cuda_error(result));
      return false;
    }
    region.dmabuf_fd = fd;
    state.dmabuf_fds.push_back(fd);
  }

  if (!state.provider->register_region(&region)) {
    DAQIRI_LOG_ERROR("Provider registration failed for PCIe MR '{}': {}", mr_name,
                     state.provider->last_error());
    return false;
  }
  queue.region = region;
  queue.payload_size = config.buf_size_;
  queue.gpu = config.affinity_;
  queue.owners.assign(config.num_bufs_, SlotOwner::FREE);
  queue.expected_sequence.assign(config.num_bufs_, 0);
  if (!queue.rx) {
    queue.expected_length.assign(config.num_bufs_, 0);
    for (uint32_t slot = 0; slot < region.slot_count; ++slot) {
      queue.free_slots.push_back(slot);
    }
  }
  return true;
}

bool PcieEngine::initialize_cuda_ordering(QueueState& queue) {
  InterfaceState& state = *queue.interface;
  if (!queue.rx || state.loopback) {
    return true;
  }
  if (cudaSetDevice(queue.gpu) != cudaSuccess) {
    return false;
  }
  CUdevice device = 0;
  if (cuDeviceGet(&device, queue.gpu) != CUDA_SUCCESS) {
    return false;
  }
  int ordering = CU_GPU_DIRECT_RDMA_WRITES_ORDERING_NONE;
  CUresult result =
      cuDeviceGetAttribute(&ordering, CU_DEVICE_ATTRIBUTE_GPU_DIRECT_RDMA_WRITES_ORDERING, device);
  if (result != CUDA_SUCCESS) {
    DAQIRI_LOG_ERROR("Cannot query GPUDirect write ordering for GPU {}: {}", queue.gpu,
                     cuda_error(result));
    return false;
  }
  queue.needs_cuda_flush = ordering < CU_GPU_DIRECT_RDMA_WRITES_ORDERING_OWNER;
  if (!queue.needs_cuda_flush) {
    return true;
  }

  int flush_options = 0;
  result = cuDeviceGetAttribute(&flush_options,
                                CU_DEVICE_ATTRIBUTE_GPU_DIRECT_RDMA_FLUSH_WRITES_OPTIONS, device);
  if (result != CUDA_SUCCESS ||
      (flush_options & CU_FLUSH_GPU_DIRECT_RDMA_WRITES_OPTION_HOST) == 0) {
    DAQIRI_LOG_ERROR("GPU {} lacks native OWNER ordering and host GPUDirect write flush",
                     queue.gpu);
    return false;
  }
  return true;
}

bool PcieEngine::post_initial_rx_slots(QueueState& queue) {
  InterfaceState& state = *queue.interface;
  if (!queue.rx) {
    return true;
  }
  std::vector<daqiri_pcie_ring_entry> entries;
  entries.reserve(queue.region.slot_count);
  {
    std::lock_guard<std::mutex> lock(queue.slot_mutex);
    for (uint32_t slot = 0; slot < queue.region.slot_count; ++slot) {
      const uint64_t sequence = queue.next_sequence++;
      queue.expected_sequence[slot] = sequence;
      queue.owners[slot] = SlotOwner::DEVICE;
      entries.push_back(make_entry(state.epoch, sequence, queue.region.region_id, slot,
                                   static_cast<uint32_t>(queue.payload_size)));
    }
  }
  std::lock_guard<std::mutex> provider_lock(state.provider_mutex);
  if (!state.provider->post_rx_available(queue.queue_id, entries.data(), entries.size())) {
    DAQIRI_LOG_ERROR("Initial RX credit post failed for PCIe interface '{}' queue {}", state.name,
                     queue.queue_id);
    return false;
  }
  return true;
}

bool PcieEngine::post_rx_slot(QueueState& queue, uint32_t slot_id) {
  InterfaceState& state = *queue.interface;
  std::lock_guard<std::mutex> slot_lock(queue.slot_mutex);
  if (slot_id >= queue.owners.size() || queue.owners[slot_id] != SlotOwner::APPLICATION) {
    return false;
  }
  if (!state.active.load(std::memory_order_acquire)) {
    queue.owners[slot_id] = SlotOwner::FREE;
    return true;
  }
  const uint64_t sequence = queue.next_sequence++;
  queue.expected_sequence[slot_id] = sequence;
  queue.owners[slot_id] = SlotOwner::REPOST;
  const auto entry = make_entry(state.epoch, sequence, queue.region.region_id, slot_id,
                                static_cast<uint32_t>(queue.payload_size));
  std::lock_guard<std::mutex> provider_lock(state.provider_mutex);
  if (!state.active.load(std::memory_order_acquire)) {
    queue.owners[slot_id] = SlotOwner::FREE;
    return true;
  }
  if (!state.provider->post_rx_available(queue.queue_id, &entry, 1)) {
    queue.deferred_slots.push_back(slot_id);
    rx_backpressure_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  queue.owners[slot_id] = SlotOwner::DEVICE;
  return true;
}

void PcieEngine::retry_deferred_rx_slots(QueueState& queue) {
  InterfaceState& state = *queue.interface;
  if (!state.active.load(std::memory_order_acquire)) {
    return;
  }
  std::lock_guard<std::mutex> slot_lock(queue.slot_mutex);
  while (!queue.deferred_slots.empty()) {
    const uint32_t slot = queue.deferred_slots.front();
    if (slot >= queue.owners.size() || queue.owners[slot] != SlotOwner::REPOST) {
      mark_unhealthy(state, "deferred RX credit has invalid slot ownership");
      return;
    }
    const auto entry =
        make_entry(state.epoch, queue.expected_sequence[slot], queue.region.region_id, slot,
                   static_cast<uint32_t>(queue.payload_size));
    std::lock_guard<std::mutex> provider_lock(state.provider_mutex);
    if (!state.active.load(std::memory_order_acquire)) {
      return;
    }
    if (!state.provider->post_rx_available(queue.queue_id, &entry, 1)) {
      return;
    }
    queue.owners[slot] = SlotOwner::DEVICE;
    queue.deferred_slots.pop_front();
  }
}

bool PcieEngine::flush_remote_writes(QueueState& queue) {
  if (!queue.needs_cuda_flush) {
    return true;
  }
  if (cudaSetDevice(queue.gpu) != cudaSuccess) {
    cuda_flush_failures_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const CUresult result = cuFlushGPUDirectRDMAWrites(
      CU_FLUSH_GPU_DIRECT_RDMA_WRITES_TARGET_CURRENT_CTX, CU_FLUSH_GPU_DIRECT_RDMA_WRITES_TO_OWNER);
  if (result != CUDA_SUCCESS) {
    cuda_flush_failures_.fetch_add(1, std::memory_order_relaxed);
    DAQIRI_LOG_ERROR("GPUDirect remote-write flush failed: {}", cuda_error(result));
    return false;
  }
  return true;
}

bool PcieEngine::provider_is_healthy(InterfaceState& state) {
  bool provider_healthy = false;
  std::string provider_error;
  {
    std::lock_guard<std::mutex> provider_lock(state.provider_mutex);
    provider_healthy = state.provider->healthy();
    if (!provider_healthy) {
      provider_error = state.provider->last_error();
    }
  }
  if (!provider_healthy) {
    mark_unhealthy(state, "provider failure: " + provider_error);
  }
  return provider_healthy;
}

void PcieEngine::rx_worker_loop(QueueState* queue) {
  if (queue == nullptr || queue->interface == nullptr) {
    return;
  }
  InterfaceState& state = *queue->interface;
  if (queue->worker_cpu >= 0 && queue->worker_cpu < CPU_SETSIZE) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(queue->worker_cpu, &set);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
  }
  (void)cudaSetDevice(queue->gpu);

  while (running_.load(std::memory_order_acquire) && state.active.load()) {
    process_rx_completions(*queue);
    if (!state.active.load(std::memory_order_acquire)) {
      break;
    }
    retry_deferred_rx_slots(*queue);
    if (!provider_is_healthy(state)) {
      break;
    }
    std::this_thread::yield();
  }
}

void PcieEngine::tx_worker_loop(QueueState* queue) {
  if (queue == nullptr || queue->interface == nullptr) {
    return;
  }
  InterfaceState& state = *queue->interface;
  if (queue->worker_cpu >= 0 && queue->worker_cpu < CPU_SETSIZE) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(queue->worker_cpu, &set);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
  }
  (void)cudaSetDevice(queue->gpu);

  while (running_.load(std::memory_order_acquire) && state.active.load()) {
    process_tx_completions(*queue);
    if (!provider_is_healthy(state)) {
      break;
    }
    std::this_thread::yield();
  }
}

void PcieEngine::process_rx_completions(QueueState& queue) {
  InterfaceState& state = *queue.interface;
  if (!queue.rx) {
    return;
  }
  std::array<daqiri_pcie_ring_entry, kCompletionPollBatch> completions{};
  size_t count = 0;
  {
    std::lock_guard<std::mutex> provider_lock(state.provider_mutex);
    count =
        state.provider->poll_rx_completion(queue.queue_id, completions.data(), completions.size());
  }

  std::string failure;
  if (count != 0) {
    std::lock_guard<std::mutex> slot_lock(queue.slot_mutex);
    for (size_t i = 0; i < count; ++i) {
      const auto& completion = completions[i];
      const uint32_t slot = completion_slot(completion);
      const uint32_t length = completion_length(completion);
      if (completion_status(completion) != DAQIRI_PCIE_COMPLETION_OK) {
        failure = "RX completion status " + std::to_string(completion_status(completion));
        break;
      }
      if (completion_epoch(completion) != state.epoch ||
          completion_region(completion) != queue.region.region_id || slot >= queue.owners.size() ||
          length > queue.payload_size || queue.owners[slot] != SlotOwner::DEVICE ||
          completion_sequence(completion) != queue.expected_sequence[slot]) {
        failure = "stale, duplicate, or malformed RX completion";
        break;
      }
      queue.owners[slot] = SlotOwner::PENDING;
      if (queue.pending.empty()) {
        queue.pending_since = std::chrono::steady_clock::now();
      }
      queue.pending.push_back(completion);
    }
  }
  if (!failure.empty()) {
    malformed_completions_.fetch_add(1, std::memory_order_relaxed);
    mark_unhealthy(state, failure);
    return;
  }

  while (queue.pending.size() >= queue.batch_size && state.active.load()) {
    if (!publish_rx_burst(queue, queue.batch_size)) {
      return;
    }
  }
  if (!queue.pending.empty() && queue.timeout_us != 0) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - queue.pending_since);
    if (elapsed.count() >= static_cast<int64_t>(queue.timeout_us)) {
      (void)publish_rx_burst(queue, queue.pending.size());
    }
  }
}

bool PcieEngine::publish_rx_burst(QueueState& queue, size_t count) {
  InterfaceState& state = *queue.interface;
  if (count == 0 || count > queue.pending.size()) {
    return false;
  }
  if (!flush_remote_writes(queue)) {
    mark_unhealthy(state, "CUDA could not make remote writes visible to the owner context");
    return false;
  }

  auto* burst = new (std::nothrow) BurstParams{};
  auto* storage = new (std::nothrow) BurstStorage{};
  if (burst == nullptr || storage == nullptr) {
    delete burst;
    delete storage;
    mark_unhealthy(state, "RX metadata allocation failed");
    return false;
  }
  storage->queue = &queue;
  storage->rx = true;
  storage->slots.reserve(count);
  storage->pointers.reserve(count);
  storage->lengths.reserve(count);
  storage->released.assign(count, 0);
  auto* base = static_cast<uint8_t*>(queue.region.gpu_base);
  {
    std::lock_guard<std::mutex> slot_lock(queue.slot_mutex);
    for (size_t i = 0; i < count; ++i) {
      const uint32_t slot = completion_slot(queue.pending[i]);
      if (slot >= queue.owners.size() || queue.owners[slot] != SlotOwner::PENDING) {
        delete burst;
        delete storage;
        mark_unhealthy(state, "RX slot ownership changed before burst publication");
        return false;
      }
      queue.owners[slot] = SlotOwner::APPLICATION;
      storage->slots.push_back(slot);
      storage->pointers.push_back(base + static_cast<size_t>(slot) * queue.region.slot_stride);
      storage->lengths.push_back(completion_length(queue.pending[i]));
    }
  }
  queue.pending.erase(queue.pending.begin(), queue.pending.begin() + count);
  if (!queue.pending.empty()) {
    queue.pending_since = std::chrono::steady_clock::now();
  }

  burst->custom_pkt_data = std::shared_ptr<void>(storage);
  burst->hdr.hdr.num_pkts = count;
  burst->hdr.hdr.port_id = state.port;
  burst->hdr.hdr.q_id = queue.queue_id;
  burst->hdr.hdr.num_segs = 1;
  burst->hdr.hdr.max_pkt = queue.region.slot_count;
  burst->hdr.hdr.max_pkt_size = static_cast<uint32_t>(queue.payload_size);
  burst->hdr.hdr.first_pkt_addr =
      count == 0 ? 0 : reinterpret_cast<uintptr_t>(storage->pointers.front());
  burst->pkts[0] = storage->pointers.data();
  burst->pkt_lens[0] = storage->lengths.data();
  uint64_t bytes = 0;
  for (uint32_t length : storage->lengths) {
    bytes += length;
  }
  burst->hdr.hdr.nbytes = bytes;

  {
    std::lock_guard<std::mutex> ready_lock(queue.ready_mutex);
    queue.ready.push_back(burst);
  }
  rx_packets_.fetch_add(count, std::memory_order_relaxed);
  rx_bytes_.fetch_add(bytes, std::memory_order_relaxed);
  return true;
}

void PcieEngine::process_tx_completions(QueueState& queue) {
  InterfaceState& state = *queue.interface;
  if (queue.rx) {
    return;
  }
  std::array<daqiri_pcie_ring_entry, kCompletionPollBatch> completions{};
  size_t count = 0;
  {
    std::lock_guard<std::mutex> provider_lock(state.provider_mutex);
    count =
        state.provider->poll_tx_completion(queue.queue_id, completions.data(), completions.size());
  }
  if (count == 0) {
    return;
  }

  std::string failure;
  {
    std::lock_guard<std::mutex> slot_lock(queue.slot_mutex);
    for (size_t i = 0; i < count; ++i) {
      const auto& completion = completions[i];
      const uint32_t slot = completion_slot(completion);
      if (completion_status(completion) != DAQIRI_PCIE_COMPLETION_OK) {
        failure = "TX completion status " + std::to_string(completion_status(completion));
        break;
      }
      if (completion_epoch(completion) != state.epoch ||
          completion_region(completion) != queue.region.region_id || slot >= queue.owners.size() ||
          queue.owners[slot] != SlotOwner::DEVICE ||
          completion_sequence(completion) != queue.expected_sequence[slot] ||
          completion_length(completion) != queue.expected_length[slot]) {
        failure = "stale, duplicate, or malformed TX completion";
        break;
      }
      queue.owners[slot] = SlotOwner::FREE;
      queue.free_slots.push_back(slot);
    }
  }
  if (!failure.empty()) {
    malformed_completions_.fetch_add(1, std::memory_order_relaxed);
    mark_unhealthy(state, failure);
  }
}

void PcieEngine::mark_unhealthy(InterfaceState& state, const std::string& reason) {
  const bool was_active = state.active.exchange(false);
  healthy_.store(false, std::memory_order_release);
  accepting_tx_.store(false, std::memory_order_release);
  if (was_active) {
    DAQIRI_LOG_ERROR("PCIe interface '{}' is unhealthy: {}", state.name, reason);
  }
}

void* PcieEngine::get_packet_ptr(BurstParams* burst, int idx) {
  return get_segment_packet_ptr(burst, 0, idx);
}

uint32_t PcieEngine::get_packet_length(BurstParams* burst, int idx) {
  return get_segment_packet_length(burst, 0, idx);
}

void* PcieEngine::get_segment_packet_ptr(BurstParams* burst, int seg, int idx) {
  if (burst == nullptr || seg != 0 || idx < 0 || idx >= static_cast<int>(burst->hdr.hdr.num_pkts) ||
      burst->pkts[0] == nullptr) {
    return nullptr;
  }
  return burst->pkts[0][idx];
}

uint32_t PcieEngine::get_segment_packet_length(BurstParams* burst, int seg, int idx) {
  if (burst == nullptr || seg != 0 || idx < 0 || idx >= static_cast<int>(burst->hdr.hdr.num_pkts) ||
      burst->pkt_lens[0] == nullptr) {
    return 0;
  }
  return burst->pkt_lens[0][idx];
}

FlowId PcieEngine::get_packet_flow_id(BurstParams* burst, int idx) {
  (void)burst;
  (void)idx;
  return 0;
}

Status PcieEngine::get_packet_rx_timestamp(BurstParams* burst, int idx, uint64_t* timestamp_ns) {
  if (burst == nullptr || timestamp_ns == nullptr) {
    return Status::NULL_PTR;
  }
  if (idx < 0 || idx >= static_cast<int>(burst->hdr.hdr.num_pkts)) {
    return Status::INVALID_PARAMETER;
  }
  return Status::NOT_SUPPORTED;
}

void* PcieEngine::get_packet_extra_info(BurstParams* burst, int idx) {
  (void)burst;
  (void)idx;
  return nullptr;
}

BurstParams* PcieEngine::create_tx_burst_params() {
  return new (std::nothrow) BurstParams{};
}

Status PcieEngine::get_tx_metadata_buffer(BurstParams** burst) {
  if (burst == nullptr) {
    return Status::NULL_PTR;
  }
  *burst = create_tx_burst_params();
  return *burst == nullptr ? Status::NO_FREE_BURST_BUFFERS : Status::SUCCESS;
}

bool PcieEngine::is_tx_burst_available(BurstParams* burst) {
  if (burst == nullptr || !healthy_.load(std::memory_order_acquire) ||
      !accepting_tx_.load(std::memory_order_acquire)) {
    return false;
  }
  auto* state = find_interface(burst->hdr.hdr.port_id);
  if (state == nullptr || !state->active.load() || burst->hdr.hdr.num_pkts == 0) {
    return false;
  }
  auto* queue = find_queue(*state, false, burst->hdr.hdr.q_id);
  if (queue == nullptr || burst->hdr.hdr.num_pkts > queue->batch_size ||
      burst->hdr.hdr.num_pkts > queue->region.slot_count) {
    return false;
  }
  std::lock_guard<std::mutex> lock(queue->slot_mutex);
  return queue->free_slots.size() >= burst->hdr.hdr.num_pkts;
}

Status PcieEngine::get_tx_packet_burst(BurstParams* burst) {
  if (burst == nullptr) {
    return Status::NULL_PTR;
  }
  if (!healthy_.load(std::memory_order_acquire) || !accepting_tx_.load(std::memory_order_acquire)) {
    return Status::INTERNAL_ERROR;
  }
  if (burst_storage(burst) != nullptr || burst->hdr.hdr.num_segs != 1 ||
      burst->hdr.hdr.num_pkts == 0) {
    return Status::INVALID_PARAMETER;
  }
  auto* state = find_interface(burst->hdr.hdr.port_id);
  if (state == nullptr || !state->active.load(std::memory_order_acquire)) {
    return Status::INVALID_PARAMETER;
  }
  auto* queue = find_queue(*state, false, burst->hdr.hdr.q_id);
  if (queue == nullptr || burst->hdr.hdr.num_pkts > queue->region.slot_count ||
      burst->hdr.hdr.num_pkts > queue->batch_size) {
    return Status::INVALID_PARAMETER;
  }

  auto* storage = new (std::nothrow) BurstStorage{};
  if (storage == nullptr) {
    return Status::NO_FREE_BURST_BUFFERS;
  }
  storage->queue = queue;
  storage->rx = false;
  const size_t count = burst->hdr.hdr.num_pkts;
  storage->slots.reserve(count);
  storage->pointers.reserve(count);
  storage->lengths.assign(count, 0);
  storage->released.assign(count, 0);
  {
    std::lock_guard<std::mutex> lock(queue->slot_mutex);
    if (!accepting_tx_.load(std::memory_order_acquire) ||
        !state->active.load(std::memory_order_acquire)) {
      delete storage;
      return Status::INTERNAL_ERROR;
    }
    if (queue->free_slots.size() < count) {
      delete storage;
      tx_backpressure_.fetch_add(1, std::memory_order_relaxed);
      return Status::NO_FREE_PACKET_BUFFERS;
    }
    auto* base = static_cast<uint8_t*>(queue->region.gpu_base);
    for (size_t i = 0; i < count; ++i) {
      const uint32_t slot = queue->free_slots.front();
      queue->free_slots.pop_front();
      queue->owners[slot] = SlotOwner::APPLICATION;
      storage->slots.push_back(slot);
      storage->pointers.push_back(base + static_cast<size_t>(slot) * queue->region.slot_stride);
    }
  }

  burst->custom_pkt_data = std::shared_ptr<void>(storage);
  burst->pkts[0] = storage->pointers.data();
  burst->pkt_lens[0] = storage->lengths.data();
  burst->hdr.hdr.max_pkt = queue->region.slot_count;
  burst->hdr.hdr.max_pkt_size = static_cast<uint32_t>(queue->payload_size);
  burst->hdr.hdr.first_pkt_addr = reinterpret_cast<uintptr_t>(storage->pointers.front());
  return Status::SUCCESS;
}

Status PcieEngine::set_packet_lengths(BurstParams* burst, int idx,
                                      const std::initializer_list<int>& lens) {
  auto* storage = burst_storage(burst);
  if (burst == nullptr || storage == nullptr || idx < 0 ||
      idx >= static_cast<int>(storage->lengths.size()) || lens.size() != 1) {
    return Status::INVALID_PARAMETER;
  }
  const int length = *lens.begin();
  const size_t maximum = storage->queue->payload_size;
  if (length < 0 || static_cast<size_t>(length) > maximum) {
    return Status::INVALID_PARAMETER;
  }
  storage->lengths[idx] = static_cast<uint32_t>(length);
  return Status::SUCCESS;
}

Status PcieEngine::set_eth_header(BurstParams* burst, int idx, char* dst_addr) {
  (void)burst;
  (void)idx;
  (void)dst_addr;
  return Status::NOT_SUPPORTED;
}

Status PcieEngine::set_ipv4_header(BurstParams* burst, int idx, int ip_len, uint8_t proto,
                                   unsigned int src_host, unsigned int dst_host) {
  (void)burst;
  (void)idx;
  (void)ip_len;
  (void)proto;
  (void)src_host;
  (void)dst_host;
  return Status::NOT_SUPPORTED;
}

Status PcieEngine::set_udp_header(BurstParams* burst, int idx, int udp_len, uint16_t src_port,
                                  uint16_t dst_port) {
  (void)burst;
  (void)idx;
  (void)udp_len;
  (void)src_port;
  (void)dst_port;
  return Status::NOT_SUPPORTED;
}

Status PcieEngine::set_udp_payload(BurstParams* burst, int idx, void* data, int len) {
  (void)burst;
  (void)idx;
  (void)data;
  (void)len;
  return Status::NOT_SUPPORTED;
}

Status PcieEngine::set_packet_tx_time(BurstParams* burst, int idx, uint64_t time) {
  (void)burst;
  (void)idx;
  (void)time;
  return Status::NOT_SUPPORTED;
}

bool PcieEngine::release_packet(BurstParams* burst, int pkt) {
  auto* storage = burst_storage(burst);
  if (storage == nullptr || storage->queue == nullptr || storage->queue->interface == nullptr ||
      pkt < 0 || pkt >= static_cast<int>(storage->slots.size())) {
    return false;
  }
  std::lock_guard<std::mutex> storage_lock(storage->mutex);
  if (storage->released[pkt] != 0 || storage->submitted) {
    return false;
  }
  QueueState& queue = *storage->queue;
  InterfaceState& state = *queue.interface;
  const uint32_t slot = storage->slots[pkt];
  if (storage->rx) {
    if (!post_rx_slot(queue, slot)) {
      mark_unhealthy(state, "RX credit ring rejected a returned application slot");
      return false;
    }
  } else {
    std::lock_guard<std::mutex> slot_lock(queue.slot_mutex);
    if (slot >= queue.owners.size() || queue.owners[slot] != SlotOwner::APPLICATION) {
      return false;
    }
    queue.owners[slot] = SlotOwner::FREE;
    queue.free_slots.push_back(slot);
  }
  storage->released[pkt] = 1;
  storage->pointers[pkt] = nullptr;
  return true;
}

void PcieEngine::free_packet(BurstParams* burst, int pkt) {
  (void)release_packet(burst, pkt);
}

void PcieEngine::free_packet_segment(BurstParams* burst, int seg, int pkt) {
  if (seg == 0) {
    free_packet(burst, pkt);
  }
}

void PcieEngine::free_all_segment_packets(BurstParams* burst, int seg) {
  if (seg != 0 || burst == nullptr) {
    return;
  }
  for (size_t i = 0; i < burst->hdr.hdr.num_pkts; ++i) {
    (void)release_packet(burst, static_cast<int>(i));
  }
}

void PcieEngine::free_all_packets(BurstParams* burst) {
  free_all_segment_packets(burst, 0);
}

void PcieEngine::reclaim_unsent_tx(BurstParams* burst) {
  auto* storage = burst_storage(burst);
  if (storage == nullptr || storage->rx || storage->submitted) {
    return;
  }
  for (size_t i = 0; i < storage->slots.size(); ++i) {
    (void)release_packet(burst, static_cast<int>(i));
  }
}

void PcieEngine::delete_burst(BurstParams* burst) {
  delete burst;
}

void PcieEngine::free_rx_burst(BurstParams* burst) {
  delete_burst(burst);
}

void PcieEngine::free_tx_burst(BurstParams* burst) {
  delete_burst(burst);
}

void PcieEngine::free_rx_metadata(BurstParams* burst) {
  delete_burst(burst);
}

void PcieEngine::free_tx_metadata(BurstParams* burst) {
  delete_burst(burst);
}

Status PcieEngine::send_tx_burst(BurstParams* burst) {
  auto* storage = burst_storage(burst);
  if (burst == nullptr || storage == nullptr || storage->rx || storage->queue == nullptr ||
      storage->queue->interface == nullptr) {
    return Status::INVALID_PARAMETER;
  }
  QueueState& queue = *storage->queue;
  InterfaceState& state = *queue.interface;
  if (!accepting_tx_.load(std::memory_order_acquire) || !state.active.load()) {
    return Status::INTERNAL_ERROR;
  }

  std::vector<daqiri_pcie_ring_entry> entries;
  entries.reserve(storage->slots.size());
  uint64_t bytes = 0;
  {
    std::lock_guard<std::mutex> storage_lock(storage->mutex);
    std::lock_guard<std::mutex> slot_lock(queue.slot_mutex);
    for (size_t i = 0; i < storage->slots.size(); ++i) {
      const uint32_t slot = storage->slots[i];
      if (storage->released[i] != 0 || slot >= queue.owners.size() ||
          queue.owners[slot] != SlotOwner::APPLICATION ||
          storage->lengths[i] > queue.payload_size) {
        return Status::INVALID_PARAMETER;
      }
    }
    for (size_t i = 0; i < storage->slots.size(); ++i) {
      const uint32_t slot = storage->slots[i];
      const uint32_t length = storage->lengths[i];
      const uint64_t sequence = queue.next_sequence++;
      queue.expected_sequence[slot] = sequence;
      queue.expected_length[slot] = length;
      queue.owners[slot] = SlotOwner::DEVICE;
      entries.push_back(make_entry(state.epoch, sequence, queue.region.region_id, slot, length));
      bytes += length;
    }
    std::lock_guard<std::mutex> provider_lock(state.provider_mutex);
    if (!accepting_tx_.load(std::memory_order_acquire) ||
        !state.active.load(std::memory_order_acquire) ||
        !state.provider->post_tx_submission(queue.queue_id, entries.data(), entries.size())) {
      for (uint32_t slot : storage->slots) {
        queue.owners[slot] = SlotOwner::APPLICATION;
      }
      tx_backpressure_.fetch_add(1, std::memory_order_relaxed);
    } else {
      storage->submitted = true;
    }
  }

  if (!storage->submitted) {
    reclaim_unsent_tx(burst);
    delete_burst(burst);
    return Status::NO_SPACE_AVAILABLE;
  }
  tx_packets_.fetch_add(storage->slots.size(), std::memory_order_relaxed);
  tx_bytes_.fetch_add(bytes, std::memory_order_relaxed);
  delete_burst(burst);
  return Status::SUCCESS;
}

Status PcieEngine::get_rx_burst(BurstParams** burst, int port, int q) {
  if (burst == nullptr) {
    return Status::NULL_PTR;
  }
  *burst = nullptr;
  if (!healthy_.load(std::memory_order_acquire)) {
    return Status::INTERNAL_ERROR;
  }
  auto* state = find_interface(static_cast<uint16_t>(port));
  if (state == nullptr || q < 0 || q > UINT16_MAX) {
    return Status::INVALID_PARAMETER;
  }
  auto* queue = find_queue(*state, true, static_cast<uint16_t>(q));
  if (queue == nullptr) {
    return Status::INVALID_PARAMETER;
  }
  std::lock_guard<std::mutex> lock(queue->ready_mutex);
  if (queue->ready.empty()) {
    return Status::NULL_PTR;
  }
  *burst = queue->ready.front();
  queue->ready.pop_front();
  return Status::SUCCESS;
}

uint64_t PcieEngine::get_burst_tot_byte(BurstParams* burst) {
  if (burst == nullptr || burst->pkt_lens[0] == nullptr) {
    return 0;
  }
  uint64_t total = 0;
  for (size_t i = 0; i < burst->hdr.hdr.num_pkts; ++i) {
    total += burst->pkt_lens[0][i];
  }
  return total;
}

Status PcieEngine::get_mac_addr(int port, char* mac) {
  (void)port;
  if (mac == nullptr) {
    return Status::NULL_PTR;
  }
  return Status::NOT_SUPPORTED;
}

void PcieEngine::print_stats() {
  DAQIRI_LOG_INFO(
      "PCIe stats: rx_pkts={} rx_bytes={} tx_pkts={} tx_bytes={} rx_backpressure={} "
      "tx_backpressure={} malformed_completions={} cuda_flush_failures={}",
      rx_packets_.load(), rx_bytes_.load(), tx_packets_.load(), tx_bytes_.load(),
      rx_backpressure_.load(), tx_backpressure_.load(), malformed_completions_.load(),
      cuda_flush_failures_.load());
}

void PcieEngine::shutdown() {
  if (!initialized_ && interfaces_.empty()) {
    return;
  }
  accepting_tx_.store(false, std::memory_order_release);
  running_.store(false, std::memory_order_release);
  for (auto& state : interfaces_) {
    if (state != nullptr) {
      state->active.store(false, std::memory_order_release);
    }
  }

  for (auto& state : interfaces_) {
    if (state == nullptr || state->provider == nullptr || !state->provider_started) {
      continue;
    }
    std::lock_guard<std::mutex> provider_lock(state->provider_mutex);
    if (!state->provider->stop(kStopTimeoutMs)) {
      DAQIRI_LOG_ERROR("PCIe provider '{}' failed to quiesce: {}", state->name,
                       state->provider->last_error());
      const uint64_t reset_epoch = make_epoch();
      if (!state->provider->reset(reset_epoch)) {
        state->quiesce_failed = true;
        DAQIRI_LOG_CRITICAL(
            "PCIe provider '{}' could not be reset after a quiesce failure; its provider, "
            "DMA-BUF descriptors, and GPU allocations will be retained for process safety",
            state->name);
      } else {
        state->epoch = reset_epoch;
        DAQIRI_LOG_WARN("PCIe provider '{}' required a device reset to stop DMA", state->name);
      }
    }
    state->provider_started = false;
  }
  for (auto& state : interfaces_) {
    if (state == nullptr) {
      continue;
    }
    for (auto& queue : state->rx_queues) {
      if (queue->worker.joinable()) {
        queue->worker.join();
      }
    }
    for (auto& queue : state->tx_queues) {
      if (queue->worker.joinable()) {
        queue->worker.join();
      }
    }
  }

  for (auto& state : interfaces_) {
    if (state == nullptr) {
      continue;
    }
    for (auto& queue : state->rx_queues) {
      {
        std::lock_guard<std::mutex> ready_lock(queue->ready_mutex);
        while (!queue->ready.empty()) {
          delete_burst(queue->ready.front());
          queue->ready.pop_front();
        }
      }
      queue->pending.clear();
    }
    if (state->quiesce_failed) {
      for (const auto& queue : state->rx_queues) {
        ar_[queue->mr_name].deallocator_ = AllocRegion::Deallocator::NONE;
      }
      for (const auto& queue : state->tx_queues) {
        ar_[queue->mr_name].deallocator_ = AllocRegion::Deallocator::NONE;
      }
      (void)state->provider.release();
      state->dmabuf_fds.clear();
      continue;
    }
    state->provider.reset();
    for (int fd : state->dmabuf_fds) {
      if (fd >= 0) {
        ::close(fd);
      }
    }
    state->dmabuf_fds.clear();
  }
  interfaces_.clear();
  initialized_ = false;
  healthy_.store(false, std::memory_order_release);
}

bool PcieEngine::validate_config() const {
  bool valid = true;
  if (cfg_.common_.stream_type != StreamType::PCIE) {
    DAQIRI_LOG_ERROR("PCIe engine requires stream_type=pcie");
    valid = false;
  }
  if (is_explicit_engine_type(cfg_.common_.engine)) {
    DAQIRI_LOG_ERROR("stream_type=pcie does not accept an engine override");
    valid = false;
  }
  if (cfg_.common_.protocol != SocketProtocol::INVALID) {
    DAQIRI_LOG_ERROR("stream_type=pcie does not accept a socket protocol");
    valid = false;
  }
  if (cfg_.ifs_.empty() || cfg_.ifs_.size() > MAX_INTERFACES) {
    DAQIRI_LOG_ERROR("PCIe streams require between one and {} interfaces", MAX_INTERFACES);
    valid = false;
  }

  std::unordered_set<std::string> used_regions;
  for (const auto& interface : cfg_.ifs_) {
    const size_t queue_count = interface.rx_.queues_.size() + interface.tx_.queues_.size();
    if (interface.rx_.queues_.size() > DAQIRI_PCIE_MAX_QUEUES_PER_DIRECTION ||
        interface.tx_.queues_.size() > DAQIRI_PCIE_MAX_QUEUES_PER_DIRECTION ||
        queue_count > DAQIRI_PCIE_MAX_QUEUES) {
      DAQIRI_LOG_ERROR("PCIe interface '{}' exceeds the {}-queue limit", interface.name_,
                       DAQIRI_PCIE_MAX_QUEUES);
      valid = false;
    }
    if (interface.rx_.queues_.empty() && interface.tx_.queues_.empty()) {
      DAQIRI_LOG_ERROR("PCIe interface '{}' has no RX or TX queue", interface.name_);
      valid = false;
    }
    if (socket_configured(interface.socket_) ||
        interface.roce_.transport_mode_ != RDMATransportMode::INVALID ||
        interface.rdma_.mode_ != RDMAMode::INVALID ||
        interface.rdma_.xmode_ != RDMATransportMode::INVALID || interface.rdma_.port_ != 0) {
      DAQIRI_LOG_ERROR("PCIe interface '{}' contains socket/RoCE/RDMA configuration",
                       interface.name_);
      valid = false;
    }
    if (interface.rx_.flow_isolation_ || interface.rx_.hardware_timestamps_ ||
        interface.rx_.dynamic_flow_capacity_ != 0 || !interface.rx_.flows_.empty() ||
        !interface.rx_.flex_items_.empty() || !interface.rx_.reorder_configs_.empty() ||
        !interface.tx_.flows_.empty() || interface.tx_.accurate_send_) {
      DAQIRI_LOG_ERROR(
          "PCIe interface '{}' contains unsupported flow/reorder/timestamp/offload "
          "configuration",
          interface.name_);
      valid = false;
    }

    std::unordered_set<uint16_t> rx_ids;
    std::unordered_set<uint16_t> tx_ids;
    auto validate_queue = [&](const auto& queue, bool tx) {
      const int queue_id = queue.common_.id_;
      if (queue_id < 0 || queue_id > UINT16_MAX) {
        DAQIRI_LOG_ERROR("PCIe {} queue '{}' ID must be in the range [0, {}]", tx ? "TX" : "RX",
                         queue.common_.name_, UINT16_MAX);
        valid = false;
        return;
      }
      auto& ids = tx ? tx_ids : rx_ids;
      if (!ids.emplace(queue_id).second) {
        DAQIRI_LOG_ERROR("PCIe {} queue ID {} is duplicated on interface '{}'", tx ? "TX" : "RX",
                         queue.common_.id_, interface.name_);
        valid = false;
      }
      if (queue.common_.batch_size_ <= 0 || queue.common_.mrs_.size() != 1 ||
          !queue.common_.offloads_.empty() || queue.common_.split_boundary_ != 0 ||
          queue.common_.extra_queue_config_ != nullptr) {
        DAQIRI_LOG_ERROR(
            "PCIe {} queue '{}' must use one MR, a positive batch size, and no HDS, "
            "offloads, or engine-specific queue data",
            tx ? "TX" : "RX", queue.common_.name_);
        valid = false;
        return;
      }
      if constexpr (std::is_same_v<std::decay_t<decltype(queue)>, TxQueueConfig>) {
        if (queue.pacing_mbps_ != 0) {
          DAQIRI_LOG_ERROR("PCIe TX queue '{}' does not support pacing", queue.common_.name_);
          valid = false;
        }
      }
      const std::string& mr_name = queue.common_.mrs_.front();
      auto mr = cfg_.mrs_.find(mr_name);
      if (mr == cfg_.mrs_.end()) {
        DAQIRI_LOG_ERROR("PCIe queue '{}' references missing MR '{}'", queue.common_.name_,
                         mr_name);
        valid = false;
        return;
      }
      if (mr->second.kind_ != MemoryKind::DEVICE || !mr->second.owned_ ||
          mr->second.buf_size_ == 0 || mr->second.num_bufs_ == 0 ||
          mr->second.num_bufs_ > UINT32_MAX || mr->second.adj_size_ > UINT32_MAX ||
          static_cast<size_t>(queue.common_.batch_size_) > mr->second.num_bufs_) {
        DAQIRI_LOG_ERROR("PCIe MR '{}' must be DAQIRI-owned device memory with valid fixed slots",
                         mr_name);
        valid = false;
      }
      if (!used_regions.emplace(mr_name).second) {
        DAQIRI_LOG_ERROR("PCIe MR '{}' is shared by multiple queues/directions/interfaces",
                         mr_name);
        valid = false;
      }
    };
    for (const auto& queue : interface.rx_.queues_) {
      validate_queue(queue, false);
    }
    for (const auto& queue : interface.tx_.queues_) {
      validate_queue(queue, true);
    }
  }
  return Engine::validate_config() && valid;
}

}  // namespace daqiri
