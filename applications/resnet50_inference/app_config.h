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

#pragma once

#include <yaml-cpp/yaml.h>

#include <cstdint>
#include <string>
#include <vector>

#include "raw_bench_common.h"
#include "trt_runner.hpp"

namespace daqiri::apps::resnet {

enum class AppRole { Both, Tx, Rx };

// Resolved configuration for the ResNet inference application.
//
// App-level `reorder:` is the single source of truth for reorder geometry.
// Before daqiri_init, call materialize_daqiri_reorder() to synthesize
// daqiri.cfg…rx.reorder_configs from that block (do not author reorder_configs
// in the YAML). packets_per_batch = packets_per_image * images_per_batch.
struct AppConfig {
  AppRole role = AppRole::Both;
  bool has_rx = false;
  bool has_tx = false;

  daqiri::bench::RawBenchRxConfig rx;
  daqiri::bench::RawBenchTxConfig tx;
  TrtRunnerConfig trt;

  std::string reorder_name;
  // "fp16" (int8→fp16 convert) or "int8" (passthrough into INT8 TRT).
  std::string reorder_output_type = "fp16";
  std::string reorder_type = "gpu";
  std::string reorder_memory_region = "Reorder_RX_GPU";
  // Empty => all flow IDs on the RX interface (must be non-empty after resolve).
  std::vector<uint32_t> reorder_flow_ids;

  uint32_t out_payload_len = 1176;     // wire int8 bytes per packet
  uint32_t output_slot_stride = 2352;  // bytes per packet slot (2352 fp16 / 1176 int8)
  uint32_t packets_per_image = 128;
  // Derived at YAML parse. --images-per-batch may shrink images_per_batch
  // afterward without changing this (engine window stays fixed).
  uint32_t packets_per_batch = 4096;
  uint32_t payload_byte_offset = 64;
  uint16_t seq_bit_offset = 128;
  uint8_t seq_bit_width = 12;
  uint32_t images_per_batch = 32;
  uint32_t image_out_bytes = 0;  // packets_per_image * output_slot_stride

  int inference_cpu_core = -1;

  std::string dataset_pcap;
  std::string labels_path;

  bool stats_enabled = true;
  int stats_top_k = 8;

  uint32_t frame_bytes() const { return tx.header_size + out_payload_len; }

  bool example_mode() const { return !dataset_pcap.empty(); }

  static AppConfig from_yaml(const YAML::Node& root);

  // Inject daqiri.cfg…rx.reorder_configs from this app config. Errors if the
  // YAML already defines reorder_configs (remove them; use app reorder: only).
  // No-op when has_rx is false.
  void materialize_daqiri_reorder(YAML::Node& root) const;

  // Clamp --images-per-batch to a power-of-two divisor of
  // (packets_per_batch / packets_per_image). Does not change packets_per_batch.
  bool apply_images_per_batch_override(uint32_t n);
};

}  // namespace daqiri::apps::resnet
