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

#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <filesystem>
#include <limits>

#include "src/engine.h"
#include "src/metrics.h"
#include <daqiri/daqiri.h>
#include <daqiri/logging.hpp>
#if DAQIRI_ENGINE_DPDK
#include <rte_mbuf.h>
#include <rte_memcpy.h>
#include <rte_ethdev.h>
#endif

#define ASSERT_DAQIRI_ENGINE_INITIALIZED() \
  assert(g_daqiri_engine != nullptr && "DAQIRI Engine is not initialized")
namespace daqiri {

// Declare a static global variable for the engine
static Engine* g_daqiri_engine = nullptr;

namespace {

constexpr size_t kEthAddrLength = 6;
constexpr size_t kEthAddrOctetLength = 2;

int hex_digit_value(char digit) {
  const unsigned char c = static_cast<unsigned char>(digit);
  if (c >= '0' && c <= '9') { return c - '0'; }
  if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
  if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
  return -1;
}

Status parse_eth_addr(std::array<char, kEthAddrLength>* dst,
                      const std::string& addr) {
  if (dst == nullptr) { return Status::NULL_PTR; }

  std::array<char, kEthAddrLength> parsed = {};
  size_t offset = 0;

  for (size_t octet = 0; octet < kEthAddrLength; ++octet) {
    if (offset + kEthAddrOctetLength > addr.size()) {
      return Status::INVALID_PARAMETER;
    }

    const int high = hex_digit_value(addr[offset]);
    const int low = hex_digit_value(addr[offset + 1]);
    if (high < 0 || low < 0) { return Status::INVALID_PARAMETER; }

    parsed[octet] = static_cast<char>((high << 4) | low);
    offset += kEthAddrOctetLength;

    if (octet + 1 == kEthAddrLength) {
      if (offset != addr.size()) { return Status::INVALID_PARAMETER; }
      continue;
    }

    if (offset >= addr.size() || addr[offset] != ':') {
      return Status::INVALID_PARAMETER;
    }
    ++offset;
  }

  *dst = parsed;
  return Status::SUCCESS;
}

}  // namespace

const std::unordered_map<LogLevel::Level, std::string> LogLevel::level_to_string_map = {
    {TRACE, "trace"},
    {DEBUG, "debug"},
    {INFO, "info"},
    {WARN, "warn"},
    {ERROR, "error"},
    {CRITICAL, "critical"},
    {OFF, "off"},
};

const std::unordered_map<std::string, LogLevel::Level> LogLevel::string_to_level_map = {
    {"trace", TRACE},
    {"debug", DEBUG},
    {"info", INFO},
    {"warn", WARN},
    {"error", ERROR},
    {"critical", CRITICAL},
    {"off", OFF},
};

[[deprecated("Use create_tx_burst_params() instead")]] BurstParams* create_burst_params() {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->create_tx_burst_params();
}

BurstParams* create_tx_burst_params() {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->create_tx_burst_params();
}

void initialize_engine(Engine* engine) {
  g_daqiri_engine = engine;
}

Engine* get_active_engine() {
  return g_daqiri_engine;
}

EngineType get_engine_type() {
  return EngineFactory::get_engine_type();
}

template <typename Config>
EngineType get_engine_type(const Config& config) {
  return EngineFactory::get_engine_type(config);
}

void free_packet(BurstParams* burst, int pkt) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  g_daqiri_engine->free_packet(burst, pkt);
}

void free_packet_segment(BurstParams* burst, int seg, int pkt) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  g_daqiri_engine->free_packet_segment(burst, seg, pkt);
}

uint32_t get_packet_length(BurstParams* burst, int idx) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->get_packet_length(burst, idx);
}

uint16_t get_packet_flow_id(BurstParams* burst, int idx) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->get_packet_flow_id(burst, idx);
}

Status get_packet_rx_timestamp(BurstParams* burst, int idx, uint64_t* timestamp_ns) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->get_packet_rx_timestamp(burst, idx, timestamp_ns);
}

uint64_t get_burst_tot_byte(BurstParams* burst) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->get_burst_tot_byte(burst);
}

uint32_t get_segment_packet_length(BurstParams* burst, int seg, int idx) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->get_segment_packet_length(burst, seg, idx);
}

void free_all_segment_packets(BurstParams* burst, int seg) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  g_daqiri_engine->free_all_segment_packets(burst, seg);
}

void free_all_burst_packets(BurstParams* burst) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  g_daqiri_engine->free_all_packets(burst);
}

void free_all_packets_and_burst_rx(BurstParams* burst) {
  free_all_burst_packets(burst);
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  g_daqiri_engine->free_rx_burst(burst);
}

void free_all_packets_and_burst_tx(BurstParams* burst) {
  free_all_burst_packets(burst);
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  g_daqiri_engine->free_tx_burst(burst);
}

void free_segment_packets_and_burst(BurstParams* burst, int seg) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  g_daqiri_engine->free_all_segment_packets(burst, seg);
  g_daqiri_engine->free_rx_burst(burst);
}

void format_eth_addr(char* dst, std::string addr) {
  if (dst == nullptr) {
    DAQIRI_LOG_ERROR("Invalid MAC address destination buffer");
    return;
  }

  std::array<char, kEthAddrLength> parsed = {};
  const Status status = parse_eth_addr(&parsed, addr);
  if (status != Status::SUCCESS) {
    DAQIRI_LOG_ERROR("Invalid MAC address format: {}", addr);
    std::fill_n(dst, kEthAddrLength, 0x00);
    return;
  }

  std::copy(parsed.begin(), parsed.end(), dst);
}

Status get_mac_addr(int port, char* mac) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->get_mac_addr(port, mac);
}

Status drop_all_traffic(int port) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->drop_all_traffic(port);
}

Status allow_all_traffic(int port) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->allow_all_traffic(port);
}

bool is_tx_burst_available(BurstParams* burst) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->is_tx_burst_available(burst);
}

int get_port_id(const std::string& key) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->get_port_id(key);
}

Status get_tx_packet_burst(BurstParams* burst) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  if (!g_daqiri_engine->is_tx_burst_available(burst)) return Status::NO_FREE_BURST_BUFFERS;
  return g_daqiri_engine->get_tx_packet_burst(burst);
}

Status set_eth_header(BurstParams* burst, int idx, char* dst_addr) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->set_eth_header(burst, idx, dst_addr);
}

Status set_ipv4_header(BurstParams* burst, int idx, int ip_len, uint8_t proto,
                       unsigned int src_host, unsigned int dst_host) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->set_ipv4_header(burst, idx, ip_len, proto, src_host, dst_host);
}

Status set_udp_header(BurstParams* burst, int idx, int udp_len, uint16_t src_port,
                      uint16_t dst_port) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->set_udp_header(burst, idx, udp_len, src_port, dst_port);
}

Status set_udp_payload(BurstParams* burst, int idx, void* data, int len) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->set_udp_payload(burst, idx, data, len);
}

Status set_packet_lengths(BurstParams* burst, int idx, const std::initializer_list<int>& lens) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->set_packet_lengths(burst, idx, lens);
}

Status set_all_packet_lengths(BurstParams* burst, const std::initializer_list<int>& lens) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->set_all_packet_lengths(burst, lens);
}

Status set_packet_tx_time(BurstParams* burst, int idx, uint64_t time) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->set_packet_tx_time(burst, idx, time);
}

int64_t get_num_packets(BurstParams* burst) {
  return burst->hdr.hdr.num_pkts;
}

int64_t get_q_id(BurstParams* burst) {
  assert(burst != nullptr && "burst is null");
  return burst->hdr.hdr.q_id;
}

uintptr_t get_connection_id(const BurstParams* burst) {
  assert(burst != nullptr && "burst is null");
  return burst->transport_hdr.conn_id;
}

void set_connection_id(BurstParams* burst, uintptr_t conn_id) {
  assert(burst != nullptr && "burst is null");
  burst->transport_hdr.conn_id = conn_id;
}

void set_num_packets(BurstParams* burst, int64_t num) {
  assert(burst != nullptr && "burst is null");
  burst->hdr.hdr.num_pkts = num;
}

void set_header(BurstParams* burst, uint16_t port, uint16_t q, int64_t num, int segs) {
  assert(burst != nullptr && "burst is null");
  burst->hdr.hdr.num_pkts = num;
  burst->hdr.hdr.port_id = port;
  burst->hdr.hdr.q_id = q;
  burst->hdr.hdr.num_segs = segs;
}

void free_tx_burst(BurstParams* burst) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  g_daqiri_engine->free_tx_burst(burst);
}

void free_tx_metadata(BurstParams* burst) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  g_daqiri_engine->free_tx_metadata(burst);
}

void free_rx_burst(BurstParams* burst) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  g_daqiri_engine->free_rx_burst(burst);
}

void free_rx_metadata(BurstParams* burst) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  g_daqiri_engine->free_rx_metadata(burst);
}

void* get_segment_packet_ptr(BurstParams* burst, int seg, int idx) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->get_segment_packet_ptr(burst, seg, idx);
}

void* get_packet_ptr(BurstParams* burst, int idx) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->get_packet_ptr(burst, idx);
}

void shutdown() {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  g_daqiri_engine->shutdown();
  metrics::shutdown();
}

Status send_tx_burst(BurstParams* burst) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->send_tx_burst(burst);
}

Status get_rx_burst(BurstParams** burst, int port, int q) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->get_rx_burst(burst, port, q);
}

Status get_rx_burst(BurstParams** burst, int port) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->get_rx_burst(burst, port);
}

Status get_rx_burst(BurstParams** burst) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->get_rx_burst(burst);
}

Status get_rx_burst(BurstParams** burst, uintptr_t conn_id, bool server) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->get_rx_burst(burst, conn_id, server);
}

Status set_reorder_cuda_stream(const std::string& interface_name,
                               const std::string& reorder_name,
                               cudaStream_t stream) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->set_reorder_cuda_stream(interface_name, reorder_name, stream);
}

Status get_reorder_burst_info(BurstParams* burst, ReorderBurstInfo* info) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->get_reorder_burst_info(burst, info);
}

uint16_t get_num_rx_queues(int port_id) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->get_num_rx_queues(port_id);
}

void flush_port_queue(int port, int queue) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  g_daqiri_engine->flush_port_queue(port, queue);
}

void print_stats() {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  g_daqiri_engine->print_stats();
}

Status daqiri_init(NetworkConfig& config) {
  if (config.common_.engine_type == EngineType::UNKNOWN) {
    if (is_explicit_engine_type(config.common_.engine)) {
      config.common_.engine_type = config.common_.engine;
    } else if (config.common_.stream_type != StreamType::INVALID) {
      config.common_.engine_type =
          engine_type_from_stream_type(config.common_.stream_type, config.common_.protocol);
      config.common_.engine = config.common_.engine_type;
    }
  }

  if (config.common_.stream_type != StreamType::INVALID &&
      is_explicit_engine_type(config.common_.engine_type) &&
      !engine_type_supports_stream_type(config.common_.engine_type,
                                         config.common_.stream_type)) {
    DAQIRI_LOG_ERROR("engine '{}' is not valid for stream_type '{}'",
                     config_engine_to_string(config.common_.engine_type),
                     stream_type_to_string(config.common_.stream_type));
    return Status::INVALID_PARAMETER;
  }

  if (config.common_.stream_type == StreamType::SOCKET &&
      config.common_.protocol != SocketProtocol::INVALID &&
      is_explicit_engine_type(config.common_.engine_type) &&
      !engine_type_supports_socket_protocol(config.common_.engine_type,
                                             config.common_.protocol)) {
    DAQIRI_LOG_ERROR("engine '{}' is not valid for transport '{}'",
                     config_engine_to_string(config.common_.engine_type),
                     socket_protocol_to_string(config.common_.protocol));
    return Status::INVALID_PARAMETER;
  }

  if (config.common_.stream_type == StreamType::SOCKET &&
      (config.common_.protocol == SocketProtocol::TCP ||
       config.common_.protocol == SocketProtocol::UDP)) {
    std::unordered_set<std::string> gpu_mrs;
    for (const auto& intf : config.ifs_) {
      for (const auto& q : intf.rx_.queues_) {
        for (const auto& mr_name : q.common_.mrs_) {
          const auto it = config.mrs_.find(mr_name);
          if (it != config.mrs_.end() && it->second.kind_ == MemoryKind::DEVICE) {
            gpu_mrs.emplace(mr_name);
          }
        }
      }
      for (const auto& q : intf.tx_.queues_) {
        for (const auto& mr_name : q.common_.mrs_) {
          const auto it = config.mrs_.find(mr_name);
          if (it != config.mrs_.end() && it->second.kind_ == MemoryKind::DEVICE) {
            gpu_mrs.emplace(mr_name);
          }
        }
      }
    }

    if (!gpu_mrs.empty()) {
      std::string joined;
      for (const auto& mr_name : gpu_mrs) {
        if (!joined.empty()) { joined += ", "; }
        joined += mr_name;
      }
      DAQIRI_LOG_ERROR(
          "GPU memory regions are not supported for protocol '{}'. Offending "
          "memory_regions: {}",
          socket_protocol_to_string(config.common_.protocol),
          joined);
      return Status::INVALID_PARAMETER;
    }
  }

  EngineFactory::set_engine_type(config.common_.engine_type);

  auto engine = &(EngineFactory::get_active_engine());

  if (!engine->set_config_and_initialize(config)) { return Status::INTERNAL_ERROR; }

  for (const auto& intf : config.ifs_) {
    const auto& rx = intf.rx_;
    auto port = engine->get_port_id(intf.address_);
    if (port < 0) {
      DAQIRI_LOG_ERROR("Failed to get port from name {}", intf.address_);
      return Status::INVALID_PARAMETER;
    }
  }

  return Status::SUCCESS;
}

namespace {

YAML::Node get_network_node(const YAML::Node& root) {
  if (root["daqiri"] && root["daqiri"]["cfg"]) { return root["daqiri"]["cfg"]; }
  return root;
}

Status parse_network_config_node(const YAML::Node& root, NetworkConfig& config) {
  try {
    const YAML::Node network_node = get_network_node(root);
    if (!network_node || !network_node.IsMap()) {
      DAQIRI_LOG_ERROR("Invalid YAML: expected top-level map for network configuration");
      return Status::INVALID_PARAMETER;
    }
    config = network_node.as<NetworkConfig>();
    return Status::SUCCESS;
  } catch (const YAML::Exception& e) {
    DAQIRI_LOG_ERROR("YAML parsing error: {}", e.what());
    return Status::INVALID_PARAMETER;
  } catch (const std::exception& e) {
    DAQIRI_LOG_ERROR("Failed to parse network configuration: {}", e.what());
    return Status::INTERNAL_ERROR;
  }
}

bool has_yaml_extension(const std::string& path_str) {
  std::filesystem::path path(path_str);
  std::string ext = path.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return ext == ".yaml" || ext == ".yml";
}

}  // namespace

Status parse_network_config_from_yaml_string(const std::string& yaml_string, NetworkConfig& config) {
  try {
    const YAML::Node root = YAML::Load(yaml_string);
    return parse_network_config_node(root, config);
  } catch (const YAML::Exception& e) {
    DAQIRI_LOG_ERROR("Failed to parse YAML string: {}", e.what());
    return Status::INVALID_PARAMETER;
  } catch (const std::exception& e) {
    DAQIRI_LOG_ERROR("Failed to parse YAML string: {}", e.what());
    return Status::INTERNAL_ERROR;
  }
}

Status parse_network_config_from_yaml_file(const std::string& yaml_path, NetworkConfig& config) {
  try {
    const YAML::Node root = YAML::LoadFile(yaml_path);
    return parse_network_config_node(root, config);
  } catch (const YAML::Exception& e) {
    DAQIRI_LOG_ERROR("Failed to parse YAML file '{}': {}", yaml_path, e.what());
    return Status::INVALID_PARAMETER;
  } catch (const std::exception& e) {
    DAQIRI_LOG_ERROR("Failed to parse YAML file '{}': {}", yaml_path, e.what());
    return Status::INTERNAL_ERROR;
  }
}

Status parse_network_config(const std::string& yaml_string_or_path, NetworkConfig& config) {
  std::error_code ec;
  const std::filesystem::path path(yaml_string_or_path);
  if (std::filesystem::exists(path, ec) && std::filesystem::is_regular_file(path, ec)) {
    return parse_network_config_from_yaml_file(yaml_string_or_path, config);
  }

  if (has_yaml_extension(yaml_string_or_path)) {
    DAQIRI_LOG_ERROR("YAML file '{}' does not exist", yaml_string_or_path);
    return Status::INVALID_PARAMETER;
  }

  return parse_network_config_from_yaml_string(yaml_string_or_path, config);
}

Status daqiri_init_from_yaml_string(const std::string& yaml_string) {
  NetworkConfig config;
  const Status parse_status = parse_network_config_from_yaml_string(yaml_string, config);
  if (parse_status != Status::SUCCESS) { return parse_status; }
  return daqiri_init(config);
}

Status daqiri_init_from_yaml_file(const std::string& yaml_path) {
  NetworkConfig config;
  const Status parse_status = parse_network_config_from_yaml_file(yaml_path, config);
  if (parse_status != Status::SUCCESS) { return parse_status; }
  return daqiri_init(config);
}

Status daqiri_init(const std::string& yaml_string_or_path) {
  NetworkConfig config;
  const Status parse_status = parse_network_config(yaml_string_or_path, config);
  if (parse_status != Status::SUCCESS) { return parse_status; }
  return daqiri_init(config);
}

// Generic socket functions
Status socket_connect_to_server(const std::string& server_addr, uint16_t server_port,
                                uintptr_t* conn_id) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->socket_connect_to_server(server_addr, server_port, conn_id);
}

Status socket_connect_to_server(const std::string& server_addr, uint16_t server_port,
                                const std::string& src_addr, uintptr_t* conn_id) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->socket_connect_to_server(server_addr, server_port, src_addr, conn_id);
}

Status socket_get_port_queue(uintptr_t conn_id, uint16_t* port, uint16_t* queue) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->socket_get_port_queue(conn_id, port, queue);
}

Status socket_get_server_conn_id(const std::string& server_addr, uint16_t server_port,
                                 uintptr_t* conn_id) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->socket_get_server_conn_id(server_addr, server_port, conn_id);
}

// RDMA Functions
Status rdma_connect_to_server(const std::string& server_addr, uint16_t server_port,
                              uintptr_t* conn_id) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->rdma_connect_to_server(server_addr, server_port, conn_id);
}

Status rdma_connect_to_server(const std::string& server_addr, uint16_t server_port,
                              const std::string& src_addr, uintptr_t* conn_id) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->rdma_connect_to_server(server_addr, server_port, src_addr, conn_id);
}

Status rdma_get_port_queue(uintptr_t conn_id, uint16_t* port, uint16_t* queue) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->rdma_get_port_queue(conn_id, port, queue);
}

Status rdma_get_server_conn_id(const std::string& server_addr, uint16_t server_port,
                               uintptr_t* conn_id) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->rdma_get_server_conn_id(server_addr, server_port, conn_id);
}

Status rdma_set_header(BurstParams* burst, RDMAOpCode op_code, uintptr_t conn_id, bool is_server,
                       int num_pkts, uint64_t wr_id, const std::string& local_mr_name) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->rdma_set_header(
      burst, op_code, conn_id, is_server, num_pkts, wr_id, local_mr_name);
}

RDMAOpCode rdma_get_opcode(BurstParams* burst) {
  ASSERT_DAQIRI_ENGINE_INITIALIZED();
  return g_daqiri_engine->rdma_get_opcode(burst);
}

};  // namespace daqiri

/**
 * @brief Parse flow configuration from a YAML node.
 *
 * @param flow_item The YAML node containing the flow configuration.
 * @param flow The FlowConfig object to populate.
 * @return true if parsing was successful, false otherwise.
 */
bool YAML::convert<daqiri::NetworkConfig>::parse_flow_config(
    const YAML::Node& flow_item, daqiri::FlowConfig& flow) {
  struct in_addr addr;
  try {
    flow.name_ = flow_item["name"].as<std::string>();
    flow.id_ = flow_item["id"].as<int>();
    flow.action_.type_ = daqiri::FlowType::QUEUE;
    flow.action_.id_ = flow_item["action"]["id"].as<int>();
  } catch (const std::exception& e) {
    DAQIRI_LOG_ERROR("Error parsing FlowConfig: {}", e.what());
    return false;
  }

  memset(&flow.match_, 0, sizeof(flow.match_));
  flow.match_.type_ = daqiri::FlowMatchType::NORMAL;

  try {
    flow.match_.udp_src_ = flow_item["match"]["udp_src"].as<uint16_t>();
  } catch (const std::exception& e) {
    flow.match_.udp_src_ = 0;
  }

  try {
    flow.match_.udp_dst_ = flow_item["match"]["udp_dst"].as<uint16_t>();
  } catch (const std::exception& e) {
    flow.match_.udp_dst_ = 0;
  }

  try {
    flow.match_.ipv4_len_ = flow_item["match"]["ipv4_len"].as<uint16_t>();
  } catch (const std::exception& e) {
    flow.match_.ipv4_len_ = 0;
  }

  try {
    std::string ipv4_src = flow_item["match"]["ipv4_src"].as<std::string>();
    if (inet_pton(AF_INET, ipv4_src.c_str(), &addr) != 1) {
      DAQIRI_LOG_ERROR("Error parsing ipv4_src : {}", ipv4_src);
      return false;
    } else {
      flow.match_.ipv4_src_ = addr.s_addr;
    }
  } catch (const std::exception& e) {
    flow.match_.ipv4_src_ = INADDR_ANY;
  }

  try {
    std::string ipv4_dst = flow_item["match"]["ipv4_dst"].as<std::string>();
    if (inet_pton(AF_INET, ipv4_dst.c_str(), &addr) != 1) {
      DAQIRI_LOG_ERROR("Error parsing ipv4_dst : {}", ipv4_dst);
      return false;
    } else {
      flow.match_.ipv4_dst_ = addr.s_addr;
    }
  } catch (const std::exception& e) {
    flow.match_.ipv4_dst_ = INADDR_ANY;
  }

  // if none of the normal match criteria are defined, use flex item match
  if (   flow.match_.udp_src_  == 0
      && flow.match_.udp_dst_  == 0
      && flow.match_.ipv4_len_ == 0
      && flow.match_.ipv4_src_ == INADDR_ANY
      && flow.match_.ipv4_dst_ == INADDR_ANY
    ) {
    // No match criteria defined, use flex item match
    flow.match_.flex_item_match_.flex_item_id_ = flow_item["match"]["flex_item_id"].as<uint16_t>();
    flow.match_.flex_item_match_.val_ = flow_item["match"]["val"].as<uint32_t>();
    flow.match_.flex_item_match_.mask_ = flow_item["match"]["mask"].as<uint32_t>();
    flow.match_.type_ = daqiri::FlowMatchType::FLEX_ITEM;
    DAQIRI_LOG_INFO("Using flex item match: flex_item_id={}, val={}, mask={}",
                       flow.match_.flex_item_match_.flex_item_id_,
                       flow.match_.flex_item_match_.val_,
                       flow.match_.flex_item_match_.mask_);
  }

  return true;
}

/**
 * @brief Parse flex item configuration from a YAML node.
 *
 * @param flex_item The YAML node containing the flex item configuration.
 * @param flex_item_config The FlexItemConfig object to populate.
 * @return true if parsing was successful, false otherwise.
 */
bool YAML::convert<daqiri::NetworkConfig>::parse_flex_item_config(
    const YAML::Node& flex_item, daqiri::FlexItemConfig& flex_item_config) {
  try {
    flex_item_config.name_ = flex_item["name"].as<std::string>();
    flex_item_config.id_ = flex_item["id"].as<uint16_t>();
    flex_item_config.udp_dst_port_ = flex_item["udp_dst_port"].as<uint16_t>();
    flex_item_config.offset_ = flex_item["offset"].as<uint16_t>();
    if ((flex_item_config.offset_ % 4) != 0 || flex_item_config.offset_ > 28) {
      DAQIRI_LOG_CRITICAL("Flex item offset (in bytes) must be a multiple of 4 and less than 28");
      return false;
    }
  } catch (const std::exception& e) {
    DAQIRI_LOG_ERROR("Error parsing FlexItemConfig: {}", e.what());
    return false;
  }
  return true;
}

/**
 * @brief Parse reorder configuration from a YAML node.
 *
 * @param reorder_item The YAML node containing the reorder configuration.
 * @param reorder_config The ReorderConfig object to populate.
 * @return true if parsing was successful, false otherwise.
 */
bool YAML::convert<daqiri::NetworkConfig>::parse_reorder_config(
    const YAML::Node& reorder_item, daqiri::ReorderConfig& reorder_config) {
  auto parse_bit_field = [](const YAML::Node& node,
                            const char* field_name,
                            daqiri::ReorderBitFieldConfig& field_cfg) -> bool {
    if (!node[field_name]) {
      DAQIRI_LOG_ERROR("Missing required bit field '{}'", field_name);
      return false;
    }

    try {
      const auto& bit_field = node[field_name];
      field_cfg.bit_offset_ = bit_field["bit_offset"].as<uint16_t>();
      field_cfg.bit_width_ = bit_field["bit_width"].as<uint8_t>();
    } catch (const std::exception& e) {
      DAQIRI_LOG_ERROR("Failed to parse bit field '{}': {}", field_name, e.what());
      return false;
    }

    if (field_cfg.bit_width_ < 1 || field_cfg.bit_width_ > 32) {
      DAQIRI_LOG_ERROR("Invalid bit_width {} for '{}'. Supported range is [1, 32]",
                       field_cfg.bit_width_,
                       field_name);
      return false;
    }

    return true;
  };

  auto pow2_u64 = [](uint8_t exponent, uint64_t* out) -> bool {
    if (exponent > 63) { return false; }
    *out = (1ULL << exponent);
    return true;
  };

  try {
    reorder_config.name_ = reorder_item["name"].as<std::string>();
    reorder_config.reorder_type_ = reorder_item["reorder_type"].as<std::string>();
    reorder_config.memory_region_ = reorder_item["memory_region"].as<std::string>();
    reorder_config.payload_byte_offset_ = reorder_item["payload_byte_offset"].as<uint32_t>();

    if (!reorder_item["flow_ids"] || !reorder_item["flow_ids"].IsSequence()) {
      DAQIRI_LOG_ERROR("Reorder config '{}' requires a non-empty flow_ids sequence",
                       reorder_config.name_);
      return false;
    }

    for (const auto& flow_id_node : reorder_item["flow_ids"]) {
      reorder_config.flow_ids_.push_back(flow_id_node.as<uint16_t>());
    }
    if (reorder_config.flow_ids_.empty()) {
      DAQIRI_LOG_ERROR("Reorder config '{}' requires at least one flow ID",
                       reorder_config.name_);
      return false;
    }
  } catch (const std::exception& e) {
    DAQIRI_LOG_ERROR("Error parsing ReorderConfig: {}", e.what());
    return false;
  }

  if (reorder_config.reorder_type_ != "gpu" && reorder_config.reorder_type_ != "cpu") {
    DAQIRI_LOG_ERROR("Unsupported reorder_type '{}' in reorder config '{}'. Valid values are 'gpu' and 'cpu'",
                     reorder_config.reorder_type_,
                     reorder_config.name_);
    return false;
  }

  if (reorder_item["data_types"].IsDefined()) {
    const auto& data_types_node = reorder_item["data_types"];
    if (!data_types_node.IsMap()) {
      DAQIRI_LOG_ERROR("Reorder config '{}' data_types must be a map", reorder_config.name_);
      return false;
    }

    const auto input_node =
        data_types_node["input_type"] ? data_types_node["input_type"] : data_types_node["input"];
    const auto output_node =
        data_types_node["output_type"] ? data_types_node["output_type"] : data_types_node["output"];
    if (!input_node || !output_node) {
      DAQIRI_LOG_ERROR(
          "Reorder config '{}' data_types requires input_type and output_type",
          reorder_config.name_);
      return false;
    }

    try {
      reorder_config.data_types_.input_type_ =
          daqiri::reorder_data_type_from_string(input_node.as<std::string>());
      reorder_config.data_types_.output_type_ =
          daqiri::reorder_data_type_from_string(output_node.as<std::string>());
      const auto endianness_node = data_types_node["endianness"]
                                       ? data_types_node["endianness"]
                                       : data_types_node["input_endianness"];
      if (endianness_node) {
        reorder_config.data_types_.input_endianness_ =
            daqiri::reorder_endianness_from_string(endianness_node.as<std::string>());
      }
    } catch (const std::exception& e) {
      DAQIRI_LOG_ERROR("Failed to parse data_types in reorder config '{}': {}",
                       reorder_config.name_,
                       e.what());
      return false;
    }

    if (!daqiri::is_reorder_input_data_type(reorder_config.data_types_.input_type_)) {
      DAQIRI_LOG_ERROR(
          "Invalid reorder input_type '{}' in config '{}'. Valid values: int4, int8, int16, int32",
          daqiri::reorder_data_type_to_string(reorder_config.data_types_.input_type_),
          reorder_config.name_);
      return false;
    }
    if (!daqiri::is_reorder_output_data_type(reorder_config.data_types_.output_type_)) {
      DAQIRI_LOG_ERROR(
          "Invalid reorder output_type '{}' in config '{}'. Valid values: fp16, bf16, fp32, fp64, int32",
          daqiri::reorder_data_type_to_string(reorder_config.data_types_.output_type_),
          reorder_config.name_);
      return false;
    }
    if (reorder_config.data_types_.input_endianness_ == daqiri::ReorderEndianness::INVALID) {
      DAQIRI_LOG_ERROR(
          "Invalid reorder endianness '{}' in config '{}'. Valid values: host, network",
          daqiri::reorder_endianness_to_string(reorder_config.data_types_.input_endianness_),
          reorder_config.name_);
      return false;
    }

    reorder_config.data_types_.enabled_ = true;
  }

  if (!reorder_item["method"] || !reorder_item["method"].IsMap()) {
    DAQIRI_LOG_ERROR("Reorder config '{}' requires a method section", reorder_config.name_);
    return false;
  }

  const auto& method_node = reorder_item["method"];
  const bool has_seq_batch_number = method_node["seq_batch_number"].IsDefined();
  const bool has_seq_packets_per_batch = method_node["seq_packets_per_batch"].IsDefined();
  if (has_seq_batch_number == has_seq_packets_per_batch) {
    DAQIRI_LOG_ERROR(
        "Reorder config '{}' must define exactly one method: seq_batch_number or "
        "seq_packets_per_batch",
        reorder_config.name_);
    return false;
  }

  if (has_seq_batch_number) {
    const auto& seq_batch_node = method_node["seq_batch_number"];
    reorder_config.method_ = daqiri::ReorderMethod::SEQ_BATCH_NUMBER;

    if (!parse_bit_field(seq_batch_node, "sequence_number", reorder_config.seq_batch_number_.sequence_number_)) {
      return false;
    }
    if (!parse_bit_field(seq_batch_node, "batch_number", reorder_config.seq_batch_number_.batch_number_)) {
      return false;
    }

    uint64_t total_sequence_numbers = 0;
    uint64_t total_batches = 0;
    if (!pow2_u64(reorder_config.seq_batch_number_.sequence_number_.bit_width_, &total_sequence_numbers)
        || !pow2_u64(reorder_config.seq_batch_number_.batch_number_.bit_width_, &total_batches)) {
      DAQIRI_LOG_ERROR("Bit width too large in reorder config '{}'", reorder_config.name_);
      return false;
    }

    if (total_batches == 0 || (total_sequence_numbers % total_batches) != 0) {
      DAQIRI_LOG_ERROR(
          "Derived packets_per_batch is not integral in reorder config '{}' "
          "(seq_bits={}, batch_bits={})",
          reorder_config.name_,
          reorder_config.seq_batch_number_.sequence_number_.bit_width_,
          reorder_config.seq_batch_number_.batch_number_.bit_width_);
      return false;
    }

    const uint64_t derived_packets_per_batch = total_sequence_numbers / total_batches;
    if (derived_packets_per_batch == 0
        || derived_packets_per_batch > std::numeric_limits<uint32_t>::max()) {
      DAQIRI_LOG_ERROR("Derived packets_per_batch is out of range in reorder config '{}'",
                       reorder_config.name_);
      return false;
    }
    reorder_config.seq_batch_number_.packets_per_batch_ =
        static_cast<uint32_t>(derived_packets_per_batch);
  } else {
    const auto& seq_ppb_node = method_node["seq_packets_per_batch"];
    reorder_config.method_ = daqiri::ReorderMethod::SEQ_PACKETS_PER_BATCH;

    if (!parse_bit_field(seq_ppb_node, "sequence_number", reorder_config.seq_packets_per_batch_.sequence_number_)) {
      return false;
    }

    try {
      reorder_config.seq_packets_per_batch_.packets_per_batch_ =
          seq_ppb_node["packets_per_batch"].as<uint32_t>();
    } catch (const std::exception& e) {
      DAQIRI_LOG_ERROR("Failed to parse packets_per_batch in reorder config '{}': {}",
                       reorder_config.name_,
                       e.what());
      return false;
    }

    if (reorder_config.seq_packets_per_batch_.packets_per_batch_ == 0) {
      DAQIRI_LOG_ERROR("packets_per_batch must be > 0 in reorder config '{}'",
                       reorder_config.name_);
      return false;
    }

    uint64_t total_sequence_numbers = 0;
    if (!pow2_u64(reorder_config.seq_packets_per_batch_.sequence_number_.bit_width_,
                  &total_sequence_numbers)) {
      DAQIRI_LOG_ERROR("Bit width too large in reorder config '{}'", reorder_config.name_);
      return false;
    }

    if ((total_sequence_numbers
         % static_cast<uint64_t>(reorder_config.seq_packets_per_batch_.packets_per_batch_)) != 0) {
      DAQIRI_LOG_ERROR(
          "2^seq_bits must be divisible by packets_per_batch in reorder config '{}' "
          "(seq_bits={}, packets_per_batch={})",
          reorder_config.name_,
          reorder_config.seq_packets_per_batch_.sequence_number_.bit_width_,
          reorder_config.seq_packets_per_batch_.packets_per_batch_);
      return false;
    }
  }

  return true;
}

/**
 * @brief Parse memory region configuration from a YAML node.
 *
 * @param mr The YAML node containing the memory region configuration.
 * @param tmr The MemoryRegionConfig object to populate.
 * @return true if parsing was successful, false otherwise.
 */
bool YAML::convert<daqiri::NetworkConfig>::parse_memory_region_config(
    const YAML::Node& mr, daqiri::MemoryRegionConfig& tmr) {
  try {
    tmr.name_ = mr["name"].as<std::string>();
    tmr.kind_ =
        daqiri::GetMemoryKindFromString(mr["kind"].template as<std::string>());
    tmr.buf_size_ = mr["buf_size"].as<size_t>();
    tmr.num_bufs_ = mr["num_bufs"].as<size_t>();
    tmr.affinity_ = mr["affinity"].as<uint32_t>();
    if (mr["access"].IsDefined()) {
        tmr.access_ = daqiri::GetMemoryAccessPropertiesFromList(mr["access"]);
    } else {
      tmr.access_ = daqiri::MEM_ACCESS_LOCAL;
    }
    tmr.owned_ = mr["owned"].template as<bool>(true);
  } catch (const std::exception& e) {
    DAQIRI_LOG_ERROR("Error parsing MemoryRegionConfig: {}", e.what());
    return false;
  }
  return true;
}

namespace {

struct ParsedEndpointAddress {
  daqiri::SocketProtocol protocol = daqiri::SocketProtocol::INVALID;
  std::string host;
  uint16_t port = 0;
};

constexpr const char* kRoceEngineIbverbs = "ibverbs";

daqiri::SocketProtocol protocol_from_endpoint_scheme(const std::string& scheme) {
  if (scheme == "tcp") { return daqiri::SocketProtocol::TCP; }
  if (scheme == "udp") { return daqiri::SocketProtocol::UDP; }
  if (scheme == "rdma" || scheme == "roce") { return daqiri::SocketProtocol::ROCE; }
  return daqiri::SocketProtocol::INVALID;
}

std::string endpoint_scheme_from_protocol(daqiri::SocketProtocol protocol) {
  switch (protocol) {
    case daqiri::SocketProtocol::TCP:
      return "tcp";
    case daqiri::SocketProtocol::UDP:
      return "udp";
    case daqiri::SocketProtocol::ROCE:
      return "roce";
    default:
      return "";
  }
}

bool parse_endpoint_query(const std::string& query,
                          daqiri::SocketProtocol protocol,
                          const char* field_name) {
  if (query.empty()) { return true; }
  if (protocol != daqiri::SocketProtocol::ROCE) {
    DAQIRI_LOG_ERROR("{} query parameters are only supported for RoCE endpoints",
                     field_name);
    return false;
  }

  size_t start = 0;
  while (start <= query.size()) {
    const auto sep = query.find('&', start);
    const std::string item =
        query.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
    const auto eq = item.find('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 >= item.size()) {
      DAQIRI_LOG_ERROR("{} has invalid query parameter '{}'", field_name, item);
      return false;
    }

    const std::string key = item.substr(0, eq);
    const std::string value = item.substr(eq + 1);
    if (key != "engine") {
      DAQIRI_LOG_ERROR("{} only supports the 'engine' query parameter", field_name);
      return false;
    }
    if (value != kRoceEngineIbverbs) {
      DAQIRI_LOG_ERROR("{} engine '{}' is not supported. Valid value: {}",
                       field_name,
                       value,
                       kRoceEngineIbverbs);
      return false;
    }

    if (sep == std::string::npos) { break; }
    start = sep + 1;
  }

  return true;
}

bool parse_endpoint_addr(const std::string& value,
                         const char* field_name,
                         bool allow_missing_roce_port,
                         ParsedEndpointAddress& parsed) {
  const auto scheme_end = value.find("://");
  if (scheme_end == std::string::npos || scheme_end == 0) {
    DAQIRI_LOG_ERROR("{} must use '<scheme>://<ipv4>[:<port>]'", field_name);
    return false;
  }

  const std::string scheme = value.substr(0, scheme_end);
  parsed.protocol = protocol_from_endpoint_scheme(scheme);
  if (parsed.protocol == daqiri::SocketProtocol::INVALID) {
    DAQIRI_LOG_ERROR("Invalid scheme '{}' in {}. Valid schemes: tcp, udp, roce",
                     scheme,
                     field_name);
    return false;
  }

  const std::string endpoint = value.substr(scheme_end + 3);
  const auto query_sep = endpoint.find('?');
  const std::string authority = endpoint.substr(0, query_sep);
  const std::string query =
      query_sep == std::string::npos ? "" : endpoint.substr(query_sep + 1);
  if (!parse_endpoint_query(query, parsed.protocol, field_name)) { return false; }

  if (authority.empty() || authority.find('/') != std::string::npos) {
    DAQIRI_LOG_ERROR("{} must use '<scheme>://<ipv4>[:<port>]'", field_name);
    return false;
  }

  const auto port_sep = authority.rfind(':');
  if (port_sep == std::string::npos) {
    if (allow_missing_roce_port &&
        parsed.protocol == daqiri::SocketProtocol::ROCE) {
      parsed.host = authority;
      return true;
    }
    DAQIRI_LOG_ERROR("{} must include an IPv4 address and port", field_name);
    return false;
  }
  if (port_sep == 0 || port_sep + 1 >= authority.size()) {
    DAQIRI_LOG_ERROR("{} must include an IPv4 address and port", field_name);
    return false;
  }

  parsed.host = authority.substr(0, port_sep);
  const std::string port_str = authority.substr(port_sep + 1);
  try {
    const unsigned long port = std::stoul(port_str);
    if (port > std::numeric_limits<uint16_t>::max() ||
        (port == 0 &&
         !(allow_missing_roce_port &&
           parsed.protocol == daqiri::SocketProtocol::ROCE))) {
      DAQIRI_LOG_ERROR("{} port '{}' is out of range", field_name, port_str);
      return false;
    }
    parsed.port = static_cast<uint16_t>(port);
  } catch (const std::exception&) {
    DAQIRI_LOG_ERROR("{} port '{}' is not a valid integer", field_name, port_str);
    return false;
  }

  return true;
}

bool apply_endpoint_addr(const std::string& value,
                         const char* field_name,
                         bool allow_missing_roce_port,
                         daqiri::SocketProtocol& protocol,
                         std::string& ip,
                         uint16_t& port) {
  ParsedEndpointAddress parsed;
  if (!parse_endpoint_addr(value, field_name, allow_missing_roce_port, parsed)) {
    return false;
  }

  if (protocol == daqiri::SocketProtocol::INVALID) {
    protocol = parsed.protocol;
  } else if (protocol != parsed.protocol) {
    DAQIRI_LOG_ERROR("{} scheme '{}' conflicts with inferred protocol '{}'",
                     field_name,
                     value.substr(0, value.find("://")),
                     daqiri::socket_protocol_to_string(protocol));
    return false;
  }

  ip = parsed.host;
  port = parsed.port;
  return true;
}

std::string make_endpoint_addr(daqiri::SocketProtocol protocol,
                               const std::string& ip,
                               uint16_t port) {
  const std::string scheme = endpoint_scheme_from_protocol(protocol);
  if (scheme.empty() || ip.empty()) { return ""; }
  if (protocol == daqiri::SocketProtocol::ROCE && port == 0) {
    return scheme + "://" + ip;
  }
  if (port == 0) { return ""; }
  return scheme + "://" + ip + ":" + std::to_string(port);
}

}  // namespace

bool YAML::convert<daqiri::NetworkConfig>::parse_socket_config(
    const YAML::Node& socket_item,
    daqiri::SocketConfig& socket_cfg,
    daqiri::SocketProtocol& protocol) {
  try {
    socket_cfg.mode_ = daqiri::GetSocketModeFromString(
        socket_item["mode"].template as<std::string>());
    if (socket_cfg.mode_ == daqiri::SocketMode::INVALID) {
      DAQIRI_LOG_ERROR("Invalid socket mode '{}'. Valid values: client, server",
                       socket_item["mode"].template as<std::string>());
      return false;
    }

    socket_cfg.local_addr_ = socket_item["local_addr"].template as<std::string>("");
    socket_cfg.remote_addr_ = socket_item["remote_addr"].template as<std::string>("");
    const bool has_local_addr = !socket_cfg.local_addr_.empty();
    const bool has_remote_addr = !socket_cfg.remote_addr_.empty();
    const bool has_legacy_local = socket_item["local_ip"].IsDefined() ||
                                  socket_item["local_port"].IsDefined();
    const bool has_legacy_remote = socket_item["remote_ip"].IsDefined() ||
                                   socket_item["remote_port"].IsDefined();

    if (has_local_addr && has_legacy_local) {
      DAQIRI_LOG_ERROR(
          "socket_config.local_addr cannot be combined with local_ip/local_port");
      return false;
    }
    if (has_remote_addr && has_legacy_remote) {
      DAQIRI_LOG_ERROR(
          "socket_config.remote_addr cannot be combined with remote_ip/remote_port");
      return false;
    }

    socket_cfg.local_ip_ = socket_item["local_ip"].template as<std::string>("");
    socket_cfg.remote_ip_ = socket_item["remote_ip"].template as<std::string>("");
    socket_cfg.local_port_ = socket_item["local_port"].as<uint16_t>(0);
    socket_cfg.remote_port_ = socket_item["remote_port"].as<uint16_t>(0);

    if (has_local_addr &&
        !apply_endpoint_addr(socket_cfg.local_addr_,
                             "socket_config.local_addr",
                             socket_cfg.mode_ == daqiri::SocketMode::CLIENT,
                             protocol,
                             socket_cfg.local_ip_,
                             socket_cfg.local_port_)) {
      return false;
    }
    if (has_remote_addr &&
        !apply_endpoint_addr(socket_cfg.remote_addr_,
                             "socket_config.remote_addr",
                             false,
                             protocol,
                             socket_cfg.remote_ip_,
                             socket_cfg.remote_port_)) {
      return false;
    }

    if (protocol == daqiri::SocketProtocol::INVALID) {
      DAQIRI_LOG_ERROR(
          "Socket configs must set protocol or use local_addr/remote_addr URI "
          "schemes");
      return false;
    }

    if (!has_local_addr) {
      socket_cfg.local_addr_ = make_endpoint_addr(
          protocol, socket_cfg.local_ip_, socket_cfg.local_port_);
    }
    if (!has_remote_addr) {
      socket_cfg.remote_addr_ = make_endpoint_addr(
          protocol, socket_cfg.remote_ip_, socket_cfg.remote_port_);
    }

    socket_cfg.max_payload_size_ = socket_item["max_payload_size"].as<uint16_t>(0);
    socket_cfg.max_burst_interval_ms_ = socket_item["max_burst_interval_ms"].as<uint64_t>(0);
    socket_cfg.min_ipg_ns_ = socket_item["min_ipg_ns"].as<uint32_t>(0);
    socket_cfg.retry_connect_s_ = socket_item["retry_connect_s"].as<int32_t>(1);

    const bool roce_client = socket_cfg.mode_ == daqiri::SocketMode::CLIENT &&
                             protocol == daqiri::SocketProtocol::ROCE;
    if (roce_client && (has_remote_addr || has_legacy_remote)) {
      DAQIRI_LOG_ERROR("RoCE client peer endpoints belong in application config, "
                       "not socket_config.remote_addr");
      return false;
    }

    if (socket_cfg.mode_ == daqiri::SocketMode::SERVER) {
      if (socket_cfg.local_ip_.empty()) {
        DAQIRI_LOG_ERROR("socket_config.local_addr is required for server mode");
        return false;
      }
      if (socket_cfg.local_port_ == 0) {
        DAQIRI_LOG_ERROR("socket_config.local_addr must include a non-zero port");
        return false;
      }
    } else if (roce_client) {
      if (socket_cfg.local_ip_.empty()) {
        DAQIRI_LOG_ERROR("socket_config.local_addr is required for RoCE client mode");
        return false;
      }
    } else {
      if (socket_cfg.remote_ip_.empty()) {
        DAQIRI_LOG_ERROR("socket_config.remote_addr is required for client mode");
        return false;
      }
      if (socket_cfg.remote_port_ == 0) {
        DAQIRI_LOG_ERROR("socket_config.remote_addr must include a non-zero port");
        return false;
      }
    }
  } catch (const std::exception& e) {
    DAQIRI_LOG_ERROR("Error parsing SocketConfig: {}", e.what());
    return false;
  }
  return true;
}

bool YAML::convert<daqiri::NetworkConfig>::parse_roce_config(
    const YAML::Node& roce_item, daqiri::RoCEConfig& roce_cfg) {
  try {
    roce_cfg.transport_mode_ = daqiri::GetRDMATransportModeFromString(
        roce_item["transport_mode"].template as<std::string>());
    if (roce_cfg.transport_mode_ == daqiri::RDMATransportMode::INVALID) {
      DAQIRI_LOG_ERROR("Invalid roce_config.transport_mode '{}'. Valid values: RC, UC, UD",
                       roce_item["transport_mode"].template as<std::string>());
      return false;
    }
  } catch (const std::exception& e) {
    DAQIRI_LOG_ERROR("Error parsing RoCEConfig: {}", e.what());
    return false;
  }
  return true;
}

/**
 * @brief Parse common queue configuration from a YAML node.
 *
 * @param q_item The YAML node containing the queue configuration.
 * @param common The CommonQueueConfig object to populate.
 * @param parse_memory_regions True if memory regions should be parsed, false otherwise.
 * @return true if parsing was successful, false otherwise.
 */
bool parse_common_queue_config(const YAML::Node& q_item,
                               daqiri::CommonQueueConfig& common,
                               bool parse_memory_regions) {
  try {
    common.name_ = q_item["name"].as<std::string>();
    common.id_ = q_item["id"].as<int>();
    common.cpu_core_ = q_item["cpu_core"].as<std::string>();
    common.batch_size_ = q_item["batch_size"].as<int>();
    common.extra_queue_config_ = nullptr;
    if (q_item["memory_regions"].IsDefined()) {
      if (!parse_memory_regions) {
        DAQIRI_LOG_WARN("Memory regions in queue section not used in RoCE engine for queue: {}",
          common.name_);
      }
      else {
        const auto& mrs = q_item["memory_regions"];
        if (mrs.size() > 0) { common.mrs_.reserve(mrs.size()); }
        for (const auto& mr : mrs) { common.mrs_.push_back(mr.as<std::string>()); }
      }
    }
  } catch (const std::exception& e) {
    DAQIRI_LOG_ERROR("Error parsing CommonQueueConfig: {}", e.what());
    return false;
  }
  if (parse_memory_regions && common.mrs_.empty()) {
    DAQIRI_LOG_ERROR("No memory regions defined for queue: {}", common.name_);
    return false;
  }
  return true;
}

/**
 * @brief Parse common RX queue configuration from a YAML node.
 *
 * @param q_item The YAML node containing the RX queue configuration.
 * @param q The RxQueueConfig object to populate.
 * @return true if parsing was successful, false otherwise.
 */
bool YAML::convert<daqiri::NetworkConfig>::parse_rx_queue_common_config(
    const YAML::Node& q_item, daqiri::RxQueueConfig& q,
    bool parse_memory_regions) {
  if (!parse_common_queue_config(q_item, q.common_, parse_memory_regions)) { return false; }
  return true;
}

/**
 * @brief Parse RX queue configuration from a YAML node.
 *
 * @param q_item The YAML node containing the RX queue configuration.
 * @param engine_type The engine type.
 * @param q The RxQueueConfig object to populate.
 * @param parse_memory_regions True if memory regions should be parsed, false otherwise.
 * @return true if parsing was successful, false otherwise.
 */
bool YAML::convert<daqiri::NetworkConfig>::parse_rx_queue_config(
    const YAML::Node& q_item, const daqiri::EngineType& engine_type,
    daqiri::RxQueueConfig& q, bool parse_memory_regions) {
  try {
    daqiri::EngineType _engine_type = engine_type;

    if (!parse_rx_queue_common_config(q_item, q, parse_memory_regions)) { return false; }

    if (engine_type == daqiri::EngineType::DEFAULT) {
      _engine_type = daqiri::EngineFactory::get_default_engine_type();
    }
  } catch (const std::exception& e) {
    DAQIRI_LOG_ERROR("Error parsing RxQueueConfig: {}", e.what());
    return false;
  }
  return true;
}

/**
 * @brief Parse common TX queue configuration from a YAML node.
 *
 * @param q_item The YAML node containing the TX queue configuration.
 * @param q The TxQueueConfig object to populate.
 * @return true if parsing was successful, false otherwise.
 */
bool YAML::convert<daqiri::NetworkConfig>::parse_tx_queue_common_config(
    const YAML::Node& q_item, daqiri::TxQueueConfig& q,
    bool parse_memory_regions) {
  if (!parse_common_queue_config(q_item, q.common_, parse_memory_regions)) { return false; }
  try {
    if (q_item["offloads"].IsDefined()) {
      const auto& offload = q_item["offloads"];
      q.common_.offloads_.reserve(offload.size());
      for (const auto& off : offload) {
        q.common_.offloads_.push_back(off.as<std::string>());
      }
    }
  } catch (const std::exception& e) {
    DAQIRI_LOG_ERROR("Error parsing TxQueueConfig: {}", e.what());
    return false;
  }
  return true;
}

/**
 * @brief Parse TX queue configuration from a YAML node.
 *
 * @param q_item The YAML node containing the TX queue configuration.
 * @param engine_type The engine type.
 * @param q The TxQueueConfig object to populate.
 * @return true if parsing was successful, false otherwise.
 */
bool YAML::convert<daqiri::NetworkConfig>::parse_tx_queue_config(
    const YAML::Node& q_item, const daqiri::EngineType& engine_type,
    daqiri::TxQueueConfig& q, bool parse_memory_regions) {
  try {
    daqiri::EngineType _engine_type = engine_type;

    if (engine_type == daqiri::EngineType::DEFAULT) {
      _engine_type = daqiri::EngineFactory::get_default_engine_type();
    }

    if (!parse_tx_queue_common_config(q_item, q, parse_memory_regions)) { return false; }

  } catch (const std::exception& e) {
    DAQIRI_LOG_ERROR("Error parsing TxQueueConfig: {}", e.what());
    return false;
  }
  return true;
}
