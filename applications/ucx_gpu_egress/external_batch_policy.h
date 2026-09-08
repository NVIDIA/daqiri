// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "protocol.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace daqiri::ucx_gpu::detail {

inline constexpr std::uint32_t kExternalBatchImages =
    static_cast<std::uint32_t>(ucx_example::geometry::kImagesPerBatch);
inline constexpr std::size_t kExternalBatchBytes = ucx_example::geometry::kBatchBytes;
static_assert(kExternalBatchBytes == kExternalBatchImages * kImageBytes);

// Records the monotonically increasing, non-overlapping image ranges submitted
// by the processing thread. Gaps are allowed when native DAQIRI reorder output
// drops an incomplete input unit before it reaches UCP.
class SubmittedSequenceLedger {
 public:
  explicit SubmittedSequenceLedger(std::uint64_t sequence_space_size)
      : sequence_space_size_(sequence_space_size) {}

  bool record(std::uint64_t first_sequence, std::uint32_t image_count, std::string& error) noexcept;
  std::uint64_t submitted_images() const noexcept {
    return submitted_images_;
  }

 private:
  std::uint64_t sequence_space_size_{0};
  std::uint64_t next_minimum_sequence_{0};
  std::uint64_t submitted_images_{0};
};

}  // namespace daqiri::ucx_gpu::detail
