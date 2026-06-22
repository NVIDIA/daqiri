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

#include "bench_workload.h"

#include <cublas_v2.h>
#include <cuda_runtime.h>
#include <cufft.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>

namespace daqiri::bench {
namespace {

// 1D FFT length; the burst working set is fanned out across as many batched
// transforms of this length as fit.
constexpr int kFftLen = 1024;
// Default working-set size when the caller passes bytes_per_burst == 0.
constexpr size_t kDefaultBytes = 1u << 16; // 64 KiB

cufftHandle as_fft_plan(int p) { return static_cast<cufftHandle>(p); }
cudaStream_t as_stream(void *s) { return static_cast<cudaStream_t>(s); }
cublasHandle_t as_cublas(void *h) { return static_cast<cublasHandle_t>(h); }

} // namespace

BenchWorkload parse_workload(int argc, char **argv) {
  BenchWorkload workload = BenchWorkload::None;
  for (int i = 2; i + 1 < argc; i += 2) {
    if (std::string(argv[i]) == "--workload") {
      const std::string val = argv[i + 1];
      if (val == "fft") {
        workload = BenchWorkload::Fft;
      } else if (val == "gemm") {
        workload = BenchWorkload::Gemm;
      } else {
        workload = BenchWorkload::None;
      }
    }
  }
  return workload;
}

const char *workload_name(BenchWorkload workload) {
  switch (workload) {
  case BenchWorkload::Fft:
    return "fft";
  case BenchWorkload::Gemm:
    return "gemm";
  case BenchWorkload::None:
  default:
    return "none";
  }
}

GpuWorkload::~GpuWorkload() { destroy(); }

void GpuWorkload::destroy() {
  if (fft_plan_ >= 0) {
    cufftDestroy(as_fft_plan(fft_plan_));
    fft_plan_ = -1;
  }
  if (cublas_ != nullptr) {
    cublasDestroy(as_cublas(cublas_));
    cublas_ = nullptr;
  }
  if (fft_buf_ != nullptr) {
    cudaFree(fft_buf_);
    fft_buf_ = nullptr;
  }
  if (gemm_a_ != nullptr) {
    cudaFree(gemm_a_);
    gemm_a_ = nullptr;
  }
  if (gemm_b_ != nullptr) {
    cudaFree(gemm_b_);
    gemm_b_ = nullptr;
  }
  if (gemm_c_ != nullptr) {
    cudaFree(gemm_c_);
    gemm_c_ = nullptr;
  }
  if (stream_ != nullptr) {
    cudaStreamDestroy(as_stream(stream_));
    stream_ = nullptr;
  }
  ok_ = false;
}

bool GpuWorkload::init(BenchWorkload kind, size_t bytes_per_burst,
                       int sync_interval) {
  kind_ = kind;
  sync_interval_ = sync_interval > 0 ? sync_interval : 1;
  run_count_ = 0;
  if (kind_ == BenchWorkload::None) {
    ok_ = false;
    return true; // inert no-op object; not an error
  }

  const size_t bytes = bytes_per_burst > 0 ? bytes_per_burst : kDefaultBytes;

  cudaStream_t stream = nullptr;
  if (cudaStreamCreate(&stream) != cudaSuccess) {
    std::cerr << "GpuWorkload: cudaStreamCreate failed\n";
    destroy();
    return false;
  }
  stream_ = stream;

  if (kind_ == BenchWorkload::Fft) {
    // Fan the working set out across batched length-kFftLen C2C transforms.
    const size_t n_complex =
        std::max<size_t>(kFftLen, bytes / sizeof(cufftComplex));
    const int batch =
        std::max<int>(1, static_cast<int>(n_complex / kFftLen));
    const size_t total = static_cast<size_t>(kFftLen) * batch;

    if (cudaMalloc(&fft_buf_, total * sizeof(cufftComplex)) != cudaSuccess ||
        cudaMemset(fft_buf_, 0, total * sizeof(cufftComplex)) != cudaSuccess) {
      std::cerr << "GpuWorkload: FFT buffer alloc failed\n";
      destroy();
      return false;
    }
    cufftHandle plan = 0;
    if (cufftPlan1d(&plan, kFftLen, CUFFT_C2C, batch) != CUFFT_SUCCESS) {
      std::cerr << "GpuWorkload: cufftPlan1d failed\n";
      destroy();
      return false;
    }
    fft_plan_ = static_cast<int>(plan);
    if (cufftSetStream(as_fft_plan(fft_plan_), stream) != CUFFT_SUCCESS) {
      std::cerr << "GpuWorkload: cufftSetStream failed\n";
      destroy();
      return false;
    }
  } else { // Gemm
    // Square SGEMM whose three matrices roughly match the working set.
    int n = static_cast<int>(std::sqrt(static_cast<double>(bytes) /
                                        sizeof(float)));
    n = std::max(64, (n / 8) * 8); // multiple of 8, sane floor
    gemm_n_ = n;
    const size_t elems = static_cast<size_t>(n) * n;
    if (cudaMalloc(&gemm_a_, elems * sizeof(float)) != cudaSuccess ||
        cudaMalloc(&gemm_b_, elems * sizeof(float)) != cudaSuccess ||
        cudaMalloc(&gemm_c_, elems * sizeof(float)) != cudaSuccess ||
        cudaMemset(gemm_a_, 0, elems * sizeof(float)) != cudaSuccess ||
        cudaMemset(gemm_b_, 0, elems * sizeof(float)) != cudaSuccess) {
      std::cerr << "GpuWorkload: GEMM buffer alloc failed\n";
      destroy();
      return false;
    }
    cublasHandle_t handle = nullptr;
    if (cublasCreate(&handle) != CUBLAS_STATUS_SUCCESS) {
      std::cerr << "GpuWorkload: cublasCreate failed\n";
      destroy();
      return false;
    }
    cublas_ = handle;
    if (cublasSetStream(handle, stream) != CUBLAS_STATUS_SUCCESS) {
      std::cerr << "GpuWorkload: cublasSetStream failed\n";
      destroy();
      return false;
    }
  }

  ok_ = true;
  return true;
}

void GpuWorkload::run() {
  if (!enabled()) {
    return;
  }
  if (kind_ == BenchWorkload::Fft) {
    cufftExecC2C(as_fft_plan(fft_plan_),
                 static_cast<cufftComplex *>(fft_buf_),
                 static_cast<cufftComplex *>(fft_buf_), CUFFT_FORWARD);
  } else { // Gemm
    const float alpha = 1.0f;
    const float beta = 0.0f;
    cublasSgemm(as_cublas(cublas_), CUBLAS_OP_N, CUBLAS_OP_N, gemm_n_, gemm_n_,
                gemm_n_, &alpha, static_cast<const float *>(gemm_a_), gemm_n_,
                static_cast<const float *>(gemm_b_), gemm_n_, &beta,
                static_cast<float *>(gemm_c_), gemm_n_);
  }
  ++run_count_;
  maybe_sync();
}

void GpuWorkload::maybe_sync() {
  if (!enabled()) {
    return;
  }
  if (run_count_ % static_cast<unsigned long long>(sync_interval_) == 0) {
    cudaStreamSynchronize(as_stream(stream_));
  }
}

void GpuWorkload::sync() {
  if (stream_ != nullptr) {
    cudaStreamSynchronize(as_stream(stream_));
  }
}

} // namespace daqiri::bench
