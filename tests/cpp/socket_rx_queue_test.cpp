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
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "src/engines/socket/daqiri_socket_engine.h"

namespace daqiri {

class SocketEngineQueueTestPeer {
 public:
  static bool idle_connection_does_not_reserve_capacity() {
    SocketEngine engine;
    engine.running_.store(true);

    auto queue = std::make_shared<SocketEngine::RxQueueState>();
    queue->max_bursts = 1;

    int idle_fds[2] = {-1, -1};
    int active_fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, idle_fds) != 0 ||
        ::socketpair(AF_UNIX, SOCK_STREAM, 0, active_fds) != 0) {
      if (idle_fds[0] >= 0) {
        ::close(idle_fds[0]);
      }
      if (idle_fds[1] >= 0) {
        ::close(idle_fds[1]);
      }
      return false;
    }

    auto idle = std::make_shared<SocketEngine::ConnectionState>();
    idle->fd = idle_fds[0];
    idle->conn_id = 1;
    idle->rx_queue = queue;
    idle->running.store(true);
    std::thread idle_thread(&SocketEngine::tcp_rx_loop, &engine, idle);

    // Give the idle receiver time to block. The old implementation reserved
    // the only queue slot before entering recv() and starved the active peer.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto active = std::make_shared<SocketEngine::ConnectionState>();
    active->fd = active_fds[0];
    active->conn_id = 2;
    active->rx_queue = queue;
    active->running.store(true);
    std::thread active_thread(&SocketEngine::tcp_rx_loop, &engine, active);

    constexpr uint8_t payload = 42;
    const bool sent = ::send(active_fds[1], &payload, sizeof(payload), 0) == sizeof(payload);

    BurstParams* received = nullptr;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (sent && std::chrono::steady_clock::now() < deadline && received == nullptr) {
      if (engine.pop_rx_burst(queue, &received) != Status::SUCCESS) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }

    engine.running_.store(false);
    {
      std::lock_guard<std::mutex> lock(queue->mutex);
      idle->running.store(false);
      active->running.store(false);
    }
    queue->capacity_cv.notify_all();
    ::shutdown(idle_fds[1], SHUT_RDWR);
    ::shutdown(active_fds[1], SHUT_RDWR);
    ::close(idle_fds[1]);
    ::close(active_fds[1]);
    idle_thread.join();
    active_thread.join();

    const bool correct_payload = received != nullptr && received->hdr.hdr.num_pkts == 1 &&
                                 received->pkt_lens[0][0] == sizeof(payload) &&
                                 *reinterpret_cast<uint8_t*>(received->pkts[0][0]) == payload;
    engine.free_all_packets(received);
    engine.free_rx_burst(received);
    return sent && correct_payload;
  }

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
  if (!daqiri::SocketEngineQueueTestPeer::idle_connection_does_not_reserve_capacity()) {
    std::cerr << "idle TCP connection blocked an active peer\n";
    return 1;
  }
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
