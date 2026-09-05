// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "raw/image_assembler.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <vector>

namespace raw = daqiri::ucx_example;

namespace {

int failures = 0;
constexpr std::uint64_t kEpoch = 0x123456789abcdef0ull;

#define CHECK(condition)                                                                    \
  do {                                                                                      \
    if (!(condition)) {                                                                     \
      std::cerr << __FILE__ << ':' << __LINE__ << ": check failed: " << #condition << '\n'; \
      ++failures;                                                                           \
    }                                                                                       \
  } while (false)

raw::FragmentInput fragment(std::uint32_t batch, std::uint16_t slot) {
  raw::FragmentInput result;
  result.header = raw::make_dqri_header(batch, kEpoch, slot);
  result.payload_device = reinterpret_cast<const std::uint8_t*>(
      0x100000ull + static_cast<std::uintptr_t>(slot) * raw::kFragmentPayloadBytes);
  return result;
}

raw::SlotReservation ring_slot(std::uint32_t index, std::uint64_t generation = 1) {
  raw::SlotReservation result;
  result.slot_index = index;
  result.generation = generation;
  result.destination_device = reinterpret_cast<void*>(
      0x10000000ull + static_cast<std::uintptr_t>(index) * raw::kBatchBytes);
  return result;
}

std::vector<raw::FragmentInput> full_batch(std::uint32_t batch) {
  std::vector<raw::FragmentInput> result;
  result.reserve(raw::kFragmentsPerBatch);
  for (std::size_t order = 0; order < raw::kFragmentsPerBatch; ++order) {
    const std::uint16_t slot = static_cast<std::uint16_t>((order * 73) % raw::kFragmentsPerBatch);
    result.push_back(fragment(batch, slot));
  }
  return result;
}

void test_start_mid_batch_and_reordering() {
  raw::ImageBatchAssembler assembler({kEpoch, 2'000'000});
  const raw::SlotReservation slot = ring_slot(3, 9);
  std::vector<raw::FragmentInput> middle = {fragment(100, 9), fragment(100, 8)};
  auto result = assembler.consume_burst(1, 100, middle.data(), middle.size(), &slot, 1);
  CHECK(result.lease_disposition == raw::BurstLeaseDisposition::kReleaseImmediately);
  CHECK(result.supplied_slots_consumed == 0);
  CHECK(!assembler.has_active_batch());

  auto complete = full_batch(100);
  result = assembler.consume_burst(2, 200, complete.data(), complete.size(), &slot, 1);
  CHECK(result.lease_disposition == raw::BurstLeaseDisposition::kReleaseAfterPlacement);
  CHECK(result.supplied_slots_consumed == 1);
  CHECK(result.placement_operations.size() == 1);
  CHECK(result.placement_operations[0].fragments.size() == raw::kFragmentsPerBatch);
  CHECK(result.placement_operations[0].completed_batch.has_value());
  CHECK(result.placement_operations[0].completed_batch->slot.slot_index == 3);
  CHECK(result.placement_operations[0].completed_batch->slot.generation == 9);
  CHECK(!assembler.has_active_batch());
  for (bool valid : result.placement_operations[0].completed_batch->image_valid) {
    CHECK(valid);
  }
}

void test_duplicate_rejects_without_using_current_burst() {
  raw::ImageBatchAssembler assembler({kEpoch, 2'000'000});
  const raw::SlotReservation slot = ring_slot(0);
  std::vector<raw::FragmentInput> first = {fragment(1, 0), fragment(1, 1)};
  auto result = assembler.consume_burst(10, 10, first.data(), first.size(), &slot, 1);
  CHECK(result.placement_operations.size() == 1);
  CHECK(result.lease_disposition == raw::BurstLeaseDisposition::kReleaseAfterPlacement);

  std::vector<raw::FragmentInput> duplicate = {fragment(1, 1)};
  result = assembler.consume_burst(11, 20, duplicate.data(), duplicate.size(), nullptr, 0);
  CHECK(result.placement_operations.empty());
  CHECK(result.lease_disposition == raw::BurstLeaseDisposition::kReleaseImmediately);
  CHECK(result.rejected_batches.size() == 1);
  CHECK(result.rejected_batches[0].reason == raw::BatchRejectReason::kDuplicateFragment);
  CHECK(result.rejected_batches[0].batch.received_fragments == 2);
  CHECK(!assembler.has_active_batch());
}

void test_same_burst_rejection_cancels_unlaunched_placements() {
  raw::ImageBatchAssembler assembler({kEpoch, 2'000'000});
  const raw::SlotReservation slot = ring_slot(2);
  std::vector<raw::FragmentInput> fragments = {fragment(3, 0), fragment(3, 1), fragment(3, 1)};
  const auto result = assembler.consume_burst(12, 30, fragments.data(), fragments.size(), &slot, 1);
  CHECK(result.placement_operations.empty());
  CHECK(result.lease_disposition == raw::BurstLeaseDisposition::kReleaseImmediately);
  CHECK(result.rejected_batches.size() == 1);
  CHECK(result.rejected_batches[0].reason == raw::BatchRejectReason::kDuplicateFragment);
  CHECK(result.rejected_batches[0].batch.received_fragments == 2);
  CHECK(assembler.counters().fragments_placed == 0);
}

void test_loss_then_new_batch_never_mixes_slots() {
  raw::ImageBatchAssembler assembler({kEpoch, 2'000'000});
  const raw::SlotReservation old_slot = ring_slot(4, 1);
  std::vector<raw::FragmentInput> partial;
  for (std::uint16_t index = 0; index < 16; ++index) {
    partial.push_back(fragment(10, index));
  }
  auto result = assembler.consume_burst(20, 100, partial.data(), partial.size(), &old_slot, 1);
  CHECK(result.placement_operations.size() == 1);

  const raw::SlotReservation new_slot = ring_slot(5, 2);
  auto next = full_batch(11);
  result = assembler.consume_burst(21, 200, next.data(), next.size(), &new_slot, 1);
  CHECK(result.rejected_batches.size() == 1);
  CHECK(result.rejected_batches[0].reason == raw::BatchRejectReason::kNewerBatch);
  CHECK(result.rejected_batches[0].batch.slot.slot_index == 4);
  CHECK(result.rejected_batches[0].batch.image_valid[0]);
  for (std::size_t image = 1; image < raw::kImagesPerBatch; ++image) {
    CHECK(!result.rejected_batches[0].batch.image_valid[image]);
  }
  CHECK(result.placement_operations.size() == 1);
  CHECK(result.placement_operations[0].destination.slot_index == 5);
  CHECK(result.placement_operations[0].completed_batch.has_value());
  CHECK(result.placement_operations[0].completed_batch->source_batch_id == 11);
}

void test_transition_mid_batch_waits_for_next_fragment_zero() {
  raw::ImageBatchAssembler assembler({kEpoch, 2'000'000});
  const raw::SlotReservation old_slot = ring_slot(10);
  raw::FragmentInput old_start = fragment(20, 0);
  assembler.consume_burst(22, 100, &old_start, 1, &old_slot, 1);

  const raw::SlotReservation new_slot = ring_slot(11);
  raw::FragmentInput mid_new = fragment(21, 7);
  auto result = assembler.consume_burst(23, 200, &mid_new, 1, &new_slot, 1);
  CHECK(result.rejected_batches.size() == 1);
  CHECK(result.rejected_batches[0].reason == raw::BatchRejectReason::kNewerBatch);
  CHECK(result.supplied_slots_consumed == 0);
  CHECK(!assembler.has_active_batch());

  raw::FragmentInput new_start = fragment(21, 0);
  result = assembler.consume_burst(24, 300, &new_start, 1, &new_slot, 1);
  CHECK(result.supplied_slots_consumed == 1);
  CHECK(result.placement_operations.size() == 1);
  CHECK(result.placement_operations[0].destination.slot_index == 11);
  CHECK(assembler.has_active_batch());
}

void test_header_error_rejections() {
  const raw::HeaderStatus statuses[] = {
      raw::HeaderStatus::kBadHeaderCrc,
      raw::HeaderStatus::kUnexpectedSourceEpoch,
      raw::HeaderStatus::kBadFragmentCount,
  };
  const raw::BatchRejectReason reasons[] = {
      raw::BatchRejectReason::kHeaderCrc,
      raw::BatchRejectReason::kSourceEpoch,
      raw::BatchRejectReason::kSchema,
  };
  for (std::size_t test = 0; test < std::size(statuses); ++test) {
    raw::ImageBatchAssembler assembler({kEpoch, 2'000'000});
    const raw::SlotReservation slot = ring_slot(static_cast<std::uint32_t>(test));
    raw::FragmentInput start = fragment(30 + test, 0);
    auto result = assembler.consume_burst(30, 100, &start, 1, &slot, 1);
    CHECK(result.placement_operations.size() == 1);

    raw::FragmentInput bad;
    bad.header_status = statuses[test];
    result = assembler.consume_burst(31, 101, &bad, 1, nullptr, 0);
    CHECK(result.rejected_batches.size() == 1);
    CHECK(result.rejected_batches[0].reason == reasons[test]);
    CHECK(result.lease_disposition == raw::BurstLeaseDisposition::kReleaseImmediately);
  }
}

void test_timeout() {
  raw::ImageBatchAssembler assembler({kEpoch, 2'000'000});
  const raw::SlotReservation slot = ring_slot(1);
  raw::FragmentInput start = fragment(40, 0);
  assembler.consume_burst(40, 1'000, &start, 1, &slot, 1);
  CHECK(!assembler.flush_timeout(2'000'999).has_value());
  const auto rejected = assembler.flush_timeout(2'001'000);
  CHECK(rejected.has_value());
  CHECK(rejected->reason == raw::BatchRejectReason::kTimeout);
  CHECK(rejected->batch.received_fragments == 1);
  CHECK(!assembler.has_active_batch());
}

void test_uint32_wrap() {
  raw::ImageBatchAssembler assembler({kEpoch, 2'000'000});
  const raw::SlotReservation slots[] = {ring_slot(6), ring_slot(7)};
  raw::FragmentInput old_start = fragment(0xffffffffu, 0);
  auto result = assembler.consume_burst(50, 10, &old_start, 1, slots, 1);
  CHECK(result.placement_operations.size() == 1);

  raw::FragmentInput wrapped_start = fragment(0, 0);
  result = assembler.consume_burst(51, 20, &wrapped_start, 1, slots + 1, 1);
  CHECK(result.rejected_batches.size() == 1);
  CHECK(result.rejected_batches[0].reason == raw::BatchRejectReason::kNewerBatch);
  CHECK(assembler.has_active_batch());

  auto tail = full_batch(0);
  tail.erase(tail.begin());
  result = assembler.consume_burst(52, 30, tail.data(), tail.size(), nullptr, 0);
  CHECK(result.placement_operations.size() == 1);
  CHECK(result.placement_operations[0].completed_batch.has_value());
  CHECK(result.placement_operations[0].completed_batch->unwrapped_batch_id == 0x100000000ull);
}

void test_multiple_complete_batches_in_one_burst() {
  raw::ImageBatchAssembler assembler({kEpoch, 2'000'000});
  auto first = full_batch(70);
  auto second = full_batch(71);
  first.insert(first.end(), second.begin(), second.end());
  const raw::SlotReservation slots[] = {ring_slot(8), ring_slot(9)};
  const auto result =
      assembler.consume_burst(60, 100, first.data(), first.size(), slots, std::size(slots));
  CHECK(result.supplied_slots_consumed == 2);
  CHECK(result.placement_operations.size() == 2);
  CHECK(result.placement_operations[0].completed_batch.has_value());
  CHECK(result.placement_operations[1].completed_batch.has_value());
  CHECK(result.placement_operations[0].destination.slot_index == 8);
  CHECK(result.placement_operations[1].destination.slot_index == 9);
  CHECK(result.lease_disposition == raw::BurstLeaseDisposition::kReleaseAfterPlacement);
}

void test_no_slot_drops_whole_batch() {
  raw::ImageBatchAssembler assembler({kEpoch, 2'000'000});
  auto batch = full_batch(80);
  auto result = assembler.consume_burst(70, 100, batch.data(), batch.size(), nullptr, 0);
  CHECK(result.placement_operations.empty());
  CHECK(result.supplied_slots_consumed == 0);
  CHECK(result.lease_disposition == raw::BurstLeaseDisposition::kReleaseImmediately);
  CHECK(assembler.counters().batches_dropped_no_slot == 1);
  CHECK(assembler.accounted_batch_boundary() == 81);
  CHECK(!assembler.has_active_batch());
}

void test_wholly_missing_interior_batch_advances_boundary() {
  raw::ImageBatchAssembler assembler({kEpoch, 2'000'000});
  auto first = full_batch(0);
  auto third = full_batch(2);
  const raw::SlotReservation slots[] = {ring_slot(12), ring_slot(13)};

  auto result = assembler.consume_burst(80, 100, first.data(), first.size(), slots, 1);
  CHECK(result.placement_operations.size() == 1);
  CHECK(result.placement_operations[0].completed_batch.has_value());
  CHECK(assembler.accounted_batch_boundary() == 1);

  result = assembler.consume_burst(81, 200, third.data(), third.size(), slots + 1, 1);
  CHECK(result.placement_operations.size() == 1);
  CHECK(result.placement_operations[0].completed_batch.has_value());
  CHECK(result.placement_operations[0].completed_batch->unwrapped_batch_id == 2);
  CHECK(assembler.counters().batches_missing == 1);
  CHECK(assembler.accounted_batch_boundary() == 3);
}

}  // namespace

int main() {
  test_start_mid_batch_and_reordering();
  test_duplicate_rejects_without_using_current_burst();
  test_same_burst_rejection_cancels_unlaunched_placements();
  test_loss_then_new_batch_never_mixes_slots();
  test_transition_mid_batch_waits_for_next_fragment_zero();
  test_header_error_rejections();
  test_timeout();
  test_uint32_wrap();
  test_multiple_complete_batches_in_one_burst();
  test_no_slot_drops_whole_batch();
  test_wholly_missing_interior_batch_advances_boundary();

  if (failures != 0) {
    std::cerr << failures << " image assembler test(s) failed\n";
    return 1;
  }
  std::cout << "image assembler state-machine tests passed\n";
  return 0;
}
