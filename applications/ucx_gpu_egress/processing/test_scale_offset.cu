// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#include "scale_offset.h"

#include <cuda_runtime.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using daqiri::ucx_gpu::kProcessingBatchImages;
using daqiri::ucx_gpu::kProcessingBatchPixels;
using daqiri::ucx_gpu::kProcessingImagePixels;

void check_cuda(cudaError_t status, const char* operation) {
  if (status != cudaSuccess) {
    throw std::runtime_error(std::string(operation) + ": " + cudaGetErrorString(status));
  }
}

std::uint16_t round_nearest_even_clamped(float value) {
  value = std::min(std::max(value, 0.0F), 65535.0F);
  const float lower_f = std::floor(value);
  std::uint32_t lower = static_cast<std::uint32_t>(lower_f);
  const float fraction = value - lower_f;
  if (fraction > 0.5F || (fraction == 0.5F && (lower & 1U) != 0)) {
    ++lower;
  }
  return static_cast<std::uint16_t>(std::min(lower, 65535U));
}

std::uint16_t cpu_golden(std::uint16_t input, float scale, float offset) {
  return round_nearest_even_clamped(std::fma(scale, static_cast<float>(input), offset));
}

void run_case(const char* name, const std::vector<std::uint16_t>& input, float scale, float offset,
              const std::vector<std::uint16_t>* explicit_expected = nullptr) {
  if (input.size() != kProcessingBatchPixels) {
    throw std::runtime_error(std::string(name) + ": wrong input size");
  }

  std::vector<std::uint16_t> expected(input.size());
  if (explicit_expected != nullptr) {
    expected = *explicit_expected;
  } else {
    for (std::size_t i = 0; i < input.size(); ++i) {
      expected[i] = cpu_golden(input[i], scale, offset);
    }
  }

  std::uint16_t* device_data = nullptr;
  cudaStream_t stream = nullptr;
  check_cuda(cudaMalloc(&device_data, input.size() * sizeof(input[0])), "cudaMalloc");
  check_cuda(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking),
             "cudaStreamCreateWithFlags");
  check_cuda(cudaMemcpyAsync(device_data, input.data(), input.size() * sizeof(input[0]),
                             cudaMemcpyHostToDevice, stream),
             "cudaMemcpyAsync(H2D)");
  check_cuda(daqiri::ucx_gpu::scale_offset_u16_batch_async(device_data, scale, offset, stream),
             "scale_offset_u16_batch_async");

  std::vector<std::uint16_t> actual(input.size());
  check_cuda(cudaMemcpyAsync(actual.data(), device_data, actual.size() * sizeof(actual[0]),
                             cudaMemcpyDeviceToHost, stream),
             "cudaMemcpyAsync(D2H)");
  check_cuda(cudaStreamSynchronize(stream), "cudaStreamSynchronize");

  for (std::size_t i = 0; i < actual.size(); ++i) {
    if (actual[i] != expected[i]) {
      throw std::runtime_error(std::string(name) + " mismatch at index " + std::to_string(i) +
                               ": expected " + std::to_string(expected[i]) + ", got " +
                               std::to_string(actual[i]));
    }
  }

  check_cuda(cudaStreamDestroy(stream), "cudaStreamDestroy");
  check_cuda(cudaFree(device_data), "cudaFree");
  std::cout << name << " passed\n";
}

std::vector<std::uint16_t> patterned_input() {
  std::vector<std::uint16_t> input(kProcessingBatchPixels);
  for (std::size_t image = 0; image < kProcessingBatchImages; ++image) {
    for (std::size_t pixel = 0; pixel < kProcessingImagePixels; ++pixel) {
      input[image * kProcessingImagePixels + pixel] =
          static_cast<std::uint16_t>((image * 4093U + pixel * 17U) & 0xffffU);
    }
  }
  return input;
}

}  // namespace

int main() {
  try {
    check_cuda(cudaSetDevice(0), "cudaSetDevice");

    const auto pattern = patterned_input();
    run_case("identity_all_16_images", pattern, 1.0F, 0.0F);
    run_case("cpu_golden_all_16_images", pattern, 1.25F, -123.5F);
    run_case("negative_scale_all_16_images", pattern, -0.75F, 32768.0F);

    std::vector<std::uint16_t> boundaries(kProcessingBatchPixels, 0);
    std::vector<std::uint16_t> expected(kProcessingBatchPixels, 0);
    for (std::size_t image = 0; image < kProcessingBatchImages; ++image) {
      const std::size_t base = image * kProcessingImagePixels;
      boundaries[base + 0] = 0;
      boundaries[base + 1] = 65535;
      boundaries[base + 2] = 100;
      boundaries[base + 3] = 101;
      boundaries[base + 4] = 200;
      expected[base + 0] = 0;
      expected[base + 1] = 65535;
      expected[base + 2] = 199;
      expected[base + 3] = 201;
      expected[base + 4] = 399;
    }
    run_case("lower_and_upper_saturation", boundaries, 2.0F, -1.0F, &expected);

    std::fill(boundaries.begin(), boundaries.end(), 100);
    std::fill(expected.begin(), expected.end(), 100);
    for (std::size_t image = 0; image < kProcessingBatchImages; ++image) {
      const std::size_t base = image * kProcessingImagePixels;
      boundaries[base] = 100;
      boundaries[base + 1] = 101;
      expected[base] = 100;      // 100.5 ties to even 100.
      expected[base + 1] = 102;  // 101.5 ties to even 102.
    }
    run_case("ties_to_even_both_parities", boundaries, 1.0F, 0.5F, &expected);

    if (daqiri::ucx_gpu::scale_offset_u16_batch_async(nullptr, 1.0F, 0.0F, nullptr) !=
            cudaErrorInvalidValue ||
        daqiri::ucx_gpu::scale_offset_u16_batch_async(reinterpret_cast<std::uint16_t*>(1),
                                                      std::numeric_limits<float>::infinity(), 0.0F,
                                                      nullptr) != cudaErrorInvalidValue) {
      throw std::runtime_error("invalid argument checks failed");
    }

    std::cout << "all scale/offset tests passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "test failure: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
