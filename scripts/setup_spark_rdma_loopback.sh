#!/bin/bash
# Host network config for the DGX Spark RDMA loopback bench.
# Adapted from a colleague's single-adapter script for this host's
# inter-adapter loopback: Adapter1:port0 (1.1.1.1) <-> Adapter2:port0 (2.2.2.2).
#
# Matches examples/daqiri_bench_rdma_tx_rx_spark.yaml (1.1.1.1 / 2.2.2.2).
# Re-running is safe: replaces addresses, flushes per-port tables, and
# deletes any matching rules before re-adding.

set -euo pipefail

p0="enp1s0f0np0"     # Adapter1 port 0, PCI 0000:01:00.0, MAC 4c:bb:47:7c:f2:d8
p1="enP2p1s0f0np0"   # Adapter2 port 0, PCI 0002:01:00.0, MAC 4c:bb:47:7c:f2:dc

p0_mac="4c:bb:47:7c:f2:d8"
p1_mac="4c:bb:47:7c:f2:dc"

p0_ip="1.1.1.1"
p1_ip="2.2.2.2"

# Numeric table IDs avoid having to edit /etc/iproute2/rt_tables.
table_p0=100
table_p1=101

ip link set "${p0}" up
ip link set "${p1}" up

ip addr replace "${p0_ip}/24" dev "${p0}"
ip addr replace "${p1_ip}/24" dev "${p1}"

ip route flush table "${table_p0}" 2>/dev/null || true
ip route flush table "${table_p1}" 2>/dev/null || true
ip route add table "${table_p0}" default dev "${p0}"
ip route add table "${table_p1}" default dev "${p1}"

ip rule del from "${p0_ip}/32" table "${table_p0}" 2>/dev/null || true
ip rule del to   "${p0_ip}/32" table "${table_p0}" 2>/dev/null || true
ip rule del from "${p1_ip}/32" table "${table_p1}" 2>/dev/null || true
ip rule del to   "${p1_ip}/32" table "${table_p1}" 2>/dev/null || true

ip rule add from "${p0_ip}/32" table "${table_p0}"
ip rule add to   "${p0_ip}/32" table "${table_p0}"
ip rule add from "${p1_ip}/32" table "${table_p1}"
ip rule add to   "${p1_ip}/32" table "${table_p1}"

arp -i "${p0}" -s "${p0_ip}" "${p0_mac}"
arp -i "${p0}" -s "${p1_ip}" "${p1_mac}"
arp -i "${p1}" -s "${p0_ip}" "${p0_mac}"
arp -i "${p1}" -s "${p1_ip}" "${p1_mac}"

echo "RDMA loopback config applied."
echo "  ${p0} (${p0_mac}) -> ${p0_ip}/24, table ${table_p0}"
echo "  ${p1} (${p1_mac}) -> ${p1_ip}/24, table ${table_p1}"
