// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <arpa/inet.h>
#include <endian.h>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace mgbe::bench {
inline void put16(uint8_t* p, uint16_t v) {
  v = htons(v);
  std::memcpy(p, &v, 2);
}
inline uint16_t get16(const uint8_t* p) {
  uint16_t v;
  std::memcpy(&v, p, 2);
  return ntohs(v);
}
inline void put64(uint8_t* p, uint64_t v) {
  v = htobe64(v);
  std::memcpy(p, &v, 8);
}
inline uint64_t get64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return be64toh(v);
}
inline uint8_t pattern(uint64_t sequence, uint32_t offset) {
  // Vary every cache line and every packet, including high sequence bits.
  uint64_t x = sequence ^ (uint64_t(offset) * 0x9e3779b97f4a7c15ULL);
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  return static_cast<uint8_t>(x ^ (x >> 29));
}
inline void fill(uint8_t* p, uint32_t n, uint64_t sequence, uint64_t stamp, uint32_t verify_every) {
  if (n < 16) throw std::invalid_argument("benchmark payload is too small");
  put64(p, sequence);
  put64(p + 8, stamp);
  if (verify_every && sequence % verify_every == 0)
    for (uint32_t i = 16; i < n; ++i) p[i] = pattern(sequence, i);
}
inline bool verify(const uint8_t* p, uint32_t n, uint32_t every) {
  if (n < 16) return false;
  const uint64_t sequence = get64(p);
  if (every && sequence % every == 0)
    for (uint32_t i = 16; i < n; ++i)
      if (p[i] != pattern(sequence, i)) return false;
  return true;
}
struct Endpoint {
  uint32_t ip;
  uint16_t port;
};
inline uint16_t checksum(const uint8_t* p, uint32_t n) {
  uint32_t sum = 0;
  for (uint32_t i = 0; i < n; i += 2) sum += (uint32_t(p[i]) << 8) | p[i + 1];
  while (sum >> 16) sum = (sum & 0xffffU) + (sum >> 16);
  return static_cast<uint16_t>(~sum);
}
inline void header(uint8_t* frame, uint32_t bytes, const uint8_t* source_mac,
                   const uint8_t* dest_mac, Endpoint source, Endpoint dest) {
  std::memcpy(frame, dest_mac, 6);
  std::memcpy(frame + 6, source_mac, 6);
  put16(frame + 12, 0x0800);
  std::memset(frame + 14, 0, 28);
  frame[14] = 0x45;
  put16(frame + 16, bytes - 14);
  frame[22] = 64;
  frame[23] = 17;
  std::memcpy(frame + 26, &source.ip, 4);
  std::memcpy(frame + 30, &dest.ip, 4);
  put16(frame + 24, checksum(frame + 14, 20));
  put16(frame + 34, source.port);
  put16(frame + 36, dest.port);
  put16(frame + 38, bytes - 34);
  // Zero UDP checksum is valid for IPv4. No hardware checksum/TSO assumptions.
}
inline bool udp_payload(uint8_t* frame, uint32_t bytes, Endpoint local, uint8_t*& payload,
                        uint32_t& length, Endpoint& source) {
  if (bytes < 42 || get16(frame + 12) != 0x0800 || frame[14] != 0x45 || frame[23] != 17 ||
      (get16(frame + 20) & 0x3fffU) || checksum(frame + 14, 20) != 0)
    return false;
  uint32_t ip;
  std::memcpy(&ip, frame + 30, 4);
  const uint32_t total = get16(frame + 16), udp = get16(frame + 38);
  if (ip != local.ip || get16(frame + 36) != local.port || total < 28 || total > bytes - 14 ||
      udp < 8 || udp != total - 20)
    return false;
  std::memcpy(&source.ip, frame + 26, 4);
  source.port = get16(frame + 34);
  payload = frame + 42;
  length = udp - 8;
  return true;
}
// Shared by the AF_PACKET baseline and PMD. Swapping both pseudo-header
// addresses and ports preserves a nonzero UDP checksum as well as the payload.
inline bool echo_reply(const uint8_t* request, uint32_t bytes, uint8_t* reply, uint32_t capacity,
                       const uint8_t* mac, Endpoint local) {
  if (bytes < 60 || bytes > capacity || std::memcmp(request, mac, 6)) return false;
  uint8_t* payload = nullptr;
  uint32_t length = 0;
  Endpoint source{};
  if (!udp_payload(const_cast<uint8_t*>(request), bytes, local, payload, length, source) ||
      get16(request + 16) + 14U != bytes)
    return false;
  std::memcpy(reply, request, bytes);
  std::memcpy(reply, request + 6, 6);
  std::memcpy(reply + 6, request, 6);
  std::memcpy(reply + 26, request + 30, 4);
  std::memcpy(reply + 30, request + 26, 4);
  std::memcpy(reply + 34, request + 36, 2);
  std::memcpy(reply + 36, request + 34, 2);
  return true;
}
// ARP responder only: TX peers are supplied explicitly with --peer-mac.
inline bool arp_reply(const uint8_t* request, uint32_t bytes, uint8_t* reply, const uint8_t* mac,
                      Endpoint local) {
  if (bytes < 42 || get16(request + 12) != 0x0806 || get16(request + 14) != 1 ||
      get16(request + 16) != 0x0800 || request[18] != 6 || request[19] != 4 ||
      get16(request + 20) != 1 || std::memcmp(request + 38, &local.ip, 4))
    return false;
  std::memset(reply, 0, 60);
  std::memcpy(reply, request + 22, 6);
  std::memcpy(reply + 6, mac, 6);
  put16(reply + 12, 0x0806);
  put16(reply + 14, 1);
  put16(reply + 16, 0x0800);
  reply[18] = 6;
  reply[19] = 4;
  put16(reply + 20, 2);
  std::memcpy(reply + 22, mac, 6);
  std::memcpy(reply + 28, &local.ip, 4);
  std::memcpy(reply + 32, request + 22, 10);
  return true;
}
}  // namespace mgbe::bench
