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

struct BatchAdmission {
  std::uint32_t admitted{0};
  std::uint32_t dropped{0};
};

BatchAdmission decide_batch_admission(std::uint32_t image_count,
                                      std::uint64_t available_credits) noexcept;

// Records the monotonically increasing, non-overlapping image ranges submitted
// by the processing thread. Gaps are allowed because an upstream assembler may
// reject complete batches before they reach UCP.
class SubmittedSequenceLedger {
 public:
  explicit SubmittedSequenceLedger(std::uint64_t sequence_space_size)
      : sequence_space_size_(sequence_space_size) {}

  bool record(std::uint64_t first_sequence, std::uint32_t image_count, std::string& error) noexcept;
  bool record_drop(std::uint64_t first_sequence, std::uint32_t image_count,
                   std::string& error) noexcept;

  std::uint64_t submitted_images() const noexcept {
    return submitted_images_;
  }
  std::uint64_t dropped_before_submit() const noexcept {
    return sequence_space_size_ - submitted_images_;
  }

 private:
  bool advance(std::uint64_t first_sequence, std::uint32_t image_count, bool submitted,
               std::string& error) noexcept;

  std::uint64_t sequence_space_size_{0};
  std::uint64_t next_minimum_sequence_{0};
  std::uint64_t submitted_images_{0};
};

}  // namespace daqiri::ucx_gpu::detail
