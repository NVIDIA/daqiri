/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <cstddef>

namespace daqiri::detail {

struct RdmaClientQueueAssignment {
  int interface_index;
  int queue_index;
};

template <typename Assignments>
constexpr int first_available_rdma_client_queue(int interface_index, std::size_t queue_count,
                                                const Assignments& assignments) {
  for (std::size_t candidate = 0; candidate < queue_count; ++candidate) {
    bool occupied = false;
    for (const auto& assignment : assignments) {
      if (assignment.interface_index == interface_index &&
          assignment.queue_index == static_cast<int>(candidate)) {
        occupied = true;
        break;
      }
    }
    if (!occupied) {
      return static_cast<int>(candidate);
    }
  }
  return -1;
}

}  // namespace daqiri::detail
