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

// Resolved configuration for the ResNet inference application.
//
// DAQIRI's native daqiri.cfg...rx.reorder_configs block is the source of truth
// for RX reorder behavior. The app-level packetization block carries only the
// image framing facts the TX/replay side needs.
struct AppConfig {
  AppRole role = AppRole::Both;
  bool has_rx = false;
  bool has_tx = false;

  daqiri::bench::RawBenchRxConfig rx;
  daqiri::bench::RawBenchTxConfig tx;
  std::vector<daqiri::bench::RawBenchTxConfig> tx_all;
  TrtRunnerConfig trt;

  std::string reorder_name;
  std::string reorder_engine = "sw";
  // Native reorder output in GPU memory. Hardware reorder produces int8; the
  // application may convert that buffer to fp16 before TensorRT.
  std::string reorder_output_type = "fp16";
  std::string reorder_memory_region = "Reorder_RX_GPU";

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
  // CLI-only measurement mode for the DAQIRI-alone campaign row. The RX
  // producer stays identical; only the consumer skips TensorRT.
  bool no_inference = false;
  int stats_top_k = 8;

  uint32_t frame_bytes() const { return tx.header_size + out_payload_len; }

  bool example_mode() const { return !dataset_pcap.empty(); }

  bool needs_post_reorder_conversion() const {
    return reorder_engine == "hw" && reorder_output_type == "int8" && !trt.enable_int8;
  }

  const char* trt_input_type() const {
    return trt.enable_int8 ? "int8" : "fp16";
  }

  static AppConfig from_yaml(const YAML::Node& root);

  // Clamp --images-per-batch to a power-of-two divisor of
  // (packets_per_batch / packets_per_image). Does not change packets_per_batch.
  bool apply_images_per_batch_override(uint32_t n);
};

}  // namespace daqiri::apps::resnet
