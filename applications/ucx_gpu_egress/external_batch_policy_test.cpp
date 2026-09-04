// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
// All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#include "external_batch_policy.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

#define CHECK(condition)                                                                         \
  do {                                                                                           \
    if (!(condition)) {                                                                          \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
      return EXIT_FAILURE;                                                                       \
    }                                                                                            \
  } while (false)

int run() {
  using daqiri::ucx_gpu::detail::decide_batch_admission;
  using daqiri::ucx_gpu::detail::SubmittedSequenceLedger;

  const auto none = decide_batch_admission(16, 0);
  CHECK(none.admitted == 0);
  CHECK(none.dropped == 16);

  const auto prefix = decide_batch_admission(16, 5);
  CHECK(prefix.admitted == 5);
  CHECK(prefix.dropped == 11);

  const auto all = decide_batch_admission(7, 64);
  CHECK(all.admitted == 7);
  CHECK(all.dropped == 0);

  SubmittedSequenceLedger ledger(64);
  std::string error;
  CHECK(ledger.record(0, 16, error));
  CHECK(ledger.record_drop(16, 16, error));
  CHECK(ledger.record(32, 16, error));
  CHECK(ledger.record(63, 1, error));
  CHECK(ledger.submitted_images() == 33);
  CHECK(ledger.dropped_before_submit() == 31);

  error.clear();
  CHECK(!ledger.record(48, 16, error));
  CHECK(!ledger.record_drop(48, 16, error));
  CHECK(!error.empty());

  SubmittedSequenceLedger bounds(32);
  error.clear();
  CHECK(!bounds.record(17, 16, error));
  CHECK(!bounds.record(0, 0, error));
  CHECK(!bounds.record(0, 17, error));
  CHECK(bounds.record(16, 16, error));
  CHECK(bounds.submitted_images() == 16);
  CHECK(bounds.dropped_before_submit() == 16);
  return EXIT_SUCCESS;
}

}  // namespace

int main() {
  return run();
}
