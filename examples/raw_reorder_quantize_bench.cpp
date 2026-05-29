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

#include <arpa/inet.h>
#include <cuda_runtime.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "raw_bench_common.h"
#include <daqiri/daqiri.h>

namespace {

volatile std::sig_atomic_t g_stop_requested = 0;

void signal_handler(int signum) {
  if (signum == SIGINT) {
    g_stop_requested = 1;
  }
}

enum class SampleType {
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

enum class Endianness {
  HOST,
  NETWORK,
  INVALID,
};

struct BitField {
  uint32_t bit_offset = 0;
  uint8_t bit_width = 0;
};

struct ReorderPlanConfig {
  std::string interface_name;
  std::string reorder_name;
  BitField sequence_number;
  BitField batch_number;
  uint32_t packets_per_batch = 0;
  uint32_t payload_byte_offset = 0;
  SampleType input_type = SampleType::INT32;
  SampleType output_type = SampleType::INT32;
  Endianness input_endianness = Endianness::HOST;
  bool data_types_defined = false;
};

struct BenchConfig {
  std::string interface_name = "loopback_ports";
  uint32_t queue_id = 0;
  uint32_t batch_size = 256;
  uint32_t payload_size = 256;
  uint32_t header_size = 64;
  std::string eth_src_addr = "00:00:00:00:00:00";
  std::string eth_dst_addr = "00:00:00:00:00:00";
  std::string ip_src_addr = "1.2.3.4";
  std::string ip_dst_addr = "5.6.7.8";
  uint16_t udp_src_port = 4096;
  uint16_t udp_dst_port = 4096;
  SampleType input_type = SampleType::INT4;
  SampleType output_type = SampleType::FP32;
  Endianness input_endianness = Endianness::HOST;
  uint32_t verify_batches = 3;
  uint32_t verify_chunks = 6;
  uint32_t chunk_elements = 16;
};

struct SharedStats {
  std::atomic<uint32_t> verified_batches{0};
  std::atomic<uint32_t> verified_chunks{0};
  std::atomic<uint32_t> failures{0};
};

SampleType sample_type_from_string(const std::string &value) {
  if (value == "int4") {
    return SampleType::INT4;
  }
  if (value == "int8") {
    return SampleType::INT8;
  }
  if (value == "int16") {
    return SampleType::INT16;
  }
  if (value == "int32") {
    return SampleType::INT32;
  }
  if (value == "fp16") {
    return SampleType::FP16;
  }
  if (value == "bf16") {
    return SampleType::BF16;
  }
  if (value == "fp32") {
    return SampleType::FP32;
  }
  if (value == "fp64") {
    return SampleType::FP64;
  }
  return SampleType::INVALID;
}

std::string sample_type_to_string(SampleType type) {
  switch (type) {
  case SampleType::INT4:
    return "int4";
  case SampleType::INT8:
    return "int8";
  case SampleType::INT16:
    return "int16";
  case SampleType::INT32:
    return "int32";
  case SampleType::FP16:
    return "fp16";
  case SampleType::BF16:
    return "bf16";
  case SampleType::FP32:
    return "fp32";
  case SampleType::FP64:
    return "fp64";
  default:
    return "invalid";
  }
}

const char *endianness_to_string(Endianness endianness) {
  switch (endianness) {
  case Endianness::HOST:
    return "host";
  case Endianness::NETWORK:
    return "network";
  default:
    return "invalid";
  }
}

Endianness endianness_from_string(const std::string &value) {
  if (value == "host") {
    return Endianness::HOST;
  }
  if (value == "network") {
    return Endianness::NETWORK;
  }
  return Endianness::INVALID;
}

uint32_t input_type_bits(SampleType type) {
  switch (type) {
  case SampleType::INT4:
    return 4;
  case SampleType::INT8:
    return 8;
  case SampleType::INT16:
    return 16;
  case SampleType::INT32:
    return 32;
  default:
    return 0;
  }
}

uint32_t output_type_bytes(SampleType type) {
  switch (type) {
  case SampleType::FP16:
  case SampleType::BF16:
    return 2;
  case SampleType::FP32:
  case SampleType::INT32:
    return 4;
  case SampleType::FP64:
    return 8;
  default:
    return 0;
  }
}

uint32_t element_count_for_payload(uint32_t payload_size,
                                   SampleType input_type) {
  const uint32_t bits = input_type_bits(input_type);
  if (bits == 0) {
    return 0;
  }
  const uint64_t payload_bits = static_cast<uint64_t>(payload_size) * 8ULL;
  if ((payload_bits % bits) != 0ULL) {
    return 0;
  }
  return static_cast<uint32_t>(payload_bits / bits);
}

uint32_t bit_field_end_byte(const BitField &field) {
  return (field.bit_offset + field.bit_width + 7U) / 8U;
}

uint32_t bit_field_value_mask(uint8_t bit_width) {
  return bit_width >= 32U ? std::numeric_limits<uint32_t>::max()
                          : ((1U << bit_width) - 1U);
}

void set_bits_be(uint8_t *data, uint32_t bit_offset, uint8_t bit_width,
                 uint32_t value) {
  for (uint8_t i = 0; i < bit_width; ++i) {
    const uint32_t src_shift = static_cast<uint32_t>(bit_width - 1U - i);
    const uint8_t bit = static_cast<uint8_t>((value >> src_shift) & 0x1U);
    const uint32_t bit_idx = bit_offset + i;
    const uint32_t byte_idx = bit_idx / 8U;
    const uint8_t bit_pos = static_cast<uint8_t>(7U - (bit_idx % 8U));
    const uint8_t mask = static_cast<uint8_t>(1U << bit_pos);
    data[byte_idx] =
        static_cast<uint8_t>((data[byte_idx] & ~mask) | (bit << bit_pos));
  }
}

int32_t expected_value(uint32_t slot_idx, uint32_t element_idx) {
  const uint32_t raw =
      static_cast<uint32_t>((slot_idx * 3ULL + element_idx) % 15ULL);
  return static_cast<int32_t>(raw) - 7;
}

void write_int16(uint8_t *dst, int32_t value, Endianness endianness) {
  const auto raw = static_cast<uint16_t>(static_cast<int16_t>(value));
  if (endianness == Endianness::NETWORK) {
    dst[0] = static_cast<uint8_t>(raw >> 8U);
    dst[1] = static_cast<uint8_t>(raw & 0xFFU);
  } else {
    dst[0] = static_cast<uint8_t>(raw & 0xFFU);
    dst[1] = static_cast<uint8_t>(raw >> 8U);
  }
}

void write_int32(uint8_t *dst, int32_t value, Endianness endianness) {
  const auto raw = static_cast<uint32_t>(value);
  if (endianness == Endianness::NETWORK) {
    dst[0] = static_cast<uint8_t>((raw >> 24U) & 0xFFU);
    dst[1] = static_cast<uint8_t>((raw >> 16U) & 0xFFU);
    dst[2] = static_cast<uint8_t>((raw >> 8U) & 0xFFU);
    dst[3] = static_cast<uint8_t>(raw & 0xFFU);
  } else {
    dst[0] = static_cast<uint8_t>(raw & 0xFFU);
    dst[1] = static_cast<uint8_t>((raw >> 8U) & 0xFFU);
    dst[2] = static_cast<uint8_t>((raw >> 16U) & 0xFFU);
    dst[3] = static_cast<uint8_t>((raw >> 24U) & 0xFFU);
  }
}

void fill_payload(uint8_t *payload, uint32_t payload_size,
                  SampleType input_type, Endianness endianness,
                  uint32_t slot_idx) {
  const uint32_t element_count =
      element_count_for_payload(payload_size, input_type);
  std::memset(payload, 0, payload_size);

  switch (input_type) {
  case SampleType::INT4:
    for (uint32_t element_idx = 0; element_idx < element_count;
         element_idx += 2U) {
      const uint8_t high =
          static_cast<uint8_t>(expected_value(slot_idx, element_idx)) & 0x0FU;
      const uint8_t low =
          static_cast<uint8_t>(expected_value(slot_idx, element_idx + 1U)) &
          0x0FU;
      payload[element_idx / 2U] = static_cast<uint8_t>((high << 4U) | low);
    }
    break;
  case SampleType::INT8:
    for (uint32_t element_idx = 0; element_idx < element_count; ++element_idx) {
      payload[element_idx] = static_cast<uint8_t>(
          static_cast<int8_t>(expected_value(slot_idx, element_idx)));
    }
    break;
  case SampleType::INT16:
    for (uint32_t element_idx = 0; element_idx < element_count; ++element_idx) {
      write_int16(payload + (element_idx * 2U),
                  expected_value(slot_idx, element_idx), endianness);
    }
    break;
  case SampleType::INT32:
    for (uint32_t element_idx = 0; element_idx < element_count; ++element_idx) {
      write_int32(payload + (element_idx * 4U),
                  expected_value(slot_idx, element_idx), endianness);
    }
    break;
  default:
    break;
  }
}

float half_to_float(uint16_t half) {
  const uint32_t sign = static_cast<uint32_t>(half & 0x8000U) << 16U;
  uint32_t exp = (half >> 10U) & 0x1FU;
  uint32_t mant = half & 0x03FFU;
  uint32_t bits = 0;

  if (exp == 0) {
    if (mant == 0) {
      bits = sign;
    } else {
      exp = 1;
      while ((mant & 0x0400U) == 0U) {
        mant <<= 1U;
        --exp;
      }
      mant &= 0x03FFU;
      bits = sign | ((exp + 127U - 15U) << 23U) | (mant << 13U);
    }
  } else if (exp == 0x1FU) {
    bits = sign | 0x7F800000U | (mant << 13U);
  } else {
    bits = sign | ((exp + 127U - 15U) << 23U) | (mant << 13U);
  }

  float value = 0.0F;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

float bf16_to_float(uint16_t bf16) {
  const uint32_t bits = static_cast<uint32_t>(bf16) << 16U;
  float value = 0.0F;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

double read_output_value(const uint8_t *data, SampleType output_type,
                         uint32_t element_idx) {
  switch (output_type) {
  case SampleType::FP16: {
    uint16_t bits = 0;
    std::memcpy(&bits, data + (element_idx * 2U), sizeof(bits));
    return static_cast<double>(half_to_float(bits));
  }
  case SampleType::BF16: {
    uint16_t bits = 0;
    std::memcpy(&bits, data + (element_idx * 2U), sizeof(bits));
    return static_cast<double>(bf16_to_float(bits));
  }
  case SampleType::FP32: {
    float value = 0.0F;
    std::memcpy(&value, data + (element_idx * 4U), sizeof(value));
    return static_cast<double>(value);
  }
  case SampleType::FP64: {
    double value = 0.0;
    std::memcpy(&value, data + (element_idx * 8U), sizeof(value));
    return value;
  }
  case SampleType::INT32: {
    int32_t value = 0;
    std::memcpy(&value, data + (element_idx * 4U), sizeof(value));
    return static_cast<double>(value);
  }
  default:
    return std::numeric_limits<double>::quiet_NaN();
  }
}

bool nearly_equal(double actual, double expected) {
  return std::fabs(actual - expected) <= 0.001;
}

bool verify_reorder_output(const uint8_t *output,
                           const daqiri::ReorderBurstInfo &info,
                           const BenchConfig &cfg) {
  const uint32_t element_count =
      element_count_for_payload(cfg.payload_size, cfg.input_type);
  const uint32_t output_bytes = output_type_bytes(cfg.output_type);
  if (element_count == 0 || output_bytes == 0 ||
      info.payload_len != element_count * output_bytes) {
    std::cerr << "Unexpected output payload length. got=" << info.payload_len
              << " expected=" << (element_count * output_bytes) << "\n";
    return false;
  }

  const uint32_t chunk_elements = std::min(cfg.chunk_elements, element_count);
  const uint32_t span = element_count - chunk_elements + 1U;

  for (uint32_t chunk_idx = 0; chunk_idx < cfg.verify_chunks; ++chunk_idx) {
    const uint32_t slot_idx = static_cast<uint32_t>(
        (static_cast<uint64_t>(chunk_idx) * 37ULL) % info.packets_per_batch);
    const uint32_t element_start = static_cast<uint32_t>(
        (static_cast<uint64_t>(chunk_idx) * 53ULL) % span);
    const uint8_t *slot =
        output + (static_cast<size_t>(slot_idx) * info.payload_len);

    for (uint32_t i = 0; i < chunk_elements; ++i) {
      const uint32_t element_idx = element_start + i;
      const double expected =
          static_cast<double>(expected_value(slot_idx, element_idx));
      const double actual =
          read_output_value(slot, cfg.output_type, element_idx);
      if (!nearly_equal(actual, expected)) {
        std::cerr << "Verification mismatch batch_id=" << info.batch_id
                  << " slot=" << slot_idx << " element=" << element_idx
                  << " expected=" << expected << " actual=" << actual << "\n";
        return false;
      }
    }
  }

  return true;
}

BitField parse_bit_field(const YAML::Node &node, const std::string &key) {
  BitField field;
  field.bit_offset = node[key]["bit_offset"].as<uint32_t>();
  field.bit_width = node[key]["bit_width"].as<uint8_t>();
  return field;
}

ReorderPlanConfig parse_reorder_plan(const YAML::Node &root) {
  const auto interfaces = root["daqiri"]["cfg"]["interfaces"];
  for (const auto &intf : interfaces) {
    const auto rx = intf["rx"];
    if (!rx || !rx["reorder_configs"]) {
      continue;
    }
    for (const auto &reorder : rx["reorder_configs"]) {
      if (reorder["reorder_type"].as<std::string>("") != "gpu") {
        continue;
      }
      if (!reorder["method"] || !reorder["method"]["seq_batch_number"]) {
        continue;
      }
      ReorderPlanConfig plan;
      plan.interface_name = intf["name"].as<std::string>();
      plan.reorder_name = reorder["name"].as<std::string>();
      plan.payload_byte_offset = reorder["payload_byte_offset"].as<uint32_t>();
      if (reorder["data_types"]) {
        const auto data_types = reorder["data_types"];
        const auto input_node = data_types["input_type"]
                                    ? data_types["input_type"]
                                    : data_types["input"];
        const auto output_node = data_types["output_type"]
                                     ? data_types["output_type"]
                                     : data_types["output"];
        if (!input_node || !output_node) {
          throw std::runtime_error(
              "Reorder data_types requires input_type and output_type");
        }
        plan.input_type = sample_type_from_string(input_node.as<std::string>());
        plan.output_type =
            sample_type_from_string(output_node.as<std::string>());
        const auto endianness_node = data_types["endianness"]
                                         ? data_types["endianness"]
                                         : data_types["input_endianness"];
        plan.input_endianness =
            endianness_from_string(endianness_node.as<std::string>("host"));
        plan.data_types_defined = true;
      }
      const auto seq_batch = reorder["method"]["seq_batch_number"];
      plan.sequence_number = parse_bit_field(seq_batch, "sequence_number");
      plan.batch_number = parse_bit_field(seq_batch, "batch_number");
      plan.packets_per_batch = (1U << plan.sequence_number.bit_width) /
                               (1U << plan.batch_number.bit_width);
      return plan;
    }
  }
  throw std::runtime_error("No GPU seq_batch_number reorder config found");
}

BenchConfig parse_bench_config(const YAML::Node &root,
                               const ReorderPlanConfig &plan) {
  BenchConfig cfg;
  const auto bench = root["bench_reorder_quantize"];
  const auto bench_tx = root["bench_tx"];
  const auto tx = bench_tx && bench_tx.IsSequence() && bench_tx.size() > 0
                      ? bench_tx[0]
                      : bench_tx;
  cfg.input_type = plan.input_type;
  cfg.output_type = plan.output_type;
  cfg.input_endianness = plan.input_endianness;

  if (tx && tx["interface_name"]) {
    cfg.interface_name =
        tx["interface_name"].as<std::string>(cfg.interface_name);
  } else {
    cfg.interface_name = plan.interface_name;
  }
  if (tx) {
    cfg.queue_id = tx["queue_id"].as<uint32_t>(cfg.queue_id);
    cfg.batch_size = tx["batch_size"].as<uint32_t>(cfg.batch_size);
    cfg.payload_size = tx["payload_size"].as<uint32_t>(cfg.payload_size);
    cfg.header_size = tx["header_size"].as<uint32_t>(cfg.header_size);
    cfg.eth_src_addr = tx["eth_src_addr"].as<std::string>(cfg.eth_src_addr);
    cfg.eth_dst_addr = tx["eth_dst_addr"].as<std::string>(cfg.eth_dst_addr);
    cfg.ip_src_addr = tx["ip_src_addr"].as<std::string>(cfg.ip_src_addr);
    cfg.ip_dst_addr = tx["ip_dst_addr"].as<std::string>(cfg.ip_dst_addr);
    cfg.udp_src_port = tx["udp_src_port"].as<uint16_t>(cfg.udp_src_port);
    cfg.udp_dst_port = tx["udp_dst_port"].as<uint16_t>(cfg.udp_dst_port);
  }

  if (bench) {
    const auto bench_input =
        sample_type_from_string(bench["input_type"].as<std::string>(
            sample_type_to_string(cfg.input_type)));
    const auto bench_output =
        sample_type_from_string(bench["output_type"].as<std::string>(
            sample_type_to_string(cfg.output_type)));
    const auto bench_endianness =
        endianness_from_string(bench["endianness"].as<std::string>(
            endianness_to_string(cfg.input_endianness)));
    if (bench_input != cfg.input_type || bench_output != cfg.output_type ||
        bench_endianness != cfg.input_endianness) {
      throw std::runtime_error(
          "bench_reorder_quantize types must match reorder data_types");
    }
    cfg.verify_batches =
        bench["verify_batches"].as<uint32_t>(cfg.verify_batches);
    cfg.verify_chunks = bench["verify_chunks"].as<uint32_t>(cfg.verify_chunks);
    cfg.chunk_elements =
        bench["chunk_elements"].as<uint32_t>(cfg.chunk_elements);
  }

  if (cfg.input_type == SampleType::INVALID ||
      input_type_bits(cfg.input_type) == 0) {
    throw std::runtime_error("Invalid bench input_type");
  }
  if (cfg.output_type == SampleType::INVALID ||
      output_type_bytes(cfg.output_type) == 0) {
    throw std::runtime_error("Invalid bench output_type");
  }
  if (cfg.input_endianness == Endianness::INVALID) {
    throw std::runtime_error("Invalid bench endianness");
  }
  if (!plan.data_types_defined) {
    throw std::runtime_error(
        "Reorder quantize example requires reorder data_types");
  }
  if (element_count_for_payload(cfg.payload_size, cfg.input_type) == 0) {
    throw std::runtime_error(
        "payload_size is not an integral number of input elements");
  }
  if (cfg.header_size > plan.payload_byte_offset) {
    throw std::runtime_error(
        "bench_tx header_size must not exceed reorder payload_byte_offset");
  }
  const auto packet_size =
      static_cast<size_t>(std::max(cfg.header_size, plan.payload_byte_offset)) +
      cfg.payload_size;
  const uint64_t packet_bits = static_cast<uint64_t>(packet_size) * 8ULL;
  const uint64_t sequence_end_bit =
      static_cast<uint64_t>(plan.sequence_number.bit_offset) +
      plan.sequence_number.bit_width;
  const uint64_t batch_end_bit =
      static_cast<uint64_t>(plan.batch_number.bit_offset) +
      plan.batch_number.bit_width;
  if (plan.sequence_number.bit_width == 0 || plan.batch_number.bit_width == 0 ||
      sequence_end_bit > packet_bits || batch_end_bit > packet_bits) {
    throw std::runtime_error(
        "Reorder sequence or batch field is outside the TX packet");
  }
  if (bit_field_end_byte(plan.sequence_number) > plan.payload_byte_offset ||
      bit_field_end_byte(plan.batch_number) > plan.payload_byte_offset) {
    throw std::runtime_error("Fast quantize example requires sequence and "
                             "batch fields before payload_byte_offset");
  }
  if (cfg.batch_size < plan.packets_per_batch) {
    throw std::runtime_error("TX batch_size must be >= packets_per_batch");
  }
  return cfg;
}

void tx_worker(const BenchConfig &cfg, const ReorderPlanConfig &plan,
               std::atomic<bool> &stop, SharedStats &stats) {
  const int port_id = daqiri::get_port_id(cfg.interface_name);
  if (port_id < 0) {
    std::cerr << "Invalid TX interface_name: " << cfg.interface_name << "\n";
    stats.failures.fetch_add(1);
    stop.store(true);
    return;
  }

  char eth_dst[6] = {0};
  char eth_src[6] = {0};
  daqiri::format_eth_addr(eth_src, cfg.eth_src_addr);
  daqiri::format_eth_addr(eth_dst, cfg.eth_dst_addr);

  uint32_t ip_src = 0;
  uint32_t ip_dst = 0;
  inet_pton(AF_INET, cfg.ip_src_addr.c_str(), &ip_src);
  inet_pton(AF_INET, cfg.ip_dst_addr.c_str(), &ip_dst);
  ip_src = ntohl(ip_src);
  ip_dst = ntohl(ip_dst);

  uint32_t current_batch_id = 0;
  uint32_t packets_in_current_batch = 0;
  uint32_t next_slot_assignment = 0;
  const auto packet_size =
      static_cast<size_t>(std::max(cfg.header_size, plan.payload_byte_offset)) +
      cfg.payload_size;
  const auto wire_payload_size =
      static_cast<uint32_t>(packet_size - cfg.header_size);
  daqiri::bench::PinnedHostBuffer packet_template;
  daqiri::bench::PinnedHostBuffer tx_copy_staging;
  if (!packet_template.resize(packet_size) ||
      !tx_copy_staging.resize(packet_size * cfg.batch_size)) {
    std::cerr << "Failed to allocate pinned TX staging buffers\n";
    stats.failures.fetch_add(1);
    stop.store(true);
    return;
  }
  daqiri::bench::populate_udp_ipv4_headers(
      packet_template.data(), cfg.header_size, wire_payload_size, eth_src,
      eth_dst, ip_src, ip_dst, cfg.udp_src_port, cfg.udp_dst_port);

  const uint32_t patch_start = std::min(plan.sequence_number.bit_offset / 8U,
                                        plan.batch_number.bit_offset / 8U);
  const uint32_t patch_end = std::max(bit_field_end_byte(plan.sequence_number),
                                      bit_field_end_byte(plan.batch_number));
  const size_t metadata_patch_size = patch_end - patch_start;
  const uint32_t sequence_mask =
      bit_field_value_mask(plan.sequence_number.bit_width);
  const uint32_t batch_mask = bit_field_value_mask(plan.batch_number.bit_width);
  std::unordered_map<void *, uint32_t> tx_buffer_slots;
  cudaStream_t tx_copy_stream = nullptr;
  std::vector<void *> batch_copy_dsts;
  std::vector<const void *> batch_copy_srcs;
  std::vector<size_t> batch_copy_sizes;
  if (cudaStreamCreateWithFlags(&tx_copy_stream, cudaStreamNonBlocking) !=
      cudaSuccess) {
    std::cerr << "Failed to create TX copy stream\n";
    stats.failures.fetch_add(1);
    stop.store(true);
    return;
  }
  batch_copy_dsts.reserve(cfg.batch_size);
  batch_copy_srcs.reserve(cfg.batch_size);
  batch_copy_sizes.reserve(cfg.batch_size);

  while (!stop.load()) {
    auto *msg = daqiri::create_tx_burst_params();
    daqiri::set_header(msg, static_cast<uint16_t>(port_id),
                       static_cast<uint16_t>(cfg.queue_id), cfg.batch_size, 1);

    if (!daqiri::is_tx_burst_available(msg)) {
      daqiri::free_tx_metadata(msg);
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      continue;
    }
    if (daqiri::get_tx_packet_burst(msg) != daqiri::Status::SUCCESS) {
      daqiri::free_tx_metadata(msg);
      continue;
    }

    bool failed = false;
    const auto num_pkts = static_cast<int>(daqiri::get_num_packets(msg));
    if (num_pkts > static_cast<int>(cfg.batch_size)) {
      std::cerr << "TX burst packet count exceeds configured batch_size\n";
      failed = true;
    }
    size_t staging_offset = 0;
    batch_copy_dsts.clear();
    batch_copy_srcs.clear();
    batch_copy_sizes.clear();
    for (int i = 0; i < num_pkts; ++i) {
      if (failed) {
        break;
      }
      const uint32_t batch_id = current_batch_id & batch_mask;

      auto *gpu_pkt =
          static_cast<uint8_t *>(daqiri::get_segment_packet_ptr(msg, 0, i));
      if (gpu_pkt == nullptr) {
        failed = true;
        break;
      }

      const auto [slot_it, inserted] =
          tx_buffer_slots.emplace(gpu_pkt, next_slot_assignment);
      if (inserted) {
        next_slot_assignment =
            (next_slot_assignment + 1U) % plan.packets_per_batch;
      }

      const uint32_t sequence =
          ((batch_id * plan.packets_per_batch) + slot_it->second) &
          sequence_mask;
      if (inserted) {
        auto *packet_init = tx_copy_staging.data() + staging_offset;
        staging_offset += packet_size;
        std::memcpy(packet_init, packet_template.data(), packet_size);
        fill_payload(packet_init + plan.payload_byte_offset, cfg.payload_size,
                     cfg.input_type, cfg.input_endianness, slot_it->second);
        set_bits_be(packet_init, plan.sequence_number.bit_offset,
                    plan.sequence_number.bit_width, sequence);
        set_bits_be(packet_init, plan.batch_number.bit_offset,
                    plan.batch_number.bit_width, batch_id);
        batch_copy_dsts.push_back(gpu_pkt);
        batch_copy_srcs.push_back(packet_init);
        batch_copy_sizes.push_back(packet_size);
      } else {
        auto *metadata_patch = tx_copy_staging.data() + staging_offset;
        staging_offset += metadata_patch_size;
        std::memcpy(metadata_patch, packet_template.data() + patch_start,
                    metadata_patch_size);
        set_bits_be(metadata_patch,
                    plan.sequence_number.bit_offset - (patch_start * 8U),
                    plan.sequence_number.bit_width, sequence);
        set_bits_be(metadata_patch,
                    plan.batch_number.bit_offset - (patch_start * 8U),
                    plan.batch_number.bit_width, batch_id);
        batch_copy_dsts.push_back(gpu_pkt + patch_start);
        batch_copy_srcs.push_back(metadata_patch);
        batch_copy_sizes.push_back(metadata_patch_size);
      }
      ++packets_in_current_batch;
      if (packets_in_current_batch == plan.packets_per_batch) {
        packets_in_current_batch = 0;
        ++current_batch_id;
      }
    }

    if (!failed && !batch_copy_dsts.empty()) {
      const auto copy_status = daqiri::bench::memcpy_batch_async(
          batch_copy_dsts, batch_copy_srcs, batch_copy_sizes, tx_copy_stream);
      if (copy_status != cudaSuccess ||
          cudaStreamSynchronize(tx_copy_stream) != cudaSuccess) {
        std::cerr << "Batched TX packet copy failed: "
                  << cudaGetErrorString(copy_status) << "\n";
        failed = true;
      }
    }

    if (!failed &&
        daqiri::set_all_packet_lengths(msg, {static_cast<int>(packet_size)}) !=
            daqiri::Status::SUCCESS) {
      failed = true;
    }

    if (failed) {
      stats.failures.fetch_add(1);
      daqiri::free_all_packets_and_burst_tx(msg);
      stop.store(true);
      continue;
    }
    daqiri::send_tx_burst(msg);
  }
  if (tx_copy_stream != nullptr) {
    cudaStreamDestroy(tx_copy_stream);
  }
}

void rx_worker(const BenchConfig &cfg, const ReorderPlanConfig &plan,
               std::atomic<bool> &stop, SharedStats &stats) {
  const int port_id = daqiri::get_port_id(plan.interface_name);
  if (port_id < 0) {
    std::cerr << "Invalid RX interface_name: " << plan.interface_name << "\n";
    stats.failures.fetch_add(1);
    stop.store(true);
    return;
  }

  daqiri::bench::PinnedHostBuffer output;
  while (!stop.load()) {
    daqiri::BurstParams *burst = nullptr;
    if (daqiri::get_rx_burst(&burst, port_id, 0) != daqiri::Status::SUCCESS ||
        burst == nullptr) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      continue;
    }

    const bool reordered = (burst->hdr.hdr.burst_flags &
                            daqiri::DAQIRI_BURST_FLAG_REORDERED) != 0U;
    if (!reordered) {
      daqiri::free_all_packets_and_burst_rx(burst);
      continue;
    }

    if (cfg.verify_batches == 0) {
      daqiri::free_all_packets_and_burst_rx(burst);
      continue;
    }

    if (burst->event != nullptr) {
      cudaEventSynchronize(burst->event);
    }
    daqiri::ReorderBurstInfo info{};
    const auto info_status = daqiri::get_reorder_burst_info(burst, &info);
    if (info_status != daqiri::Status::SUCCESS) {
      std::cerr << "get_reorder_burst_info failed with status "
                << static_cast<int>(info_status) << "\n";
      stats.failures.fetch_add(1);
      daqiri::free_all_packets_and_burst_rx(burst);
      stop.store(true);
      return;
    }

    const auto aggregate_len = daqiri::get_packet_length(burst, 0);
    if (!output.resize(aggregate_len)) {
      std::cerr << "Failed to allocate pinned verification output buffer\n";
      stats.failures.fetch_add(1);
      daqiri::free_all_packets_and_burst_rx(burst);
      stop.store(true);
      return;
    }
    if (cudaMemcpy(output.data(), daqiri::get_packet_ptr(burst, 0),
                   aggregate_len, cudaMemcpyDeviceToHost) != cudaSuccess) {
      std::cerr << "Failed to copy reordered output to host\n";
      stats.failures.fetch_add(1);
      daqiri::free_all_packets_and_burst_rx(burst);
      stop.store(true);
      return;
    }

    const bool ok = verify_reorder_output(output.data(), info, cfg);
    daqiri::free_all_packets_and_burst_rx(burst);
    if (!ok) {
      stats.failures.fetch_add(1);
      stop.store(true);
      return;
    }

    stats.verified_batches.fetch_add(1);
    stats.verified_chunks.fetch_add(cfg.verify_chunks);
    if (stats.verified_batches.load() >= cfg.verify_batches) {
      stop.store(true);
      return;
    }
  }
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <config.yaml> [--seconds N]\n";
    return 1;
  }

  const int run_seconds = daqiri::bench::parse_run_seconds(argc, argv);

  const auto root = YAML::LoadFile(argv[1]);
  const auto plan = parse_reorder_plan(root);
  const auto cfg = parse_bench_config(root, plan);

  if (daqiri::daqiri_init(argv[1]) != daqiri::Status::SUCCESS) {
    std::cerr << "daqiri_init failed\n";
    return 1;
  }

  cudaStream_t reorder_stream = nullptr;
  if (cudaStreamCreate(&reorder_stream) != cudaSuccess) {
    std::cerr << "Failed to create CUDA stream\n";
    daqiri::shutdown();
    return 1;
  }
  const auto stream_status = daqiri::set_reorder_cuda_stream(
      plan.interface_name, plan.reorder_name, reorder_stream);
  if (stream_status != daqiri::Status::SUCCESS) {
    std::cerr << "set_reorder_cuda_stream failed with status "
              << static_cast<int>(stream_status) << "\n";
    cudaStreamDestroy(reorder_stream);
    daqiri::shutdown();
    return 1;
  }

  std::atomic<bool> stop{false};
  SharedStats stats;
  std::thread rx_thread(rx_worker, cfg, plan, std::ref(stop), std::ref(stats));
  std::thread tx_thread(tx_worker, cfg, plan, std::ref(stop), std::ref(stats));

  std::signal(SIGINT, signal_handler);
  const auto start = std::chrono::steady_clock::now();
  while (!stop.load() && !g_stop_requested) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start);
    if (run_seconds > 0 && elapsed.count() >= run_seconds) {
      stop.store(true);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  stop.store(true);

  if (tx_thread.joinable()) {
    tx_thread.join();
  }
  if (rx_thread.joinable()) {
    rx_thread.join();
  }

  daqiri::print_stats();
  cudaStreamDestroy(reorder_stream);
  daqiri::shutdown();

  std::cout << "Reorder quantize verify: input="
            << sample_type_to_string(cfg.input_type)
            << " output=" << sample_type_to_string(cfg.output_type)
            << " verified_batches=" << stats.verified_batches.load()
            << " verified_chunks=" << stats.verified_chunks.load()
            << " failures=" << stats.failures.load() << "\n";

  return stats.failures.load() == 0 &&
                 stats.verified_batches.load() >= cfg.verify_batches
             ? 0
             : 2;
}
