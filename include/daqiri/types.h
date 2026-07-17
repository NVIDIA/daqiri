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

#pragma once
#include <array>
#include <vector>
#include <string>
#include <memory>
#include <optional>
#include <stdint.h>
#include <stdexcept>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <linux/if_ether.h>
#include <netinet/ip.h>
#include <linux/udp.h>
#include <netinet/in.h>
#include <cuda_runtime.h>

namespace daqiri {

/**
 * @brief Reserved header bytes for burst structure
 *
 */
static inline constexpr uint32_t ADV_NETWORK_HEADER_SIZE_BYTES = 256;
static inline constexpr uint32_t MAX_NUM_RX_QUEUES = 32;
static inline constexpr uint32_t MAX_NUM_TX_QUEUES = 32;
static inline constexpr uint32_t MAX_INTERFACES = 4;
static inline constexpr int MAX_NUM_SEGS = 4;
static inline constexpr uint32_t DAQIRI_BURST_FLAG_REORDERED = (1U << 28);
static inline constexpr uint32_t DAQIRI_BURST_FLAG_REORDER_TIMEOUT = (1U << 29);
static inline constexpr uint32_t DEFAULT_DYNAMIC_FLOW_CAPACITY = 0;

using FlowId = uint32_t;
using FlowOpId = uint64_t;

struct ReorderBurstInfo {
  uint64_t batch_id;
  uint32_t source_packet_count;
  uint32_t packets_per_batch;
  uint32_t payload_len;
  uint32_t aggregate_len;
  uint32_t burst_flags;
};

/**
 * @brief Return status codes from communication with the NIC
 *
 */
enum class Status {
  SUCCESS,
  NULL_PTR,
  NO_FREE_BURST_BUFFERS,
  NO_FREE_PACKET_BUFFERS,
  NOT_READY,
  INVALID_PARAMETER,
  NO_SPACE_AVAILABLE,
  NOT_SUPPORTED,
  GENERIC_FAILURE,
  CONNECT_FAILURE,
  INTERNAL_ERROR,
};

enum class RDMAOpCode {
  CONNECT,
  SEND,
  RECEIVE,
  RDMA_WRITE,
  RDMA_WRITE_IMM,
  RDMA_READ,
  RDMA_READ_IMM,
  INVALID
};

enum class RDMACompletionType { RX, TX, INVALID };

struct BurstTransportHeader {
  uint8_t version;
  RDMAOpCode opcode;
  Status status;
  uint16_t port_id;
  uint16_t q_id;
  bool server;
  bool tx;
  size_t num_pkts;
  int num_segs;
  uint64_t wr_id;
  uintptr_t conn_id;
  char local_mr_name[32];
  char remote_mr_name[32];
  void* raddr;
  uint64_t dst_key;
  uint32_t imm;
  uint32_t server_addr;
  uint16_t server_port;
};

/**
 * @brief Header of BurstParams
 *
 */
struct BurstHeaderParams {
  size_t num_pkts;
  uint16_t port_id;
  uint16_t q_id;
  int num_segs;
  uint64_t nbytes;
  uintptr_t first_pkt_addr;
  uint32_t max_pkt;
  uint32_t max_pkt_size;
  uint32_t gpu_pkt0_idx;
  uintptr_t gpu_pkt0_addr;
  uint32_t burst_flags;
};

struct BurstHeader {
  BurstHeaderParams hdr;

  // Pad without union to make bindings readable
  void* extra_burst_data;
  uint8_t
      custom_burst_data[ADV_NETWORK_HEADER_SIZE_BYTES - sizeof(void*) - sizeof(BurstHeaderParams)];
};

/**
 * @brief Structure for passing packets
 *
 * The BurstParams structure describes metadata about a packet batch and its packet pointers.
 *
 */
struct BurstParams {
  union {
    BurstHeader hdr;
    BurstTransportHeader transport_hdr;
  };

  std::array<void**, MAX_NUM_SEGS> pkts;
  std::array<uint32_t*, MAX_NUM_SEGS> pkt_lens;
  void** pkt_extra_info;
  std::shared_ptr<void> custom_pkt_data;
  cudaEvent_t event;
};

// Example IPV4 UDP packet using Linux headers
struct UDPIPV4Pkt {
  struct ethhdr eth;
  struct iphdr ip;
  struct udphdr udp;
} __attribute__((packed));

enum class MemoryKind { HOST, HOST_PINNED, HUGE, DEVICE, INVALID };

enum MemoryAccess {
  MEM_ACCESS_LOCAL = 1U,
  MEM_ACCESS_RDMA_WRITE = 1U << 1,
  MEM_ACCESS_RDMA_READ = 1U << 2
};

enum class StreamType {
  RAW,
  SOCKET,
  INVALID,
};

static constexpr const char* DAQIRI_STREAM_TYPE_STR__RAW = "raw";
static constexpr const char* DAQIRI_STREAM_TYPE_STR__SOCKET = "socket";

inline StreamType stream_type_from_string(const std::string& str) {
  if (str == DAQIRI_STREAM_TYPE_STR__RAW) { return StreamType::RAW; }
  if (str == DAQIRI_STREAM_TYPE_STR__SOCKET) { return StreamType::SOCKET; }
  return StreamType::INVALID;
}

inline std::string stream_type_to_string(StreamType type) {
  switch (type) {
    case StreamType::RAW:
      return DAQIRI_STREAM_TYPE_STR__RAW;
    case StreamType::SOCKET:
      return DAQIRI_STREAM_TYPE_STR__SOCKET;
    default:
      return "invalid";
  }
}

enum class SocketProtocol {
  TCP,
  UDP,
  ROCE,
  INVALID,
};

static constexpr const char* DAQIRI_SOCKET_PROTOCOL_STR__TCP = "tcp";
static constexpr const char* DAQIRI_SOCKET_PROTOCOL_STR__UDP = "udp";
static constexpr const char* DAQIRI_SOCKET_PROTOCOL_STR__ROCE = "roce";

inline SocketProtocol socket_protocol_from_string(const std::string& str) {
  if (str == DAQIRI_SOCKET_PROTOCOL_STR__TCP) { return SocketProtocol::TCP; }
  if (str == DAQIRI_SOCKET_PROTOCOL_STR__UDP) { return SocketProtocol::UDP; }
  if (str == DAQIRI_SOCKET_PROTOCOL_STR__ROCE) { return SocketProtocol::ROCE; }
  return SocketProtocol::INVALID;
}

inline std::string socket_protocol_to_string(SocketProtocol proto) {
  switch (proto) {
    case SocketProtocol::TCP:
      return DAQIRI_SOCKET_PROTOCOL_STR__TCP;
    case SocketProtocol::UDP:
      return DAQIRI_SOCKET_PROTOCOL_STR__UDP;
    case SocketProtocol::ROCE:
      return DAQIRI_SOCKET_PROTOCOL_STR__ROCE;
    default:
      return "invalid";
  }
}

inline MemoryKind GetMemoryKindFromString(const std::string& mode_str) {
  if (mode_str == "host") {
    return MemoryKind::HOST;
  } else if (mode_str == "host_pinned") {
    return MemoryKind::HOST_PINNED;
  } else if (mode_str == "huge") {
    return MemoryKind::HUGE;
  } else if (mode_str == "device") {
    return MemoryKind::DEVICE;
  }

  return MemoryKind::INVALID;
}

template <typename T>
uint32_t GetMemoryAccessPropertiesFromList(const T& list) {
  uint32_t access = 0;
  for (const auto& it : list) {
    const auto str = it.template as<std::string>();
    if (str == "local") {
      access |= MEM_ACCESS_LOCAL;
    } else if (str == "rdma_write") {
      access |= MEM_ACCESS_RDMA_WRITE;
    } else if (str == "rdma_read") {
      access |= MEM_ACCESS_RDMA_READ;
    } else {
      return 0;
    }
  }

  return access;
}

/**
 * @brief Location of packet buffers
 *
 */
enum class BufferLocation : uint8_t {
  CPU = 0,
  GPU = 1,
  CPU_GPU_SPLIT = 2,
};

/**
 * @brief Direction of operator
 *
 */
enum class Direction : uint8_t {
  RX = 0,
  TX = 1,
  TX_RX = 2,
};

/**
 * @brief Loopback type
 */
enum class LoopbackType : uint8_t {
  DISABLED = 0,
  LOOPBACK_TYPE_SW = 1,
};

/**
 * @brief Engine Type
 *
 */
enum class EngineType {
  UNKNOWN = -1,
  DEFAULT,
  DPDK,
  SOCKET,
  RDMA,
  IBVERBS,  // pure-DevX MPRQ raw-Ethernet engine (stream_type: raw, engine: ibverbs)
};

static constexpr const char* DAQIRI_ENGINE_STR__DPDK = "dpdk";
static constexpr const char* DAQIRI_ENGINE_STR__SOCKET = "socket";
static constexpr const char* DAQIRI_ENGINE_STR__RDMA = "rdma";
static constexpr const char* DAQIRI_ENGINE_STR__IBVERBS = "ibverbs";
static constexpr const char* DAQIRI_ENGINE_STR__DEFAULT = "default";
/**
 * @brief Convert string to engine type
 *
 * @param str
 * @return EngineType
 */
inline EngineType engine_type_from_string(const std::string& str) {
  if (str == DAQIRI_ENGINE_STR__DEFAULT) return EngineType::DEFAULT;

  std::string available_engines;
  bool is_known_but_unavailable = false;

#if DAQIRI_ENGINE_DPDK
  if (str == DAQIRI_ENGINE_STR__DPDK) return EngineType::DPDK;
  available_engines += std::string(DAQIRI_ENGINE_STR__DPDK) + " ";
#else
  if (str == DAQIRI_ENGINE_STR__DPDK) is_known_but_unavailable = true;
#endif

#if DAQIRI_ENGINE_SOCKET
  if (str == DAQIRI_ENGINE_STR__SOCKET) return EngineType::SOCKET;
  available_engines += std::string(DAQIRI_ENGINE_STR__SOCKET) + " ";
#else
  if (str == DAQIRI_ENGINE_STR__SOCKET) is_known_but_unavailable = true;
#endif


#if DAQIRI_ENGINE_RDMA
  // Accept both the user-facing "ibverbs" and the internal "rdma" name.
  if (str == DAQIRI_ENGINE_STR__IBVERBS || str == DAQIRI_ENGINE_STR__RDMA) {
    return EngineType::RDMA;
  }
  available_engines += std::string(DAQIRI_ENGINE_STR__IBVERBS) + " ";
#else
  if (str == DAQIRI_ENGINE_STR__IBVERBS || str == DAQIRI_ENGINE_STR__RDMA) {
    is_known_but_unavailable = true;
  }
#endif

  if (!available_engines.empty()) {
    available_engines.pop_back();  // Remove trailing space
  }

  if (is_known_but_unavailable) {
    throw std::invalid_argument(
        "Engine type '" + str + "' is not available in this build. "
        "Available engines: " + available_engines + ". "
        "To enable '" + str + "', rebuild with CMake option: "
        "-DDAQIRI_ENGINE=\"" + available_engines + " " + str + "\"");
  }

  throw std::invalid_argument(
      "Unknown engine type '" + str + "'. Valid options: " +
      available_engines + " " + DAQIRI_ENGINE_STR__DEFAULT);
}

enum class RDMAMode {
  CLIENT,
  SERVER,
  INVALID
};

inline RDMAMode GetRDMAModeFromString(const std::string& mode_str) {
  if (mode_str == "client") {
    return RDMAMode::CLIENT;
  } else if (mode_str == "server") {
    return RDMAMode::SERVER;
  }

  return RDMAMode::INVALID;
}

enum class RDMATransportMode {
  RC,
  UC,
  UD,
  INVALID
};

inline RDMATransportMode GetRDMATransportModeFromString(const std::string& mode_str) {
  if (mode_str == "RC") {
    return RDMATransportMode::RC;
  } else if (mode_str == "UC") {
    return RDMATransportMode::UC;
  } else if (mode_str == "UD") {
    return RDMATransportMode::UD;
  }

  return RDMATransportMode::INVALID;
}

struct RDMAConfig {
  RDMAMode mode_ = RDMAMode::INVALID;
  RDMATransportMode xmode_ = RDMATransportMode::INVALID;
  uint16_t port_ = 0;
};

/**
 * @brief Convert engine type to string
 *
 * @param type
 * @return std::string
 */
inline std::string engine_type_to_string(EngineType type) {
  switch (type) {
    case EngineType::DPDK:
      return DAQIRI_ENGINE_STR__DPDK;
    case EngineType::SOCKET:
      return DAQIRI_ENGINE_STR__SOCKET;
    case EngineType::RDMA:
      return DAQIRI_ENGINE_STR__RDMA;
    case EngineType::IBVERBS:
      return DAQIRI_ENGINE_STR__IBVERBS;
    case EngineType::DEFAULT:
      return DAQIRI_ENGINE_STR__DEFAULT;
  }
  return "unknown";
}

// Parse the optional `engine:` config override. Accepts the user-facing
// engine names; "rdma" is rejected in favor of "ibverbs". Sockets (UDP/TCP)
// are always built in, so the socket engine is always available.
inline EngineType config_engine_from_string(const std::string& str) {
  if (str == DAQIRI_ENGINE_STR__DEFAULT) return EngineType::DEFAULT;

  bool is_known_but_unavailable = false;

#if DAQIRI_ENGINE_DPDK
  if (str == DAQIRI_ENGINE_STR__DPDK) return EngineType::DPDK;
#else
  if (str == DAQIRI_ENGINE_STR__DPDK) is_known_but_unavailable = true;
#endif

#if DAQIRI_ENGINE_SOCKET
  if (str == DAQIRI_ENGINE_STR__SOCKET) return EngineType::SOCKET;
#endif

#if DAQIRI_ENGINE_RDMA
  if (str == DAQIRI_ENGINE_STR__IBVERBS) return EngineType::RDMA;
#else
  if (str == DAQIRI_ENGINE_STR__IBVERBS) is_known_but_unavailable = true;
#endif

  if (str == DAQIRI_ENGINE_STR__RDMA) {
    throw std::invalid_argument(
        "Engine 'rdma' is not valid in config. Use 'ibverbs' for RoCE.");
  }

  if (is_known_but_unavailable) {
    throw std::invalid_argument(
        "Engine '" + str + "' is not available in this build. Rebuild with "
        "-DDAQIRI_ENGINE including '" + str + "' (valid values: dpdk, ibverbs).");
  }

  throw std::invalid_argument(
      "Unknown engine '" + str + "'. Valid options: dpdk, socket, ibverbs, " +
      DAQIRI_ENGINE_STR__DEFAULT);
}

// Stream-aware engine resolution. The user-facing name "ibverbs" maps to two
// different engines depending on the stream type: with `raw` it selects the
// pure-DevX MPRQ raw-Ethernet engine (EngineType::IBVERBS); with `socket` (a
// roce:// endpoint) it selects the RoCE/RDMA engine. Every other case defers to
// the string-only resolver above.
inline EngineType config_engine_from_string(const std::string& str, StreamType stream_type) {
#if DAQIRI_ENGINE_IBVERBS
  if (stream_type == StreamType::RAW && str == DAQIRI_ENGINE_STR__IBVERBS) {
    return EngineType::IBVERBS;
  }
#endif
  return config_engine_from_string(str);
}

inline std::string config_engine_to_string(EngineType type) {
  if (type == EngineType::RDMA) { return DAQIRI_ENGINE_STR__IBVERBS; }
  return engine_type_to_string(type);
}

inline EngineType engine_type_from_stream_type(StreamType stream_type,
                                               SocketProtocol protocol = SocketProtocol::INVALID) {
  switch (stream_type) {
    case StreamType::RAW:
      return EngineType::DPDK;
    case StreamType::SOCKET:
      if (protocol == SocketProtocol::ROCE) { return EngineType::RDMA; }
      return EngineType::SOCKET;
    default:
      return EngineType::UNKNOWN;
  }
}

inline bool is_explicit_engine_type(EngineType type) {
  return type != EngineType::UNKNOWN && type != EngineType::DEFAULT;
}

inline bool engine_type_supports_stream_type(EngineType type, StreamType stream_type) {
  switch (stream_type) {
    case StreamType::RAW:
      return type == EngineType::DPDK || type == EngineType::IBVERBS;
    case StreamType::SOCKET:
      return type == EngineType::SOCKET || type == EngineType::RDMA;
    default:
      return false;
  }
}

inline bool engine_type_supports_socket_protocol(EngineType type, SocketProtocol protocol) {
  if (protocol == SocketProtocol::ROCE) {
    return type == EngineType::RDMA || type == EngineType::SOCKET;
  }
  if (protocol == SocketProtocol::TCP || protocol == SocketProtocol::UDP) {
    return type == EngineType::SOCKET;
  }
  return false;
}
class LogLevel {
 public:
  enum Level {
    TRACE,
    DEBUG,
    INFO,
    WARN,
    ERROR,
    CRITICAL,
    OFF,
  };

  static std::string to_string(Level level) {
    auto it = level_to_string_map.find(level);
    if (it != level_to_string_map.end()) { return it->second; }
    return "warn";
  }

  static Level from_string(const std::string& str) {
    std::string lower_str = str;
    std::transform(lower_str.begin(), lower_str.end(), lower_str.begin(), [](unsigned char c) {
      return std::tolower(c);
    });

    auto it = string_to_level_map.find(lower_str);
    if (it != string_to_level_map.end()) { return it->second; }
    throw std::logic_error(
        "Unrecognized log level, available options trace/debug/info/warn/error/critical/off");
  }

 private:
  static const std::unordered_map<Level, std::string> level_to_string_map;
  static const std::unordered_map<std::string, Level> string_to_level_map;
};

/**
 * @class EngineLogLevelCommandBuilder
 * @brief Abstract base class for building engine log level commands.
 *
 * This class defines an interface for building commands that manage log levels.
 * Derived classes must implement the `get_cmd_flags_strings` method to provide
 * the specific command flag strings.
 */
class EngineLogLevelCommandBuilder {
 public:
  /**
   * @brief Virtual destructor for the EngineLogLevelCommandBuilder class.
   */
  virtual ~EngineLogLevelCommandBuilder() = default;

  /**
   * @brief Pure virtual function to get the command flag strings.
   *
   * This function must be implemented by derived classes to return
   * the specific command flag strings for managing log levels.
   *
   * @return A vector of command flag strings.
   */
  virtual std::vector<std::string> get_cmd_flags_strings() const = 0;
};

/**
 * @brief Base class for additional queue configuration.
 *
 * This class serves as a base class for any additional queue configuration
 * that might be needed by different engine types. This class should be
 * inherited by the derived class that will hold the additional configuration
 * for a specific engine type.
 */
class EngineExtraQueueConfig {
 public:
  /**
   * @brief Virtual destructor for proper cleanup of derived class objects.
   */
  virtual ~EngineExtraQueueConfig() = default;
};

struct CommonQueueConfig {
  std::string name_;
  int id_;
  int batch_size_;
  int split_boundary_;
  std::string cpu_core_;
  std::vector<std::string> mrs_;
  std::vector<std::string> offloads_;
  EngineExtraQueueConfig* extra_queue_config_;
};

struct MemoryRegionConfig {
  std::string name_;
  MemoryKind kind_;
  uint16_t affinity_;
  uint32_t access_;
  size_t buf_size_;
  size_t adj_size_ = 0;  // Populated by driver
  size_t ttl_size_ = 0;  // Populated by driver
  size_t num_bufs_;
  bool owned_;
};

struct RxQueueConfig {
  CommonQueueConfig common_;
  uint64_t timeout_us_;
};

struct TxQueueConfig {
  CommonQueueConfig common_;
  // Packet pacing: average TX rate cap in megabits/sec (L2 frame bytes). 0
  // disables pacing (line-rate). Honored only by engines/devices with accurate
  // send scheduling (wait-on-time + real-time clock).
  uint64_t pacing_mbps_ = 0;
};

// struct FlowConfig {
//   FlowConfig() = default;
//   std::string name_;
//   std::string pattern_;
// };

enum class FlowType {
  QUEUE,
  VLAN_PUSH,
  VLAN_POP,
  TUNNEL_ENCAP,
  TUNNEL_DECAP,
};

inline FlowType flow_type_from_string(const std::string& str) {
  if (str == "queue") { return FlowType::QUEUE; }
  if (str == "vlan_push") { return FlowType::VLAN_PUSH; }
  if (str == "vlan_pop") { return FlowType::VLAN_POP; }
  if (str == "tunnel_encap") { return FlowType::TUNNEL_ENCAP; }
  if (str == "tunnel_decap") { return FlowType::TUNNEL_DECAP; }
  throw std::invalid_argument("Unknown flow action type '" + str + "'");
}

inline std::string flow_type_to_string(FlowType type) {
  switch (type) {
    case FlowType::QUEUE:
      return "queue";
    case FlowType::VLAN_PUSH:
      return "vlan_push";
    case FlowType::VLAN_POP:
      return "vlan_pop";
    case FlowType::TUNNEL_ENCAP:
      return "tunnel_encap";
    case FlowType::TUNNEL_DECAP:
      return "tunnel_decap";
  }
  return "unknown";
}

enum class TunnelType {
  NONE,
  VXLAN,
  GRE,
  NVGRE,
};

inline TunnelType tunnel_type_from_string(const std::string& str) {
  if (str == "vxlan") { return TunnelType::VXLAN; }
  if (str == "gre") { return TunnelType::GRE; }
  if (str == "nvgre") { return TunnelType::NVGRE; }
  throw std::invalid_argument("Unknown tunnel type '" + str + "'");
}

inline std::string tunnel_type_to_string(TunnelType type) {
  switch (type) {
    case TunnelType::VXLAN:
      return "vxlan";
    case TunnelType::GRE:
      return "gre";
    case TunnelType::NVGRE:
      return "nvgre";
    case TunnelType::NONE:
      return "none";
  }
  return "unknown";
}

struct VlanActionConfig {
  uint16_t vlan_id_ = 0;
  uint8_t pcp_ = 0;
  uint8_t dei_ = 0;
  uint16_t ethertype_ = 0x8100;
};

struct TunnelConfig {
  TunnelType type_ = TunnelType::NONE;
  std::string outer_eth_src_;
  std::string outer_eth_dst_;
  std::string outer_ipv4_src_;
  std::string outer_ipv4_dst_;
  uint8_t outer_ipv4_ttl_ = 64;
  uint8_t outer_ipv4_tos_ = 0;
  uint16_t outer_udp_src_ = 0;
  uint16_t outer_udp_dst_ = 4789;
  uint32_t vni_ = 0;
  uint16_t gre_protocol_ = 0x0800;
  uint32_t tni_ = 0;
  uint8_t flow_id_ = 0;
};

struct FlowAction {
  FlowType type_ = FlowType::QUEUE;
  uint16_t id_ = 0;
  VlanActionConfig vlan_;
  TunnelConfig tunnel_;
  // Kept after the legacy fields so positional scalar aggregate initializers
  // remain source-compatible. A non-empty list replaces id_ as the queue
  // destination; two or more entries request flow-affine RSS.
  std::vector<uint16_t> ids_;
};

struct FlexItemMatch {
  uint16_t flex_item_id_ = 0;
  uint32_t val_ = 0;
  uint32_t mask_ = 0;
};

// eCPRI-over-Ethernet (EtherType 0xAEFE) RX flow match. The eCPRI EtherType is
// matched implicitly; the common-header message type and the message-body
// identifier (pc_id for message types 0/1, rtc_id for type 2) are matched only
// when their match_*_ flag is set. Matching the identifier requires a message
// type (the hardware needs a known message type to locate the body field), so
// match_id_ implies match_msg_type_.
struct EcpriMatch {
  bool match_msg_type_ = false;
  uint8_t msg_type_ = 0;  // eCPRI common-header message type (RTE_ECPRI_MSG_TYPE_*)
  bool match_id_ = false;
  uint16_t id_ = 0;  // pc_id (msg type 0/1) or rtc_id (msg type 2), at eCPRI offset 4
};

enum class FlowMatchType {
  IPV4_UDP,
  FLEX_ITEM,
  ECPRI,
};

struct FlowMatch {
  FlowMatchType type_ = FlowMatchType::IPV4_UDP;
  uint16_t udp_src_ = 0;
  uint16_t udp_dst_ = 0;
  uint16_t ipv4_len_ = 0;
  in_addr_t ipv4_src_ = INADDR_ANY;
  in_addr_t ipv4_dst_ = INADDR_ANY;
  FlexItemMatch flex_item_match_;
  EcpriMatch ecpri_match_;
};
struct FlowConfig {
  std::string name_;
  FlowId id_ = 0;
  FlowAction action_;
  std::vector<FlowAction> actions_;
  FlowMatch match_;
  void* backend_config_ = nullptr;  // Filled in by operator
};

struct FlowRuleConfig {
  std::string name_;
  FlowAction action_;
  std::vector<FlowAction> actions_;
  FlowMatch match_;
  void* backend_config_ = nullptr;  // Filled in by operator
};

enum class FlowOpType {
  ADD_RX,
  ADD_RX_BATCH,
  DELETE,
};

struct FlowOpResult {
  FlowOpId op_id_ = 0;
  FlowOpType type_ = FlowOpType::ADD_RX;
  Status status_ = Status::NOT_READY;
  FlowId flow_id_ = 0;
  std::vector<FlowId> flow_ids_;
};

inline bool flow_action_is_transform(const FlowAction& action) {
  return action.type_ == FlowType::VLAN_PUSH || action.type_ == FlowType::VLAN_POP ||
         action.type_ == FlowType::TUNNEL_ENCAP || action.type_ == FlowType::TUNNEL_DECAP;
}

inline std::vector<FlowAction> flow_rule_actions(const FlowRuleConfig& flow) {
  if (!flow.actions_.empty()) { return flow.actions_; }
  return {flow.action_};
}

inline std::vector<FlowAction> flow_config_actions(const FlowConfig& flow) {
  if (!flow.actions_.empty()) {
    return flow.actions_;
  }
  return {flow.action_};
}

inline FlowAction flow_queue_action(const std::vector<FlowAction>& actions) {
  auto it = std::find_if(actions.begin(), actions.end(), [](const FlowAction& action) {
    return action.type_ == FlowType::QUEUE;
  });
  return it == actions.end() ? FlowAction{} : *it;
}

inline std::vector<uint16_t> flow_queue_ids(const FlowAction& action) {
  if (!action.ids_.empty()) {
    return action.ids_;
  }
  return {action.id_};
}

inline bool flow_queue_action_uses_rss(const FlowAction& action) {
  return action.type_ == FlowType::QUEUE && action.ids_.size() > 1;
}

inline bool flow_actions_have_transform(const std::vector<FlowAction>& actions) {
  return std::any_of(actions.begin(), actions.end(), flow_action_is_transform);
}

inline bool flow_has_transform_actions(const FlowConfig& flow) {
  return flow_actions_have_transform(flow_config_actions(flow));
}

inline bool flow_rule_has_transform_actions(const FlowRuleConfig& flow) {
  return flow_actions_have_transform(flow_rule_actions(flow));
}

inline size_t flow_action_wire_overhead(const FlowAction& action) {
  switch (action.type_) {
    case FlowType::VLAN_PUSH:
      return 4;
    case FlowType::VLAN_POP:
      return 0;
    case FlowType::TUNNEL_ENCAP:
      switch (action.tunnel_.type_) {
        case TunnelType::VXLAN:
          return sizeof(ethhdr) + sizeof(iphdr) + sizeof(udphdr) + 8;
        case TunnelType::GRE:
          return sizeof(ethhdr) + sizeof(iphdr) + 4;
        case TunnelType::NVGRE:
          return sizeof(ethhdr) + sizeof(iphdr) + 8;
        case TunnelType::NONE:
          return 0;
      }
      break;
    case FlowType::TUNNEL_DECAP:
    case FlowType::QUEUE:
      return 0;
  }
  return 0;
}

inline size_t flow_decap_wire_overhead(const FlowAction& action) {
  if (action.type_ == FlowType::VLAN_POP) { return 4; }
  if (action.type_ != FlowType::TUNNEL_DECAP) { return 0; }
  switch (action.tunnel_.type_) {
    case TunnelType::VXLAN:
      return sizeof(ethhdr) + sizeof(iphdr) + sizeof(udphdr) + 8;
    case TunnelType::GRE:
      return sizeof(ethhdr) + sizeof(iphdr) + 4;
    case TunnelType::NVGRE:
      return sizeof(ethhdr) + sizeof(iphdr) + 8;
    case TunnelType::NONE:
      return 0;
  }
  return 0;
}

inline size_t flow_max_decap_wire_overhead(const std::vector<FlowConfig>& flows) {
  size_t overhead = 0;
  for (const auto& flow : flows) {
    size_t per_flow = 0;
    for (const auto& action : flow_config_actions(flow)) {
      per_flow += flow_decap_wire_overhead(action);
    }
    overhead = std::max(overhead, per_flow);
  }
  return overhead;
}

inline size_t flow_max_encap_wire_overhead(const std::vector<FlowConfig>& flows) {
  size_t overhead = 0;
  for (const auto& flow : flows) {
    size_t per_flow = 0;
    for (const auto& action : flow_config_actions(flow)) {
      per_flow += flow_action_wire_overhead(action);
    }
    overhead = std::max(overhead, per_flow);
  }
  return overhead;
}

struct CommonConfig {
  int version;
  int master_core_;
  Direction dir;
  StreamType stream_type = StreamType::INVALID;
  SocketProtocol protocol = SocketProtocol::INVALID;
  EngineType engine = EngineType::DEFAULT;        // optional `engine:` override
  EngineType engine_type = EngineType::UNKNOWN;   // resolved active engine
  LoopbackType loopback_;
};

enum class SocketMode {
  CLIENT,
  SERVER,
  INVALID,
};

inline SocketMode GetSocketModeFromString(const std::string& mode_str) {
  if (mode_str == "client") { return SocketMode::CLIENT; }
  if (mode_str == "server") { return SocketMode::SERVER; }
  return SocketMode::INVALID;
}

struct SocketConfig {
  SocketMode mode_ = SocketMode::INVALID;
  std::string local_addr_;
  std::string remote_addr_;
  std::string local_ip_;
  std::string remote_ip_;
  uint16_t local_port_ = 0;
  uint16_t remote_port_ = 0;
  uint16_t max_payload_size_ = 0;
  uint64_t max_burst_interval_ms_ = 0;
  uint32_t min_ipg_ns_ = 0;
  int32_t retry_connect_s_ = 1;
};

struct RoCEConfig {
  RDMATransportMode transport_mode_ = RDMATransportMode::INVALID;
};

struct FlexItemConfig {
  std::string name_;
  uint16_t id_;
  uint16_t udp_dst_port_;
  uint16_t offset_;
};

enum class ReorderMethod {
  INVALID = 0,
  SEQ_BATCH_NUMBER,
  SEQ_PACKETS_PER_BATCH,
};

enum class ReorderDataType : uint8_t {
  SAME = 0,
  INT4,
  INT8,
  INT16,
  INT32,
  FP16,
  BF16,
  FP32,
  FP64,
  INVALID,
};

enum class ReorderEndianness : uint8_t {
  HOST = 0,
  NETWORK,
  INVALID,
};

inline ReorderDataType reorder_data_type_from_string(const std::string& str) {
  if (str == "int4") { return ReorderDataType::INT4; }
  if (str == "int8") { return ReorderDataType::INT8; }
  if (str == "int16") { return ReorderDataType::INT16; }
  if (str == "int32") { return ReorderDataType::INT32; }
  if (str == "fp16") { return ReorderDataType::FP16; }
  if (str == "bf16") { return ReorderDataType::BF16; }
  if (str == "fp32") { return ReorderDataType::FP32; }
  if (str == "fp64") { return ReorderDataType::FP64; }
  return ReorderDataType::INVALID;
}

inline std::string reorder_data_type_to_string(ReorderDataType type) {
  switch (type) {
    case ReorderDataType::SAME:
      return "same";
    case ReorderDataType::INT4:
      return "int4";
    case ReorderDataType::INT8:
      return "int8";
    case ReorderDataType::INT16:
      return "int16";
    case ReorderDataType::INT32:
      return "int32";
    case ReorderDataType::FP16:
      return "fp16";
    case ReorderDataType::BF16:
      return "bf16";
    case ReorderDataType::FP32:
      return "fp32";
    case ReorderDataType::FP64:
      return "fp64";
    default:
      return "invalid";
  }
}

inline ReorderEndianness reorder_endianness_from_string(const std::string& str) {
  if (str == "host") { return ReorderEndianness::HOST; }
  if (str == "network") { return ReorderEndianness::NETWORK; }
  return ReorderEndianness::INVALID;
}

inline std::string reorder_endianness_to_string(ReorderEndianness endianness) {
  switch (endianness) {
    case ReorderEndianness::HOST:
      return "host";
    case ReorderEndianness::NETWORK:
      return "network";
    default:
      return "invalid";
  }
}

inline bool is_reorder_input_data_type(ReorderDataType type) {
  return type == ReorderDataType::INT4 || type == ReorderDataType::INT8
         || type == ReorderDataType::INT16 || type == ReorderDataType::INT32;
}

inline bool is_reorder_output_data_type(ReorderDataType type) {
  return type == ReorderDataType::FP16 || type == ReorderDataType::BF16
         || type == ReorderDataType::FP32 || type == ReorderDataType::FP64
         || type == ReorderDataType::INT32;
}

inline uint32_t reorder_data_type_bit_width(ReorderDataType type) {
  switch (type) {
    case ReorderDataType::INT4:
      return 4;
    case ReorderDataType::INT8:
      return 8;
    case ReorderDataType::INT16:
    case ReorderDataType::FP16:
    case ReorderDataType::BF16:
      return 16;
    case ReorderDataType::INT32:
    case ReorderDataType::FP32:
      return 32;
    case ReorderDataType::FP64:
      return 64;
    default:
      return 0;
  }
}

struct ReorderBitFieldConfig {
  uint16_t bit_offset_ = 0;
  uint8_t bit_width_ = 0;
};

struct ReorderSeqBatchNumberConfig {
  ReorderBitFieldConfig sequence_number_;
  ReorderBitFieldConfig batch_number_;
  uint32_t packets_per_batch_ = 0;  // Derived from bit widths
};

struct ReorderSeqPacketsPerBatchConfig {
  ReorderBitFieldConfig sequence_number_;
  uint32_t packets_per_batch_ = 0;
};

struct ReorderDataTypesConfig {
  bool enabled_ = false;
  ReorderDataType input_type_ = ReorderDataType::SAME;
  ReorderDataType output_type_ = ReorderDataType::SAME;
  ReorderEndianness input_endianness_ = ReorderEndianness::HOST;
};

struct ReorderConfig {
  std::string name_;
  std::string reorder_type_;
  std::string memory_region_;
  uint32_t payload_byte_offset_ = 0;
  std::vector<FlowId> flow_ids_;
  ReorderMethod method_ = ReorderMethod::INVALID;
  ReorderSeqBatchNumberConfig seq_batch_number_;
  ReorderSeqPacketsPerBatchConfig seq_packets_per_batch_;
  ReorderDataTypesConfig data_types_;
};

struct RxConfig {
  bool flow_isolation_ = false;
  bool hardware_timestamps_ = false;
  uint32_t dynamic_flow_capacity_ = DEFAULT_DYNAMIC_FLOW_CAPACITY;
  std::vector<RxQueueConfig> queues_;
  std::vector<FlowConfig> flows_;
  std::vector<FlexItemConfig> flex_items_;
  std::vector<ReorderConfig> reorder_configs_;
};

struct TxConfig {
  bool accurate_send_ = false;
  std::vector<TxQueueConfig> queues_;
  std::vector<FlowConfig> flows_;
};

struct InterfaceConfig {
  std::string name_;
  std::string address_;
  uint16_t port_id_;
  SocketConfig socket_;
  RoCEConfig roce_;
  RDMAConfig rdma_;
  RxConfig rx_;
  TxConfig tx_;
};

struct NetworkConfig {
  CommonConfig common_;
  std::unordered_map<std::string, MemoryRegionConfig> mrs_;
  std::vector<InterfaceConfig> ifs_;
  uint16_t debug_;
  // Number of metadata buffers for TX and RX. Higher numbers can handle more bursts per second
  // at the cost of more memory.
  uint32_t tx_meta_buffers_;
  uint32_t rx_meta_buffers_;
  LogLevel::Level log_level_;
};

template <typename Config>
auto get_rdma_configs_enabled(const Config& config) {
  bool server = false;
  bool client = false;

  auto& yaml_nodes = config.yaml_nodes();
  for (const auto& yaml_node : yaml_nodes) {
    auto cfg_node = yaml_node["daqiri"]["cfg"];
    auto stream_type = cfg_node["stream_type"].template as<std::string>("");
    auto engine = cfg_node["engine"].template as<std::string>("");
    // Protocol is encoded in the endpoint URI scheme (roce://), checked below.
    bool uses_rdma = engine == DAQIRI_ENGINE_STR__IBVERBS;
    if (!uses_rdma) {
      auto interfaces_node = cfg_node["interfaces"];
      for (const auto& intf : interfaces_node) {
        auto socket_config_node = intf["socket_config"];
        if (!socket_config_node.IsDefined()) { continue; }
        auto local_addr = socket_config_node["local_addr"].template as<std::string>("");
        auto remote_addr = socket_config_node["remote_addr"].template as<std::string>("");
        uses_rdma = local_addr.rfind("rdma://", 0) == 0 ||
                    remote_addr.rfind("rdma://", 0) == 0 ||
                    local_addr.rfind("roce://", 0) == 0 ||
                    remote_addr.rfind("roce://", 0) == 0;
        if (uses_rdma) { break; }
      }
    }
    if (stream_type != DAQIRI_STREAM_TYPE_STR__SOCKET || !uses_rdma) { continue; }
    auto interfaces_node = cfg_node["interfaces"];
    for (const auto& intf : interfaces_node) {
      auto socket_config_node = intf["socket_config"];
      if (socket_config_node.IsDefined()) {
        std::string mode = socket_config_node["mode"].template as<std::string>();
        if (mode == "server") {
          server = true;
        } else if (mode == "client") {
          client = true;
        }
      }
    }
  }

  return std::make_tuple(server, client);
}

template <typename Config>
auto get_rx_tx_configs_enabled(const Config& config) {
  bool rx = false;
  bool tx = false;

  auto& yaml_nodes = config.yaml_nodes();
  for (const auto& yaml_node : yaml_nodes) {
    auto node = yaml_node["daqiri"]["cfg"]["interfaces"];
    for (const auto& intf : node) {
      if (intf["rx"]) { rx = true; }
      if (intf["tx"]) { tx = true; }
    }
  }

  return std::make_tuple(rx, tx);
}

};  // namespace daqiri
