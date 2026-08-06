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

#include "raw_bench_common.h"
#include "trt_runner.hpp"

namespace daqiri::apps::resnet {

enum class AppRole { Both, Tx, Rx };

// Resolved configuration for the ResNet inference application. Geometry is
// taken from the app-level `reorder:` block (must match the RX
// `reorder_configs` plan): int8 wire → fp16 or int8 GPU, one reordered burst =
// one inference batch.
struct AppConfig {
  AppRole role = AppRole::Both;
  bool has_rx = false;
  bool has_tx = false;

  daqiri::bench::RawBenchRxConfig rx;
  daqiri::bench::RawBenchTxConfig tx;
  TrtRunnerConfig trt;

  // Config-based reorder geometry (authoritative; matches YAML reorder_configs).
  std::string reorder_name;
  // "fp16" (int8→fp16 convert) or "int8" (passthrough into INT8 TRT).
  std::string reorder_output_type = "fp16";
  uint32_t out_payload_len = 1176;       // wire int8 bytes per packet
  uint32_t output_slot_stride = 2352;    // bytes per packet slot (2352 fp16 / 1176 int8)
  uint32_t packets_per_image = 128;
  uint32_t packets_per_batch = 4096;
  uint32_t payload_byte_offset = 64;
  uint16_t seq_bit_offset = 128;
  uint8_t seq_bit_width = 12;
  uint32_t images_per_batch = 32;
  uint32_t image_out_bytes = 0;  // packets_per_image * output_slot_stride

  // Optional affinity for the inference consumer thread (RX role).
  int inference_cpu_core = -1;

  std::string dataset_pcap;
  std::string labels_path;

  bool stats_enabled = true;
  int stats_top_k = 8;
  // Headless 2-component PCA on a ring of recent features; print pc1/pc2 every N
  // batches (0 disables). Default matches issue #73 acceptance criteria.
  int pca_every_n_batches = 8;

  // Wire frame: header + int8 payload (no 4-byte seq prefix).
  uint32_t frame_bytes() const { return tx.header_size + out_payload_len; }

  bool example_mode() const { return !dataset_pcap.empty(); }

  static AppConfig from_yaml(const YAML::Node& root);

  // Clamp --images-per-batch to a power-of-two divisor of packets_per_batch /
  // packets_per_image. Returns false if invalid.
  bool apply_images_per_batch_override(uint32_t n);
};

}  // namespace daqiri::apps::resnet
