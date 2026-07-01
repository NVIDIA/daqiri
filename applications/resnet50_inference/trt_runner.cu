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

// TensorRT 10.x engine build + FP16 inference + double-buffered async D2H. We
// use TRT directly (rather than a higher-level inference runtime) because the
// pipeline needs exactly one engine, one input, one output, and dynamic batch.

#include "trt_runner.hpp"

#include <NvInfer.h>
#include <NvInferRuntime.h>
#include <NvOnnxParser.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <vector>

namespace daqiri::apps::resnet {

namespace {

class TrtLogger : public nvinfer1::ILogger {
 public:
  void log(Severity severity, const char* msg) noexcept override {
    if (severity == Severity::kERROR || severity == Severity::kINTERNAL_ERROR) {
      std::cerr << "[TRT] " << msg << "\n";
    } else if (severity == Severity::kWARNING) {
      std::cerr << "[TRT][warn] " << msg << "\n";
    }
  }
};

TrtLogger& trt_logger() {
  static TrtLogger logger;
  return logger;
}

}  // namespace

TrtRunnerConfig TrtRunnerConfig::from_yaml(const YAML::Node& root) {
  TrtRunnerConfig cfg;
  const auto& inf = root["inference"];
  if (!inf) return cfg;
  if (inf["onnx_path"]) cfg.onnx_path = inf["onnx_path"].as<std::string>();
  if (inf["engine_path"]) cfg.engine_path = inf["engine_path"].as<std::string>();
  if (inf["opt_min"]) cfg.opt_min = inf["opt_min"].as<int>();
  if (inf["opt_avg"]) cfg.opt_avg = inf["opt_avg"].as<int>();
  if (inf["opt_max"]) cfg.opt_max = inf["opt_max"].as<int>();
  if (inf["enable_fp16"]) cfg.enable_fp16 = inf["enable_fp16"].as<bool>();
  if (inf["channels"]) cfg.channels = inf["channels"].as<int>();
  if (inf["height"]) cfg.height = inf["height"].as<int>();
  if (inf["width"]) cfg.width = inf["width"].as<int>();
  if (inf["feature_dim"]) cfg.feature_dim = inf["feature_dim"].as<int>();
  if (inf["gpu_id"]) cfg.gpu_id = inf["gpu_id"].as<int>();
  if (inf["input_name"]) cfg.input_name = inf["input_name"].as<std::string>();
  if (inf["output_name"]) cfg.output_name = inf["output_name"].as<std::string>();
  return cfg;
}

TrtRunner::TrtRunner(TrtRunnerConfig cfg, cudaStream_t inf_stream)
    : cfg_(std::move(cfg)), inf_stream_(inf_stream) {}

TrtRunner::~TrtRunner() {
  for (int i = 0; i < kBuffers; ++i) {
    if (host_buf_[i]) cudaFreeHost(host_buf_[i]);
    if (trt_out_dev_[i]) cudaFree(trt_out_dev_[i]);
    if (d2h_event_[i]) cudaEventDestroy(d2h_event_[i]);
    if (start_evt_[i]) cudaEventDestroy(start_evt_[i]);
  }
  delete context_;
  delete engine_;
  delete runtime_;
}

void TrtRunner::initialize() {
  std::cerr << "TrtRunner: onnx=" << cfg_.onnx_path << " engine_cache=" << cfg_.engine_path
            << " fp16=" << cfg_.enable_fp16 << " input=" << cfg_.channels << "x" << cfg_.height
            << "x" << cfg_.width << " feature_dim=" << cfg_.feature_dim << " opt=(" << cfg_.opt_min
            << "," << cfg_.opt_avg << "," << cfg_.opt_max << ")\n";

  runtime_ = nvinfer1::createInferRuntime(trt_logger());
  if (!runtime_) {
    std::cerr << "createInferRuntime failed\n";
    std::exit(1);
  }

  build_or_load_engine_();

  context_ = engine_->createExecutionContext();
  if (!context_) {
    std::cerr << "createExecutionContext failed\n";
    std::exit(1);
  }

  // The pcap carries FP32 NCHW pixels, so the engine input binding must be
  // FP32 (the kFP16 builder flag runs the network internally in FP16 but keeps
  // the declared input binding as the ONNX type). Catch an accidentally
  // FP16-input ONNX early rather than reading garbage at the first infer.
  if (engine_->getTensorDataType(cfg_.input_name.c_str()) != nvinfer1::DataType::kFLOAT) {
    std::cerr << "TrtRunner: input tensor '" << cfg_.input_name
              << "' is not FP32; export the ONNX with an FP32 input\n";
    std::exit(1);
  }

  allocate_buffers_();
  std::cerr << "TrtRunner ready\n";
}

void TrtRunner::build_or_load_engine_() {
  std::vector<char> plan_data;

  if (!cfg_.engine_path.empty()) {
    std::ifstream f(cfg_.engine_path, std::ios::binary | std::ios::ate);
    if (f) {
      const auto sz = static_cast<std::streamsize>(f.tellg());
      f.seekg(0, std::ios::beg);
      plan_data.resize(static_cast<size_t>(sz));
      if (f.read(plan_data.data(), sz)) {
        std::cerr << "Loaded cached engine from " << cfg_.engine_path << " (" << sz << " bytes)\n";
      } else {
        plan_data.clear();
      }
    }
  }

  if (plan_data.empty()) {
    std::cerr << "Building TRT engine from " << cfg_.onnx_path
              << " (first run; this can take a minute)\n";

    auto* builder = nvinfer1::createInferBuilder(trt_logger());
    if (!builder) {
      std::cerr << "createInferBuilder failed\n";
      std::exit(1);
    }

    // TRT 10 makes explicit batch the only (default) mode; pass no creation
    // flags (the old kEXPLICIT_BATCH flag is deprecated).
    auto* network = builder->createNetworkV2(0U);
    if (!network) {
      std::cerr << "createNetworkV2 failed\n";
      std::exit(1);
    }

    auto* parser = nvonnxparser::createParser(*network, trt_logger());
    if (!parser->parseFromFile(cfg_.onnx_path.c_str(),
                               static_cast<int>(nvinfer1::ILogger::Severity::kWARNING))) {
      std::cerr << "ONNX parse failed: " << cfg_.onnx_path << "\n";
      std::exit(1);
    }

    auto* config = builder->createBuilderConfig();
    // All supported targets (A100/H100/Ada/GB10) have fast FP16, so gate only on
    // the config flag. kFP16 is marked deprecated in TRT 10 in favor of
    // strongly-typed networks, but remains the simplest way to request a mixed
    // FP16 build from an FP32 ONNX.
    if (cfg_.enable_fp16) {
      config->setFlag(nvinfer1::BuilderFlag::kFP16);
    }

    auto* profile = builder->createOptimizationProfile();
    const nvinfer1::Dims4 dmin{cfg_.opt_min, cfg_.channels, cfg_.height, cfg_.width};
    const nvinfer1::Dims4 dopt{cfg_.opt_avg, cfg_.channels, cfg_.height, cfg_.width};
    const nvinfer1::Dims4 dmax{cfg_.opt_max, cfg_.channels, cfg_.height, cfg_.width};
    profile->setDimensions(cfg_.input_name.c_str(), nvinfer1::OptProfileSelector::kMIN, dmin);
    profile->setDimensions(cfg_.input_name.c_str(), nvinfer1::OptProfileSelector::kOPT, dopt);
    profile->setDimensions(cfg_.input_name.c_str(), nvinfer1::OptProfileSelector::kMAX, dmax);
    config->addOptimizationProfile(profile);

    auto* plan = builder->buildSerializedNetwork(*network, *config);
    if (!plan) {
      std::cerr << "buildSerializedNetwork failed\n";
      std::exit(1);
    }

    plan_data.assign(static_cast<const char*>(plan->data()),
                     static_cast<const char*>(plan->data()) + plan->size());

    if (!cfg_.engine_path.empty()) {
      std::ofstream out(cfg_.engine_path, std::ios::binary);
      out.write(plan_data.data(), static_cast<std::streamsize>(plan_data.size()));
      std::cerr << "Cached engine to " << cfg_.engine_path << " (" << plan_data.size()
                << " bytes)\n";
    }

    delete plan;
    delete config;
    delete parser;
    delete network;
    delete builder;
  }

  engine_ = runtime_->deserializeCudaEngine(plan_data.data(), plan_data.size());
  if (!engine_) {
    std::cerr << "deserializeCudaEngine failed\n";
    std::exit(1);
  }
}

void TrtRunner::allocate_buffers_() {
  // Feature-extractor output: feature_dim floats per image (batch * feature_dim).
  const size_t max_out_bytes = static_cast<size_t>(cfg_.opt_max) * cfg_.feature_dim * sizeof(float);
  for (int i = 0; i < kBuffers; ++i) {
    cudaMalloc(&trt_out_dev_[i], max_out_bytes);
    cudaMallocHost(&host_buf_[i], max_out_bytes);
    // Timing-enabled (default flags) so cudaEventElapsedTime(start_evt, d2h)
    // yields the per-batch latency; both events in a pair must be timing-enabled.
    cudaEventCreate(&d2h_event_[i]);
    cudaEventCreate(&start_evt_[i]);
  }
  std::cerr << "TrtRunner buffers: 2x" << max_out_bytes << " bytes pinned host + GPU output\n";
}

void TrtRunner::infer(float* dev_input, uint32_t batch, cudaEvent_t input_ready,
                      cudaEvent_t release_evt, float*& host_out_prev, uint32_t& host_out_prev_n) {
  host_out_prev = nullptr;
  host_out_prev_n = 0;

  if (batch == 0 || dev_input == nullptr) {
    std::cerr << "TrtRunner::infer called with batch=" << batch
              << " dev_input=" << static_cast<void*>(dev_input) << "\n";
    return;
  }
  // setInputShape logs + returns on overflow which would silently skip
  // inference; clamp so we never quietly produce zero features.
  if (batch > static_cast<uint32_t>(cfg_.opt_max)) {
    std::cerr << "TrtRunner::infer batch=" << batch << " > opt_max=" << cfg_.opt_max
              << "; clamping\n";
    batch = static_cast<uint32_t>(cfg_.opt_max);
  }

  // Gate this inference on the upstream reorder/copy completion.
  cudaStreamWaitEvent(inf_stream_, input_ready, 0);

  // Latency clock starts once the batch is ready on the stream (post-wait),
  // ending at this batch's d2h_event; read one batch late (see below).
  cudaEventRecord(start_evt_[parity_], inf_stream_);

  const nvinfer1::Dims4 dims{static_cast<int>(batch), cfg_.channels, cfg_.height, cfg_.width};
  if (!context_->setInputShape(cfg_.input_name.c_str(), dims)) {
    std::cerr << "setInputShape failed for batch=" << batch << "\n";
    return;
  }
  context_->setTensorAddress(cfg_.input_name.c_str(), dev_input);
  context_->setTensorAddress(cfg_.output_name.c_str(), trt_out_dev_[parity_]);

  if (!context_->enqueueV3(inf_stream_)) {
    std::cerr << "enqueueV3 failed\n";
    return;
  }

  const size_t out_bytes = static_cast<size_t>(batch) * cfg_.feature_dim * sizeof(float);
  cudaMemcpyAsync(host_buf_[parity_], trt_out_dev_[parity_], out_bytes, cudaMemcpyDeviceToHost,
                  inf_stream_);
  cudaEventRecord(d2h_event_[parity_], inf_stream_);

  // Back-edge signal: when this fires the input buffer is safe to reuse.
  cudaEventRecord(release_evt, inf_stream_);

  // Return the *previous* batch's now-ready buffer for the sink.
  const int prev = 1 - parity_;
  if (has_pending_[prev]) {
    cudaEventSynchronize(d2h_event_[prev]);
    float ms = 0.0f;
    if (cudaEventElapsedTime(&ms, start_evt_[prev], d2h_event_[prev]) == cudaSuccess) {
      batch_latency_ms_.push_back(ms);
    }
    host_out_prev = host_buf_[prev];
    host_out_prev_n = pending_n_[prev];
    has_pending_[prev] = false;
  }

  has_pending_[parity_] = true;
  pending_n_[parity_] = batch;
  parity_ = prev;

  ++total_batches_inferred_;
}

void TrtRunner::drain_final(float*& host_out, uint32_t& host_out_n) {
  host_out = nullptr;
  host_out_n = 0;
  for (int i = 0; i < kBuffers; ++i) {
    if (has_pending_[i]) {
      cudaEventSynchronize(d2h_event_[i]);
      float ms = 0.0f;
      if (cudaEventElapsedTime(&ms, start_evt_[i], d2h_event_[i]) == cudaSuccess) {
        batch_latency_ms_.push_back(ms);
      }
      host_out = host_buf_[i];
      host_out_n = pending_n_[i];
      has_pending_[i] = false;
      return;
    }
  }
}

}  // namespace daqiri::apps::resnet
