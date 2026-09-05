// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#include "external_batch_policy.h"

#include <algorithm>

namespace daqiri::ucx_gpu::detail {

BatchAdmission decide_batch_admission(std::uint32_t image_count,
                                      std::uint64_t available_credits) noexcept {
  const auto admitted =
      static_cast<std::uint32_t>(std::min<std::uint64_t>(image_count, available_credits));
  return {admitted, image_count - admitted};
}

bool SubmittedSequenceLedger::record(std::uint64_t first_sequence, std::uint32_t image_count,
                                     std::string& error) noexcept {
  return advance(first_sequence, image_count, true, error);
}

bool SubmittedSequenceLedger::record_drop(std::uint64_t first_sequence, std::uint32_t image_count,
                                          std::string& error) noexcept {
  return advance(first_sequence, image_count, false, error);
}

bool SubmittedSequenceLedger::advance(std::uint64_t first_sequence, std::uint32_t image_count,
                                      bool submitted, std::string& error) noexcept {
  if (image_count == 0 || image_count > kExternalBatchImages) {
    error = "external batch image count must be in [1, 16]";
    return false;
  }
  if (first_sequence > sequence_space_size_ ||
      image_count > sequence_space_size_ - first_sequence) {
    error = "external batch sequence range exceeds the fixed run";
    return false;
  }
  if (first_sequence < next_minimum_sequence_) {
    error = "external batch sequence range regressed or overlaps";
    return false;
  }
  first_sequence += image_count;
  next_minimum_sequence_ = first_sequence;
  if (submitted) {
    submitted_images_ += image_count;
  }
  return true;
}

}  // namespace daqiri::ucx_gpu::detail
