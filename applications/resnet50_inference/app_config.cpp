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

namespace daqiri::apps::resnet {

AppConfig AppConfig::from_yaml(const YAML::Node& root) {
  AppConfig cfg;

  const auto rx_cfgs = daqiri::bench::parse_rx_configs(root);
  const auto tx_cfgs = daqiri::bench::parse_tx_configs(root);
  if (rx_cfgs.empty() || tx_cfgs.empty()) {
    throw std::runtime_error("config must define one bench_rx and one bench_tx interface");
  }
  cfg.rx = rx_cfgs.front();
  cfg.tx = tx_cfgs.front();
  cfg.trt = TrtRunnerConfig::from_yaml(root);

  const auto& reorder = root["reorder"];
  if (reorder && reorder["out_payload_len"]) {
    cfg.out_payload_len = reorder["out_payload_len"].as<uint32_t>();
  }
  if (reorder && reorder["images_per_batch"]) {
    cfg.images_per_batch = reorder["images_per_batch"].as<uint32_t>();
  } else if (root["inference"] && root["inference"]["images_per_batch"]) {
    cfg.images_per_batch = root["inference"]["images_per_batch"].as<uint32_t>();
  }

  // Image size and reorder geometry derived from the model input + chunk size.
  cfg.image_bytes =
      static_cast<uint32_t>(cfg.trt.channels) * cfg.trt.height * cfg.trt.width * sizeof(float);
  if (cfg.out_payload_len == 0 || cfg.image_bytes % cfg.out_payload_len != 0) {
    throw std::runtime_error("reorder.out_payload_len (" + std::to_string(cfg.out_payload_len) +
                             ") must evenly divide the image size (" +
                             std::to_string(cfg.image_bytes) +
                             " bytes = channels*height*width*4); pick a clean divisor so images "
                             "reassemble with no padding");
  }
  cfg.packets_per_image = cfg.image_bytes / cfg.out_payload_len;
  // The seq number occupies the first 4 payload bytes; pixels start after it.
  cfg.seq_bit_offset = static_cast<uint16_t>(cfg.tx.header_size * 8u);
  cfg.payload_byte_offset = cfg.tx.header_size + 4u;
  cfg.seq_bit_width = 32;

  if (cfg.images_per_batch == 0) cfg.images_per_batch = 1;
  cfg.images_per_batch =
      std::min<uint32_t>(cfg.images_per_batch, static_cast<uint32_t>(cfg.trt.opt_max));

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
