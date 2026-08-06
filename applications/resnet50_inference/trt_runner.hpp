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

// Builds (or loads from a disk cache) a TensorRT engine for a ResNet feature
// extractor, then runs inference per batch with double-buffered async D2H to
// pinned host memory. The caller feeds a contiguous NCHW device buffer (DAQIRI
// reorder output: FP16 or INT8) + batch and gets back the *previous* batch's
// host feature buffer for the FeatureSink (always float32).
//
// FP16 path: ONNX input authored HALF by export_resnet_onnx.py (in-model norm);
// features output is authored FLOAT (.float() after the backbone). Stale engines
// with HALF features are still accepted and converted to float before the sink.
// INT8 path: ONNX authored INT8 input + cast/norm; built with kINT8 (+ optional
// kFP16) and an entropy calibrator (cache next to the engine).

#include <yaml-cpp/yaml.h>

#include <cstdint>
#include <string>
#include <vector>

#include <cuda_runtime.h>

// Forward-declare TRT types to keep this header light.
namespace nvinfer1 {
class IRuntime;
class ICudaEngine;
class IExecutionContext;
}  // namespace nvinfer1

namespace daqiri::apps::resnet {

struct TrtRunnerConfig {
  std::string onnx_path;
  std::string engine_path;  // cache path; built on first launch if absent
  int opt_min = 1;
  int opt_avg = 32;
  int opt_max = 256;
  bool enable_fp16 = true;
  bool enable_int8 = false;
  // Two execution contexts on two streams to overlap consecutive batches.
  bool enable_dual_context = false;
  // Capture a CUDA graph for the fixed opt_avg batch (staging copy + enqueue + D2H).
  bool enable_cuda_graph = false;
  // Written next to engine when enable_int8 builds; reused on later builds.
  std::string calib_cache_path;
  int channels = 3;
  int height = 224;
  int width = 224;
  int feature_dim = 2048;  // 512 for resnet18/34; 2048 for resnet50/101/152
  int gpu_id = 0;
  std::string input_name = "input";
  std::string output_name = "features";

  static TrtRunnerConfig from_yaml(const YAML::Node& root);
};

class TrtRunner {
 public:
  TrtRunner(TrtRunnerConfig cfg, cudaStream_t inf_stream);
  ~TrtRunner();

  TrtRunner(const TrtRunner&) = delete;
  TrtRunner& operator=(const TrtRunner&) = delete;

  void initialize();

  // Per-batch inference. Stream-waits on input_ready before TRT runs, records
  // release_evt after enqueue (optional back-edge). Writes the *previous*
  // batch's host-pinned output through host_out_prev/_n (a [n, feature_dim]
  // row-major float matrix). Returns true only when a batch was issued and
  // internal parity advanced; false on early-return (caller must not rotate
  // prev_burst). On the first successful call both outputs are null.
  // dev_input is a contiguous NCHW buffer of [batch, channels, height, width]
  // (FP16 or INT8 matching the engine input binding).
  bool infer(void* dev_input, uint32_t batch, cudaEvent_t input_ready, cudaEvent_t release_evt,
             float*& host_out_prev, uint32_t& host_out_prev_n);

  // Synchronously flush the final pending batch at shutdown.
  void drain_final(float*& host_out, uint32_t& host_out_n);

  int feature_dim() const {
    return cfg_.feature_dim;
  }
  int opt_max() const {
    return cfg_.opt_max;
  }
  uint64_t total_batches_inferred() const {
    return total_batches_inferred_;
  }

  // Per-batch inference latency in milliseconds (batch-ready -> features on
  // host), one entry per completed batch. Measured with CUDA timing events.
  const std::vector<float>& batch_latencies_ms() const {
    return batch_latency_ms_;
  }

 private:
  static constexpr int kBuffers = 2;

  TrtRunnerConfig cfg_;
  cudaStream_t inf_stream_;

  nvinfer1::IRuntime* runtime_ = nullptr;
  nvinfer1::ICudaEngine* engine_ = nullptr;
  // One or two contexts (dual_context uses both).
  nvinfer1::IExecutionContext* context_[kBuffers] = {nullptr, nullptr};
  cudaStream_t streams_[kBuffers] = {nullptr, nullptr};
  int n_contexts_ = 1;
  bool owns_streams_ = false;

  float* trt_out_dev_[kBuffers] = {nullptr, nullptr};  // float features for sink
  float* host_buf_[kBuffers] = {nullptr, nullptr};
  void* trt_out_raw_[kBuffers] = {nullptr, nullptr};  // engine output (float or half)
  size_t out_elem_bytes_ = sizeof(float);
  bool out_is_half_ = false;

  cudaEvent_t d2h_event_[kBuffers] = {nullptr, nullptr};
  cudaEvent_t start_evt_[kBuffers] = {nullptr, nullptr};
  bool has_pending_[kBuffers] = {false, false};
  uint32_t pending_n_[kBuffers] = {0, 0};
  int parity_ = 0;

  bool graph_ready_ = false;
  bool graph_capture_failed_ = false;
  cudaGraph_t graph_ = nullptr;
  cudaGraphExec_t graph_exec_ = nullptr;
  void* graph_input_staging_ = nullptr;
  size_t graph_input_bytes_ = 0;

  uint64_t total_batches_inferred_ = 0;
  std::vector<float> batch_latency_ms_;

  void build_or_load_engine_();
  void allocate_buffers_();
  void ensure_cuda_graph_(uint32_t batch);
  void convert_half_out_to_float_(int buf, uint32_t n);
};

}  // namespace daqiri::apps::resnet
