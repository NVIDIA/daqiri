// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cuda_runtime_api.h>

#include <cstddef>
#include <cstdint>

#include "raw/dqri_header.h"

namespace daqiri::ucx_example {

// Source is the CUDA-visible alias of a validated 8,192-byte payload. For
// mapped-host DAQIRI buffers this must be the device alias, not an arbitrary
// pageable host pointer. Descriptors themselves reside in device memory.
struct FragmentPlacement {
  const std::uint8_t* source = nullptr;
  std::uint16_t fragment_slot = 0;
  std::uint16_t reserved = 0;
  std::uint32_t payload_length = kFragmentPayloadBytes;
};

static_assert(sizeof(FragmentPlacement) == 16);

// Precondition: destination_slot is a CUDA-visible mapped-host or device pointer;
// placements_device contains at most 256 CPU-validated, unique fragment slots;
// every source is readable by the selected CUDA device for 8,192 bytes. The
// descriptor array and source buffers must outlive completion on stream.
cudaError_t place_fragments_async(void* destination_slot,
                                  const FragmentPlacement* placements_device,
                                  std::size_t placement_count, cudaStream_t stream) noexcept;

// Test/debug helper used before slot reuse. The destination may be a mapped-host
// device alias or a cudaMalloc allocation.
cudaError_t poison_batch_slot_async(void* destination_slot, std::uint8_t poison,
                                    cudaStream_t stream) noexcept;

}  // namespace daqiri::ucx_example
