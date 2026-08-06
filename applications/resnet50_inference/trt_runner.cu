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

// TensorRT 10.x engine build + FP16/INT8 inference + double-buffered async D2H.

#include "trt_runner.hpp"

#include <NvInfer.h>
#include <NvInferRuntime.h>
#include <NvOnnxParser.h>

#include <cuda_fp16.h>
#include <cuda_runtime.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <string>
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

// Entropy calibrator for INT8 PTQ. Feeds synthetic signed-int8 NCHW batches that
// match the wire distribution (pixel_uint8 - 128). Cache is reused across builds.
class Int8EntropyCalibrator : public nvinfer1::IInt8EntropyCalibrator2 {
 public:
  Int8EntropyCalibrator(std::string input_name, int batch, int c, int h, int w, int n_batches,
                        std::string cache_path)
      : input_name_(std::move(input_name)),
        batch_(batch),
        c_(c),
        h_(h),
        w_(w),
        n_batches_(n_batches),
        cache_path_(std::move(cache_path)) {
    const size_t elems = static_cast<size_t>(batch_) * c_ * h_ * w_;
    host_.resize(elems);
    cudaMalloc(&device_, elems * sizeof(int8_t));
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(-128, 127);
    for (auto& v : host_) v = static_cast<int8_t>(dist(rng));
  }

  ~Int8EntropyCalibrator() override {
    if (device_) cudaFree(device_);
  }

  int getBatchSize() const noexcept override { return batch_; }

  bool getBatch(void* bindings[], const char* names[], int nbBindings) noexcept override {
    if (done_ >= n_batches_) return false;
    for (int i = 0; i < nbBindings; ++i) {
      if (names[i] && input_name_ == names[i]) {
        cudaMemcpy(device_, host_.data(), host_.size() * sizeof(int8_t), cudaMemcpyHostToDevice);
        bindings[i] = device_;
        ++done_;
        return true;
      }
    }
    return false;
  }

  const void* readCalibrationCache(size_t& length) noexcept override {
    cache_.clear();
    std::ifstream f(cache_path_, std::ios::binary);
    if (!f) {
      length = 0;
      return nullptr;
    }
    f.seekg(0, std::ios::end);
    const auto sz = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    cache_.resize(sz);
    f.read(cache_.data(), static_cast<std::streamsize>(sz));
    length = cache_.size();
    std::cerr << "TrtRunner: loaded INT8 calib cache " << cache_path_ << " (" << length
              << " bytes)\n";
    return cache_.data();
  }

  void writeCalibrationCache(const void* cache, size_t length) noexcept override {
    if (cache_path_.empty() || cache == nullptr || length == 0) return;
    std::ofstream f(cache_path_, std::ios::binary);
    f.write(static_cast<const char*>(cache), static_cast<std::streamsize>(length));
    std::cerr << "TrtRunner: wrote INT8 calib cache " << cache_path_ << " (" << length
              << " bytes)\n";
  }

 private:
  std::string input_name_;
  int batch_;
  int c_;
  int h_;
  int w_;
  int n_batches_;
  int done_ = 0;
  std::string cache_path_;
  std::vector<int8_t> host_;
  std::vector<char> cache_;
  void* device_ = nullptr;
};

void* out_dev_ptr(bool out_is_half, void* raw, float* fp32) {
  return out_is_half ? raw : static_cast<void*>(fp32);
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
  if (inf["enable_int8"]) cfg.enable_int8 = inf["enable_int8"].as<bool>();
  if (inf["enable_dual_context"]) cfg.enable_dual_context = inf["enable_dual_context"].as<bool>();
  if (inf["enable_cuda_graph"]) cfg.enable_cuda_graph = inf["enable_cuda_graph"].as<bool>();
  if (inf["calib_cache_path"]) cfg.calib_cache_path = inf["calib_cache_path"].as<std::string>();
  if (inf["channels"]) cfg.channels = inf["channels"].as<int>();
  if (inf["height"]) cfg.height = inf["height"].as<int>();
  if (inf["width"]) cfg.width = inf["width"].as<int>();
  if (inf["feature_dim"]) cfg.feature_dim = inf["feature_dim"].as<int>();
  if (inf["gpu_id"]) cfg.gpu_id = inf["gpu_id"].as<int>();
  if (inf["input_name"]) cfg.input_name = inf["input_name"].as<std::string>();
  if (inf["output_name"]) cfg.output_name = inf["output_name"].as<std::string>();
  if (cfg.enable_int8 && cfg.calib_cache_path.empty() && !cfg.engine_path.empty()) {
    cfg.calib_cache_path = cfg.engine_path + ".calib";
  }
  return cfg;
}

TrtRunner::TrtRunner(TrtRunnerConfig cfg, cudaStream_t inf_stream)
    : cfg_(std::move(cfg)), inf_stream_(inf_stream) {}

TrtRunner::~TrtRunner() {
  if (graph_exec_) {
    cudaGraphExecDestroy(graph_exec_);
    graph_exec_ = nullptr;
  }
  if (graph_) {
    cudaGraphDestroy(graph_);
    graph_ = nullptr;
  }
  if (graph_input_staging_) {
    cudaFree(graph_input_staging_);
    graph_input_staging_ = nullptr;
  }

  for (int i = 0; i < kBuffers; ++i) {
    if (host_buf_[i]) cudaFreeHost(host_buf_[i]);
    if (trt_out_dev_[i]) cudaFree(trt_out_dev_[i]);
    if (trt_out_raw_[i]) {
      // When output is FP32, trt_out_raw_ is unused (nullptr) or not aliased.
      if (out_is_half_) cudaFree(trt_out_raw_[i]);
      trt_out_raw_[i] = nullptr;
    }
    if (d2h_event_[i]) cudaEventDestroy(d2h_event_[i]);
    if (start_evt_[i]) cudaEventDestroy(start_evt_[i]);
  }

  if (owns_streams_) {
    for (int i = 0; i < kBuffers; ++i) {
      if (streams_[i]) {
        cudaStreamDestroy(streams_[i]);
        streams_[i] = nullptr;
      }
    }
  }

  for (int i = 0; i < kBuffers; ++i) {
    delete context_[i];
    context_[i] = nullptr;
  }
  delete engine_;
  delete runtime_;
}

void TrtRunner::initialize() {
  std::cerr << "TrtRunner: onnx=" << cfg_.onnx_path << " engine_cache=" << cfg_.engine_path
            << " fp16=" << cfg_.enable_fp16 << " int8=" << cfg_.enable_int8
            << " dual_ctx=" << cfg_.enable_dual_context << " cuda_graph=" << cfg_.enable_cuda_graph
            << " input=" << cfg_.channels << "x" << cfg_.height << "x" << cfg_.width
            << " feature_dim=" << cfg_.feature_dim << " opt=(" << cfg_.opt_min << ","
            << cfg_.opt_avg << "," << cfg_.opt_max << ")\n";

  runtime_ = nvinfer1::createInferRuntime(trt_logger());
  if (!runtime_) {
    std::cerr << "createInferRuntime failed\n";
    std::exit(1);
  }

  build_or_load_engine_();

  n_contexts_ = cfg_.enable_dual_context ? 2 : 1;
  for (int i = 0; i < n_contexts_; ++i) {
    context_[i] = engine_->createExecutionContext();
    if (!context_[i]) {
      std::cerr << "createExecutionContext failed (ctx " << i << ")\n";
      std::exit(1);
    }
  }

  if (cfg_.enable_dual_context) {
    owns_streams_ = true;
    for (int i = 0; i < n_contexts_; ++i) {
      if (cudaStreamCreateWithFlags(&streams_[i], cudaStreamNonBlocking) != cudaSuccess) {
        std::cerr << "cudaStreamCreateWithFlags failed (stream " << i << ")\n";
        std::exit(1);
      }
    }
  } else {
    owns_streams_ = false;
    streams_[0] = inf_stream_;
    streams_[1] = nullptr;
  }

  const auto in_dtype = engine_->getTensorDataType(cfg_.input_name.c_str());
  if (cfg_.enable_int8) {
    if (in_dtype != nvinfer1::DataType::kINT8) {
      std::cerr << "TrtRunner: enable_int8 requires input '" << cfg_.input_name
                << "' to be INT8; re-export with export_resnet_onnx.py --input-dtype int8 "
                   "and delete any stale *.engine\n";
      std::exit(1);
    }
  } else if (in_dtype != nvinfer1::DataType::kHALF) {
    std::cerr << "TrtRunner: input tensor '" << cfg_.input_name
              << "' is not FP16 (kHALF); re-export with export_resnet_onnx.py "
                 "(fp16 input + in-model norm) and delete any stale *.engine\n";
    std::exit(1);
  }

  allocate_buffers_();
  std::cerr << "TrtRunner ready (n_contexts=" << n_contexts_ << ")\n";
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
    if (cfg_.enable_fp16) {
      config->setFlag(nvinfer1::BuilderFlag::kFP16);
    }

    std::unique_ptr<Int8EntropyCalibrator> calibrator;
    if (cfg_.enable_int8) {
      if (!builder->platformHasFastInt8()) {
        std::cerr << "TrtRunner: platformHasFastInt8() is false; INT8 build may be slow/fail\n";
      }
      config->setFlag(nvinfer1::BuilderFlag::kINT8);
      calibrator = std::make_unique<Int8EntropyCalibrator>(
          cfg_.input_name, cfg_.opt_avg, cfg_.channels, cfg_.height, cfg_.width,
          /*n_batches=*/64, cfg_.calib_cache_path);
      config->setInt8Calibrator(calibrator.get());
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
  const auto out_dtype = engine_->getTensorDataType(cfg_.output_name.c_str());
  // Primary contract: export_resnet_onnx.py ends with .float() → kFLOAT features.
  // kHALF is tolerated for cached engines built from older half-output ONNX.
  if (out_dtype == nvinfer1::DataType::kHALF) {
    out_is_half_ = true;
    out_elem_bytes_ = sizeof(__half);
  } else if (out_dtype == nvinfer1::DataType::kFLOAT) {
    out_is_half_ = false;
    out_elem_bytes_ = sizeof(float);
  } else {
    std::cerr << "TrtRunner: output tensor '" << cfg_.output_name
              << "' must be FP32 (kFLOAT) or FP16 (kHALF); FeatureSink expects float features\n";
    std::exit(1);
  }

  const nvinfer1::Dims4 max_in{cfg_.opt_max, cfg_.channels, cfg_.height, cfg_.width};
  if (!context_[0]->setInputShape(cfg_.input_name.c_str(), max_in)) {
    std::cerr << "TrtRunner: setInputShape(opt_max=" << cfg_.opt_max << ") failed during alloc\n";
    std::exit(1);
  }
  const nvinfer1::Dims out_dims = context_[0]->getTensorShape(cfg_.output_name.c_str());
  size_t out_elems = 1;
  for (int i = 0; i < out_dims.nbDims; ++i) {
    if (out_dims.d[i] <= 0) {
      std::cerr << "TrtRunner: output '" << cfg_.output_name << "' dim[" << i
                << "]=" << out_dims.d[i] << " still dynamic/invalid after setInputShape\n";
      std::exit(1);
    }
    out_elems *= static_cast<size_t>(out_dims.d[i]);
  }
  if (out_elems % static_cast<size_t>(cfg_.opt_max) != 0) {
    std::cerr << "TrtRunner: output volume " << out_elems
              << " is not divisible by opt_max=" << cfg_.opt_max << "\n";
    std::exit(1);
  }
  const size_t per_image = out_elems / static_cast<size_t>(cfg_.opt_max);
  if (static_cast<int>(per_image) != cfg_.feature_dim) {
    std::cerr << "TrtRunner: engine output feature dim " << per_image
              << " != configured feature_dim " << cfg_.feature_dim
              << " (fix inference.feature_dim or delete stale engine cache)\n";
    std::exit(1);
  }

  const size_t max_out_float_bytes = out_elems * sizeof(float);
  const size_t max_out_raw_bytes = out_elems * out_elem_bytes_;
  for (int i = 0; i < kBuffers; ++i) {
    // Always keep a float device buffer (sink conversion / FP32 engines).
    cudaMalloc(&trt_out_dev_[i], max_out_float_bytes);
    cudaMallocHost(&host_buf_[i], max_out_float_bytes);
    if (out_is_half_) {
      cudaMalloc(&trt_out_raw_[i], max_out_raw_bytes);
    } else {
      trt_out_raw_[i] = nullptr;
    }
    cudaEventCreate(&d2h_event_[i]);
    cudaEventCreate(&start_evt_[i]);
  }
  std::cerr << "TrtRunner buffers: 2x" << max_out_float_bytes
            << " bytes pinned host + GPU float output"
            << (out_is_half_ ? " + GPU half raw" : "") << " (engine [" << cfg_.opt_max << ","
            << per_image << "] " << (out_is_half_ ? "FP16" : "FP32") << ")\n";
}

void TrtRunner::ensure_cuda_graph_(uint32_t batch) {
  if (graph_ready_ || graph_capture_failed_) return;

  const size_t in_elem = cfg_.enable_int8 ? sizeof(int8_t) : sizeof(__half);
  graph_input_bytes_ = static_cast<size_t>(batch) * static_cast<size_t>(cfg_.channels) *
                       static_cast<size_t>(cfg_.height) * static_cast<size_t>(cfg_.width) *
                       in_elem;
  if (graph_input_bytes_ == 0) {
    std::cerr << "TrtRunner: ensure_cuda_graph_ got zero input bytes; disabling graphs\n";
    graph_capture_failed_ = true;
    return;
  }
  if (!graph_input_staging_ &&
      cudaMalloc(&graph_input_staging_, graph_input_bytes_) != cudaSuccess) {
    std::cerr << "TrtRunner: cudaMalloc graph_input_staging_ failed; disabling graphs\n";
    graph_capture_failed_ = true;
    return;
  }

  auto* ctx = context_[0];
  cudaStream_t stream = streams_[0];
  void* out_ptr = out_dev_ptr(out_is_half_, trt_out_raw_[0], trt_out_dev_[0]);

  const nvinfer1::Dims4 dims{static_cast<int>(batch), cfg_.channels, cfg_.height, cfg_.width};
  if (!ctx->setInputShape(cfg_.input_name.c_str(), dims)) {
    std::cerr << "TrtRunner: setInputShape failed during CUDA graph capture; disabling graphs\n";
    graph_capture_failed_ = true;
    return;
  }
  ctx->setTensorAddress(cfg_.input_name.c_str(), graph_input_staging_);
  ctx->setTensorAddress(cfg_.output_name.c_str(), out_ptr);

  // Warm-up enqueue outside capture (Myelin often fails on cold capture).
  if (!ctx->enqueueV3(stream) || cudaStreamSynchronize(stream) != cudaSuccess) {
    std::cerr << "TrtRunner: CUDA graph warm-up enqueue failed; disabling graphs\n";
    graph_capture_failed_ = true;
    return;
  }

  // Capture enqueue only; D2H stays outside the graph so host_buf_ parity works.
  if (cudaStreamBeginCapture(stream, cudaStreamCaptureModeThreadLocal) != cudaSuccess) {
    std::cerr << "TrtRunner: cudaStreamBeginCapture failed; disabling graphs\n";
    graph_capture_failed_ = true;
    return;
  }

  if (!ctx->enqueueV3(stream)) {
    std::cerr << "TrtRunner: enqueueV3 failed during CUDA graph capture; disabling graphs\n";
    cudaGraph_t aborted = nullptr;
    cudaStreamEndCapture(stream, &aborted);
    if (aborted) cudaGraphDestroy(aborted);
    graph_capture_failed_ = true;
    return;
  }

  if (cudaStreamEndCapture(stream, &graph_) != cudaSuccess || graph_ == nullptr) {
    std::cerr << "TrtRunner: cudaStreamEndCapture failed; disabling graphs\n";
    graph_capture_failed_ = true;
    return;
  }
  if (cudaGraphInstantiate(&graph_exec_, graph_, nullptr, nullptr, 0) != cudaSuccess) {
    std::cerr << "TrtRunner: cudaGraphInstantiate failed; disabling graphs\n";
    cudaGraphDestroy(graph_);
    graph_ = nullptr;
    graph_capture_failed_ = true;
    return;
  }

  graph_ready_ = true;
  std::cerr << "TrtRunner: CUDA graph captured for batch=" << batch
            << " input_bytes=" << graph_input_bytes_ << "\n";
}

void TrtRunner::convert_half_out_to_float_(int buf, uint32_t n) {
  const size_t elems = static_cast<size_t>(n) * static_cast<size_t>(cfg_.feature_dim);
  // host_buf_ holds packed __half from D2H; convert in-place via a temp copy.
  std::vector<__half> tmp(elems);
  std::memcpy(tmp.data(), host_buf_[buf], elems * sizeof(__half));
  for (size_t i = 0; i < elems; ++i) {
    host_buf_[buf][i] = __half2float(tmp[i]);
  }
}

bool TrtRunner::infer(void* dev_input, uint32_t batch, cudaEvent_t input_ready,
                      cudaEvent_t release_evt, float*& host_out_prev, uint32_t& host_out_prev_n) {
  host_out_prev = nullptr;
  host_out_prev_n = 0;

  if (batch == 0 || dev_input == nullptr) {
    std::cerr << "TrtRunner::infer called with batch=" << batch
              << " dev_input=" << static_cast<void*>(dev_input) << "\n";
    return false;
  }
  if (batch > static_cast<uint32_t>(cfg_.opt_max)) {
    std::cerr << "TrtRunner::infer batch=" << batch << " > opt_max=" << cfg_.opt_max
              << "; clamping\n";
    batch = static_cast<uint32_t>(cfg_.opt_max);
  }

  const int buf = parity_;
  const int ctx_i = parity_ % n_contexts_;
  auto* ctx = context_[ctx_i];
  cudaStream_t stream = streams_[ctx_i];

  if (input_ready != nullptr) {
    cudaStreamWaitEvent(stream, input_ready, 0);
  }

  cudaEventRecord(start_evt_[buf], stream);

  void* out_ptr = out_dev_ptr(out_is_half_, trt_out_raw_[buf], trt_out_dev_[buf]);
  const size_t out_bytes =
      static_cast<size_t>(batch) * static_cast<size_t>(cfg_.feature_dim) * out_elem_bytes_;

  // Graph is captured against context_0 / out buffer 0. Only launch when buf==0.
  const bool try_graph = cfg_.enable_cuda_graph && !graph_capture_failed_ && buf == 0 &&
                         batch == static_cast<uint32_t>(cfg_.opt_avg);

  bool used_graph = false;
  if (try_graph) {
    ensure_cuda_graph_(batch);
    if (graph_ready_) {
      cudaMemcpyAsync(graph_input_staging_, dev_input, graph_input_bytes_,
                      cudaMemcpyDeviceToDevice, stream);
      if (cudaGraphLaunch(graph_exec_, stream) != cudaSuccess) {
        std::cerr << "cudaGraphLaunch failed; falling back to eager for this batch\n";
      } else {
        used_graph = true;
      }
    }
  }

  if (!used_graph) {
    const nvinfer1::Dims4 dims{static_cast<int>(batch), cfg_.channels, cfg_.height, cfg_.width};
    if (!ctx->setInputShape(cfg_.input_name.c_str(), dims)) {
      std::cerr << "setInputShape failed for batch=" << batch << "\n";
      return false;
    }
    ctx->setTensorAddress(cfg_.input_name.c_str(), dev_input);
    ctx->setTensorAddress(cfg_.output_name.c_str(), out_ptr);

    if (!ctx->enqueueV3(stream)) {
      std::cerr << "enqueueV3 failed\n";
      return false;
    }
  }

  cudaMemcpyAsync(host_buf_[buf], out_ptr, out_bytes, cudaMemcpyDeviceToHost, stream);

  cudaEventRecord(d2h_event_[buf], stream);

  if (release_evt != nullptr) {
    cudaEventRecord(release_evt, stream);
  }

  const int prev = 1 - parity_;
  if (has_pending_[prev]) {
    cudaEventSynchronize(d2h_event_[prev]);
    float ms = 0.0f;
    if (cudaEventElapsedTime(&ms, start_evt_[prev], d2h_event_[prev]) == cudaSuccess) {
      batch_latency_ms_.push_back(ms);
    }
    if (out_is_half_) {
      convert_half_out_to_float_(prev, pending_n_[prev]);
    }
    host_out_prev = host_buf_[prev];
    host_out_prev_n = pending_n_[prev];
    has_pending_[prev] = false;
  }

  has_pending_[parity_] = true;
  pending_n_[parity_] = batch;
  parity_ = prev;

  ++total_batches_inferred_;
  return true;
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
      if (out_is_half_) {
        convert_half_out_to_float_(i, pending_n_[i]);
      }
      host_out = host_buf_[i];
      host_out_n = pending_n_[i];
      has_pending_[i] = false;
      return;
    }
  }
}

}  // namespace daqiri::apps::resnet
