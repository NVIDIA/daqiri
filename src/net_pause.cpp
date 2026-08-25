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

#include "src/net_pause.h"

#include <dirent.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include <daqiri/logging.hpp>

namespace daqiri {
namespace {

/// mlx5 exposes nine pause counters; these two are the ones that answer "did
/// flow control throttle this link". The duration counters are deliberately left
/// out: their unit is not documented consistently, so reporting them invites
/// arithmetic nobody can check.
constexpr const char* kRxPauseCounter = "rx_pause_ctrl_phy";
constexpr const char* kTxPauseCounter = "tx_pause_ctrl_phy";

/// RAII wrapper so every early return closes the ioctl socket.
class IoctlSocket {
 public:
  IoctlSocket() : fd_(socket(AF_INET, SOCK_DGRAM, 0)) {}
  ~IoctlSocket() {
    if (fd_ >= 0) {
      close(fd_);
    }
  }
  IoctlSocket(const IoctlSocket&) = delete;
  IoctlSocket& operator=(const IoctlSocket&) = delete;

  bool valid() const {
    return fd_ >= 0;
  }

  /// Run an ethtool command against a netdev. `data` must point at a struct
  /// whose first member is the ethtool command word.
  bool call(const std::string& netdev, void* data) const {
    if (fd_ < 0) {
      return false;
    }
    struct ifreq ifr {};
    strncpy(ifr.ifr_name, netdev.c_str(), IFNAMSIZ - 1);
    ifr.ifr_data = reinterpret_cast<char*>(data);
    return ioctl(fd_, SIOCETHTOOL, &ifr) == 0;
  }

 private:
  int fd_;
};

/// Read the two pause counters by name. Returns without touching `state` when
/// the NIC does not expose a named statistics set, which leaves the counters at
/// -1 ("not exposed") rather than a misleading 0.
void read_pause_counters_into(const IoctlSocket& sock, const std::string& netdev,
                              PauseState& state) {
  // ethtool_sset_info ends in a flexible array, so it cannot be nested in another
  // struct; size a raw buffer for the header plus the single count we asked for.
  std::vector<char> sset_buf(sizeof(struct ethtool_sset_info) + sizeof(uint32_t), 0);
  auto* sset_info = reinterpret_cast<struct ethtool_sset_info*>(sset_buf.data());
  sset_info->cmd = ETHTOOL_GSSET_INFO;
  sset_info->sset_mask = 1ULL << ETH_SS_STATS;
  if (!sock.call(netdev, sset_info)) {
    return;
  }
  // sset_mask comes back cleared when the set is unsupported; data[0] then holds
  // nothing meaningful.
  const uint32_t n_stats = sset_info->sset_mask ? sset_info->data[0] : 0;
  if (n_stats == 0) {
    return;
  }

  const size_t strings_size = sizeof(struct ethtool_gstrings) + n_stats * ETH_GSTRING_LEN;
  std::vector<char> strings_buf(strings_size, 0);
  auto* strings = reinterpret_cast<struct ethtool_gstrings*>(strings_buf.data());
  strings->cmd = ETHTOOL_GSTRINGS;
  strings->string_set = ETH_SS_STATS;
  strings->len = n_stats;
  if (!sock.call(netdev, strings)) {
    return;
  }

  const size_t stats_size = sizeof(struct ethtool_stats) + n_stats * sizeof(uint64_t);
  std::vector<char> stats_buf(stats_size, 0);
  auto* stats = reinterpret_cast<struct ethtool_stats*>(stats_buf.data());
  stats->cmd = ETHTOOL_GSTATS;
  stats->n_stats = n_stats;
  if (!sock.call(netdev, stats)) {
    return;
  }

  // The kernel may return fewer strings than it advertised; trust the smaller.
  const uint32_t count = std::min(n_stats, strings->len);
  for (uint32_t i = 0; i < count; i++) {
    const char* name = reinterpret_cast<const char*>(strings->data) + i * ETH_GSTRING_LEN;
    // Names are not guaranteed NUL-terminated when they fill the field.
    if (strncmp(name, kRxPauseCounter, ETH_GSTRING_LEN) == 0) {
      state.rx_pause_frames = static_cast<int64_t>(stats->data[i]);
    } else if (strncmp(name, kTxPauseCounter, ETH_GSTRING_LEN) == 0) {
      state.tx_pause_frames = static_cast<int64_t>(stats->data[i]);
    }
  }
}

/// Per-port counter baseline taken at init. Process-global because there is only
/// ever one active Engine, and because the DPDK stats dump is static and cannot
/// reach engine state. Guarded since init and teardown may run on other threads.
std::mutex g_baseline_mutex;
std::map<int, PauseState> g_baseline;

}  // namespace

std::string netdev_for_pci(const std::string& pci_addr) {
  if (pci_addr.empty()) {
    return "";
  }
  const std::string net_dir = "/sys/bus/pci/devices/" + pci_addr + "/net";
  std::string netdev;
  if (DIR* d = opendir(net_dir.c_str())) {
    for (struct dirent* e = readdir(d); e != nullptr; e = readdir(d)) {
      if (e->d_name[0] != '.') {
        netdev = e->d_name;
        break;
      }
    }
    closedir(d);
  }
  return netdev;
}

PauseState read_pause_state(const std::string& netdev) {
  PauseState state;
  if (netdev.empty()) {
    return state;
  }

  const IoctlSocket sock;
  if (!sock.valid()) {
    return state;
  }

  struct ethtool_pauseparam pause {};
  pause.cmd = ETHTOOL_GPAUSEPARAM;
  if (sock.call(netdev, &pause)) {
    state.config_valid = true;
    state.autoneg = pause.autoneg != 0;
    state.rx_enabled = pause.rx_pause != 0;
    state.tx_enabled = pause.tx_pause != 0;
  }

  read_pause_counters_into(sock, netdev, state);
  return state;
}

void check_pause_at_init(const std::string& netdev, int port_id) {
  const PauseState state = read_pause_state(netdev);
  if (!state.config_valid) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(g_baseline_mutex);
    g_baseline[port_id] = state;
  }

  if (!state.enabled()) {
    return;
  }
  // Deliberately no per-direction claim about the configuration: ethtool and
  // systemd-networkd document their rx/tx pause naming with opposite senses, so
  // the actionable advice is to disable both rather than to reason about one.
  DAQIRI_LOG_WARN(
      "Flow control: port {} ({}) has 802.3x pause enabled (rx {}, tx {}). A paused link idles "
      "instead of dropping, so this can prevent achieving higher rates with no drop counter to "
      "reveal it. Disable both directions with 'ethtool -A {} rx off tx off' if this link is "
      "meant to be lossy; leave it on for lossless RoCE/PFC fabrics and for peers that cannot "
      "absorb line rate, where disabling it turns the throttling into drops.",
      port_id, netdev, state.rx_enabled ? "on" : "off", state.tx_enabled ? "on" : "off", netdev);
}

void log_pause_counters(const std::string& netdev, int port_id) {
  const PauseState state = read_pause_state(netdev);
  if (state.rx_pause_frames < 0 && state.tx_pause_frames < 0) {
    return;  // NIC does not expose them; saying nothing beats printing zeros
  }

  // These counters are cumulative since boot, so the totals say nothing about
  // this run -- a link with pause disabled still carries millions from earlier.
  // Subtract the init baseline to report this run, matching the surrounding
  // xstats dump, which is also per-run.
  int64_t rx = state.rx_pause_frames;
  int64_t tx = state.tx_pause_frames;
  bool per_run = false;
  {
    std::lock_guard<std::mutex> lock(g_baseline_mutex);
    const auto it = g_baseline.find(port_id);
    if (it != g_baseline.end()) {
      per_run = true;
      if (rx >= 0 && it->second.rx_pause_frames >= 0) {
        rx -= it->second.rx_pause_frames;
      }
      if (tx >= 0 && it->second.tx_pause_frames >= 0) {
        tx -= it->second.tx_pause_frames;
      }
    }
  }

  DAQIRI_LOG_INFO("      {}:\t\t{}", kRxPauseCounter, rx);
  DAQIRI_LOG_INFO("      {}:\t\t{}", kTxPauseCounter, tx);
  if (rx <= 0 && tx <= 0) {
    return;
  }

  // The counters, unlike the ethtool -A knobs, do say which end asked for
  // backpressure, and that is what separates a misconfigured port from a peer
  // doing exactly what it should. Report the direction and stop there: whether
  // the pause was legitimate depends on the peer, which DAQIRI cannot see.
  const char* cause;
  if (rx > 0 && tx > 0) {
    cause =
        "Both ends asserted pause: the link partner throttled this port's transmit and this "
        "port throttled the partner.";
  } else if (rx > 0) {
    cause =
        "The link partner asserted pause, throttling this port's transmit. That is working "
        "backpressure when the peer cannot absorb line rate (an FPGA or other shallow-buffer "
        "device) and a problem only when it should have kept up.";
  } else {
    cause =
        "This port asserted pause, so its own receive path -- or a conservative NIC watermark "
        "-- fell behind the sender.";
  }
  DAQIRI_LOG_WARN(
      "Flow control: port {} ({}) exchanged {} pause frames {} (received {}, sent {}), so the "
      "link spent time paused and throughput below line rate here may be flow control rather "
      "than the transmitter. {}",
      port_id, netdev, std::max<int64_t>(rx, 0) + std::max<int64_t>(tx, 0),
      per_run ? "during this run" : "since boot", rx, tx, cause);
}

}  // namespace daqiri
