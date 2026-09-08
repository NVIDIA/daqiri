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
  using daqiri::ucx_gpu::detail::SubmittedSequenceLedger;

  SubmittedSequenceLedger ledger(64);
  std::string error;
  CHECK(ledger.record(0, 16, error));
  CHECK(ledger.record(32, 16, error));
  CHECK(ledger.record(63, 1, error));
  CHECK(ledger.submitted_images() == 33);

  error.clear();
  CHECK(!ledger.record(48, 16, error));
  CHECK(!error.empty());

  SubmittedSequenceLedger bounds(32);
  error.clear();
  CHECK(!bounds.record(17, 16, error));
  CHECK(!bounds.record(0, 0, error));
  CHECK(!bounds.record(0, 17, error));
  CHECK(bounds.record(16, 16, error));
  CHECK(bounds.submitted_images() == 16);
  return EXIT_SUCCESS;
}

}  // namespace

int main() {
  return run();
}
