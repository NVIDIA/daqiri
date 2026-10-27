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
if [[ -z "${RUN_ID:-}" ]]; then
  args=("$@")
  for ((i = 0; i < ${#args[@]}; ++i)); do
    if [[ "${args[i]}" == "--run-id" && $((i + 1)) -lt ${#args[@]} ]]; then
      RUN_ID="${args[i + 1]}"
      break
    fi
  done
fi
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
roob0="$(phy_counter "${RX_IFACE}" rx_out_of_buffer)"
rbuf0="$(phy_counter "${RX_IFACE}" rx_prio0_buf_discard)"
rsteer0="$(phy_counter "${RX_IFACE}" rx_steer_missed_packets)"
t0="$(date +%s.%N)"
RUN_ID="${RUN_ID}" RESULTS_DIR="${RESULTS_DIR}" \
  "${SCRIPT_DIR}/run_resnet_xhost.sh" "$@"
rc=$?
t1="$(date +%s.%N)"
tx_active="$(awk -F, '$1=="tx_active_seconds" {print $2; exit}' "${RESULTS_DIR}/${RUN_ID}.summary" 2>/dev/null || true)"
rb1="$(phy_counter "${RX_IFACE}" rx_bytes_phy)"
rp1="$(phy_counter "${RX_IFACE}" rx_packets_phy)"
rd1="$(phy_counter "${RX_IFACE}" rx_discards_phy)"
roob1="$(phy_counter "${RX_IFACE}" rx_out_of_buffer)"
rbuf1="$(phy_counter "${RX_IFACE}" rx_prio0_buf_discard)"
rsteer1="$(phy_counter "${RX_IFACE}" rx_steer_missed_packets)"

awk -v rb0="$rb0" -v rb1="$rb1" -v rp0="$rp0" -v rp1="$rp1" \
    -v rd0="$rd0" -v rd1="$rd1" -v t0="$t0" -v t1="$t1" \
    -v roob0="$roob0" -v roob1="$roob1" -v rbuf0="$rbuf0" -v rbuf1="$rbuf1" \
    -v rsteer0="$rsteer0" -v rsteer1="$rsteer1" \
    -v iface="${RX_IFACE}" -v rc="$rc" -v tx_active="$tx_active" '
BEGIN {
  db=rb1-rb0; dp=rp1-rp0; dd=rd1-rd0; dt=t1-t0
  printf "iface=%s\nexit_code=%d\nwall_seconds=%.3f\n", iface, rc, dt
  printf "rx_bytes_phy_delta=%d\nrx_packets_phy_delta=%d\nrx_discards_phy_delta=%d\n", db, dp, dd
  printf "rx_out_of_buffer_delta=%d\nrx_prio0_buf_discard_delta=%d\nrx_steer_missed_packets_delta=%d\n", \
         roob1-roob0, rbuf1-rbuf0, rsteer1-rsteer0
  if (dt > 0) {
    printf "wire_gbps_over_wall=%.3f\nwire_pps_over_wall=%.0f\n", db*8/dt/1e9, dp/dt
  }
  if (tx_active > 0) {
    printf "wire_gbps_over_tx_active=%.3f\nwire_pps_over_tx_active=%.0f\n", db*8/tx_active/1e9, dp/tx_active
  }
  if (dp > 0) printf "physical_bytes_per_packet=%.3f\n", db/dp
}' > "${PHY}"

echo "=== physical counters (${PHY}) ==="
cat "${PHY}"
exit "${rc}"
