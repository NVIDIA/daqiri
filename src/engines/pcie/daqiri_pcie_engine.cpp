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

struct PcieEngine::InterfaceState {
  uint16_t port = 0;
  std::string name;
  std::string address;
  bool loopback = false;
  bool has_rx = false;
  bool has_tx = false;
  uint64_t epoch = 0;
  uint64_t next_sequence = 1;

  std::string rx_mr_name;
  std::string tx_mr_name;
  pcie::RegionRegistration rx_region{};
  pcie::RegionRegistration tx_region{};
  size_t rx_payload_size = 0;
  size_t tx_payload_size = 0;
  uint32_t rx_batch_size = 1;
  uint32_t tx_batch_size = 1;
  uint64_t rx_timeout_us = 0;
  int rx_worker_cpu = -1;
  int tx_worker_cpu = -1;
  int rx_gpu = -1;
  int tx_gpu = -1;
  bool rx_needs_cuda_flush = false;

  std::unique_ptr<pcie::Provider> provider;
  std::mutex provider_mutex;
  bool provider_started = false;
  bool quiesce_failed = false;
  std::vector<int> dmabuf_fds;

  std::mutex slot_mutex;
  std::vector<SlotOwner> rx_owners;
  std::vector<uint64_t> rx_expected_sequence;
  std::deque<uint32_t> deferred_rx_slots;
  std::vector<SlotOwner> tx_owners;
  std::vector<uint64_t> tx_expected_sequence;
  std::vector<uint32_t> tx_expected_length;
  std::deque<uint32_t> free_tx_slots;

  std::vector<daqiri_pcie_ring_entry> pending_rx;
  std::chrono::steady_clock::time_point pending_since{};
  std::mutex ready_mutex;
  std::deque<BurstParams*> ready_rx;

  std::atomic<bool> active{true};
  std::thread rx_worker;
  std::thread tx_worker;
};

struct PcieEngine::BurstStorage {
  InterfaceState* interface = nullptr;
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
    if (state->has_rx) {
      state->rx_worker = std::thread(&PcieEngine::rx_worker_loop, this, state.get());
    }
    if (state->has_tx) {
      state->tx_worker = std::thread(&PcieEngine::tx_worker_loop, this, state.get());
    }
  }
  init_rx_core_q_map();
  initialized_ = true;
  DAQIRI_LOG_INFO("PCIe stream initialized with {} FPGA interface(s)", interfaces_.size());
}

void PcieEngine::run() {}

bool PcieEngine::initialize_interface(InterfaceState& state, const InterfaceConfig& config) {
  state.has_rx = !config.rx_.queues_.empty();
  state.has_tx = !config.tx_.queues_.empty();
  if (state.has_rx) {
    const auto& queue = config.rx_.queues_.front();
    state.rx_mr_name = queue.common_.mrs_.front();
    state.rx_batch_size = static_cast<uint32_t>(queue.common_.batch_size_);
    state.rx_timeout_us = queue.timeout_us_;
    state.rx_worker_cpu = std::strtol(queue.common_.cpu_core_.c_str(), nullptr, 10);
  }
  if (state.has_tx) {
    const auto& queue = config.tx_.queues_.front();
    state.tx_mr_name = queue.common_.mrs_.front();
    state.tx_batch_size = static_cast<uint32_t>(queue.common_.batch_size_);
    state.tx_worker_cpu = std::strtol(queue.common_.cpu_core_.c_str(), nullptr, 10);
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
      static_cast<uint32_t>(state.has_rx) + static_cast<uint32_t>(state.has_tx);
  if ((caps.capabilities & DAQIRI_PCIE_CAP_DMA_FENCE) == 0 ||
      (!state.loopback && (caps.capabilities & DAQIRI_PCIE_CAP_DMABUF_PCIE) == 0)) {
    DAQIRI_LOG_ERROR("PCIe provider '{}' lacks a required DMA-BUF or DMA-fence capability",
                     state.name);
    return false;
  }
  if (caps.max_regions < region_count || caps.min_slot_alignment == 0 ||
      caps.min_slot_alignment > kPcieSlotAlignment ||
      (kPcieSlotAlignment % caps.min_slot_alignment) != 0) {
    DAQIRI_LOG_ERROR("PCIe provider '{}' cannot register {} regions with {}-byte slot alignment",
                     state.name, region_count, kPcieSlotAlignment);
    return false;
  }

  if (state.has_rx && !register_region(state, state.rx_mr_name, true)) {
    return false;
  }
  if (state.has_tx && !register_region(state, state.tx_mr_name, false)) {
    return false;
  }
  if (!initialize_cuda_ordering(state)) {
    return false;
  }

  pcie::QueueConfiguration queue_config{};
  queue_config.epoch = state.epoch;
  if (state.has_rx) {
    const uint32_t depth = ring_depth_for(state.rx_region.slot_count);
    if (depth == 0) {
      DAQIRI_LOG_ERROR("PCIe RX region for '{}' has too many slots for a v1 ring", state.name);
      return false;
    }
    queue_config.depths[DAQIRI_PCIE_RING_RX_AVAILABLE] = depth;
    queue_config.depths[DAQIRI_PCIE_RING_RX_COMPLETION] = depth;
  }
  if (state.has_tx) {
    const uint32_t depth = ring_depth_for(state.tx_region.slot_count);
    if (depth == 0) {
      DAQIRI_LOG_ERROR("PCIe TX region for '{}' has too many slots for a v1 ring", state.name);
      return false;
    }
    queue_config.depths[DAQIRI_PCIE_RING_TX_SUBMISSION] = depth;
    queue_config.depths[DAQIRI_PCIE_RING_TX_COMPLETION] = depth;
  }
  for (uint32_t depth : queue_config.depths) {
    if (depth != 0 && (depth > caps.max_ring_depth || depth < 2)) {
      DAQIRI_LOG_ERROR("PCIe ring depth {} is unsupported by provider '{}'", depth, state.name);
      return false;
    }
  }
  if (!state.provider->configure(queue_config)) {
    DAQIRI_LOG_ERROR("PCIe queue configuration failed for '{}': {}", state.name,
                     state.provider->last_error());
    return false;
  }
  if (!post_initial_rx_slots(state)) {
    return false;
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

bool PcieEngine::register_region(InterfaceState& state, const std::string& mr_name, bool rx) {
  auto config_it = cfg_.mrs_.find(mr_name);
  auto allocation_it = ar_.find(mr_name);
  if (config_it == cfg_.mrs_.end() || allocation_it == ar_.end()) {
    return false;
  }
  const auto& config = config_it->second;
  auto& allocation = allocation_it->second;

  pcie::RegionRegistration region{};
  region.direction = rx ? DAQIRI_PCIE_DIRECTION_RX : DAQIRI_PCIE_DIRECTION_TX;
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
    result = cuMemGetHandleForAddressRange(&fd, pointer, allocation.size_,
                                           CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD,
                                           CU_MEM_RANGE_FLAG_DMA_BUF_MAPPING_TYPE_PCIE);
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
  if (rx) {
    state.rx_region = region;
    state.rx_payload_size = config.buf_size_;
    state.rx_gpu = config.affinity_;
    state.rx_owners.assign(config.num_bufs_, SlotOwner::FREE);
    state.rx_expected_sequence.assign(config.num_bufs_, 0);
  } else {
    state.tx_region = region;
    state.tx_payload_size = config.buf_size_;
    state.tx_gpu = config.affinity_;
    state.tx_owners.assign(config.num_bufs_, SlotOwner::FREE);
    state.tx_expected_sequence.assign(config.num_bufs_, 0);
    state.tx_expected_length.assign(config.num_bufs_, 0);
    for (uint32_t slot = 0; slot < region.slot_count; ++slot) {
      state.free_tx_slots.push_back(slot);
    }
  }
  return true;
}

bool PcieEngine::initialize_cuda_ordering(InterfaceState& state) {
  if (!state.has_rx || state.loopback) {
    return true;
  }
  if (cudaSetDevice(state.rx_gpu) != cudaSuccess) {
    return false;
  }
  CUdevice device = 0;
  if (cuDeviceGet(&device, state.rx_gpu) != CUDA_SUCCESS) {
    return false;
  }
  int ordering = CU_GPU_DIRECT_RDMA_WRITES_ORDERING_NONE;
  CUresult result =
      cuDeviceGetAttribute(&ordering, CU_DEVICE_ATTRIBUTE_GPU_DIRECT_RDMA_WRITES_ORDERING, device);
  if (result != CUDA_SUCCESS) {
    DAQIRI_LOG_ERROR("Cannot query GPUDirect write ordering for GPU {}: {}", state.rx_gpu,
                     cuda_error(result));
    return false;
  }
  state.rx_needs_cuda_flush = ordering < CU_GPU_DIRECT_RDMA_WRITES_ORDERING_OWNER;
  if (!state.rx_needs_cuda_flush) {
    return true;
  }

  int flush_options = 0;
  result = cuDeviceGetAttribute(&flush_options,
                                CU_DEVICE_ATTRIBUTE_GPU_DIRECT_RDMA_FLUSH_WRITES_OPTIONS, device);
  if (result != CUDA_SUCCESS ||
      (flush_options & CU_FLUSH_GPU_DIRECT_RDMA_WRITES_OPTION_HOST) == 0) {
    DAQIRI_LOG_ERROR("GPU {} lacks native OWNER ordering and host GPUDirect write flush",
                     state.rx_gpu);
    return false;
  }
  return true;
}

bool PcieEngine::post_initial_rx_slots(InterfaceState& state) {
  if (!state.has_rx) {
    return true;
  }
  std::vector<daqiri_pcie_ring_entry> entries;
  entries.reserve(state.rx_region.slot_count);
  {
    std::lock_guard<std::mutex> lock(state.slot_mutex);
    for (uint32_t slot = 0; slot < state.rx_region.slot_count; ++slot) {
      const uint64_t sequence = state.next_sequence++;
      state.rx_expected_sequence[slot] = sequence;
      state.rx_owners[slot] = SlotOwner::DEVICE;
      entries.push_back(make_entry(state.epoch, sequence, state.rx_region.region_id, slot,
                                   static_cast<uint32_t>(state.rx_payload_size)));
    }
  }
  std::lock_guard<std::mutex> provider_lock(state.provider_mutex);
  if (!state.provider->post_rx_available(entries.data(), entries.size())) {
    DAQIRI_LOG_ERROR("Initial RX credit post failed for PCIe interface '{}'", state.name);
    return false;
  }
  return true;
}

bool PcieEngine::post_rx_slot(InterfaceState& state, uint32_t slot_id) {
  std::lock_guard<std::mutex> slot_lock(state.slot_mutex);
  if (slot_id >= state.rx_owners.size() || state.rx_owners[slot_id] != SlotOwner::APPLICATION) {
    return false;
  }
  if (!state.active.load(std::memory_order_acquire)) {
    state.rx_owners[slot_id] = SlotOwner::FREE;
    return true;
  }
  const uint64_t sequence = state.next_sequence++;
  state.rx_expected_sequence[slot_id] = sequence;
  state.rx_owners[slot_id] = SlotOwner::REPOST;
  const auto entry = make_entry(state.epoch, sequence, state.rx_region.region_id, slot_id,
                                static_cast<uint32_t>(state.rx_payload_size));
  std::lock_guard<std::mutex> provider_lock(state.provider_mutex);
  if (!state.active.load(std::memory_order_acquire)) {
    state.rx_owners[slot_id] = SlotOwner::FREE;
    return true;
  }
  if (!state.provider->post_rx_available(&entry, 1)) {
    state.deferred_rx_slots.push_back(slot_id);
    rx_backpressure_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  state.rx_owners[slot_id] = SlotOwner::DEVICE;
  return true;
}

void PcieEngine::retry_deferred_rx_slots(InterfaceState& state) {
  if (!state.active.load(std::memory_order_acquire)) {
    return;
  }
  std::lock_guard<std::mutex> slot_lock(state.slot_mutex);
  while (!state.deferred_rx_slots.empty()) {
    const uint32_t slot = state.deferred_rx_slots.front();
    if (slot >= state.rx_owners.size() || state.rx_owners[slot] != SlotOwner::REPOST) {
      mark_unhealthy(state, "deferred RX credit has invalid slot ownership");
      return;
    }
    const auto entry =
        make_entry(state.epoch, state.rx_expected_sequence[slot], state.rx_region.region_id, slot,
                   static_cast<uint32_t>(state.rx_payload_size));
    std::lock_guard<std::mutex> provider_lock(state.provider_mutex);
    if (!state.active.load(std::memory_order_acquire)) {
      return;
    }
    if (!state.provider->post_rx_available(&entry, 1)) {
      return;
    }
    state.rx_owners[slot] = SlotOwner::DEVICE;
    state.deferred_rx_slots.pop_front();
  }
}

bool PcieEngine::flush_remote_writes(InterfaceState& state) {
  if (!state.rx_needs_cuda_flush) {
    return true;
  }
  if (cudaSetDevice(state.rx_gpu) != cudaSuccess) {
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

void PcieEngine::rx_worker_loop(InterfaceState* state) {
  if (state == nullptr) {
    return;
  }
  if (state->rx_worker_cpu >= 0 && state->rx_worker_cpu < CPU_SETSIZE) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(state->rx_worker_cpu, &set);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
  }
  (void)cudaSetDevice(state->rx_gpu);

  while (running_.load(std::memory_order_acquire) && state->active.load()) {
    process_rx_completions(*state);
    if (!state->active.load(std::memory_order_acquire)) {
      break;
    }
    retry_deferred_rx_slots(*state);
    if (!provider_is_healthy(*state)) {
      break;
    }
    std::this_thread::yield();
  }
}

void PcieEngine::tx_worker_loop(InterfaceState* state) {
  if (state == nullptr) {
    return;
  }
  if (state->tx_worker_cpu >= 0 && state->tx_worker_cpu < CPU_SETSIZE) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(state->tx_worker_cpu, &set);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
  }
  (void)cudaSetDevice(state->tx_gpu);

  while (running_.load(std::memory_order_acquire) && state->active.load()) {
    process_tx_completions(*state);
    if (!provider_is_healthy(*state)) {
      break;
    }
    std::this_thread::yield();
  }
}

void PcieEngine::process_rx_completions(InterfaceState& state) {
  if (!state.has_rx) {
    return;
  }
  std::array<daqiri_pcie_ring_entry, kCompletionPollBatch> completions{};
  size_t count = 0;
  {
    std::lock_guard<std::mutex> provider_lock(state.provider_mutex);
    count = state.provider->poll_rx_completion(completions.data(), completions.size());
  }

  std::string failure;
  if (count != 0) {
    std::lock_guard<std::mutex> slot_lock(state.slot_mutex);
    for (size_t i = 0; i < count; ++i) {
      const auto& completion = completions[i];
      const uint32_t slot = completion_slot(completion);
      const uint32_t length = completion_length(completion);
      if (completion_status(completion) != DAQIRI_PCIE_COMPLETION_OK) {
        failure = "RX completion status " + std::to_string(completion_status(completion));
        break;
      }
      if (completion_epoch(completion) != state.epoch ||
          completion_region(completion) != state.rx_region.region_id ||
          slot >= state.rx_owners.size() || length > state.rx_payload_size ||
          state.rx_owners[slot] != SlotOwner::DEVICE ||
          completion_sequence(completion) != state.rx_expected_sequence[slot]) {
        failure = "stale, duplicate, or malformed RX completion";
        break;
      }
      state.rx_owners[slot] = SlotOwner::PENDING;
      if (state.pending_rx.empty()) {
        state.pending_since = std::chrono::steady_clock::now();
      }
      state.pending_rx.push_back(completion);
    }
  }
  if (!failure.empty()) {
    malformed_completions_.fetch_add(1, std::memory_order_relaxed);
    mark_unhealthy(state, failure);
    return;
  }

  while (state.pending_rx.size() >= state.rx_batch_size && state.active.load()) {
    if (!publish_rx_burst(state, state.rx_batch_size)) {
      return;
    }
  }
  if (!state.pending_rx.empty() && state.rx_timeout_us != 0) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - state.pending_since);
    if (elapsed.count() >= static_cast<int64_t>(state.rx_timeout_us)) {
      (void)publish_rx_burst(state, state.pending_rx.size());
    }
  }
}

bool PcieEngine::publish_rx_burst(InterfaceState& state, size_t count) {
  if (count == 0 || count > state.pending_rx.size()) {
    return false;
  }
  if (!flush_remote_writes(state)) {
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
  storage->interface = &state;
  storage->rx = true;
  storage->slots.reserve(count);
  storage->pointers.reserve(count);
  storage->lengths.reserve(count);
  storage->released.assign(count, 0);
  auto* base = static_cast<uint8_t*>(state.rx_region.gpu_base);
  {
    std::lock_guard<std::mutex> slot_lock(state.slot_mutex);
    for (size_t i = 0; i < count; ++i) {
      const uint32_t slot = completion_slot(state.pending_rx[i]);
      if (slot >= state.rx_owners.size() || state.rx_owners[slot] != SlotOwner::PENDING) {
        delete burst;
        delete storage;
        mark_unhealthy(state, "RX slot ownership changed before burst publication");
        return false;
      }
      state.rx_owners[slot] = SlotOwner::APPLICATION;
      storage->slots.push_back(slot);
      storage->pointers.push_back(base + static_cast<size_t>(slot) * state.rx_region.slot_stride);
      storage->lengths.push_back(completion_length(state.pending_rx[i]));
    }
  }
  state.pending_rx.erase(state.pending_rx.begin(), state.pending_rx.begin() + count);
  if (!state.pending_rx.empty()) {
    state.pending_since = std::chrono::steady_clock::now();
  }

  burst->custom_pkt_data = std::shared_ptr<void>(storage);
  burst->hdr.hdr.num_pkts = count;
  burst->hdr.hdr.port_id = state.port;
  burst->hdr.hdr.q_id = 0;
  burst->hdr.hdr.num_segs = 1;
  burst->hdr.hdr.max_pkt = state.rx_region.slot_count;
  burst->hdr.hdr.max_pkt_size = static_cast<uint32_t>(state.rx_payload_size);
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
    std::lock_guard<std::mutex> ready_lock(state.ready_mutex);
    state.ready_rx.push_back(burst);
  }
  rx_packets_.fetch_add(count, std::memory_order_relaxed);
  rx_bytes_.fetch_add(bytes, std::memory_order_relaxed);
  return true;
}

void PcieEngine::process_tx_completions(InterfaceState& state) {
  if (!state.has_tx) {
    return;
  }
  std::array<daqiri_pcie_ring_entry, kCompletionPollBatch> completions{};
  size_t count = 0;
  {
    std::lock_guard<std::mutex> provider_lock(state.provider_mutex);
    count = state.provider->poll_tx_completion(completions.data(), completions.size());
  }
  if (count == 0) {
    return;
  }

  std::string failure;
  {
    std::lock_guard<std::mutex> slot_lock(state.slot_mutex);
    for (size_t i = 0; i < count; ++i) {
      const auto& completion = completions[i];
      const uint32_t slot = completion_slot(completion);
      if (completion_status(completion) != DAQIRI_PCIE_COMPLETION_OK) {
        failure = "TX completion status " + std::to_string(completion_status(completion));
        break;
      }
      if (completion_epoch(completion) != state.epoch ||
          completion_region(completion) != state.tx_region.region_id ||
          slot >= state.tx_owners.size() || state.tx_owners[slot] != SlotOwner::DEVICE ||
          completion_sequence(completion) != state.tx_expected_sequence[slot] ||
          completion_length(completion) != state.tx_expected_length[slot]) {
        failure = "stale, duplicate, or malformed TX completion";
        break;
      }
      state.tx_owners[slot] = SlotOwner::FREE;
      state.free_tx_slots.push_back(slot);
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
  if (state == nullptr || !state->has_tx || !state->active.load() || burst->hdr.hdr.q_id != 0 ||
      burst->hdr.hdr.num_pkts == 0 || burst->hdr.hdr.num_pkts > state->tx_batch_size ||
      burst->hdr.hdr.num_pkts > state->tx_region.slot_count) {
    return false;
  }
  std::lock_guard<std::mutex> lock(state->slot_mutex);
  return state->free_tx_slots.size() >= burst->hdr.hdr.num_pkts;
}

Status PcieEngine::get_tx_packet_burst(BurstParams* burst) {
  if (burst == nullptr) {
    return Status::NULL_PTR;
  }
  if (!healthy_.load(std::memory_order_acquire) || !accepting_tx_.load(std::memory_order_acquire)) {
    return Status::INTERNAL_ERROR;
  }
  if (burst_storage(burst) != nullptr || burst->hdr.hdr.num_segs != 1 || burst->hdr.hdr.q_id != 0 ||
      burst->hdr.hdr.num_pkts == 0) {
    return Status::INVALID_PARAMETER;
  }
  auto* state = find_interface(burst->hdr.hdr.port_id);
  if (state == nullptr || !state->has_tx || !state->active.load(std::memory_order_acquire) ||
      burst->hdr.hdr.num_pkts > state->tx_region.slot_count ||
      burst->hdr.hdr.num_pkts > state->tx_batch_size) {
    return Status::INVALID_PARAMETER;
  }

  auto* storage = new (std::nothrow) BurstStorage{};
  if (storage == nullptr) {
    return Status::NO_FREE_BURST_BUFFERS;
  }
  storage->interface = state;
  storage->rx = false;
  const size_t count = burst->hdr.hdr.num_pkts;
  storage->slots.reserve(count);
  storage->pointers.reserve(count);
  storage->lengths.assign(count, 0);
  storage->released.assign(count, 0);
  {
    std::lock_guard<std::mutex> lock(state->slot_mutex);
    if (!accepting_tx_.load(std::memory_order_acquire) ||
        !state->active.load(std::memory_order_acquire)) {
      delete storage;
      return Status::INTERNAL_ERROR;
    }
    if (state->free_tx_slots.size() < count) {
      delete storage;
      tx_backpressure_.fetch_add(1, std::memory_order_relaxed);
      return Status::NO_FREE_PACKET_BUFFERS;
    }
    auto* base = static_cast<uint8_t*>(state->tx_region.gpu_base);
    for (size_t i = 0; i < count; ++i) {
      const uint32_t slot = state->free_tx_slots.front();
      state->free_tx_slots.pop_front();
      state->tx_owners[slot] = SlotOwner::APPLICATION;
      storage->slots.push_back(slot);
      storage->pointers.push_back(base + static_cast<size_t>(slot) * state->tx_region.slot_stride);
    }
  }

  burst->custom_pkt_data = std::shared_ptr<void>(storage);
  burst->pkts[0] = storage->pointers.data();
  burst->pkt_lens[0] = storage->lengths.data();
  burst->hdr.hdr.max_pkt = state->tx_region.slot_count;
  burst->hdr.hdr.max_pkt_size = static_cast<uint32_t>(state->tx_payload_size);
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
  const size_t maximum =
      storage->rx ? storage->interface->rx_payload_size : storage->interface->tx_payload_size;
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
  if (storage == nullptr || storage->interface == nullptr || pkt < 0 ||
      pkt >= static_cast<int>(storage->slots.size())) {
    return false;
  }
  std::lock_guard<std::mutex> storage_lock(storage->mutex);
  if (storage->released[pkt] != 0 || storage->submitted) {
    return false;
  }
  InterfaceState& state = *storage->interface;
  const uint32_t slot = storage->slots[pkt];
  if (storage->rx) {
    if (!post_rx_slot(state, slot)) {
      mark_unhealthy(state, "RX credit ring rejected a returned application slot");
      return false;
    }
  } else {
    std::lock_guard<std::mutex> slot_lock(state.slot_mutex);
    if (slot >= state.tx_owners.size() || state.tx_owners[slot] != SlotOwner::APPLICATION) {
      return false;
    }
    state.tx_owners[slot] = SlotOwner::FREE;
    state.free_tx_slots.push_back(slot);
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
  if (burst == nullptr || storage == nullptr || storage->rx || storage->interface == nullptr) {
    return Status::INVALID_PARAMETER;
  }
  InterfaceState& state = *storage->interface;
  if (!accepting_tx_.load(std::memory_order_acquire) || !state.active.load()) {
    return Status::INTERNAL_ERROR;
  }

  std::vector<daqiri_pcie_ring_entry> entries;
  entries.reserve(storage->slots.size());
  uint64_t bytes = 0;
  {
    std::lock_guard<std::mutex> storage_lock(storage->mutex);
    std::lock_guard<std::mutex> slot_lock(state.slot_mutex);
    for (size_t i = 0; i < storage->slots.size(); ++i) {
      const uint32_t slot = storage->slots[i];
      if (storage->released[i] != 0 || slot >= state.tx_owners.size() ||
          state.tx_owners[slot] != SlotOwner::APPLICATION ||
          storage->lengths[i] > state.tx_payload_size) {
        return Status::INVALID_PARAMETER;
      }
    }
    for (size_t i = 0; i < storage->slots.size(); ++i) {
      const uint32_t slot = storage->slots[i];
      const uint32_t length = storage->lengths[i];
      const uint64_t sequence = state.next_sequence++;
      state.tx_expected_sequence[slot] = sequence;
      state.tx_expected_length[slot] = length;
      state.tx_owners[slot] = SlotOwner::DEVICE;
      entries.push_back(make_entry(state.epoch, sequence, state.tx_region.region_id, slot, length));
      bytes += length;
    }
    std::lock_guard<std::mutex> provider_lock(state.provider_mutex);
    if (!accepting_tx_.load(std::memory_order_acquire) ||
        !state.active.load(std::memory_order_acquire) ||
        !state.provider->post_tx_submission(entries.data(), entries.size())) {
      for (uint32_t slot : storage->slots) {
        state.tx_owners[slot] = SlotOwner::APPLICATION;
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
  if (state == nullptr || !state->has_rx || q != 0) {
    return Status::INVALID_PARAMETER;
  }
  std::lock_guard<std::mutex> lock(state->ready_mutex);
  if (state->ready_rx.empty()) {
    return Status::NULL_PTR;
  }
  *burst = state->ready_rx.front();
  state->ready_rx.pop_front();
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
      std::lock_guard<std::mutex> slot_lock(state->slot_mutex);
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
    if (state != nullptr && state->rx_worker.joinable()) {
      state->rx_worker.join();
    }
    if (state != nullptr && state->tx_worker.joinable()) {
      state->tx_worker.join();
    }
  }

  for (auto& state : interfaces_) {
    if (state == nullptr) {
      continue;
    }
    {
      std::lock_guard<std::mutex> ready_lock(state->ready_mutex);
      while (!state->ready_rx.empty()) {
        delete_burst(state->ready_rx.front());
        state->ready_rx.pop_front();
      }
    }
    state->pending_rx.clear();
    if (state->quiesce_failed) {
      if (state->has_rx) {
        ar_[state->rx_mr_name].deallocator_ = AllocRegion::Deallocator::NONE;
      }
      if (state->has_tx) {
        ar_[state->tx_mr_name].deallocator_ = AllocRegion::Deallocator::NONE;
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
    if (interface.rx_.queues_.size() > 1 || interface.tx_.queues_.size() > 1) {
      DAQIRI_LOG_ERROR("PCIe interface '{}' supports only one RX and one TX queue",
                       interface.name_);
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

    auto validate_queue = [&](const auto& queue, bool tx) {
      if (queue.common_.id_ != 0 || queue.common_.batch_size_ <= 0 ||
          queue.common_.mrs_.size() != 1 || !queue.common_.offloads_.empty() ||
          queue.common_.split_boundary_ != 0 || queue.common_.extra_queue_config_ != nullptr) {
        DAQIRI_LOG_ERROR(
            "PCIe {} queue '{}' must use id 0, one MR, a positive batch size, and "
            "no HDS, offloads, or engine-specific queue data",
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
        DAQIRI_LOG_ERROR("PCIe MR '{}' is shared by multiple directions/interfaces", mr_name);
        valid = false;
      }
    };
    if (!interface.rx_.queues_.empty()) {
      validate_queue(interface.rx_.queues_.front(), false);
    }
    if (!interface.tx_.queues_.empty()) {
      validate_queue(interface.tx_.queues_.front(), true);
    }
  }
  return Engine::validate_config() && valid;
}

}  // namespace daqiri
