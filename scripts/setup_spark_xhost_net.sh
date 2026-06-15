#!/bin/bash
# Host network config for two-DGX-Spark cross-cable benches (p0 <-> p0).
# Adds the host route and static neighbor the kernel needs to reach the peer
# when daqiri-tx / daqiri-rx nmcli profiles are split across two boxes.
#
# Matches examples/*_spark_xhost.yaml (1.1.1.1 on TX, 2.2.2.2 on RX).
# Re-running is safe: replaces route and neighbor entries.
#
# Usage:
#   sudo scripts/setup_spark_xhost_net.sh --role tx --peer-mac <RX_P0_MAC>
#   sudo scripts/setup_spark_xhost_net.sh --role rx --peer-mac <TX_P0_MAC>
#
# Env overrides: SPARK_XHOST_IFACE, SPARK_TX_IP, SPARK_RX_IP

set -euo pipefail

iface="${SPARK_XHOST_IFACE:-enp1s0f0np0}"
tx_ip="${SPARK_TX_IP:-1.1.1.1}"
rx_ip="${SPARK_RX_IP:-2.2.2.2}"
role=""
peer_ip=""
peer_mac=""

usage() {
  cat <<EOF
Usage: $0 --role tx|rx --peer-mac <MAC> [--iface IFACE] [--peer-ip IP]

  tx  local ${tx_ip} on IFACE; peer defaults to ${rx_ip}
  rx  local ${rx_ip} on IFACE; peer defaults to ${tx_ip}

Read the peer MAC on the other host:
  cat /sys/class/net/${iface}/address

Verify after running:
  ping -c1 <peer-ip>
  ip route get <peer-ip>
  ip neigh show dev ${iface}
EOF
}

read_mac() {
  local dev="$1"
  local path="/sys/class/net/${dev}/address"
  if [[ ! -r "$path" ]]; then
    echo "cannot read $path" >&2
    exit 1
  fi
  cat "$path"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --role)
      role="$2"
      shift 2
      ;;
    --iface)
      iface="$2"
      shift 2
      ;;
    --peer-ip)
      peer_ip="$2"
      shift 2
      ;;
    --peer-mac)
      peer_mac="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ -z "$role" || -z "$peer_mac" ]]; then
  echo "error: --role and --peer-mac are required" >&2
  usage
  exit 1
fi

case "$role" in
  tx)
    local_ip="$tx_ip"
    peer_ip="${peer_ip:-$rx_ip}"
    ;;
  rx)
    local_ip="$rx_ip"
    peer_ip="${peer_ip:-$tx_ip}"
    ;;
  *)
    echo "error: --role must be tx or rx" >&2
    exit 1
    ;;
esac

local_mac="$(read_mac "$iface")"

ip link set "${iface}" up

ip route replace "${peer_ip}/32" dev "${iface}"
ip neigh replace "${peer_ip}" lladdr "${peer_mac}" dev "${iface}" nud permanent

echo "Cross-host network config applied (${role} role)."
echo "  ${iface} (${local_mac}) local ${local_ip}/24"
echo "  peer ${peer_ip} -> ${peer_mac} via ${iface}"
echo
echo "Verify:"
echo "  ping -c1 ${peer_ip}"
echo "  ip route get ${peer_ip}"
echo "  ip neigh show dev ${iface}"
