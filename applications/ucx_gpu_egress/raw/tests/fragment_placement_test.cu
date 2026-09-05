// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "raw/fragment_placement.h"

#include <cuda_runtime_api.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <vector>

namespace dqri = daqiri::ucx_example;

namespace {

bool check_cuda(cudaError_t status, const char* expression, int line) {
  if (status == cudaSuccess) {
    return true;
  }
  std::cerr << "line " << line << ": " << expression << " failed: " << cudaGetErrorString(status)
            << '\n';
  return false;
}

#define CUDA_CHECK(expression)                              \
  do {                                                      \
    if (!check_cuda((expression), #expression, __LINE__)) { \
      return 1;                                             \
    }                                                       \
  } while (false)

std::uint8_t expected_byte(std::size_t fragment, std::size_t byte) {
  return static_cast<std::uint8_t>((fragment * 131 + byte * 17 + 23) & 0xff);
}

bool validate_complete(const std::uint8_t* destination) {
  for (std::size_t fragment = 0; fragment < dqri::kFragmentsPerBatch; ++fragment) {
    for (std::size_t byte = 0; byte < dqri::kFragmentPayloadBytes; ++byte) {
      const auto actual = destination[fragment * dqri::kFragmentPayloadBytes + byte];
      if (actual != expected_byte(fragment, byte)) {
        std::cerr << "mismatch at fragment " << fragment << " byte " << byte << ": got "
                  << static_cast<unsigned>(actual) << " expected "
                  << static_cast<unsigned>(expected_byte(fragment, byte)) << '\n';
        return false;
      }
    }
  }
  return true;
}

}  // namespace

int main() {
  cudaError_t flags_status = cudaSetDeviceFlags(cudaDeviceMapHost);
  if (flags_status != cudaSuccess && flags_status != cudaErrorSetOnActiveProcess) {
    CUDA_CHECK(flags_status);
  }
  CUDA_CHECK(cudaSetDevice(0));

  cudaStream_t stream = nullptr;
  CUDA_CHECK(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));

  std::uint8_t* source_host = nullptr;
  CUDA_CHECK(cudaHostAlloc(reinterpret_cast<void**>(&source_host), dqri::kBatchBytes,
                           cudaHostAllocMapped));
  std::uint8_t* source_device_alias = nullptr;
  CUDA_CHECK(
      cudaHostGetDevicePointer(reinterpret_cast<void**>(&source_device_alias), source_host, 0));
  for (std::size_t fragment = 0; fragment < dqri::kFragmentsPerBatch; ++fragment) {
    for (std::size_t byte = 0; byte < dqri::kFragmentPayloadBytes; ++byte) {
      source_host[fragment * dqri::kFragmentPayloadBytes + byte] = expected_byte(fragment, byte);
    }
  }

  std::vector<dqri::FragmentPlacement> placements(dqri::kFragmentsPerBatch);
  for (std::size_t order = 0; order < dqri::kFragmentsPerBatch; ++order) {
    const std::size_t fragment = (order * 73) % dqri::kFragmentsPerBatch;
    placements[order].source = source_device_alias + fragment * dqri::kFragmentPayloadBytes;
    placements[order].fragment_slot = static_cast<std::uint16_t>(fragment);
  }

  dqri::FragmentPlacement* placements_device = nullptr;
  CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&placements_device),
                        placements.size() * sizeof(placements.front())));
  CUDA_CHECK(cudaMemcpyAsync(placements_device, placements.data(),
                             placements.size() * sizeof(placements.front()), cudaMemcpyHostToDevice,
                             stream));

  std::uint8_t* mapped_destination_host = nullptr;
  CUDA_CHECK(cudaHostAlloc(reinterpret_cast<void**>(&mapped_destination_host), dqri::kBatchBytes,
                           cudaHostAllocMapped));
  std::uint8_t* mapped_destination_device = nullptr;
  CUDA_CHECK(cudaHostGetDevicePointer(reinterpret_cast<void**>(&mapped_destination_device),
                                      mapped_destination_host, 0));
  CUDA_CHECK(dqri::poison_batch_slot_async(mapped_destination_device, 0xa5, stream));
  CUDA_CHECK(dqri::place_fragments_async(mapped_destination_device, placements_device,
                                         placements.size(), stream));
  CUDA_CHECK(cudaStreamSynchronize(stream));
  if (!validate_complete(mapped_destination_host)) {
    return 1;
  }

  std::uint8_t* device_destination = nullptr;
  CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&device_destination), dqri::kBatchBytes));
  CUDA_CHECK(dqri::poison_batch_slot_async(device_destination, 0x5a, stream));
  CUDA_CHECK(dqri::place_fragments_async(device_destination, placements_device, placements.size(),
                                         stream));
  std::vector<std::uint8_t> device_result(dqri::kBatchBytes);
  CUDA_CHECK(cudaMemcpyAsync(device_result.data(), device_destination, device_result.size(),
                             cudaMemcpyDeviceToHost, stream));
  CUDA_CHECK(cudaStreamSynchronize(stream));
  if (!validate_complete(device_result.data())) {
    return 1;
  }

  constexpr std::uint8_t kPoison = 0xcd;
  constexpr std::size_t kPartialCount = 3;
  const std::size_t partial_indexes[kPartialCount] = {0, 127, 255};
  std::vector<dqri::FragmentPlacement> partial(kPartialCount);
  for (std::size_t i = 0; i < kPartialCount; ++i) {
    const std::size_t fragment = partial_indexes[i];
    partial[i].source = source_device_alias + fragment * dqri::kFragmentPayloadBytes;
    partial[i].fragment_slot = static_cast<std::uint16_t>(fragment);
  }
  CUDA_CHECK(cudaMemcpyAsync(placements_device, partial.data(),
                             partial.size() * sizeof(partial.front()), cudaMemcpyHostToDevice,
                             stream));
  CUDA_CHECK(dqri::poison_batch_slot_async(device_destination, kPoison, stream));
  CUDA_CHECK(
      dqri::place_fragments_async(device_destination, placements_device, partial.size(), stream));
  CUDA_CHECK(cudaMemcpyAsync(device_result.data(), device_destination, device_result.size(),
                             cudaMemcpyDeviceToHost, stream));
  CUDA_CHECK(cudaStreamSynchronize(stream));

  for (std::size_t fragment = 0; fragment < dqri::kFragmentsPerBatch; ++fragment) {
    const bool was_placed = std::find(std::begin(partial_indexes), std::end(partial_indexes),
                                      fragment) != std::end(partial_indexes);
    for (std::size_t byte = 0; byte < dqri::kFragmentPayloadBytes; ++byte) {
      const std::uint8_t expected = was_placed ? expected_byte(fragment, byte) : kPoison;
      if (device_result[fragment * dqri::kFragmentPayloadBytes + byte] != expected) {
        std::cerr << "poison/partial-placement mismatch at fragment " << fragment << " byte "
                  << byte << '\n';
        return 1;
      }
    }
  }

  CUDA_CHECK(dqri::place_fragments_async(device_destination, placements_device, 0, stream));
  if (dqri::place_fragments_async(nullptr, placements_device, 1, stream) != cudaErrorInvalidValue) {
    std::cerr << "null destination was not rejected\n";
    return 1;
  }
  if (dqri::place_fragments_async(device_destination, placements_device,
                                  dqri::kFragmentsPerBatch + 1, stream) != cudaErrorInvalidValue) {
    std::cerr << "oversized placement list was not rejected\n";
    return 1;
  }

  CUDA_CHECK(cudaFree(device_destination));
  CUDA_CHECK(cudaFreeHost(mapped_destination_host));
  CUDA_CHECK(cudaFree(placements_device));
  CUDA_CHECK(cudaFreeHost(source_host));
  CUDA_CHECK(cudaStreamDestroy(stream));

  std::cout << "mapped-host/device placement and slot poisoning tests passed\n";
  return 0;
}
