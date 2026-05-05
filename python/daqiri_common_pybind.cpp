/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
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

#include "src/common.h"

#include <cuda_runtime.h>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <yaml-cpp/yaml.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace py = pybind11;
using pybind11::literals::operator""_a;

namespace daqiri {
namespace {

constexpr int kIpProtoUdp = 17;

size_t buffer_nbytes(const py::buffer_info &info) {
  if (info.size < 0 || info.itemsize < 0) {
    throw py::value_error("buffer has invalid size metadata");
  }
  return static_cast<size_t>(info.size) * static_cast<size_t>(info.itemsize);
}

bool is_cuda_device_pointer(const void *ptr) {
  if (ptr == nullptr) {
    return false;
  }

  cudaPointerAttributes attr{};
  const cudaError_t err = cudaPointerGetAttributes(&attr, ptr);
  if (err != cudaSuccess) {
    // cudaPointerGetAttributes sets the per-thread last error for non-CUDA
    // pointers.
    cudaGetLastError();
    return false;
  }

#if CUDART_VERSION >= 10000
  return attr.type == cudaMemoryTypeDevice ||
         attr.type == cudaMemoryTypeManaged;
#else
  return attr.memoryType == cudaMemoryTypeDevice;
#endif
}

Status copy_host_to_pointer(void *dst, const void *src, size_t nbytes) {
  if (dst == nullptr || src == nullptr) {
    return Status::NULL_PTR;
  }
  if (nbytes == 0) {
    return Status::SUCCESS;
  }

  if (is_cuda_device_pointer(dst)) {
    const cudaError_t err =
        cudaMemcpy(dst, src, nbytes, cudaMemcpyHostToDevice);
    if (err != cudaSuccess) {
      DAQIRI_LOG_ERROR("cudaMemcpy host-to-device failed: {}",
                       cudaGetErrorString(err));
      return Status::GENERIC_FAILURE;
    }
  } else {
    std::memcpy(dst, src, nbytes);
  }

  return Status::SUCCESS;
}

Status copy_pointer_to_host(const void *src, void *dst, size_t nbytes) {
  if (dst == nullptr || src == nullptr) {
    return Status::NULL_PTR;
  }
  if (nbytes == 0) {
    return Status::SUCCESS;
  }

  if (is_cuda_device_pointer(src)) {
    const cudaError_t err =
        cudaMemcpy(dst, src, nbytes, cudaMemcpyDeviceToHost);
    if (err != cudaSuccess) {
      DAQIRI_LOG_ERROR("cudaMemcpy device-to-host failed: {}",
                       cudaGetErrorString(err));
      return Status::GENERIC_FAILURE;
    }
  } else {
    std::memcpy(dst, src, nbytes);
  }

  return Status::SUCCESS;
}

Status validate_lengths(BurstParams *burst, const std::vector<int> &lens) {
  if (burst == nullptr) {
    return Status::NULL_PTR;
  }
  if (lens.empty() || lens.size() > MAX_NUM_SEGS) {
    return Status::INVALID_PARAMETER;
  }
  if (burst->hdr.hdr.num_segs > 0 &&
      lens.size() != static_cast<size_t>(burst->hdr.hdr.num_segs)) {
    return Status::INVALID_PARAMETER;
  }
  return Status::SUCCESS;
}

Status set_packet_lengths_from_vector(BurstParams *burst, int idx,
                                      const std::vector<int> &lens) {
  const Status validation = validate_lengths(burst, lens);
  if (validation != Status::SUCCESS) {
    return validation;
  }

  switch (lens.size()) {
  case 1:
    return set_packet_lengths(burst, idx, {lens[0]});
  case 2:
    return set_packet_lengths(burst, idx, {lens[0], lens[1]});
  case 3:
    return set_packet_lengths(burst, idx, {lens[0], lens[1], lens[2]});
  case 4:
    return set_packet_lengths(burst, idx, {lens[0], lens[1], lens[2], lens[3]});
  default:
    return Status::INVALID_PARAMETER;
  }
}

Status set_all_packet_lengths_from_vector(BurstParams *burst,
                                          const std::vector<int> &lens) {
  const Status validation = validate_lengths(burst, lens);
  if (validation != Status::SUCCESS) {
    return validation;
  }

  switch (lens.size()) {
  case 1:
    return set_all_packet_lengths(burst, {lens[0]});
  case 2:
    return set_all_packet_lengths(burst, {lens[0], lens[1]});
  case 3:
    return set_all_packet_lengths(burst, {lens[0], lens[1], lens[2]});
  case 4:
    return set_all_packet_lengths(burst, {lens[0], lens[1], lens[2], lens[3]});
  default:
    return Status::INVALID_PARAMETER;
  }
}

Status copy_buffer_to_segment_packet_impl(BurstParams *burst, int seg, int idx,
                                          py::buffer data,
                                          py::object nbytes_obj,
                                          size_t src_offset,
                                          size_t dst_offset) {
  py::buffer_info info = data.request();
  const size_t total = buffer_nbytes(info);
  if (src_offset > total) {
    return Status::INVALID_PARAMETER;
  }

  const size_t available = total - src_offset;
  const size_t nbytes =
      nbytes_obj.is_none() ? available : py::cast<size_t>(nbytes_obj);
  if (nbytes > available) {
    return Status::INVALID_PARAMETER;
  }

  auto *dst = static_cast<uint8_t *>(get_segment_packet_ptr(burst, seg, idx));
  if (dst == nullptr) {
    return Status::NULL_PTR;
  }
  auto *src = static_cast<const uint8_t *>(info.ptr);

  py::gil_scoped_release release;
  return copy_host_to_pointer(dst + dst_offset, src + src_offset, nbytes);
}

py::tuple segment_packet_bytes_impl(BurstParams *burst, int seg, int idx,
                                    py::object nbytes_obj, size_t src_offset) {
  size_t nbytes = 0;
  if (nbytes_obj.is_none()) {
    const uint32_t len = get_segment_packet_length(burst, seg, idx);
    if (src_offset > len) {
      return py::make_tuple(Status::INVALID_PARAMETER, py::bytes(""));
    }
    nbytes = static_cast<size_t>(len) - src_offset;
  } else {
    nbytes = py::cast<size_t>(nbytes_obj);
  }

  auto *src =
      static_cast<const uint8_t *>(get_segment_packet_ptr(burst, seg, idx));
  if (src == nullptr) {
    return py::make_tuple(Status::NULL_PTR, py::bytes(""));
  }

  std::string out(nbytes, '\0');
  Status status = Status::SUCCESS;
  {
    py::gil_scoped_release release;
    status = copy_pointer_to_host(src + src_offset, out.data(), nbytes);
  }
  if (status != Status::SUCCESS) {
    out.clear();
  }
  return py::make_tuple(status, py::bytes(out));
}

Status daqiri_init_from_python(py::object config_obj) {
  try {
    if (py::isinstance<py::str>(config_obj) ||
        py::isinstance<py::bytes>(config_obj)) {
      const auto yaml_string_or_path = py::cast<std::string>(config_obj);
      py::gil_scoped_release release;
      return daqiri_init(yaml_string_or_path);
    }

    if (py::isinstance<py::dict>(config_obj)) {
      py::object yaml_module = py::module_::import("yaml");
      py::object py_yaml_str = yaml_module.attr("dump")(
          config_obj, py::arg("default_flow_style") = false);
      const auto yaml_str = py::cast<std::string>(py_yaml_str);
      py::gil_scoped_release release;
      return daqiri_init_from_yaml_string(yaml_str);
    }

    if (py::isinstance<NetworkConfig>(config_obj)) {
      auto &config = config_obj.cast<NetworkConfig &>();
      py::gil_scoped_release release;
      return daqiri_init(config);
    }

    if (py::hasattr(config_obj, "value")) {
      const auto yaml_string_or_path_obj = py::str(config_obj.attr("value"));
      const auto yaml_string_or_path =
          py::cast<std::string>(yaml_string_or_path_obj);
      py::gil_scoped_release release;
      return daqiri_init(yaml_string_or_path);
    }

    if (py::hasattr(config_obj, "as_dict")) {
      py::object yaml_module = py::module_::import("yaml");
      py::object py_yaml_str = yaml_module.attr("dump")(
          config_obj.attr("as_dict")(), py::arg("default_flow_style") = false);
      const auto yaml_str = py::cast<std::string>(py_yaml_str);
      py::gil_scoped_release release;
      return daqiri_init_from_yaml_string(yaml_str);
    }

    const auto yaml_string_or_path_obj = py::str(config_obj);
    const auto yaml_string_or_path =
        py::cast<std::string>(yaml_string_or_path_obj);
    py::gil_scoped_release release;
    return daqiri_init(yaml_string_or_path);
  } catch (const py::error_already_set &e) {
    DAQIRI_LOG_ERROR("Python config conversion failed: {}", e.what());
    return Status::INTERNAL_ERROR;
  } catch (const YAML::Exception &e) {
    DAQIRI_LOG_ERROR("YAML parsing error in Python config: {}", e.what());
    return Status::INVALID_PARAMETER;
  } catch (const std::exception &e) {
    DAQIRI_LOG_ERROR("Failed to initialize DAQIRI from Python config: {}",
                     e.what());
    return Status::INTERNAL_ERROR;
  }
}

void bind_enums(py::module_ &m) {
  py::enum_<Status>(m, "Status")
      .value("SUCCESS", Status::SUCCESS)
      .value("NULL_PTR", Status::NULL_PTR)
      .value("NO_FREE_BURST_BUFFERS", Status::NO_FREE_BURST_BUFFERS)
      .value("NO_FREE_PACKET_BUFFERS", Status::NO_FREE_PACKET_BUFFERS)
      .value("NOT_READY", Status::NOT_READY)
      .value("INVALID_PARAMETER", Status::INVALID_PARAMETER)
      .value("NO_SPACE_AVAILABLE", Status::NO_SPACE_AVAILABLE)
      .value("NOT_SUPPORTED", Status::NOT_SUPPORTED)
      .value("GENERIC_FAILURE", Status::GENERIC_FAILURE)
      .value("CONNECT_FAILURE", Status::CONNECT_FAILURE)
      .value("INTERNAL_ERROR", Status::INTERNAL_ERROR);

  py::enum_<RDMAOpCode>(m, "RDMAOpCode")
      .value("CONNECT", RDMAOpCode::CONNECT)
      .value("SEND", RDMAOpCode::SEND)
      .value("RECEIVE", RDMAOpCode::RECEIVE)
      .value("RDMA_WRITE", RDMAOpCode::RDMA_WRITE)
      .value("RDMA_WRITE_IMM", RDMAOpCode::RDMA_WRITE_IMM)
      .value("RDMA_READ", RDMAOpCode::RDMA_READ)
      .value("RDMA_READ_IMM", RDMAOpCode::RDMA_READ_IMM)
      .value("INVALID", RDMAOpCode::INVALID);

  py::enum_<RDMACompletionType>(m, "RDMACompletionType")
      .value("RX", RDMACompletionType::RX)
      .value("TX", RDMACompletionType::TX)
      .value("INVALID", RDMACompletionType::INVALID);

  py::enum_<ManagerType>(m, "ManagerType")
      .value("UNKNOWN", ManagerType::UNKNOWN)
      .value("DEFAULT", ManagerType::DEFAULT)
      .value("DPDK", ManagerType::DPDK)
      .value("SOCKET", ManagerType::SOCKET)
      .value("RDMA", ManagerType::RDMA);

  py::enum_<Direction>(m, "Direction")
      .value("RX", Direction::RX)
      .value("TX", Direction::TX)
      .value("TX_RX", Direction::TX_RX);

  py::enum_<BufferLocation>(m, "BufferLocation")
      .value("CPU", BufferLocation::CPU)
      .value("GPU", BufferLocation::GPU)
      .value("CPU_GPU_SPLIT", BufferLocation::CPU_GPU_SPLIT);

  py::enum_<MemoryKind>(m, "MemoryKind")
      .value("HOST", MemoryKind::HOST)
      .value("HOST_PINNED", MemoryKind::HOST_PINNED)
      .value("HUGE", MemoryKind::HUGE)
      .value("DEVICE", MemoryKind::DEVICE)
      .value("INVALID", MemoryKind::INVALID);

  py::enum_<StreamType>(m, "StreamType")
      .value("RAW", StreamType::RAW)
      .value("SOCKET", StreamType::SOCKET)
      .value("INVALID", StreamType::INVALID);

  py::enum_<SocketProtocol>(m, "SocketProtocol")
      .value("TCP", SocketProtocol::TCP)
      .value("UDP", SocketProtocol::UDP)
      .value("ROCE", SocketProtocol::ROCE)
      .value("INVALID", SocketProtocol::INVALID);

  py::enum_<LoopbackType>(m, "LoopbackType")
      .value("DISABLED", LoopbackType::DISABLED)
      .value("LOOPBACK_TYPE_SW", LoopbackType::LOOPBACK_TYPE_SW);

  py::enum_<RDMAMode>(m, "RDMAMode")
      .value("CLIENT", RDMAMode::CLIENT)
      .value("SERVER", RDMAMode::SERVER)
      .value("INVALID", RDMAMode::INVALID);

  py::enum_<RDMATransportMode>(m, "RDMATransportMode")
      .value("RC", RDMATransportMode::RC)
      .value("UC", RDMATransportMode::UC)
      .value("UD", RDMATransportMode::UD)
      .value("INVALID", RDMATransportMode::INVALID);

  py::enum_<SocketMode>(m, "SocketMode")
      .value("CLIENT", SocketMode::CLIENT)
      .value("SERVER", SocketMode::SERVER)
      .value("INVALID", SocketMode::INVALID);

  py::enum_<FlowType>(m, "FlowType").value("QUEUE", FlowType::QUEUE);

  py::enum_<FlowMatchType>(m, "FlowMatchType")
      .value("NORMAL", FlowMatchType::NORMAL)
      .value("FLEX_ITEM", FlowMatchType::FLEX_ITEM);

  py::enum_<ReorderMethod>(m, "ReorderMethod")
      .value("INVALID", ReorderMethod::INVALID)
      .value("SEQ_BATCH_NUMBER", ReorderMethod::SEQ_BATCH_NUMBER)
      .value("SEQ_PACKETS_PER_BATCH", ReorderMethod::SEQ_PACKETS_PER_BATCH);

  py::enum_<ReorderDataType>(m, "ReorderDataType")
      .value("SAME", ReorderDataType::SAME)
      .value("INT4", ReorderDataType::INT4)
      .value("INT8", ReorderDataType::INT8)
      .value("INT16", ReorderDataType::INT16)
      .value("INT32", ReorderDataType::INT32)
      .value("FP16", ReorderDataType::FP16)
      .value("BF16", ReorderDataType::BF16)
      .value("FP32", ReorderDataType::FP32)
      .value("FP64", ReorderDataType::FP64)
      .value("INVALID", ReorderDataType::INVALID);

  py::enum_<ReorderEndianness>(m, "ReorderEndianness")
      .value("HOST", ReorderEndianness::HOST)
      .value("NETWORK", ReorderEndianness::NETWORK)
      .value("INVALID", ReorderEndianness::INVALID);

  py::enum_<LogLevel::Level>(m, "LogLevel")
      .value("TRACE", LogLevel::TRACE)
      .value("DEBUG", LogLevel::DEBUG)
      .value("INFO", LogLevel::INFO)
      .value("WARN", LogLevel::WARN)
      .value("ERROR", LogLevel::ERROR)
      .value("CRITICAL", LogLevel::CRITICAL)
      .value("OFF", LogLevel::OFF);

  py::enum_<ErrorGlobalStats>(m, "ErrorGlobalStats")
      .value("OUT_OF_RX_BUFFERS", ErrorGlobalStats::OUT_OF_RX_BUFFERS)
      .value("RX_QUEUE_FULL", ErrorGlobalStats::RX_QUEUE_FULL)
      .value("METADATA_BUF_DEPLETED", ErrorGlobalStats::METADATA_BUF_DEPLETED)
      .value("SENTINEL", ErrorGlobalStats::SENTINEL);
}

void bind_config_types(py::module_ &m) {
  py::class_<ReorderBurstInfo>(m, "ReorderBurstInfo")
      .def(py::init<>())
      .def_readwrite("batch_id", &ReorderBurstInfo::batch_id)
      .def_readwrite("source_packet_count",
                     &ReorderBurstInfo::source_packet_count)
      .def_readwrite("packets_per_batch", &ReorderBurstInfo::packets_per_batch)
      .def_readwrite("payload_len", &ReorderBurstInfo::payload_len)
      .def_readwrite("aggregate_len", &ReorderBurstInfo::aggregate_len)
      .def_readwrite("burst_flags", &ReorderBurstInfo::burst_flags);

  py::class_<BurstHeaderParams>(m, "BurstHeaderParams")
      .def(py::init<>())
      .def_readwrite("num_pkts", &BurstHeaderParams::num_pkts)
      .def_readwrite("port_id", &BurstHeaderParams::port_id)
      .def_readwrite("q_id", &BurstHeaderParams::q_id)
      .def_readwrite("num_segs", &BurstHeaderParams::num_segs)
      .def_readwrite("nbytes", &BurstHeaderParams::nbytes)
      .def_readwrite("first_pkt_addr", &BurstHeaderParams::first_pkt_addr)
      .def_readwrite("max_pkt", &BurstHeaderParams::max_pkt)
      .def_readwrite("max_pkt_size", &BurstHeaderParams::max_pkt_size)
      .def_readwrite("gpu_pkt0_idx", &BurstHeaderParams::gpu_pkt0_idx)
      .def_readwrite("gpu_pkt0_addr", &BurstHeaderParams::gpu_pkt0_addr)
      .def_readwrite("burst_flags", &BurstHeaderParams::burst_flags);

  py::class_<BurstHeader>(m, "BurstHeader")
      .def(py::init<>())
      .def_readwrite("hdr", &BurstHeader::hdr);

  py::class_<BurstParams>(m, "BurstParams")
      .def(py::init<>())
      .def_readwrite("hdr", &BurstParams::hdr)
      .def_property(
          "rdma_conn_id",
          [](const BurstParams &burst) { return burst.rdma_hdr.conn_id; },
          [](BurstParams &burst, uintptr_t conn_id) {
            burst.rdma_hdr.conn_id = conn_id;
          })
      .def_property(
          "rdma_wr_id",
          [](const BurstParams &burst) { return burst.rdma_hdr.wr_id; },
          [](BurstParams &burst, uint64_t wr_id) {
            burst.rdma_hdr.wr_id = wr_id;
          });

  py::class_<RDMAConfig>(m, "RDMAConfig")
      .def(py::init<>())
      .def_readwrite("mode", &RDMAConfig::mode_)
      .def_readwrite("transport_mode", &RDMAConfig::xmode_)
      .def_readwrite("port", &RDMAConfig::port_);

  py::class_<CommonQueueConfig>(m, "CommonQueueConfig")
      .def(py::init<>())
      .def_readwrite("name", &CommonQueueConfig::name_)
      .def_readwrite("id", &CommonQueueConfig::id_)
      .def_readwrite("batch_size", &CommonQueueConfig::batch_size_)
      .def_readwrite("split_boundary", &CommonQueueConfig::split_boundary_)
      .def_readwrite("cpu_core", &CommonQueueConfig::cpu_core_)
      .def_readwrite("memory_regions", &CommonQueueConfig::mrs_)
      .def_readwrite("offloads", &CommonQueueConfig::offloads_);

  py::class_<MemoryRegionConfig>(m, "MemoryRegionConfig")
      .def(py::init<>())
      .def_readwrite("name", &MemoryRegionConfig::name_)
      .def_readwrite("kind", &MemoryRegionConfig::kind_)
      .def_readwrite("affinity", &MemoryRegionConfig::affinity_)
      .def_readwrite("access", &MemoryRegionConfig::access_)
      .def_readwrite("buf_size", &MemoryRegionConfig::buf_size_)
      .def_readwrite("adj_size", &MemoryRegionConfig::adj_size_)
      .def_readwrite("ttl_size", &MemoryRegionConfig::ttl_size_)
      .def_readwrite("num_bufs", &MemoryRegionConfig::num_bufs_)
      .def_readwrite("owned", &MemoryRegionConfig::owned_);

  py::class_<RxQueueConfig>(m, "RxQueueConfig")
      .def(py::init<>())
      .def_readwrite("common", &RxQueueConfig::common_)
      .def_readwrite("timeout_us", &RxQueueConfig::timeout_us_);

  py::class_<TxQueueConfig>(m, "TxQueueConfig")
      .def(py::init<>())
      .def_readwrite("common", &TxQueueConfig::common_);

  py::class_<FlowAction>(m, "FlowAction")
      .def(py::init<>())
      .def_readwrite("type", &FlowAction::type_)
      .def_readwrite("id", &FlowAction::id_);

  py::class_<FlexItemMatch>(m, "FlexItemMatch")
      .def(py::init<>())
      .def_readwrite("flex_item_id", &FlexItemMatch::flex_item_id_)
      .def_readwrite("val", &FlexItemMatch::val_)
      .def_readwrite("mask", &FlexItemMatch::mask_);

  py::class_<FlowMatch>(m, "FlowMatch")
      .def(py::init<>())
      .def_readwrite("type", &FlowMatch::type_)
      .def_readwrite("udp_src", &FlowMatch::udp_src_)
      .def_readwrite("udp_dst", &FlowMatch::udp_dst_)
      .def_readwrite("ipv4_len", &FlowMatch::ipv4_len_)
      .def_readwrite("ipv4_src", &FlowMatch::ipv4_src_)
      .def_readwrite("ipv4_dst", &FlowMatch::ipv4_dst_)
      .def_readwrite("flex_item_match", &FlowMatch::flex_item_match_);

  py::class_<FlowConfig>(m, "FlowConfig")
      .def(py::init<>())
      .def_readwrite("name", &FlowConfig::name_)
      .def_readwrite("id", &FlowConfig::id_)
      .def_readwrite("action", &FlowConfig::action_)
      .def_readwrite("match", &FlowConfig::match_);

  py::class_<CommonConfig>(m, "CommonConfig")
      .def(py::init<>())
      .def_readwrite("version", &CommonConfig::version)
      .def_readwrite("master_core", &CommonConfig::master_core_)
      .def_readwrite("direction", &CommonConfig::dir)
      .def_readwrite("stream_type", &CommonConfig::stream_type)
      .def_readwrite("protocol", &CommonConfig::protocol)
      .def_readwrite("manager_type", &CommonConfig::manager_type)
      .def_readwrite("loopback", &CommonConfig::loopback_);

  py::class_<SocketConfig>(m, "SocketConfig")
      .def(py::init<>())
      .def_readwrite("mode", &SocketConfig::mode_)
      .def_readwrite("local_ip", &SocketConfig::local_ip_)
      .def_readwrite("remote_ip", &SocketConfig::remote_ip_)
      .def_readwrite("local_port", &SocketConfig::local_port_)
      .def_readwrite("remote_port", &SocketConfig::remote_port_)
      .def_readwrite("max_payload_size", &SocketConfig::max_payload_size_)
      .def_readwrite("max_burst_interval_ms",
                     &SocketConfig::max_burst_interval_ms_)
      .def_readwrite("min_ipg_ns", &SocketConfig::min_ipg_ns_)
      .def_readwrite("retry_connect_s", &SocketConfig::retry_connect_s_);

  py::class_<RoCEConfig>(m, "RoCEConfig")
      .def(py::init<>())
      .def_readwrite("transport_mode", &RoCEConfig::transport_mode_);

  py::class_<FlexItemConfig>(m, "FlexItemConfig")
      .def(py::init<>())
      .def_readwrite("name", &FlexItemConfig::name_)
      .def_readwrite("id", &FlexItemConfig::id_)
      .def_readwrite("udp_dst_port", &FlexItemConfig::udp_dst_port_)
      .def_readwrite("offset", &FlexItemConfig::offset_);

  py::class_<ReorderBitFieldConfig>(m, "ReorderBitFieldConfig")
      .def(py::init<>())
      .def_readwrite("bit_offset", &ReorderBitFieldConfig::bit_offset_)
      .def_readwrite("bit_width", &ReorderBitFieldConfig::bit_width_);

  py::class_<ReorderSeqBatchNumberConfig>(m, "ReorderSeqBatchNumberConfig")
      .def(py::init<>())
      .def_readwrite("sequence_number",
                     &ReorderSeqBatchNumberConfig::sequence_number_)
      .def_readwrite("batch_number",
                     &ReorderSeqBatchNumberConfig::batch_number_)
      .def_readwrite("packets_per_batch",
                     &ReorderSeqBatchNumberConfig::packets_per_batch_);

  py::class_<ReorderSeqPacketsPerBatchConfig>(m,
                                              "ReorderSeqPacketsPerBatchConfig")
      .def(py::init<>())
      .def_readwrite("sequence_number",
                     &ReorderSeqPacketsPerBatchConfig::sequence_number_)
      .def_readwrite("packets_per_batch",
                     &ReorderSeqPacketsPerBatchConfig::packets_per_batch_);

  py::class_<ReorderDataTypesConfig>(m, "ReorderDataTypesConfig")
      .def(py::init<>())
      .def_readwrite("enabled", &ReorderDataTypesConfig::enabled_)
      .def_readwrite("input_type", &ReorderDataTypesConfig::input_type_)
      .def_readwrite("output_type", &ReorderDataTypesConfig::output_type_)
      .def_readwrite("input_endianness",
                     &ReorderDataTypesConfig::input_endianness_);

  py::class_<ReorderConfig>(m, "ReorderConfig")
      .def(py::init<>())
      .def_readwrite("name", &ReorderConfig::name_)
      .def_readwrite("reorder_type", &ReorderConfig::reorder_type_)
      .def_readwrite("memory_region", &ReorderConfig::memory_region_)
      .def_readwrite("payload_byte_offset",
                     &ReorderConfig::payload_byte_offset_)
      .def_readwrite("flow_ids", &ReorderConfig::flow_ids_)
      .def_readwrite("method", &ReorderConfig::method_)
      .def_readwrite("seq_batch_number", &ReorderConfig::seq_batch_number_)
      .def_readwrite("seq_packets_per_batch",
                     &ReorderConfig::seq_packets_per_batch_)
      .def_readwrite("data_types", &ReorderConfig::data_types_);

  py::class_<RxConfig>(m, "RxConfig")
      .def(py::init<>())
      .def_readwrite("flow_isolation", &RxConfig::flow_isolation_)
      .def_readwrite("hardware_timestamps", &RxConfig::hardware_timestamps_)
      .def_readwrite("queues", &RxConfig::queues_)
      .def_readwrite("flows", &RxConfig::flows_)
      .def_readwrite("flex_items", &RxConfig::flex_items_)
      .def_readwrite("reorder_configs", &RxConfig::reorder_configs_);

  py::class_<TxConfig>(m, "TxConfig")
      .def(py::init<>())
      .def_readwrite("accurate_send", &TxConfig::accurate_send_)
      .def_readwrite("queues", &TxConfig::queues_)
      .def_readwrite("flows", &TxConfig::flows_);

  py::class_<InterfaceConfig>(m, "InterfaceConfig")
      .def(py::init<>())
      .def_readwrite("name", &InterfaceConfig::name_)
      .def_readwrite("address", &InterfaceConfig::address_)
      .def_readwrite("port_id", &InterfaceConfig::port_id_)
      .def_readwrite("socket", &InterfaceConfig::socket_)
      .def_readwrite("roce", &InterfaceConfig::roce_)
      .def_readwrite("rdma", &InterfaceConfig::rdma_)
      .def_readwrite("rx", &InterfaceConfig::rx_)
      .def_readwrite("tx", &InterfaceConfig::tx_);

  py::class_<NetworkConfig>(m, "NetworkConfig")
      .def(py::init<>())
      .def_readwrite("common", &NetworkConfig::common_)
      .def_readwrite("memory_regions", &NetworkConfig::mrs_)
      .def_readwrite("interfaces", &NetworkConfig::ifs_)
      .def_readwrite("debug", &NetworkConfig::debug_)
      .def_readwrite("tx_meta_buffers", &NetworkConfig::tx_meta_buffers_)
      .def_readwrite("rx_meta_buffers", &NetworkConfig::rx_meta_buffers_)
      .def_readwrite("log_level", &NetworkConfig::log_level_);
}

} // namespace

PYBIND11_MODULE(_daqiri, m) {
  m.doc() = "Python bindings for the DAQIRI packet I/O library";

  m.attr("ADV_NETWORK_HEADER_SIZE_BYTES") = ADV_NETWORK_HEADER_SIZE_BYTES;
  m.attr("MAX_NUM_RX_QUEUES") = MAX_NUM_RX_QUEUES;
  m.attr("MAX_NUM_TX_QUEUES") = MAX_NUM_TX_QUEUES;
  m.attr("MAX_INTERFACES") = MAX_INTERFACES;
  m.attr("MAX_NUM_SEGS") = MAX_NUM_SEGS;
  m.attr("DAQIRI_BURST_FLAG_REORDERED") = DAQIRI_BURST_FLAG_REORDERED;
  m.attr("DAQIRI_BURST_FLAG_REORDER_TIMEOUT") =
      DAQIRI_BURST_FLAG_REORDER_TIMEOUT;
  m.attr("MEM_ACCESS_LOCAL") =
      py::int_(static_cast<uint32_t>(MEM_ACCESS_LOCAL));
  m.attr("MEM_ACCESS_RDMA_WRITE") =
      py::int_(static_cast<uint32_t>(MEM_ACCESS_RDMA_WRITE));
  m.attr("MEM_ACCESS_RDMA_READ") =
      py::int_(static_cast<uint32_t>(MEM_ACCESS_RDMA_READ));
  m.attr("IPPROTO_UDP") = kIpProtoUdp;

  bind_enums(m);
  bind_config_types(m);

  m.def("daqiri_init", &daqiri_init_from_python, "config"_a,
        "Initialize DAQIRI from a YAML path, YAML string, dict, or config-like "
        "object");
  m.def("daqiri_init_from_yaml_string", &daqiri_init_from_yaml_string,
        "yaml_string"_a, py::call_guard<py::gil_scoped_release>());
  m.def("daqiri_init_from_yaml_file", &daqiri_init_from_yaml_file,
        "yaml_path"_a, py::call_guard<py::gil_scoped_release>());
  m.def(
      "parse_network_config",
      [](const std::string &yaml_string_or_path) {
        NetworkConfig config;
        const Status status = parse_network_config(yaml_string_or_path, config);
        return py::make_tuple(status, config);
      },
      "yaml_string_or_path"_a);
  m.def(
      "parse_network_config_from_yaml_string",
      [](const std::string &yaml_string) {
        NetworkConfig config;
        const Status status =
            parse_network_config_from_yaml_string(yaml_string, config);
        return py::make_tuple(status, config);
      },
      "yaml_string"_a);
  m.def(
      "parse_network_config_from_yaml_file",
      [](const std::string &yaml_path) {
        NetworkConfig config;
        const Status status =
            parse_network_config_from_yaml_file(yaml_path, config);
        return py::make_tuple(status, config);
      },
      "yaml_path"_a);

  m.def("get_manager_type", static_cast<ManagerType (*)()>(&get_manager_type),
        "Get the current manager type");
  m.def("manager_type_from_string", &manager_type_from_string, "str"_a,
        "Convert a string to a manager type");
  m.def("manager_type_to_string", &manager_type_to_string, "type"_a,
        "Convert a manager type to a string");
  m.def("stream_type_from_string", &stream_type_from_string, "str"_a);
  m.def("stream_type_to_string", &stream_type_to_string, "type"_a);
  m.def("socket_protocol_from_string", &socket_protocol_from_string, "str"_a);
  m.def("socket_protocol_to_string", &socket_protocol_to_string, "protocol"_a);
  m.def("reorder_data_type_from_string", &reorder_data_type_from_string,
        "str"_a);
  m.def("reorder_data_type_to_string", &reorder_data_type_to_string, "type"_a);
  m.def("reorder_endianness_from_string", &reorder_endianness_from_string,
        "str"_a);
  m.def("reorder_endianness_to_string", &reorder_endianness_to_string,
        "endianness"_a);
  m.def("log_level_to_string", &LogLevel::to_string, "level"_a);
  m.def("log_level_from_string", &LogLevel::from_string, "str"_a);

  m.def("create_burst_params", &create_burst_params,
        py::return_value_policy::reference);
  m.def("create_tx_burst_params", &create_tx_burst_params,
        py::return_value_policy::reference);

  m.def("set_header", &set_header, "burst"_a, "port"_a, "q"_a, "num"_a,
        "segs"_a);
  m.def("set_num_packets", &set_num_packets, "burst"_a, "num"_a);
  m.def("get_num_packets", &get_num_packets, "burst"_a);
  m.def("get_q_id", &get_q_id, "burst"_a);

  m.def(
      "get_segment_packet_ptr",
      [](BurstParams *burst, int seg, int idx) {
        return reinterpret_cast<uintptr_t>(
            get_segment_packet_ptr(burst, seg, idx));
      },
      "burst"_a, "seg"_a, "idx"_a,
      "Return the packet segment data pointer as an integer address");
  m.def(
      "get_packet_ptr",
      [](BurstParams *burst, int idx) {
        return reinterpret_cast<uintptr_t>(get_packet_ptr(burst, idx));
      },
      "burst"_a, "idx"_a,
      "Return the packet data pointer as an integer address");
  m.def("get_segment_packet_length", &get_segment_packet_length, "burst"_a,
        "seg"_a, "idx"_a);
  m.def("get_packet_length", &get_packet_length, "burst"_a, "idx"_a);
  m.def("get_packet_flow_id", &get_packet_flow_id, "burst"_a, "idx"_a);
  m.def(
      "get_packet_rx_timestamp",
      [](BurstParams *burst, int idx) {
        uint64_t timestamp_ns = 0;
        const Status status =
            get_packet_rx_timestamp(burst, idx, &timestamp_ns);
        return py::make_tuple(status, timestamp_ns);
      },
      "burst"_a, "idx"_a,
      "Return (Status, RX timestamp nanoseconds) for a packet");
  m.def("get_burst_tot_byte", &get_burst_tot_byte, "burst"_a);

  m.def("copy_buffer_to_segment_packet", &copy_buffer_to_segment_packet_impl,
        "burst"_a, "seg"_a, "idx"_a, "data"_a, "nbytes"_a = py::none(),
        "src_offset"_a = 0, "dst_offset"_a = 0,
        "Copy a Python buffer into a CPU or CUDA packet segment");
  m.def(
      "copy_buffer_to_packet",
      [](BurstParams *burst, int idx, py::buffer data, py::object nbytes_obj,
         size_t src_offset, size_t dst_offset) {
        return copy_buffer_to_segment_packet_impl(
            burst, 0, idx, data, nbytes_obj, src_offset, dst_offset);
      },
      "burst"_a, "idx"_a, "data"_a, "nbytes"_a = py::none(), "src_offset"_a = 0,
      "dst_offset"_a = 0,
      "Copy a Python buffer into segment 0 of a CPU or CUDA packet");
  m.def("get_segment_packet_bytes", &segment_packet_bytes_impl, "burst"_a,
        "seg"_a, "idx"_a, "nbytes"_a = py::none(), "src_offset"_a = 0,
        "Copy a CPU or CUDA packet segment into Python bytes. Returns (Status, "
        "bytes).");
  m.def(
      "get_packet_bytes",
      [](BurstParams *burst, int idx, py::object nbytes_obj,
         size_t src_offset) {
        return segment_packet_bytes_impl(burst, 0, idx, nbytes_obj, src_offset);
      },
      "burst"_a, "idx"_a, "nbytes"_a = py::none(), "src_offset"_a = 0,
      "Copy segment 0 into Python bytes. Returns (Status, bytes).");

  m.def("is_tx_burst_available", &is_tx_burst_available, "burst"_a,
        py::call_guard<py::gil_scoped_release>());
  m.def("get_tx_packet_burst", &get_tx_packet_burst, "burst"_a,
        py::call_guard<py::gil_scoped_release>());
  m.def("send_tx_burst", &send_tx_burst, "burst"_a,
        py::call_guard<py::gil_scoped_release>());
  m.def("set_packet_lengths", &set_packet_lengths_from_vector, "burst"_a,
        "idx"_a, "lens"_a);
  m.def("set_all_packet_lengths", &set_all_packet_lengths_from_vector,
        "burst"_a, "lens"_a);
  m.def("set_packet_tx_time", &set_packet_tx_time, "burst"_a, "idx"_a,
        "time"_a);

  m.def(
      "set_eth_header",
      [](BurstParams *burst, int idx, const std::string &dst_addr) {
        char mac_bytes[6] = {};
        format_eth_addr(mac_bytes, dst_addr);
        return set_eth_header(burst, idx, mac_bytes);
      },
      "burst"_a, "idx"_a, "dst_addr"_a);
  m.def("set_ipv4_header", &set_ipv4_header, "burst"_a, "idx"_a, "ip_len"_a,
        "proto"_a, "src_host"_a, "dst_host"_a);
  m.def("set_udp_header", &set_udp_header, "burst"_a, "idx"_a, "udp_len"_a,
        "src_port"_a, "dst_port"_a);
  m.def(
      "set_udp_payload",
      [](BurstParams *burst, int idx, py::buffer data) {
        py::buffer_info info = data.request();
        const size_t nbytes = buffer_nbytes(info);
        if (nbytes > static_cast<size_t>(std::numeric_limits<int>::max())) {
          return Status::INVALID_PARAMETER;
        }
        return set_udp_payload(burst, idx, info.ptr, static_cast<int>(nbytes));
      },
      "burst"_a, "idx"_a, "data"_a);

  m.def(
      "get_rx_burst",
      [](int port, int q) {
        BurstParams *burst = nullptr;
        Status status = Status::SUCCESS;
        {
          py::gil_scoped_release release;
          status = get_rx_burst(&burst, port, q);
        }
        return py::make_tuple(
            status, py::cast(burst, py::return_value_policy::reference));
      },
      "port"_a, "q"_a);
  m.def(
      "get_rx_burst",
      [](int port) {
        BurstParams *burst = nullptr;
        Status status = Status::SUCCESS;
        {
          py::gil_scoped_release release;
          status = get_rx_burst(&burst, port);
        }
        return py::make_tuple(
            status, py::cast(burst, py::return_value_policy::reference));
      },
      "port"_a);
  m.def("get_rx_burst", []() {
    BurstParams *burst = nullptr;
    Status status = Status::SUCCESS;
    {
      py::gil_scoped_release release;
      status = get_rx_burst(&burst);
    }
    return py::make_tuple(status,
                          py::cast(burst, py::return_value_policy::reference));
  });
  m.def(
      "get_rx_burst_for_connection",
      [](uintptr_t conn_id, bool server) {
        BurstParams *burst = nullptr;
        Status status = Status::SUCCESS;
        {
          py::gil_scoped_release release;
          status = get_rx_burst(&burst, conn_id, server);
        }
        return py::make_tuple(
            status, py::cast(burst, py::return_value_policy::reference));
      },
      "conn_id"_a, "server"_a);

  m.def("free_packet", &free_packet, "burst"_a, "idx"_a);
  m.def("free_packet_segment", &free_packet_segment, "burst"_a, "seg"_a,
        "idx"_a);
  m.def("free_all_segment_packets", &free_all_segment_packets, "burst"_a,
        "seg"_a);
  m.def("free_all_packets_and_burst_rx", &free_all_packets_and_burst_rx,
        "burst"_a);
  m.def("free_all_packets_and_burst_tx", &free_all_packets_and_burst_tx,
        "burst"_a);
  m.def("free_segment_packets_and_burst", &free_segment_packets_and_burst,
        "burst"_a, "seg"_a);
  m.def("free_tx_burst", &free_tx_burst, "burst"_a);
  m.def("free_rx_burst", &free_rx_burst, "burst"_a);
  m.def("free_tx_metadata", &free_tx_metadata, "burst"_a);
  m.def("free_rx_metadata", &free_rx_metadata, "burst"_a);

  m.def(
      "get_mac_addr",
      [](int port) {
        char mac[6] = {};
        const Status status = get_mac_addr(port, mac);
        if (status != Status::SUCCESS) {
          return py::make_tuple(status, std::string());
        }
        char formatted[18] = {};
        std::snprintf(formatted, sizeof(formatted),
                      "%02x:%02x:%02x:%02x:%02x:%02x",
                      static_cast<unsigned char>(mac[0]),
                      static_cast<unsigned char>(mac[1]),
                      static_cast<unsigned char>(mac[2]),
                      static_cast<unsigned char>(mac[3]),
                      static_cast<unsigned char>(mac[4]),
                      static_cast<unsigned char>(mac[5]));
        return py::make_tuple(status, std::string(formatted));
      },
      "port"_a);
  m.def(
      "format_eth_addr",
      [](const std::string &addr) {
        char mac[6] = {};
        format_eth_addr(mac, addr);
        return py::bytes(mac, sizeof(mac));
      },
      "addr"_a);
  m.def("get_port_id", &get_port_id, "key"_a);
  m.def("drop_all_traffic", &drop_all_traffic, "port"_a);
  m.def("allow_all_traffic", &allow_all_traffic, "port"_a);
  m.def("get_num_rx_queues", &get_num_rx_queues, "port_id"_a);
  m.def("flush_port_queue", &flush_port_queue, "port"_a, "queue"_a);

  m.def(
      "set_reorder_cuda_stream",
      [](const std::string &interface_name, const std::string &reorder_name,
         uintptr_t stream) {
        return set_reorder_cuda_stream(interface_name, reorder_name,
                                       reinterpret_cast<cudaStream_t>(stream));
      },
      "interface_name"_a, "reorder_name"_a, "stream"_a = 0);
  m.def(
      "get_reorder_burst_info",
      [](BurstParams *burst) {
        ReorderBurstInfo info{};
        const Status status = get_reorder_burst_info(burst, &info);
        return py::make_tuple(status, info);
      },
      "burst"_a);
  m.def(
      "synchronize_burst_event",
      [](BurstParams *burst) {
        if (burst == nullptr) {
          return Status::NULL_PTR;
        }
        if (burst->event == nullptr) {
          return Status::SUCCESS;
        }
        const cudaError_t err = cudaEventSynchronize(burst->event);
        if (err != cudaSuccess) {
          DAQIRI_LOG_ERROR("cudaEventSynchronize failed: {}",
                           cudaGetErrorString(err));
          return Status::GENERIC_FAILURE;
        }
        return Status::SUCCESS;
      },
      "burst"_a, py::call_guard<py::gil_scoped_release>());

  m.def(
      "socket_connect_to_server",
      [](const std::string &server_addr, uint16_t server_port) {
        uintptr_t conn_id = 0;
        const Status status =
            socket_connect_to_server(server_addr, server_port, &conn_id);
        return py::make_tuple(status, conn_id);
      },
      "server_addr"_a, "server_port"_a);
  m.def(
      "socket_connect_to_server",
      [](const std::string &server_addr, uint16_t server_port,
         const std::string &src_addr) {
        uintptr_t conn_id = 0;
        const Status status = socket_connect_to_server(server_addr, server_port,
                                                       src_addr, &conn_id);
        return py::make_tuple(status, conn_id);
      },
      "server_addr"_a, "server_port"_a, "src_addr"_a);
  m.def(
      "socket_get_port_queue",
      [](uintptr_t conn_id) {
        uint16_t port = 0;
        uint16_t queue = 0;
        const Status status = socket_get_port_queue(conn_id, &port, &queue);
        return py::make_tuple(status, port, queue);
      },
      "conn_id"_a);
  m.def(
      "socket_get_server_conn_id",
      [](const std::string &server_addr, uint16_t server_port) {
        uintptr_t conn_id = 0;
        const Status status =
            socket_get_server_conn_id(server_addr, server_port, &conn_id);
        return py::make_tuple(status, conn_id);
      },
      "server_addr"_a, "server_port"_a);

  m.def(
      "rdma_connect_to_server",
      [](const std::string &server_addr, uint16_t server_port) {
        uintptr_t conn_id = 0;
        const Status status =
            rdma_connect_to_server(server_addr, server_port, &conn_id);
        return py::make_tuple(status, conn_id);
      },
      "server_addr"_a, "server_port"_a);
  m.def(
      "rdma_connect_to_server",
      [](const std::string &server_addr, uint16_t server_port,
         const std::string &src_addr) {
        uintptr_t conn_id = 0;
        const Status status = rdma_connect_to_server(server_addr, server_port,
                                                     src_addr, &conn_id);
        return py::make_tuple(status, conn_id);
      },
      "server_addr"_a, "server_port"_a, "src_addr"_a);
  m.def(
      "rdma_get_port_queue",
      [](uintptr_t conn_id) {
        uint16_t port = 0;
        uint16_t queue = 0;
        const Status status = rdma_get_port_queue(conn_id, &port, &queue);
        return py::make_tuple(status, port, queue);
      },
      "conn_id"_a);
  m.def(
      "rdma_get_server_conn_id",
      [](const std::string &server_addr, uint16_t server_port) {
        uintptr_t conn_id = 0;
        const Status status =
            rdma_get_server_conn_id(server_addr, server_port, &conn_id);
        return py::make_tuple(status, conn_id);
      },
      "server_addr"_a, "server_port"_a);
  m.def("rdma_set_header", &rdma_set_header, "burst"_a, "op_code"_a,
        "conn_id"_a, "is_server"_a, "num_pkts"_a, "wr_id"_a, "local_mr_name"_a);
  m.def("rdma_get_opcode", &rdma_get_opcode, "burst"_a);

  m.def("shutdown", &shutdown, "Shut down the active DAQIRI manager");
  m.def("print_stats", &print_stats, "Print DAQIRI manager statistics");
}

} // namespace daqiri
