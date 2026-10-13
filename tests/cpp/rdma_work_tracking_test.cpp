// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#include "src/engines/rdma/rdma_work_tracking.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
void require(bool condition) {
  if (!condition) {
    std::cerr << "RDMA work tracking assertion failed\n";
    std::exit(1);
  }
}
}  // namespace

int main() {
  using daqiri::detail::rdma_range_valid;
  require(rdma_range_valid(120, 80, 100, 100));
  require(!rdma_range_valid(120, 81, 100, 100));
  require(!rdma_range_valid(99, 1, 100, 100));
  require(!rdma_range_valid(UINT64_MAX - 1, 2, UINT64_MAX - 10, 20));
  require(rdma_range_valid(UINT64_MAX - 1, 1, UINT64_MAX - 10, 10));

  int first = 0, second = 0, third = 0;
  daqiri::detail::RdmaWorkTracking<int> work;
  std::vector<int*> completed;
  std::vector<bool> failures;
  std::vector<uint32_t> packets;
  auto packet_done = [&](int*, uint32_t packet, bool) { packets.push_back(packet); };
  auto burst_done = [&](int* burst, bool failed) {
    completed.push_back(burst);
    failures.push_back(failed);
  };
  work.posted(&first, 0);
  work.posted(&first, 1);
  work.posted(&first, 2);
  const auto final_first = work.next_id() - 1;
  require(work.complete_through(1, false, packet_done, burst_done));
  require(completed.empty());  // A READ buffer must remain owned until the whole batch finishes.
  require(work.complete_through(final_first, false, packet_done, burst_done));
  require(completed.size() == 1 && completed[0] == &first && !failures[0]);
  require((packets == std::vector<uint32_t>{0, 1, 2}));
  require(!work.complete_through(final_first, false, packet_done, burst_done));

  // Simulate a partial post failure: only the first two WRs reached the NIC.
  work.posted(&second, 0);
  work.posted(&second, 1);
  work.mark_failed(&second);
  work.posted(&third, 0);
  require(work.complete_through(work.next_id() - 1, false, packet_done, burst_done));
  require(completed.size() == 3 && completed[1] == &second && failures[1]);
  require(completed[2] == &third && !failures[2]);
  require(work.size() == 0);

  work.posted(&first, 0);
  work.posted(&first, 1);
  require(work.complete_through(work.next_id() - 2, true, packet_done, burst_done));
  require(completed.size() == 3);
  require(work.complete_through(work.next_id() - 1, false, packet_done, burst_done));
  require(completed.size() == 4 && failures[3]);  // Preserve errors from earlier CQEs.
  require(!work.complete_through(UINT64_MAX, false, packet_done, burst_done));
  return 0;
}
