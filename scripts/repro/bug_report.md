# Flow Isolation Blocks Kernel Traffic on Unmatched Packets

## Summary

On DGX Spark, starting DAQIRI raw RX with `rx.flow_isolation: true` on `det1`
breaks normal Linux kernel traffic on the same PF. Packets that do not match the
configured DAQIRI flow should remain available to the Linux kernel, but ping to
the peer stops while DAQIRI is running and recovers after DAQIRI exits.

## Hardware and Network

- Host under test: spark1
- Peer: spark2
- spark1 netdev: `det1`
- spark1 BDF: `0000:01:00.0`
- spark1 IP: `169.254.101.10/24`
- spark2 IP: `169.254.101.100/24`

## Build

Build DAQIRI freshly on spark1 from this branch:

```bash
export PKG_CONFIG_PATH=/opt/nvidia-gpu-rx/dpdk/lib/aarch64-linux-gnu/pkgconfig:${PKG_CONFIG_PATH:-}

cmake -S . -B build-flow-isolation \
  -DBUILD_SHARED_LIBS=ON \
  -DDAQIRI_BUILD_PYTHON=OFF \
  -DDAQIRI_MGR="dpdk socket rdma"
cmake --build build-flow-isolation -j
```

The tested branch was:

```text
33b75869a0bc3afee1b2aa20d8b39a4139615d7c
```

## Reproducer

First verify ping works before DAQIRI starts:

```bash
ping -I det1 -c 5 169.254.101.100
```

Start the RX-only DAQIRI process:

```bash
sudo env LD_LIBRARY_PATH="$PWD/build-flow-isolation/src:$PWD/build-flow-isolation/src/third_party/yaml-cpp:/opt/nvidia-gpu-rx/dpdk/lib/aarch64-linux-gnu:/usr/local/cuda/lib64:${LD_LIBRARY_PATH:-}" \
  ./build-flow-isolation/examples/daqiri_bench_raw_gpudirect \
  scripts/repro/rx_flow_isolation.yaml \
  --seconds 30
```

Root privileges are required for the DPDK/mlx5 attach on this host. Running the
same host-built binary as an unprivileged user failed before the repro with mlx5
DevX/TIS allocation errors.

While that process is running, ping the peer again from another shell:

```bash
ping -I det1 -c 5 169.254.101.100
```

After DAQIRI exits, ping should recover:

```bash
ping -I det1 -c 5 169.254.101.100
```

## Expected Behavior

The DAQIRI YAML installs a raw RX flow that matches only UDP destination port
`42337`. ICMP ping does not match that flow, so ping should continue to work via
the Linux kernel while DAQIRI is running.

## Observed Behavior

Ping succeeds before DAQIRI starts, fails with 100% packet loss while DAQIRI is
running in flow isolation mode, and succeeds again after DAQIRI exits.

Before DAQIRI:

```text
5 packets transmitted, 5 received, 0% packet loss, time 4086ms
rtt min/avg/max/mdev = 0.471/0.534/0.582/0.037 ms
```

While DAQIRI is running:

```text
5 packets transmitted, 0 received, 100% packet loss, time 4119ms
```

After DAQIRI exits:

```text
5 packets transmitted, 5 received, 0% packet loss, time 4109ms
rtt min/avg/max/mdev = 0.238/0.488/0.558/0.125 ms
```

DAQIRI logs from the same run confirm that flow isolation was active:

```text
Port 0 in isolation mode
Successfully configured ethdev
Successfully started port 0
Adding UDP port match for dst 42337
RX complete: interface=rx_port queue=0 packets=0 bytes=0 bursts=0 seconds=30.0421
```

## Minimal Config

See `scripts/repro/rx_flow_isolation.yaml`.
