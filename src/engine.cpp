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

#include "src/engine.h"
// Include the appropriate headers based on which DAQIRI_ENGINE types are defined
#if DAQIRI_ENGINE_DPDK
#include "src/engines/dpdk/daqiri_dpdk_engine.h"
#endif
#if DAQIRI_ENGINE_SOCKET
#include "src/engines/socket/daqiri_socket_engine.h"
#endif
#if DAQIRI_ENGINE_RDMA
#include "src/engines/rdma/daqiri_rdma_engine.h"
#endif

#if DAQIRI_ENGINE_DPDK || DAQIRI_ENGINE_SOCKET || DAQIRI_ENGINE_RDMA
#include <rte_common.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_mbuf.h>
#include <rte_ethdev.h>
#include <rte_ring.h>
#include <rte_eal.h>
#include <rte_version.h>
#endif

#include <chrono>
#include <cstdlib>
#include <cuda.h>
#include <dirent.h>
#include <fstream>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <unistd.h>

#include <daqiri/logging.hpp>

namespace daqiri {

// Initialize static members
std::unique_ptr<Engine> EngineFactory::EngineInstance_ = nullptr;  // Initialize static members
EngineType EngineFactory::EngineType_ = EngineType::UNKNOWN;

extern void initialize_engine(Engine* _engine);

namespace {

SocketProtocol protocol_from_endpoint_addr(const std::string& addr) {
  const auto scheme_end = addr.find("://");
  if (scheme_end == std::string::npos) { return SocketProtocol::INVALID; }
  const auto scheme = addr.substr(0, scheme_end);
  if (scheme == DAQIRI_SOCKET_PROTOCOL_STR__TCP) { return SocketProtocol::TCP; }
  if (scheme == DAQIRI_SOCKET_PROTOCOL_STR__UDP) { return SocketProtocol::UDP; }
  if (scheme == DAQIRI_ENGINE_STR__RDMA || scheme == DAQIRI_SOCKET_PROTOCOL_STR__ROCE) {
    return SocketProtocol::ROCE;
  }
  return SocketProtocol::INVALID;
}

}  // namespace

std::string Engine::generate_random_string(int len) {
  constexpr char tokens[] = "abcdefghijklmnopqrstuvwxyz";
  if (len <= 0) { return {}; }

  std::random_device random_device;
  const auto timestamp = static_cast<uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  std::seed_seq seed{
      random_device(),
      random_device(),
      static_cast<uint32_t>(timestamp),
      static_cast<uint32_t>(timestamp >> 32U),
      static_cast<uint32_t>(getpid()),
  };
  std::mt19937 rng(seed);
  std::uniform_int_distribution<size_t> dist(0, sizeof(tokens) - 2);

  std::string tmp;
  tmp.reserve(static_cast<size_t>(len));
  for (int i = 0; i < len; i++) { tmp += tokens[dist(rng)]; }

  return tmp;
}

EngineType EngineFactory::get_default_engine_type() {
#if DAQIRI_ENGINE_DPDK
  return EngineType::DPDK;
#elif DAQIRI_ENGINE_SOCKET
  return EngineType::SOCKET;
#elif DAQIRI_ENGINE_RDMA
  return EngineType::RDMA;
#else
#error "No DAQIRI engine defined"
#endif
}

std::unique_ptr<Engine> EngineFactory::create_instance(EngineType type) {
  std::unique_ptr<Engine> _engine;
  switch (type) {
#if DAQIRI_ENGINE_DPDK
    case EngineType::DPDK:
      _engine = std::make_unique<DpdkEngine>();
      break;
#endif
#if DAQIRI_ENGINE_SOCKET
    case EngineType::SOCKET:
      _engine = std::make_unique<SocketEngine>();
      break;
#endif
#if DAQIRI_ENGINE_RDMA
    case EngineType::RDMA:
      _engine = std::make_unique<RdmaEngine>();
      break;
#endif
    case EngineType::DEFAULT:
      _engine = create_instance(get_default_engine_type());
      return _engine;
    default:
      throw std::invalid_argument(
          "Engine type '" + engine_type_to_string(type) +
          "' is not available in this build");
  }

  // Initialize the ADV Net Common API
  initialize_engine(_engine.get());
  return _engine;
}

template <typename Config>
EngineType EngineFactory::get_engine_type(const Config& config) {
  // Ensure that Config has a method yaml_nodes() that returns a collection
  // of YAML nodes
  static_assert(
      std::is_member_function_pointer<decltype(&Config::yaml_nodes)>::value,
      "Config type must have a method yaml_nodes() that returns a collection of YAML nodes");

  auto& yaml_nodes = config.yaml_nodes();
  for (const auto& yaml_node : yaml_nodes) {
    try {
      auto node = yaml_node["daqiri"]["cfg"];
      const std::string stream_type_str = node["stream_type"].template as<std::string>("");
      const auto stream_type = stream_type_from_string(stream_type_str);
      if (stream_type == StreamType::INVALID) { continue; }

      const std::string engine_str = node["engine"].template as<std::string>("");
      if (!engine_str.empty() && engine_str != DAQIRI_ENGINE_STR__DEFAULT) {
        return config_engine_from_string(engine_str);
      }

      // Protocol is derived from the endpoint URI scheme (udp://, tcp://,
      // roce://), not from a config field.
      SocketProtocol protocol = SocketProtocol::INVALID;
      auto interfaces_node = node["interfaces"];
      for (const auto& intf : interfaces_node) {
        auto socket_config_node = intf["socket_config"];
        if (!socket_config_node.IsDefined()) { continue; }
        protocol = protocol_from_endpoint_addr(
            socket_config_node["local_addr"].template as<std::string>(""));
        if (protocol == SocketProtocol::INVALID) {
          protocol = protocol_from_endpoint_addr(
              socket_config_node["remote_addr"].template as<std::string>(""));
        }
        if (protocol != SocketProtocol::INVALID) { break; }
      }

      return engine_type_from_stream_type(stream_type, protocol);
    } catch (const std::exception& e) {
      return get_default_engine_type();
    }
  }

  return get_default_engine_type();
}

size_t Engine::get_alignment(MemoryKind kind) {
  switch (kind) {
    case MemoryKind::HOST:
    case MemoryKind::HOST_PINNED:
    case MemoryKind::HUGE:
      return 128;  // Twice the size of a cache line on the CPU
    case MemoryKind::DEVICE:
      return 256;  // Twice the cache line size on the GPU
    default:
      return 128;
  }
}

Status Engine::populate_pool(struct rte_ring* ring, const std::string& mr_name) {
  auto mr = cfg_.mrs_[mr_name];
  auto base = reinterpret_cast<char*>(ar_[mr_name].ptr_);

  for (size_t i = 0; i < mr.num_bufs_; i++) {
    if (rte_ring_enqueue(ring, base + i * mr.adj_size_) != 0) {
      DAQIRI_LOG_CRITICAL("Failed to enqueue buffer {} to ring", i);
      return Status::NULL_PTR;
    }
  }
  return Status::SUCCESS;
}

#if DAQIRI_ENGINE_DPDK || DAQIRI_ENGINE_RDMA

Engine::HugepageEstimate Engine::estimate_required_hugepages() const {
  HugepageEstimate est;
  est.eal_fixed_bytes = DPDK_EAL_FIXED_OVERHEAD;

  for (const auto& [name, mr] : cfg_.mrs_) {
    const size_t elt = mr.adj_size_ != 0 ? mr.adj_size_ : mr.buf_size_;
    if (mr.kind_ == MemoryKind::HUGE) {
      est.huge_mr_bytes += static_cast<size_t>(mr.num_bufs_) * elt;
      ++est.huge_mr_count;
    } else {
      ++est.extbuf_pool_count;
    }
  }
  est.pool_overhead_bytes =
      (est.huge_mr_count + est.extbuf_pool_count) * DPDK_PER_POOL_HUGEPAGE_OVERHEAD;

  // DpdkEngine injects a kind: HUGE dummy MR (32768 bufs * JUMBOFRAME_SIZE) for
  // every interface that has no TX or no RX queue configured. Account for it
  // here so the preflight matches what initialize() will actually request.
  // JUMBOFRAME_SIZE lives in DpdkEngine; use a portable upper bound (9100).
  constexpr size_t kDummyJumboFrameSize = 9100;
  constexpr size_t kDummyNumBufs = 32768;
  for (const auto& intf : cfg_.ifs_) {
    if (intf.rx_.queues_.empty()) {
      est.dummy_queue_bytes += kDummyNumBufs * kDummyJumboFrameSize;
      est.pool_overhead_bytes += DPDK_PER_POOL_HUGEPAGE_OVERHEAD;
      ++est.dummy_queue_count;
    }
    if (intf.tx_.queues_.empty()) {
      est.dummy_queue_bytes += kDummyNumBufs * kDummyJumboFrameSize;
      est.pool_overhead_bytes += DPDK_PER_POOL_HUGEPAGE_OVERHEAD;
      ++est.dummy_queue_count;
    }
  }

  est.total_bytes = est.eal_fixed_bytes + est.huge_mr_bytes +
                    est.pool_overhead_bytes + est.dummy_queue_bytes;
  return est;
}

size_t Engine::available_hugepage_bytes() {
  const char kSysHugepageDir[] = "/sys/kernel/mm/hugepages";
  DIR* dir = opendir(kSysHugepageDir);
  if (dir == nullptr) { return 0; }

  size_t total = 0;
  static const std::regex kSizeRe("^hugepages-([0-9]+)kB$");
  for (struct dirent* ent = readdir(dir); ent != nullptr; ent = readdir(dir)) {
    std::cmatch m;
    if (!std::regex_match(ent->d_name, m, kSizeRe)) { continue; }
    const size_t page_kb = std::stoull(m[1].str());

    std::ifstream fs(std::string(kSysHugepageDir) + "/" + ent->d_name + "/free_hugepages");
    if (!fs.is_open()) { continue; }
    size_t free_pages = 0;
    fs >> free_pages;
    if (!fs.fail()) { total += free_pages * page_kb * 1024UL; }
  }
  closedir(dir);
  return total;
}

bool Engine::check_hugepage_availability() const {
  const HugepageEstimate est = estimate_required_hugepages();
  const size_t avail = available_hugepage_bytes();
  const auto mib = [](size_t b) { return b / (1024.0 * 1024.0); };

  if (avail == 0) {
    DAQIRI_LOG_WARN(
        "Could not read /sys/kernel/mm/hugepages; skipping hugepage preflight. "
        "If init fails, verify hugepages are configured per "
        "docs/tutorials/system_configuration.md");
    return true;
  }
  if (avail >= est.total_bytes) {
    DAQIRI_LOG_INFO(
        "Hugepage preflight OK: {:.0f} MiB free, ~{:.0f} MiB required "
        "({} kind:HUGE MRs={:.0f} MiB, {} dummy queue(s)={:.0f} MiB, "
        "{} pool overhead={:.0f} MiB, EAL fixed={:.0f} MiB)",
        mib(avail), mib(est.total_bytes),
        est.huge_mr_count, mib(est.huge_mr_bytes),
        est.dummy_queue_count, mib(est.dummy_queue_bytes),
        est.huge_mr_count + est.extbuf_pool_count + est.dummy_queue_count,
        mib(est.pool_overhead_bytes),
        mib(est.eal_fixed_bytes));
    return true;
  }

  DAQIRI_LOG_CRITICAL(
      "Insufficient free hugepages: {:.0f} MiB free, ~{:.0f} MiB required for this config.",
      mib(avail), mib(est.total_bytes));
  DAQIRI_LOG_CRITICAL("Breakdown of the requirement:");
  DAQIRI_LOG_CRITICAL(
      "  - {} memory_region(s) with kind: HUGE (full size in hugepages): {:.0f} MiB",
      est.huge_mr_count, mib(est.huge_mr_bytes));
  DAQIRI_LOG_CRITICAL(
      "  - {} dummy queue(s) auto-injected for interfaces missing TX or RX "
      "(32768 bufs x 9100 B each, kind: HUGE): {:.0f} MiB",
      est.dummy_queue_count, mib(est.dummy_queue_bytes));
  DAQIRI_LOG_CRITICAL(
      "  - DPDK per-pool overhead ({} pools x ~{:.0f} MiB = mbuf headers, mempool ring, "
      "per-lcore caches): {:.0f} MiB",
      est.huge_mr_count + est.extbuf_pool_count + est.dummy_queue_count,
      mib(DPDK_PER_POOL_HUGEPAGE_OVERHEAD),
      mib(est.pool_overhead_bytes));
  DAQIRI_LOG_CRITICAL(
      "  - DPDK/EAL fixed overhead (services, memzones, ethdev tables): {:.0f} MiB",
      mib(est.eal_fixed_bytes));
  DAQIRI_LOG_CRITICAL(
      "Tip: lowering memory_regions[*].num_bufs / buf_size in your YAML reduces the "
      "kind:HUGE total; the dummy-queue cost only applies to interfaces with no TX or "
      "no RX queues configured.");
  DAQIRI_LOG_CRITICAL(
      "Configure more hugepages before starting (see "
      "docs/tutorials/system_configuration.md \"Enable Huge pages\"), for example:");
  const size_t need_2mib_pages =
      (est.total_bytes + (2UL * 1024 * 1024) - 1) / (2UL * 1024 * 1024);
  const size_t need_1gib_pages =
      (est.total_bytes + (1024UL * 1024 * 1024) - 1) / (1024UL * 1024 * 1024);
  DAQIRI_LOG_CRITICAL(
      "  echo {} | sudo tee /proc/sys/vm/nr_hugepages                                   "
      "# 2 MiB pool ({} pages x 2 MiB = {:.0f} MiB)",
      need_2mib_pages, need_2mib_pages, need_2mib_pages * 2.0);
  DAQIRI_LOG_CRITICAL(
      "  echo {} | sudo tee /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages   "
      "# 1 GiB pool ({} pages x 1 GiB = {} MiB)",
      need_1gib_pages, need_1gib_pages, need_1gib_pages * 1024UL);
  DAQIRI_LOG_CRITICAL(
      "Verify with: grep Huge /proc/meminfo");
  DAQIRI_LOG_CRITICAL(
      "For a persistent allocation across reboots, add hugepagesz=/hugepages= to the "
      "kernel cmdline per docs/tutorials/system_configuration.md.");
  DAQIRI_LOG_CRITICAL(
      "If a previous run failed mid-init, also remove its leftover files:");
  DAQIRI_LOG_CRITICAL(
      "  sudo rm -f /dev/hugepages/*map_* /mnt/huge/*map_*");
  return false;
}

void Engine::cleanup_eal() {
  if (!eal_initialized_) { return; }

  // rte_eal_cleanup() releases EAL state and (on DPDK >= 22.07) unlinks the
  // per-segment hugepage files this process created. Older DPDK leaves them
  // behind, so we also do a best-effort unlink targeted at our --file-prefix.
  rte_eal_cleanup();

  if (!eal_file_prefix_.empty()) {
    static const char* kHugepageMounts[] = {"/dev/hugepages", "/mnt/huge"};
    for (const char* mount : kHugepageMounts) {
      DIR* dir = opendir(mount);
      if (dir == nullptr) { continue; }
      for (struct dirent* ent = readdir(dir); ent != nullptr; ent = readdir(dir)) {
        const std::string name = ent->d_name;
        if (name.find(eal_file_prefix_) != std::string::npos &&
            name.find("map_") != std::string::npos) {
          const std::string full = std::string(mount) + "/" + name;
          if (unlink(full.c_str()) == 0) {
            DAQIRI_LOG_INFO("Removed leftover hugepage file {}", full);
          }
        }
      }
      closedir(dir);
    }
  }

  eal_initialized_ = false;
  eal_file_prefix_.clear();
}

#else  // !DAQIRI_ENGINE_DPDK && !DAQIRI_ENGINE_RDMA

size_t Engine::estimate_required_hugepage_bytes() const { return 0; }
size_t Engine::available_hugepage_bytes() { return 0; }
bool Engine::check_hugepage_availability() const { return true; }
void Engine::cleanup_eal() {}

#endif  // DAQIRI_ENGINE_DPDK || DAQIRI_ENGINE_RDMA

Status Engine::allocate_memory_regions() {
  DAQIRI_LOG_INFO("Registering memory regions");
#if DAQIRI_ENGINE_DPDK || DAQIRI_ENGINE_RDMA
  for (auto& mr : cfg_.mrs_) {
    void* ptr;
    AllocRegion ar;
    mr.second.ttl_size_ = RTE_ALIGN_CEIL(mr.second.adj_size_ * mr.second.num_bufs_, GPU_PAGE_SIZE);

    if (mr.second.owned_) {
      switch (mr.second.kind_) {
        case MemoryKind::HOST:
          if (posix_memalign(&ptr, GPU_PAGE_SIZE, mr.second.ttl_size_) != 0) {
            DAQIRI_LOG_CRITICAL("Failed to allocate aligned host memory!");
            return Status::NULL_PTR;
          }
          break;
        case MemoryKind::HOST_PINNED:
          cudaSetDevice(mr.second.affinity_);
          if (cudaHostAlloc(&ptr, mr.second.ttl_size_, 0) != cudaSuccess) {
            DAQIRI_LOG_CRITICAL("Failed to allocate CUDA pinned host memory!");
            return Status::NULL_PTR;
          }
          break;
        case MemoryKind::HUGE:
          ptr = rte_malloc_socket(nullptr, mr.second.ttl_size_, 0, mr.second.affinity_);
          break;
        case MemoryKind::DEVICE: {
          unsigned int flag = 1;
          const auto align = RTE_ALIGN_CEIL(mr.second.ttl_size_, GPU_PAGE_SIZE);
          CUdeviceptr cuptr;

          cudaSetDevice(mr.second.affinity_);
          cudaFree(0);  // Create primary context if it doesn't exist
          const auto alloc_res = cuMemAlloc(&cuptr, align);

          if (alloc_res != CUDA_SUCCESS) {
            const char* err_str = nullptr;
            cuGetErrorString(alloc_res, &err_str);
            DAQIRI_LOG_CRITICAL(
                "Could not allocate {:.2f}MB of GPU memory. Error: {}", align / 1e6, err_str);
            return Status::NULL_PTR;
          }

          ptr = reinterpret_cast<void*>(cuptr);

          const auto attr_res =
              cuPointerSetAttribute(&flag, CU_POINTER_ATTRIBUTE_SYNC_MEMOPS, cuptr);
          if (attr_res != CUDA_SUCCESS) {
            DAQIRI_LOG_CRITICAL("Could not set pointer attributes");
            return Status::NULL_PTR;
          }
          break;
        }
        default:
          DAQIRI_LOG_ERROR("Unknown memory type {}!", static_cast<int>(mr.second.kind_));
          return Status::INVALID_PARAMETER;
      }

      if (ptr == nullptr) {
        DAQIRI_LOG_CRITICAL("Fatal to allocate {} of type {} for MR",
                              mr.second.ttl_size_,
                              static_cast<int>(mr.second.kind_));
        return Status::NULL_PTR;
      }
    }

    DAQIRI_LOG_INFO(
        "Successfully allocated memory region {} at {} type {} with {} bytes "
        "({} elements @ {} bytes total {})",
        mr.second.name_,
        ptr,
        (int)mr.second.kind_,
        mr.second.buf_size_,
        mr.second.num_bufs_,
        mr.second.adj_size_,
        mr.second.ttl_size_);
    ar_[mr.second.name_] = {mr.second.name_, ptr};
  }
#endif
  DAQIRI_LOG_INFO("Finished allocating memory regions");
  return Status::SUCCESS;
}

Status Engine::map_memory_regions() {
  // Map every MR to every device for now
  for (const auto& intf : cfg_.ifs_) {
    struct rte_eth_dev_info dev_info;
    int ret = rte_eth_dev_info_get(intf.port_id_, &dev_info);
    if (ret != 0) {
      DAQIRI_LOG_CRITICAL("Failed to get device info for port {}", intf.port_id_);
      return Status::NULL_PTR;
    }

    for (const auto& ext_mem_el : ext_pktmbufs_) {
      const auto& ext_mem = ext_mem_el.second;
      const auto& mr = cfg_.mrs_[ext_mem_el.first];

      if (mr.kind_ != MemoryKind::DEVICE) { continue; }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
      ret = rte_dev_dma_map(dev_info.device, ext_mem->buf_ptr, ext_mem->buf_iova, ext_mem->buf_len);
#pragma GCC diagnostic pop

      if (ret) {
        DAQIRI_LOG_CRITICAL(
            "Could not DMA map EXT memory: {} err={}", ret, rte_strerror(rte_errno));
        return Status::NULL_PTR;
      }

      DAQIRI_LOG_INFO(
          "Mapped external memory descriptor for {} to device {}", ext_mem->buf_ptr, intf.port_id_);
    }
  }

  return Status::SUCCESS;
}

// Register memory regions with the RTE library. If using DPDK as the engine to create memory
// regions/pools, this function can register external memory regions (such as GPU memory).
Status Engine::register_memory_regions() {
  for (const auto& ar : ar_) {
    const auto& mr = cfg_.mrs_[ar.second.mr_name_];

    // Hugepages use the normal rte functions that don't require extmem
    if (mr.kind_ == MemoryKind::HUGE) { continue; }

    auto ext_mem = std::make_shared<struct rte_pktmbuf_extmem>();
    ext_mem->buf_len = mr.ttl_size_;
    ext_mem->buf_iova = RTE_BAD_IOVA;
    ext_mem->buf_ptr = ar.second.ptr_;
    ext_mem->elt_size = mr.adj_size_;

    int ret = 0;
    if (mr.kind_ == MemoryKind::DEVICE) {
      int flag = 0;
      CUresult s =
          cuDeviceGetAttribute(&flag, CU_DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED, mr.affinity_);
      if (s != CUDA_SUCCESS) {
        DAQIRI_LOG_CRITICAL("Failed to get dma-buf supported for device {}", mr.affinity_);
        return Status::NULL_PTR;
      }

      if (flag == 0) {
        DAQIRI_LOG_WARN(
            "dma-buf not supported for device {}. Attempting to use nvidia-peermem",
            mr.affinity_);
        // GPUs have the largest page size vs CPUs, so just use that
        ret = rte_extmem_register(
            ext_mem->buf_ptr, ext_mem->buf_len, NULL, ext_mem->buf_iova, GPU_PAGE_SIZE);
      } else {
        DAQIRI_LOG_INFO("dma-buf supported for device {}", mr.affinity_);

        const size_t host_page_size = sysconf(_SC_PAGESIZE);
        const auto base_addr = reinterpret_cast<uintptr_t>(ext_mem->buf_ptr);
        const auto aligned_addr = base_addr & ~(static_cast<uintptr_t>(host_page_size) - 1);
        const auto offset = base_addr - aligned_addr;
        const auto aligned_size =
            (ext_mem->buf_len + offset + host_page_size - 1) & ~(host_page_size - 1);
        const CUdeviceptr aligned_ptr = static_cast<CUdeviceptr>(aligned_addr);

        DAQIRI_LOG_INFO("dma-buf GPU buffer address at {} aligned at {} with aligned size {}",
                        ext_mem->buf_ptr,
                        reinterpret_cast<void*>(aligned_addr),
                        aligned_size);

        int dmabuf_fd = 0;
        CUresult error = cuMemGetHandleForAddressRange(
            reinterpret_cast<void*>(&dmabuf_fd),
            aligned_ptr,
            aligned_size,
            CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD,
            0);

        if (error != CUDA_SUCCESS) {
          DAQIRI_LOG_CRITICAL("cuMemGetHandleForAddressRange error={}. Falling back to peermem",
                              static_cast<int>(error));
          // GPUs have the largest page size vs CPUs, so just use that
          ret = rte_extmem_register(
              ext_mem->buf_ptr, ext_mem->buf_len, NULL, ext_mem->buf_iova, GPU_PAGE_SIZE);
        } else {
#if RTE_VERSION >= RTE_VERSION_NUM(24, 11, 0, 0)
          ret = rte_extmem_register_dmabuf(
              ext_mem->buf_ptr, ext_mem->buf_len, dmabuf_fd, offset, NULL, 0, GPU_PAGE_SIZE);
#else
          DAQIRI_LOG_WARN(
              "rte_extmem_register_dmabuf unavailable in DPDK {}; falling back to peermem "
              "registration",
              rte_version());
          close(dmabuf_fd);
          ret = rte_extmem_register(
              ext_mem->buf_ptr, ext_mem->buf_len, NULL, ext_mem->buf_iova, GPU_PAGE_SIZE);
#endif
        }
      }
    } else {
      const unsigned int n_pages = static_cast<unsigned int>(ext_mem->buf_len / GPU_PAGE_SIZE);
      ret = rte_extmem_register(
          ext_mem->buf_ptr, ext_mem->buf_len, NULL, n_pages, GPU_PAGE_SIZE);
    }
    if (ret) {
      if (mr.kind_ == MemoryKind::DEVICE) {
        DAQIRI_LOG_CRITICAL(
            "Unable to register addr {}, ret {} errno {}. Either nvidia-peermem is not running "
            "or the memory kind is not supported",
            ext_mem->buf_ptr,
            ret,
            rte_strerror(rte_errno));
      } else {
        DAQIRI_LOG_CRITICAL("Unable to register addr {}, ret {} errno {} for memory kind {}",
                            ext_mem->buf_ptr,
                            ret,
                            rte_strerror(rte_errno),
                            static_cast<int>(mr.kind_));
      }
      return Status::NULL_PTR;
    } else {
      DAQIRI_LOG_INFO("Successfully registered external memory for {}", mr.name_);
    }

    ext_pktmbufs_[mr.name_] = ext_mem;
  }

  return Status::SUCCESS;
}

struct rte_mempool* Engine::create_pktmbuf_pool(const std::string& name,
                                                 const MemoryRegionConfig& mr) {
  struct rte_mempool* pool;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  if (mr.kind_ == MemoryKind::HUGE) {
    pool =
        rte_pktmbuf_pool_create(name.c_str(), mr.num_bufs_, 0, 0, mr.adj_size_, numa_from_mem(mr));
  } else {
    auto pktmbuf = ext_pktmbufs_[mr.name_];
    pool = rte_pktmbuf_pool_create_extbuf(
        name.c_str(), mr.num_bufs_, 0, 0, mr.adj_size_, numa_from_mem(mr), pktmbuf.get(), 1);
  }
#pragma GCC diagnostic pop

  return pool;
}

struct rte_mempool* Engine::create_generic_pool(const std::string& name,
                                                 const MemoryRegionConfig& mr) {
  struct rte_mempool* pool;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  pool = rte_mempool_create_empty(name.c_str(),
                                  mr.num_bufs_,
                                  mr.adj_size_,
                                  0,
                                  sizeof(struct rte_pktmbuf_pool_private),
                                  numa_from_mem(mr),
                                  0);

  // Now we have to populate the memory pool with the correct memory region buffers
  if (pool == nullptr) {
    DAQIRI_LOG_ERROR("Failed to create empty mempool {}", name);
    return nullptr;
  }

  rte_pktmbuf_pool_init(pool, nullptr);

  auto ar_it = ar_.find(mr.name_);
  if (ar_it == ar_.end()) {
    DAQIRI_LOG_ERROR(
        "Memory region {} not found in allocated regions for pool {}", mr.name_, name);
    rte_mempool_free(pool);
    return nullptr;
  }

  const AllocRegion& alloc_region = ar_it->second;
  size_t total_size = static_cast<size_t>(mr.num_bufs_) * mr.adj_size_;
  size_t page_size = mr.adj_size_;  // Default to adjusted size (element size)

  if (mr.kind_ == MemoryKind::DEVICE) {
    page_size = GPU_PAGE_SIZE;
  } else {
    page_size = 4096;
  }

  DAQIRI_LOG_INFO("Populating mempool {} for MR {} with {} objects of size {} at VA {} ",
                    name,
                    mr.name_,
                    mr.num_bufs_,
                    mr.adj_size_,
                    alloc_region.ptr_);
  // Check if the allocated size matches the expected total size
  // Note: AllocRegion might store the originally requested size (buf_size_ * num_bufs_)
  // or the adjusted size. Assuming it holds the base pointer to the whole region.
  // We need the IOVA of the start of the buffer.
  int ret = rte_mempool_populate_iova(pool,
                                      static_cast<char*>(alloc_region.ptr_),
                                      RTE_BAD_IOVA,
                                      total_size,
                                      nullptr,
                                      nullptr);  // Opaque data for callback

  if (ret < 0) {
    DAQIRI_LOG_ERROR(
        "Failed to populate mempool {} for MR {}: {}", name, mr.name_, rte_strerror(rte_errno));
    rte_mempool_free(pool);
    return nullptr;
  } else if (static_cast<unsigned>(ret) != mr.num_bufs_) {
    DAQIRI_LOG_WARN("Populated mempool {} for MR {} with {} objects, expected {}",
                      name,
                      mr.name_,
                      ret,
                      mr.num_bufs_);
    // This might not be critical depending on how sizes align, but worth noting.
  } else {
    DAQIRI_LOG_INFO(
        "Successfully populated generic mempool {} for MR {} with {} objects of size {} at VA {} ",
        name,
        mr.name_,
        ret,
        mr.adj_size_,
        alloc_region.ptr_);
  }
#pragma GCC diagnostic pop

  return pool;
}

int Engine::numa_from_mem(const MemoryRegionConfig& mr) const {
  if (mr.kind_ == MemoryKind::DEVICE) {
    int val;
    if (cudaDeviceGetAttribute(&val, cudaDevAttrHostNumaId, mr.affinity_) != cudaSuccess) {
      DAQIRI_LOG_ERROR("Failed to get NUMA node from device {}", mr.affinity_);
      return -1;
    }

    return val;
  } else {
    return mr.affinity_;
  }
}

/**
 * @brief Generic implementation of get_port_id that looks up port in config
 * This is a final method that cannot be overridden by subclasses.
 *
 * @param key PCIe address, IP address, or config name of the interface to look up
 * @return int Port ID or -1 if not found
 */
int Engine::get_port_id(const std::string& key) {
  for (const auto& intf : cfg_.ifs_) {
    if (intf.address_ == key) { return intf.port_id_; }
    if (intf.name_ == key) { return intf.port_id_; }
  }
  return -1;
}

bool Engine::validate_config() const {
  bool pass = true;
  std::set<std::string> mr_names;
  std::set<std::string> q_mr_names;

  // Verify all memory regions are used in queues and all queue MRs are listed in the MR section
  for (const auto& mr : cfg_.mrs_) { mr_names.emplace(mr.second.name_); }

  for (const auto& intf : cfg_.ifs_) {
    for (const auto& rxq : intf.rx_.queues_) {
      for (const auto& mr : rxq.common_.mrs_) { q_mr_names.emplace(mr); }
    }
    for (const auto& txq : intf.tx_.queues_) {
      for (const auto& mr : txq.common_.mrs_) { q_mr_names.emplace(mr); }
    }
  }

  // All MRs are in queues
  for (const auto& mr : mr_names) {
    if (q_mr_names.find(mr) == q_mr_names.end()) {
      DAQIRI_LOG_WARN("Extra MR section with name {} unused in queues section", mr);
    }
  }

  // All queue MRs are in MR list
  for (const auto& mr : q_mr_names) {
    if (mr_names.find(mr) == mr_names.end()) {
      DAQIRI_LOG_ERROR(
          "Queue found using MR {}, but that MR doesn't exist in the memory_region config", mr);
      pass = false;
    }
  }

  return pass;
}

void Engine::init_rx_core_q_map() {
  for (const auto& intf : cfg_.ifs_) {
    // Initialize the round-robin index for this port
    next_queue_index_map_.try_emplace(intf.port_id_, 0);

    for (const auto& q : intf.rx_.queues_) {
      int cpu_core = strtol(q.common_.cpu_core_.c_str(), nullptr, 10);
      rx_core_q_map[cpu_core].push_back(std::make_pair(intf.port_id_, q.common_.id_));

      if (rx_core_q_map[cpu_core].size() > MAX_RX_Q_PER_CORE) {
        DAQIRI_LOG_CRITICAL("Too many RX queues assigned to core {}!", cpu_core);
      }
    }
  }
}

uint16_t Engine::get_num_rx_queues(int port_id) const {
  return cfg_.ifs_[port_id].rx_.queues_.size();
}

void Engine::flush_port_queue(int port, int queue) {
  DAQIRI_LOG_ERROR("flush_port_queue not implemented for this engine type");
}

Status Engine::drop_all_traffic(int port) {
  DAQIRI_LOG_ERROR("drop_all_traffic not implemented for this engine type");
  return Status::NOT_SUPPORTED;
}

Status Engine::allow_all_traffic(int port) {
  DAQIRI_LOG_ERROR("allow_all_traffic not implemented for this engine type");
  return Status::NOT_SUPPORTED;
}

Status Engine::get_rx_burst(BurstParams** burst, int port_id) {
  // Check if the port_id is valid
  if (port_id < 0 || port_id >= static_cast<int>(cfg_.ifs_.size())) {
    DAQIRI_LOG_ERROR("Invalid port_id {} provided to get_rx_burst", port_id);
    return Status::INVALID_PARAMETER;
  }

  const auto& queues = cfg_.ifs_[port_id].rx_.queues_;
  size_t num_queues = queues.size();
  size_t& next_queue_index = next_queue_index_map_[port_id];

  // Check all queues once, starting from the next index
  for (size_t i = 0; i < num_queues; ++i) {
    size_t check_index = (next_queue_index + i) % num_queues;
    int queue_id = queues[check_index].common_.id_;

    Status ret = get_rx_burst(burst, port_id, queue_id);
    if (ret != Status::NULL_PTR) {
      // Got something, update index for next time and return status
      next_queue_index = (check_index + 1) % num_queues;
      return ret;
    }
  }

  // If we checked all queues and none had data
  return Status::NULL_PTR;
}

Status Engine::get_rx_burst(BurstParams** burst) {
  if (cfg_.ifs_.empty()) {
    DAQIRI_LOG_ERROR("No interfaces configured");
    return Status::NULL_PTR;
  }

  size_t num_interfaces = cfg_.ifs_.size();

  // Check all queues once, starting from the next index
  for (size_t i = 0; i < num_interfaces; ++i) {
    size_t check_index = (next_port_index_ + i) % num_interfaces;
    int port_id = cfg_.ifs_[check_index].port_id_;

    Status ret = get_rx_burst(burst, port_id);
    if (ret != Status::NULL_PTR) {
      // Got something, update index for next time and return status
      next_port_index_ = (check_index + 1) % num_interfaces;
      return ret;
    }
  }

  // If we checked all interfaces and none yielded a burst
  return Status::NULL_PTR;
}

Status Engine::socket_connect_to_server(const std::string& dst_addr, uint16_t dst_port,
                                         uintptr_t* conn_id) {
  DAQIRI_LOG_CRITICAL("Socket connect to server not implemented");
  return Status::NOT_SUPPORTED;
}

Status Engine::socket_connect_to_server(const std::string& dst_addr, uint16_t dst_port,
                                         const std::string& src_addr, uintptr_t* conn_id) {
  DAQIRI_LOG_CRITICAL("Socket connect to server not implemented");
  return Status::NOT_SUPPORTED;
}

Status Engine::socket_get_port_queue(uintptr_t conn_id, uint16_t* port, uint16_t* queue) {
  DAQIRI_LOG_CRITICAL("Socket get port queue not implemented");
  return Status::NOT_SUPPORTED;
}

Status Engine::socket_get_server_conn_id(const std::string& server_addr, uint16_t server_port,
                                          uintptr_t* conn_id) {
  DAQIRI_LOG_CRITICAL("Socket get server conn ID not implemented");
  return Status::NOT_SUPPORTED;
}

Status Engine::rdma_connect_to_server(const std::string& dst_addr, uint16_t dst_port,
                                       uintptr_t* conn_id) {
  DAQIRI_LOG_CRITICAL("RDMA connect to server not implemented");
  return Status::NOT_SUPPORTED;
}

Status Engine::rdma_connect_to_server(const std::string& dst_addr, uint16_t dst_port,
                                       const std::string& src_addr, uintptr_t* conn_id) {
  DAQIRI_LOG_CRITICAL("RDMA connect to server not implemented");
  return Status::NOT_SUPPORTED;
}

Status Engine::rdma_get_port_queue(uintptr_t conn_id, uint16_t* port, uint16_t* queue) {
  DAQIRI_LOG_CRITICAL("RDMA get port queue not implemented");
  return Status::NOT_SUPPORTED;
}

Status Engine::rdma_get_server_conn_id(const std::string& server_addr, uint16_t server_port,
                                        uintptr_t* conn_id) {
  DAQIRI_LOG_CRITICAL("RDMA get server conn ID not implemented");
  return Status::NOT_SUPPORTED;
}

Status Engine::get_rx_burst(BurstParams** burst, uintptr_t conn_id, bool server) {
  DAQIRI_LOG_CRITICAL("RDMA get RX burst not implemented");
  return Status::NOT_SUPPORTED;
}

Status Engine::set_all_packet_lengths(BurstParams* burst,
                                       const std::initializer_list<int>& lens) {
  if (burst == nullptr) { return Status::NULL_PTR; }
  for (size_t idx = 0; idx < burst->hdr.hdr.num_pkts; ++idx) {
    const auto status = set_packet_lengths(burst, static_cast<int>(idx), lens);
    if (status != Status::SUCCESS) { return status; }
  }
  return Status::SUCCESS;
}

Status Engine::set_reorder_cuda_stream(const std::string& interface_name,
                                        const std::string& reorder_name,
                                        cudaStream_t stream) {
  DAQIRI_LOG_ERROR(
      "set_reorder_cuda_stream not implemented for this engine type "
      "(interface='{}', reorder='{}')",
      interface_name,
      reorder_name);
  return Status::NOT_SUPPORTED;
}

Status Engine::get_reorder_burst_info(BurstParams* burst, ReorderBurstInfo* info) {
  (void)burst;
  (void)info;
  DAQIRI_LOG_ERROR("get_reorder_burst_info not implemented for this engine type");
  return Status::NOT_SUPPORTED;
}

Status Engine::rdma_set_header(BurstParams* burst, RDMAOpCode op_code, uintptr_t conn_id,
                                bool is_server, int num_pkts, uint64_t wr_id,
                                const std::string& local_mr_name) {
  DAQIRI_LOG_CRITICAL("RDMA set header not implemented");
  return Status::NOT_SUPPORTED;
}

RDMAOpCode Engine::rdma_get_opcode(BurstParams* burst) {
  DAQIRI_LOG_CRITICAL("RDMA get opcode not implemented");
  return RDMAOpCode::INVALID;
}

};  // namespace daqiri
