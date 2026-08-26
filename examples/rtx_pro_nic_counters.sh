#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# The NIC's own counters, read the same way for every RTX PRO benchmark backend.
#
# Sourced by run_rtx_pro_bench.sh and run_rtx_pro_mq_bench.sh so that there is
# one implementation of "did this traffic reach the cable, and how fast" rather
# than a per-runner reimplementation that has to be trusted separately.
#
# Why ethtool and not the engine's own log: every backend here drives an mlx5
# port whose kernel netdev stays up (the DPDK PMD is bifurcated, so the port is
# never handed away to vfio), which means `ethtool -S` answers for all of them,
# including a port that has been moved into a network namespace. The engine's
# stderr dump, by contrast, reports counters that are cumulative since boot, in a
# format that differs per engine, and it is absent entirely for the socket and
# RoCE paths.
#
# Why *_bytes_phy: it counts what actually occupies the wire -- frame, CRC,
# preamble and interframe gap -- so a rate derived from it is directly comparable
# to the port's nominal line rate. And because it is the NIC counting, the result
# does not depend on when the benchmark started and stopped its own timer.
#
# Callers set before use:
#   WIRE_TX_IFACE, WIRE_RX_IFACE  the two ends of the cable (required)
#   WIRE_TX_NS, WIRE_RX_NS        namespace each port lives in ("" = this one)
#   NIC_VALIDATION_LOG            file to append the per-cell record to

WIRE_TX_IFACE="${WIRE_TX_IFACE:-}"
WIRE_RX_IFACE="${WIRE_RX_IFACE:-}"
WIRE_TX_NS="${WIRE_TX_NS:-}"
WIRE_RX_NS="${WIRE_RX_NS:-}"
NIC_VALIDATION_LOG="${NIC_VALIDATION_LOG:-/dev/null}"

# Rates from the most recent nic_report, for the caller's CSV.
WIRE_TX_GBPS=0
WIRE_RX_GBPS=0

# `ip netns exec` where iproute2 exists, nsenter against the same handle where it
# does not (the benchmark container has no iproute2). Only defined here if the
# sourcing script has not already provided it.
if ! declare -F netns_prefix >/dev/null 2>&1; then
  netns_prefix() {
    local ns="$1"
    if command -v ip >/dev/null 2>&1; then
      echo "ip netns exec $ns"
    else
      echo "nsenter --net=/var/run/netns/$ns"
    fi
  }
fi

# The port's nominal line rate in whole Gb/s, or 0 when it cannot be read. Used
# only to catch a rate that cannot be true.
nic_link_gbps() {
  local ns="$1" iface="$2" pre=() mbps
  [[ -n "$iface" ]] || { echo 0; return 0; }
  [[ -n "$ns" ]] && read -r -a pre <<< "$(netns_prefix "$ns")"
  mbps="$("${pre[@]}" cat "/sys/class/net/$iface/speed" 2>/dev/null)"
  [[ "$mbps" =~ ^[0-9]+$ ]] || { echo 0; return 0; }
  awk -v m="$mbps" 'BEGIN { printf "%d", m / 1000 }'
}

# One port's counters: "tx_bytes tx_frames rx_bytes rx_frames pause_frames".
nic_counters() {
  local ns="$1" iface="$2" pre=()
  [[ -n "$iface" ]] || { echo "0 0 0 0 0"; return 0; }
  [[ -n "$ns" ]] && read -r -a pre <<< "$(netns_prefix "$ns")"
  "${pre[@]}" ethtool -S "$iface" 2>/dev/null | awk '
    { gsub(/,/, "", $2) }
    $1 == "tx_bytes_phy:"      { tb = $2 }
    $1 == "tx_packets_phy:"    { tp = $2 }
    $1 == "rx_bytes_phy:"      { rb = $2 }
    $1 == "rx_packets_phy:"    { rp = $2 }
    $1 == "tx_pause_ctrl_phy:" { pause += $2 }
    $1 == "rx_pause_ctrl_phy:" { pause += $2 }
    END { printf "%d %d %d %d %d\n", tb+0, tp+0, rb+0, rp+0, pause+0 }
  '
}

# Both ends of the cable at one instant, with a timestamp, so that two snapshots
# are enough to compute a rate. These counters are cumulative since boot, so only
# the difference between two snapshots describes a single run.
#
# Fields: epoch_ns, then five for the TX port, then five for the RX port.
nic_snapshot() {
  echo "$(date +%s%N) $(nic_counters "$WIRE_TX_NS" "$WIRE_TX_IFACE") $(nic_counters "$WIRE_RX_NS" "$WIRE_RX_IFACE")"
}

# Turn two snapshots into this run's wire rates, append the record to
# NIC_VALIDATION_LOG, and set WIRE_TX_GBPS / WIRE_RX_GBPS.
#
# Returns non-zero when the NIC contradicts the application: either the frames
# never crossed the cable, or 802.3x pause throttled the sender. Both are
# failures rather than warnings, because in both cases a plausible-looking
# throughput number is produced that does not mean what it appears to.
#
# $3 is how many units (packets or messages) the application believes it moved,
# used to decide whether silence on the wire is suspicious. $4 is the length of
# the application's own measured window, which is what the byte totals are
# divided by: the gap between two snapshots is necessarily wider than the run,
# since it also spans process start-up and teardown, and dividing by it would
# understate the rate by however long the engine took to come up.
NIC_UNMEASURABLE_WARNED=0
nic_report() {
  local before="$1" after="$2" app_units="${3:-0}" app_secs="${4:-0}"
  WIRE_TX_GBPS=0
  WIRE_RX_GBPS=0
  [[ -n "$before" && -n "$after" ]] || return 0

  # With no port to read there is nothing to verify. Say so rather than reporting
  # zero frames, which is indistinguishable from traffic that missed the cable.
  if [[ -z "$WIRE_TX_IFACE" && -z "$WIRE_RX_IFACE" ]]; then
    if [[ "$NIC_UNMEASURABLE_WARNED" == "0" ]]; then
      echo "WARNING: no interface names available, so the NIC cannot confirm this run" >&2
      echo "         reached the wire. Set WIRE_TX_IFACE/WIRE_RX_IFACE or run discovery." >&2
      NIC_UNMEASURABLE_WARNED=1
    fi
    return 0
  fi

  local b=() a=()
  read -r -a b <<< "$before"
  read -r -a a <<< "$after"
  [[ "${#b[@]}" -eq 11 && "${#a[@]}" -eq 11 ]] || return 0

  local window rate_secs
  window="$(awk -v x="${b[0]}" -v y="${a[0]}" 'BEGIN { printf "%.4f", (y - x) / 1e9 }')"
  # Too short a window to divide by.
  awk -v s="$window" 'BEGIN { exit !(s > 0.01) }' || return 0
  rate_secs="$app_secs"
  awk -v s="$rate_secs" 'BEGIN { exit !(s > 0.01) }' || rate_secs="$window"

  local d_tx_bytes=$(( a[1] - b[1] ))
  local d_tx_frames=$(( a[2] - b[2] ))
  local d_rx_bytes=$(( a[8] - b[8] ))
  local d_rx_frames=$(( a[9] - b[9] ))
  local d_pause=$(( (a[5] - b[5]) + (a[10] - b[10]) ))
  local lost=$(( d_tx_frames - d_rx_frames ))

  WIRE_TX_GBPS="$(awk -v x="$d_tx_bytes" -v s="$rate_secs" 'BEGIN { printf "%.3f", x * 8 / s / 1e9 }')"
  WIRE_RX_GBPS="$(awk -v x="$d_rx_bytes" -v s="$rate_secs" 'BEGIN { printf "%.3f", x * 8 / s / 1e9 }')"

  # A rate above the port's line rate is arithmetic, not physics. The byte totals
  # are exactly what crossed the wire between the two snapshots, so the only thing
  # that can be wrong is the divisor: $app_secs bounds that traffic only when the
  # application is the sole driver of the port for precisely its own timed window.
  # The ibverbs backend breaks that -- its transmit side is a separate, longer
  # lived process -- and the run then reports more than 400 Gb/s on a 400 GbE
  # port. Fall back to the whole snapshot window, which by construction contains
  # all of the traffic and so can only understate the rate.
  local link
  link="$(nic_link_gbps "$WIRE_TX_NS" "$WIRE_TX_IFACE")"
  if [[ "${link:-0}" -gt 0 ]] &&
     awk -v r="$WIRE_TX_GBPS" -v l="$link" 'BEGIN { exit !(r > l * 1.02) }'; then
    echo "WARNING: the NIC counters give ${WIRE_TX_GBPS} Gbps over the application's" >&2
    echo "         ${rate_secs}s window, which a ${link} Gb/s port cannot do. The port was" >&2
    echo "         driven outside that window, so the rate is being reported over the" >&2
    echo "         full ${window}s between counter snapshots instead, and is a lower" >&2
    echo "         bound. Use the sampled mean for this backend's sustained rate." >&2
    rate_secs="$window"
    WIRE_TX_GBPS="$(awk -v x="$d_tx_bytes" -v s="$rate_secs" 'BEGIN { printf "%.3f", x * 8 / s / 1e9 }')"
    WIRE_RX_GBPS="$(awk -v x="$d_rx_bytes" -v s="$rate_secs" 'BEGIN { printf "%.3f", x * 8 / s / 1e9 }')"
  fi

  {
    printf 'nic counter window %.2fs, rate over the run of %.2fs\n' "$window" "$rate_secs"
    printf 'nic %s tx: %d frames %d bytes %s Gbps\n' \
      "${WIRE_TX_IFACE:-?}" "$d_tx_frames" "$d_tx_bytes" "$WIRE_TX_GBPS"
    printf 'nic %s rx: %d frames %d bytes %s Gbps\n' \
      "${WIRE_RX_IFACE:-?}" "$d_rx_frames" "$d_rx_bytes" "$WIRE_RX_GBPS"
    # Frames the TX port put on the cable that the RX port never saw. This is the
    # cable's own loss, independent of anything the application or the engine
    # reports, and on a direct loopback it should be zero.
    printf 'nic frames sent but not received: %d\n' "$lost"
    printf 'nic pause frames: %d\n' "$d_pause"
  } >> "$NIC_VALIDATION_LOG"

  if [[ "$d_pause" -gt 0 ]]; then
    echo "ERROR: the NIC counted $d_pause 802.3x pause frames during this run." >&2
    echo "       Pause throttles the sender without touching any drop counter, so the" >&2
    echo "       result would look clean and simply be low. Disable it on both ports:" >&2
    echo "       ethtool -A ${WIRE_TX_IFACE:-<tx-iface>} rx off tx off" >&2
    echo "       ethtool -A ${WIRE_RX_IFACE:-<rx-iface>} rx off tx off" >&2
    return 1
  fi

  if [[ "${app_units:-0}" -gt 1000 && ( "$d_tx_frames" -lt 100 || "$d_rx_frames" -lt 100 ) ]]; then
    echo "ERROR: the application moved $app_units units, but the NIC counted only" >&2
    echo "       $d_tx_frames frames out of ${WIRE_TX_IFACE:-?} and $d_rx_frames into ${WIRE_RX_IFACE:-?}." >&2
    echo "       Nothing crossed the cable, so this is not a wire rate." >&2
    if [[ -n "$WIRE_TX_NS" || -n "$WIRE_RX_NS" ]]; then
      echo "       Check that ${WIRE_TX_IFACE:-?} is in ${WIRE_TX_NS:-?} and ${WIRE_RX_IFACE:-?} in ${WIRE_RX_NS:-?}." >&2
    else
      echo "       Check the cable, the destination MAC, and the RX flow rules." >&2
    fi
    return 1
  fi

  return 0
}
