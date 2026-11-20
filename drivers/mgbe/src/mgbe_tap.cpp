// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
#include "mgbe_tap.hpp"
#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/if_arp.h>
#include <linux/if_tun.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <future>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace mgbe {
namespace {
struct Fd {
  int value;
  explicit Fd(int fd) : value(fd) {}
  Fd(const Fd&) = delete;
  Fd(Fd&& other) noexcept : value(other.release()) {}
  ~Fd() {
    if (value >= 0) close(value);
  }
  int release() {
    int result = value;
    value = -1;
    return result;
  }
};
[[noreturn]] void fail(const char* operation) {
  throw std::system_error(errno, std::generic_category(), operation);
}
void validate(const KernelTap::Options& options) {
  if (options.name.empty() || options.name.size() >= IFNAMSIZ ||
      options.name.find_first_not_of(
          "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") !=
          std::string::npos ||
      options.name == "." || options.name == ".." || options.mtu < 68 || options.mtu > 9000 ||
      options.queue_size < 2 || options.queue_size > 4096 ||
      (options.queue_size & (options.queue_size - 1)) || options.worker_cpu < 0 ||
      options.worker_cpu >= CPU_SETSIZE || (options.mac[0] & 1) ||
      std::all_of(options.mac.begin(), options.mac.end(), [](uint8_t v) { return v == 0; }))
    throw std::invalid_argument("invalid MGBE TAP name, MAC, MTU, queue size or worker CPU");
}
int open_tap(const KernelTap::Options& options) {
  validate(options);
  Fd tap(open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC));
  if (tap.value < 0) fail("open TAP");
  ifreq request{};
  std::memcpy(request.ifr_name, options.name.c_str(), options.name.size() + 1);
  request.ifr_flags = static_cast<short>(IFF_TAP | IFF_NO_PI | IFF_TUN_EXCL);
  if (ioctl(tap.value, TUNSETIFF, &request)) fail("create exclusive TAP");
  if (ioctl(tap.value, TUNSETOFFLOAD, 0)) fail("disable TAP offloads");
  Fd control(socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
  if (control.value < 0) fail("open TAP control");
  request.ifr_hwaddr.sa_family = ARPHRD_ETHER;
  std::memcpy(request.ifr_hwaddr.sa_data, options.mac.data(), options.mac.size());
  if (ioctl(control.value, SIOCSIFHWADDR, &request)) fail("set TAP MAC");
  request.ifr_mtu = options.mtu;
  if (ioctl(control.value, SIOCSIFMTU, &request)) fail("set TAP MTU");
  if (ioctl(control.value, SIOCGIFFLAGS, &request)) fail("read TAP flags");
  request.ifr_flags |= IFF_UP;
  if (ioctl(control.value, SIOCSIFFLAGS, &request)) fail("bring TAP up");
  return tap.release();
}

// Bounded MPMC frame queue. Per-slot generations publish payload copies and
// prevent a producer from overwriting a consumer's frame at ring wraparound.
class Frames {
  struct alignas(64) Cell {
    std::atomic<size_t> sequence{0};
    size_t length = 0;
  };
  size_t capacity_, stride_;
  std::unique_ptr<Cell[]> cells_;
  std::unique_ptr<uint8_t[]> bytes_;
  alignas(64) std::atomic<size_t> head_{0};
  alignas(64) std::atomic<size_t> tail_{0};

 public:
  Frames(size_t capacity, size_t frame_size)
      : capacity_(capacity),
        stride_((frame_size + 63) & ~size_t(63)),
        cells_(new Cell[capacity]),
        bytes_(new uint8_t[capacity * stride_]{}) {
    for (size_t i = 0; i < capacity_; ++i) cells_[i].sequence.store(i);
  }
  bool push(const void* data, size_t length) {
    size_t pos = head_.load(std::memory_order_relaxed);
    Cell* cell;
    for (;;) {
      cell = &cells_[pos & (capacity_ - 1)];
      const auto delta =
          static_cast<intptr_t>(cell->sequence.load(std::memory_order_acquire) - pos);
      if (delta < 0) return false;
      if (!delta && head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
      if (delta > 0) pos = head_.load(std::memory_order_relaxed);
    }
    std::memcpy(bytes_.get() + (pos & (capacity_ - 1)) * stride_, data, length);
    cell->length = length;
    cell->sequence.store(pos + 1, std::memory_order_release);
    return true;
  }
  bool ready() const {
    const size_t pos = tail_.load(std::memory_order_relaxed);
    return cells_[pos & (capacity_ - 1)].sequence.load(std::memory_order_acquire) == pos + 1;
  }
  bool pop(void* data, size_t& length) {
    size_t pos = tail_.load(std::memory_order_relaxed);
    Cell* cell;
    for (;;) {
      cell = &cells_[pos & (capacity_ - 1)];
      const auto delta =
          static_cast<intptr_t>(cell->sequence.load(std::memory_order_acquire) - (pos + 1));
      if (delta < 0) return false;
      if (!delta && tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
      if (delta > 0) pos = tail_.load(std::memory_order_relaxed);
    }
    length = cell->length;
    std::memcpy(data, bytes_.get() + (pos & (capacity_ - 1)) * stride_, length);
    cell->sequence.store(pos + capacity_, std::memory_order_release);
    return true;
  }
};
}  // namespace

struct KernelTap::Impl {
  Options options;
  Fd packet, wake;
  Frames to_kernel, from_kernel;
  std::unique_ptr<uint8_t[]> inbound, outbound;
  std::atomic<bool> stopping{false};
  std::atomic<int> error{0};
  std::thread worker;
  int actual_cpu = -1, actual_policy = -1;
  alignas(64) std::atomic<uint64_t> enqueued{0}, dropped{0};
  alignas(64) std::atomic<uint64_t> written{0}, read{0}, invalid{0}, discarded{0};
  alignas(64) std::atomic<uint64_t> delivered{0};
  size_t maximum() const {
    return options.mtu + 14;
  }
  void check() const {
    const int code = error.load(std::memory_order_acquire);
    if (code) throw std::system_error(code, std::generic_category(), "MGBE TAP worker failed");
  }
  Impl(const Options& opts, Fd fd)
      : options(opts),
        packet(std::move(fd)),
        wake(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)),
        to_kernel(opts.queue_size, opts.mtu + 14),
        from_kernel(opts.queue_size, opts.mtu + 14),
        inbound(new uint8_t[opts.mtu + 15]),
        outbound(new uint8_t[opts.mtu + 14]) {
    if (wake.value < 0) fail("create TAP shutdown event");
    const int flags = fcntl(packet.value, F_GETFL);
    if (flags < 0 || fcntl(packet.value, F_SETFL, flags | O_NONBLOCK) < 0 ||
        fcntl(packet.value, F_SETFD, FD_CLOEXEC) < 0)
      fail("configure TAP descriptor");
    std::promise<void> started;
    auto ready = started.get_future();
    worker = std::thread([this, started = std::move(started)]() mutable {
      try {
        cpu_set_t cpus;
        CPU_ZERO(&cpus);
        CPU_SET(options.worker_cpu, &cpus);
        int result = pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus);
        if (result) throw std::system_error(result, std::generic_category(), "pin TAP worker");
        // The creating application may be SCHED_FIFO on its isolated CPU.
        sched_param normal{};
        result = pthread_setschedparam(pthread_self(), SCHED_OTHER, &normal);
        if (result)
          throw std::system_error(result, std::generic_category(), "set TAP worker policy");
        actual_cpu = sched_getcpu();
        actual_policy = sched_getscheduler(0);
        started.set_value();
      } catch (...) {
        started.set_exception(std::current_exception());
        return;
      }
      run();
    });
    try {
      ready.get();
    } catch (...) {
      worker.join();
      throw;
    }
  }
  void run() noexcept {
    try {
      size_t in_length = 0, out_length = 0;
      while (!stopping.load(std::memory_order_acquire)) {
        // Bound each direction's work so shutdown and the opposite direction
        // remain serviceable under a continuous exception stream.
        for (size_t budget = 0; budget < 64; ++budget) {
          if (!out_length && !to_kernel.pop(outbound.get(), out_length)) break;
          const auto n = write(packet.value, outbound.get(), out_length);
          if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            fail("write TAP frame");
          }
          if (size_t(n) != out_length)
            throw std::system_error(EIO, std::generic_category(), "partial TAP write");
          written.fetch_add(1, std::memory_order_relaxed);
          out_length = 0;
        }
        for (size_t budget = 0; budget < 64; ++budget) {
          if (!in_length) {
            const auto n = ::read(packet.value, inbound.get(), maximum() + 1);
            if (n < 0) {
              if (errno == EINTR) continue;
              if (errno == EAGAIN || errno == EWOULDBLOCK) break;
              fail("read TAP frame");
            }
            if (!n) throw std::system_error(EIO, std::generic_category(), "TAP closed");
            if (n < 14 || size_t(n) > maximum()) {
              invalid.fetch_add(1, std::memory_order_relaxed);
              continue;
            }
            in_length = n;
            read.fetch_add(1, std::memory_order_relaxed);
          }
          if (!from_kernel.push(inbound.get(), in_length)) break;
          in_length = 0;
        }
        if (to_kernel.ready() && !out_length) continue;
        pollfd fds[] = {
            {packet.value, short((in_length ? 0 : POLLIN) | (out_length ? POLLOUT : 0)), 0},
            {wake.value, POLLIN, 0}};
        // Producers never make a syscall. Check their queue at least every
        // millisecond; TAP readability wakes kernel-originated traffic sooner.
        const int result = poll(fds, 2, 1);
        if (result < 0 && errno != EINTR) fail("poll TAP");
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
          throw std::system_error(EIO, std::generic_category(), "TAP poll fault");
      }
    } catch (const std::system_error& e) {
      error.store(e.code().value() ? e.code().value() : EIO, std::memory_order_release);
    } catch (...) {
      error.store(ENOMEM, std::memory_order_release);
    }
  }
  void stop() {
    if (!worker.joinable()) return;
    stopping.store(true, std::memory_order_release);
    uint64_t one = 1;
    const auto ignored = write(wake.value, &one, sizeof(one));
    (void)ignored;
    worker.join();
    size_t length;
    while (to_kernel.pop(outbound.get(), length)) {
    }
    while (from_kernel.pop(outbound.get(), length)) {
    }
    discarded.store(enqueued.load() - written.load() + read.load() - delivered.load());
    if (packet.value >= 0) close(packet.release());
  }
  ~Impl() {
    stop();
  }
};

KernelTap::KernelTap(const Options& options) : KernelTap(options, open_tap(options)) {}
KernelTap::KernelTap(const Options& options, int packet_fd) {
  Fd guard(packet_fd);
  validate(options);
  // The FD parameter retains ownership if allocation or any member
  // construction fails, including copying the options string.
  impl_ = std::make_unique<Impl>(options, std::move(guard));
}
KernelTap::~KernelTap() = default;
bool KernelTap::send_to_kernel(const void* frame, size_t length) {
  impl_->check();
  if (!frame || length < 14 || length > max_frame_size() ||
      impl_->stopping.load(std::memory_order_acquire)) {
    impl_->dropped.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (!impl_->to_kernel.push(frame, length)) {
    impl_->dropped.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  impl_->enqueued.fetch_add(1, std::memory_order_relaxed);
  return true;
}
bool KernelTap::has_tx() const {
  impl_->check();
  return impl_->from_kernel.ready();
}
bool KernelTap::receive_from_kernel(void* frame, size_t capacity, size_t& length) {
  impl_->check();
  length = 0;
  if (!frame || capacity < max_frame_size()) throw std::invalid_argument("short TAP TX buffer");
  if (!impl_->from_kernel.pop(frame, length)) return false;
  if (length < 60) {
    std::memset(static_cast<uint8_t*>(frame) + length, 0, 60 - length);
    length = 60;
  }
  impl_->delivered.fetch_add(1, std::memory_order_relaxed);
  return true;
}
size_t KernelTap::max_frame_size() const {
  return impl_->maximum();
}
const std::string& KernelTap::name() const {
  return impl_->options.name;
}
KernelTap::Stats KernelTap::stats() const {
  return {impl_->enqueued.load(),  impl_->written.load(),   impl_->dropped.load(),
          impl_->read.load(),      impl_->delivered.load(), impl_->invalid.load(),
          impl_->discarded.load(), impl_->actual_cpu,       impl_->actual_policy,
          impl_->error.load()};
}
void KernelTap::stop() {
  impl_->stop();
}
}  // namespace mgbe
