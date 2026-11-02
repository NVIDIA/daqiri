// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cerrno>
#include <cstdint>
#include <ctime>

namespace daqiri::detail {
// An eight-second cyclic comparison is unambiguous only within half a cycle.
inline bool rdma_tx_time_in_range(uint64_t target, uint64_t now) {
  return target == 0 || (target >= now ? target - now : now - target) < 4000000000ULL;
}
inline int validate_rdma_tx_time(uint64_t target) {
  if (!target) return 0;
  timespec now{};
  if (clock_gettime(CLOCK_REALTIME, &now)) return errno;
  return rdma_tx_time_in_range(target, uint64_t(now.tv_sec) * 1000000000ULL + now.tv_nsec) ? 0
                                                                                           : EINVAL;
}
}  // namespace daqiri::detail
