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

////////////////////////////////////////////////////////////////////////////////
///
///  \brief 802.3x link-level pause (flow control) inspection.
///
///  Pause is enabled by default on many NIC ports and caps raw-Ethernet
///  throughput without incrementing any drop counter: the receiver asserts XOFF
///  on a conservative internal watermark, the sender obeys, and the link idles.
///  On a 400 GbE loopback this cost 21.7% of line rate while rx_discards_phy and
///  rx_out_of_buffer both stayed at 0, which is indistinguishable from a
///  transmitter that simply cannot go faster.
///
///  Pause is not always a misconfiguration: a peer that cannot absorb line rate
///  (an FPGA or another device with shallow buffers) asserts it by design, and
///  disabling pause there converts the throttling into drops. So these helpers
///  report which end asserted it and leave the judgement to the operator.
///
///  The pause counters are not reachable through DPDK's xstats on mlx5, so these
///  helpers go to the kernel netdev directly via the ethtool ioctl interface.
///  That works alongside a bound PMD because mlx5 is a bifurcated driver and
///  keeps its netdev while DPDK drives the port.
///
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>
#include <string>

namespace daqiri {

/// 802.3x pause configuration and frame counters for one netdev.
struct PauseState {
  bool config_valid = false;  ///< pause parameters were readable
  bool autoneg = false;
  bool rx_enabled = false;
  bool tx_enabled = false;

  /// Cumulative pause frames since boot, or -1 when the NIC does not expose the
  /// counter. Non-zero means pause has actually fired on this link, which is the
  /// difference between a real problem and a latent one. Unlike the `ethtool -A`
  /// rx/tx knobs, these counters are unambiguous about wire direction: rx counts
  /// frames received (the peer throttling this port's transmit) and tx counts
  /// frames sent (this port's receive path throttling the peer).
  int64_t rx_pause_frames = -1;
  int64_t tx_pause_frames = -1;

  bool enabled() const {
    return rx_enabled || tx_enabled;
  }
  bool asserted() const {
    return rx_pause_frames > 0 || tx_pause_frames > 0;
  }
};

/// Resolve the kernel netdev backing a PCIe address ("0000:05:00.0"), or "" if
/// there is none. A port with several netdevs (multi-port PF) returns the first.
std::string netdev_for_pci(const std::string& pci_addr);

/// Read pause parameters and pause frame counters. Every field is best-effort;
/// an unreadable netdev yields a default-constructed state rather than an error,
/// since none of this is required for the data path to run.
PauseState read_pause_state(const std::string& netdev);

/// Warn when pause is enabled on a port, and record the current counters as the
/// baseline for this run. Call once per port during init: the warning lands
/// before any throughput number can be misread, and the baseline is what makes
/// the end-of-run report about this run rather than about everything since boot.
/// No-op when the netdev cannot be read.
void check_pause_at_init(const std::string& netdev, int port_id);

/// Log the pause frames exchanged during this run, and warn if there were any,
/// naming the end that asserted pause without claiming it was wrong to. Call
/// alongside the end-of-run stats dump, where this is the only signal that flow
/// control throttled the run. Reports the delta against the baseline taken by
/// check_pause_at_init; without a baseline it reports totals since boot.
void log_pause_counters(const std::string& netdev, int port_id);

}  // namespace daqiri
