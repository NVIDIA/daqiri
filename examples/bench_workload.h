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

#include <cstddef>

namespace daqiri::bench {

// Representative GPU workloads that can be dropped into any benchmark's receive
// path to model downstream GPU compute. Bare loopback (None) vs loopback + FFT
// vs loopback + GEMM. GemmFp16 is the same square matmul as Gemm but in
// mixed-precision (FP16 inputs, FP32 accumulate) on the tensor cores — the core
// op of GPU inference, and far faster than the FP32 path on tensor-core GPUs.
enum class BenchWorkload { None, Fft, Gemm, GemmFp16 };

// Parse "--workload none|fft|gemm|gemm_fp16" from argv (default None). Mirrors
// the flag/value stride used by parse_run_seconds / parse_target_gbps.
BenchWorkload parse_workload(int argc, char **argv);

// Lower-case name ("none"/"fft"/"gemm"); used for the run_spark_bench.sh
// post_process CSV column and log lines.
const char *workload_name(BenchWorkload workload);

// Engine-agnostic representative GPU compute, run once per received burst.
//
// The component deliberately operates on its OWN device scratch buffers, not
// the received packet bytes: this keeps it a true drop-in across every
// stream_type / engine (raw, HDS, RoCE, socket) without needing a payload
// device pointer (RoCE exposes none). It therefore measures the GPU-load
// headroom of the receive path, not a data transform.
//
// Owns its own CUDA stream, cuFFT plan, and cuBLAS handle. cuFFT plans and
// cuBLAS handles are not safe to share across threads, so construct one
// GpuWorkload per RX worker thread (each multi-queue RX thread gets its own).
class GpuWorkload {
public:
  GpuWorkload() = default;
  ~GpuWorkload();
  GpuWorkload(const GpuWorkload &) = delete;
  GpuWorkload &operator=(const GpuWorkload &) = delete;

  // Build the plan/handle and size the problem to ~bytes_per_burst of working
  // set (0 => an internal default). kind == None leaves the object an inert
  // no-op. sync_interval bounds outstanding GPU work (sync every N runs).
  // Returns false on CUDA / library error; the caller may warn and continue
  // with the workload disabled (enabled() will report false).
  bool init(BenchWorkload kind, size_t bytes_per_burst, int sync_interval = 2);

  // Enqueue one representative FFT/SGEMM on the internal stream. No-op unless
  // enabled().
  void run();

  // Every sync_interval runs, block until the stream drains so the GPU stays on
  // the critical path without unbounded queueing.
  void maybe_sync();

  // Drain any remaining queued work (call once on shutdown).
  void sync();

  bool enabled() const { return kind_ != BenchWorkload::None && ok_; }
  BenchWorkload kind() const { return kind_; }

private:
  void destroy();
  // Enqueue one op on the stream; returns false if the cuFFT/cuBLAS call reports
  // an error at enqueue. Shared by run() (hot path) and init() (warmup/validate).
  bool issue_op();

  BenchWorkload kind_ = BenchWorkload::None;
  bool ok_ = false;
  int sync_interval_ = 16;
  unsigned long long run_count_ = 0;

  // Opaque handles, cast in the .cu so this header stays free of CUDA library
  // includes (it is included from plain .cpp bench mains).
  void *stream_ = nullptr;   // cudaStream_t
  void *cublas_ = nullptr;   // cublasHandle_t
  int fft_plan_ = -1;        // cufftHandle (-1 == unset)

  // Device scratch.
  void *fft_buf_ = nullptr;  // cufftComplex[fft_total_]
  void *gemm_a_ = nullptr;   // float[n*n]   (Gemm) or __half[n*n] (GemmFp16)
  void *gemm_b_ = nullptr;
  void *gemm_c_ = nullptr;
  int gemm_n_ = 0;
};

} // namespace daqiri::bench
