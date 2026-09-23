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

#include "app_config.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace daqiri::apps::resnet {
namespace {

bool is_power_of_two(uint32_t n) { return n != 0 && (n & (n - 1)) == 0; }

uint64_t pow2_u8(uint8_t width, const std::string& context) {
  if (width >= 64) {
    throw std::runtime_error(context + " bit_width is too large");
  }
  return 1ULL << width;
}

uint32_t checked_u32(uint64_t value, const std::string& context) {
  if (value > std::numeric_limits<uint32_t>::max()) {
    throw std::runtime_error(context + " is out of uint32 range");
  }
  return static_cast<uint32_t>(value);
}

uint32_t data_type_bytes(const std::string& type) {
  if (type == "int8") return 1;
  if (type == "fp16") return 2;
  return 0;
}

uint64_t memory_region_buf_size(const YAML::Node& root, const std::string& name) {
  const auto dcfg = root["daqiri"] ? root["daqiri"]["cfg"] : YAML::Node();
  const auto mrs = dcfg ? dcfg["memory_regions"] : YAML::Node();
  if (!mrs) {
    throw std::runtime_error("config missing daqiri.cfg.memory_regions");
  }
  for (const auto& mr : mrs) {
    if (mr["name"] && mr["name"].as<std::string>() == name) {
      return mr["buf_size"].as<uint64_t>();
    }
  }
  throw std::runtime_error("memory region '" + name + "' not found in daqiri.cfg.memory_regions");
}

YAML::Node find_interface(const YAML::Node& root, const std::string& name) {
  const auto dcfg = root["daqiri"] ? root["daqiri"]["cfg"] : YAML::Node();
  const auto ifaces = dcfg ? dcfg["interfaces"] : YAML::Node();
  if (!ifaces) {
    throw std::runtime_error("config missing daqiri.cfg.interfaces");
  }
  for (const auto& iface : ifaces) {
    if (iface["name"] && iface["name"].as<std::string>() == name) {
      return iface;
    }
  }
  throw std::runtime_error("interface '" + name + "' not found in daqiri.cfg.interfaces");
}

std::string rx_source_mr_for_queue(const YAML::Node& iface, int queue_id) {
  const auto rx = iface["rx"];
  const auto queues = rx ? rx["queues"] : YAML::Node();
  if (!queues || queues.size() == 0) {
    throw std::runtime_error("RX interface has no queues");
  }
  for (const auto& queue : queues) {
    const int id = queue["id"].as<int>(0);
    if (queue_id >= 0 && id != queue_id) continue;
    const auto mrs = queue["memory_regions"];
    if (!mrs || mrs.size() == 0) {
      throw std::runtime_error("RX queue has no memory_regions");
    }
    return mrs[0].as<std::string>();
  }
  throw std::runtime_error("RX queue_id " + std::to_string(queue_id) + " not found");
}

struct NativeReorderInfo {
  YAML::Node node;
  std::string source_mr;
  uint64_t source_buf_size = 0;
  std::string output_mr;
  uint64_t output_buf_size = 0;
  std::string reorder_engine = "sw";
  uint32_t payload_byte_offset = 0;
  uint32_t packet_size = 0;
  uint32_t packets_per_batch = 0;
  uint16_t seq_bit_offset = 0;
  uint8_t seq_bit_width = 0;
  bool has_data_types = false;
  std::string input_type = "int8";
  std::string output_type = "int8";
};

NativeReorderInfo find_native_reorder(const YAML::Node& root,
                                      const std::string& interface_name,
                                      int queue_id,
                                      const std::string& requested_name) {
  const YAML::Node iface = find_interface(root, interface_name);
  const auto rx = iface["rx"];
  if (!rx) {
    throw std::runtime_error("interface '" + interface_name + "' has no rx block");
  }
  const auto reorder_configs = rx["reorder_configs"];
  if (!reorder_configs || reorder_configs.size() == 0) {
    throw std::runtime_error("RX interface '" + interface_name +
                             "' must define native rx.reorder_configs");
  }

  YAML::Node selected;
  if (requested_name.empty()) {
    if (reorder_configs.size() != 1) {
      throw std::runtime_error(
          "packetization.reorder_name is required when multiple reorder_configs exist");
    }
    selected = reorder_configs[0];
  } else {
    for (std::size_t i = 0; i < reorder_configs.size(); ++i) {
      const auto item = reorder_configs[i];
      if (item["name"] && item["name"].as<std::string>() == requested_name) {
        selected = item;
        break;
      }
    }
    if (!selected) {
      throw std::runtime_error("reorder config '" + requested_name +
                               "' not found on RX interface '" + interface_name + "'");
    }
  }

  NativeReorderInfo info;
  info.node = selected;
  info.source_mr = rx_source_mr_for_queue(iface, queue_id);
  info.source_buf_size = memory_region_buf_size(root, info.source_mr);
  info.output_mr = selected["memory_region"].as<std::string>();
  info.output_buf_size = memory_region_buf_size(root, info.output_mr);
  info.reorder_engine = selected["reorder_engine"].as<std::string>("sw");
  info.payload_byte_offset = selected["payload_byte_offset"].as<uint32_t>();
  info.packet_size = selected["packet_size"].as<uint32_t>(0);

  const auto method = selected["method"];
  if (!method || !method.IsMap()) {
    throw std::runtime_error("reorder config '" + selected["name"].as<std::string>() +
                             "' requires method");
  }
  if (method["seq_packets_per_batch"]) {
    const auto spp = method["seq_packets_per_batch"];
    const auto seq = spp["sequence_number"];
    info.seq_bit_offset = seq["bit_offset"].as<uint16_t>();
    info.seq_bit_width = seq["bit_width"].as<uint8_t>();
    info.packets_per_batch = spp["packets_per_batch"].as<uint32_t>();
  } else if (method["seq_batch_number"]) {
    const auto sbn = method["seq_batch_number"];
    const auto seq = sbn["sequence_number"];
    const auto batch = sbn["batch_number"];
    info.seq_bit_offset = seq["bit_offset"].as<uint16_t>();
    info.seq_bit_width = seq["bit_width"].as<uint8_t>();
    const auto total_seq = pow2_u8(info.seq_bit_width, "sequence_number");
    const auto total_batches = pow2_u8(batch["bit_width"].as<uint8_t>(), "batch_number");
    if (total_batches == 0 || total_seq % total_batches != 0) {
      throw std::runtime_error("seq_batch_number does not derive an integral packets_per_batch");
    }
    info.packets_per_batch = checked_u32(total_seq / total_batches, "packets_per_batch");
  } else {
    throw std::runtime_error(
        "reorder config must define seq_packets_per_batch or seq_batch_number");
  }

  const auto data_types = selected["data_types"];
  if (data_types) {
    info.has_data_types = true;
    const auto input = data_types["input_type"] ? data_types["input_type"] : data_types["input"];
    const auto output =
        data_types["output_type"] ? data_types["output_type"] : data_types["output"];
    if (!input || !output) {
      throw std::runtime_error("reorder data_types requires input_type and output_type");
    }
    info.input_type = input.as<std::string>();
    info.output_type = output.as<std::string>();
  }
  return info;
}

void validate_optional_u32(const YAML::Node& node,
                           const char* key,
                           uint32_t actual,
                           const char* source) {
  if (!node || !node[key]) return;
  const uint32_t expected = node[key].as<uint32_t>();
  if (expected != actual) {
    throw std::runtime_error(std::string("packetization.") + key + " (" +
                             std::to_string(expected) + ") must match " + source + " (" +
                             std::to_string(actual) + ")");
  }
}

void validate_optional_u16(const YAML::Node& node,
                           const char* key,
                           uint16_t actual,
                           const char* source) {
  if (!node || !node[key]) return;
  const uint16_t expected = node[key].as<uint16_t>();
  if (expected != actual) {
    throw std::runtime_error(std::string("packetization.") + key + " (" +
                             std::to_string(expected) + ") must match " + source + " (" +
                             std::to_string(actual) + ")");
  }
}

void validate_optional_u8(const YAML::Node& node,
                          const char* key,
                          uint8_t actual,
                          const char* source) {
  if (!node || !node[key]) return;
  const uint8_t expected = node[key].as<uint8_t>();
  if (expected != actual) {
    throw std::runtime_error(std::string("packetization.") + key + " (" +
                             std::to_string(expected) + ") must match " + source + " (" +
                             std::to_string(actual) + ")");
  }
}

}  // namespace

bool AppConfig::apply_images_per_batch_override(uint32_t n) {
  if (n == 0 || packets_per_image == 0 || packets_per_batch == 0) return false;
  if (packets_per_batch % packets_per_image != 0) return false;
  const uint32_t max_imgs = packets_per_batch / packets_per_image;
  if (n > max_imgs || max_imgs % n != 0 || !is_power_of_two(n)) return false;
  images_per_batch = std::min(n, static_cast<uint32_t>(trt.opt_max));
  return true;
}

AppConfig AppConfig::from_yaml(const YAML::Node& root) {
  AppConfig cfg;

  const auto rx_cfgs = daqiri::bench::parse_rx_configs(root);
  const auto tx_cfgs = daqiri::bench::parse_tx_configs(root);
  cfg.has_rx = !rx_cfgs.empty();
  cfg.has_tx = !tx_cfgs.empty();
  if (!cfg.has_rx && !cfg.has_tx) {
    throw std::runtime_error("config must define bench_rx and/or bench_tx");
  }
  if (cfg.has_rx) cfg.rx = rx_cfgs.front();
  if (cfg.has_tx) cfg.tx = tx_cfgs.front();

  if (cfg.has_rx && cfg.has_tx) {
    cfg.role = AppRole::Both;
  } else if (cfg.has_tx) {
    cfg.role = AppRole::Tx;
  } else {
    cfg.role = AppRole::Rx;
  }

  cfg.trt = TrtRunnerConfig::from_yaml(root);

  const auto packetization = root["packetization"];
  if (!packetization) {
    throw std::runtime_error("config must define packetization: for app image/frame geometry");
  }
  if (packetization["reorder_name"]) {
    cfg.reorder_name = packetization["reorder_name"].as<std::string>();
  }
  if (packetization["packets_per_image"]) {
    cfg.packets_per_image = packetization["packets_per_image"].as<uint32_t>();
  }
  bool images_per_batch_set = false;
  if (packetization["images_per_batch"]) {
    cfg.images_per_batch = packetization["images_per_batch"].as<uint32_t>();
    images_per_batch_set = true;
  } else if (root["inference"] && root["inference"]["images_per_batch"]) {
    cfg.images_per_batch = root["inference"]["images_per_batch"].as<uint32_t>();
    images_per_batch_set = true;
  }

  if (cfg.packets_per_image == 0) {
    throw std::runtime_error("packetization.packets_per_image must be > 0");
  }
  if (cfg.images_per_batch == 0) cfg.images_per_batch = 1;

  if (cfg.has_rx) {
    NativeReorderInfo reorder =
        find_native_reorder(root, cfg.rx.interface_name, cfg.rx.queue_id, cfg.reorder_name);
    cfg.reorder_name = reorder.node["name"].as<std::string>();
    cfg.reorder_engine = reorder.reorder_engine;
    cfg.reorder_memory_region = reorder.output_mr;
    cfg.payload_byte_offset = reorder.payload_byte_offset;
    cfg.seq_bit_offset = reorder.seq_bit_offset;
    cfg.seq_bit_width = reorder.seq_bit_width;
    cfg.packets_per_batch = reorder.packets_per_batch;

    if (reorder.input_type != "int8") {
      throw std::runtime_error("ResNet packetization expects reorder data_types.input_type int8");
    }
    if (reorder.output_type != "fp16" && reorder.output_type != "int8") {
      throw std::runtime_error("ResNet inference supports reorder output_type fp16 or int8");
    }
    if (reorder.reorder_engine == "hw" && reorder.has_data_types) {
      throw std::runtime_error(
          "hardware reorder cannot use data_types conversion; use INT8 TRT input");
    }

    if (reorder.packet_size != 0) {
      cfg.out_payload_len = reorder.packet_size;
    } else {
      if (reorder.source_buf_size <= reorder.payload_byte_offset) {
        throw std::runtime_error("RX source memory region is too small for payload_byte_offset");
      }
      cfg.out_payload_len = checked_u32(reorder.source_buf_size - reorder.payload_byte_offset,
                                        "RX source payload length");
    }
    cfg.reorder_output_type = reorder.output_type;
    const uint32_t output_elem_bytes = data_type_bytes(cfg.reorder_output_type);
    if (output_elem_bytes == 0) {
      throw std::runtime_error("unsupported reorder output_type '" + cfg.reorder_output_type + "'");
    }
    cfg.output_slot_stride = cfg.out_payload_len * output_elem_bytes;

    validate_optional_u32(packetization, "out_payload_len", cfg.out_payload_len,
                          "native reorder packet payload length");
    validate_optional_u32(packetization, "payload_byte_offset", cfg.payload_byte_offset,
                          "native reorder payload_byte_offset");
    validate_optional_u16(packetization, "seq_bit_offset", cfg.seq_bit_offset,
                          "native reorder sequence_number.bit_offset");
    validate_optional_u8(packetization, "seq_bit_width", cfg.seq_bit_width,
                         "native reorder sequence_number.bit_width");

    if (reorder.reorder_engine == "sw" &&
        reorder.source_buf_size != cfg.payload_byte_offset + cfg.out_payload_len) {
      throw std::runtime_error(
          "software reorder source MR '" + reorder.source_mr + "' buf_size " +
          std::to_string(reorder.source_buf_size) +
          " must equal payload_byte_offset + payload length (" +
          std::to_string(cfg.payload_byte_offset + cfg.out_payload_len) +
          "); DAQIRI derives software reorder slot stride from source buf_size");
    }
    if (reorder.source_buf_size < cfg.payload_byte_offset + cfg.out_payload_len) {
      throw std::runtime_error("RX source MR is smaller than payload_byte_offset + payload length");
    }
    const uint64_t required_output =
        static_cast<uint64_t>(cfg.packets_per_batch) * cfg.output_slot_stride;
    if (reorder.output_buf_size < required_output) {
      throw std::runtime_error("reorder memory region '" + cfg.reorder_memory_region +
                               "' buf_size " + std::to_string(reorder.output_buf_size) +
                               " must be >= packets_per_batch * output_slot_stride (" +
                               std::to_string(required_output) + ")");
    }
    if (!images_per_batch_set) {
      cfg.images_per_batch = cfg.packets_per_batch / cfg.packets_per_image;
    }
  } else {
    const char* required[] = {"out_payload_len", "payload_byte_offset", "seq_bit_offset",
                              "seq_bit_width"};
    for (const char* key : required) {
      if (!packetization[key]) {
        throw std::runtime_error(std::string("TX-only configs require packetization.") + key);
      }
    }
    cfg.out_payload_len = packetization["out_payload_len"].as<uint32_t>();
    cfg.payload_byte_offset = packetization["payload_byte_offset"].as<uint32_t>();
    cfg.seq_bit_offset = packetization["seq_bit_offset"].as<uint16_t>();
    cfg.seq_bit_width = packetization["seq_bit_width"].as<uint8_t>();
    cfg.reorder_output_type = cfg.trt.enable_int8 ? "int8" : "fp16";
    cfg.output_slot_stride = cfg.out_payload_len * data_type_bytes(cfg.reorder_output_type);
    cfg.packets_per_batch = cfg.packets_per_image * cfg.images_per_batch;
  }

  if (root["inference"] && root["inference"]["cpu_core"]) {
    cfg.inference_cpu_core = root["inference"]["cpu_core"].as<int>();
  }

  if (cfg.out_payload_len == 0 || cfg.output_slot_stride == 0) {
    throw std::runtime_error("packetization/reorder payload lengths must be > 0");
  }
  if (!is_power_of_two(cfg.packets_per_batch)) {
    throw std::runtime_error("packets_per_batch must be a power of two");
  }
  if (cfg.packets_per_batch % cfg.packets_per_image != 0) {
    throw std::runtime_error("packetization.packets_per_image must divide packets_per_batch");
  }
  if (!is_power_of_two(cfg.packets_per_image)) {
    throw std::runtime_error("packetization.packets_per_image must be a power of two");
  }
  const uint64_t seq_mod = pow2_u8(cfg.seq_bit_width, "sequence_number");
  if (cfg.seq_bit_width == 0 || (seq_mod % cfg.packets_per_batch) != 0) {
    throw std::runtime_error("2^seq_bit_width must be divisible by packets_per_batch");
  }
  if (static_cast<uint32_t>(cfg.seq_bit_offset) + cfg.seq_bit_width >
      cfg.payload_byte_offset * 8u) {
    throw std::runtime_error("sequence field must end at or before payload_byte_offset");
  }

  const bool int8_out = (cfg.reorder_output_type == "int8");
  if (!int8_out && cfg.reorder_output_type != "fp16") {
    throw std::runtime_error("reorder output_type must be fp16 or int8");
  }
  if (cfg.has_rx && int8_out && !cfg.trt.enable_int8) {
    throw std::runtime_error("INT8 reorder output requires inference.enable_int8: true");
  }

  const uint32_t elems_per_image =
      static_cast<uint32_t>(cfg.trt.channels) * cfg.trt.height * cfg.trt.width;
  const uint32_t bytes_per_elem = int8_out ? 1u : 2u;
  const uint32_t expected_out = elems_per_image * bytes_per_elem;
  cfg.image_out_bytes = cfg.packets_per_image * cfg.output_slot_stride;
  if (cfg.has_rx && cfg.image_out_bytes != expected_out) {
    throw std::runtime_error(
        "packets_per_image * output_slot_stride (" + std::to_string(cfg.image_out_bytes) +
        ") must equal channels*height*width*" + std::to_string(bytes_per_elem) + " (" +
        std::to_string(expected_out) + ") for " + cfg.reorder_output_type + " NCHW");
  }
  if (cfg.packets_per_image * cfg.out_payload_len != elems_per_image) {
    throw std::runtime_error(
        "packets_per_image * out_payload_len must equal channels*height*width (int8 elems)");
  }

  if (!cfg.apply_images_per_batch_override(cfg.images_per_batch)) {
    throw std::runtime_error(
        "images_per_batch must be a power-of-two divisor of "
        "(packets_per_batch / packets_per_image) and <= opt_max");
  }

  if (cfg.has_tx) {
    const auto dcfg = root["daqiri"] ? root["daqiri"]["cfg"] : YAML::Node();
    if (dcfg && dcfg["interfaces"]) {
      for (const auto& iface : dcfg["interfaces"]) {
        if (!iface["tx"] || iface["name"].as<std::string>() != cfg.tx.interface_name) continue;
        const auto queues = iface["tx"]["queues"];
        if (!queues || queues.size() == 0) continue;
        for (const auto& queue : queues) {
          if (queue["id"] && queue["id"].as<int>() != cfg.tx.queue_id) continue;
          const auto mrs = queue["memory_regions"];
          if (!mrs || mrs.size() == 0) continue;
          const auto tx_name = mrs[0].as<std::string>();
          const auto buf_size = memory_region_buf_size(root, tx_name);
          const uint32_t frame_bytes = cfg.frame_bytes();
          if (buf_size < frame_bytes) {
            throw std::runtime_error(
                "TX memory region '" + tx_name + "' buf_size " + std::to_string(buf_size) +
                " must be >= frame_bytes (" + std::to_string(frame_bytes) +
                ") so PCAP replay cannot overrun packet buffers");
          }
        }
      }
    }
  }

  const auto dataset = root["dataset"];
  if (dataset && dataset["pcap_path"]) {
    cfg.dataset_pcap = dataset["pcap_path"].as<std::string>();
  }
  if (dataset && dataset["labels_path"]) {
    cfg.labels_path = dataset["labels_path"].as<std::string>();
  }

  const auto stats = root["stats"];
  if (stats) {
    if (stats["enabled"]) cfg.stats_enabled = stats["enabled"].as<bool>();
    if (stats["top_k"]) cfg.stats_top_k = stats["top_k"].as<int>();
  }

  return cfg;
}

}  // namespace daqiri::apps::resnet
