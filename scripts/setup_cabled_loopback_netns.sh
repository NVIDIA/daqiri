#!/usr/bin/env bash
#
# Force true over-the-wire socket/RoCE loopback via network namespaces.
#
# WHY THIS EXISTS
# ---------------
# Same-host ports can share an embedded switch and the kernel's local routes.
# When both the client and server IP live in the same (default) network
# namespace, the NIC's embedded switch + the kernel's local routing recognize
# the peer as locally-owned and short-cut the packets internally -- they never
# hit the cable. Putting each cabled port in its own netns removes that local
# knowledge, so the only path from client to server is OUT the wire and back.
#
# RDMA is handled as well as the netdevs: the script tries exclusive RDMA netns
# mode and falls back to shared mode when other namespaces make that impossible.
#
# USAGE
#   sudo ./setup_cabled_loopback_netns.sh --platform dgx-spark|igx-thor up
#   sudo ./setup_cabled_loopback_netns.sh --platform dgx-spark|igx-thor down
#   sudo ./setup_cabled_loopback_netns.sh --platform dgx-spark|igx-thor verify
#   sudo ./setup_cabled_loopback_netns.sh --platform dgx-spark|igx-thor monitor
#
# Run as root, inside the privileged run-container (or on the host as root).
# Do not run another network setup against the same ports at the same time.
#
# CLIENT_IF / SERVER_IF must be on the TWO DIFFERENT PHYSICAL PORTS (p0, p1) of
# the NIC. Hardware defaults and live discovery choose them unless overridden.
#
# CLIENT_RDMA / SERVER_RDMA are the RDMA devices bound to those netdevs
# (from `rdma link show` / `ibdev2netdev`).
#
# CLIENT_MAC / SERVER_MAC are the hardware MACs of those interfaces, used as
# permanent neighbors so nothing tries to ARP across the isolated namespaces.

CLIENT_NS=dq_wire_client
SERVER_NS=dq_wire_server
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HARDWARE_RESOLVER="$SCRIPT_DIR/resolve_benchmark_hardware.py"
HARDWARE_DEFAULTS="$REPO_ROOT/examples/cabled_loopback_hardware.yaml"
PLATFORM=""

# Leave CLIENT_IF / SERVER_IF blank to AUTO-DETECT the cabled data-plane ports
# (recommended): autodetect_ports() collects the carrier-up RoCE netdevs, groups
# them by phys_port_name, and keeps one per physical port -- so the pair always
# straddles both ports (over-the-wire). PCIe enumeration / interface names drift
# across reboots and kernel upgrades on this box, so hardcoding them is fragile
# (a stale name aborts setup, the netns never gets created, and the netns-based
# benches silently have nowhere to run); phys_port_name does not drift. Override
# from the environment only if auto-detect picks the wrong pair, e.g.:
#   CLIENT_IF=enp1s0f1np1 SERVER_IF=enp1s0f0np0 sudo -E ./setup_cabled_loopback_netns.sh --platform igx-thor up
CLIENT_IF="${CLIENT_IF:-}"
SERVER_IF="${SERVER_IF:-}"

# RDMA devices backing the netdevs above. Blank = derive from the netdev via
# sysfs (/sys/class/net/<if>/device/infiniband). Only needed for RDMA exclusive
# netns mode; the socket/UDP/TCP benches do not use these.
CLIENT_RDMA="${CLIENT_RDMA:-}"
SERVER_RDMA="${SERVER_RDMA:-}"

# Leave these blank to auto-detect from the interfaces (recommended). Only set
# them if you need to override the peer MAC for some reason.
CLIENT_MAC=""
SERVER_MAC=""

CLIENT_IP=10.250.0.1
SERVER_IP=10.250.0.2

MTU=9000

set -euo pipefail

require_root() {
  if [[ "$(id -u)" -ne 0 ]]; then
    echo "ERROR: run as root (sudo)." >&2
    exit 1
  fi
}

rdma_for_netdev() {  # netdev -> backing RDMA device name (sysfs); empty if none
  local ifc="$1" d
  for d in /sys/class/net/"$ifc"/device/infiniband/*; do
    [[ -e "$d" ]] || continue
    basename "$d"
    return 0
  done
  return 1
}

autodetect_ports() {
  # Ports must be in init_net here. up() calls down() first so the shared
  # resolver sees the same live inventory used by the benchmark harness.
  if [[ -z "$CLIENT_IF" || -z "$SERVER_IF" ]]; then
    local resolved
    resolved="$(python3 "$HARDWARE_RESOLVER" --defaults "$HARDWARE_DEFAULTS" \
      --platform "$PLATFORM" --format shell)" || exit 1
    eval "$resolved"
    [[ -z "$CLIENT_IF" ]] && CLIENT_IF="$DEFAULT_DPDK_TX_NETDEV"
    [[ -z "$SERVER_IF" ]] && SERVER_IF="$DEFAULT_DPDK_RX_NETDEV"
  fi
  [[ -z "$CLIENT_RDMA" ]] && CLIENT_RDMA="$(rdma_for_netdev "$CLIENT_IF" || true)"
  [[ -z "$SERVER_RDMA" ]] && SERVER_RDMA="$(rdma_for_netdev "$SERVER_IF" || true)"
  echo "Using ports: CLIENT_IF=$CLIENT_IF (rdma=${CLIENT_RDMA:-none})  SERVER_IF=$SERVER_IF (rdma=${SERVER_RDMA:-none})"
}

resolve_ports() {
  # For verify()/monitor(), which run AFTER up() with the ports already moved
  # into the namespaces: read each ns's non-lo netdev name. If the namespaces
  # are absent, fall back to init_net auto-detect. Honours explicit overrides.
  if ip netns list 2>/dev/null | grep -qw "$CLIENT_NS"; then
    [[ -z "$CLIENT_IF" ]] && CLIENT_IF="$(ip netns exec "$CLIENT_NS" ls /sys/class/net 2>/dev/null | grep -vx lo | head -1)"
    [[ -z "$SERVER_IF" ]] && SERVER_IF="$(ip netns exec "$SERVER_NS" ls /sys/class/net 2>/dev/null | grep -vx lo | head -1)"
  else
    autodetect_ports
  fi
}

detect_macs() {
  # Interfaces must be in the current (init) namespace at this point -- up()
  # calls down() first, which returns any moved netdevs to init_net.
  for IF in "$CLIENT_IF" "$SERVER_IF"; do
    if [[ ! -e "/sys/class/net/$IF/address" ]]; then
      echo "ERROR: interface '$IF' not found in this namespace." >&2
      echo "       List interfaces with: ip -br link show" >&2
      echo "       Then fix CLIENT_IF / SERVER_IF at the top of this script." >&2
      exit 1
    fi
  done
  [[ -z "$CLIENT_MAC" ]] && CLIENT_MAC="$(cat "/sys/class/net/$CLIENT_IF/address")"
  [[ -z "$SERVER_MAC" ]] && SERVER_MAC="$(cat "/sys/class/net/$SERVER_IF/address")"
  echo "Using MACs: $CLIENT_IF=$CLIENT_MAC  $SERVER_IF=$SERVER_MAC"
}

down() {
  # Best-effort teardown: move devices back to init_net, drop namespaces,
  # return RDMA to shared mode. Safe to run repeatedly.
  for NS in "$CLIENT_NS" "$SERVER_NS"; do
    if ip netns list | grep -qw "$NS"; then
      # Move any rdma devices in this ns back to init_net (pid 1's ns).
      while read -r dev _; do
        [[ -n "$dev" ]] && ip netns exec "$NS" rdma dev set "$dev" netns 1 2>/dev/null || true
      done < <(ip netns exec "$NS" rdma dev show 2>/dev/null | awk -F': ' '/link/ {print $2}' | awk '{print $1}')
      # Move every (non-lo) netdev back to init_net. Name-independent so
      # teardown can't break on a stale/auto-detected interface name.
      for IF in $(ip netns exec "$NS" ls /sys/class/net 2>/dev/null); do
        [[ "$IF" == lo ]] && continue
        ip netns exec "$NS" ip link set "$IF" netns 1 2>/dev/null || true
      done
      ip netns delete "$NS" 2>/dev/null || true
    fi
  done
  rdma system set netns shared 2>/dev/null || true
  echo "Torn down. RDMA subsystem returned to shared netns mode."
}

up() {
  echo "== Tearing down any previous state =="
  down

  autodetect_ports
  detect_macs

  echo "== Clearing shared-namespace IPs on the cabled ports =="
  ip addr flush dev "$CLIENT_IF" 2>/dev/null || true
  ip addr flush dev "$SERVER_IF" 2>/dev/null || true

  echo "== Trying RDMA exclusive netns mode =="
  # Exclusive mode makes each RDMA device visible only in its assigned
  # namespace. It can ONLY be enabled when init_net is the sole net namespace
  # on the host -- if Docker/containerd (or any other netns) is running, the
  # kernel returns EBUSY. That is fine: in the default SHARED mode the RDMA
  # devices stay global, but RoCE GIDs are still scoped to the netdev's
  # namespace, so ib_send_bw under `ip netns exec` resolves the correct GID
  # for each side regardless. We only need exclusive mode for hard isolation.
  RDMA_EXCLUSIVE=0
  if rdma system set netns exclusive 2>/dev/null; then
    RDMA_EXCLUSIVE=1
    echo "   exclusive mode enabled."
  else
    echo "   EBUSY -- other net namespaces exist (likely Docker/containerd)."
    echo "   Falling back to SHARED mode; ib_send_bw via 'ip netns exec' still works."
  fi

  echo "== Creating namespaces =="
  ip netns add "$CLIENT_NS"
  ip netns add "$SERVER_NS"

  if [[ "$RDMA_EXCLUSIVE" == "1" ]]; then
    echo "== Moving RDMA devices into their namespaces =="
    rdma dev set "$CLIENT_RDMA" netns "$CLIENT_NS"
    rdma dev set "$SERVER_RDMA" netns "$SERVER_NS"
  fi

  echo "== Moving netdevs into the same namespaces =="
  ip link set "$CLIENT_IF" netns "$CLIENT_NS"
  ip link set "$SERVER_IF" netns "$SERVER_NS"

  echo "== Addressing, MTU, link up =="
  ip -n "$CLIENT_NS" addr add "$CLIENT_IP/24" dev "$CLIENT_IF"
  ip -n "$SERVER_NS" addr add "$SERVER_IP/24" dev "$SERVER_IF"

  ip -n "$CLIENT_NS" link set lo up
  ip -n "$SERVER_NS" link set lo up
  ip -n "$CLIENT_NS" link set "$CLIENT_IF" mtu "$MTU" up
  ip -n "$SERVER_NS" link set "$SERVER_IF" mtu "$MTU" up

  echo "== Static routes (host route to the peer out the cabled port) =="
  ip -n "$CLIENT_NS" route add "$SERVER_IP/32" dev "$CLIENT_IF"
  ip -n "$SERVER_NS" route add "$CLIENT_IP/32" dev "$SERVER_IF"

  echo "== Permanent neighbors (no ARP across the isolated namespaces) =="
  ip -n "$CLIENT_NS" neigh replace "$SERVER_IP" lladdr "$SERVER_MAC" dev "$CLIENT_IF" nud permanent
  ip -n "$SERVER_NS" neigh replace "$CLIENT_IP" lladdr "$CLIENT_MAC" dev "$SERVER_IF" nud permanent

  echo
  echo "Done. Verify with: $0 verify"
  echo
  print_perftest_hint
}

verify() {
  resolve_ports
  echo "== RDMA device per namespace (each should show exactly its own dev) =="
  echo "-- $CLIENT_NS --"; ip netns exec "$CLIENT_NS" rdma dev show || true
  echo "-- $SERVER_NS --"; ip netns exec "$SERVER_NS" rdma dev show || true

  echo "== GID table per namespace (the RoCEv2 IPv4 GID index is what -x wants) =="
  echo "-- $CLIENT_NS --"; ip netns exec "$CLIENT_NS" show_gids 2>/dev/null || \
    ip netns exec "$CLIENT_NS" ibv_devinfo -v 2>/dev/null | grep -iE 'GID|state|link_layer' || true
  echo "-- $SERVER_NS --"; ip netns exec "$SERVER_NS" show_gids 2>/dev/null || \
    ip netns exec "$SERVER_NS" ibv_devinfo -v 2>/dev/null | grep -iE 'GID|state|link_layer' || true

  echo "== L3 reachability over the wire (should reply via the cable) =="
  ip netns exec "$CLIENT_NS" ping -c2 -W2 "$SERVER_IP" || true

  echo "== Carrier: both cabled ports should report 'Link detected: yes' =="
  ip netns exec "$CLIENT_NS" ethtool "$CLIENT_IF" 2>/dev/null | grep -i 'link detected' || true
  ip netns exec "$SERVER_NS" ethtool "$SERVER_IF" 2>/dev/null | grep -i 'link detected' || true
}

ethtool_stats() {  # ns(- for default) netdev  ->  "name value" lines (numeric only)
  local ns="$1" ifc="$2"
  { if [[ "$ns" == "-" ]]; then ethtool -S "$ifc" 2>/dev/null
    else ip netns exec "$ns" ethtool -S "$ifc" 2>/dev/null; fi; } \
    | awk -F'[: ]+' 'NF>=3 && $3 ~ /^[0-9]+$/ {print $2, $3}' || true
}

report_movers() {  # label before-file after-file
  echo "== $1 =="
  join -j1 <(sort "$2") <(sort "$3") 2>/dev/null \
    | awk '{ d=$3-$2; if (d>0) { gb=($1 ~ /bytes/)?d*8/1e9:0;
             printf "  %-38s %16d  %8.2f Gb/s\n", $1, d, gb } }' \
    | sort -k2 -nr | head -8
  echo
}

monitor() {
  # Name-agnostic wire monitor. Samples ALL `ethtool -S` counters on both
  # netdevs once a second and prints the TOP MOVERS -- so you don't have to
  # know the right counter name (the *_phy names don't move on every CX-7/fw).
  # *bytes* counters are also shown as Gb/s. IMPORTANT: distinguish the two
  # families -- *_vport_* counters sit at the host<->eswitch boundary and count
  # your traffic whether it goes out the wire OR loops internally; only *_phy
  # counters (SerDes) prove it crossed the cable. vport moving while phy stays
  # flat == on-chip short-cut. NOTE: ethtool wants the NETDEV (enp1s0f0np0 /
  # enP2p1s0f0np0), not the rdma device (rocep1s0f0).
  # Auto-detects whether the ports live in this script's namespaces or the
  # default namespace.
  resolve_ports
  local cns sns
  if ip netns list 2>/dev/null | grep -qw "$CLIENT_NS"; then
    cns="$CLIENT_NS"; sns="$SERVER_NS"
    echo "Reading netdevs inside namespaces: $CLIENT_NS / $SERVER_NS"
  else
    cns="-"; sns="-"
    echo "Namespaces absent -- reading $CLIENT_IF / $SERVER_IF in the current namespace."
  fi
  if [[ -z "$(ethtool_stats "$cns" "$CLIENT_IF")" ]]; then
    echo "ERROR: no counters from netdev '$CLIENT_IF' (wrong name, or not root?)." >&2
    exit 1
  fi
  local tmp; tmp="$(mktemp -d "${TMPDIR:-/tmp}/phymon.XXXXXX")"
  trap 'rm -rf "$tmp"' EXIT
  echo "Top moving counters per 1s (Ctrl-C to stop). Client=TX side, Server=RX side."
  echo
  while :; do
    ethtool_stats "$cns" "$CLIENT_IF" > "$tmp/c0"
    ethtool_stats "$sns" "$SERVER_IF" > "$tmp/s0"
    sleep 1
    ethtool_stats "$cns" "$CLIENT_IF" > "$tmp/c1"
    ethtool_stats "$sns" "$SERVER_IF" > "$tmp/s1"
    report_movers "CLIENT $CLIENT_IF (TX side)" "$tmp/c0" "$tmp/c1"
    report_movers "SERVER $SERVER_IF (RX side)" "$tmp/s0" "$tmp/s1"
    echo "------------------------------------------------------------"
  done
}

print_perftest_hint() {
  cat <<EOF
========================  ib_send_bw across the wire  ========================
Open two shells (server first). Easiest path uses RDMA-CM (-R), which resolves
over the netdev IP and avoids hunting for the right GID index:

  # --- server (in $SERVER_NS) ---
  sudo ip netns exec $SERVER_NS \\
    ib_send_bw -d $SERVER_RDMA -R --report_gbits -s 8388608 -D 30

  # --- client (in $CLIENT_NS) ---
  sudo ip netns exec $CLIENT_NS \\
    ib_send_bw -d $CLIENT_RDMA -R --report_gbits -s 8388608 -D 30 $SERVER_IP

Non-CM path instead (socket QP exchange on tcp/18515, which also crosses the
wire). You must pass the RoCEv2 GID index -- find it per-namespace with
\`ip netns exec <ns> show_gids\` or \`ibv_devinfo -v | grep -i gid\`:

  sudo ip netns exec $SERVER_NS ib_send_bw -d $SERVER_RDMA -i 1 -x <GID> -F --report_gbits -s 8388608 -D 30
  sudo ip netns exec $CLIENT_NS ib_send_bw -d $CLIENT_RDMA -i 1 -x <GID> -F --report_gbits -s 8388608 -D 30 $SERVER_IP

Add -b for bidirectional. Confirm the directional *_phy counters move before
treating the result as cabled-loopback throughput.
=============================================================================
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --platform)
      [[ $# -ge 2 ]] || { echo "--platform requires a value" >&2; exit 1; }
      PLATFORM="$2"
      shift 2
      ;;
    *) break ;;
  esac
done

if [[ -z "$PLATFORM" ]]; then
  echo "--platform is required (dgx-spark or igx-thor)" >&2
  exit 1
fi
require_root
case "${1:-up}" in
  up)      up ;;
  down)    down ;;
  verify)  verify ;;
  monitor) monitor ;;
  *) echo "usage: $0 --platform dgx-spark|igx-thor {up|down|verify|monitor}" >&2; exit 1 ;;
esac
