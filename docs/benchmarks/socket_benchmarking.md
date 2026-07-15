---
hide:
  - navigation
---

# Socket and RDMA Benchmarking

Use this page when the peer transport is TCP, UDP, or RoCE/RDMA. These benchmarks use the Linux networking stack for TCP/UDP and RDMA verbs for RoCE, so the same client/server namespace shape is useful for proving that traffic leaves the host through the expected NIC path.

For **two-host Spark cross-cable** tests (not netns), RoCE/RDMA still needs kernel reachability to the peer — apply the host route and static neighbor steps in [System Configuration → Cross-host variant](../tutorials/system_configuration.md#cross-host-variant-two-sparks) before running `daqiri_bench_rdma` with the `_xhost` configs.

Make sure to [build DAQIRI](../getting-started.md#build-the-daqiri-library) with the `ibverbs` engine first (for the RoCE/RDMA benchmark); Linux UDP/TCP sockets are always available.

## Protocol choices

| Transport | YAML selector | Benchmark executable | Typical reason to use it |
|---|---|---|---|
| TCP | `stream_type: "socket"` with `tcp://` endpoint URIs | `daqiri_bench_socket` | Baseline against normal Linux streams or test a TCP-speaking peer. |
| UDP | `stream_type: "socket"` with `udp://` endpoint URIs | `daqiri_bench_socket` | Datagram baseline against Linux networking. UDP payloads must be at most `65507` bytes. |
| RoCE/RDMA | `stream_type: "socket"` and `roce://` endpoint URIs | `daqiri_bench_rdma` | Compare DAQIRI RDMA verbs against tools such as `ib_send_bw` or `ib_write_bw`. |

## Build and launch a test shell

Build the socket and RDMA benchmarks inside the DAQIRI container:

```bash
docker run --rm --privileged --network=host --gpus all --ipc=host \
  --user "$(id -u):$(id -g)" \
  -v /dev/hugepages:/dev/hugepages \
  -v "$PWD:/work" \
  -w /work daqiri:local \
  bash -lc 'cmake -S . -B build-socket-rdma \
    -DBUILD_SHARED_LIBS=ON \
    -DDAQIRI_BUILD_PYTHON=OFF \
    -DDAQIRI_ENGINE="dpdk ibverbs" &&
    cmake --build build-socket-rdma \
      --target daqiri_bench_socket daqiri_bench_rdma -j"$(nproc)"'
```

Run the benchmark setup commands as root. The easiest path is a privileged, host-networked DAQIRI container:

```bash
docker run --rm -it --privileged --network=host --pid=host --ipc=host \
  --gpus all \
  -v "$PWD:/work" \
  -v /tmp:/tmp \
  -w /work daqiri:local bash
```

Install network tools inside the container if needed:

```bash
apt-get update
apt-get install -y iproute2 iputils-ping ethtool iperf3 rdma-core ibverbs-utils
```

## Create isolated namespaces

Choose one transmit-facing interface and one receive-facing interface. The example below uses the Spark pair that was verified to increment physical counters on the tested system; adjust names, IPs, and MAC addresses on other machines.

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

Create namespaces and pin routes to the physical interfaces:

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

Verify the route and a short control packet:

```bash
ip -n "$CLIENT_NS" route get "$SERVER_IP" from "$CLIENT_IP"
ip -n "$SERVER_NS" route get "$CLIENT_IP" from "$SERVER_IP"
ip netns exec "$CLIENT_NS" ping -c 1 -W 1 "$SERVER_IP"
```

The route output should name the namespace interface, not `lo`.

!!! note "RDMA device visibility"

    On most RoCE setups, the RDMA device follows the netdev/GID association used by the namespace. If `ibv_devinfo` or `rdma link show` inside a namespace cannot see the expected device, move the matching RDMA device into the namespace with `rdma dev set <rdma_device> netns <namespace>`, or run the RDMA benchmark in the host namespace and still verify the same physical counters.

## Prove the pair hits the wire

Capture directional PHY counters before and after a short transfer. For one-way client-to-server traffic:

```bash
ip netns exec "$CLIENT_NS" ethtool -S "$CLIENT_IF" | \
  grep -E 'tx_packets_phy|tx_bytes_phy|tx_vport_unicast'
ip netns exec "$SERVER_NS" ethtool -S "$SERVER_IF" | \
  grep -E 'rx_packets_phy|rx_bytes_phy|rx_vport_unicast'
```

Use `iperf3` as a quick proof before running DAQIRI:

```bash
ip netns exec "$SERVER_NS" iperf3 -s -B "$SERVER_IP" -1 &
sleep 1
ip netns exec "$CLIENT_NS" iperf3 -c "$SERVER_IP" -B "$CLIENT_IP" -t 2 -P 1
wait
```

Then check the counters again. Treat the result as on-wire only when the client `tx_packets_phy` and server `rx_packets_phy` counters increase by matching packet counts. If only vport counters move, pick a different port pair.

## Run the Linux socket benchmark

The shipped configs run both endpoints on `127.0.0.1` and are useful for a smoke test:

```bash
./build-socket-rdma/examples/daqiri_bench_socket \
  examples/daqiri_bench_socket_udp_tx_rx.yaml \
  --seconds 10 --mode both

./build-socket-rdma/examples/daqiri_bench_socket \
  examples/daqiri_bench_socket_tcp_tx_rx.yaml \
  --seconds 10 --mode both
```

For an on-wire namespace test, use separate server and client YAML files. The important fields are the endpoint URI scheme, namespace IPs, server port, `max_payload_size`, memory-region `buf_size`, and benchmark `message_size`.

Applications can tune the underlying TCP/UDP socket after resolving a connection ID
with `socket_connect_to_server()` or `socket_get_server_conn_id()`. Use
`socket_setsockopt(conn_id, level, optname, optval, optlen)` with the integer
constants from your system headers.

Server-side UDP template:

```yaml
%YAML 1.2
---
daqiri:
  cfg:
    version: 1
    stream_type: "socket"
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
        local_addr: "udp://10.250.0.2:5021"
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
  cpu_core: 9
  iterations: 1000000000
  message_size: 65507
  server_address: 10.250.0.2
  client_address: 10.250.0.1
  server_port: 5021
```

Client-side UDP template:

```yaml
%YAML 1.2
---
daqiri:
  cfg:
    version: 1
    stream_type: "socket"
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
        local_addr: "udp://10.250.0.1:5121"
        remote_addr: "udp://10.250.0.2:5021"
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
  cpu_core: 9
  iterations: 1000000000
  message_size: 65507
  server_address: 10.250.0.2
  client_address: 10.250.0.1
  server_port: 5021
```

For TCP, change each endpoint URI scheme from `udp://` to `tcp://` in both files. For UDP, keep `message_size` at or below `65507`.

Run the server and client in their namespaces:

```bash
export LD_LIBRARY_PATH=/work/build-socket-rdma/src:${LD_LIBRARY_PATH:-}
BIN=/work/build-socket-rdma/examples/daqiri_bench_socket

ip netns exec "$SERVER_NS" env LD_LIBRARY_PATH="$LD_LIBRARY_PATH" \
  "$BIN" /tmp/socket-server.yaml --seconds 11 --mode server &

sleep 1

ip netns exec "$CLIENT_NS" env LD_LIBRARY_PATH="$LD_LIBRARY_PATH" \
  "$BIN" /tmp/socket-client.yaml --seconds 10 --mode client

wait
```

For a four-process run, create four server/client YAML pairs with unique server ports such as `5021`, `5022`, `5023`, and `5024`, and unique client local ports such as `5121`, `5122`, `5123`, and `5124`.

## Run the RDMA RoCE benchmark

Start from `examples/daqiri_bench_rdma_tx_rx.yaml` or `examples/daqiri_bench_rdma_tx_rx_spark.yaml`. The full config can run both endpoints in one process:

```bash
./build-socket-rdma/examples/daqiri_bench_rdma \
  examples/daqiri_bench_rdma_tx_rx_spark.yaml \
  --seconds 10 --mode both
```

`rdma_bench_server` and `rdma_bench_client` accept `cpu_core` plus optional
`tx_depth` and `rx_depth` fields. `cpu_core` pins the benchmark application's
server/client role thread, separately from the DAQIRI queue worker cores. The
depth defaults follow the `ib_send_bw` shape:
`tx_depth: 128`, `rx_depth: 512`. The benchmark preposts receive work before
sending, refills receive work after completions, and keeps the transmit side
bounded by `tx_depth`. For 4 KiB SEND-style stress tests, set the RX/TX
memory-region `num_bufs` high enough for the chosen windows, for example at
least `rx_depth` receive buffers and `tx_depth` transmit buffers per side.

For namespace testing, split the file by role just as in the Linux socket test:

- The server YAML keeps the server memory regions, the server interface with `socket_config.mode: server`, and `rdma_bench_server`.
- The client YAML keeps the client memory regions, the client interface with `socket_config.mode: client`, and `rdma_bench_client`.
- Both files use `stream_type: "socket"` and `roce://` endpoint URIs.
- The client DAQIRI interface uses its local `roce://` endpoint; `rdma_bench_client.server_address` and `server_port` choose the peer at the app layer.
- `rdma_bench_client.client_address` should be the client namespace IP.

Run the split RDMA test with the same namespace pair:

```bash
export LD_LIBRARY_PATH=/work/build-socket-rdma/src:${LD_LIBRARY_PATH:-}
BIN=/work/build-socket-rdma/examples/daqiri_bench_rdma

ip netns exec "$SERVER_NS" env LD_LIBRARY_PATH="$LD_LIBRARY_PATH" \
  "$BIN" /tmp/rdma-server.yaml --seconds 11 --mode server &

sleep 1

ip netns exec "$CLIENT_NS" env LD_LIBRARY_PATH="$LD_LIBRARY_PATH" \
  "$BIN" /tmp/rdma-client.yaml --seconds 10 --mode client

wait
```

Use `ib_send_bw` or `ib_write_bw` in the same namespaces as a comparison baseline, and monitor `mlnx_perf` or `ethtool -S` on the same directional interfaces.

## Example Spark socket results

The following DAQIRI socket matrix was run on the verified physical path `enp1s0f0np0 -> enp1s0f1np1` with four client/server process pairs:

| Protocol | Message size | App TX | App RX | Loss | Client `tx_packets_phy` | Server `rx_packets_phy` |
|---|---:|---:|---:|---:|---:|---:|
| TCP | 1000 | 10.93 Gb/s | 10.93 Gb/s | 0.00% | 1,513,047 | 1,513,047 |
| TCP | 8000 | 11.20 Gb/s | 11.20 Gb/s | 0.00% | 1,550,052 | 1,550,052 |
| TCP | 1 MiB | 11.67 Gb/s | 11.67 Gb/s | 0.00% | 1,615,399 | 1,615,399 |
| UDP | 1000 | 12.28 Gb/s | 11.68 Gb/s | 4.88% | 15,350,463 | 15,350,463 |
| UDP | 8000 | 12.93 Gb/s | 10.10 Gb/s | 21.91% | 2,020,461 | 2,020,461 |
| UDP | 65507 | 12.84 Gb/s | 12.41 Gb/s | 3.34% | 1,960,392 | 1,960,392 |

UDP 1 MiB is intentionally skipped because Linux UDP payloads above `65507` bytes require fragmentation or segmentation behavior outside the benchmark's supported payload model.

## Restore host networking

After tests, move interfaces back to the host and restore the usual IPs. Adjust names and addresses for the target machine:

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
```

## Loopback disable knobs

If namespace isolation still increments only vport counters, check whether the platform exposes loopback control:

```bash
ethtool --show-priv-flags <interface>
ethtool --set-priv-flags <interface> local_lb off

mlxconfig -d <device> q | grep FORCE_LOOPBACK_DISABLE
mlxconfig -d <device> set FORCE_LOOPBACK_DISABLE=1
```

Treat firmware settings as maintenance-window changes: query first, set only with the proper Mellanox tooling available, then reset or reboot as required and rerun the same `rx_packets_phy` proof.

---
**Previous:** [Benchmarking](index.md)<br>
**Next:** [Raw Ethernet Benchmarking](raw_benchmarking.md)
