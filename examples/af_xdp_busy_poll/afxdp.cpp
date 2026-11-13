// SPDX-License-Identifier: Apache-2.0
#include "afxdp.hpp"
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <xdp/xsk.h>
#include <linux/if_link.h>
#include <linux/if_xdp.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace mgbe {
namespace {
constexpr uint32_t chunk = 4096, rx_chunks = 8192, tx_chunks = 4096, ring = 4096;
constexpr size_t bytes = size_t(rx_chunks + tx_chunks) * chunk;
struct Endpoint {
  uint32_t ip;
  uint16_t port;
  uint8_t mac[6];
};
void check(int error, const char* operation) {
  if (error)
    throw std::runtime_error(std::string(operation) + ": " +
                             std::strerror(error < 0 ? -error : error));
}
}  // namespace

struct AfXdp::Impl {
  void* memory = MAP_FAILED;
  xsk_umem* umem = nullptr;
  xsk_socket* socket = nullptr;
  xsk_ring_prod fill{}, tx{};
  xsk_ring_cons completion{}, rx{};
  bpf_object* object = nullptr;
  int ifindex = 0;
  bool attached = false, held = false, reserved = false;
  uint64_t held_addr = 0, tx_addr = 0;
  uint32_t rx_index = 0, held_count = 0, tx_count = 0;
  std::array<uint64_t, 64> held_addresses{}, tx_addresses{};
  uint64_t completed = 0;
  std::array<uint8_t, 6> mac{};
  std::vector<uint64_t> free_tx, pending_rx;
  uint32_t busy_poll_us = 0;
  ~Impl() {
    if (attached) bpf_xdp_detach(ifindex, XDP_FLAGS_DRV_MODE, nullptr);
    if (socket) xsk_socket__delete(socket);
    if (umem) xsk_umem__delete(umem);
    if (object) bpf_object__close(object);
    if (memory != MAP_FAILED) munmap(memory, bytes);
  }
  void refill() {
    while (!pending_rx.empty()) {
      const uint32_t count =
          std::min<uint32_t>({64, uint32_t(pending_rx.size()), xsk_prod_nb_free(&fill, 64)});
      if (!count) break;
      uint32_t index;
      if (xsk_ring_prod__reserve(&fill, count, &index) != count)
        throw std::logic_error("fill reservation failed");
      for (uint32_t i = 0; i < count; ++i) {
        *xsk_ring_prod__fill_addr(&fill, index + i) = pending_rx.back();
        pending_rx.pop_back();
      }
      xsk_ring_prod__submit(&fill, count);
    }
  }
};

AfXdp::AfXdp(const std::string& interface, const std::string& program, uint32_t local_ip,
             uint16_t local_port, uint32_t busy_poll_us)
    : impl_(std::make_unique<Impl>()) {
  auto& s = *impl_;
  s.ifindex = if_nametoindex(interface.c_str());
  if (!s.ifindex) throw std::runtime_error("AF_XDP interface does not exist");
  for (uint32_t mode : {uint32_t(XDP_FLAGS_DRV_MODE), uint32_t(XDP_FLAGS_SKB_MODE)}) {
    uint32_t id = 0;
    check(bpf_xdp_query_id(s.ifindex, mode, &id), "query existing XDP program");
    if (id) throw std::runtime_error("interface already has an XDP program");
  }
  int control = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (control < 0) throw std::runtime_error("cannot query AF_XDP MAC");
  ifreq req{};
  if (interface.size() >= sizeof(req.ifr_name)) {
    close(control);
    throw std::runtime_error("interface name too long");
  }
  std::memcpy(req.ifr_name, interface.c_str(), interface.size() + 1);
  const int result = ioctl(control, SIOCGIFHWADDR, &req);
  const int saved_errno = errno;
  close(control);
  if (result) check(saved_errno, "query MAC");
  std::memcpy(s.mac.data(), req.ifr_hwaddr.sa_data, 6);
  s.memory = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (s.memory == MAP_FAILED) check(errno, "allocate UMEM");
  std::memset(s.memory, 0, bytes);
  s.free_tx.reserve(tx_chunks);
  s.pending_rx.reserve(rx_chunks);
  for (uint32_t i = 0; i < tx_chunks; ++i) s.free_tx.push_back(uint64_t(rx_chunks + i) * chunk);
  const xsk_umem_config config{ring, ring, chunk, 0, 0};
  check(xsk_umem__create(&s.umem, s.memory, bytes, &s.fill, &s.completion, &config), "create UMEM");
  uint32_t index = 0;
  if (xsk_ring_prod__reserve(&s.fill, ring, &index) != ring)
    throw std::runtime_error("cannot prime UMEM");
  for (uint32_t i = 0; i < ring; ++i)
    *xsk_ring_prod__fill_addr(&s.fill, index + i) = uint64_t(i) * chunk;
  xsk_ring_prod__submit(&s.fill, ring);
  xsk_socket_config socket_config{};
  socket_config.rx_size = ring;
  socket_config.tx_size = ring;
  socket_config.libbpf_flags = XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD;
  socket_config.xdp_flags = XDP_FLAGS_DRV_MODE;
  socket_config.bind_flags = XDP_ZEROCOPY | XDP_USE_NEED_WAKEUP;
  check(xsk_socket__create(&s.socket, interface.c_str(), 0, s.umem, &s.rx, &s.tx, &socket_config),
        "bind native AF_XDP zero-copy socket");
  xdp_options options{};
  socklen_t options_size = sizeof(options);
  if (getsockopt(xsk_socket__fd(s.socket), SOL_XDP, XDP_OPTIONS, &options, &options_size) ||
      !(options.flags & XDP_OPTIONS_ZEROCOPY))
    throw std::runtime_error("AF_XDP zero-copy was not established");
  for (uint32_t i = ring; i < rx_chunks; ++i) s.pending_rx.push_back(uint64_t(i) * chunk);
  s.refill();
  if (busy_poll_us) {
    int us = busy_poll_us, one = 1, budget = 64;
    check(
        setsockopt(xsk_socket__fd(s.socket), SOL_SOCKET, SO_BUSY_POLL, &us, sizeof(us)) ? errno : 0,
        "SO_BUSY_POLL");
    check(setsockopt(xsk_socket__fd(s.socket), SOL_SOCKET, SO_PREFER_BUSY_POLL, &one, sizeof(one))
              ? errno
              : 0,
          "SO_PREFER_BUSY_POLL");
    check(setsockopt(xsk_socket__fd(s.socket), SOL_SOCKET, SO_BUSY_POLL_BUDGET, &budget,
                     sizeof(budget))
              ? errno
              : 0,
          "SO_BUSY_POLL_BUDGET");
    s.busy_poll_us = busy_poll_us;
  }
  s.object = bpf_object__open_file(program.c_str(), nullptr);
  const int object_error = static_cast<int>(libbpf_get_error(s.object));
  if (object_error) {
    s.object = nullptr;
    check(object_error, "open redirect BPF object");
  }
  if (!s.object) throw std::runtime_error("cannot open BPF object");
  check(bpf_object__load(s.object), "load redirect BPF object");
  const int endpoint_map = bpf_object__find_map_fd_by_name(s.object, "endpoints");
  const int xsks_map = bpf_object__find_map_fd_by_name(s.object, "xsks");
  const uint32_t zero = 0;
  Endpoint endpoint{local_ip, local_port, {}};
  std::memcpy(endpoint.mac, s.mac.data(), 6);
  check(bpf_map_update_elem(endpoint_map, &zero, &endpoint, BPF_ANY),
        "configure redirect endpoint");
  const int socket_fd = xsk_socket__fd(s.socket);
  check(bpf_map_update_elem(xsks_map, &zero, &socket_fd, BPF_ANY), "configure XSK map");
  bpf_program* bpf = bpf_object__find_program_by_name(s.object, "echo_redirect");
  if (!bpf) throw std::runtime_error("missing redirect BPF program");
  check(bpf_xdp_attach(s.ifindex, bpf_program__fd(bpf),
                       XDP_FLAGS_DRV_MODE | XDP_FLAGS_UPDATE_IF_NOEXIST, nullptr),
        "attach native XDP");
  s.attached = true;
}

AfXdp::~AfXdp() = default;
bool AfXdp::receive(const uint8_t*& data, uint32_t& length) {
  auto& s = *impl_;
  if (s.held || s.held_count) throw std::logic_error("RX buffer is still held");
  if (!xsk_ring_cons__peek(&s.rx, 1, &s.rx_index)) {
    if (s.busy_poll_us) {
      // Linux AF_XDP recvmsg invokes sk_busy_loop without consuming payload;
      // packet bytes continue to arrive exclusively through the UMEM RX ring.
      if (recvfrom(xsk_socket__fd(s.socket), nullptr, 0, MSG_DONTWAIT, nullptr, nullptr) < 0 &&
          errno != EAGAIN && errno != ENOBUFS && errno != EINTR)
        check(errno, "AF_XDP busy poll");
    }
    return false;
  }
  const auto* descriptor = xsk_ring_cons__rx_desc(&s.rx, s.rx_index);
  s.held_addr = descriptor->addr;
  const uint64_t offset = xsk_umem__add_offset_to_addr(descriptor->addr);
  length = descriptor->len;
  if (offset >= uint64_t(rx_chunks) * chunk || length > chunk || (offset % chunk) + length > chunk)
    throw std::runtime_error("invalid AF_XDP RX descriptor");
  data = static_cast<const uint8_t*>(xsk_umem__get_data(s.memory, offset));
  s.held = true;
  return true;
}
void AfXdp::release() {
  auto& s = *impl_;
  if (!s.held) throw std::logic_error("no RX buffer to release");
  s.pending_rx.push_back(xsk_umem__extract_addr(s.held_addr) & ~(uint64_t(chunk) - 1));
  xsk_ring_cons__release(&s.rx, 1);
  s.held = false;
  s.refill();
}
void AfXdp::reclaim() {
  auto& s = *impl_;
  uint32_t index;
  const auto count = xsk_ring_cons__peek(&s.completion, 64, &index);
  for (uint32_t i = 0; i < count; ++i) {
    const auto address = *xsk_ring_cons__comp_addr(&s.completion, index + i);
    if (address < uint64_t(rx_chunks) * chunk || address >= bytes || address % chunk)
      throw std::runtime_error("invalid AF_XDP TX completion");
    s.free_tx.push_back(address);
  }
  if (count) {
    xsk_ring_cons__release(&s.completion, count);
    s.completed += count;
  }
  s.refill();
  if (xsk_ring_prod__needs_wakeup(&s.fill)) {
    pollfd p{xsk_socket__fd(s.socket), POLLIN, 0};
    if (poll(&p, 1, 0) < 0 && errno != EINTR) check(errno, "wake RX refill");
  }
}
uint8_t* AfXdp::tx_buffer() {
  auto& s = *impl_;
  if (s.reserved || s.tx_count) throw std::logic_error("TX buffer is already reserved");
  if (s.free_tx.empty()) return nullptr;
  s.tx_addr = s.free_tx.back();
  s.free_tx.pop_back();
  s.reserved = true;
  return static_cast<uint8_t*>(s.memory) + s.tx_addr;
}
void AfXdp::cancel_tx() {
  auto& s = *impl_;
  if (!s.reserved) throw std::logic_error("no reserved TX buffer");
  s.free_tx.push_back(s.tx_addr);
  s.reserved = false;
}
bool AfXdp::submit(uint32_t length) {
  auto& s = *impl_;
  if (!s.reserved || length > chunk) throw std::logic_error("invalid TX submission");
  uint32_t index;
  if (!xsk_ring_prod__reserve(&s.tx, 1, &index)) {
    cancel_tx();
    return false;
  }
  auto* descriptor = xsk_ring_prod__tx_desc(&s.tx, index);
  descriptor->addr = s.tx_addr;
  descriptor->len = length;
  descriptor->options = 0;
  xsk_ring_prod__submit(&s.tx, 1);
  s.reserved = false;
  if (sendto(xsk_socket__fd(s.socket), nullptr, 0, MSG_DONTWAIT, nullptr, 0) < 0 &&
      errno != EAGAIN && errno != ENOBUFS && errno != EINTR)
    check(errno, "wake AF_XDP TX");
  return true;
}
uint32_t AfXdp::receive_burst(Frame* frames, uint32_t capacity) {
  auto& s = *impl_;
  if (s.held || s.held_count || !capacity || capacity > 64)
    throw std::logic_error("invalid RX burst state");
  const uint32_t count = xsk_ring_cons__peek(&s.rx, capacity, &s.rx_index);
  for (uint32_t i = 0; i < count; ++i) {
    const auto* d = xsk_ring_cons__rx_desc(&s.rx, s.rx_index + i);
    const uint64_t offset = xsk_umem__add_offset_to_addr(d->addr);
    if (d->options || offset >= uint64_t(rx_chunks) * chunk || d->len > chunk ||
        (offset % chunk) + d->len > chunk)
      throw std::runtime_error("invalid AF_XDP RX burst descriptor");
    s.held_addresses[i] = d->addr;
    frames[i] = {static_cast<const uint8_t*>(xsk_umem__get_data(s.memory, offset)), d->len};
  }
  s.held_count = count;
  return count;
}
void AfXdp::release_burst() {
  auto& s = *impl_;
  if (!s.held_count) throw std::logic_error("no RX burst to release");
  for (uint32_t i = 0; i < s.held_count; ++i)
    s.pending_rx.push_back(xsk_umem__extract_addr(s.held_addresses[i]) & ~(uint64_t(chunk) - 1));
  xsk_ring_cons__release(&s.rx, s.held_count);
  s.held_count = 0;
  s.refill();
}
uint32_t AfXdp::reserve_tx_burst(uint8_t** frames, uint32_t count) {
  auto& s = *impl_;
  if (s.reserved || s.tx_count || !count || count > 64)
    throw std::logic_error("invalid TX burst state");
  count = std::min<uint32_t>(count, s.free_tx.size());
  for (uint32_t i = 0; i < count; ++i) {
    s.tx_addresses[i] = s.free_tx.back();
    s.free_tx.pop_back();
    frames[i] = static_cast<uint8_t*>(s.memory) + s.tx_addresses[i];
  }
  s.tx_count = count;
  return count;
}
bool AfXdp::submit_tx_burst(uint32_t length) {
  auto& s = *impl_;
  if (!s.tx_count || length > chunk || length < 14)
    throw std::logic_error("invalid TX burst submission");
  uint32_t index;
  const uint32_t count = s.tx_count;
  if (xsk_ring_prod__reserve(&s.tx, count, &index) != count) {
    for (uint32_t i = 0; i < count; ++i) s.free_tx.push_back(s.tx_addresses[i]);
    s.tx_count = 0;
    return false;
  }
  for (uint32_t i = 0; i < count; ++i) {
    auto* d = xsk_ring_prod__tx_desc(&s.tx, index + i);
    d->addr = s.tx_addresses[i];
    d->len = length;
    d->options = 0;
  }
  xsk_ring_prod__submit(&s.tx, count);
  s.tx_count = 0;
  if (xsk_ring_prod__needs_wakeup(&s.tx) &&
      sendto(xsk_socket__fd(s.socket), nullptr, 0, MSG_DONTWAIT, nullptr, 0) < 0 &&
      errno != EAGAIN && errno != ENOBUFS && errno != EINTR)
    check(errno, "wake AF_XDP TX burst");
  return true;
}
uint64_t AfXdp::outstanding_tx() const {
  return tx_chunks - impl_->free_tx.size();
}
uint64_t AfXdp::completed_tx() const {
  return impl_->completed;
}
AfXdp::Stats AfXdp::statistics() const {
  xdp_statistics stats{};
  socklen_t size = sizeof(stats);
  if (getsockopt(xsk_socket__fd(impl_->socket), SOL_XDP, XDP_STATISTICS, &stats, &size))
    check(errno, "AF_XDP statistics");
  return {stats.rx_dropped,   stats.rx_invalid_descs,         stats.tx_invalid_descs,
          stats.rx_ring_full, stats.rx_fill_ring_empty_descs, stats.tx_ring_empty_descs};
}
const uint8_t* AfXdp::mac() const {
  return impl_->mac.data();
}
}  // namespace mgbe
