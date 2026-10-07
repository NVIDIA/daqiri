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
    queue->max_packets = 1;

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
    queue->max_packets = 1;
    auto* queued_burst = new BurstParams{};
    queued_burst->hdr.hdr.num_pkts = 1;
    engine.push_rx_burst(queue, queued_burst);

    std::atomic<bool> connection_running{true};
    auto reservation = std::async(std::launch::async, [&] {
      return engine.reserve_rx_packets(queue, 1, &connection_running);
    });

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
    const size_t reserved = reservation_ready ? reservation.get() : 0;
    if (reserved > 0) {
      engine.cancel_rx_packet_reservation(queue, reserved);
    }
    engine.running_.store(false);
    engine.free_rx_burst(popped_burst);
    return initially_blocked && popped && reserved == 1 && queue->bursts.empty() &&
           queue->queued_packets == 0 && queue->reserved_packets == 0;
  }

  static bool shutdown_wakes_blocked_receiver() {
    SocketEngine engine;
    engine.running_.store(true);

    auto queue = std::make_shared<SocketEngine::RxQueueState>();
    queue->max_packets = 1;
    auto* queued_burst = new BurstParams{};
    queued_burst->hdr.hdr.num_pkts = 1;
    engine.push_rx_burst(queue, queued_burst);

    std::atomic<bool> connection_running{true};
    auto reservation = std::async(std::launch::async, [&] {
      return engine.reserve_rx_packets(queue, 1, &connection_running);
    });

    const bool initially_blocked =
        reservation.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout;

    {
      std::lock_guard<std::mutex> lock(queue->mutex);
      connection_running.store(false);
    }
    queue->capacity_cv.notify_all();
    const bool stopped =
        reservation.wait_for(std::chrono::seconds(1)) == std::future_status::ready &&
        reservation.get() == 0;

    BurstParams* popped_burst = nullptr;
    engine.pop_rx_burst(queue, &popped_burst);
    engine.running_.store(false);
    engine.free_rx_burst(popped_burst);
    return initially_blocked && stopped;
  }

  static bool udp_queue_is_bounded_by_packet_capacity() {
    SocketEngine engine;
    engine.running_.store(true);

    auto queue = std::make_shared<SocketEngine::RxQueueState>();
    queue->max_packets = 4;

    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_DGRAM, 0, fds) != 0) {
      return false;
    }

    auto endpoint = std::make_unique<SocketEngine::EndpointState>();
    endpoint->if_index = 0;
    endpoint->udp_fd = fds[0];
    endpoint->rx_batch_size = 3;
    endpoint->max_packet_size = 64;
    endpoint->socket_cfg.mode_ = SocketMode::CLIENT;
    endpoint->rx_queue_state = queue;
    auto* endpoint_ptr = endpoint.get();
    engine.endpoints_.push_back(std::move(endpoint));

    std::thread rx_thread(&SocketEngine::udp_rx_loop, &engine, 0);

    constexpr uint8_t payload = 42;
    for (int i = 0; i < 256; ++i) {
      ::send(fds[1], &payload, sizeof(payload), MSG_DONTWAIT);
    }

    bool reached_capacity = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
      {
        std::lock_guard<std::mutex> lock(queue->mutex);
        reached_capacity = queue->queued_packets == queue->max_packets;
      }
      if (reached_capacity) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    bool bounded = false;
    {
      std::lock_guard<std::mutex> lock(queue->mutex);
      bounded = queue->queued_packets + queue->reserved_packets <= queue->max_packets;
    }

    engine.running_.store(false);
    queue->capacity_cv.notify_all();
    ::shutdown(fds[0], SHUT_RDWR);
    ::close(fds[0]);
    endpoint_ptr->udp_fd = -1;
    rx_thread.join();
    ::close(fds[1]);

    BurstParams* burst = nullptr;
    while (engine.pop_rx_burst(queue, &burst) == Status::SUCCESS) {
      engine.free_all_packets(burst);
      engine.free_rx_burst(burst);
      burst = nullptr;
    }
    engine.endpoints_.clear();

    return reached_capacity && bounded && engine.rx_pkts_.load() == queue->max_packets &&
           queue->queued_packets == 0 && queue->reserved_packets == 0;
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
  if (!daqiri::SocketEngineQueueTestPeer::udp_queue_is_bounded_by_packet_capacity()) {
    std::cerr << "UDP RX queue exceeded its packet capacity\n";
    return 1;
  }
  return 0;
}
