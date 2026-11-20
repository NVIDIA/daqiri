// SPDX-License-Identifier: Apache-2.0
#include "mgbe_tap.hpp"
#include <fcntl.h>
#include <sched.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

namespace mgbe {
struct KernelTapTestAccess {
  static std::unique_ptr<KernelTap> adopt(const KernelTap::Options& options, int fd) {
    return std::unique_ptr<KernelTap>(new KernelTap(options, fd));
  }
};
}  // namespace mgbe
namespace {
using Clock = std::chrono::steady_clock;
template <typename F>
void until(F predicate) {
  const auto deadline = Clock::now() + std::chrono::seconds(5);
  while (!predicate()) {
    assert(Clock::now() < deadline);
    std::this_thread::yield();
  }
}
mgbe::KernelTap::Options options() {
  mgbe::KernelTap::Options o;
  o.name = "mgbe-test";
  o.mac = {2, 0, 0, 0, 0, 1};
  o.mtu = 128;
  o.queue_size = 8;
  cpu_set_t cpus;
  CPU_ZERO(&cpus);
  assert(!sched_getaffinity(0, sizeof(cpus), &cpus));
  for (int i = 0; i < CPU_SETSIZE; ++i)
    if (CPU_ISSET(i, &cpus)) {
      o.worker_cpu = i;
      break;
    }
  return o;
}
struct Fixture {
  int peer = -1;
  std::unique_ptr<mgbe::KernelTap> tap;
  explicit Fixture(mgbe::KernelTap::Options o = options()) {
    int fds[2];
    assert(!socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds));
    peer = fds[1];
    tap = mgbe::KernelTapTestAccess::adopt(o, fds[0]);
  }
  ~Fixture() {
    tap.reset();
    if (peer >= 0) close(peer);
  }
};
void two_way_copy_and_padding() {
  Fixture f;
  std::array<uint8_t, 142> input{}, output{};
  for (size_t i = 0; i < input.size(); ++i) input[i] = i;
  const auto expected = input;
  assert(f.tap->send_to_kernel(input.data(), input.size()));
  input.fill(0xff);  // DMA owner may immediately overwrite/recycle its RX slot.
  ssize_t n = -1;
  until([&] {
    n = recv(f.peer, output.data(), output.size(), MSG_DONTWAIT);
    return n >= 0;
  });
  assert(n == ssize_t(expected.size()) && output == expected);
  assert(send(f.peer, expected.data(), 42, 0) == 42);
  size_t length = 0;
  until([&] { return f.tap->has_tx(); });
  bool rejected = false;
  try {
    f.tap->receive_from_kernel(output.data(), 60, length);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  assert(rejected && f.tap->has_tx());  // Small caller buffer must not consume it.
  output.fill(0xff);
  assert(f.tap->receive_from_kernel(output.data(), output.size(), length) && length == 60);
  assert(!std::memcmp(output.data(), expected.data(), 42));
  for (size_t i = 42; i < 60; ++i) assert(!output[i]);
  assert(!f.tap->receive_from_kernel(output.data(), output.size(), length));
  assert(!f.tap->send_to_kernel(nullptr, 42));
  assert(!f.tap->send_to_kernel(input.data(), 13));
  assert(!f.tap->send_to_kernel(input.data(), input.size() + 1));
  std::array<uint8_t, 200> oversized{};
  assert(send(f.peer, oversized.data(), oversized.size(), 0) == ssize_t(oversized.size()));
  assert(send(f.peer, input.data(), 5, 0) == 5);
  until([&] { return f.tap->stats().invalid_frames == 2; });
  auto stats = f.tap->stats();
  assert(stats.worker_cpu == options().worker_cpu && stats.worker_policy == SCHED_OTHER);
  assert(stats.to_kernel_enqueued == 1 && stats.to_kernel_delivered == 1);
  assert(stats.from_kernel_received == 1 && stats.from_kernel_delivered == 1);
  f.tap->stop();
  f.tap->stop();
  assert(!f.tap->stats().discarded_on_stop && !f.tap->stats().error);
}
void multi_producer_wrap() {
  Fixture f;
  constexpr uint32_t producers = 4, count = 3000;
  std::vector<std::thread> threads;
  for (uint32_t p = 0; p < producers; ++p)
    threads.emplace_back([&, p] {
      std::array<uint32_t, 16> frame{};
      for (uint32_t i = 0; i < count; ++i) {
        frame.fill(i ^ (p << 24));
        until([&] { return f.tap->send_to_kernel(frame.data(), sizeof(frame)); });
      }
    });
  std::vector<bool> seen(producers * count);
  for (uint32_t i = 0; i < producers * count; ++i) {
    std::array<uint32_t, 16> frame{};
    ssize_t n = -1;
    until([&] {
      n = recv(f.peer, frame.data(), sizeof(frame), MSG_DONTWAIT);
      return n >= 0;
    });
    assert(n == sizeof(frame));
    for (auto word : frame) assert(word == frame[0]);
    const uint32_t p = frame[0] >> 24, seq = frame[0] & 0xffffff;
    assert(p < producers && seq < count && !seen[p * count + seq]);
    seen[p * count + seq] = true;
  }
  for (auto& t : threads) t.join();
  until([&] { return f.tap->stats().to_kernel_delivered == producers * count; });
  assert(f.tap->stats().to_kernel_enqueued == producers * count);
}
void backpressure_and_bounded_stop() {
  auto o = options();
  o.queue_size = 2;
  Fixture f(o);
  std::array<uint8_t, 100> frame{};
  bool full = false;
  for (size_t i = 0; i < 100000 && !full; ++i)
    full = !f.tap->send_to_kernel(frame.data(), frame.size());
  assert(full && f.tap->stats().to_kernel_dropped);
  // Fill the opposite handoff and exercise its pending frame/backpressure.
  for (size_t i = 0; i < 20; ++i) send(f.peer, frame.data(), frame.size(), MSG_DONTWAIT);
  until([&] { return f.tap->has_tx(); });
  const auto begin = Clock::now();
  f.tap->stop();
  assert(Clock::now() - begin < std::chrono::seconds(1));
  const auto s = f.tap->stats();
  assert(s.discarded_on_stop == s.to_kernel_enqueued - s.to_kernel_delivered +
                                    s.from_kernel_received - s.from_kernel_delivered);
  assert(!f.tap->has_tx());
}
void multi_consumer_wrap() {
  Fixture f;
  constexpr uint32_t consumers = 4, count = 6000;
  auto seen = std::make_unique<std::atomic<unsigned>[]>(count);
  for (size_t i = 0; i < count; ++i) seen[i] = 0;
  std::atomic<uint32_t> received{0};
  std::vector<std::thread> threads;
  for (uint32_t i = 0; i < consumers; ++i)
    threads.emplace_back([&] {
      std::array<uint8_t, 142> frame{};
      const auto deadline = Clock::now() + std::chrono::seconds(5);
      while (received.load(std::memory_order_relaxed) < count) {
        size_t length = 0;
        if (f.tap->receive_from_kernel(frame.data(), frame.size(), length)) {
          assert(length == 64);
          uint32_t seq;
          std::memcpy(&seq, frame.data(), sizeof(seq));
          assert(seq < count && seen[seq].fetch_add(1) == 0);
          for (size_t j = 4; j < length; ++j) assert(frame[j] == uint8_t(seq));
          received.fetch_add(1, std::memory_order_relaxed);
        } else {
          assert(Clock::now() < deadline);
          std::this_thread::yield();
        }
      }
    });
  for (uint32_t seq = 0; seq < count; ++seq) {
    std::array<uint8_t, 64> frame{};
    frame.fill(uint8_t(seq));
    std::memcpy(frame.data(), &seq, sizeof(seq));
    until([&] {
      return send(f.peer, frame.data(), frame.size(), MSG_DONTWAIT) == ssize_t(frame.size());
    });
  }
  for (auto& thread : threads) thread.join();
  assert(f.tap->stats().from_kernel_delivered == count);
  for (size_t i = 0; i < count; ++i) assert(seen[i] == 1);
}
void worker_fault_and_constructor_cleanup() {
  Fixture f;
  close(f.peer);
  f.peer = -1;
  std::array<uint8_t, 60> frame{};
  assert(f.tap->send_to_kernel(frame.data(), frame.size()));
  until([&] { return f.tap->stats().error != 0; });
  bool rejected = false;
  try {
    f.tap->has_tx();
  } catch (const std::system_error&) {
    rejected = true;
  }
  assert(rejected);
  int fds[2];
  assert(!socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, fds));
  auto o = options();
  o.queue_size = 3;
  rejected = false;
  try {
    auto tap = mgbe::KernelTapTestAccess::adopt(o, fds[0]);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  assert(rejected && fcntl(fds[0], F_GETFD) < 0);
  close(fds[1]);
}
}  // namespace
int main() {
  two_way_copy_and_padding();
  multi_producer_wrap();
  multi_consumer_wrap();
  backpressure_and_bounded_stop();
  worker_fault_and_constructor_cleanup();
  std::cout << "MGBE TAP copy, ownership, concurrency, backpressure and recovery tests passed\n";
}
