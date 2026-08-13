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
#include <stdexcept>
#include <string>
#include <vector>

namespace daqiri::apps::resnet {
namespace {

bool is_power_of_two(uint32_t n) { return n != 0 && (n & (n - 1)) == 0; }

std::vector<uint32_t> collect_rx_flow_ids(const YAML::Node& root, const std::string& iface_name) {
  std::vector<uint32_t> ids;
  const auto& daq = root["daqiri"];
  const auto& dcfg = daq ? daq["cfg"] : YAML::Node();
  if (!dcfg || !dcfg["interfaces"]) return ids;
  for (const auto& iface : dcfg["interfaces"]) {
    if (!iface["rx"] || !iface["name"] || iface["name"].as<std::string>() != iface_name) continue;
    const auto& flows = iface["rx"]["flows"];
    if (!flows) continue;
    for (const auto& flow : flows) {
      if (flow["id"]) ids.push_back(flow["id"].as<uint32_t>());
    }
  }
  return ids;
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

void AppConfig::materialize_daqiri_reorder(YAML::Node& root) const {
  if (!has_rx) return;
  if (reorder_name.empty()) {
    throw std::runtime_error("materialize_daqiri_reorder: reorder_name is empty");
  }
  if (reorder_flow_ids.empty()) {
    throw std::runtime_error("materialize_daqiri_reorder: reorder_flow_ids is empty");
  }

  auto daq = root["daqiri"];
  if (!daq) throw std::runtime_error("config missing daqiri:");
  auto dcfg = daq["cfg"];
  if (!dcfg) throw std::runtime_error("config missing daqiri.cfg");
  auto ifaces = dcfg["interfaces"];
  if (!ifaces) throw std::runtime_error("config missing daqiri.cfg.interfaces");

  bool found = false;
  for (std::size_t i = 0; i < ifaces.size(); ++i) {
    auto iface = ifaces[i];
    if (!iface["rx"] || !iface["name"] || iface["name"].as<std::string>() != rx.interface_name) {
      continue;
    }
    found = true;
    if (iface["rx"]["reorder_configs"]) {
      throw std::runtime_error(
          "daqiri…rx.reorder_configs is synthesized from app-level reorder:; "
          "remove reorder_configs from the YAML");
    }

    YAML::Node seq;
    seq["bit_offset"] = static_cast<int>(seq_bit_offset);
    seq["bit_width"] = static_cast<int>(seq_bit_width);

    YAML::Node spp;
    spp["sequence_number"] = seq;
    spp["packets_per_batch"] = packets_per_batch;

    YAML::Node method;
    method["seq_packets_per_batch"] = spp;

    YAML::Node dtypes;
    dtypes["input_type"] = "int8";
    dtypes["output_type"] = reorder_output_type;
    dtypes["endianness"] = "host";

    YAML::Node flow_ids_node;
    for (uint32_t id : reorder_flow_ids) flow_ids_node.push_back(id);

    YAML::Node rc;
    rc["name"] = reorder_name;
    rc["reorder_type"] = reorder_type;
    rc["memory_region"] = reorder_memory_region;
    rc["payload_byte_offset"] = payload_byte_offset;
    rc["flow_ids"] = flow_ids_node;
    rc["data_types"] = dtypes;
    rc["method"] = method;

    YAML::Node rcfgs;
    rcfgs.push_back(rc);
    iface["rx"]["reorder_configs"] = rcfgs;
    ifaces[i] = iface;
  }
  if (!found) {
    throw std::runtime_error("materialize_daqiri_reorder: no RX interface named '" +
                             rx.interface_name + "'");
  }
  dcfg["interfaces"] = ifaces;
  daq["cfg"] = dcfg;
  root["daqiri"] = daq;
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

  const auto& reorder = root["reorder"];
  if (!reorder) {
    throw std::runtime_error("config must define a reorder: block (app source of truth)");
  }
  if (reorder["reorder_name"]) cfg.reorder_name = reorder["reorder_name"].as<std::string>();
  if (reorder["output_type"]) {
    cfg.reorder_output_type = reorder["output_type"].as<std::string>();
  }
  if (reorder["reorder_type"]) cfg.reorder_type = reorder["reorder_type"].as<std::string>();
  if (reorder["memory_region"]) {
    cfg.reorder_memory_region = reorder["memory_region"].as<std::string>();
  }
  if (reorder["flow_ids"]) {
    for (const auto& id : reorder["flow_ids"]) {
      cfg.reorder_flow_ids.push_back(id.as<uint32_t>());
    }
  }
  if (reorder["out_payload_len"]) cfg.out_payload_len = reorder["out_payload_len"].as<uint32_t>();
  if (reorder["output_slot_stride"]) {
    cfg.output_slot_stride = reorder["output_slot_stride"].as<uint32_t>();
  }
  if (reorder["packets_per_image"]) {
    cfg.packets_per_image = reorder["packets_per_image"].as<uint32_t>();
  }
  if (reorder["payload_byte_offset"]) {
    cfg.payload_byte_offset = reorder["payload_byte_offset"].as<uint32_t>();
  }
  if (reorder["seq_bit_offset"]) {
    cfg.seq_bit_offset = reorder["seq_bit_offset"].as<uint16_t>();
  }
  if (reorder["seq_bit_width"]) cfg.seq_bit_width = reorder["seq_bit_width"].as<uint8_t>();
  if (reorder["images_per_batch"]) {
    cfg.images_per_batch = reorder["images_per_batch"].as<uint32_t>();
  } else if (root["inference"] && root["inference"]["images_per_batch"]) {
    cfg.images_per_batch = root["inference"]["images_per_batch"].as<uint32_t>();
  }
  if (reorder["image_out_bytes"]) {
    cfg.image_out_bytes = reorder["image_out_bytes"].as<uint32_t>();
  }

  if (cfg.images_per_batch == 0) cfg.images_per_batch = 1;
  if (cfg.packets_per_image == 0) {
    throw std::runtime_error("reorder.packets_per_image must be > 0");
  }
  const uint32_t derived_ppb = cfg.packets_per_image * cfg.images_per_batch;
  if (reorder["packets_per_batch"]) {
    const uint32_t yaml_ppb = reorder["packets_per_batch"].as<uint32_t>();
    if (yaml_ppb != derived_ppb) {
      throw std::runtime_error(
          "reorder.packets_per_batch (" + std::to_string(yaml_ppb) +
          ") must equal packets_per_image * images_per_batch (" + std::to_string(derived_ppb) +
          "); prefer omitting reorder.packets_per_batch (it is derived)");
    }
  }
  cfg.packets_per_batch = derived_ppb;

  if (cfg.has_rx && cfg.reorder_flow_ids.empty()) {
    cfg.reorder_flow_ids = collect_rx_flow_ids(root, cfg.rx.interface_name);
    if (cfg.reorder_flow_ids.empty()) {
      throw std::runtime_error(
          "reorder.flow_ids not set and no flows found on RX interface '" + cfg.rx.interface_name +
          "'");
    }
  }

  if (root["inference"] && root["inference"]["cpu_core"]) {
    cfg.inference_cpu_core = root["inference"]["cpu_core"].as<int>();
  }

  if (cfg.has_rx && cfg.reorder_name.empty()) {
    throw std::runtime_error("RX role requires reorder.reorder_name");
  }
  if (cfg.out_payload_len == 0 || cfg.output_slot_stride == 0) {
    throw std::runtime_error("reorder.out_payload_len and output_slot_stride must be > 0");
  }
  if (!is_power_of_two(cfg.packets_per_batch)) {
    throw std::runtime_error(
        "derived packets_per_batch (packets_per_image * images_per_batch) must be a power of two");
  }
  if (cfg.packets_per_batch % cfg.packets_per_image != 0) {
    throw std::runtime_error("reorder.packets_per_image must divide packets_per_batch");
  }
  if (!is_power_of_two(cfg.packets_per_image)) {
    throw std::runtime_error("reorder.packets_per_image must be a power of two");
  }
  const uint64_t seq_mod = 1ULL << cfg.seq_bit_width;
  if (cfg.seq_bit_width == 0 || cfg.seq_bit_width > 32 ||
      (seq_mod % cfg.packets_per_batch) != 0) {
    throw std::runtime_error(
        "2^seq_bit_width must be divisible by packets_per_batch (DAQIRI reorder constraint)");
  }
  if (static_cast<uint32_t>(cfg.seq_bit_offset) + cfg.seq_bit_width >
      cfg.payload_byte_offset * 8u) {
    throw std::runtime_error("seq field must end at or before payload_byte_offset");
  }
  const bool int8_out = (cfg.reorder_output_type == "int8");
  if (!int8_out && cfg.reorder_output_type != "fp16") {
    throw std::runtime_error("reorder.output_type must be \"fp16\" or \"int8\"");
  }
  if (int8_out) {
    if (cfg.output_slot_stride != cfg.out_payload_len) {
      throw std::runtime_error(
          "for int8→int8, output_slot_stride must equal out_payload_len (wire bytes)");
    }
    if (!cfg.trt.enable_int8) {
      throw std::runtime_error("reorder.output_type int8 requires inference.enable_int8: true");
    }
  } else if (cfg.output_slot_stride != cfg.out_payload_len * 2u) {
    throw std::runtime_error(
        "for int8→fp16, output_slot_stride must equal 2 * out_payload_len (wire bytes)");
  }

  const uint32_t elems_per_image =
      static_cast<uint32_t>(cfg.trt.channels) * cfg.trt.height * cfg.trt.width;
  const uint32_t bytes_per_elem = int8_out ? 1u : 2u;
  const uint32_t expected_out = elems_per_image * bytes_per_elem;
  if (cfg.image_out_bytes == 0) {
    cfg.image_out_bytes = cfg.packets_per_image * cfg.output_slot_stride;
  }
  if (cfg.image_out_bytes != expected_out) {
    throw std::runtime_error(
        "image_out_bytes (" + std::to_string(cfg.image_out_bytes) +
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

  if (cfg.has_rx) {
    const auto& daq = root["daqiri"];
    const auto& dcfg = daq ? daq["cfg"] : YAML::Node();
    if (dcfg && dcfg["interfaces"]) {
      for (const auto& iface : dcfg["interfaces"]) {
        if (!iface["rx"] || iface["name"].as<std::string>() != cfg.rx.interface_name) continue;
        if (iface["rx"]["reorder_configs"]) {
          throw std::runtime_error(
              "daqiri…rx.reorder_configs is synthesized from app-level reorder:; "
              "remove reorder_configs from the YAML");
        }
        const auto& queues = iface["rx"]["queues"];
        if (!queues || queues.size() == 0) continue;
        const auto& mrs = queues[0]["memory_regions"];
        if (!mrs || mrs.size() == 0) continue;
        const auto src_name = mrs[0].as<std::string>();
        bool found_reorder_mr = false;
        for (const auto& mr : dcfg["memory_regions"]) {
          const auto mr_name = mr["name"].as<std::string>();
          if (mr_name == src_name) {
            const auto buf_size = mr["buf_size"].as<uint64_t>();
            if (buf_size != cfg.payload_byte_offset + cfg.out_payload_len) {
              throw std::runtime_error(
                  "RX source memory region '" + src_name + "' buf_size " +
                  std::to_string(buf_size) + " must equal payload_byte_offset + out_payload_len (" +
                  std::to_string(cfg.payload_byte_offset + cfg.out_payload_len) +
                  "); the reorder slot stride is derived from it, so slack pads the NCHW batch");
            }
          }
          if (mr_name == cfg.reorder_memory_region) {
            found_reorder_mr = true;
            const auto buf_size = mr["buf_size"].as<uint64_t>();
            const uint64_t expect =
                static_cast<uint64_t>(cfg.packets_per_batch) * cfg.output_slot_stride;
            if (buf_size != expect) {
              throw std::runtime_error(
                  "reorder memory region '" + cfg.reorder_memory_region + "' buf_size " +
                  std::to_string(buf_size) + " must equal packets_per_batch * output_slot_stride (" +
                  std::to_string(expect) + ")");
            }
          }
        }
        if (!found_reorder_mr) {
          throw std::runtime_error("reorder.memory_region '" + cfg.reorder_memory_region +
                                   "' not found in daqiri.cfg.memory_regions");
        }
      }
    }
  }
  if (cfg.has_tx) {
    const auto& daq = root["daqiri"];
    const auto& dcfg = daq ? daq["cfg"] : YAML::Node();
    if (dcfg && dcfg["interfaces"]) {
      for (const auto& iface : dcfg["interfaces"]) {
        if (!iface["tx"] || iface["name"].as<std::string>() != cfg.tx.interface_name) continue;
        const auto& queues = iface["tx"]["queues"];
        if (!queues || queues.size() == 0) continue;
        for (const auto& queue : queues) {
          if (queue["id"] && queue["id"].as<int>() != cfg.tx.queue_id) continue;
          const auto& mrs = queue["memory_regions"];
          if (!mrs || mrs.size() == 0) continue;
          const auto tx_name = mrs[0].as<std::string>();
          for (const auto& mr : dcfg["memory_regions"]) {
            if (mr["name"].as<std::string>() != tx_name) continue;
            const auto buf_size = mr["buf_size"].as<uint64_t>();
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
  }

  const auto& dataset = root["dataset"];
  if (dataset && dataset["pcap_path"]) {
    cfg.dataset_pcap = dataset["pcap_path"].as<std::string>();
  }
  if (dataset && dataset["labels_path"]) {
    cfg.labels_path = dataset["labels_path"].as<std::string>();
  }

  const auto& stats = root["stats"];
  if (stats) {
    if (stats["enabled"]) cfg.stats_enabled = stats["enabled"].as<bool>();
    if (stats["top_k"]) cfg.stats_top_k = stats["top_k"].as<int>();
  }

  return cfg;
}

}  // namespace daqiri::apps::resnet
