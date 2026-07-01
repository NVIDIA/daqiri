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
// extractor, then runs FP16 inference per batch with double-buffered async D2H
// to pinned host memory. The caller (inference_pipeline) feeds a contiguous
// FP32 NCHW device buffer + batch and gets back the *previous* batch's host
// feature buffer (now ready, since the stream serializes) for the FeatureSink.
//
// Adapted from the daqiri reorder->inference reference pipeline. The
// adaptation is mechanical but touches several sites versus a 1-channel
// classifier: 3-channel 224x224 input, a wide feature-vector output
// (feature_dim floats per image instead of one logit), and the matching D2H
// byte math.

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
  // release_evt after the input has been consumed (the back-edge signal a
  // producer reusing the input buffer can wait on). Writes the *previous*
  // batch's host-pinned output through host_out_prev/_n (a [n, feature_dim]
  // row-major float matrix) so the caller can hand it to the sink. On the first
  // call both outputs are null (no previous batch). dev_input is a contiguous
  // FP32 NCHW buffer of [batch, channels, height, width].
  void infer(float* dev_input, uint32_t batch, cudaEvent_t input_ready, cudaEvent_t release_evt,
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
  TrtRunnerConfig cfg_;
  cudaStream_t inf_stream_;

  nvinfer1::IRuntime* runtime_ = nullptr;
  nvinfer1::ICudaEngine* engine_ = nullptr;
  nvinfer1::IExecutionContext* context_ = nullptr;

  // Double-buffered output (parity alternates between batches).
  static constexpr int kBuffers = 2;
  float* trt_out_dev_[kBuffers] = {nullptr, nullptr};
  float* host_buf_[kBuffers] = {nullptr, nullptr};
  cudaEvent_t d2h_event_[kBuffers] = {nullptr, nullptr};
  // Timing-enabled start marker per buffer: recorded when the batch begins on
  // the inference stream; elapsed-to-d2h_event gives the batch latency.
  cudaEvent_t start_evt_[kBuffers] = {nullptr, nullptr};
  bool has_pending_[kBuffers] = {false, false};
  uint32_t pending_n_[kBuffers] = {0, 0};
  int parity_ = 0;

  uint64_t total_batches_inferred_ = 0;
  std::vector<float> batch_latency_ms_;

  void build_or_load_engine_();
  void allocate_buffers_();
};

}  // namespace daqiri::apps::resnet
