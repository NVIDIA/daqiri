// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
// SPDX-License-Identifier: Apache-2.0
//
// AF_XDP branch extracted from the MGBE frame-echo benchmarking example.
// afxdp.cpp, afxdp.hpp, benchmark_packet.hpp and xdp_redirect.bpf.c are verbatim
// copies of the sources used by that example. The command-line wrapper below
// exposes only its single-packet AF_XDP echo path.
//
// Usage:
//   ./afxdp_busy_poll_echo IFACE XDP_OBJECT LOCAL_IP PORT CPU BUSY_US SECONDS [FRAME_BYTES]
// Example configuration (frame length excludes FCS):
//   ./afxdp_busy_poll_echo mgbe0_0 ./xdp_redirect.bpf.o 192.0.2.1 9000 9 20 30 60
// BUSY_US=0 selects the original ring-polling/IRQ control case.
//
// This worker attaches native XDP, binds queue 0 in zero-copy mode, and returns
// each matching frame with Ethernet/IPv4/UDP endpoints swapped. Configure RSS
// to queue 0 after XSK bind: the vendor driver reopens the interface on bind.
// IRQ/NAPI CPU affinity and coalescing are external experiment controls.
// The sender/receiver on the peer collects TX-CQE and RX hardware timestamps;
// this echo worker does not calculate RTT. Optional trace markers are enabled
// only by MGBE_TRACE_MARKER, pointing at an existing tracefs trace_marker file.
//
// Source-only example, outside the DAQIRI CMake build. Its AF_XDP dependency is
// libxdp/libbpf; the matching NIC driver must support native AF_XDP zero-copy.

#include "afxdp.hpp"
#include "benchmark_packet.hpp"
#include <sched.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <csignal>
#include <ctime>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
volatile sig_atomic_t stopped = 0;
void signal_stop(int) {
  stopped = 1;
}
uint64_t now() {
  timespec t{};
  clock_gettime(CLOCK_MONOTONIC_RAW, &t);
  return uint64_t(t.tv_sec) * 1000000000ULL + t.tv_nsec;
}
uint32_t number(const char* text) {
  std::string value(text);
  size_t used = 0;
  const auto result = std::stoull(value, &used);
  if (value.empty() || value[0] == '-' || used != value.size() || result > UINT32_MAX)
    throw std::invalid_argument("invalid number: " + value);
  return static_cast<uint32_t>(result);
}
struct TraceMarker {
  int fd = -1;
  TraceMarker() {
    if (const char* path = std::getenv("MGBE_TRACE_MARKER")) {
      fd = open(path, O_WRONLY | O_CLOEXEC);
      if (fd < 0) throw std::runtime_error("cannot open experiment trace marker");
    }
  }
  ~TraceMarker() {
    if (fd >= 0) close(fd);
  }
  void mark(const char* text, size_t length) {
    if (fd >= 0 && write(fd, text, length) != ssize_t(length))
      throw std::runtime_error("cannot write experiment trace marker");
  }
};
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 8 && argc != 9) {
      std::cerr << "Usage: " << argv[0]
                << " IFACE XDP_OBJECT LOCAL_IP PORT CPU BUSY_US SECONDS [FRAME_BYTES]\n";
      return 2;
    }
    mgbe::bench::Endpoint local{};
    if (inet_pton(AF_INET, argv[3], &local.ip) != 1)
      throw std::invalid_argument("invalid local IPv4 address");
    const auto port = number(argv[4]), cpu = number(argv[5]);
    const auto busy_us = number(argv[6]), seconds = number(argv[7]);
    const auto frame_bytes = argc == 9 ? number(argv[8]) : 60U;
    if (!port || port > UINT16_MAX || cpu >= CPU_SETSIZE || !seconds || frame_bytes < 60 ||
        frame_bytes > 1514)
      throw std::invalid_argument("invalid port, CPU, duration or frame length");
    local.port = static_cast<uint16_t>(port);
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    CPU_SET(cpu, &affinity);
    if (sched_setaffinity(0, sizeof(affinity), &affinity))
      throw std::runtime_error("cannot pin worker CPU");
    std::signal(SIGINT, signal_stop);
    std::signal(SIGTERM, signal_stop);
    TraceMarker trace;
    mgbe::AfXdp xdp(argv[1], argv[2], local.ip, local.port, busy_us);
    std::array<uint8_t, 6> mac{};
    std::copy_n(xdp.mac(), mac.size(), mac.begin());
    std::cout << "{\"event\":\"afxdp\",\"zero_copy\":true,\"native\":true,\"queue\":0,"
                 "\"busy_poll_us\":"
              << busy_us << "}" << std::endl;
    std::cout << "{\"event\":\"ready\",\"backend\":\"xdp\"}" << std::endl;

    uint64_t rx = 0, tx = 0, malformed = 0, backpressure = 0;
    const uint64_t deadline = now() + uint64_t(seconds) * 1000000000ULL;
    while (!stopped && now() < deadline) {
      const uint8_t* data = nullptr;
      uint32_t bytes;
      xdp.reclaim();
      // receive() peeks UMEM. When empty, nonzero BUSY_US invokes the kernel's
      // NAPI busy loop with a zero-byte, nonblocking recvfrom(); see afxdp.cpp.
      if (!xdp.receive(data, bytes)) continue;
      ++rx;
      trace.mark("user_rx\n", 8);
      uint8_t* reply = xdp.tx_buffer();
      if (!reply) {
        ++backpressure;
      } else if (bytes != frame_bytes ||
                 !mgbe::bench::echo_reply(data, bytes, reply, 9216, mac.data(), local)) {
        ++malformed;
        xdp.cancel_tx();
      } else {
        trace.mark("user_tx\n", 8);
        if (xdp.submit(bytes)) {
          trace.mark("user_posted\n", 12);
          ++tx;
        } else {
          ++backpressure;
        }
      }
      // TX uses its own UMEM chunks; release RX only after the reply copy.
      xdp.release();
    }
    std::cout << "{\"backend\":\"xdp\",\"rx\":" << rx << ",\"tx\":" << tx
              << ",\"malformed\":" << malformed << ",\"backpressure\":" << backpressure << "}\n";
    return backpressure ? 1 : 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
