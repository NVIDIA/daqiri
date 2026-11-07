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
      auto bad = mgbe_pmd_config{MGBE_PMD_ABI, 65, 1500, 0};
      assert(ioctl(fd, MGBE_PMD_IOC_CONFIGURE, &bad) == -1 && errno == EINVAL);
      assert(ioctl(fd, MGBE_PMD_IOC_CONFIGURE, reinterpret_cast<void*>(1)) == -1 &&
             errno == EFAULT);
      assert(ioctl(fd, 0x12345678) == -1 && errno == ENOTTY);
      assert(ioctl(fd, MGBE_PMD_IOC_MAC_LOOPBACK) == -1 && errno == EINVAL);
      auto config = mgbe_pmd_config{MGBE_PMD_ABI, 64, 1500, 0};
      assert(ioctl(fd, MGBE_PMD_IOC_CONFIGURE, &config) == 0);
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
      {
        mgbe::Device d(argv[1], 64, 1500);
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
    int ready[2];
    assert(pipe(ready) == 0);
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
      close(ready[0]);
      mgbe::Device d(argv[1], 64, 1500);
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
    { mgbe::Device recovered(argv[1], 64, 1500); }
    std::cout
        << "10 exclusive cycles, read-only mappings, VMA lifetime and SIGKILL recovery passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
