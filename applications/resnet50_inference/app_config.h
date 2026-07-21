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

#include "raw_bench_common.h"  // daqiri::bench::RawBench{Rx,Tx}Config + parsers
#include "trt_runner.hpp"

namespace daqiri::apps::resnet {

// Resolved configuration for the ResNet inference application. The rx/tx blocks
// reuse the bench parsers; the inference/reorder/dataset/stats blocks are
// app-specific. Geometry (packets_per_image, offsets) is derived from the model
// input dims and the per-packet chunk size so the runtime stays pure
// reorder -> infer.
struct AppConfig {
  daqiri::bench::RawBenchRxConfig rx;
  daqiri::bench::RawBenchTxConfig tx;
  TrtRunnerConfig trt;

  // Derived image / reorder geometry.
  uint32_t image_bytes = 0;          // channels * height * width * sizeof(float)
  uint32_t out_payload_len = 7168;   // image-pixel bytes carried per packet
  uint32_t packets_per_image = 0;    // image_bytes / out_payload_len (must divide)
  uint32_t payload_byte_offset = 0;  // header_size + seq prefix (4 bytes)
  uint16_t seq_bit_offset = 0;       // header_size * 8
  uint8_t seq_bit_width = 32;
  uint32_t images_per_batch = 32;  // TRT dynamic batch (clamped to opt_max)

  // Example mode (dataset replay). Empty pcap path => synthetic benchmark mode.
  std::string dataset_pcap;
  std::string labels_path;  // dataset_pcap + ".labels" (resolved if empty)

  // Per-class mean-feature stats (example mode only).
  bool stats_enabled = true;
  int stats_top_k = 8;

  // Bytes of a single frame on the wire: header + 4-byte seq prefix + chunk.
  uint32_t frame_bytes() const {
    return tx.header_size + 4u + out_payload_len;
  }

  // True when a dataset pcap was supplied (drives real images + per-class stats).
  bool example_mode() const {
    return !dataset_pcap.empty();
  }

  // Parse the YAML config. Throws YAML::Exception / std::runtime_error on a
  // malformed or geometrically-inconsistent config.
  static AppConfig from_yaml(const YAML::Node& root);
};

}  // namespace daqiri::apps::resnet
