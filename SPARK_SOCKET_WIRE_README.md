# Spark Socket Wire Benchmark Notes

This note documents the setup needed to make DAQIRI socket benchmark traffic
leave the NIC and return through a physical receive path on a NVIDIA DGX Spark
style machine.

Linux network namespaces are not enough by themselves. On Spark, some pairs of
mlx5 netdevs can exchange traffic through an internal NIC or e-switch path. In
that case DAQIRI and vport counters move, but physical counters such as
`rx_packets_phy` do not. Treat benchmark results as "on wire" only after the
directional PHY counters move.

## Summary

The pair below was verified to send packets out through the physical path:

```text
client TX: enp1s0f0np0
server RX: enp1s0f1np1
```

The pair below did not hit the physical receive path on the tested Spark
machine:

```text
client TX: enp1s0f0np0
server RX: enP2p1s0f0np0
```

For one-way client to server traffic, the counters that must increase are:

```text
client interface: tx_packets_phy, tx_bytes_phy
server interface: rx_packets_phy, rx_bytes_phy
```

The client `tx_packets_phy` delta should match the server `rx_packets_phy`
delta, apart from a few control packets. The byte counters should also match.

## Required DAQIRI Socket Changes

Use a DAQIRI build that includes the socket backend and the socket changes from
this branch:

- UDP payloads above `65507` bytes are rejected, matching iperf's UDP payload
  limit.
- TCP client setup reuses the configured primary connection when the requested
  peer matches.
- The TCP RX loop no longer erases its own connection state while running in its
  RX thread.

Build inside the DAQIRI container, as usual:

```bash
docker run --rm --privileged --network=host --gpus all --ipc=host \
  --user "$(id -u):$(id -g)" \
  -v /dev/hugepages:/dev/hugepages \
  -v "$PWD:/work" \
  -w /work daqiri:local \
  cmake --build build-spark-rdma --target daqiri_bench_socket -j"$(nproc)"
```

If the build directory is not configured yet:

```bash
docker run --rm --privileged --network=host --gpus all --ipc=host \
  --user "$(id -u):$(id -g)" \
  -v /dev/hugepages:/dev/hugepages \
  -v "$PWD:/work" \
  -w /work daqiri:local \
  bash -lc 'cmake -S . -B build-spark-rdma \
    -DBUILD_SHARED_LIBS=ON \
    -DDAQIRI_BUILD_PYTHON=OFF \
    -DDAQIRI_MGR="dpdk socket rdma" &&
    cmake --build build-spark-rdma --target daqiri_bench_socket -j"$(nproc)"'
```

## Identify Candidate Ports

List the mlx5 netdevs, PCI functions, and physical port names:

```bash
for ifc in enp1s0f0np0 enp1s0f1np1 enP2p1s0f0np0 enP2p1s0f1np1; do
  [ -e "/sys/class/net/$ifc" ] || continue
  echo "=== $ifc ==="
  ip -br link show dev "$ifc"
  readlink -f "/sys/class/net/$ifc/device"
  cat "/sys/class/net/$ifc/phys_port_name" 2>/dev/null || true
  cat "/sys/class/net/$ifc/dev_port" 2>/dev/null || true
  ethtool "$ifc" | grep -E 'Speed:|Link detected:'
done
```

On the tested Spark, all four links were up at 100 Gb/s. The verified wire path
used two ports on the same card:

```text
enp1s0f0np0 -> enp1s0f1np1
```

Do not assume the same names on another host. First find the interfaces, then
prove the selected pair with PHY counters.

## Namespace Setup

Run networking setup as root. The easiest way on this repo is to use the
`daqiri:local` container in privileged host-network mode because it can move
host interfaces into namespaces without requiring host sudo prompts:

```bash
docker run --rm -it --privileged --network=host --pid=host --ipc=host \
  --gpus all \
  -v "$PWD:/work" \
  -v /tmp:/tmp \
  -w /work daqiri:local bash
```

Install tools inside the container if needed:

```bash
apt-get update
apt-get install -y iproute2 iputils-ping ethtool iperf3
```

Set variables for the verified wire-facing pair:

```bash
CLIENT_NS=dq_wire_client
SERVER_NS=dq_wire_server

CLIENT_IF=enp1s0f0np0
SERVER_IF=enp1s0f1np1

CLIENT_IP=10.250.0.1
SERVER_IP=10.250.0.2

CLIENT_MAC=4c:bb:47:2a:ea:ed
SERVER_MAC=4c:bb:47:2a:ea:ee

MTU=9082
```

Create the namespaces:

```bash
ip netns delete "$CLIENT_NS" >/dev/null 2>&1 || true
ip netns delete "$SERVER_NS" >/dev/null 2>&1 || true

ip addr flush dev "$CLIENT_IF" || true
ip addr flush dev "$SERVER_IF" || true

ip netns add "$CLIENT_NS"
ip netns add "$SERVER_NS"

ip link set "$CLIENT_IF" netns "$CLIENT_NS"
ip link set "$SERVER_IF" netns "$SERVER_NS"

ip -n "$CLIENT_NS" addr add "$CLIENT_IP/24" dev "$CLIENT_IF"
ip -n "$SERVER_NS" addr add "$SERVER_IP/24" dev "$SERVER_IF"

ip -n "$CLIENT_NS" link set lo up
ip -n "$SERVER_NS" link set lo up
ip -n "$CLIENT_NS" link set "$CLIENT_IF" mtu "$MTU" up
ip -n "$SERVER_NS" link set "$SERVER_IF" mtu "$MTU" up

ip -n "$CLIENT_NS" route add "$SERVER_IP/32" dev "$CLIENT_IF"
ip -n "$SERVER_NS" route add "$CLIENT_IP/32" dev "$SERVER_IF"

ip -n "$CLIENT_NS" neigh replace "$SERVER_IP" \
  lladdr "$SERVER_MAC" dev "$CLIENT_IF" nud permanent
ip -n "$SERVER_NS" neigh replace "$CLIENT_IP" \
  lladdr "$CLIENT_MAC" dev "$SERVER_IF" nud permanent
```

Verify that routing is pinned to the namespace interfaces:

```bash
ip -n "$CLIENT_NS" route get "$SERVER_IP" from "$CLIENT_IP"
ip -n "$SERVER_NS" route get "$CLIENT_IP" from "$SERVER_IP"
ip netns exec "$CLIENT_NS" ping -c 1 -W 1 "$SERVER_IP"
```

The route output should name the physical netdevs, not `lo`.

## Prove the Pair Hits the Wire

Capture the counters before and after a short transfer. For a client to server
test, use the client TX PHY counter and the server RX PHY counter:

```bash
ip netns exec "$CLIENT_NS" ethtool -S "$CLIENT_IF" | \
  grep -E 'tx_packets_phy|tx_bytes_phy|tx_vport_unicast'
ip netns exec "$SERVER_NS" ethtool -S "$SERVER_IF" | \
  grep -E 'rx_packets_phy|rx_bytes_phy|rx_vport_unicast'
```

Use iperf3 as a quick proof before running DAQIRI:

```bash
ip netns exec "$SERVER_NS" iperf3 -s -B "$SERVER_IP" -1 &
sleep 1
ip netns exec "$CLIENT_NS" iperf3 -c "$SERVER_IP" -B "$CLIENT_IP" -t 2 -P 1
```

Then check the counters again. On the verified Spark pair,
`client tx_packets_phy` and `server rx_packets_phy` increased by the same packet
count. That proves traffic is entering the server NIC from the physical path.

If only `tx_vport_unicast_*` and `rx_vport_unicast_*` move, but `rx_packets_phy`
does not, the traffic is not on the wire. Pick another port pair.

## DAQIRI Socket Configs

The socket benchmark config must use the namespace IPs. Use a large iteration
count because current `daqiri_bench_socket` treats `iterations: 0` as zero work,
not as "run until --seconds expires".

Server template:

```yaml
%YAML 1.2
---
daqiri:
  cfg:
    version: 1
    stream_type: "socket"
    protocol: "udp"
    master_core: 3
    debug: false
    log_level: "info"
    memory_regions:
    - name: "DATA_SOCKET_SERVER"
      kind: "host"
      affinity: 0
      num_bufs: 1024
      buf_size: 65507
    interfaces:
    - name: udp_server
      address: 10.250.0.2
      socket_config:
        mode: server
        local_ip: 10.250.0.2
        local_port: 5021
        remote_ip: ""
        remote_port: 0
        max_payload_size: 65535
      rx:
        queues:
        - name: "RX_Queue"
          id: 0
          cpu_core: 8
          batch_size: 1
          memory_regions: ["DATA_SOCKET_SERVER"]
      tx:
        queues:
        - name: "TX_Queue"
          id: 0
          cpu_core: 7
          batch_size: 1
          memory_regions: ["DATA_SOCKET_SERVER"]

socket_bench_server:
  server: true
  send: false
  receive: true
  iterations: 1000000000
  message_size: 65507
  server_address: 10.250.0.2
  client_address: 10.250.0.1
  server_port: 5021
```

Client template:

```yaml
%YAML 1.2
---
daqiri:
  cfg:
    version: 1
    stream_type: "socket"
    protocol: "udp"
    master_core: 3
    debug: false
    log_level: "info"
    memory_regions:
    - name: "DATA_SOCKET_CLIENT"
      kind: "host"
      affinity: 0
      num_bufs: 1024
      buf_size: 65507
    interfaces:
    - name: udp_client
      address: 10.250.0.1
      socket_config:
        mode: client
        local_ip: 10.250.0.1
        local_port: 5121
        remote_ip: 10.250.0.2
        remote_port: 5021
        max_payload_size: 65535
      rx:
        queues:
        - name: "RX_Queue"
          id: 0
          cpu_core: 8
          batch_size: 1
          memory_regions: ["DATA_SOCKET_CLIENT"]
      tx:
        queues:
        - name: "TX_Queue"
          id: 0
          cpu_core: 7
          batch_size: 1
          memory_regions: ["DATA_SOCKET_CLIENT"]

socket_bench_client:
  server: false
  send: true
  receive: false
  iterations: 1000000000
  message_size: 65507
  server_address: 10.250.0.2
  client_address: 10.250.0.1
  server_port: 5021
```

For TCP tests, set `protocol: "tcp"` in both files and use distinct ports for
parallel processes. For UDP tests, keep message sizes at or below `65507`.

Run server and client:

```bash
export LD_LIBRARY_PATH=/work/build-spark-rdma/src:${LD_LIBRARY_PATH:-}
BIN=/work/build-spark-rdma/examples/daqiri_bench_socket

ip netns exec "$SERVER_NS" env LD_LIBRARY_PATH="$LD_LIBRARY_PATH" \
  "$BIN" /tmp/server.yaml --seconds 11 --mode server &

sleep 1

ip netns exec "$CLIENT_NS" env LD_LIBRARY_PATH="$LD_LIBRARY_PATH" \
  "$BIN" /tmp/client.yaml --seconds 10 --mode client

wait
```

For a four-process run, generate four server/client config pairs with ports such
as `5021`, `5022`, `5023`, and `5024`, then launch all four servers followed by
all four clients. Use different client local ports, for example server port plus
100.

## Known Verified Results

The following DAQIRI matrix was run on the verified physical path
`enp1s0f0np0 -> enp1s0f1np1` with four client/server process pairs:

| Protocol | Message size | App TX | App RX | Loss | Client `tx_packets_phy` | Server `rx_packets_phy` |
|---|---:|---:|---:|---:|---:|---:|
| TCP | 1000 | 10.93 Gb/s | 10.93 Gb/s | 0.00% | 1,513,047 | 1,513,047 |
| TCP | 8000 | 11.20 Gb/s | 11.20 Gb/s | 0.00% | 1,550,052 | 1,550,052 |
| TCP | 1 MiB | 11.67 Gb/s | 11.67 Gb/s | 0.00% | 1,615,399 | 1,615,399 |
| UDP | 1000 | 12.28 Gb/s | 11.68 Gb/s | 4.88% | 15,350,463 | 15,350,463 |
| UDP | 8000 | 12.93 Gb/s | 10.10 Gb/s | 21.91% | 2,020,461 | 2,020,461 |
| UDP | 65507 | 12.84 Gb/s | 12.41 Gb/s | 3.34% | 1,960,392 | 1,960,392 |

UDP 1 MiB was intentionally skipped because the socket backend rejects UDP
payloads above `65507`.

## Restore Host Networking

After tests, move interfaces back to the host and restore the usual Spark IPs.
Adjust interface names and addresses for the target machine:

```bash
for ns in "$CLIENT_NS" "$SERVER_NS"; do
  ip netns exec "$ns" ip link set "$CLIENT_IF" netns 1 >/dev/null 2>&1 || true
  ip netns exec "$ns" ip link set "$SERVER_IF" netns 1 >/dev/null 2>&1 || true
done

ip netns delete "$CLIENT_NS" >/dev/null 2>&1 || true
ip netns delete "$SERVER_NS" >/dev/null 2>&1 || true

for ifc in enp1s0f0np0 enp1s0f1np1 enP2p1s0f0np0 enP2p1s0f1np1; do
  ip addr flush dev "$ifc" >/dev/null 2>&1 || true
  ip link set dev "$ifc" mtu 9082 up >/dev/null 2>&1 || true
done

ip addr add 1.1.1.1/24 dev enp1s0f0np0
ip addr add 2.2.2.2/24 dev enP2p1s0f0np0

ip neigh replace 2.2.2.2 lladdr 4c:bb:47:2a:ea:f1 \
  dev enp1s0f0np0 nud permanent
ip neigh replace 1.1.1.1 lladdr 4c:bb:47:2a:ea:ed \
  dev enP2p1s0f0np0 nud permanent
```

## Loopback Disable Knobs

Two mlx5 loopback knobs were considered:

```bash
ethtool --set-priv-flags <interface> local_lb off
mlxconfig -d <device> set FORCE_LOOPBACK_DISABLE=1
```

On the tested Spark, the `local_lb` private flag was not exposed by
`ethtool --show-priv-flags`, so the runtime flag could not be used.

`mlxconfig` was present in the container, but direct PCI config queries hung in
this session. No firmware settings were changed. Treat
`FORCE_LOOPBACK_DISABLE=1` as a maintenance-window experiment: query and set it
only with the proper Mellanox firmware tooling available, then reset or reboot
as required and rerun the same `rx_packets_phy` proof.
