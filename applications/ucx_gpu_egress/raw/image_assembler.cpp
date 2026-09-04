// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "raw/image_assembler.h"

#include <cassert>
#include <utility>

namespace daqiri::ucx_example {
namespace {

BatchRejectReason reject_reason_for_header(HeaderStatus status) noexcept {
  if (status == HeaderStatus::kBadHeaderCrc) {
    return BatchRejectReason::kHeaderCrc;
  }
  if (status == HeaderStatus::kZeroSourceEpoch || status == HeaderStatus::kUnexpectedSourceEpoch) {
    return BatchRejectReason::kSourceEpoch;
  }
  return BatchRejectReason::kSchema;
}

}  // namespace

const char* batch_reject_reason_string(BatchRejectReason reason) noexcept {
  switch (reason) {
    case BatchRejectReason::kDuplicateFragment:
      return "duplicate fragment";
    case BatchRejectReason::kNewerBatch:
      return "newer batch before completion";
    case BatchRejectReason::kHeaderCrc:
      return "header CRC32C failure";
    case BatchRejectReason::kSourceEpoch:
      return "source epoch mismatch";
    case BatchRejectReason::kSchema:
      return "malformed or incompatible schema";
    case BatchRejectReason::kSerialAmbiguous:
      return "ambiguous 32-bit serial distance";
    case BatchRejectReason::kSerialOutOfRange:
      return "unwrapped batch ID is out of range";
    case BatchRejectReason::kTimeout:
      return "assembly timeout";
  }
  return "unknown rejection";
}

ImageBatchAssembler::ImageBatchAssembler(ImageAssemblerOptions options) noexcept
    : options_(options) {}

std::size_t ImageBatchAssembler::active_fragment_count() const noexcept {
  return active_ ? active_->received.count() : 0;
}

HeaderStatus ImageBatchAssembler::validate_fragment(const FragmentInput& fragment) const noexcept {
  if (fragment.header_status != HeaderStatus::kOk) {
    return fragment.header_status;
  }
  const DqriHeader& header = fragment.header;
  if (header.source_epoch == 0) {
    return HeaderStatus::kZeroSourceEpoch;
  }
  if (header.source_epoch != options_.expected_source_epoch) {
    return HeaderStatus::kUnexpectedSourceEpoch;
  }
  if (header.flags != 0 || header.payload_length != kFragmentPayloadBytes ||
      header.fragments_per_batch != kFragmentsPerBatch || fragment.payload_device == nullptr) {
    return HeaderStatus::kBadPayloadLength;
  }
  if (header.fragment_slot >= kFragmentsPerBatch) {
    return HeaderStatus::kFragmentOutOfRange;
  }
  const std::uint32_t expected_sequence =
      header.batch_id * static_cast<std::uint32_t>(kFragmentsPerBatch) + header.fragment_slot;
  if (header.packet_sequence != expected_sequence) {
    return HeaderStatus::kSequenceMismatch;
  }
  return HeaderStatus::kOk;
}

BatchDescriptor ImageBatchAssembler::snapshot_active() const noexcept {
  assert(active_);
  BatchDescriptor result;
  result.slot = active_->slot;
  result.source_batch_id = active_->source_batch_id;
  result.unwrapped_batch_id = active_->unwrapped_batch_id;
  result.received_fragments = active_->received.count();
  for (std::size_t image = 0; image < kImagesPerBatch; ++image) {
    result.image_valid[image] = active_->image_fragment_counts[image] == kFragmentsPerImage;
  }
  return result;
}

void ImageBatchAssembler::remember_batch(std::uint32_t source_batch_id,
                                         std::uint64_t unwrapped_batch_id) noexcept {
  const std::uint64_t expected = have_last_batch_ ? last_unwrapped_batch_id_ + 1 : 0;
  if (unwrapped_batch_id > expected) {
    counters_.batches_missing += unwrapped_batch_id - expected;
  }
  have_last_batch_ = true;
  last_source_batch_id_ = source_batch_id;
  last_unwrapped_batch_id_ = unwrapped_batch_id;
}

RejectedBatch ImageBatchAssembler::reject_active(BatchRejectReason reason) noexcept {
  assert(active_);
  RejectedBatch result;
  result.batch = snapshot_active();
  result.reason = reason;
  remember_batch(active_->source_batch_id, active_->unwrapped_batch_id);
  active_.reset();
  ++counters_.batches_rejected;
  if (reason == BatchRejectReason::kNewerBatch) {
    ++counters_.batches_rejected_transition;
  } else if (reason == BatchRejectReason::kTimeout) {
    ++counters_.batches_rejected_timeout;
  }
  return result;
}

std::optional<std::uint64_t> ImageBatchAssembler::unwrap_after_last(
    std::uint32_t source_batch_id) const noexcept {
  if (!have_last_batch_) {
    return static_cast<std::uint64_t>(source_batch_id);
  }
  return unwrap_serial32_near(source_batch_id, last_unwrapped_batch_id_);
}

std::optional<RejectedBatch> ImageBatchAssembler::flush_timeout(std::uint64_t now_ns) {
  if (!active_ || now_ns < active_->start_time_ns ||
      now_ns - active_->start_time_ns < options_.assembly_timeout_ns) {
    return std::nullopt;
  }
  return reject_active(BatchRejectReason::kTimeout);
}

BurstAssemblyResult ImageBatchAssembler::consume_burst(std::uint64_t burst_lease_id,
                                                       std::uint64_t now_ns,
                                                       const FragmentInput* fragments,
                                                       std::size_t fragment_count,
                                                       const SlotReservation* available_slots,
                                                       std::size_t available_slot_count) {
  BurstAssemblyResult result;
  result.burst_lease_id = burst_lease_id;
  ++counters_.bursts;

  if (auto timed_out = flush_timeout(now_ns)) {
    result.rejected_batches.push_back(std::move(*timed_out));
  }

  std::optional<std::size_t> active_operation_index;

  auto remove_active_operation = [&]() {
    if (!active_operation_index) {
      return;
    }
    assert(*active_operation_index + 1 == result.placement_operations.size());
    counters_.fragments_placed -= result.placement_operations.back().fragments.size();
    result.placement_operations.pop_back();
    active_operation_index.reset();
  };

  auto reject_current = [&](BatchRejectReason reason) {
    remove_active_operation();
    result.rejected_batches.push_back(reject_active(reason));
  };

  auto mark_dropped_batch = [&](const DqriHeader& header, std::uint64_t unwrapped) {
    remember_batch(header.batch_id, unwrapped);
    ++counters_.batches_dropped_no_slot;
  };

  for (std::size_t input_index = 0; input_index < fragment_count; ++input_index) {
    ++counters_.fragments_seen;
    const FragmentInput& fragment = fragments[input_index];
    const HeaderStatus validation_status = validate_fragment(fragment);
    if (validation_status != HeaderStatus::kOk) {
      ++counters_.malformed_headers;
      const BatchRejectReason reason = reject_reason_for_header(validation_status);
      if (reason == BatchRejectReason::kHeaderCrc) {
        ++counters_.header_crc_errors;
      } else if (reason == BatchRejectReason::kSourceEpoch) {
        ++counters_.source_epoch_errors;
      } else {
        ++counters_.schema_errors;
      }
      if (active_) {
        reject_current(reason);
      }
      continue;
    }

    const DqriHeader& header = fragment.header;
    if (active_) {
      const Serial32Relation relation = compare_serial32(header.batch_id, active_->source_batch_id);
      if (relation == Serial32Relation::kOlder) {
        ++counters_.fragments_older;
        continue;
      }
      if (relation == Serial32Relation::kAmbiguous) {
        ++counters_.schema_errors;
        reject_current(BatchRejectReason::kSerialAmbiguous);
        continue;
      }
      if (relation == Serial32Relation::kNewer) {
        reject_current(BatchRejectReason::kNewerBatch);
      }
    }

    if (!active_) {
      if (have_last_batch_) {
        const Serial32Relation relation = compare_serial32(header.batch_id, last_source_batch_id_);
        if (relation == Serial32Relation::kOlder || relation == Serial32Relation::kEqual) {
          ++counters_.fragments_older;
          continue;
        }
        if (relation == Serial32Relation::kAmbiguous) {
          ++counters_.schema_errors;
          continue;
        }
      }
      if (header.fragment_slot != 0) {
        ++counters_.fragments_start_mid_batch;
        continue;
      }

      const std::optional<std::uint64_t> unwrapped = unwrap_after_last(header.batch_id);
      if (!unwrapped) {
        ++counters_.schema_errors;
        continue;
      }
      if (result.supplied_slots_consumed >= available_slot_count || available_slots == nullptr ||
          available_slots[result.supplied_slots_consumed].destination_device == nullptr) {
        mark_dropped_batch(header, *unwrapped);
        continue;
      }

      ActiveBatch started;
      started.slot = available_slots[result.supplied_slots_consumed++];
      started.source_batch_id = header.batch_id;
      started.unwrapped_batch_id = *unwrapped;
      started.start_time_ns = now_ns;
      active_ = started;
      ++counters_.batches_started;
    }

    // A newer nonzero fragment rejected the old batch above but may not start
    // the new one. Equal is now guaranteed for any active batch.
    if (!active_ || header.batch_id != active_->source_batch_id) {
      ++counters_.fragments_start_mid_batch;
      continue;
    }

    const FragmentInsertResult insert_result = active_->received.insert(header.fragment_slot);
    if (insert_result == FragmentInsertResult::kDuplicate) {
      ++counters_.duplicate_fragments;
      reject_current(BatchRejectReason::kDuplicateFragment);
      continue;
    }
    if (insert_result == FragmentInsertResult::kOutOfRange) {
      ++counters_.schema_errors;
      reject_current(BatchRejectReason::kSchema);
      continue;
    }

    const std::size_t image = header.fragment_slot / kFragmentsPerImage;
    ++active_->image_fragment_counts[image];
    if (!active_operation_index) {
      PlacementOperation operation;
      operation.destination = active_->slot;
      operation.fragments.reserve(kFragmentsPerBatch);
      result.placement_operations.push_back(std::move(operation));
      active_operation_index = result.placement_operations.size() - 1;
    }
    FragmentPlacement placement;
    placement.source = fragment.payload_device;
    placement.fragment_slot = header.fragment_slot;
    result.placement_operations[*active_operation_index].fragments.push_back(placement);
    ++counters_.fragments_placed;

    if (active_->received.complete()) {
      BatchDescriptor completed = snapshot_active();
      for (bool valid : completed.image_valid) {
        assert(valid);
      }
      result.placement_operations[*active_operation_index].completed_batch = completed;
      remember_batch(active_->source_batch_id, active_->unwrapped_batch_id);
      active_.reset();
      active_operation_index.reset();
      ++counters_.batches_completed;
    }
  }

  if (!result.placement_operations.empty()) {
    result.lease_disposition = BurstLeaseDisposition::kReleaseAfterPlacement;
  }
  return result;
}

}  // namespace daqiri::ucx_example
