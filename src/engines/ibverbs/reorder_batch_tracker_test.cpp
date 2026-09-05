// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#include "reorder_batch_tracker.h"

#include <cstdlib>
#include <iostream>

namespace {

#define CHECK(condition)                                                                         \
  do {                                                                                           \
    if (!(condition)) {                                                                          \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
      return EXIT_FAILURE;                                                                       \
    }                                                                                            \
  } while (false)

int run() {
  using daqiri::ibverbs_detail::BatchRelation;
  using daqiri::ibverbs_detail::PacketDisposition;
  using daqiri::ibverbs_detail::ReorderBatchTracker;
  using daqiri::ibverbs_detail::compare_batch;

  CHECK(compare_batch(3, 3, 8) == BatchRelation::same);
  CHECK(compare_batch(0, 7, 8) == BatchRelation::newer);
  CHECK(compare_batch(7, 0, 8) == BatchRelation::older);
  CHECK(compare_batch(4, 0, 8) == BatchRelation::older);

  ReorderBatchTracker tracker(4, 8);
  CHECK(tracker.observe(0, 0).disposition == PacketDisposition::accept);
  CHECK(tracker.observe(0, 0).disposition == PacketDisposition::duplicate);
  const auto newer = tracker.observe(1, 0);
  CHECK(newer.disposition == PacketDisposition::accept);
  CHECK(newer.closed_incomplete_batch);
  CHECK(tracker.observe(0, 1).disposition == PacketDisposition::stale);

  CHECK(tracker.observe(1, 1).disposition == PacketDisposition::accept);
  CHECK(tracker.observe(1, 2).disposition == PacketDisposition::accept);
  CHECK(tracker.observe(1, 3).disposition == PacketDisposition::accept);
  tracker.complete_batch();
  CHECK(tracker.observe(2, 0).disposition == PacketDisposition::accept);

  ReorderBatchTracker wrap(2, 8);
  CHECK(wrap.observe(7, 0).disposition == PacketDisposition::accept);
  CHECK(wrap.observe(7, 1).disposition == PacketDisposition::accept);
  wrap.complete_batch();
  const auto wrapped = wrap.observe(0, 0);
  CHECK(wrapped.disposition == PacketDisposition::accept);
  CHECK(!wrapped.closed_incomplete_batch);
  CHECK(wrap.observe(7, 1).disposition == PacketDisposition::stale);
  return EXIT_SUCCESS;
}

}  // namespace

int main() {
  return run();
}
