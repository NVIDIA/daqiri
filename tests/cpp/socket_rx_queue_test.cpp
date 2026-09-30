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

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>

#include "src/engines/socket/daqiri_socket_engine.h"

namespace daqiri {

class SocketEngineQueueTestPeer {
 public:
  static bool bounded_queue_blocks_until_pop() {
    SocketEngine engine;
    engine.running_.store(true);

    auto queue = std::make_shared<SocketEngine::RxQueueState>();
    queue->max_bursts = 1;
    auto* queued_burst = new BurstParams{};
    engine.push_rx_burst(queue, queued_burst);

    std::atomic<bool> connection_running{true};
    auto reservation = std::async(
        std::launch::async, [&] { return engine.reserve_rx_burst(queue, connection_running); });

    const bool initially_blocked =
        reservation.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout;

    BurstParams* popped_burst = nullptr;
    const bool popped = engine.pop_rx_burst(queue, &popped_burst) == Status::SUCCESS &&
                        popped_burst == queued_burst;
    if (!popped) {
      {
        std::lock_guard<std::mutex> lock(queue->mutex);
        connection_running.store(false);
      }
      queue->capacity_cv.notify_all();
    }

    bool reservation_ready =
        reservation.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    if (!reservation_ready) {
      {
        std::lock_guard<std::mutex> lock(queue->mutex);
        connection_running.store(false);
      }
      queue->capacity_cv.notify_all();
      reservation_ready =
          reservation.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    }
    const bool reserved = reservation_ready && reservation.get();
    if (reserved) {
      engine.cancel_rx_burst_reservation(queue);
    }
    engine.running_.store(false);
    engine.free_rx_burst(popped_burst);
    return initially_blocked && popped && reserved && queue->bursts.empty() &&
           queue->reserved_bursts == 0;
  }

  static bool shutdown_wakes_blocked_receiver() {
    SocketEngine engine;
    engine.running_.store(true);

    auto queue = std::make_shared<SocketEngine::RxQueueState>();
    queue->max_bursts = 1;
    auto* queued_burst = new BurstParams{};
    engine.push_rx_burst(queue, queued_burst);

    std::atomic<bool> connection_running{true};
    auto reservation = std::async(
        std::launch::async, [&] { return engine.reserve_rx_burst(queue, connection_running); });

    const bool initially_blocked =
        reservation.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout;

    {
      std::lock_guard<std::mutex> lock(queue->mutex);
      connection_running.store(false);
    }
    queue->capacity_cv.notify_all();
    const bool stopped =
        reservation.wait_for(std::chrono::seconds(1)) == std::future_status::ready &&
        !reservation.get();

    BurstParams* popped_burst = nullptr;
    engine.pop_rx_burst(queue, &popped_burst);
    engine.running_.store(false);
    engine.free_rx_burst(popped_burst);
    return initially_blocked && stopped;
  }
};

}  // namespace daqiri

int main() {
  if (!daqiri::SocketEngineQueueTestPeer::bounded_queue_blocks_until_pop()) {
    std::cerr << "bounded TCP RX queue did not unblock after a pop\n";
    return 1;
  }
  if (!daqiri::SocketEngineQueueTestPeer::shutdown_wakes_blocked_receiver()) {
    std::cerr << "blocked TCP RX queue did not wake for shutdown\n";
    return 1;
  }
  return 0;
}
