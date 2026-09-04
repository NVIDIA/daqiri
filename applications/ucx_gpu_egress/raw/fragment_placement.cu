// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "raw/fragment_placement.h"

#include <cstdint>

namespace daqiri::ucx_example {
namespace {

__global__ void place_fragments_kernel(std::uint8_t* destination_slot,
                                       const FragmentPlacement* placements,
                                       std::size_t placement_count) {
  const std::size_t placement_index = blockIdx.x;
  if (placement_index >= placement_count) {
    return;
  }

  const FragmentPlacement placement = placements[placement_index];
  if (placement.source == nullptr || placement.fragment_slot >= kFragmentsPerBatch ||
      placement.payload_length != kFragmentPayloadBytes || placement.reserved != 0) {
    return;
  }

  const std::uint8_t* source = placement.source;
  std::uint8_t* destination =
      destination_slot + static_cast<std::size_t>(placement.fragment_slot) * kFragmentPayloadBytes;

  const auto combined_alignment =
      reinterpret_cast<std::uintptr_t>(source) | reinterpret_cast<std::uintptr_t>(destination);
  if ((combined_alignment & (alignof(uint4) - 1)) == 0) {
    const auto* source_vectors = reinterpret_cast<const uint4*>(source);
    auto* destination_vectors = reinterpret_cast<uint4*>(destination);
    constexpr std::size_t kVectorCount = kFragmentPayloadBytes / sizeof(uint4);
    for (std::size_t index = threadIdx.x; index < kVectorCount; index += blockDim.x) {
      destination_vectors[index] = source_vectors[index];
    }
  } else {
    for (std::size_t index = threadIdx.x; index < kFragmentPayloadBytes; index += blockDim.x) {
      destination[index] = source[index];
    }
  }
}

}  // namespace

cudaError_t place_fragments_async(void* destination_slot,
                                  const FragmentPlacement* placements_device,
                                  std::size_t placement_count, cudaStream_t stream) noexcept {
  if (destination_slot == nullptr || (placement_count != 0 && placements_device == nullptr) ||
      placement_count > kFragmentsPerBatch) {
    return cudaErrorInvalidValue;
  }
  if (placement_count == 0) {
    return cudaSuccess;
  }
  place_fragments_kernel<<<static_cast<unsigned>(placement_count), 256, 0, stream>>>(
      static_cast<std::uint8_t*>(destination_slot), placements_device, placement_count);
  return cudaPeekAtLastError();
}

cudaError_t poison_batch_slot_async(void* destination_slot, std::uint8_t poison,
                                    cudaStream_t stream) noexcept {
  if (destination_slot == nullptr) {
    return cudaErrorInvalidValue;
  }
  return cudaMemsetAsync(destination_slot, poison, kBatchBytes, stream);
}

}  // namespace daqiri::ucx_example
