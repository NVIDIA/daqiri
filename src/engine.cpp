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
#if DAQIRI_ENGINE_IBVERBS
#include "src/engines/ibverbs/daqiri_ibverbs_engine.h"
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
#include <sys/mman.h>
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
#if DAQIRI_ENGINE_IBVERBS
    case EngineType::IBVERBS:
      _engine = std::make_unique<IbverbsEngine>();
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
        // "ibverbs" + raw selects the pure-DevX MPRQ engine; "ibverbs" + socket
        // resolves to the RoCE/RDMA engine (handled by the stream-aware resolver).
        return config_engine_from_string(engine_str, stream_type);
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

Status Engine::populate_pool(daqiri::Ring* ring, const std::string& mr_name) {
  auto mr = cfg_.mrs_[mr_name];
  auto base = reinterpret_cast<char*>(ar_[mr_name].ptr_);

  for (size_t i = 0; i < mr.num_bufs_; i++) {
    if (!ring->enqueue(base + i * mr.adj_size_)) {
      DAQIRI_LOG_CRITICAL("Failed to enqueue buffer {} to ring", i);
      return Status::NULL_PTR;
    }
  }
  return Status::SUCCESS;
}

// Round `value` up to the next multiple of `align`, which must be a power of two.
// Replaces DPDK's RTE_ALIGN_CEIL so engine.cpp carries no libdpdk dependency.
static inline size_t align_ceil(size_t value, size_t align) {
  return (value + (align - 1)) & ~(align - 1);
}

void* Engine::alloc_huge(size_t bytes, int /*numa*/) {
  // Base (non-DPDK) implementation: try a real hugepage-backed mapping, falling
  // back to ordinary page-aligned host memory if MAP_HUGETLB is unavailable.
  // The DPDK engine overrides this to use rte_malloc_socket (EAL hugepages,
  // IOVA-contiguous for the NIC) -- see DpdkEngine::alloc_huge.
#if defined(MAP_HUGETLB)
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                 -1, 0);
  if (p != MAP_FAILED) {
    return p;
  }
  DAQIRI_LOG_WARN(
      "MAP_HUGETLB allocation of {} bytes failed; falling back to regular pages. "
      "Configure hugepages per docs/tutorials/system_configuration.md for best performance.",
      bytes);
#endif
  void* ptr = nullptr;
  if (posix_memalign(&ptr, GPU_PAGE_SIZE, bytes) != 0) {
    return nullptr;
  }
  return ptr;
}

Status Engine::allocate_memory_regions() {
  DAQIRI_LOG_INFO("Registering memory regions");
  for (auto& mr : cfg_.mrs_) {
    void* ptr = nullptr;
    AllocRegion ar;
    mr.second.ttl_size_ = align_ceil(mr.second.adj_size_ * mr.second.num_bufs_, GPU_PAGE_SIZE);

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
          ptr = alloc_huge(mr.second.ttl_size_, mr.second.affinity_);
          break;
        case MemoryKind::DEVICE: {
          unsigned int flag = 1;
          const auto align = align_ceil(mr.second.ttl_size_, GPU_PAGE_SIZE);
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
  DAQIRI_LOG_INFO("Finished allocating memory regions");
  return Status::SUCCESS;
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
