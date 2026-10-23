#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# SPDX-License-Identifier: Apache-2.0
# Snapshot physical RX counters around one cross-host application run.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
[[ -n "${RX_ENV:-}" ]] && source "${RX_ENV}"
RX_IFACE="${RX_IFACE:-enP4p3s0f1np1}"
RESULTS_DIR="${RESULTS_DIR:-${REPO_ROOT}/resnet-results}"
RUN_ID="${RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}"
mkdir -p "${RESULTS_DIR}"
PHY="${RESULTS_DIR}/${RUN_ID}.phy"

phy_counter() {
  ethtool -S "$1" 2>/dev/null | awk -v k="$2" '$1 ~ k":" {s += $2} END {print s+0}'
}

for command in ethtool awk; do
  command -v "${command}" >/dev/null || { echo "ERROR: ${command} is required" >&2; exit 1; }
done

rb0="$(phy_counter "${RX_IFACE}" rx_bytes_phy)"
rp0="$(phy_counter "${RX_IFACE}" rx_packets_phy)"
rd0="$(phy_counter "${RX_IFACE}" rx_discards_phy)"
t0="$(date +%s.%N)"
RUN_ID="${RUN_ID}" RESULTS_DIR="${RESULTS_DIR}" \
  "${SCRIPT_DIR}/run_resnet_xhost.sh" "$@"
rc=$?
t1="$(date +%s.%N)"
rb1="$(phy_counter "${RX_IFACE}" rx_bytes_phy)"
rp1="$(phy_counter "${RX_IFACE}" rx_packets_phy)"
rd1="$(phy_counter "${RX_IFACE}" rx_discards_phy)"

awk -v rb0="$rb0" -v rb1="$rb1" -v rp0="$rp0" -v rp1="$rp1" \
    -v rd0="$rd0" -v rd1="$rd1" -v t0="$t0" -v t1="$t1" \
    -v iface="${RX_IFACE}" -v rc="$rc" '
BEGIN {
  db=rb1-rb0; dp=rp1-rp0; dd=rd1-rd0; dt=t1-t0
  printf "iface=%s\nexit_code=%d\nwall_seconds=%.3f\n", iface, rc, dt
  printf "rx_bytes_phy_delta=%d\nrx_packets_phy_delta=%d\nrx_discards_phy_delta=%d\n", db, dp, dd
  if (dt > 0) {
    printf "wire_gbps_over_wall=%.3f\nwire_pps_over_wall=%.0f\n", db*8/dt/1e9, dp/dt
  }
  if (dp > 0) printf "physical_bytes_per_packet=%.3f\n", db/dp
}' > "${PHY}"

echo "=== physical counters (${PHY}) ==="
cat "${PHY}"
exit "${rc}"
