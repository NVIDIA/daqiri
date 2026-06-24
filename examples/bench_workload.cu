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
#include <cuda_fp16.h>
#include <cuda_runtime.h>
#include <cufft.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace daqiri::bench {
namespace {

// 1D FFT length; the burst working set is fanned out across as many batched
// transforms of this length as fit.
constexpr int kFftLen = 1024;
// Default working-set size when the caller passes bytes_per_burst == 0.
constexpr size_t kDefaultBytes = 1u << 16;  // 64 KiB

cufftHandle as_fft_plan(int p) {
  return static_cast<cufftHandle>(p);
}
cudaStream_t as_stream(void* s) {
  return static_cast<cudaStream_t>(s);
}
cublasHandle_t as_cublas(void* h) {
  return static_cast<cublasHandle_t>(h);
}

}  // namespace

BenchWorkload parse_workload(int argc, char** argv) {
  BenchWorkload workload = BenchWorkload::None;
  for (int i = 2; i + 1 < argc; i += 2) {
    if (std::string(argv[i]) == "--workload") {
      const std::string val = argv[i + 1];
      if (val == "fft") {
        workload = BenchWorkload::Fft;
      } else if (val == "gemm") {
        workload = BenchWorkload::Gemm;
      } else if (val == "gemm_fp16" || val == "gemm-fp16") {
        workload = BenchWorkload::GemmFp16;
      } else if (val == "none") {
        workload = BenchWorkload::None;
      } else {
        std::cerr << "Unknown --workload value '" << val
                  << "' (expected none|fft|gemm|gemm_fp16); using none\n";
        workload = BenchWorkload::None;
      }
    }
  }
  return workload;
}

size_t parse_workload_batch_bytes(int argc, char** argv) {
  for (int i = 2; i + 1 < argc; i += 2) {
    if (std::string(argv[i]) == "--workload-batch-bytes") {
      const long long v = std::atoll(argv[i + 1]);
      if (v > 0) { return static_cast<size_t>(v); }
    }
  }
  return 0;
}

const char* workload_name(BenchWorkload workload) {
  switch (workload) {
    case BenchWorkload::Fft:
      return "fft";
    case BenchWorkload::Gemm:
      return "gemm";
    case BenchWorkload::GemmFp16:
      return "gemm_fp16";
    case BenchWorkload::None:
    default:
      return "none";
  }
}

GpuWorkload::~GpuWorkload() {
  destroy();
}

void GpuWorkload::destroy() {
  if (fft_plan_ >= 0) {
    cufftDestroy(as_fft_plan(fft_plan_));
    fft_plan_ = -1;
  }
  if (cublas_ != nullptr) {
    cublasDestroy(as_cublas(cublas_));
    cublas_ = nullptr;
  }
  if (fft_out_ != nullptr) {
    cudaFree(fft_out_);
    fft_out_ = nullptr;
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

bool GpuWorkload::init(BenchWorkload kind, size_t batch_bytes, int sync_interval) {
  kind_ = kind;
  sync_interval_ = sync_interval > 0 ? sync_interval : 1;
  run_count_ = 0;
  if (kind_ == BenchWorkload::None) {
    ok_ = false;
    return true;  // inert no-op object; not an error
  }

  const size_t bytes = batch_bytes > 0 ? batch_bytes : kDefaultBytes;

  cudaStream_t stream = nullptr;
  if (cudaStreamCreate(&stream) != cudaSuccess) {
    std::cerr << "GpuWorkload: cudaStreamCreate failed\n";
    destroy();
    return false;
  }
  stream_ = stream;

  if (kind_ == BenchWorkload::Fft) {
    // Fan the working set out across batched length-kFftLen C2C transforms. The
    // transform reads the caller's input buffer and writes the owned fft_out_;
    // total*sizeof(cufftComplex) <= bytes so the input always holds enough data.
    const size_t n_complex = std::max<size_t>(kFftLen, bytes / sizeof(cufftComplex));
    const int batch = std::max<int>(1, static_cast<int>(n_complex / kFftLen));
    const size_t total = static_cast<size_t>(kFftLen) * batch;

    if (cudaMalloc(&fft_out_, total * sizeof(cufftComplex)) != cudaSuccess ||
        cudaMemset(fft_out_, 0, total * sizeof(cufftComplex)) != cudaSuccess) {
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
  } else {  // Gemm or GemmFp16
    // Square matmul whose matrices match the working set. Size n from FP32 in
    // both cases so gemm and gemm_fp16 use the SAME dimension -> identical FLOP
    // count, isolating the precision / tensor-core effect. The A operand is the
    // caller's input buffer (n*n*elem_size <= bytes); B and C are owned scratch.
    int n = static_cast<int>(std::sqrt(static_cast<double>(bytes) / sizeof(float)));
    n = std::max(64, (n / 8) * 8);  // multiple of 8, sane floor
    gemm_n_ = n;
    const size_t elems = static_cast<size_t>(n) * n;
    const size_t elem_size = kind_ == BenchWorkload::GemmFp16 ? sizeof(__half) : sizeof(float);
    if (cudaMalloc(&gemm_b_, elems * elem_size) != cudaSuccess ||
        cudaMalloc(&gemm_c_, elems * elem_size) != cudaSuccess ||
        cudaMemset(gemm_b_, 0, elems * elem_size) != cudaSuccess) {
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

  // Warm up and validate the chosen op once against a transient zeroed input
  // buffer: this surfaces a misconfigured cuFFT/cuBLAS call (which would
  // otherwise no-op silently on the hot path and look like a free workload) and
  // excludes one-time library setup from the measured run. The warmup buffer is
  // freed immediately; the measured runs read the caller's received-data buffer.
  void* warmup_in = nullptr;
  const bool warmup_ok = cudaMalloc(&warmup_in, bytes) == cudaSuccess &&
                         cudaMemset(warmup_in, 0, bytes) == cudaSuccess && issue_op(warmup_in) &&
                         cudaStreamSynchronize(stream) == cudaSuccess;
  if (warmup_in != nullptr) {
    cudaFree(warmup_in);
  }
  if (!warmup_ok) {
    std::cerr << "GpuWorkload: warmup of '" << workload_name(kind_)
              << "' failed (cuda=" << cudaGetErrorString(cudaGetLastError()) << ")\n";
    destroy();
    return false;
  }

  ok_ = true;
  return true;
}

bool GpuWorkload::issue_op(const void* input) {
  if (kind_ == BenchWorkload::Fft) {
    return cufftExecC2C(as_fft_plan(fft_plan_),
                        const_cast<cufftComplex*>(static_cast<const cufftComplex*>(input)),
                        static_cast<cufftComplex*>(fft_out_), CUFFT_FORWARD) == CUFFT_SUCCESS;
  }
  const float alpha = 1.0f;
  const float beta = 0.0f;
  if (kind_ == BenchWorkload::Gemm) {
    return cublasSgemm(as_cublas(cublas_), CUBLAS_OP_N, CUBLAS_OP_N, gemm_n_, gemm_n_, gemm_n_,
                       &alpha, static_cast<const float*>(input), gemm_n_,
                       static_cast<const float*>(gemm_b_), gemm_n_, &beta,
                       static_cast<float*>(gemm_c_), gemm_n_) == CUBLAS_STATUS_SUCCESS;
  }
  // GemmFp16: FP16 inputs, FP32 accumulate, on the tensor cores.
  return cublasGemmEx(as_cublas(cublas_), CUBLAS_OP_N, CUBLAS_OP_N, gemm_n_, gemm_n_, gemm_n_,
                      &alpha, input, CUDA_R_16F, gemm_n_, gemm_b_, CUDA_R_16F, gemm_n_, &beta,
                      gemm_c_, CUDA_R_16F, gemm_n_, CUBLAS_COMPUTE_32F,
                      CUBLAS_GEMM_DEFAULT_TENSOR_OP) == CUBLAS_STATUS_SUCCESS;
}

void GpuWorkload::run(const void* input) {
  if (!enabled() || input == nullptr) {
    return;
  }
  issue_op(input);
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

}  // namespace daqiri::bench
