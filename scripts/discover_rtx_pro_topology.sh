#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Emit RTX PRO 6000 topology for benchmark YAML generation.
# Run on the host while NICs are kernel-bound (before DPDK devbind).
#
# Usage:
#   ./scripts/discover_rtx_pro_topology.sh          # print summary
#   source ./scripts/discover_rtx_pro_topology.sh # export ETH_DST_ADDR, etc.
#
# Override when auto-detection is wrong:
#   RTX_TX_BDF RTX_RX_BDF RTX_TX_IFACE RTX_RX_IFACE ETH_DST_ADDR

set -u

read_mac() {
  local iface="$1"
  local path="/sys/class/net/${iface}/address"
  [[ -r "$path" ]] || return 1
  cat "$path"
}

read_carrier() {
  local iface="$1"
  local path="/sys/class/net/${iface}/carrier"
  [[ -r "$path" ]] || return 1
  tr -d '\n' < "$path"
}

bdf_for_iface() {
  local iface="$1"
  local path="/sys/class/net/${iface}/device"
  [[ -L "$path" ]] || return 1
  basename "$(readlink -f "$path")"
}

# Collect kernel mlx5 netdevs with link up, sorted for stable defaults.
list_mlx_ifaces() {
  if command -v ibdev2netdev >/dev/null 2>&1; then
    ibdev2netdev 2>/dev/null | awk '/\(Up\)/ {
      for (i=1;i<=NF;i++) if ($i ~ /^ens/) { print $i; break }
    }'
    return 0
  fi
  ls /sys/class/net 2>/dev/null | awk '/^ens/ { print }'
}

resolve_iface_for_bdf() {
  local bdf="$1"
  if command -v ibdev2netdev >/dev/null 2>&1; then
    ibdev2netdev 2>/dev/null | awk -v b="${bdf#0000:}" '
      $0 ~ ("0000:" b) { for (i=1;i<=NF;i++) if ($i ~ /^ens/) { print $i; exit } }
    ' | head -n1
  fi
}

pick_default_pair() {
  mapfile -t ifaces < <(list_mlx_ifaces)
  local up=()
  local iface carrier
  for iface in "${ifaces[@]}"; do
    carrier="$(read_carrier "$iface" 2>/dev/null || echo 0)"
    [[ "$carrier" == "1" ]] && up+=("$iface")
  done
  if [[ ${#up[@]} -ge 2 ]]; then
    echo "${up[0]} ${up[1]}"
    return 0
  fi
  if [[ ${#ifaces[@]} -ge 2 ]]; then
    echo "${ifaces[0]} ${ifaces[1]}"
    return 0
  fi
  return 1
}

TX_BDF="${RTX_TX_BDF:-}"
RX_BDF="${RTX_RX_BDF:-}"
TX_IFACE="${RTX_TX_IFACE:-}"
RX_IFACE="${RTX_RX_IFACE:-}"

# Legacy reference-box defaults when those devices exist.
if [[ -z "$TX_BDF" && -d /sys/bus/pci/devices/0000:61:00.0 ]]; then
  TX_BDF="0000:61:00.0"
  RX_BDF="${RX_BDF:-0000:61:00.1}"
  TX_IFACE="${TX_IFACE:-ens1f0np0}"
  RX_IFACE="${RX_IFACE:-ens1f1np1}"
fi

if [[ -z "$TX_IFACE" || ! -d "/sys/class/net/${TX_IFACE}" ]]; then
  if [[ -n "$TX_BDF" ]]; then
    resolved="$(resolve_iface_for_bdf "$TX_BDF" || true)"
    [[ -n "$resolved" ]] && TX_IFACE="$resolved"
  fi
fi
if [[ -z "$RX_IFACE" || ! -d "/sys/class/net/${RX_IFACE}" ]]; then
  if [[ -n "$RX_BDF" ]]; then
    resolved="$(resolve_iface_for_bdf "$RX_BDF" || true)"
    [[ -n "$resolved" ]] && RX_IFACE="$resolved"
  fi
fi

if [[ -z "$TX_IFACE" || -z "$RX_IFACE" || "$TX_IFACE" == "$RX_IFACE" ]]; then
  if pair="$(pick_default_pair)"; then
    read -r _tx _rx <<< "$pair"
    TX_IFACE="${TX_IFACE:-$_tx}"
    RX_IFACE="${RX_IFACE:-$_rx}"
  fi
fi

if [[ -z "$TX_BDF" && -n "$TX_IFACE" ]]; then
  TX_BDF="$(bdf_for_iface "$TX_IFACE" || true)"
fi
if [[ -z "$RX_BDF" && -n "$RX_IFACE" ]]; then
  RX_BDF="$(bdf_for_iface "$RX_IFACE" || true)"
fi

RX_MAC="$(read_mac "$RX_IFACE" 2>/dev/null || true)"
TX_MAC="$(read_mac "$TX_IFACE" 2>/dev/null || true)"
P0_CARRIER="$(read_carrier "$TX_IFACE" 2>/dev/null || echo "?")"
P1_CARRIER="$(read_carrier "$RX_IFACE" 2>/dev/null || echo "?")"

export RTX_TX_BDF="${TX_BDF:-}"
export RTX_RX_BDF="${RX_BDF:-}"
export RTX_TX_IFACE="${TX_IFACE:-}"
export RTX_RX_IFACE="${RX_IFACE:-}"
export RTX_TX_MAC="${TX_MAC:-}"
export RTX_RX_MAC="${RX_MAC:-}"
export ETH_DST_ADDR="${ETH_DST_ADDR:-$RX_MAC}"
export RX_IFACE="$RX_IFACE"

echo "RTX PRO 6000 topology"
echo "  TX BDF:    ${TX_BDF:-unknown}  iface=${TX_IFACE:-unknown}  mac=${TX_MAC:-unknown}  carrier=$P0_CARRIER"
echo "  RX BDF:    ${RX_BDF:-unknown}  iface=${RX_IFACE:-unknown}  mac=${RX_MAC:-unknown}  carrier=$P1_CARRIER"
echo "  ETH_DST_ADDR=${ETH_DST_ADDR:-}"

if [[ -z "${RX_MAC:-}" ]]; then
  echo "WARNING: could not read RX MAC; set ETH_DST_ADDR manually" >&2
  return 0 2>/dev/null || exit 0
fi

if [[ "$P0_CARRIER" != "1" || "$P1_CARRIER" != "1" ]]; then
  echo "WARNING: one or both ports not carrier=1; wire tests may fail" >&2
fi

if [[ -z "${TX_BDF:-}" || -z "${RX_BDF:-}" ]]; then
  echo "WARNING: could not resolve PCIe BDFs; pass --tx-bdf/--rx-bdf to run_rtx_pro_bench.sh" >&2
fi
