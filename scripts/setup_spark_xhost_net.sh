#!/bin/bash
# Host network config for two-DGX-Spark cross-cable benches (p0 <-> p0).
# Adds the host route and static neighbor the kernel needs to reach the peer
# when daqiri-tx / daqiri-rx nmcli profiles are split across two boxes.
#
# Matches examples/*_spark_xhost.yaml (1.1.1.1 on TX, 2.2.2.2 on RX).
# Re-running is safe: replaces route and neighbor entries.
#
# Conflicts with scripts/setup_spark_rdma_loopback.sh on the same host: that
# script reassigns 1.1.1.1 / 2.2.2.2 across two local ports for inter-port
# loopback. To switch back to xhost, flush the iface (ip addr flush dev IFACE),
# re-up the right nmcli profile (daqiri-tx or daqiri-rx), then re-run this.
#
# Usage:
#   sudo scripts/setup_spark_xhost_net.sh --role tx --peer-mac <RX_P0_MAC>
#   sudo scripts/setup_spark_xhost_net.sh --role rx --peer-mac <TX_P0_MAC>
#
# Env overrides: SPARK_XHOST_IFACE, SPARK_TX_IP, SPARK_RX_IP,
# SPARK_TX_INT_IP, SPARK_RX_INT_IP (169.254 cable-side addresses for SSH/mgmt)

set -euo pipefail

iface="${SPARK_XHOST_IFACE:-enp1s0f0np0}"
tx_ip="${SPARK_TX_IP:-1.1.1.1}"
rx_ip="${SPARK_RX_IP:-2.2.2.2}"
tx_int_ip="${SPARK_TX_INT_IP:-169.254.95.47}"
rx_int_ip="${SPARK_RX_INT_IP:-169.254.100.253}"
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

# Drop stale on-link routes left from prior runs (wrong subnet on this host).
if [[ "$role" == "tx" ]]; then
  ip route del "${rx_ip}/24" dev "${iface}" 2>/dev/null || true
else
  ip route del "${tx_ip}/24" dev "${iface}" 2>/dev/null || true
fi

# Bench peer: pin egress source to the local bench address, not 169.254/16.
ip route replace "${peer_ip}/32" dev "${iface}" src "${local_ip}"
ip neigh replace "${peer_ip}" lladdr "${peer_mac}" dev "${iface}" nud permanent

# Cable internal address (169.254.x on p0): route + neighbor for SSH between hosts.
# Only applied when the configured local_int_ip is actually assigned to iface;
# some hosts put 169.254/16 on the *other* cable instead, in which case skip.
if [[ "$role" == "tx" ]]; then
  peer_int_ip="${peer_int_ip:-$rx_int_ip}"
  local_int_ip="$tx_int_ip"
else
  peer_int_ip="${peer_int_ip:-$tx_int_ip}"
  local_int_ip="$rx_int_ip"
fi
if [[ -n "$peer_int_ip" && "$peer_int_ip" != "$peer_ip" ]] && \
   ip -4 -o addr show dev "${iface}" | grep -qw "${local_int_ip}"; then
  ip route replace "${peer_int_ip}/32" dev "${iface}" src "${local_int_ip}"
  ip neigh replace "${peer_int_ip}" lladdr "${peer_mac}" dev "${iface}" nud permanent
  int_applied=1
else
  int_applied=0
fi

echo "Cross-host network config applied (${role} role)."
echo "  ${iface} (${local_mac}) bench ${local_ip}/24"
echo "  peer bench ${peer_ip} -> ${peer_mac}"
if [[ "${int_applied}" == "1" ]]; then
  echo "  peer internal ${peer_int_ip} -> ${peer_mac} (via ${iface}, src ${local_int_ip})"
else
  echo "  peer internal route skipped (${local_int_ip} not on ${iface})"
fi
echo
echo "Verify:"
echo "  ping -c1 ${peer_ip}"
echo "  ip route get ${peer_ip}"
echo "  ip neigh show dev ${iface}"
