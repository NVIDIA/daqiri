# Socket and RDMA First Run

Use this path for Linux UDP/TCP sockets, `roce://`, RDMA/RoCE, namespace wire tests, and socket baseline benchmarking.

## Protocol Selection

- TCP: `stream_type: "socket"` with `tcp://` endpoint URIs; run `daqiri_bench_socket`.
- UDP: `stream_type: "socket"` with `udp://` endpoint URIs; run `daqiri_bench_socket`; payloads must be at most 65507 bytes.
- RoCE/RDMA: `stream_type: "socket"` with `roce://` endpoint URIs; run `daqiri_bench_rdma`; build with `ibverbs`.

## Build

Build socket and RDMA benchmark targets in the container when source-mounted:

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

Use the installed `/opt/daqiri/bin` binaries if the container already has them.

## Test Shell

Use a privileged, host-networked shell for namespace and RDMA setup:

```bash
docker run --rm -it --privileged --network=host --pid=host --ipc=host \
  --gpus all \
  -v "$PWD:/work" \
  -v /tmp:/tmp \
  -w /work daqiri:local bash
```

Install tools inside the container if missing:

```bash
apt-get update
apt-get install -y iproute2 iputils-ping ethtool iperf3 rdma-core ibverbs-utils
```

## Namespace Wire Test

Use namespaces when validating that TCP/UDP traffic leaves through the intended physical NIC path. Ask for or discover:

- client and server netdevs,
- client and server IPs,
- client and server MACs,
- MTU,
- whether a cable or peer path exists.

After namespace setup, verify:

```bash
ip -n "$CLIENT_NS" route get "$SERVER_IP" from "$CLIENT_IP"
ip -n "$SERVER_NS" route get "$CLIENT_IP" from "$SERVER_IP"
ip netns exec "$CLIENT_NS" ping -c 1 -W 1 "$SERVER_IP"
```

The route output must name the namespace interface, not `lo`.

## Counter Proof

Before trusting a wire result, collect directional counters before and after a short transfer:

```bash
ip netns exec "$CLIENT_NS" ethtool -S "$CLIENT_IF" | \
  grep -E 'tx_packets_phy|tx_bytes_phy|tx_vport_unicast'
ip netns exec "$SERVER_NS" ethtool -S "$SERVER_IF" | \
  grep -E 'rx_packets_phy|rx_bytes_phy|rx_vport_unicast'
```

Use `iperf3` as a quick path proof before DAQIRI when needed:

```bash
ip netns exec "$SERVER_NS" iperf3 -s -B "$SERVER_IP" -1 &
sleep 1
ip netns exec "$CLIENT_NS" iperf3 -c "$SERVER_IP" -B "$CLIENT_IP" -t 2 -P 1
wait
```

Treat the result as on-wire only when client TX PHY and server RX PHY counters increase by matching packet counts.

## Socket Smoke Tests

The shipped socket configs use `127.0.0.1` and are useful for a basic smoke test:

```bash
./build-socket-rdma/examples/daqiri_bench_socket \
  examples/daqiri_bench_socket_udp_tx_rx.yaml \
  --seconds 10 --mode both

./build-socket-rdma/examples/daqiri_bench_socket \
  examples/daqiri_bench_socket_tcp_tx_rx.yaml \
  --seconds 10 --mode both
```

For on-wire tests, generate or adapt separate server/client YAML files. Check URI schemes, namespace IPs, server port, `max_payload_size`, memory-region `buf_size`, and benchmark `message_size`.

## RDMA/RoCE Run

Start server before client:

```bash
./build-socket-rdma/examples/daqiri_bench_rdma <server.yaml> \
  --mode server --seconds 30

./build-socket-rdma/examples/daqiri_bench_rdma <client.yaml> \
  --mode client --seconds 30
```

For RoCE, kernel reachability to the peer matters. If namespace RDMA device visibility fails, inspect `ibv_devinfo`, `rdma link show`, and whether the matching RDMA device needs to be moved into the namespace.

## Interpretation

- UDP: require matching application TX/RX packets and bytes, plus no increase in `Udp: InErrors`, `RcvbufErrors`, IP reassembly failures, or NIC receive discards.
- TCP: report delivered and achieved rates; retain retransmission and socket-error counters. Flow control may reduce achieved throughput without app loss.
- RoCE/RDMA: require non-zero send/receive completions and no RDMA-CM, completion-queue, retry, or queue-resource errors.
- Record TX/RX depths, buffer counts, CPU placement, and MTU for repeatability.

## Cleanup

Restore namespace-moved interfaces and delete temporary namespaces when finished:

```bash
ip netns delete "$CLIENT_NS"
ip netns delete "$SERVER_NS"
```
