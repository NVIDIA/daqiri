// SPDX-License-Identifier: Apache-2.0
#include "mgbe_pmd.hpp"
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <cassert>
#include <array>
#include <chrono>
#include <cstring>
#include <cerrno>
#include <iostream>
#include <stdexcept>
#include <system_error>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: mgbe_pmd_lifecycle /dev/mgbe-pmd-mgbe0_0\n";
    return 2;
  }
  try {
    for (unsigned cycle = 0; cycle < 10; ++cycle) {
      int fd = open(argv[1], O_RDWR | O_CLOEXEC);
      if (fd < 0) throw std::system_error(errno, std::generic_category(), "open control");
      auto bad = mgbe_pmd_config{MGBE_PMD_ABI, 65, 1500, 1};
      assert(ioctl(fd, MGBE_PMD_IOC_CONFIGURE, &bad) == -1 && errno == EINVAL);
      assert(ioctl(fd, MGBE_PMD_IOC_CONFIGURE, reinterpret_cast<void*>(1)) == -1 &&
             errno == EFAULT);
      assert(ioctl(fd, 0x12345678) == -1 && errno == ENOTTY);
      assert(ioctl(fd, MGBE_PMD_IOC_MAC_LOOPBACK) == -1 && errno == EINVAL);
      auto legacy = mgbe_pmd_config{1, 64, 1500, 1};
      assert(ioctl(fd, MGBE_PMD_IOC_CONFIGURE, &legacy) == -1 && errno == EINVAL);
      auto too_many = mgbe_pmd_config{MGBE_PMD_ABI, 64, 1500, MGBE_PMD_MAX_QUEUES + 1};
      assert(ioctl(fd, MGBE_PMD_IOC_CONFIGURE, &too_many) == -1 && errno == EINVAL);
      auto config = mgbe_pmd_config{MGBE_PMD_ABI, 64, 1500, 4};
      assert(ioctl(fd, MGBE_PMD_IOC_CONFIGURE, &config) == 0);
      std::array<mgbe_pmd_info, 4> info{};
      for (uint32_t q = 0; q < 4; ++q) {
        info[q].abi = MGBE_PMD_ABI;
        info[q].channel = q;
        assert(ioctl(fd, MGBE_PMD_IOC_QUEUE_INFO, &info[q]) == 0);
        assert(info[q].channel == q && info[q].queue_count == 4);
        for (uint32_t other = 0; other < q; ++other) {
          assert(info[q].rx_ring_dma != info[other].rx_ring_dma);
          assert(info[q].tx_pool_dma != info[other].tx_pool_dma);
        }
      }
      auto invalid = mgbe_pmd_info{};
      invalid.abi = MGBE_PMD_ABI;
      invalid.channel = 4;
      assert(ioctl(fd, MGBE_PMD_IOC_QUEUE_INFO, &invalid) == -1 && errno == EINVAL);
      auto rss = mgbe_pmd_rss{};
      assert(ioctl(fd, MGBE_PMD_IOC_GET_RSS, &rss) == 0);
      assert(rss.abi == MGBE_PMD_ABI);
      rss.table[0] = 4;
      assert(ioctl(fd, MGBE_PMD_IOC_SET_RSS, &rss) == -1 && errno == EINVAL);
      for (auto& entry : rss.table) entry = cycle % 4;
      assert(ioctl(fd, MGBE_PMD_IOC_SET_RSS, &rss) == 0);
      assert(mmap(nullptr, 4096, PROT_READ, MAP_SHARED, fd, MGBE_PMD_REGIONS * 4096) == MAP_FAILED);
      assert(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                  (4 * MGBE_PMD_REGIONS + MGBE_PMD_RX_RING) * 4096) == MAP_FAILED);
      void* extra = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                         (MGBE_PMD_REGIONS + MGBE_PMD_RX_RING) * 4096);
      assert(extra != MAP_FAILED);
      void* status = mmap(nullptr, 4096, PROT_READ, MAP_SHARED, fd, 0);
      assert(status != MAP_FAILED);
      assert(mprotect(status, 4096, PROT_READ | PROT_WRITE) == -1 && errno == EACCES);
      assert(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) == MAP_FAILED);
      assert(mmap(nullptr, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, MGBE_PMD_REGS * 4096) ==
             MAP_FAILED);
      close(fd);
      // A VMA retains the session after close(fd); hardware memory must not be
      // freed until that last mapping disappears.
      int still_owned = open(argv[1], O_RDWR | O_CLOEXEC);
      assert(still_owned == -1 && errno == EBUSY);
      assert(munmap(status, 4096) == 0);
      assert(open(argv[1], O_RDWR | O_CLOEXEC) == -1 && errno == EBUSY);
      assert(munmap(extra, 4096) == 0);
      {
        mgbe::Device d(argv[1], 64, 1500, 4);
        int second = open(argv[1], O_RDWR | O_CLOEXEC);
        assert(second == -1 && errno == EBUSY);
        auto stats = d.hardware_stats();
        (void)stats;
        bool stopped = false;
        try {
          d.queue().tx_buffer();
        } catch (const std::runtime_error&) {
          stopped = true;
        }
        assert(stopped);
        // No carrier is required for configure/map/close validation. Do not
        // start DMA or transmit traffic from this lifecycle test.
        d.stop();
        d.stop();
      }
    }
    {
      mgbe::Device standard(argv[1], 1024, 1500);
      assert(standard.queue().info().slot_size == 2048);
    }
    {
      mgbe::Device jumbo(argv[1], 1024, 9000);
      assert(jumbo.queue().info().slot_size == 16384);
    }
    {
      // Non-IP broadcast fallback must stay on queue zero with multiple queues.
      mgbe::Device d(argv[1], 64, 1500, 4);
      d.enable_mac_loopback();
      d.start();
      auto* bytes = d.queue(3).tx_buffer();
      assert(bytes);
      std::memset(bytes, 0x5a, 60);
      std::memset(bytes, 0xff, 6);
      std::memcpy(bytes + 6, d.queue(3).info().mac, 6);
      bytes[12] = 0x88;
      bytes[13] = 0xb5;
      assert(d.queue(3).submit(60));
      bool received = false;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (!received && std::chrono::steady_clock::now() < deadline) {
        for (uint32_t q = 0; q < 4; ++q) {
          mgbe::Packet packet{};
          if (d.queue(q).receive(&packet, 1)) {
            assert(q == 0 && packet.length == 60 && packet.data[42] == 0x5a);
            d.queue(q).release(packet);
            received = true;
          }
        }
      }
      assert(received);
      auto rss = d.rss();
      assert([&] {
        try {
          d.configure_rss(rss);
        } catch (const std::system_error& error) {
          return error.code().value() == EINVAL;
        }
        return false;
      }());
    }
    int ready[2];
    assert(pipe(ready) == 0);
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
      close(ready[0]);
      mgbe::Device d(argv[1], 64, 1500, 4);
      d.enable_mac_loopback();
      d.start();
      for (uint32_t q = 0; q < 4; ++q) {
        auto* bytes = d.queue(q).tx_buffer();
        assert(bytes);
        std::memset(bytes, 0, 60);
        std::memcpy(bytes, d.queue(q).info().mac, 6);
        bytes[12] = 0x88;
        bytes[13] = 0xb5;
        assert(d.queue(q).submit(60));
      }
      const char byte = 'R';
      if (write(ready[1], &byte, 1) != 1) _exit(2);
      for (;;) pause();
    }
    close(ready[1]);
    pollfd wait{ready[0], POLLIN, 0};
    if (poll(&wait, 1, 5000) != 1 || !(wait.revents & POLLIN)) {
      kill(child, SIGKILL);
      waitpid(child, nullptr, 0);
      throw std::runtime_error("child failed to configure PMD");
    }
    char byte = 0;
    assert(read(ready[0], &byte, 1) == 1 && byte == 'R');
    close(ready[0]);
    assert(kill(child, SIGKILL) == 0);
    int status = 0;
    assert(waitpid(child, &status, 0) == child && WIFSIGNALED(status));
    { mgbe::Device recovered(argv[1], 64, 1500, 4); }
    std::cout
        << "10 exclusive cycles, read-only mappings, VMA lifetime and SIGKILL recovery passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
