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

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# sysfs attribute of an interface, wherever it currently lives. A port that has
# been moved into a network namespace is absent from this namespace's
# /sys/class/net, so fall back to reading it from inside that namespace.
iface_attr() {
  local iface="$1" attr="$2"
  local path="/sys/class/net/${iface}/${attr}" ns
  if [[ -r "$path" ]]; then
    tr -d '\n' < "$path"
    return 0
  fi
  ns="$(iface_ns "$iface")"
  [[ -n "$ns" ]] || return 1
  netns_run "$ns" cat "$path" 2>/dev/null | tr -d '\n'
}

# ethtool talks to the driver over netlink, which follows the namespace of the
# calling process, so it still answers for a moved port when the sysfs view
# does not (nsenter changes the network namespace but not the /sys mount).
read_mac() {
  local iface="$1" mac ns
  mac="$(iface_attr "$iface" address)"
  if [[ -z "$mac" ]]; then
    ns="$(iface_ns "$iface")"
    mac="$(netns_run "$ns" ethtool -P "$iface" 2>/dev/null | awk '{ print tolower($NF) }')"
    [[ "$mac" == "00:00:00:00:00:00" ]] && mac=""
  fi
  echo "$mac"
}

read_carrier() {
  local iface="$1" carrier ns
  carrier="$(iface_attr "$iface" carrier)"
  if [[ -z "$carrier" ]]; then
    ns="$(iface_ns "$iface")"
    case "$(netns_run "$ns" ethtool "$iface" 2>/dev/null | awk '/Link detected:/ { print $NF }')" in
      yes) carrier=1 ;;
      no)  carrier=0 ;;
    esac
  fi
  echo "$carrier"
}

read_speed() {
  iface_attr "$1" speed 2>/dev/null || echo 0
}

# MAC of whatever is on the other end of the cable, per LLDP. Empty if lldpd is
# not running or the peer does not speak LLDP.
lldp_peer_mac() {
  local iface="$1"
  command -v lldpctl >/dev/null 2>&1 || return 1
  lldpctl -f keyvalue "$iface" 2>/dev/null |
    awk -F= -v i="$iface" '$1 == "lldp." i ".port.mac" { print tolower($2); exit }'
}

bdf_for_iface() {
  local iface="$1" ns
  local path="/sys/class/net/${iface}/device"
  if [[ -L "$path" ]]; then
    basename "$(readlink -f "$path")"
    return 0
  fi
  ns="$(iface_ns "$iface")"
  [[ -n "$ns" ]] || return 1
  basename "$(netns_run "$ns" readlink -f "$path" 2>/dev/null)"
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

# --------------------------------------------------------------------------
# The socket/RoCE wire loopback moves each cabled port into its own network
# namespace (scripts/setup_spark_wire_loopback_netns.sh). Once moved, a port is
# gone from this namespace: sysfs does not list it, and lldpd cannot see it
# either -- so the LLDP scan below finds nothing and the carrier fallback picks
# whatever is left, which on this class of host is a pair that is not cabled
# together. The namespaces are themselves the strongest statement available that
# two ports are on one cable, so read the pair back out of them first.
# --------------------------------------------------------------------------
NETNS_CLIENT="${NETNS_CLIENT:-dq_wire_client}"
NETNS_SERVER="${NETNS_SERVER:-dq_wire_server}"

netns_run() {
  local ns="$1"; shift
  [[ -e "/var/run/netns/$ns" ]] || return 1
  if command -v ip >/dev/null 2>&1; then
    ip netns exec "$ns" "$@"
  elif command -v nsenter >/dev/null 2>&1; then
    nsenter --net="/var/run/netns/$ns" "$@"
  else
    return 1
  fi
}

# /proc/net/dev rather than /sys/class/net: sysfs is filtered by the namespace
# its mount was made in, and nsenter does not remount it.
netns_iface() {
  netns_run "$1" cat /proc/net/dev 2>/dev/null |
    awk -F: 'NR > 2 { gsub(/ /, "", $1); if ($1 != "lo") { print $1; exit } }'
}

netns_wire_pair() {
  local c s
  c="$(netns_iface "$NETNS_CLIENT")"
  s="$(netns_iface "$NETNS_SERVER")"
  [[ -n "$c" && -n "$s" ]] || return 1
  echo "$c $s"
}

# Namespace holding an interface; empty when it is in this one.
iface_ns() {
  local iface="$1" ns
  [[ -d "/sys/class/net/$iface" ]] && return 0
  for ns in "$NETNS_CLIENT" "$NETNS_SERVER"; do
    if [[ "$(netns_iface "$ns")" == "$iface" ]]; then
      echo "$ns"
      return 0
    fi
  done
  return 0
}

# Find two local ports that are cabled to each other.
#
# carrier=1 only says the port has a link partner, not which port that is. On a
# host with several cabled NICs -- some to a switch, some to each other -- taking
# the first two up interfaces picks a pair that cannot talk, and the run fails
# with RX=0 after the fact. LLDP answers the question directly: if port A reports
# port B's MAC as its neighbour *and* B reports A's, they are the two ends of one
# cable. Ports facing a switch report the switch's MAC and are skipped.
pick_default_pair() {
  local netns_pair
  if netns_pair="$(netns_wire_pair)"; then
    echo "$netns_pair"
    return 0
  fi

  mapfile -t ifaces < <(list_mlx_ifaces)
  [[ ${#ifaces[@]} -ge 2 ]] || return 1

  local iface mac
  declare -A mac_of=()
  declare -A iface_of_mac=()
  for iface in "${ifaces[@]}"; do
    mac="$(read_mac "$iface" 2>/dev/null || true)"
    [[ -n "$mac" ]] || continue
    mac="${mac,,}"
    mac_of["$iface"]="$mac"
    iface_of_mac["$mac"]="$iface"
  done

  local best_a="" best_b="" best_speed=-1
  local peer_mac peer speed
  for iface in "${ifaces[@]}"; do
    [[ "$(read_carrier "$iface" 2>/dev/null || echo 0)" == "1" ]] || continue
    peer_mac="$(lldp_peer_mac "$iface" || true)"
    [[ -n "$peer_mac" ]] || continue
    peer="${iface_of_mac[$peer_mac]:-}"
    # The neighbour must be a different local port, and the relationship must be
    # mutual -- a one-sided sighting can be a stale LLDP entry.
    [[ -n "$peer" && "$peer" != "$iface" ]] || continue
    [[ "$(lldp_peer_mac "$peer" || true)" == "${mac_of[$iface]}" ]] || continue
    speed="$(read_speed "$iface")"
    if [[ "${speed:-0}" -gt "$best_speed" ]]; then
      best_speed="${speed:-0}"
      best_a="$iface"
      best_b="$peer"
    fi
  done

  if [[ -n "$best_a" ]]; then
    echo "$best_a $best_b"
    return 0
  fi


  # No LLDP evidence. Fall back to the first two ports with a link, but say so:
  # this is the case that historically produced a passing TX and a silent RX=0.
  local up=()
  for iface in "${ifaces[@]}"; do
    [[ "$(read_carrier "$iface" 2>/dev/null || echo 0)" == "1" ]] && up+=("$iface")
  done
  if [[ ${#up[@]} -ge 2 ]]; then
    echo "WARNING: no LLDP loopback pair found (is lldpd running?). Guessing ${up[0]} -> ${up[1]}" >&2
    echo "         carrier=1 does not mean these two ports are cabled to each other." >&2
    echo "         Set RTX_TX_IFACE/RTX_RX_IFACE or RTX_TX_BDF/RTX_RX_BDF to be sure." >&2
    echo "${up[0]} ${up[1]}"
    return 0
  fi
  return 1
}

TX_BDF="${RTX_TX_BDF:-}"
RX_BDF="${RTX_RX_BDF:-}"
TX_IFACE="${RTX_TX_IFACE:-}"
RX_IFACE="${RTX_RX_IFACE:-}"

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

# Confirm the chosen pair really is one cable, including when the caller pinned
# the ports by hand. Cheap here, and it turns an RX=0 run into an up-front error.
TX_PEER="$(lldp_peer_mac "$TX_IFACE" 2>/dev/null || true)"
RX_PEER="$(lldp_peer_mac "$RX_IFACE" 2>/dev/null || true)"
RTX_TX_NS="$(iface_ns "$TX_IFACE")"
RTX_RX_NS="$(iface_ns "$RX_IFACE")"
export RTX_TX_NS RTX_RX_NS
if [[ -n "$RTX_TX_NS" && -n "$RTX_RX_NS" ]]; then
  # lldpd runs in the default namespace and cannot see a moved port, but the
  # namespaces only exist because these two ports are the ends of one cable.
  echo "  loopback:  wire-loopback namespaces (${TX_IFACE}@${RTX_TX_NS} <-> ${RX_IFACE}@${RTX_RX_NS})"
elif [[ -n "$TX_PEER" && -n "$RX_PEER" ]]; then
  if [[ "$TX_PEER" == "${RX_MAC,,}" && "$RX_PEER" == "${TX_MAC,,}" ]]; then
    echo "  loopback:  confirmed by LLDP (${TX_IFACE} <-> ${RX_IFACE})"
  else
    echo "WARNING: LLDP says ${TX_IFACE} faces ${TX_PEER} and ${RX_IFACE} faces ${RX_PEER}," >&2
    echo "         so these two ports are not cabled to each other. Wire RX will be zero." >&2
  fi
elif [[ -n "${TX_PEER:-}" && "$TX_PEER" == "${RX_MAC,,}" ]] \
  || [[ -n "${RX_PEER:-}" && "$RX_PEER" == "${TX_MAC,,}" ]]; then
  # lldpd only refreshes a port it is currently running on, so a port that has
  # been moved into a namespace and back can leave the pair confirmed from one
  # side only. One side agreeing is still the two ports naming each other.
  echo "  loopback:  confirmed by LLDP from one side (${TX_IFACE} <-> ${RX_IFACE})"
else
  echo "  loopback:  not confirmed here (no LLDP neighbour data). Every run"
  echo "             re-checks the pairing against the NIC's own frame counters"
  echo "             and fails if nothing arrives on ${RX_IFACE}."
fi

if [[ -z "${TX_BDF:-}" || -z "${RX_BDF:-}" ]]; then
  echo "WARNING: could not resolve PCIe BDFs; pass --tx-bdf/--rx-bdf to run_rtx_pro_bench.sh" >&2
fi

# GPU ordinals (PIX to each NIC) and poll-core layout for bench YAML generation.
# Requires CUDA (libcuda) and nvidia-smi; override with TX_GPU/RX_GPU/RTX_CPU_CORES.
export CUDA_DEVICE_ORDER="${CUDA_DEVICE_ORDER:-PCI_BUS_ID}"
DISCOVER_PY="$REPO_ROOT/scripts/rtx_pro_discover.py"
if [[ -f "$DISCOVER_PY" && -n "${TX_BDF:-}" && -n "${RX_BDF:-}" ]]; then
  # stderr is left attached on purpose: it carries the resolved CUDA bus ids and
  # the isolcpus/PIX warnings, which are the things worth seeing when a run's
  # affinity looks wrong.
  # shellcheck disable=SC1090
  eval "$(RTX_TX_BDF="$TX_BDF" RTX_RX_BDF="$RX_BDF" python3 "$DISCOVER_PY")" || true
fi

if [[ -n "${RTX_TX_GPU:-}" ]]; then
  echo "  TX GPU:    ordinal ${RTX_TX_GPU} (+ queue2 ${RTX_TX_GPU2:-$RTX_TX_GPU})"
  echo "  RX GPU:    ordinal ${RTX_RX_GPU} (+ queue2 ${RTX_RX_GPU2:-$RTX_RX_GPU})"
fi
if [[ -n "${RTX_CPU_CORES:-}" ]]; then
  echo "  poll cores: ${RTX_CPU_CORES}"
fi
