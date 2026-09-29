/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
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

#include "src/net_neighbor.h"

#include <arpa/inet.h>
#include <errno.h>
#include <linux/neighbour.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <functional>

#include <daqiri/logging.hpp>

namespace daqiri {
namespace {

constexpr uint16_t kUsableNeighborStates =
    NUD_REACHABLE | NUD_STALE | NUD_DELAY | NUD_PROBE | NUD_PERMANENT | NUD_NOARP;

struct NetlinkRequest {
  std::array<uint8_t, 512> storage{};

  nlmsghdr* header() {
    return reinterpret_cast<nlmsghdr*>(storage.data());
  }

  bool add_attr(uint16_t type, const void* data, size_t size) {
    nlmsghdr* hdr = header();
    const size_t offset = NLMSG_ALIGN(hdr->nlmsg_len);
    const size_t attr_size = RTA_LENGTH(size);
    const size_t next = offset + RTA_ALIGN(attr_size);
    if (next > storage.size()) {
      return false;
    }
    auto* attr = reinterpret_cast<rtattr*>(storage.data() + offset);
    attr->rta_type = type;
    attr->rta_len = static_cast<unsigned short>(attr_size);
    std::memcpy(RTA_DATA(attr), data, size);
    hdr->nlmsg_len = static_cast<uint32_t>(next);
    return true;
  }
};

class NetlinkSocket {
 public:
  NetlinkSocket() {
    fd_ = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd_ < 0) {
      error_ = errno;
      return;
    }

    sockaddr_nl local{};
    local.nl_family = AF_NETLINK;
    local.nl_groups = RTMGRP_NEIGH;
    if (bind(fd_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
      error_ = errno;
      close(fd_);
      fd_ = -1;
      return;
    }

    socklen_t len = sizeof(local);
    if (getsockname(fd_, reinterpret_cast<sockaddr*>(&local), &len) != 0) {
      error_ = errno;
      close(fd_);
      fd_ = -1;
      return;
    }
    pid_ = local.nl_pid;
  }

  ~NetlinkSocket() {
    if (fd_ >= 0) {
      close(fd_);
    }
  }

  NetlinkSocket(const NetlinkSocket&) = delete;
  NetlinkSocket& operator=(const NetlinkSocket&) = delete;

  bool valid() const {
    return fd_ >= 0;
  }
  int fd() const {
    return fd_;
  }
  uint32_t next_sequence() {
    return ++sequence_;
  }
  uint32_t pid() const {
    return pid_;
  }
  int error() const {
    return error_;
  }

  bool send(const nlmsghdr* hdr) const {
    sockaddr_nl kernel{};
    kernel.nl_family = AF_NETLINK;
    iovec iov{const_cast<nlmsghdr*>(hdr), hdr->nlmsg_len};
    msghdr msg{};
    msg.msg_name = &kernel;
    msg.msg_namelen = sizeof(kernel);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    return sendmsg(fd_, &msg, 0) == static_cast<ssize_t>(hdr->nlmsg_len);
  }

 private:
  int fd_ = -1;
  int error_ = 0;
  uint32_t pid_ = 0;
  uint32_t sequence_ = 0;
};

struct RouteResult {
  int ifindex = 0;
  uint32_t next_hop_network = 0;
};

enum class NeighborState { MISSING, PENDING, USABLE, FAILED };

struct NeighborResult {
  NeighborState state = NeighborState::MISSING;
  std::array<char, 6> mac{};
};

void parse_attrs(rtattr** attrs, size_t count, rtattr* attr, int len) {
  std::fill(attrs, attrs + count, nullptr);
  while (RTA_OK(attr, len)) {
    if (attr->rta_type < count) {
      attrs[attr->rta_type] = attr;
    }
    attr = RTA_NEXT(attr, len);
  }
}

int receive_for_sequence(NetlinkSocket& sock, uint32_t sequence,
                         const std::function<bool(const nlmsghdr*)>& consume) {
  std::array<uint8_t, 8192> buffer{};
  while (true) {
    const ssize_t size = recv(sock.fd(), buffer.data(), buffer.size(), 0);
    if (size < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -errno;
    }
    int remaining = static_cast<int>(size);
    for (auto* hdr = reinterpret_cast<nlmsghdr*>(buffer.data()); NLMSG_OK(hdr, remaining);
         hdr = NLMSG_NEXT(hdr, remaining)) {
      if (hdr->nlmsg_seq != sequence) {
        continue;
      }
      if (hdr->nlmsg_type == NLMSG_ERROR) {
        const auto* error = static_cast<const nlmsgerr*>(NLMSG_DATA(hdr));
        return error->error;
      }
      if (consume(hdr)) {
        return 0;
      }
      if (hdr->nlmsg_type == NLMSG_DONE) {
        return 0;
      }
    }
  }
}

int lookup_route(NetlinkSocket& sock, int requested_ifindex, uint32_t dst_network,
                 RouteResult* result) {
  NetlinkRequest request;
  nlmsghdr* hdr = request.header();
  hdr->nlmsg_len = NLMSG_LENGTH(sizeof(rtmsg));
  hdr->nlmsg_type = RTM_GETROUTE;
  hdr->nlmsg_flags = NLM_F_REQUEST;
  hdr->nlmsg_seq = sock.next_sequence();
  hdr->nlmsg_pid = sock.pid();
  auto* route = static_cast<rtmsg*>(NLMSG_DATA(hdr));
  route->rtm_family = AF_INET;
  route->rtm_dst_len = 32;
  route->rtm_table = RT_TABLE_UNSPEC;

  if (!request.add_attr(RTA_DST, &dst_network, sizeof(dst_network)) ||
      !request.add_attr(RTA_OIF, &requested_ifindex, sizeof(requested_ifindex))) {
    return -EMSGSIZE;
  }
  if (!sock.send(hdr)) {
    return -errno;
  }

  bool found = false;
  const int status = receive_for_sequence(sock, hdr->nlmsg_seq, [&](const nlmsghdr* response) {
    if (response->nlmsg_type != RTM_NEWROUTE) {
      return false;
    }
    const auto* message = static_cast<const rtmsg*>(NLMSG_DATA(response));
    if (message->rtm_family != AF_INET || message->rtm_type != RTN_UNICAST) {
      return false;
    }
    int attr_len = RTM_PAYLOAD(response);
    std::array<rtattr*, RTA_MAX + 1> attrs{};
    parse_attrs(attrs.data(), attrs.size(), RTM_RTA(message), attr_len);
    if (attrs[RTA_OIF] == nullptr) {
      return false;
    }
    std::memcpy(&result->ifindex, RTA_DATA(attrs[RTA_OIF]), sizeof(result->ifindex));
    result->next_hop_network = dst_network;
    if (attrs[RTA_GATEWAY] != nullptr) {
      std::memcpy(&result->next_hop_network, RTA_DATA(attrs[RTA_GATEWAY]),
                  sizeof(result->next_hop_network));
    }
    found = true;
    return true;
  });
  if (status != 0) {
    return status;
  }
  return found ? 0 : -ENETUNREACH;
}

bool parse_neighbor(const nlmsghdr* hdr, int ifindex, uint32_t address_network,
                    NeighborResult* result) {
  if (hdr->nlmsg_type != RTM_NEWNEIGH && hdr->nlmsg_type != RTM_GETNEIGH) {
    return false;
  }
  const auto* message = static_cast<const ndmsg*>(NLMSG_DATA(hdr));
  if (message->ndm_family != AF_INET || message->ndm_ifindex != ifindex) {
    return false;
  }

  int attr_len = NLMSG_PAYLOAD(hdr, sizeof(*message));
  std::array<rtattr*, NDA_MAX + 1> attrs{};
  auto* first_attr = reinterpret_cast<rtattr*>(
      reinterpret_cast<uint8_t*>(const_cast<ndmsg*>(message)) + NLMSG_ALIGN(sizeof(*message)));
  parse_attrs(attrs.data(), attrs.size(), first_attr, attr_len);
  if (attrs[NDA_DST] == nullptr || RTA_PAYLOAD(attrs[NDA_DST]) < sizeof(address_network)) {
    return false;
  }
  uint32_t candidate = 0;
  std::memcpy(&candidate, RTA_DATA(attrs[NDA_DST]), sizeof(candidate));
  if (candidate != address_network) {
    return false;
  }

  if ((message->ndm_state & NUD_FAILED) != 0) {
    result->state = NeighborState::FAILED;
  } else if ((message->ndm_state & kUsableNeighborStates) != 0 && attrs[NDA_LLADDR] != nullptr &&
             RTA_PAYLOAD(attrs[NDA_LLADDR]) == result->mac.size()) {
    std::memcpy(result->mac.data(), RTA_DATA(attrs[NDA_LLADDR]), result->mac.size());
    result->state = NeighborState::USABLE;
  } else {
    result->state = NeighborState::PENDING;
  }
  return true;
}

int lookup_neighbor(NetlinkSocket& sock, int ifindex, uint32_t address_network,
                    NeighborResult* result) {
  NetlinkRequest request;
  nlmsghdr* hdr = request.header();
  hdr->nlmsg_len = NLMSG_LENGTH(sizeof(ndmsg));
  hdr->nlmsg_type = RTM_GETNEIGH;
  hdr->nlmsg_flags = NLM_F_REQUEST;
  hdr->nlmsg_seq = sock.next_sequence();
  hdr->nlmsg_pid = sock.pid();
  auto* message = static_cast<ndmsg*>(NLMSG_DATA(hdr));
  message->ndm_family = AF_INET;
  message->ndm_ifindex = ifindex;
  if (!request.add_attr(NDA_DST, &address_network, sizeof(address_network))) {
    return -EMSGSIZE;
  }
  if (!sock.send(hdr)) {
    return -errno;
  }

  bool found = false;
  const int status = receive_for_sequence(sock, hdr->nlmsg_seq, [&](const nlmsghdr* response) {
    found = parse_neighbor(response, ifindex, address_network, result);
    return found;
  });
  if (status == -ENOENT) {
    result->state = NeighborState::MISSING;
    return 0;
  }
  if (status != 0) {
    return status;
  }
  if (!found) {
    result->state = NeighborState::MISSING;
  }
  return 0;
}

int trigger_neighbor(NetlinkSocket& sock, int ifindex, uint32_t address_network) {
  NetlinkRequest request;
  nlmsghdr* hdr = request.header();
  hdr->nlmsg_len = NLMSG_LENGTH(sizeof(ndmsg));
  hdr->nlmsg_type = RTM_NEWNEIGH;
  hdr->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE;
  hdr->nlmsg_seq = sock.next_sequence();
  hdr->nlmsg_pid = sock.pid();
  auto* message = static_cast<ndmsg*>(NLMSG_DATA(hdr));
  message->ndm_family = AF_INET;
  message->ndm_ifindex = ifindex;
  message->ndm_state = NUD_NONE;
  message->ndm_flags = NTF_USE;
  if (!request.add_attr(NDA_DST, &address_network, sizeof(address_network))) {
    return -EMSGSIZE;
  }
  if (!sock.send(hdr)) {
    return -errno;
  }
  return receive_for_sequence(sock, hdr->nlmsg_seq, [](const nlmsghdr*) { return false; });
}

const char* ipv4_text(uint32_t network, char* buffer, size_t size) {
  in_addr addr{network};
  return inet_ntop(AF_INET, &addr, buffer, static_cast<socklen_t>(size));
}

}  // namespace

Status resolve_ipv4_neighbor(const std::string& netdev, uint32_t dst_host, char* mac,
                             uint32_t timeout_ms) {
  if (mac == nullptr) {
    return Status::NULL_PTR;
  }
  if (netdev.empty() || dst_host == INADDR_ANY || dst_host == INADDR_BROADCAST ||
      IN_MULTICAST(dst_host) || IN_BADCLASS(dst_host) || timeout_ms == 0) {
    return Status::INVALID_PARAMETER;
  }

  const unsigned int ifindex = if_nametoindex(netdev.c_str());
  if (ifindex == 0) {
    DAQIRI_LOG_ERROR("ARP: kernel netdev '{}' does not exist: {}", netdev, strerror(errno));
    return Status::CONNECT_FAILURE;
  }

  const int control_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (control_fd < 0) {
    DAQIRI_LOG_ERROR("ARP: failed to inspect kernel netdev '{}': {}", netdev, strerror(errno));
    return Status::CONNECT_FAILURE;
  }
  ifreq request{};
  std::snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", netdev.c_str());
  if (ioctl(control_fd, SIOCGIFFLAGS, &request) != 0) {
    const int error = errno;
    close(control_fd);
    DAQIRI_LOG_ERROR("ARP: failed to read state for kernel netdev '{}': {}", netdev,
                     strerror(error));
    return Status::CONNECT_FAILURE;
  }
  close(control_fd);
  if ((request.ifr_flags & IFF_UP) == 0) {
    DAQIRI_LOG_ERROR("ARP: kernel netdev '{}' is down; bring it up before resolving neighbors",
                     netdev);
    return Status::CONNECT_FAILURE;
  }

  NetlinkSocket sock;
  if (!sock.valid()) {
    DAQIRI_LOG_ERROR("ARP: failed to open rtnetlink socket: {}", strerror(sock.error()));
    return Status::CONNECT_FAILURE;
  }

  const uint32_t dst_network = htonl(dst_host);
  RouteResult route;
  int error = lookup_route(sock, static_cast<int>(ifindex), dst_network, &route);
  if (error != 0) {
    DAQIRI_LOG_ERROR("ARP: no IPv4 route to destination on netdev '{}': {}", netdev,
                     strerror(-error));
    return Status::CONNECT_FAILURE;
  }
  if (route.ifindex != static_cast<int>(ifindex)) {
    DAQIRI_LOG_ERROR("ARP: route selected ifindex {} instead of netdev '{}' (ifindex {})",
                     route.ifindex, netdev, ifindex);
    return Status::CONNECT_FAILURE;
  }

  NeighborResult neighbor;
  error = lookup_neighbor(sock, route.ifindex, route.next_hop_network, &neighbor);
  if (error != 0) {
    DAQIRI_LOG_ERROR("ARP: failed to query the neighbor table on '{}': {}", netdev,
                     strerror(-error));
    return Status::CONNECT_FAILURE;
  }
  if (neighbor.state == NeighborState::USABLE) {
    std::memcpy(mac, neighbor.mac.data(), neighbor.mac.size());
    return Status::SUCCESS;
  }

  error = trigger_neighbor(sock, route.ifindex, route.next_hop_network);
  if (error != 0) {
    DAQIRI_LOG_ERROR(
        "ARP: failed to trigger neighbor resolution on '{}': {}. Check CAP_NET_ADMIN/root access",
        netdev, strerror(-error));
    return Status::CONNECT_FAILURE;
  }

  // Query once after the trigger to close the ACK/notification race, then wait
  // on RTMGRP_NEIGH for state changes until the caller's deadline.
  error = lookup_neighbor(sock, route.ifindex, route.next_hop_network, &neighbor);
  if (error != 0) {
    return Status::CONNECT_FAILURE;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  std::array<uint8_t, 8192> buffer{};
  while (neighbor.state != NeighborState::USABLE && neighbor.state != NeighborState::FAILED) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      break;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    pollfd descriptor{sock.fd(), POLLIN, 0};
    const int wait_ms = static_cast<int>(std::min<int64_t>(remaining.count() + 1, INT_MAX));
    const int ready = poll(&descriptor, 1, wait_ms);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      DAQIRI_LOG_ERROR("ARP: poll failed on '{}': {}", netdev, strerror(errno));
      return Status::CONNECT_FAILURE;
    }
    if (ready == 0) {
      break;
    }
    const ssize_t size = recv(sock.fd(), buffer.data(), buffer.size(), 0);
    if (size < 0) {
      if (errno == EINTR) {
        continue;
      }
      DAQIRI_LOG_ERROR("ARP: failed to receive neighbor update on '{}': {}", netdev,
                       strerror(errno));
      return Status::CONNECT_FAILURE;
    }
    int bytes = static_cast<int>(size);
    for (auto* hdr = reinterpret_cast<nlmsghdr*>(buffer.data()); NLMSG_OK(hdr, bytes);
         hdr = NLMSG_NEXT(hdr, bytes)) {
      if (hdr->nlmsg_seq == 0) {
        parse_neighbor(hdr, route.ifindex, route.next_hop_network, &neighbor);
      }
    }
  }

  if (neighbor.state == NeighborState::USABLE) {
    std::memcpy(mac, neighbor.mac.data(), neighbor.mac.size());
    return Status::SUCCESS;
  }

  char next_hop[INET_ADDRSTRLEN] = {};
  DAQIRI_LOG_ERROR(
      "ARP: neighbor {} on '{}' did not resolve within {} ms; ensure the peer is reachable and "
      "rx.flow_isolation is true so ARP remains on the kernel path",
      ipv4_text(route.next_hop_network, next_hop, sizeof(next_hop)), netdev, timeout_ms);
  return Status::NOT_READY;
}

}  // namespace daqiri
