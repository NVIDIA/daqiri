// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#include "external_batch_policy.h"

namespace daqiri::ucx_gpu::detail {

bool SubmittedSequenceLedger::record(std::uint64_t first_sequence, std::uint32_t image_count,
                                     std::string& error) noexcept {
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
  submitted_images_ += image_count;
  return true;
}

}  // namespace daqiri::ucx_gpu::detail
