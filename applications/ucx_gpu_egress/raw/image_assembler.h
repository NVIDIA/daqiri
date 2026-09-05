// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "raw/dqri_header.h"
#include "raw/fragment_placement.h"

namespace daqiri::ucx_example {

struct FragmentInput {
  HeaderStatus header_status = HeaderStatus::kOk;
  DqriHeader header{};
  const std::uint8_t* payload_device = nullptr;
};

// The RX owner supplies already-reserved producer-ring slots. The assembler
// consumes them in order only when fragment zero starts a new source batch.
struct SlotReservation {
  std::uint32_t slot_index = 0;
  std::uint64_t generation = 0;
  void* destination_device = nullptr;
};

struct BatchDescriptor {
  SlotReservation slot{};
  std::uint32_t source_batch_id = 0;
  std::uint64_t unwrapped_batch_id = 0;
  std::array<bool, kImagesPerBatch> image_valid{};
  std::size_t received_fragments = 0;
};

enum class BatchRejectReason {
  kDuplicateFragment,
  kNewerBatch,
  kHeaderCrc,
  kSourceEpoch,
  kSchema,
  kSerialAmbiguous,
  kSerialOutOfRange,
  kTimeout,
};

const char* batch_reject_reason_string(BatchRejectReason reason) noexcept;

struct RejectedBatch {
  BatchDescriptor batch{};
  BatchRejectReason reason = BatchRejectReason::kSchema;

  // At least fragment zero was previously submitted or may be submitted on the
  // placement stream. Record a discard event on that stream before returning
  // this slot to FREE.
  bool release_after_placement_stream = true;
};

struct PlacementOperation {
  SlotReservation destination{};
  std::vector<FragmentPlacement> fragments;

  // When set, record placement_done immediately after this operation and
  // publish the completed descriptor to the processing queue.
  std::optional<BatchDescriptor> completed_batch;
};

enum class BurstLeaseDisposition {
  kReleaseImmediately,
  kReleaseAfterPlacement,
};

struct BurstAssemblyResult {
  std::uint64_t burst_lease_id = 0;
  BurstLeaseDisposition lease_disposition = BurstLeaseDisposition::kReleaseImmediately;
  std::size_t supplied_slots_consumed = 0;
  std::vector<PlacementOperation> placement_operations;
  std::vector<RejectedBatch> rejected_batches;
};

struct AssemblerCounters {
  std::uint64_t bursts = 0;
  std::uint64_t fragments_seen = 0;
  std::uint64_t fragments_placed = 0;
  std::uint64_t fragments_older = 0;
  std::uint64_t fragments_start_mid_batch = 0;
  std::uint64_t malformed_headers = 0;
  std::uint64_t header_crc_errors = 0;
  std::uint64_t source_epoch_errors = 0;
  std::uint64_t schema_errors = 0;
  std::uint64_t duplicate_fragments = 0;
  std::uint64_t batches_started = 0;
  std::uint64_t batches_completed = 0;
  std::uint64_t batches_rejected = 0;
  std::uint64_t batches_rejected_transition = 0;
  std::uint64_t batches_rejected_timeout = 0;
  std::uint64_t batches_dropped_no_slot = 0;
  std::uint64_t batches_missing = 0;
};

struct ImageAssemblerOptions {
  std::uint64_t expected_source_epoch = 0;
  std::uint64_t assembly_timeout_ns = 2'000'000;
};

// CPU-only state machine. It never calls CUDA or DAQIRI. The RX owner executes
// placement_operations in order on its one placement stream, records completion
// and discard events where requested, and releases the burst according to
// lease_disposition.
class ImageBatchAssembler {
 public:
  explicit ImageBatchAssembler(ImageAssemblerOptions options) noexcept;

  BurstAssemblyResult consume_burst(std::uint64_t burst_lease_id, std::uint64_t now_ns,
                                    const FragmentInput* fragments, std::size_t fragment_count,
                                    const SlotReservation* available_slots,
                                    std::size_t available_slot_count);

  std::optional<RejectedBatch> flush_timeout(std::uint64_t now_ns);

  bool has_active_batch() const noexcept {
    return active_.has_value();
  }
  std::size_t active_fragment_count() const noexcept;
  // Exclusive unwrapped source-batch boundary accounted by the state machine.
  // With the fixed source starting at batch zero, jumps include wholly absent
  // interior batches and therefore expose their image sequences as gaps.
  std::uint64_t accounted_batch_boundary() const noexcept {
    return have_last_batch_ ? last_unwrapped_batch_id_ + 1 : 0;
  }
  const AssemblerCounters& counters() const noexcept {
    return counters_;
  }

 private:
  struct ActiveBatch {
    SlotReservation slot{};
    std::uint32_t source_batch_id = 0;
    std::uint64_t unwrapped_batch_id = 0;
    std::uint64_t start_time_ns = 0;
    FragmentBitmap received{};
    std::array<std::uint16_t, kImagesPerBatch> image_fragment_counts{};
  };

  HeaderStatus validate_fragment(const FragmentInput& fragment) const noexcept;
  BatchDescriptor snapshot_active() const noexcept;
  RejectedBatch reject_active(BatchRejectReason reason) noexcept;
  void remember_batch(std::uint32_t source_batch_id, std::uint64_t unwrapped_batch_id) noexcept;
  std::optional<std::uint64_t> unwrap_after_last(std::uint32_t source_batch_id) const noexcept;

  ImageAssemblerOptions options_{};
  AssemblerCounters counters_{};
  std::optional<ActiveBatch> active_;
  bool have_last_batch_ = false;
  std::uint32_t last_source_batch_id_ = 0;
  std::uint64_t last_unwrapped_batch_id_ = 0;
};

}  // namespace daqiri::ucx_example
