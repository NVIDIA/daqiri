#!/usr/bin/env bash
#
# Force true over-the-wire RoCE loopback on the DGX Spark via network namespaces.
#
# WHY THIS EXISTS
# ---------------
# The Spark has ONE ConnectX-7 whose ports share a single eswitch (switchid).
# When both the client and server IP live in the same (default) network
# namespace, the NIC's embedded switch + the kernel's local routing recognize
# the peer as locally-owned and short-cut the packets internally -- they never
# hit the cable. Putting each cabled port in its own netns removes that local
# knowledge, so the only path from client to server is OUT the wire and back.
#
# This is the RDMA-aware version of a colleague's netdev-only script: it also
# flips the RDMA subsystem into "exclusive" netns mode and moves each RDMA
# device into its namespace, which `ib_send_bw` requires to see a valid GID.
#
# CAVEAT ON PRIOR NUMBERS: the ~108 Gb/s ib_send_bw figures in
# rdma-bench-tuning-log.md were measured in the SHARED-namespace setup, i.e.
# they may have been short-cut internally rather than crossing the cable. The
# number you get here is the real over-the-wire figure and may differ.
#
# USAGE
#   sudo ./setup_spark_wire_loopback_netns.sh up       # create namespaces + move ports
#   sudo ./setup_spark_wire_loopback_netns.sh down     # tear everything back down
#   sudo ./setup_spark_wire_loopback_netns.sh verify   # sanity-check the result
#   sudo ./setup_spark_wire_loopback_netns.sh monitor  # live PHY-counter (on-the-wire) rates
#
# Run as root, inside the privileged run-container (or on the host as root).
# Do NOT run this at the same time as setup_spark_rdma_loopback.sh -- that
# script configures the same ports in the shared namespace and will conflict.
#
# =====================================================================
# >>> FILL THESE IN FOR YOUR MACHINE <<<
# =====================================================================
# Discover the right values first (read-only):
#
#   ip -br link show | grep -E 'enp|enP'      # interface names + MACs + state
#   cat /sys/class/net/<IF>/address           # MAC of one interface
#   rdma link show                            # rdma dev <-> netdev mapping
#   ibdev2netdev                              # (mlnx) same mapping, friendlier
#   ethtool <IF> | grep -i 'link detected'    # the two CABLED ports show "yes"
#
# CLIENT_IF / SERVER_IF must be the TWO PORTS YOUR QSFP CABLE PHYSICALLY
# CONNECTS. Your colleague's box cabled f0np0<->f1np1; the repo YAMLs assume the
# two f0np0 ports across PCI segments. Confirm with `ethtool ... link detected`
# -- exactly the two cabled ports report carrier up.
#
# CLIENT_RDMA / SERVER_RDMA are the RDMA devices bound to those netdevs
# (from `rdma link show` / `ibdev2netdev`).
#
# CLIENT_MAC / SERVER_MAC are the hardware MACs of those interfaces, used as
# permanent neighbors so nothing tries to ARP across the isolated namespaces.

CLIENT_NS=dq_wire_client
SERVER_NS=dq_wire_server

CLIENT_IF=enp1s0f0np0          # p0 via PCI segment 0000:01:00.0 (carrier up)
SERVER_IF=enP2p1s0f1np1        # p1 via PCI segment 0002:01:00.1 (carrier up) -- the
                               # cabled peer of CLIENT_IF (p0<->p1 QSFP loopback). NOT
                               # enP2p1s0f0np0, which is p0 again (same physical port).

CLIENT_RDMA=rocep1s0f0         # rdma dev for enp1s0f0np0
SERVER_RDMA=roceP2p1s0f1       # rdma dev for enP2p1s0f1np1

# Leave these blank to auto-detect from the interfaces (recommended). Only set
# them if you need to override the peer MAC for some reason.
CLIENT_MAC=""
SERVER_MAC=""

CLIENT_IP=10.250.0.1
SERVER_IP=10.250.0.2

MTU=9000                       # colleague used 9082; 9000 matches the data-plane ports
# =====================================================================

set -euo pipefail

require_root() {
  if [[ "$(id -u)" -ne 0 ]]; then
    echo "ERROR: run as root (sudo)." >&2
    exit 1
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
      # Move netdevs back to init_net.
      for IF in "$CLIENT_IF" "$SERVER_IF"; do
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
  # Auto-detects whether the ports live in the netns (this script's setup) or
  # the default namespace (the policy-routed setup_spark_rdma_loopback.sh setup).
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

Add -b for bidirectional. Compare against the shared-namespace numbers in
rdma-bench-tuning-log.md -- a real drop here means the old run was short-cut.
=============================================================================
EOF
}

require_root
case "${1:-up}" in
  up)      up ;;
  down)    down ;;
  verify)  verify ;;
  monitor) monitor ;;
  *) echo "usage: $0 {up|down|verify|monitor}" >&2; exit 1 ;;
esac
