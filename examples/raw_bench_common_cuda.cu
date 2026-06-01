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

#include "raw_bench_common.h"

#include <cuda_runtime.h>

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace daqiri::bench {
namespace {

constexpr size_t kInlineCopySize = 16;

struct MemcpyOp {
  uint8_t *dst = nullptr;
  const uint8_t *src = nullptr;
  size_t size = 0;
};

class DeviceMemcpyOps {
public:
  DeviceMemcpyOps() = default;
  DeviceMemcpyOps(const DeviceMemcpyOps &) = delete;
  DeviceMemcpyOps &operator=(const DeviceMemcpyOps &) = delete;

  ~DeviceMemcpyOps() {
    if (ptr_ != nullptr) {
      cudaFree(ptr_);
    }
  }

  cudaError_t reserve(size_t count) {
    if (count <= capacity_) {
      return cudaSuccess;
    }
    if (ptr_ != nullptr) {
      const cudaError_t free_status = cudaFree(ptr_);
      if (free_status != cudaSuccess) {
        ptr_ = nullptr;
        capacity_ = 0;
        return free_status;
      }
    }
    const cudaError_t alloc_status =
        cudaMalloc(reinterpret_cast<void **>(&ptr_), count * sizeof(MemcpyOp));
    if (alloc_status != cudaSuccess) {
      ptr_ = nullptr;
      capacity_ = 0;
      return alloc_status;
    }
    capacity_ = count;
    return cudaSuccess;
  }

  MemcpyOp *data() { return ptr_; }

private:
  MemcpyOp *ptr_ = nullptr;
  size_t capacity_ = 0;
};

__global__ void memcpy_batch_kernel(const MemcpyOp *ops, size_t count) {
  const size_t op_idx = static_cast<size_t>(blockIdx.x);
  if (op_idx >= count) {
    return;
  }

  __shared__ MemcpyOp op;
  if (threadIdx.x == 0) {
    op = ops[op_idx];
  }
  __syncthreads();

  for (size_t offset = static_cast<size_t>(threadIdx.x); offset < op.size;
       offset += static_cast<size_t>(blockDim.x)) {
    op.dst[offset] = op.src[offset];
  }
}

__global__ void memcpy_batch_inline_kernel(const MemcpyOp *ops, size_t count) {
  const size_t op_idx =
      (static_cast<size_t>(blockIdx.x) * blockDim.x) + threadIdx.x;
  if (op_idx >= count) {
    return;
  }

  const MemcpyOp op = ops[op_idx];
  for (size_t offset = 0; offset < op.size; ++offset) {
    op.dst[offset] = op.src[offset];
  }
}

cudaError_t memcpy_2d_async_if_strided(const std::vector<void *> &dsts,
                                       const std::vector<const void *> &srcs,
                                       const std::vector<size_t> &sizes,
                                       cudaStream_t stream, bool *handled) {
  *handled = false;
  if (dsts.empty()) {
    *handled = true;
    return cudaSuccess;
  }

  const size_t copy_size = sizes[0];
  if (copy_size == 0) {
    return cudaSuccess;
  }
  if (dsts[0] == nullptr || srcs[0] == nullptr) {
    return cudaErrorInvalidValue;
  }
  for (size_t i = 1; i < sizes.size(); ++i) {
    if (sizes[i] != copy_size || dsts[i] == nullptr || srcs[i] == nullptr) {
      return cudaSuccess;
    }
  }

  if (dsts.size() == 1) {
    *handled = true;
    return cudaMemcpyAsync(dsts[0], srcs[0], copy_size, cudaMemcpyDefault,
                           stream);
  }

  const auto dst_base = reinterpret_cast<uintptr_t>(dsts[0]);
  const auto src_base = reinterpret_cast<uintptr_t>(srcs[0]);
  const auto dst_next = reinterpret_cast<uintptr_t>(dsts[1]);
  const auto src_next = reinterpret_cast<uintptr_t>(srcs[1]);
  if (dst_next <= dst_base || src_next <= src_base) {
    return cudaSuccess;
  }

  const size_t dst_pitch = dst_next - dst_base;
  const size_t src_pitch = src_next - src_base;
  if (dst_pitch < copy_size || src_pitch < copy_size) {
    return cudaSuccess;
  }

  for (size_t i = 2; i < dsts.size(); ++i) {
    if (reinterpret_cast<uintptr_t>(dsts[i]) != dst_base + (i * dst_pitch) ||
        reinterpret_cast<uintptr_t>(srcs[i]) != src_base + (i * src_pitch)) {
      return cudaSuccess;
    }
  }

  *handled = true;
  return cudaMemcpy2DAsync(dsts[0], dst_pitch, srcs[0], src_pitch, copy_size,
                           dsts.size(), cudaMemcpyDefault, stream);
}

cudaError_t kernel_accessible_pointer(const void *ptr, const void **kernel_ptr,
                                      cudaMemoryType *memory_type = nullptr) {
  cudaPointerAttributes attr{};
  const cudaError_t attr_status = cudaPointerGetAttributes(&attr, ptr);
  if (attr_status != cudaSuccess) {
    return attr_status;
  }

  if (attr.type == cudaMemoryTypeDevice || attr.type == cudaMemoryTypeManaged) {
    *kernel_ptr = ptr;
    if (memory_type != nullptr) {
      *memory_type = attr.type;
    }
    return cudaSuccess;
  }

  if (attr.type == cudaMemoryTypeHost) {
    void *device_ptr = nullptr;
    const cudaError_t status =
        cudaHostGetDevicePointer(&device_ptr, const_cast<void *>(ptr), 0);
    if (status != cudaSuccess) {
      return status;
    }
    *kernel_ptr = device_ptr;
    if (memory_type != nullptr) {
      *memory_type = attr.type;
    }
    return cudaSuccess;
  }

  return cudaErrorInvalidValue;
}

} // namespace

cudaError_t memcpy_batch_async(const std::vector<void *> &dsts,
                               const std::vector<const void *> &srcs,
                               const std::vector<size_t> &sizes,
                               cudaStream_t stream) {
  if (dsts.size() != srcs.size() || dsts.size() != sizes.size()) {
    return cudaErrorInvalidValue;
  }

  bool handled = false;
  const cudaError_t copy_2d_status =
      memcpy_2d_async_if_strided(dsts, srcs, sizes, stream, &handled);
  if (handled || copy_2d_status != cudaSuccess) {
    return copy_2d_status;
  }

  std::vector<MemcpyOp> ops;
  ops.reserve(dsts.size());
  size_t max_size = 0;
  for (size_t i = 0; i < dsts.size(); ++i) {
    if (sizes[i] == 0) {
      continue;
    }
    if (dsts[i] == nullptr || srcs[i] == nullptr) {
      return cudaErrorInvalidValue;
    }

    MemcpyOp op{};
    const void *kernel_dst = nullptr;
    cudaError_t status = kernel_accessible_pointer(dsts[i], &kernel_dst);
    if (status != cudaSuccess) {
      return status;
    }
    op.dst = static_cast<uint8_t *>(const_cast<void *>(kernel_dst));
    op.size = sizes[i];

    const void *kernel_src = nullptr;
    status = kernel_accessible_pointer(srcs[i], &kernel_src);
    if (status != cudaSuccess) {
      return status;
    }
    op.src = static_cast<const uint8_t *>(kernel_src);

    ops.push_back(op);
    max_size = std::max(max_size, sizes[i]);
  }

  if (ops.empty()) {
    return cudaSuccess;
  }

  thread_local std::unordered_map<cudaStream_t, DeviceMemcpyOps>
      device_ops_by_stream;
  DeviceMemcpyOps &device_ops = device_ops_by_stream[stream];
  cudaError_t status = device_ops.reserve(ops.size());
  if (status != cudaSuccess) {
    return status;
  }

  status = cudaMemcpyAsync(device_ops.data(), ops.data(),
                           ops.size() * sizeof(MemcpyOp),
                           cudaMemcpyHostToDevice, stream);
  if (status == cudaSuccess) {
    constexpr int ops_per_block = 256;
    constexpr int threads_per_copy = 256;
    if (max_size <= kInlineCopySize) {
      const auto blocks = static_cast<unsigned int>(
          (ops.size() + ops_per_block - 1) / ops_per_block);
      memcpy_batch_inline_kernel<<<blocks, ops_per_block, 0, stream>>>(
          device_ops.data(), ops.size());
    } else {
      memcpy_batch_kernel<<<static_cast<unsigned int>(ops.size()),
                            threads_per_copy, 0, stream>>>(device_ops.data(),
                                                           ops.size());
    }
    status = cudaGetLastError();
  }
  return status;
}

} // namespace daqiri::bench
