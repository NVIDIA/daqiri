/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/engines/gpunetio/daqiri_gpunetio_engine.h"

#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_eth_rxq.h>
#include <doca_eth_rxq_gpu_data_path.h>
#include <doca_eth_txq.h>
#include <doca_eth_txq_gpu_data_path.h>
#include <doca_flow.h>
#include <doca_gpunetio.h>
#include <doca_gpunetio_eth_def.h>
#include <doca_log.h>
#include <doca_mmap.h>

#include <arpa/inet.h>
#include <cuda_runtime.h>
#include <endian.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <thread>

#include "src/daqiri_ring.h"
#include "src/engines/gpunetio/daqiri_gpunetio_kernels.h"
#include <daqiri/logging.hpp>

namespace daqiri {

namespace {

// DOCA GPU receive rings hold a multiple of 512 packets; the engine uses a power of two
constexpr uint64_t RX_RING_MIN_PKTS = 512;
// Up to this size DOCA receives into a striding queue, whose stride is a power of two
constexpr uint32_t RX_STRIDING_MAX_PKT_SIZE = 8192;
// Packets per receive call of the RX kernel
constexpr uint32_t RX_MAX_PKTS_PER_CALL = 4096;
// Bursts in flight on a persistent TX kernel
constexpr uint32_t TX_DESC_RING = 1024;
// Kernel launches queued on a per-burst TX queue
constexpr uint32_t TX_LAUNCHES_IN_FLIGHT = 64;
constexpr uint32_t TX_SEND_RING = 4096;
constexpr uint64_t TX_SQ_MIN_WQES = 1024;
constexpr uint64_t TX_SQ_MAX_WQES = 32768;
constexpr auto KERNEL_STOP_TIMEOUT = std::chrono::seconds(5);

constexpr uint32_t GPUNETIO_RX_BURST_FLAG = 1u << 25;
constexpr uint32_t GPUNETIO_TX_BURST_FLAG = 1u << 26;
constexpr uint32_t BURST_MAGIC = 0x6770756eu;

// Engine data of a burst, kept in its custom header area
struct BurstPriv {
  uint64_t seq;  // RX: descriptor index; TX: first TX slot
  uint32_t magic;
  uint32_t reserved;  // TX: number of slots reserved from seq and not retired yet
};
static_assert(sizeof(BurstPriv) <= sizeof(BurstHeader::custom_burst_data),
              "BurstPriv must fit in BurstHeader::custom_burst_data");

void set_priv(BurstParams* burst, uint64_t seq, uint32_t reserved = 0) {
  const BurstPriv priv{seq, BURST_MAGIC, reserved};
  std::memcpy(burst->hdr.custom_burst_data, &priv, sizeof(priv));
}

bool get_priv(const BurstParams* burst, BurstPriv* priv) {
  std::memcpy(priv, burst->hdr.custom_burst_data, sizeof(*priv));
  return priv->magic == BURST_MAGIC;
}

uint64_t next_pow2(uint64_t value) {
  uint64_t pow = 1;
  while (pow < value) {
    pow <<= 1;
  }
  return pow;
}

uint64_t round_up(uint64_t value, uint64_t align) {
  return (value + align - 1) / align * align;
}

void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#endif
}

int parse_core(const std::string& core) {
  if (core.empty()) {
    return -1;
  }
  return static_cast<int>(std::strtol(core.c_str(), nullptr, 10));
}

void pin_thread(int core, const std::string& name) {
  if (core < 0) {
    return;
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
    DAQIRI_LOG_WARN("Could not pin the {} thread to core {}", name, core);
  }
}

// Counters and flags in pinned host memory, shared with the kernels
template <typename T>
T load_acquire(const T* ptr) {
  return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

template <typename T>
void store_release(T* ptr, T value) {
  __atomic_store_n(ptr, value, __ATOMIC_RELEASE);
}

// Pinned host memory, mapped in the address space of every GPU
template <typename T>
T* alloc_pinned(size_t count) {
  void* ptr = nullptr;
  if (cudaHostAlloc(&ptr, count * sizeof(T), cudaHostAllocMapped | cudaHostAllocPortable) !=
      cudaSuccess) {
    return nullptr;
  }
  std::memset(ptr, 0, count * sizeof(T));
  return static_cast<T*>(ptr);
}

const char* kernel_error_str(uint32_t error) {
  switch (error) {
    case gpunetio::KERNEL_RX_ERROR_CQE:
      return "receive completion with error";
    case gpunetio::KERNEL_RX_OUT_OF_ORDER:
      return "packets out of the receive ring order (packet bigger than the receive stride?)";
    case gpunetio::KERNEL_TX_ERROR_CQE:
      return "send completion with error";
    default:
      return "unknown error";
  }
}

bool wait_stream(cudaStream_t stream) {
  const auto t0 = std::chrono::steady_clock::now();
  while (cudaStreamQuery(stream) == cudaErrorNotReady) {
    if (std::chrono::steady_clock::now() - t0 > KERNEL_STOP_TIMEOUT) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

// Launches a resident kernel on a new stream, one block per queue. The kernel reads the arguments
// of its blocks from pinned host memory.
template <typename Args, typename Launch>
bool launch_resident_kernel(const char* name, int ordinal, const std::vector<Args>& blocks,
                            Args** args, cudaStream_t* stream, Launch launch) {
  if (blocks.empty()) {
    return true;
  }
  *args = alloc_pinned<Args>(blocks.size());
  if (*args == nullptr || cudaStreamCreateWithFlags(stream, cudaStreamNonBlocking) != cudaSuccess) {
    DAQIRI_LOG_CRITICAL("GPU {}: could not allocate the {} kernel resources", ordinal, name);
    return false;
  }
  std::copy(blocks.begin(), blocks.end(), *args);
  const cudaError_t err = launch(*args, static_cast<uint32_t>(blocks.size()), *stream);
  if (err != cudaSuccess) {
    DAQIRI_LOG_CRITICAL("GPU {}: {} kernel launch for {} queue(s) failed: {}", ordinal, name,
                        blocks.size(), cudaGetErrorString(err));
    return false;
  }
  DAQIRI_LOG_INFO("GPU {}: {} kernel running with {} block(s), one per queue", ordinal, name,
                  blocks.size());
  return true;
}

uint32_t doca_sdk_log_level(LogLevel::Level level) {
  switch (level) {
    case LogLevel::TRACE:
      return DOCA_LOG_LEVEL_TRACE;
    case LogLevel::DEBUG:
      return DOCA_LOG_LEVEL_DEBUG;
    case LogLevel::ERROR:
      return DOCA_LOG_LEVEL_ERROR;
    case LogLevel::CRITICAL:
      return DOCA_LOG_LEVEL_CRIT;
    case LogLevel::OFF:
      return DOCA_LOG_LEVEL_DISABLE;
    default:
      // DOCA prints its own setup details at info level
      return DOCA_LOG_LEVEL_WARNING;
  }
}

// The DOCA SDK explains most failures only in its own log, which stays silent until a backend
// is registered. Backends live until the process exits, so the stderr backend is created once
// and its level follows the configuration of the last initialization.
void set_doca_sdk_log_level(LogLevel::Level level) {
  static std::mutex mutex;
  static struct doca_log_backend* backend = nullptr;
  std::lock_guard<std::mutex> lock(mutex);

  if (backend == nullptr) {
    const doca_error_t ret = doca_log_backend_create_with_file_sdk(stderr, &backend);
    if (ret != DOCA_SUCCESS) {
      backend = nullptr;
      DAQIRI_LOG_WARN("Could not route DOCA SDK logs to stderr: {}", doca_error_get_descr(ret));
      return;
    }
  }
  const uint32_t doca_level = doca_sdk_log_level(level);
  (void)doca_log_level_set_global_sdk_limit(doca_level);
  (void)doca_log_backend_set_sdk_level(backend, doca_level);
}

bool is_pci_addr(const std::string& key) {
  return key.find(':') != std::string::npos && key.find('.') != std::string::npos;
}

// Matches a PCIe address ("0000:3b:00.0" or "3b:00.0"), an IB device name ("mlx5_0") or a
// netdev name
bool devinfo_matches(struct doca_devinfo* devinfo, const std::string& key) {
  if (key.empty()) {
    return false;
  }
  if (is_pci_addr(key)) {
    uint8_t is_equal = 0;
    return doca_devinfo_is_equal_pci_addr(devinfo, key.c_str(), &is_equal) == DOCA_SUCCESS &&
           is_equal != 0;
  }
  char name[DOCA_DEVINFO_IFACE_NAME_SIZE] = {};
  if (doca_devinfo_get_ibdev_name(devinfo, name, DOCA_DEVINFO_IBDEV_NAME_SIZE) == DOCA_SUCCESS &&
      key == name) {
    return true;
  }
  return doca_devinfo_get_iface_name(devinfo, name, sizeof(name)) == DOCA_SUCCESS && key == name;
}

// RX queues a flow steers to: the queue action ends the flow
std::vector<uint16_t> flow_queues(const FlowConfig& flow) {
  const FlowAction& action = flow.actions_.empty() ? flow.action_ : flow.actions_.back();
  if (!action.ids_.empty()) {
    return action.ids_;
  }
  return {action.id_};
}

}  // namespace

// NIC port, shared by the interfaces configured on it
struct GpunetioEngine::Device {
  struct doca_dev* dev = nullptr;
  std::string pci_addr;
  std::string ibdev_name;
  std::string iface_name;
  struct doca_flow_port* flow_port = nullptr;
  std::vector<struct doca_flow_pipe*> pipes;  // Destroyed in reverse order of creation
  int rx_port = -1;                           // Interface with RX queues on this NIC
  int tx_port = -1;                           // Interface with TX queues on this NIC
};

// GPU running the datapath. One resident RX kernel serves all the RX queues of the GPU, and one
// resident TX kernel its persistent TX queues, one block per queue. Each kernel has its own stream:
// on a shared stream, the second kernel would wait for the first to exit.
struct GpunetioEngine::Gpu {
  int ordinal = -1;
  int cc_major = 0;
  struct doca_gpu* gpu = nullptr;

  gpunetio::RxKernelArgs* rx_args = nullptr;  // Pinned host memory, one entry per block
  gpunetio::TxKernelArgs* tx_args = nullptr;
  cudaStream_t rx_stream = nullptr;
  cudaStream_t tx_stream = nullptr;
};

// Registration of a memory region with the NICs of the queues using it
struct GpunetioEngine::MemoryMap {
  struct doca_mmap* mmap = nullptr;
  int dmabuf_fd = -1;
  std::vector<Device*> devices;
  Gpu* gpu = nullptr;
};

struct GpunetioEngine::RxQueue {
  // Configuration
  int port = 0;
  int id = 0;
  std::string name;
  std::string mr_name;
  int cpu_core = -1;
  uint32_t batch_size = 0;
  uint64_t timeout_ns = 0;
  Device* device = nullptr;
  Gpu* gpu = nullptr;

  // Receive ring in the memory region: ring_size slots of stride bytes
  struct doca_eth_rxq* rxq = nullptr;
  bool ctx_started = false;
  struct doca_gpu_eth_rxq* gpu_rxq = nullptr;
  uint32_t stride = 0;
  uint64_t ring_size = 0;
  uint32_t buf_size = 0;
  std::vector<void*> pkt_ptrs;

  // Shared with the RX kernel
  gpunetio::RxControl* ctrl = nullptr;
  gpunetio::RxBurstDesc* desc = nullptr;  // ring_size entries
  uint32_t* pkt_len = nullptr;            // ring_size entries
  gpunetio::RxKernelArgs kernel_args{};   // The queue's block in the RX kernel of its GPU
  bool kernel_stuck = false;

  // Burst bookkeeping. Descriptors are released in order: the worker advances the release
  // cursor over the descriptors whose packets the application freed.
  daqiri::Ring* ring = nullptr;  // Bursts ready for the application
  std::unique_ptr<uint32_t[]> desc_pkts;
  std::unique_ptr<std::atomic<uint32_t>[]> desc_freed;
  uint64_t consumed = 0;
  uint64_t release_cursor = 0;
  uint64_t released_pkts = 0;
  bool error_logged = false;
  std::thread worker;

  std::atomic<uint64_t> bursts{0};
  std::atomic<uint64_t> pkts{0};
  std::atomic<uint64_t> bytes{0};
  std::atomic<uint64_t> pool_empty{0};
  std::atomic<uint64_t> ring_full{0};
};

struct GpunetioEngine::TxQueue {
  // Configuration
  int port = 0;
  int id = 0;
  std::string name;
  std::string mr_name;
  int cpu_core = -1;
  uint32_t batch_size = 0;
  GpunetioTxKernel kernel = GpunetioTxKernel::PERSISTENT;
  Device* device = nullptr;
  Gpu* gpu = nullptr;

  // Send queue and the TX slots of the memory region
  struct doca_eth_txq* txq = nullptr;
  bool ctx_started = false;
  struct doca_gpu_eth_txq* gpu_txq = nullptr;
  uint32_t mkey = 0;  // Network byte order
  uint8_t* slot_base = nullptr;
  size_t slot_size = 0;
  uint64_t num_slots = 0;
  uint32_t sq_wqes = 0;
  uint32_t max_chunk = 0;

  // Slots are cyclic: the application thread allocates at alloc_head. A burst's range of slots is
  // retired when the NIC completes it or when the application frees it unsent, in any order;
  // completed_tail covers the slots before the first range still in use.
  uint64_t alloc_head = 0;
  std::atomic<uint64_t> completed_tail{0};
  std::mutex retire_mutex;
  std::map<uint64_t, uint64_t> retired;  // First slot to count, for ranges past completed_tail

  void retire(uint64_t first, uint64_t count) {
    if (count == 0) {
      return;
    }
    std::lock_guard<std::mutex> lock(retire_mutex);
    uint64_t tail = completed_tail.load(std::memory_order_relaxed);
    retired[first] = count;
    for (auto it = retired.begin(); it != retired.end() && it->first == tail;
         it = retired.erase(it)) {
      tail += it->second;
    }
    completed_tail.store(tail, std::memory_order_release);
  }

  // Shared with the TX kernels
  gpunetio::TxPacket* pkts = nullptr;  // num_slots entries
  gpunetio::TxBurstDesc* desc = nullptr;
  gpunetio::TxControl* ctrl = nullptr;
  gpunetio::TxState* state = nullptr;
  // The queue's block in the persistent TX kernel of its GPU, or its per-burst launches
  gpunetio::TxKernelArgs kernel_args{};
  cudaStream_t stream = nullptr;  // per_burst only
  bool kernel_stuck = false;

  daqiri::Ring* send_ring = nullptr;  // Bursts sent by the application
  std::deque<BurstParams*> inflight;
  uint64_t published = 0;
  uint64_t reaped = 0;
  std::atomic<bool> failed{false};
  std::thread worker;

  std::atomic<uint64_t> sent_bursts{0};
  std::atomic<uint64_t> sent_pkts{0};
  std::atomic<uint64_t> sent_bytes{0};
  std::atomic<uint64_t> send_ring_full{0};
};

GpunetioEngine::GpunetioEngine() = default;

GpunetioEngine::~GpunetioEngine() {
  shutdown();
}

// ---------------------------------------------------------------------------
// Bring-up
// ---------------------------------------------------------------------------
bool GpunetioEngine::set_config_and_initialize(const NetworkConfig& cfg) {
  if (initialized_) {
    DAQIRI_LOG_ERROR("gpunetio engine is already initialized; call shutdown() first");
    return false;
  }

  cfg_ = cfg;
  if (!validate_config()) {
    DAQIRI_LOG_CRITICAL("Config validation failed");
    return false;
  }
  initialize();
  if (!initialized_) {
    shutdown();
  }
  return initialized_;
}

void GpunetioEngine::initialize() {
  DAQIRI_LOG_INFO("Initializing GPUNetIO raw backend (experimental)");
  set_doca_sdk_log_level(cfg_.log_level_);

  int if_num = 0;
  for (auto& intf : cfg_.ifs_) {
    intf.port_id_ = if_num++;
  }

  for (const auto& intf : cfg_.ifs_) {
    for (const auto& cfg_q : intf.rx_.queues_) {
      auto q = std::make_unique<RxQueue>();
      q->port = intf.port_id_;
      q->id = cfg_q.common_.id_;
      q->name = cfg_q.common_.name_;
      q->mr_name = cfg_q.common_.mrs_[0];
      q->cpu_core = parse_core(cfg_q.common_.cpu_core_);
      q->batch_size = static_cast<uint32_t>(cfg_q.common_.batch_size_);
      q->timeout_ns = cfg_q.timeout_us_ * 1000;
      rx_queues_.push_back(std::move(q));
    }
    for (const auto& cfg_q : intf.tx_.queues_) {
      auto q = std::make_unique<TxQueue>();
      q->port = intf.port_id_;
      q->id = cfg_q.common_.id_;
      q->name = cfg_q.common_.name_;
      q->mr_name = cfg_q.common_.mrs_[0];
      q->cpu_core = parse_core(cfg_q.common_.cpu_core_);
      q->batch_size = static_cast<uint32_t>(cfg_q.common_.batch_size_);
      q->kernel = cfg_q.gpunetio_tx_kernel_;
      tx_queues_.push_back(std::move(q));
    }
  }

  if (!open_devices() || !size_memory_regions()) {
    return;
  }
  if (allocate_memory_regions() != Status::SUCCESS) {
    DAQIRI_LOG_CRITICAL("Failed to allocate memory regions");
    return;
  }

  for (auto& q : rx_queues_) {
    q->gpu = get_gpu(cfg_.mrs_.at(q->mr_name).affinity_);
    if (q->gpu == nullptr) {
      return;
    }
  }
  for (auto& q : tx_queues_) {
    q->gpu = get_gpu(cfg_.mrs_.at(q->mr_name).affinity_);
    if (q->gpu == nullptr) {
      return;
    }
  }

  if (!map_memory_regions() || !start_flow()) {
    return;
  }
  for (auto& q : rx_queues_) {
    if (!create_rx_queue(*q)) {
      return;
    }
  }
  for (auto& q : tx_queues_) {
    if (!create_tx_queue(*q)) {
      return;
    }
  }
  for (auto& dev : devices_) {
    if (dev->rx_port >= 0 && !program_steering(*dev, cfg_.ifs_[dev->rx_port])) {
      return;
    }
  }
  if (!create_burst_pools() || !start_kernels()) {
    return;
  }

  initialized_ = true;
  DAQIRI_LOG_INFO("gpunetio backend initialized with {} RX queue(s), {} TX queue(s)",
                  rx_queues_.size(), tx_queues_.size());

  // Engines self-start their workers at the end of initialize()
  run();
}

// Resolves every interface address, then name, to a DOCA device. Interfaces on the same NIC
// share one device; only one of them may have RX queues, and only one TX queues.
bool GpunetioEngine::open_devices() {
  struct doca_devinfo** list = nullptr;
  uint32_t num = 0;
  doca_error_t ret = doca_devinfo_create_list(&list, &num);
  if (ret != DOCA_SUCCESS) {
    DAQIRI_LOG_CRITICAL("Failed to list DOCA devices: {}", doca_error_get_descr(ret));
    return false;
  }

  bool ok = true;
  port_devices_.assign(cfg_.ifs_.size(), nullptr);
  for (const auto& intf : cfg_.ifs_) {
    struct doca_devinfo* match = nullptr;
    for (const std::string* key : {&intf.address_, &intf.name_}) {
      for (uint32_t i = 0; i < num && match == nullptr; i++) {
        if (devinfo_matches(list[i], *key)) {
          match = list[i];
        }
      }
      if (match != nullptr) {
        break;
      }
    }
    if (match == nullptr) {
      DAQIRI_LOG_CRITICAL(
          "Interface '{}' (address '{}') matches no DOCA device; use a PCIe address, an IB "
          "device name or a netdev name",
          intf.name_, intf.address_);
      ok = false;
      break;
    }

    char pci_addr[DOCA_DEVINFO_PCI_ADDR_SIZE] = {};
    (void)doca_devinfo_get_pci_addr_str(match, pci_addr);
    Device* device = nullptr;
    for (auto& dev : devices_) {
      if (dev->pci_addr == pci_addr) {
        device = dev.get();
      }
    }
    if (device == nullptr) {
      auto dev = std::make_unique<Device>();
      dev->pci_addr = pci_addr;
      ret = doca_dev_open(match, &dev->dev);
      if (ret != DOCA_SUCCESS) {
        dev->dev = nullptr;
        DAQIRI_LOG_CRITICAL("Failed to open DOCA device {}: {}", pci_addr,
                            doca_error_get_descr(ret));
        ok = false;
        break;
      }
      char name[DOCA_DEVINFO_IFACE_NAME_SIZE] = {};
      if (doca_devinfo_get_ibdev_name(match, name, DOCA_DEVINFO_IBDEV_NAME_SIZE) == DOCA_SUCCESS) {
        dev->ibdev_name = name;
      }
      if (doca_devinfo_get_iface_name(match, name, sizeof(name)) == DOCA_SUCCESS) {
        dev->iface_name = name;
      }
      device = dev.get();
      devices_.push_back(std::move(dev));
    }
    port_devices_[intf.port_id_] = device;

    struct doca_devinfo* devinfo = doca_dev_as_devinfo(device->dev);
    if (!intf.rx_.queues_.empty()) {
      if (device->rx_port >= 0) {
        DAQIRI_LOG_CRITICAL(
            "Interfaces '{}' and '{}' both define RX queues on NIC {}; only one interface per "
            "NIC may",
            cfg_.ifs_[device->rx_port].name_, intf.name_, device->pci_addr);
        ok = false;
        break;
      }
      device->rx_port = intf.port_id_;
      ret = doca_eth_rxq_cap_is_type_supported(devinfo, DOCA_ETH_RXQ_TYPE_CYCLIC,
                                               DOCA_ETH_RXQ_DATA_PATH_TYPE_GPU);
      if (ret != DOCA_SUCCESS) {
        DAQIRI_LOG_CRITICAL("DOCA device {} has no cyclic RX queue on the GPU datapath: {}",
                            device->pci_addr, doca_error_get_descr(ret));
        ok = false;
        break;
      }
    }
    if (!intf.tx_.queues_.empty()) {
      if (device->tx_port >= 0) {
        DAQIRI_LOG_CRITICAL(
            "Interfaces '{}' and '{}' both define TX queues on NIC {}; only one interface per "
            "NIC may",
            cfg_.ifs_[device->tx_port].name_, intf.name_, device->pci_addr);
        ok = false;
        break;
      }
      device->tx_port = intf.port_id_;
      ret = doca_eth_txq_cap_is_type_supported(devinfo, DOCA_ETH_TXQ_TYPE_REGULAR,
                                               DOCA_ETH_TXQ_DATA_PATH_TYPE_GPU);
      if (ret != DOCA_SUCCESS) {
        DAQIRI_LOG_CRITICAL("DOCA device {} has no TX queue on the GPU datapath: {}",
                            device->pci_addr, doca_error_get_descr(ret));
        ok = false;
        break;
      }
    }
    DAQIRI_LOG_INFO("Interface '{}' uses DOCA device {} ({}, {})", intf.name_, device->ibdev_name,
                    device->pci_addr, device->iface_name);
  }
  doca_devinfo_destroy_list(list);
  if (!ok) {
    return false;
  }

  for (auto& q : rx_queues_) {
    q->device = port_devices_[q->port];
  }
  for (auto& q : tx_queues_) {
    q->device = port_devices_[q->port];
  }
  return true;
}

GpunetioEngine::Gpu* GpunetioEngine::get_gpu(int ordinal) {
  const auto it = gpus_.find(ordinal);
  if (it != gpus_.end()) {
    return it->second.get();
  }

  if (!select_cuda_device(ordinal, "creating the DOCA GPU handle")) {
    return nullptr;
  }
  if (cudaFree(0) != cudaSuccess) {
    DAQIRI_LOG_CRITICAL("Could not initialize CUDA device {}", ordinal);
    return nullptr;
  }
  char bus_id[32] = {};
  if (cudaDeviceGetPCIBusId(bus_id, sizeof(bus_id), ordinal) != cudaSuccess) {
    DAQIRI_LOG_CRITICAL("Could not get the PCIe address of CUDA device {}", ordinal);
    return nullptr;
  }

  auto gpu = std::make_unique<Gpu>();
  gpu->ordinal = ordinal;
  (void)cudaDeviceGetAttribute(&gpu->cc_major, cudaDevAttrComputeCapabilityMajor, ordinal);
  const doca_error_t ret = doca_gpu_create(bus_id, &gpu->gpu);
  if (ret != DOCA_SUCCESS) {
    DAQIRI_LOG_CRITICAL("Failed to create the DOCA GPU handle of {}: {}", bus_id,
                        doca_error_get_descr(ret));
    return nullptr;
  }
  DAQIRI_LOG_INFO("CUDA device {} ({}) runs the gpunetio datapath", ordinal, bus_id);
  Gpu* ptr = gpu.get();
  gpus_[ordinal] = std::move(gpu);
  return ptr;
}

// The region of an RX queue becomes the receive ring: DOCA needs a power-of-two stride up to
// 8 kB, and a ring of a power-of-two number of slots, at least 512.
bool GpunetioEngine::size_memory_regions() {
  for (auto& [name, mr] : cfg_.mrs_) {
    mr.adj_size_ = round_up(std::max<size_t>(mr.buf_size_, 1), 64);
  }

  for (auto& q : rx_queues_) {
    auto& mr = cfg_.mrs_.at(q->mr_name);
    const size_t buf = std::max<size_t>(mr.buf_size_, 64);
    q->stride =
        static_cast<uint32_t>(buf <= RX_STRIDING_MAX_PKT_SIZE ? next_pow2(buf) : round_up(buf, 64));
    q->ring_size =
        next_pow2(std::max<uint64_t>({static_cast<uint64_t>(mr.num_bufs_), RX_RING_MIN_PKTS,
                                      static_cast<uint64_t>(q->batch_size)}));
    if (q->ring_size > UINT32_MAX) {
      DAQIRI_LOG_CRITICAL("RX queue '{}' ring of {} packets is too large", q->name, q->ring_size);
      return false;
    }

    const doca_error_t ret = doca_eth_rxq_estimate_packet_buf_size(
        DOCA_ETH_RXQ_TYPE_CYCLIC, 0, 0, q->stride, static_cast<uint32_t>(q->ring_size), 0, 0, 0,
        &q->buf_size);
    if (ret != DOCA_SUCCESS) {
      DAQIRI_LOG_CRITICAL("Could not size the receive ring of RX queue '{}': {}", q->name,
                          doca_error_get_descr(ret));
      return false;
    }
    mr.adj_size_ = q->stride;
    mr.num_bufs_ = std::max<size_t>(q->ring_size, (q->buf_size + q->stride - 1) / q->stride);
    if (q->stride != mr.buf_size_ || q->ring_size != mr.num_bufs_) {
      DAQIRI_LOG_INFO(
          "RX queue '{}': memory region '{}' holds a ring of {} packets with a {} byte stride",
          q->name, q->mr_name, q->ring_size, q->stride);
    }

    if (!q->device->iface_name.empty()) {
      unsigned mtu = 0;
      const std::string path = "/sys/class/net/" + q->device->iface_name + "/mtu";
      if (FILE* f = std::fopen(path.c_str(), "r")) {
        if (std::fscanf(f, "%u", &mtu) != 1) {
          mtu = 0;
        }
        std::fclose(f);
      }
      // Ethernet header and one VLAN tag
      if (mtu != 0 && mtu + 18 > q->stride) {
        DAQIRI_LOG_WARN(
            "RX queue '{}': {} has MTU {}, but packets bigger than the {} byte receive stride stop "
            "the queue; raise buf_size or lower the MTU",
            q->name, q->device->iface_name, mtu, q->stride);
      }
    }
  }
  return true;
}

// One DOCA mmap per memory region, registered with the NICs of all the queues using it
bool GpunetioEngine::map_memory_regions() {
  const auto add_user = [this](const std::string& mr_name, Device* dev, Gpu* gpu) {
    auto& map = memory_maps_[mr_name];
    if (!map) {
      map = std::make_unique<MemoryMap>();
    }
    if (std::find(map->devices.begin(), map->devices.end(), dev) == map->devices.end()) {
      map->devices.push_back(dev);
    }
    map->gpu = gpu;
  };
  for (auto& q : rx_queues_) {
    add_user(q->mr_name, q->device, q->gpu);
  }
  for (auto& q : tx_queues_) {
    add_user(q->mr_name, q->device, q->gpu);
  }

  for (auto& [name, map] : memory_maps_) {
    const auto& mr = cfg_.mrs_.at(name);
    const auto& region = ar_.at(name);
    doca_error_t ret = doca_mmap_create(&map->mmap);
    if (ret != DOCA_SUCCESS) {
      map->mmap = nullptr;
      DAQIRI_LOG_CRITICAL("Failed to create the DOCA mmap of memory region '{}': {}", name,
                          doca_error_get_descr(ret));
      return false;
    }
    for (Device* dev : map->devices) {
      ret = doca_mmap_add_dev(map->mmap, dev->dev);
      if (ret != DOCA_SUCCESS) {
        DAQIRI_LOG_CRITICAL("Failed to add DOCA device {} to the mmap of '{}': {}", dev->pci_addr,
                            name, doca_error_get_descr(ret));
        return false;
      }
    }

    // GPU memory goes through dma-buf when the kernel supports it, nvidia-peermem otherwise
    if (mr.kind_ == MemoryKind::DEVICE &&
        doca_gpu_dmabuf_fd(map->gpu->gpu, region.ptr_, region.size_, &map->dmabuf_fd) ==
            DOCA_SUCCESS) {
      ret = doca_mmap_set_dmabuf_memrange(map->mmap, map->dmabuf_fd, region.ptr_, 0, region.size_);
    } else {
      map->dmabuf_fd = -1;
      ret = doca_mmap_set_memrange(map->mmap, region.ptr_, region.size_);
    }
    if (ret != DOCA_SUCCESS) {
      DAQIRI_LOG_CRITICAL("Failed to set the memory range of the mmap of '{}': {}", name,
                          doca_error_get_descr(ret));
      return false;
    }
    ret = doca_mmap_set_permissions(map->mmap, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE);
    if (ret == DOCA_SUCCESS) {
      ret = doca_mmap_start(map->mmap);
    }
    if (ret != DOCA_SUCCESS) {
      DAQIRI_LOG_CRITICAL("Failed to start the mmap of memory region '{}': {}", name,
                          doca_error_get_descr(ret));
      return false;
    }
    DAQIRI_LOG_INFO("Memory region '{}' registered ({} bytes, {})", name, region.size_,
                    map->dmabuf_fd >= 0 ? "dma-buf" : "memrange");
  }
  return true;
}

// DOCA Flow steers received packets to the RX queues; the TX queues need the port started too
bool GpunetioEngine::start_flow() {
  if (devices_.empty()) {
    return true;
  }

  struct doca_flow_cfg* flow_cfg = nullptr;
  doca_error_t ret = doca_flow_cfg_create(&flow_cfg);
  if (ret == DOCA_SUCCESS) {
    ret = doca_flow_cfg_set_pipe_queues(flow_cfg, 1);
  }
  if (ret == DOCA_SUCCESS) {
    ret = doca_flow_cfg_set_mode_args(flow_cfg, "vnf");
  }
  if (ret == DOCA_SUCCESS) {
    ret = doca_flow_init(flow_cfg);
  }
  if (flow_cfg != nullptr) {
    doca_flow_cfg_destroy(flow_cfg);
  }
  if (ret != DOCA_SUCCESS) {
    DAQIRI_LOG_CRITICAL("Failed to initialize DOCA Flow: {}", doca_error_get_descr(ret));
    return false;
  }
  flow_initialized_ = true;

  for (size_t i = 0; i < devices_.size(); i++) {
    Device& dev = *devices_[i];
    if (dev.rx_port < 0 && dev.tx_port < 0) {
      continue;
    }
    struct doca_flow_port_cfg* port_cfg = nullptr;
    ret = doca_flow_port_cfg_create(&port_cfg);
    if (ret == DOCA_SUCCESS) {
      ret = doca_flow_port_cfg_set_port_id(port_cfg, static_cast<uint16_t>(i));
    }
    if (ret == DOCA_SUCCESS) {
      ret = doca_flow_port_cfg_set_dev(port_cfg, dev.dev);
    }
    if (ret == DOCA_SUCCESS) {
      ret = doca_flow_port_start(port_cfg, &dev.flow_port);
    }
    if (port_cfg != nullptr) {
      doca_flow_port_cfg_destroy(port_cfg);
    }
    if (ret != DOCA_SUCCESS) {
      dev.flow_port = nullptr;
      DAQIRI_LOG_CRITICAL("Failed to start the DOCA Flow port of {}: {}", dev.pci_addr,
                          doca_error_get_descr(ret));
      return false;
    }
  }
  return true;
}

bool GpunetioEngine::create_rx_queue(RxQueue& q) {
  if (!select_cuda_device(q.gpu->ordinal, "creating RX queue '" + q.name + "'")) {
    return false;
  }
  const auto fail = [&q](const char* what, doca_error_t ret) {
    DAQIRI_LOG_CRITICAL("RX queue '{}': {} failed: {}", q.name, what, doca_error_get_descr(ret));
    return false;
  };

  doca_error_t ret =
      doca_eth_rxq_create(q.device->dev, static_cast<uint32_t>(q.ring_size), q.stride, &q.rxq);
  if (ret != DOCA_SUCCESS) {
    q.rxq = nullptr;
    return fail("doca_eth_rxq_create", ret);
  }
  ret = doca_eth_rxq_set_type(q.rxq, DOCA_ETH_RXQ_TYPE_CYCLIC);
  if (ret != DOCA_SUCCESS) {
    return fail("doca_eth_rxq_set_type", ret);
  }
  ret = doca_eth_rxq_set_pkt_buf(q.rxq, memory_maps_.at(q.mr_name)->mmap, 0, q.buf_size);
  if (ret != DOCA_SUCCESS) {
    return fail("doca_eth_rxq_set_pkt_buf", ret);
  }
  // Pre-Hopper GPUs need a memory consistency QP to read the packets after the receive
  if (q.gpu->cc_major < 9) {
    ret = doca_eth_rxq_gpu_enable_mcst_qp(q.rxq);
    if (ret != DOCA_SUCCESS) {
      return fail("doca_eth_rxq_gpu_enable_mcst_qp", ret);
    }
  }
  struct doca_ctx* ctx = doca_eth_rxq_as_doca_ctx(q.rxq);
  ret = doca_ctx_set_datapath_on_gpu(ctx, q.gpu->gpu);
  if (ret != DOCA_SUCCESS) {
    return fail("doca_ctx_set_datapath_on_gpu", ret);
  }
  ret = doca_ctx_start(ctx);
  if (ret != DOCA_SUCCESS) {
    return fail("doca_ctx_start", ret);
  }
  q.ctx_started = true;
  ret = doca_eth_rxq_get_gpu_handle(q.rxq, &q.gpu_rxq);
  if (ret != DOCA_SUCCESS) {
    return fail("doca_eth_rxq_get_gpu_handle", ret);
  }
  ret = doca_eth_rxq_apply_queue_id(q.rxq, static_cast<uint16_t>(q.id));
  if (ret != DOCA_SUCCESS) {
    return fail("doca_eth_rxq_apply_queue_id", ret);
  }

  // The kernel and the CPU locate the packets from the ring layout chosen by DOCA
  struct doca_gpu_eth_rxq layout {};
  if (cudaMemcpy(&layout, q.gpu_rxq, sizeof(layout), cudaMemcpyDefault) != cudaSuccess) {
    DAQIRI_LOG_CRITICAL("RX queue '{}': could not read the GPU queue handle", q.name);
    return false;
  }
  if (layout.pkt_num != q.ring_size || layout.max_pkt_sz != q.stride) {
    DAQIRI_LOG_CRITICAL(
        "RX queue '{}': DOCA built a ring of {} packets with a {} byte stride, expected {} and {}",
        q.name, layout.pkt_num, layout.max_pkt_sz, q.ring_size, q.stride);
    return false;
  }
  // The kernel gives the receive buffers back to the NIC one WQE of wqe_strides_num packets at a
  // time
  if (static_cast<uint64_t>(layout.wqe_num) * layout.wqe_strides_num != layout.pkt_num) {
    DAQIRI_LOG_CRITICAL(
        "RX queue '{}': unexpected DOCA receive queue layout ({} WQEs of {} strides)", q.name,
        layout.wqe_num, layout.wqe_strides_num);
    return false;
  }
  q.pkt_ptrs.resize(q.ring_size);
  for (uint64_t i = 0; i < q.ring_size; i++) {
    q.pkt_ptrs[i] = reinterpret_cast<void*>(layout.pkt_addr + i * q.stride);
  }

  q.ctrl = alloc_pinned<gpunetio::RxControl>(1);
  q.desc = alloc_pinned<gpunetio::RxBurstDesc>(q.ring_size);
  q.pkt_len = alloc_pinned<uint32_t>(q.ring_size);
  if (q.ctrl == nullptr || q.desc == nullptr || q.pkt_len == nullptr) {
    DAQIRI_LOG_CRITICAL("RX queue '{}': could not allocate the kernel resources", q.name);
    return false;
  }
  const uint32_t threads = gpunetio::rx_kernel_threads();
  gpunetio::RxKernelArgs& args = q.kernel_args;
  args.rxq = q.gpu_rxq;
  args.desc = q.desc;
  args.pkt_len = q.pkt_len;
  args.ctrl = q.ctrl;
  args.ring_mask = q.ring_size - 1;
  args.timeout_ns = q.timeout_ns;
  args.desc_mask = static_cast<uint32_t>(q.ring_size - 1);
  args.batch_size = std::max<uint32_t>(q.batch_size, 1);
  args.max_pkts = static_cast<uint32_t>(std::min<uint64_t>(
      round_up(args.batch_size, threads), round_up(RX_MAX_PKTS_PER_CALL, threads)));

  // At most ring_size descriptors are outstanding, as each holds at least one packet
  q.ring = daqiri::Ring::create("gpunetio_rx", static_cast<unsigned>(q.ring_size) + 1,
                                daqiri::RingMode::MPMC);
  q.desc_pkts.reset(new (std::nothrow) uint32_t[q.ring_size]());
  q.desc_freed.reset(new (std::nothrow) std::atomic<uint32_t>[q.ring_size]);
  if (q.ring == nullptr || !q.desc_pkts || !q.desc_freed) {
    DAQIRI_LOG_CRITICAL("RX queue '{}': could not allocate the burst bookkeeping", q.name);
    return false;
  }
  for (uint64_t i = 0; i < q.ring_size; i++) {
    q.desc_freed[i].store(0, std::memory_order_relaxed);
  }

  DAQIRI_LOG_INFO("RX queue '{}' (port {} queue {}): ring of {} packets, {} byte stride, {} RQ",
                  q.name, q.port, q.id, q.ring_size, q.stride,
                  layout.striding_rq ? "striding" : "regular");
  return true;
}

bool GpunetioEngine::create_tx_queue(TxQueue& q) {
  if (!select_cuda_device(q.gpu->ordinal, "creating TX queue '" + q.name + "'")) {
    return false;
  }
  const auto fail = [&q](const char* what, doca_error_t ret) {
    DAQIRI_LOG_CRITICAL("TX queue '{}': {} failed: {}", q.name, what, doca_error_get_descr(ret));
    return false;
  };

  const auto& mr = cfg_.mrs_.at(q.mr_name);
  q.slot_base = static_cast<uint8_t*>(ar_.at(q.mr_name).ptr_);
  q.slot_size = mr.adj_size_;
  q.num_slots = mr.num_bufs_;

  uint32_t max_wqes = static_cast<uint32_t>(TX_SQ_MAX_WQES);
  if (doca_eth_txq_cap_get_max_burst_size(doca_dev_as_devinfo(q.device->dev), 1, 0, &max_wqes) !=
          DOCA_SUCCESS ||
      max_wqes == 0) {
    max_wqes = static_cast<uint32_t>(TX_SQ_MAX_WQES);
  }
  uint64_t sq = next_pow2(std::clamp<uint64_t>(q.num_slots, TX_SQ_MIN_WQES, TX_SQ_MAX_WQES));
  while (sq > max_wqes && sq > 1) {
    sq >>= 1;
  }

  doca_error_t ret = doca_eth_txq_create(q.device->dev, static_cast<uint32_t>(sq), &q.txq);
  if (ret != DOCA_SUCCESS) {
    q.txq = nullptr;
    return fail("doca_eth_txq_create", ret);
  }
  // The TX kernels poll the completions
  ret = doca_eth_txq_gpu_set_completion_on_gpu(q.txq);
  if (ret != DOCA_SUCCESS) {
    return fail("doca_eth_txq_gpu_set_completion_on_gpu", ret);
  }
  struct doca_ctx* ctx = doca_eth_txq_as_doca_ctx(q.txq);
  ret = doca_ctx_set_datapath_on_gpu(ctx, q.gpu->gpu);
  if (ret != DOCA_SUCCESS) {
    return fail("doca_ctx_set_datapath_on_gpu", ret);
  }
  ret = doca_ctx_start(ctx);
  if (ret != DOCA_SUCCESS) {
    return fail("doca_ctx_start", ret);
  }
  q.ctx_started = true;
  ret = doca_eth_txq_apply_queue_id(q.txq, static_cast<uint16_t>(q.id));
  if (ret != DOCA_SUCCESS) {
    return fail("doca_eth_txq_apply_queue_id", ret);
  }
  ret = doca_eth_txq_get_gpu_handle(q.txq, &q.gpu_txq);
  if (ret != DOCA_SUCCESS) {
    return fail("doca_eth_txq_get_gpu_handle", ret);
  }

  uint32_t mkey = 0;
  ret = doca_mmap_get_mkey(memory_maps_.at(q.mr_name)->mmap, q.device->dev, &mkey);
  if (ret != DOCA_SUCCESS) {
    return fail("doca_mmap_get_mkey", ret);
  }
  q.mkey = htobe32(mkey);

  struct doca_gpu_eth_txq layout {};
  if (cudaMemcpy(&layout, q.gpu_txq, sizeof(layout), cudaMemcpyDefault) != cudaSuccess) {
    DAQIRI_LOG_CRITICAL("TX queue '{}': could not read the GPU queue handle", q.name);
    return false;
  }
  q.sq_wqes = static_cast<uint32_t>(layout.wqe_mask) + 1;
  q.max_chunk = std::max<uint32_t>(1, q.sq_wqes / 2);

  q.pkts = alloc_pinned<gpunetio::TxPacket>(q.num_slots);
  q.desc = alloc_pinned<gpunetio::TxBurstDesc>(TX_DESC_RING);
  q.ctrl = alloc_pinned<gpunetio::TxControl>(1);
  // The kernels count WQEs and completions from where DOCA left the send queue
  gpunetio::TxState state{};
  state.next_wqe = layout.wqe_pi;
  state.done_wqe = layout.wqe_pi;
  state.chunks = layout.cqe_ci;
  if (q.pkts == nullptr || q.desc == nullptr || q.ctrl == nullptr ||
      cudaMalloc(&q.state, sizeof(gpunetio::TxState)) != cudaSuccess ||
      cudaMemcpy(q.state, &state, sizeof(state), cudaMemcpyHostToDevice) != cudaSuccess ||
      (q.kernel == GpunetioTxKernel::PER_BURST &&
       cudaStreamCreateWithFlags(&q.stream, cudaStreamNonBlocking) != cudaSuccess)) {
    DAQIRI_LOG_CRITICAL("TX queue '{}': could not allocate the kernel resources", q.name);
    return false;
  }
  gpunetio::TxKernelArgs& args = q.kernel_args;
  args.txq = q.gpu_txq;
  args.pkts = q.pkts;
  args.desc = q.desc;
  args.ctrl = q.ctrl;
  args.state = q.state;
  args.num_slots = q.num_slots;
  args.desc_mask = TX_DESC_RING - 1;
  args.mkey = q.mkey;
  args.max_chunk = q.max_chunk;
  q.send_ring = daqiri::Ring::create("gpunetio_tx", TX_SEND_RING, daqiri::RingMode::MPMC);
  if (q.send_ring == nullptr) {
    DAQIRI_LOG_CRITICAL("TX queue '{}': could not allocate the send ring", q.name);
    return false;
  }

  DAQIRI_LOG_INFO(
      "TX queue '{}' (port {} queue {}): {} slots of {} bytes, {} WQE send queue, {} kernel",
      q.name, q.port, q.id, q.num_slots, q.slot_size, q.sq_wqes,
      gpunetio_tx_kernel_to_string(q.kernel));
  return true;
}

// A root control pipe matches the configured flows in order; each forwards to a pipe spreading
// packets over its RX queues. Unmatched packets go to the kernel with flow_isolation, otherwise
// to the first RX queue.
bool GpunetioEngine::program_steering(Device& dev, const InterfaceConfig& intf) {
  std::map<std::vector<uint16_t>, struct doca_flow_pipe*> rss_pipes;
  const auto rss_pipe = [&](const std::vector<uint16_t>& queues) -> struct doca_flow_pipe* {
    const auto it = rss_pipes.find(queues);
    if (it != rss_pipes.end()) {
      return it->second;
    }

    std::vector<uint16_t> queue_ids(queues);
    struct doca_flow_match match = {};  // Every packet
    struct doca_flow_fwd fwd = {};
    fwd.type = DOCA_FLOW_FWD_RSS;
    fwd.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
    fwd.rss.queues_array = queue_ids.data();
    fwd.rss.nr_queues = static_cast<int>(queue_ids.size());
    fwd.rss.outer_flags = DOCA_FLOW_RSS_IPV4 | DOCA_FLOW_RSS_UDP;
    struct doca_flow_fwd miss = {};
    miss.type = DOCA_FLOW_FWD_DROP;

    const std::string name = "DAQIRI_RSS_" + std::to_string(rss_pipes.size());
    struct doca_flow_pipe_cfg* pipe_cfg = nullptr;
    struct doca_flow_pipe* pipe = nullptr;
    doca_error_t ret = doca_flow_pipe_cfg_create(&pipe_cfg, dev.flow_port);
    if (ret == DOCA_SUCCESS) {
      ret = doca_flow_pipe_cfg_set_name(pipe_cfg, name.c_str());
    }
    if (ret == DOCA_SUCCESS) {
      ret = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_BASIC);
    }
    if (ret == DOCA_SUCCESS) {
      ret = doca_flow_pipe_cfg_set_is_root(pipe_cfg, false);
    }
    if (ret == DOCA_SUCCESS) {
      ret = doca_flow_pipe_cfg_set_match(pipe_cfg, &match, nullptr);
    }
    if (ret == DOCA_SUCCESS) {
      ret = doca_flow_pipe_create(pipe_cfg, &fwd, &miss, &pipe);
    }
    if (pipe_cfg != nullptr) {
      doca_flow_pipe_cfg_destroy(pipe_cfg);
    }
    if (ret != DOCA_SUCCESS) {
      DAQIRI_LOG_CRITICAL("Interface '{}': failed to create the RSS pipe: {}", intf.name_,
                          doca_error_get_descr(ret));
      return nullptr;
    }
    dev.pipes.push_back(pipe);

    struct doca_flow_pipe_entry* entry = nullptr;
    ret = doca_flow_pipe_basic_add_entry(0, pipe, &match, 0, nullptr, nullptr, nullptr,
                                         DOCA_FLOW_ENTRY_FLAGS_NO_WAIT, nullptr, &entry);
    if (ret != DOCA_SUCCESS) {
      DAQIRI_LOG_CRITICAL("Interface '{}': failed to add the RSS pipe entry: {}", intf.name_,
                          doca_error_get_descr(ret));
      return nullptr;
    }
    rss_pipes[queues] = pipe;
    return pipe;
  };

  struct doca_flow_pipe_cfg* pipe_cfg = nullptr;
  struct doca_flow_pipe* root = nullptr;
  doca_error_t ret = doca_flow_pipe_cfg_create(&pipe_cfg, dev.flow_port);
  if (ret == DOCA_SUCCESS) {
    ret = doca_flow_pipe_cfg_set_name(pipe_cfg, "DAQIRI_ROOT");
  }
  if (ret == DOCA_SUCCESS) {
    ret = doca_flow_pipe_cfg_set_type(pipe_cfg, DOCA_FLOW_PIPE_CONTROL);
  }
  if (ret == DOCA_SUCCESS) {
    ret = doca_flow_pipe_cfg_set_is_root(pipe_cfg, true);
  }
  if (ret == DOCA_SUCCESS) {
    ret = doca_flow_pipe_create(pipe_cfg, nullptr, nullptr, &root);
  }
  if (pipe_cfg != nullptr) {
    doca_flow_pipe_cfg_destroy(pipe_cfg);
  }
  if (ret != DOCA_SUCCESS) {
    DAQIRI_LOG_CRITICAL("Interface '{}': failed to create the root pipe: {}", intf.name_,
                        doca_error_get_descr(ret));
    return false;
  }
  dev.pipes.push_back(root);

  // Entries match their non-zero fields; a lower priority value wins
  uint32_t priority = 0;
  for (const auto& flow : intf.rx_.flows_) {
    struct doca_flow_match match = {};
    const FlowMatch& m = flow.match_;
    if (m.type_ == FlowMatchType::ETHERNET) {
      if (m.ethernet_match_.match_src_) {
        std::memcpy(match.outer.eth.src_mac, m.ethernet_match_.src_.data(),
                    DOCA_FLOW_ETHER_ADDR_LEN);
      }
      if (m.ethernet_match_.match_dst_) {
        std::memcpy(match.outer.eth.dst_mac, m.ethernet_match_.dst_.data(),
                    DOCA_FLOW_ETHER_ADDR_LEN);
      }
    } else {
      match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
      match.parser_meta.outer_l4_type = DOCA_FLOW_L4_META_UDP;
      // A bare l3_type is a protocol-only match, which control pipes reject; parser_meta already
      // selects IPv4, so set it only when an IPv4 field is matched
      if (m.ipv4_src_ != 0 || m.ipv4_dst_ != 0 || m.ipv4_len_ != 0) {
        match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
      }
      match.outer.l4_type_ext = DOCA_FLOW_L4_TYPE_EXT_UDP;
      match.outer.ip4.src_ip = m.ipv4_src_;
      match.outer.ip4.dst_ip = m.ipv4_dst_;
      match.outer.ip4.total_len = htons(m.ipv4_len_);
      match.outer.udp.l4_port.src_port = htons(m.udp_src_);
      match.outer.udp.l4_port.dst_port = htons(m.udp_dst_);
    }

    struct doca_flow_fwd fwd = {};
    fwd.type = DOCA_FLOW_FWD_PIPE;
    fwd.next_pipe = rss_pipe(flow_queues(flow));
    if (fwd.next_pipe == nullptr) {
      return false;
    }
    struct doca_flow_pipe_entry* entry = nullptr;
    ret = doca_flow_pipe_control_add_entry(0, root, &match, nullptr, nullptr, nullptr, nullptr,
                                           nullptr, nullptr, priority++, &fwd, nullptr, &entry);
    if (ret != DOCA_SUCCESS) {
      DAQIRI_LOG_CRITICAL("Interface '{}': failed to add RX flow '{}': {}", intf.name_, flow.name_,
                          doca_error_get_descr(ret));
      return false;
    }
  }

  struct doca_flow_match match_all = {};
  struct doca_flow_fwd fwd = {};
  if (intf.rx_.flow_isolation_) {
    fwd.type = DOCA_FLOW_FWD_TARGET;
    ret = doca_flow_get_target(DOCA_FLOW_TARGET_KERNEL, &fwd.target);
    if (ret != DOCA_SUCCESS) {
      DAQIRI_LOG_CRITICAL("Interface '{}': no kernel target for unmatched packets: {}", intf.name_,
                          doca_error_get_descr(ret));
      return false;
    }
  } else {
    fwd.type = DOCA_FLOW_FWD_PIPE;
    fwd.next_pipe = rss_pipe({static_cast<uint16_t>(intf.rx_.queues_.front().common_.id_)});
    if (fwd.next_pipe == nullptr) {
      return false;
    }
  }
  struct doca_flow_pipe_entry* entry = nullptr;
  ret = doca_flow_pipe_control_add_entry(0, root, &match_all, nullptr, nullptr, nullptr, nullptr,
                                         nullptr, nullptr, priority, &fwd, nullptr, &entry);
  if (ret == DOCA_SUCCESS) {
    ret = doca_flow_entries_process(dev.flow_port, 0, 0, 0);
  }
  if (ret != DOCA_SUCCESS) {
    DAQIRI_LOG_CRITICAL("Interface '{}': failed to steer unmatched packets: {}", intf.name_,
                        doca_error_get_descr(ret));
    return false;
  }
  DAQIRI_LOG_INFO("Interface '{}': {} RX flow(s) programmed, unmatched packets to {}", intf.name_,
                  intf.rx_.flows_.size(), intf.rx_.flow_isolation_ ? "the kernel" : "queue 0");
  return true;
}

bool GpunetioEngine::create_burst_pools() {
  const auto construct = [this](daqiri::ObjectPool* pool) {
    std::vector<void*> objs(pool->size());
    for (auto& obj : objs) {
      if (!pool->get(&obj)) {
        return false;
      }
      burst_objects_.push_back(new (obj) BurstParams());
    }
    for (void* obj : objs) {
      pool->put(obj);
    }
    return true;
  };

  if (!rx_queues_.empty()) {
    rx_meta_pool_ = daqiri::ObjectPool::create(
        "GPUNETIO_RX_META", std::max<uint32_t>(cfg_.rx_meta_buffers_, 64), sizeof(BurstParams));
    if (rx_meta_pool_ == nullptr || !construct(rx_meta_pool_)) {
      DAQIRI_LOG_CRITICAL("Failed to create the RX burst pool");
      return false;
    }
  }
  if (!tx_queues_.empty()) {
    for (const auto& q : tx_queues_) {
      max_tx_batch_ = std::max(max_tx_batch_, q->batch_size);
    }
    max_tx_batch_ = std::max<uint32_t>(max_tx_batch_, 1);
    // The packet pointers and lengths of a TX burst follow its BurstParams
    const size_t elt = sizeof(BurstParams) +
                       static_cast<size_t>(max_tx_batch_) * (sizeof(void*) + sizeof(uint32_t));
    tx_meta_pool_ = daqiri::ObjectPool::create("GPUNETIO_TX_META",
                                               std::max<uint32_t>(cfg_.tx_meta_buffers_, 64), elt);
    if (tx_meta_pool_ == nullptr || !construct(tx_meta_pool_)) {
      DAQIRI_LOG_CRITICAL("Failed to create the TX burst pool");
      return false;
    }
  }
  return true;
}

// On each GPU, one RX kernel with a block per RX queue and one TX kernel with a block per
// persistent TX queue. The per_burst TX queues launch their kernels from their worker.
bool GpunetioEngine::start_kernels() {
  // Under CUDA lazy loading, a kernel launched for the first time waits for the running kernels to
  // exit, and the resident kernels exit only at shutdown: load every kernel of the engine first
  for (auto& [ordinal, gpu] : gpus_) {
    if (!select_cuda_device(ordinal, "loading the gpunetio kernels")) {
      return false;
    }
    const cudaError_t err = gpunetio::load_kernels();
    if (err != cudaSuccess) {
      DAQIRI_LOG_CRITICAL("GPU {}: could not load the gpunetio kernels: {}", ordinal,
                          cudaGetErrorString(err));
      return false;
    }
  }
  const bool resident =
      !rx_queues_.empty() || std::any_of(tx_queues_.begin(), tx_queues_.end(), [](const auto& q) {
        return q->kernel == GpunetioTxKernel::PERSISTENT;
      });
  const char* loading = std::getenv("CUDA_MODULE_LOADING");
  if (resident && (loading == nullptr || std::strcmp(loading, "EAGER") != 0)) {
    DAQIRI_LOG_WARN(
        "CUDA lazy loading is on: a kernel the application launches for the first time after "
        "daqiri_init() blocks until shutdown. Set CUDA_MODULE_LOADING=EAGER, or load the "
        "application kernels before daqiri_init() with cudaFuncGetAttributes().");
  }

  for (auto& [ordinal, gpu] : gpus_) {
    std::vector<gpunetio::RxKernelArgs> rx_blocks;
    for (const auto& q : rx_queues_) {
      if (q->gpu == gpu.get()) {
        rx_blocks.push_back(q->kernel_args);
      }
    }
    std::vector<gpunetio::TxKernelArgs> tx_blocks;
    for (const auto& q : tx_queues_) {
      if (q->gpu == gpu.get() && q->kernel == GpunetioTxKernel::PERSISTENT) {
        tx_blocks.push_back(q->kernel_args);
      }
    }
    const auto burst_queues =
        std::count_if(tx_queues_.begin(), tx_queues_.end(), [&](const auto& q) {
          return q->gpu == gpu.get() && q->kernel == GpunetioTxKernel::PER_BURST;
        });
    if (!select_cuda_device(ordinal, "launching the gpunetio kernels")) {
      return false;
    }
    // The RX and TX kernels share the GPU: check that all their blocks, and the blocks of the
    // per_burst launches, fit at once
    const cudaError_t capacity = gpunetio::check_gpu_capacity(
        static_cast<uint32_t>(rx_blocks.size()), static_cast<uint32_t>(tx_blocks.size()),
        static_cast<uint32_t>(burst_queues));
    if (capacity != cudaSuccess) {
      DAQIRI_LOG_CRITICAL(
          "GPU {}: the {} RX, {} persistent TX and {} per_burst TX queues need more blocks than "
          "the GPU can run at once: {}",
          ordinal, rx_blocks.size(), tx_blocks.size(), burst_queues, cudaGetErrorString(capacity));
      return false;
    }
    if (!launch_resident_kernel("RX", ordinal, rx_blocks, &gpu->rx_args, &gpu->rx_stream,
                                gpunetio::launch_rx_kernel) ||
        !launch_resident_kernel("TX", ordinal, tx_blocks, &gpu->tx_args, &gpu->tx_stream,
                                gpunetio::launch_tx_persistent_kernel)) {
      return false;
    }
  }
  return true;
}

// Asks the kernels to exit and waits for their streams to drain. The queues of a kernel that
// doesn't stop stay alive: destroying them under a running kernel would fault the GPU.
void GpunetioEngine::stop_kernels() {
  for (auto& q : rx_queues_) {
    if (q->ctrl != nullptr) {
      store_release(&q->ctrl->exit, 1u);
    }
  }
  for (auto& q : tx_queues_) {
    if (q->ctrl != nullptr) {
      store_release(&q->ctrl->exit, 1u);
    }
  }
  for (auto& [ordinal, gpu] : gpus_) {
    const bool rx_stuck = gpu->rx_stream != nullptr && !wait_stream(gpu->rx_stream);
    const bool tx_stuck = gpu->tx_stream != nullptr && !wait_stream(gpu->tx_stream);
    if (rx_stuck) {
      DAQIRI_LOG_CRITICAL("GPU {}: the RX kernel did not stop", ordinal);
    }
    if (tx_stuck) {
      DAQIRI_LOG_CRITICAL("GPU {}: the TX kernel did not stop", ordinal);
    }
    for (auto& q : rx_queues_) {
      if (q->gpu == gpu.get() && rx_stuck) {
        q->kernel_stuck = true;
      }
    }
    for (auto& q : tx_queues_) {
      if (q->gpu == gpu.get() && q->kernel == GpunetioTxKernel::PERSISTENT && tx_stuck) {
        q->kernel_stuck = true;
      }
    }
  }
  for (auto& q : tx_queues_) {
    if (q->stream != nullptr && !wait_stream(q->stream)) {
      q->kernel_stuck = true;
      DAQIRI_LOG_CRITICAL("TX queue '{}': the kernel did not stop", q->name);
    }
  }
}

void GpunetioEngine::run() {
  workers_running_.store(true);
  for (auto& q : rx_queues_) {
    q->worker = std::thread(&GpunetioEngine::rx_worker, this, q.get());
  }
  for (auto& q : tx_queues_) {
    q->worker = std::thread(&GpunetioEngine::tx_worker, this, q.get());
  }
}

// ---------------------------------------------------------------------------
// Workers
// ---------------------------------------------------------------------------

// Turns the bursts published by the RX kernel into BurstParams for the application, and reports
// the packets the application freed back to the kernel, oldest first.
void GpunetioEngine::rx_worker(RxQueue* q) {
  pin_thread(q->cpu_core, "RX queue '" + q->name + "'");
  const uint64_t ring_mask = q->ring_size - 1;

  while (workers_running_.load(std::memory_order_relaxed)) {
    bool busy = false;

    while (true) {
      gpunetio::RxBurstDesc& desc = q->desc[q->consumed & ring_mask];
      if (load_acquire(&desc.ready) != static_cast<uint32_t>(q->consumed + 1)) {
        break;
      }
      BurstParams* burst = nullptr;
      if (!rx_meta_pool_->get(reinterpret_cast<void**>(&burst))) {
        q->pool_empty.fetch_add(1, std::memory_order_relaxed);
        break;
      }
      const uint64_t slot = desc.pkt_seq & ring_mask;
      const uint32_t num = desc.num_pkts;
      uint64_t nbytes = 0;
      for (uint32_t i = 0; i < num; i++) {
        nbytes += q->pkt_len[slot + i];
      }

      auto& hdr = burst->hdr.hdr;
      hdr.num_pkts = num;
      hdr.port_id = static_cast<uint16_t>(q->port);
      hdr.q_id = static_cast<uint16_t>(q->id);
      hdr.num_segs = 1;
      hdr.nbytes = nbytes;
      hdr.first_pkt_addr = reinterpret_cast<uintptr_t>(q->pkt_ptrs[slot]);
      hdr.max_pkt = static_cast<uint32_t>(q->ring_size);
      hdr.max_pkt_size = q->stride;
      hdr.gpu_pkt0_idx = static_cast<uint32_t>(slot);
      hdr.gpu_pkt0_addr = hdr.first_pkt_addr;
      hdr.burst_flags = GPUNETIO_RX_BURST_FLAG;
      // The bursts never wrap around the ring, so their packets index straight into it
      burst->pkts[0] = &q->pkt_ptrs[slot];
      burst->pkt_lens[0] = &q->pkt_len[slot];
      for (size_t s = 1; s < burst->pkts.size(); s++) {
        burst->pkts[s] = nullptr;
        burst->pkt_lens[s] = nullptr;
      }
      burst->pkt_extra_info = nullptr;
      burst->event = nullptr;
      set_priv(burst, q->consumed);

      const uint64_t desc_slot = q->consumed & ring_mask;
      q->desc_pkts[desc_slot] = num;
      q->desc_freed[desc_slot].store(0, std::memory_order_relaxed);
      if (!q->ring->enqueue(burst)) {
        rx_meta_pool_->put(burst);
        q->ring_full.fetch_add(1, std::memory_order_relaxed);
        break;
      }
      q->consumed++;
      q->bursts.fetch_add(1, std::memory_order_relaxed);
      q->pkts.fetch_add(num, std::memory_order_relaxed);
      q->bytes.fetch_add(nbytes, std::memory_order_relaxed);
      busy = true;
    }

    const uint64_t released_before = q->released_pkts;
    while (q->release_cursor < q->consumed) {
      const uint64_t desc_slot = q->release_cursor & ring_mask;
      if (q->desc_freed[desc_slot].load(std::memory_order_acquire) < q->desc_pkts[desc_slot]) {
        break;
      }
      q->released_pkts += q->desc_pkts[desc_slot];
      q->release_cursor++;
    }
    if (q->released_pkts != released_before) {
      store_release(&q->ctrl->released_pkts, q->released_pkts);
      busy = true;
    }

    if (!busy) {
      const uint32_t error = load_acquire(&q->ctrl->error);
      if (error != gpunetio::KERNEL_OK && !q->error_logged) {
        q->error_logged = true;
        DAQIRI_LOG_ERROR("RX queue '{}' stopped: {}", q->name, kernel_error_str(error));
      }
      cpu_relax();
    }
  }
}

// Hands the bursts sent by the application to the GPU, then retires them once the NIC
// completes them: their TX slots and BurstParams become free again.
void GpunetioEngine::tx_worker(TxQueue* q) {
  pin_thread(q->cpu_core, "TX queue '" + q->name + "'");
  (void)select_cuda_device(q->gpu->ordinal, "running the TX worker");
  const uint32_t max_inflight =
      q->kernel == GpunetioTxKernel::PERSISTENT ? TX_DESC_RING : TX_LAUNCHES_IN_FLIGHT;

  while (workers_running_.load(std::memory_order_relaxed)) {
    bool busy = false;

    while (!q->failed.load(std::memory_order_relaxed) && q->inflight.size() < max_inflight) {
      void* obj = nullptr;
      if (!q->send_ring->dequeue(&obj)) {
        break;
      }
      auto* burst = static_cast<BurstParams*>(obj);
      BurstPriv priv{};
      (void)get_priv(burst, &priv);
      const uint32_t num = static_cast<uint32_t>(burst->hdr.hdr.num_pkts);
      if (num == 0) {
        // Nothing to send, and no completion would retire it
        q->retire(priv.seq, priv.reserved);
        tx_meta_pool_->put(burst);
        continue;
      }
      uint64_t nbytes = 0;
      for (uint32_t i = 0; i < num; i++) {
        gpunetio::TxPacket& pkt = q->pkts[(priv.seq + i) % q->num_slots];
        pkt.addr = reinterpret_cast<uint64_t>(burst->pkts[0][i]);
        pkt.len = burst->pkt_lens[0][i];
        nbytes += pkt.len;
      }

      if (q->kernel == GpunetioTxKernel::PERSISTENT) {
        gpunetio::TxBurstDesc& desc = q->desc[q->published & (TX_DESC_RING - 1)];
        desc.slot_start = priv.seq;
        desc.num_pkts = num;
        store_release(&desc.ready, static_cast<uint32_t>(q->published + 1));
      } else {
        const cudaError_t err =
            gpunetio::launch_tx_burst_kernel(q->kernel_args, priv.seq, num, q->stream);
        if (err != cudaSuccess) {
          DAQIRI_LOG_ERROR("TX queue '{}': kernel launch failed: {}", q->name,
                           cudaGetErrorString(err));
          q->failed.store(true);
          q->retire(priv.seq, priv.reserved);
          tx_meta_pool_->put(burst);
          break;
        }
      }
      q->published++;
      q->inflight.push_back(burst);
      q->sent_bursts.fetch_add(1, std::memory_order_relaxed);
      q->sent_pkts.fetch_add(num, std::memory_order_relaxed);
      q->sent_bytes.fetch_add(nbytes, std::memory_order_relaxed);
      busy = true;
    }

    const uint64_t done = load_acquire(&q->ctrl->completed_bursts);
    while (q->reaped < done && !q->inflight.empty()) {
      BurstParams* burst = q->inflight.front();
      q->inflight.pop_front();
      BurstPriv done_priv{};
      if (get_priv(burst, &done_priv)) {
        q->retire(done_priv.seq, done_priv.reserved);
      }
      tx_meta_pool_->put(burst);
      q->reaped++;
      busy = true;
    }

    if (!busy) {
      const uint32_t error = load_acquire(&q->ctrl->error);
      if (error != gpunetio::KERNEL_OK && !q->failed.exchange(true)) {
        DAQIRI_LOG_ERROR("TX queue '{}' stopped: {}", q->name, kernel_error_str(error));
      }
      cpu_relax();
    }
  }
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------
void GpunetioEngine::shutdown() {
  workers_running_.store(false);
  for (auto& q : rx_queues_) {
    if (q->worker.joinable()) {
      q->worker.join();
    }
  }
  for (auto& q : tx_queues_) {
    if (q->worker.joinable()) {
      q->worker.join();
    }
  }
  stop_kernels();

  // The DOCA Flow pipes reference the RX queues: remove the steering first
  for (auto& dev : devices_) {
    for (auto it = dev->pipes.rbegin(); it != dev->pipes.rend(); ++it) {
      doca_flow_pipe_destroy(*it);
    }
    dev->pipes.clear();
    if (dev->flow_port != nullptr) {
      const doca_error_t ret = doca_flow_port_stop(dev->flow_port);
      if (ret != DOCA_SUCCESS) {
        DAQIRI_LOG_WARN("Failed to stop the DOCA Flow port of {}: {}", dev->pci_addr,
                        doca_error_get_descr(ret));
      }
      dev->flow_port = nullptr;
    }
  }

  for (auto& q : rx_queues_) {
    if (!q->kernel_stuck) {
      if (q->ctx_started) {
        (void)doca_ctx_stop(doca_eth_rxq_as_doca_ctx(q->rxq));
      }
      if (q->rxq != nullptr) {
        (void)doca_eth_rxq_destroy(q->rxq);
      }
      if (q->ctrl != nullptr) {
        cudaFreeHost(q->ctrl);
      }
      if (q->desc != nullptr) {
        cudaFreeHost(q->desc);
      }
      if (q->pkt_len != nullptr) {
        cudaFreeHost(q->pkt_len);
      }
    }
    daqiri::Ring::free(q->ring);
  }
  for (auto& q : tx_queues_) {
    if (!q->kernel_stuck) {
      if (q->ctx_started) {
        (void)doca_ctx_stop(doca_eth_txq_as_doca_ctx(q->txq));
      }
      if (q->txq != nullptr) {
        (void)doca_eth_txq_destroy(q->txq);
      }
      if (q->state != nullptr) {
        cudaFree(q->state);
      }
      if (q->pkts != nullptr) {
        cudaFreeHost(q->pkts);
      }
      if (q->desc != nullptr) {
        cudaFreeHost(q->desc);
      }
      if (q->ctrl != nullptr) {
        cudaFreeHost(q->ctrl);
      }
      if (q->stream != nullptr) {
        cudaStreamDestroy(q->stream);
      }
    }
    daqiri::Ring::free(q->send_ring);
  }
  bool stuck = false;
  for (const auto& q : rx_queues_) {
    stuck = stuck || q->kernel_stuck;
  }
  for (const auto& q : tx_queues_) {
    stuck = stuck || q->kernel_stuck;
  }
  rx_queues_.clear();
  tx_queues_.clear();

  // A stuck kernel may still access the memory regions: leave them registered and allocated
  if (!stuck) {
    for (auto& [name, map] : memory_maps_) {
      if (map->mmap != nullptr) {
        (void)doca_mmap_destroy(map->mmap);
      }
      if (map->dmabuf_fd >= 0) {
        close(map->dmabuf_fd);
      }
    }
  }
  memory_maps_.clear();

  if (flow_initialized_) {
    doca_flow_destroy();
    flow_initialized_ = false;
  }
  if (!stuck) {
    for (auto& [ordinal, gpu] : gpus_) {
      if (gpu->rx_stream != nullptr) {
        cudaStreamDestroy(gpu->rx_stream);
      }
      if (gpu->tx_stream != nullptr) {
        cudaStreamDestroy(gpu->tx_stream);
      }
      if (gpu->rx_args != nullptr) {
        cudaFreeHost(gpu->rx_args);
      }
      if (gpu->tx_args != nullptr) {
        cudaFreeHost(gpu->tx_args);
      }
      if (gpu->gpu != nullptr) {
        (void)doca_gpu_destroy(gpu->gpu);
      }
    }
    free_memory_regions();
  }
  gpus_.clear();

  for (auto& dev : devices_) {
    if (dev->dev != nullptr) {
      const doca_error_t ret = doca_dev_close(dev->dev);
      if (ret != DOCA_SUCCESS) {
        DAQIRI_LOG_WARN("Failed to close DOCA device {}: {}", dev->pci_addr,
                        doca_error_get_descr(ret));
      }
    }
  }
  devices_.clear();
  port_devices_.clear();

  for (BurstParams* burst : burst_objects_) {
    burst->~BurstParams();
  }
  burst_objects_.clear();
  daqiri::ObjectPool::free(rx_meta_pool_);
  daqiri::ObjectPool::free(tx_meta_pool_);
  rx_meta_pool_ = nullptr;
  tx_meta_pool_ = nullptr;
  max_tx_batch_ = 0;
  initialized_ = false;
}

void GpunetioEngine::print_stats() {
  for (const auto& q : rx_queues_) {
    const uint64_t gpu_pkts = q->ctrl != nullptr ? load_acquire(&q->ctrl->pkts) : 0;
    DAQIRI_LOG_INFO(
        "gpunetio RX port {} q{} '{}': bursts={} pkts={} bytes={} gpu_pkts={} released={} "
        "burst_pool_empty={} app_ring_full={}",
        q->port, q->id, q->name, q->bursts.load(), q->pkts.load(), q->bytes.load(), gpu_pkts,
        q->released_pkts, q->pool_empty.load(), q->ring_full.load());
  }
  for (const auto& q : tx_queues_) {
    const uint64_t done = q->ctrl != nullptr ? load_acquire(&q->ctrl->completed_bursts) : 0;
    DAQIRI_LOG_INFO(
        "gpunetio TX port {} q{} '{}' ({} kernel): bursts={} pkts={} bytes={} completed_bursts={} "
        "send_ring_full={}",
        q->port, q->id, q->name, gpunetio_tx_kernel_to_string(q->kernel), q->sent_bursts.load(),
        q->sent_pkts.load(), q->sent_bytes.load(), done, q->send_ring_full.load());
  }
}

Status GpunetioEngine::get_mac_addr(int port, char* mac) {
  if (mac == nullptr) {
    return Status::NULL_PTR;
  }
  if (port < 0 || static_cast<size_t>(port) >= port_devices_.size() ||
      port_devices_[port] == nullptr) {
    DAQIRI_LOG_ERROR("get_mac_addr: invalid port {}", port);
    return Status::INVALID_PARAMETER;
  }
  const doca_error_t ret =
      doca_devinfo_get_mac_addr(doca_dev_as_devinfo(port_devices_[port]->dev),
                                reinterpret_cast<uint8_t*>(mac), DOCA_DEVINFO_MAC_ADDR_SIZE);
  if (ret != DOCA_SUCCESS) {
    DAQIRI_LOG_ERROR("get_mac_addr: failed to read the MAC address of port {}: {}", port,
                     doca_error_get_descr(ret));
    return Status::GENERIC_FAILURE;
  }
  return Status::SUCCESS;
}

GpunetioEngine::RxQueue* GpunetioEngine::find_rx_queue(int port, int q) const {
  for (const auto& rxq : rx_queues_) {
    if (rxq->port == port && rxq->id == q) {
      return rxq.get();
    }
  }
  return nullptr;
}

GpunetioEngine::TxQueue* GpunetioEngine::find_tx_queue(int port, int q) const {
  for (const auto& txq : tx_queues_) {
    if (txq->port == port && txq->id == q) {
      return txq.get();
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Packet accessors
// ---------------------------------------------------------------------------
void* GpunetioEngine::get_packet_ptr(BurstParams* burst, int idx) {
  return burst->pkts[0][idx];
}

uint32_t GpunetioEngine::get_packet_length(BurstParams* burst, int idx) {
  return burst->pkt_lens[0][idx];
}

void* GpunetioEngine::get_segment_packet_ptr(BurstParams* burst, int seg, int idx) {
  return burst->pkts[seg][idx];
}

uint32_t GpunetioEngine::get_segment_packet_length(BurstParams* burst, int seg, int idx) {
  return burst->pkt_lens[seg][idx];
}

FlowId GpunetioEngine::get_packet_flow_id(BurstParams* burst, int idx) {
  (void)burst;
  (void)idx;
  return 0;
}

Status GpunetioEngine::get_packet_rx_timestamp(BurstParams* burst, int idx,
                                               uint64_t* timestamp_ns) {
  (void)burst;
  (void)idx;
  (void)timestamp_ns;
  return Status::NOT_SUPPORTED;
}

void* GpunetioEngine::get_packet_extra_info(BurstParams* burst, int idx) {
  (void)burst;
  (void)idx;
  return nullptr;
}

uint64_t GpunetioEngine::get_burst_tot_byte(BurstParams* burst) {
  uint64_t total = 0;
  for (size_t i = 0; i < burst->hdr.hdr.num_pkts; i++) {
    total += burst->pkt_lens[0][i];
  }
  return total;
}

// ---------------------------------------------------------------------------
// RX
// ---------------------------------------------------------------------------
Status GpunetioEngine::get_rx_burst(BurstParams** burst, int port, int q) {
  if (burst == nullptr) {
    return Status::NULL_PTR;
  }
  *burst = nullptr;
  RxQueue* rxq = find_rx_queue(port, q);
  if (rxq == nullptr) {
    return Status::INVALID_PARAMETER;
  }
  void* obj = nullptr;
  if (!rxq->ring->dequeue(&obj)) {
    return Status::NOT_READY;
  }
  *burst = static_cast<BurstParams*>(obj);
  return Status::SUCCESS;
}

void GpunetioEngine::free_all_packets(BurstParams* burst) {
  if (burst == nullptr) {
    return;
  }
  BurstPriv priv{};
  if (!get_priv(burst, &priv)) {
    return;
  }
  const auto& hdr = burst->hdr.hdr;

  if (hdr.burst_flags & GPUNETIO_TX_BURST_FLAG) {
    // A TX burst the application allocated but did not send: its slots are retired, and become
    // free once the earlier ones are
    TxQueue* q = find_tx_queue(hdr.port_id, hdr.q_id);
    if (q != nullptr && priv.reserved != 0) {
      q->retire(priv.seq, priv.reserved);
      set_priv(burst, priv.seq, 0);
    }
    return;
  }
  if (hdr.burst_flags & GPUNETIO_RX_BURST_FLAG) {
    RxQueue* q = find_rx_queue(hdr.port_id, hdr.q_id);
    if (q != nullptr) {
      q->desc_freed[priv.seq & (q->ring_size - 1)].store(static_cast<uint32_t>(hdr.num_pkts),
                                                         std::memory_order_release);
    }
  }
}

void GpunetioEngine::free_packet(BurstParams* burst, int pkt) {
  (void)pkt;
  if (burst == nullptr || !(burst->hdr.hdr.burst_flags & GPUNETIO_RX_BURST_FLAG)) {
    return;
  }
  BurstPriv priv{};
  if (!get_priv(burst, &priv)) {
    return;
  }
  RxQueue* q = find_rx_queue(burst->hdr.hdr.port_id, burst->hdr.hdr.q_id);
  if (q != nullptr) {
    q->desc_freed[priv.seq & (q->ring_size - 1)].fetch_add(1, std::memory_order_release);
  }
}

void GpunetioEngine::free_all_segment_packets(BurstParams* burst, int seg) {
  if (seg == 0) {
    free_all_packets(burst);
  }
}

void GpunetioEngine::free_packet_segment(BurstParams* burst, int seg, int pkt) {
  if (seg == 0) {
    free_packet(burst, pkt);
  }
}

void GpunetioEngine::free_rx_burst(BurstParams* burst) {
  if (burst != nullptr && rx_meta_pool_ != nullptr) {
    rx_meta_pool_->put(burst);
  }
}

void GpunetioEngine::free_rx_metadata(BurstParams* burst) {
  free_rx_burst(burst);
}

// ---------------------------------------------------------------------------
// TX
// ---------------------------------------------------------------------------
BurstParams* GpunetioEngine::create_tx_burst_params() {
  BurstParams* burst = nullptr;
  if (tx_meta_pool_ == nullptr || !tx_meta_pool_->get(reinterpret_cast<void**>(&burst))) {
    return nullptr;
  }
  auto* arrays = reinterpret_cast<uint8_t*>(burst) + sizeof(BurstParams);
  burst->hdr.hdr.num_pkts = 0;
  burst->hdr.hdr.num_segs = 1;
  burst->hdr.hdr.burst_flags = GPUNETIO_TX_BURST_FLAG;
  burst->pkts[0] = reinterpret_cast<void**>(arrays);
  burst->pkt_lens[0] = reinterpret_cast<uint32_t*>(arrays + max_tx_batch_ * sizeof(void*));
  for (size_t s = 1; s < burst->pkts.size(); s++) {
    burst->pkts[s] = nullptr;
    burst->pkt_lens[s] = nullptr;
  }
  set_priv(burst, 0);
  return burst;
}

bool GpunetioEngine::is_tx_burst_available(BurstParams* burst) {
  if (burst == nullptr) {
    return false;
  }
  TxQueue* q = find_tx_queue(burst->hdr.hdr.port_id, burst->hdr.hdr.q_id);
  if (q == nullptr || burst->hdr.hdr.num_pkts > max_tx_batch_) {
    return false;
  }
  const uint64_t in_flight = q->alloc_head - q->completed_tail.load(std::memory_order_acquire);
  return q->num_slots - in_flight >= burst->hdr.hdr.num_pkts;
}

Status GpunetioEngine::get_tx_packet_burst(BurstParams* burst) {
  if (burst == nullptr) {
    return Status::NULL_PTR;
  }
  TxQueue* q = find_tx_queue(burst->hdr.hdr.port_id, burst->hdr.hdr.q_id);
  if (q == nullptr) {
    return Status::INVALID_PARAMETER;
  }
  const uint64_t num = burst->hdr.hdr.num_pkts;
  if (num > max_tx_batch_) {
    DAQIRI_LOG_ERROR("TX queue '{}': burst of {} packets exceeds the batch size {}", q->name, num,
                     max_tx_batch_);
    return Status::INVALID_PARAMETER;
  }
  const uint64_t in_flight = q->alloc_head - q->completed_tail.load(std::memory_order_acquire);
  if (q->num_slots - in_flight < num) {
    return Status::NO_FREE_PACKET_BUFFERS;
  }

  for (uint64_t i = 0; i < num; i++) {
    burst->pkts[0][i] = q->slot_base + ((q->alloc_head + i) % q->num_slots) * q->slot_size;
    burst->pkt_lens[0][i] = 0;
  }
  burst->hdr.hdr.num_segs = 1;
  burst->hdr.hdr.burst_flags = GPUNETIO_TX_BURST_FLAG;
  set_priv(burst, q->alloc_head, static_cast<uint32_t>(num));
  q->alloc_head += num;
  return Status::SUCCESS;
}

Status GpunetioEngine::set_packet_lengths(BurstParams* burst, int idx,
                                          const std::initializer_list<int>& lens) {
  if (lens.size() != 1) {
    return Status::INVALID_PARAMETER;
  }
  burst->pkt_lens[0][idx] = static_cast<uint32_t>(*lens.begin());
  return Status::SUCCESS;
}

Status GpunetioEngine::set_packet_tx_time(BurstParams* burst, int idx, uint64_t time) {
  (void)burst;
  (void)idx;
  (void)time;
  return Status::NOT_SUPPORTED;
}

// The TX slots are usually in GPU memory: write through CUDA
Status GpunetioEngine::write_packet(BurstParams* burst, int idx, size_t offset, const void* data,
                                    size_t len) {
  auto* dst = static_cast<uint8_t*>(burst->pkts[0][idx]) + offset;
  return cudaMemcpy(dst, data, len, cudaMemcpyDefault) == cudaSuccess ? Status::SUCCESS
                                                                      : Status::GENERIC_FAILURE;
}

Status GpunetioEngine::set_eth_header(BurstParams* burst, int idx, char* dst_addr) {
  struct ethhdr eth {};
  std::memcpy(eth.h_dest, dst_addr, ETH_ALEN);
  if (get_mac_addr(burst->hdr.hdr.port_id, reinterpret_cast<char*>(eth.h_source)) !=
      Status::SUCCESS) {
    return Status::GENERIC_FAILURE;
  }
  eth.h_proto = htons(ETH_P_IP);
  return write_packet(burst, idx, 0, &eth, sizeof(eth));
}

Status GpunetioEngine::set_ipv4_header(BurstParams* burst, int idx, int ip_len, uint8_t proto,
                                       unsigned int src_host, unsigned int dst_host) {
  struct iphdr ip {};
  ip.version = 4;
  ip.ihl = 5;
  ip.tot_len = htons(static_cast<uint16_t>(ip_len));
  ip.ttl = 64;
  ip.protocol = proto;
  ip.saddr = src_host;
  ip.daddr = dst_host;
  return write_packet(burst, idx, sizeof(struct ethhdr), &ip, sizeof(ip));
}

Status GpunetioEngine::set_udp_header(BurstParams* burst, int idx, int udp_len, uint16_t src_port,
                                      uint16_t dst_port) {
  struct udphdr udp {};
  udp.source = htons(src_port);
  udp.dest = htons(dst_port);
  udp.len = htons(static_cast<uint16_t>(udp_len));
  return write_packet(burst, idx, sizeof(struct ethhdr) + sizeof(struct iphdr), &udp, sizeof(udp));
}

Status GpunetioEngine::set_udp_payload(BurstParams* burst, int idx, void* data, int len) {
  return write_packet(burst, idx, sizeof(UDPIPV4Pkt), data, static_cast<size_t>(len));
}

Status GpunetioEngine::send_tx_burst(BurstParams* burst) {
  if (burst == nullptr) {
    return Status::NULL_PTR;
  }
  TxQueue* q = find_tx_queue(burst->hdr.hdr.port_id, burst->hdr.hdr.q_id);
  if (q == nullptr) {
    return Status::INVALID_PARAMETER;
  }
  if (q->failed.load(std::memory_order_relaxed)) {
    return Status::GENERIC_FAILURE;
  }
  if (!q->send_ring->enqueue(burst)) {
    // The worker is behind: give back the slots of this burst
    BurstPriv priv{};
    if (get_priv(burst, &priv)) {
      q->retire(priv.seq, priv.reserved);
    }
    q->send_ring_full.fetch_add(1, std::memory_order_relaxed);
    tx_meta_pool_->put(burst);
    return Status::NO_SPACE_AVAILABLE;
  }
  return Status::SUCCESS;
}

void GpunetioEngine::free_tx_burst(BurstParams* burst) {
  if (burst != nullptr && tx_meta_pool_ != nullptr) {
    tx_meta_pool_->put(burst);
  }
}

void GpunetioEngine::free_tx_metadata(BurstParams* burst) {
  free_tx_burst(burst);
}

}  // namespace daqiri
