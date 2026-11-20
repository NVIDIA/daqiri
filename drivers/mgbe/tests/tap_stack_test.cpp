// SPDX-License-Identifier: Apache-2.0
// Explicit root-only integration test. Run in a disposable network namespace.
#include "mgbe_tap.hpp"
#include <arpa/inet.h>
#include <net/if.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace {
void put16(uint8_t* p, uint16_t n) {
  p[0] = n >> 8;
  p[1] = n;
}
uint16_t get16(const uint8_t* p) {
  return (uint16_t(p[0]) << 8) | p[1];
}
uint16_t checksum(const uint8_t* p, size_t n) {
  uint32_t sum = 0;
  for (size_t i = 0; i < n; i += 2) sum += get16(p + i);
  while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
  return ~sum;
}
template <typename F>
void until(F predicate) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!predicate()) {
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
void address(const std::string& name) {
  int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  assert(fd >= 0);
  ifreq req{};
  std::memcpy(req.ifr_name, name.c_str(), name.size() + 1);
  auto* a = reinterpret_cast<sockaddr_in*>(&req.ifr_addr);
  a->sin_family = AF_INET;
  assert(inet_pton(AF_INET, "198.18.77.1", &a->sin_addr) == 1);
  assert(!ioctl(fd, SIOCSIFADDR, &req));
  a = reinterpret_cast<sockaddr_in*>(&req.ifr_netmask);
  a->sin_family = AF_INET;
  assert(inet_pton(AF_INET, "255.255.255.252", &a->sin_addr) == 1);
  assert(!ioctl(fd, SIOCSIFNETMASK, &req));
  close(fd);
}
}  // namespace
int main() {
  assert(geteuid() == 0);
  // Never install test addresses/routes in the caller's network namespace.
  assert(!unshare(CLONE_NEWNET));
  mgbe::KernelTap::Options o;
  o.name = "mgbe-test0";
  o.mac = {2, 0, 0, 0, 0, 1};
  cpu_set_t cpus;
  CPU_ZERO(&cpus);
  assert(!sched_getaffinity(0, sizeof(cpus), &cpus));
  for (int i = 0; i < CPU_SETSIZE; ++i)
    if (CPU_ISSET(i, &cpus)) {
      o.worker_cpu = i;
      break;
    }
  assert(!if_nametoindex(o.name.c_str()));
  mgbe::KernelTap tap(o);
  address(o.name);
  bool refused = false;
  try {
    mgbe::KernelTap duplicate(o);
  } catch (const std::system_error&) {
    refused = true;
  }
  assert(refused && if_nametoindex(o.name.c_str()));
  std::array<uint8_t, 60> arp{};
  const std::array<uint8_t, 6> peer{2, 0, 0, 0, 0, 2};
  const std::array<uint8_t, 4> source_ip{198, 18, 77, 2}, target_ip{198, 18, 77, 1};
  std::memset(arp.data(), 255, 6);
  std::memcpy(arp.data() + 6, peer.data(), 6);
  put16(arp.data() + 12, 0x0806);
  put16(arp.data() + 14, 1);
  put16(arp.data() + 16, 0x0800);
  arp[18] = 6;
  arp[19] = 4;
  put16(arp.data() + 20, 1);
  std::memcpy(arp.data() + 22, peer.data(), 6);
  std::memcpy(arp.data() + 28, source_ip.data(), 4);
  std::memcpy(arp.data() + 38, target_ip.data(), 4);
  assert(tap.send_to_kernel(arp.data(), arp.size()));
  std::array<uint8_t, 1514> reply{};
  size_t length = 0;
  until([&] {
    return tap.receive_from_kernel(reply.data(), reply.size(), length) && length >= 42 &&
           get16(reply.data() + 12) == 0x0806 && get16(reply.data() + 20) == 2;
  });
  assert(!std::memcmp(reply.data(), peer.data(), 6));
  assert(!std::memcmp(reply.data() + 22, o.mac.data(), 6));
  assert(!std::memcmp(reply.data() + 28, target_ip.data(), 4));
  std::array<uint8_t, 74> ping{};
  std::memcpy(ping.data(), o.mac.data(), 6);
  std::memcpy(ping.data() + 6, peer.data(), 6);
  put16(ping.data() + 12, 0x0800);
  ping[14] = 0x45;
  ping[22] = 64;
  ping[23] = 1;
  put16(ping.data() + 16, 60);
  std::memcpy(ping.data() + 26, source_ip.data(), 4);
  std::memcpy(ping.data() + 30, target_ip.data(), 4);
  put16(ping.data() + 24, checksum(ping.data() + 14, 20));
  ping[34] = 8;
  put16(ping.data() + 38, 0x9764);
  put16(ping.data() + 40, 1234);
  for (size_t i = 42; i < ping.size(); ++i) ping[i] = i;
  put16(ping.data() + 36, checksum(ping.data() + 34, 40));
  assert(tap.send_to_kernel(ping.data(), ping.size()));
  until([&] {
    return tap.receive_from_kernel(reply.data(), reply.size(), length) && length >= 74 &&
           get16(reply.data() + 12) == 0x0800 && reply[23] == 1 && reply[34] == 0;
  });
  assert(!std::memcmp(reply.data(), peer.data(), 6));
  assert(!std::memcmp(reply.data() + 26, target_ip.data(), 4));
  assert(!std::memcmp(reply.data() + 30, source_ip.data(), 4));
  assert(!checksum(reply.data() + 34, 40));
  assert(!std::memcmp(reply.data() + 38, ping.data() + 38, 36));
  tap.stop();
  assert(!if_nametoindex(o.name.c_str()));
  const auto stats = tap.stats();
  assert(stats.to_kernel_delivered >= 2 && stats.from_kernel_delivered >= 2 && !stats.error);
  auto bad = o;
  bad.worker_cpu = CPU_SETSIZE - 1;
  refused = false;
  try {
    mgbe::KernelTap invalid_cpu(bad);
  } catch (const std::system_error&) {
    refused = true;
  }
  assert(refused && !if_nametoindex(o.name.c_str()));
  std::cout << "Real Linux TAP ARP, ICMP echo, exclusive creation and failure cleanup passed\n";
}
