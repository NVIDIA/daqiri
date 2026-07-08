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
BenchWorkload parse_workload(int argc, char** argv);

// Parse "--workload-gemm-dim N" from argv: the square GEMM side length n, held
// FIXED so the FLOP count per call (2*n^3) is constant as the I/O unit (message /
// burst window) is swept -- this isolates pipelining depth from problem size in
// the RoCE-vs-raw comparison. The compute working set is n*n*elem_size, derived
// entirely from n; the assembled input buffer must be at least that large.
// Returns 1024 (the default) if unset. Mirrors parse_workload's stride.
int parse_workload_gemm_dim(int argc, char** argv);

// Parse "--workload-sync-interval N" from argv: drain the GPU stream every N compute
// calls (bounds outstanding GPU work). Larger N = deeper async queue, fewer CPU
// stalls waiting on the GPU; N=1 is fully synchronous. Used to characterize how much
// the single-threaded receive+compute loop is limited by sync stalls. Returns 2 (the
// default) if unset. Mirrors parse_workload's stride.
int parse_workload_sync_interval(int argc, char** argv);

// Lower-case name ("none"/"fft"/"gemm"); used for the run_spark_bench.sh
// post_process CSV column and log lines.
const char* workload_name(BenchWorkload workload);

// Engine-agnostic representative GPU compute, run once per received burst on the
// ACTUAL received packet data.
//
// The caller hands run() a contiguous device buffer holding the reordered /
// gathered payload of one burst (see ReorderPipeline in bench_pipeline.h). The
// op reads that buffer as its input operand: FFT transforms it in place of the
// scratch input; GEMM uses it as the A matrix. The bytes are arbitrary packet
// data, which is fine for a throughput benchmark — the FLOP profile and memory
// footprint are unchanged; only the input source differs. This makes the
// measurement an honest end-to-end "receive then process the data" cost.
//
// Owns its own CUDA stream, cuFFT plan, and cuBLAS handle. cuFFT plans and
// cuBLAS handles are not safe to share across threads, so construct one
// GpuWorkload per RX worker thread (each multi-queue RX thread gets its own).
// The stream is shared with the ReorderPipeline so the reorder/gather kernel and
// the workload are serialized on the same stream without an explicit sync.
class GpuWorkload {
 public:
  GpuWorkload() = default;
  ~GpuWorkload();
  GpuWorkload(const GpuWorkload&) = delete;
  GpuWorkload& operator=(const GpuWorkload&) = delete;

  // Build the plan/handle for `kind`. `input_bytes` is the size of the contiguous
  // buffer the caller will pass to run() (its reorder window / message size, 0 =>
  // an internal default); it bounds the op's read and is validated against the
  // pinned GEMM working set. `gemm_dim` is the square GEMM side length n, held
  // fixed so 2*n^3 FLOPs/call stays constant across the I/O sweep (>0 required for
  // GEMM; <=0 falls back to the 1024 default). kind == None leaves the object an
  // inert no-op. sync_interval bounds outstanding GPU work (sync every N runs) for
  // the run()+sync() callers (DPDK/socket); the RoCE path bounds work with events
  // instead and ignores it. Logs the chosen problem shape (GEMM n / FFT
  // length+batch) and FLOP count so every published benchmark number carries its
  // explicit compute size. Returns false on CUDA / library error; the caller may
  // warn and continue with the workload disabled (enabled() will report false).
  bool init(BenchWorkload kind, size_t input_bytes, int sync_interval = 2, int gemm_dim = 1024);

  // Enqueue one representative FFT/SGEMM on the internal stream, reading `input`
  // (a device pointer to >= the GEMM working set of valid bytes). No-op unless
  // enabled().
  void run(const void* input);

  // Every sync_interval runs, block until the stream drains so the GPU stays on
  // the critical path without unbounded queueing.
  void maybe_sync();

  // Drain any remaining queued work (call once on shutdown).
  void sync();

  bool enabled() const {
    return kind_ != BenchWorkload::None && ok_;
  }
  BenchWorkload kind() const {
    return kind_;
  }

  // CUDA stream (cudaStream_t) this workload runs on; share it with the
  // ReorderPipeline so the reorder/gather kernel orders before run(). null when
  // the workload is disabled.
  void* stream() const {
    return stream_;
  }

 private:
  void destroy();
  // Enqueue one op on the stream reading `input`; returns false if the
  // cuFFT/cuBLAS call reports an error at enqueue. Shared by run() (hot path)
  // and init() (warmup/validate, against an internal zeroed buffer).
  bool issue_op(const void* input);

  BenchWorkload kind_ = BenchWorkload::None;
  bool ok_ = false;
  int sync_interval_ = 16;
  unsigned long long run_count_ = 0;

  // Opaque handles, cast in the .cu so this header stays free of CUDA library
  // includes (it is included from plain .cpp bench mains).
  void* stream_ = nullptr;  // cudaStream_t
  void* cublas_ = nullptr;  // cublasHandle_t
  int fft_plan_ = -1;       // cufftHandle (-1 == unset)

  // Device scratch (operands NOT sourced from received data).
  void* fft_out_ = nullptr;  // cufftComplex[fft_total_] (FFT output)
  void* gemm_b_ = nullptr;   // B operand
  void* gemm_c_ = nullptr;   // C output
  int gemm_n_ = 0;
};

}  // namespace daqiri::bench
