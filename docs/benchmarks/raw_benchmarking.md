---
hide:
  - navigation
---

# Raw Ethernet Benchmarking

DAQIRI provides raw Ethernet benchmark applications that use DPDK to drive an NVIDIA NIC directly. This page walks through `daqiri_bench_raw_gpudirect`, the TX/RX loopback config, and the raw Ethernet checks needed before interpreting throughput results.

Make sure to [build the DAQIRI library](../getting-started.md#build-the-daqiri-library) beforehand.

**Not sure which stream type to benchmark?** Start with the [Benchmarking overview](benchmarks.md). Use this page after you have chosen the raw Ethernet stream type. Use [Socket and RDMA Benchmarking](socket_benchmarking.md) for TCP, UDP, and RoCE/RDMA runs.

!!! note "Prerequisites"

    Before running the benchmarking application, ensure your system has been fully configured per the [System Configuration](../tutorials/system_configuration.md) page.

## Configure hugepages first

Size the hugepage pool to your YAML's `memory_regions` plus DPDK overhead before running. DAQIRI's preflight aborts with an actionable error (and the exact `echo N | sudo tee …` to fix it) if the pool is too small, so the simplest workflow is: run once, copy-paste the recommendation. To check current state:

```bash
grep Huge /proc/meminfo
```

For a persistent allocation across reboots, use the grub recipe in [Step 4 of System Configuration](../tutorials/system_configuration.md#step-4-enable-huge-pages).

## Running the DAQIRI container

If you built DAQIRI using the container approach, use the following command to launch the container with Raw Ethernet (DPDK) and GPU support. The host system must be fully configured (see [System Configuration](../tutorials/system_configuration.md)) before the container can access the NIC and GPU hardware.

```bash
docker run --rm -it --privileged \
  --runtime=nvidia \
  --network=host \
  -v /dev/hugepages:/dev/hugepages \
  daqiri:local bash
```

??? info "Explanation of container flags"

    | Flag | Purpose |
    |------|---------|
    | `--privileged` | DPDK requires raw access to NIC hardware, PCI devices, and hugepage files. |
    | `--runtime=nvidia` | Makes the host GPU visible inside the container via the NVIDIA Container Toolkit |
    | `--network=host` | Shares the host network namespace so DPDK can discover the physical NIC interfaces and their PCIe topology |
    | `-v /dev/hugepages:/dev/hugepages` | Mounts the hugepage filesystem for DPDK memory allocation (`--privileged` alone does not cover mounted filesystems) |

## Update the loopback configuration

!!! tip "DGX Spark"

    For systems configured per the [DGX Spark profile](../tutorials/system_configuration.md#dgx-spark-profile), use these configs to skip the PCIe/IP/CPU-core edits below:

    - [`daqiri_bench_raw_tx_rx_spark.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_tx_rx_spark.yaml) for `daqiri_bench_raw_gpudirect` — still set `eth_dst_addr` to the RX MAC. The rx_port is `0002:01:00.1` (physical port p1), so read its MAC: `cat /sys/class/net/enP2p1s0f1np1/address`. This p0-to-p1 pairing is intentional for an over-the-wire single-machine loopback; using two PFs that map to the same physical port exercises the on-chip eswitch path instead.
    - [`daqiri_bench_rdma_tx_rx_spark.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_rdma_tx_rx_spark.yaml) for `daqiri_bench_rdma` — no further edits needed.

    For the multi-queue core-scaling matrix, use the single base config [`daqiri_bench_raw_tx_rx_spark_mq.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_tx_rx_spark_mq.yaml) (the balanced TX=2/RX=2 superset) with `daqiri_bench_raw_gpudirect`, driven by [`run_spark_mq_bench.sh`](https://github.com/nvidia/daqiri/blob/main/examples/run_spark_mq_bench.sh) — it derives the four `(TX, RX)` cells from the base (via `scripts/gen_spark_mq_config.py`) and sweeps the payload.

    The Spark configs also pin the benchmark application's `bench_tx.cpu_core` / `bench_rx.cpu_core` fields to the high-frequency Cortex-X925 cores. Keep both the DAQIRI queue cores and the application worker cores on cores 16-19 unless you intentionally want a lower-power core in the measurement.

#### Cross-host two-DGX-Spark loopback

If you have two DGX Sparks cross-cabled p0↔p0 instead of a chassis QSFP loop on one machine, use the `_xhost` configs. Each host runs only its own role, so the YAML on each side configures one port instead of two. Both hosts must already be set up per the [DGX Spark profile](../tutorials/system_configuration.md#dgx-spark-profile), with one adjustment: the `daqiri-tx` (`1.1.1.1/24`) and `daqiri-rx` (`2.2.2.2/24`) nmcli profiles are *split across* the two hosts — bring up `daqiri-tx` on the TX host's p0 and `daqiri-rx` on the RX host's p0, instead of both on one box.

**Network prerequisite (required for RDMA; recommended for raw).** Assigning `/24` addresses on each host is not enough for the kernel to reach the peer over a direct cable. RDMA-CM uses the kernel stack, so you need a host route and a static neighbor on the cabled port before ping or RoCE will work. Run [`scripts/setup_spark_xhost_net.sh`](https://github.com/nvidia/daqiri/blob/main/scripts/setup_spark_xhost_net.sh) on **both** hosts after bringing up the nmcli profile — see the [cross-host variant](../tutorials/system_configuration.md#cross-host-variant-two-sparks) in System Configuration for the full steps.

```bash
# TX host (peer MAC from RX: cat /sys/class/net/enp1s0f0np0/address)
sudo scripts/setup_spark_xhost_net.sh --role tx --peer-mac <RX_P0_MAC>

# RX host (peer MAC from TX)
sudo scripts/setup_spark_xhost_net.sh --role rx --peer-mac <TX_P0_MAC>

# Verify on each host before starting benches
ping -c 3 <peer-ip>    # 2.2.2.2 on TX, 1.1.1.1 on RX
ip route get <peer-ip> # must name enp1s0f0np0, not lo
```

**Raw GPUDirect.** Start the RX side first so the flow rule is installed before any traffic arrives:

```bash
# RX host
sudo ./daqiri_bench_raw_gpudirect daqiri_bench_raw_rx_spark_xhost.yaml --seconds 30

# TX host (set eth_dst_addr to the RX host p0's MAC first: cat /sys/class/net/enp1s0f0np0/address on the RX host)
sudo ./daqiri_bench_raw_gpudirect daqiri_bench_raw_tx_spark_xhost.yaml --seconds 30
```

Verify both sides report non-zero packet counts and no `NO_FREE_BURST_BUFFERS` / `NO_FREE_PACKET_BUFFERS` errors.

**RDMA.** Start the server side first:

```bash
# RX (server) host
sudo ./daqiri_bench_rdma daqiri_bench_rdma_tx_rx_spark_xhost.yaml --mode server --seconds 30

# TX (client) host
sudo ./daqiri_bench_rdma daqiri_bench_rdma_tx_rx_spark_xhost.yaml --mode client --seconds 30
```

Verify both sides report non-zero send/receive completions and no `CQ error` / `RETRY_EXC_ERR` lines in the client log.

The benchmark executables and example YAML configurations are located at:

| | Binaries | YAML configs |
|---|---|---|
| **Container** | `/opt/daqiri/bin/` | `/opt/daqiri/bin/` |
| **From source** | `./build/examples/` | `./examples/` |

The fields in the YAML configs will be explained in more detail in [Understanding the Configuration File](../tutorials/configuration-walkthrough.md). For now, we'll stick to modifying the strict minimum required fields to run the application as-is on your system.

### Hardware tunnel transform examples

Raw DPDK and raw ibverbs builds can program hardware flow actions that push or
encapsulate on TX and pop or decapsulate on RX. The application packet buffers
remain pre-encap on TX and post-decap on RX; DAQIRI accounts for outer-header
overhead when sizing MTU/wire frames.

| Transform | YAML config | Binary |
|---|---|---|
| VXLAN encap + decap | [`daqiri_bench_raw_tx_rx_vxlan.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_tx_rx_vxlan.yaml) | `daqiri_bench_raw_gpudirect` |
| VLAN push + pop | [`daqiri_bench_raw_tx_rx_vlan.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_tx_rx_vlan.yaml) | `daqiri_bench_raw_gpudirect` |
| GRE encap + decap | [`daqiri_bench_raw_tx_rx_gre.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_tx_rx_gre.yaml) | `daqiri_bench_raw_gpudirect` |
| NVGRE encap + decap | [`daqiri_bench_raw_tx_rx_nvgre.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_tx_rx_nvgre.yaml) | `daqiri_bench_raw_gpudirect` |

##### Identify your NIC's PCIe addresses

Retrieve the PCIe addresses of both ports of your NIC. We'll arbitrarily use the first for Tx and the second for Rx here:

=== "ibdev2netdev"

    ```bash
    sudo ibdev2netdev -v | awk '{print $1}'
    ```

=== "lspci"

    ```bash
    # `0200` is the PCI-SIG class code for NICs
    # `15b3` is the Vendor ID for Mellanox
    lspci -n | awk '$2 == "0200:" && $3 ~ /^15b3:/ {print $1}'
    ```

??? abstract "See an example output"

    ```
    0005:03:00.0
    0005:03:00.1
    ```

##### Configure the NIC for Tx and Rx

Set the NIC addresses in the `interfaces` section of the `daqiri` configuration, making sure to remove the template brackets `< >`. This configures your NIC independently of your application:

- Set the `address` field of the `tx_port` interface to one of these addresses. That interface will be able to transmit ethernet packets.
- Set the `address` field of the `rx_port` interface to the other address. This interface will be able to receive ethernet packets.

```yaml hl_lines="3 7"
interfaces:
    - name: "tx_port"
    address: <0000:00:00.0>       # The BUS address of the interface doing Tx
    tx:
        ...
    - name: "rx_port"
    address: <0000:00:00.0>       # The BUS address of the interface doing Rx
    rx:
        ...
```

???+ abstract "See an example yaml"

    ```yaml hl_lines="3 7"
    interfaces:
        - name: "tx_port"
        address: 0005:03:00.0       # The BUS address of the interface doing Tx
        tx:
            ...
        - name: "rx_port"
        address: 0005:03:00.1       # The BUS address of the interface doing Rx
        rx:
            ...
    ```

##### Configure the application

To run the benchmarking application to run a loopback on your system, you'll need to modify the `bench_tx` section which configures the application itself, to create the packet headers, pin the application TX worker, and direct the packets to the NIC. Make sure to remove the template brackets `< >`.

- `eth_dst_addr` with the MAC address (and not the PCIe address) of the NIC interface you want to use for Rx. You can get the MAC address of your `if_name` interface with `#!bash cat /sys/class/net/$if_name/address`:
- `cpu_core` with the CPU core for the benchmark application's TX thread. This is separate from the DAQIRI TX queue `cpu_core`; use a different isolated core when you have one, or deliberately share when the machine has a tight core budget.

```yaml hl_lines="3 5"
bench_tx:
- interface_name: "tx_port" # Name of the TX port from the daqiri config
  cpu_core: 10              # Benchmark application TX thread affinity
  ...
  eth_dst_addr: <00:00:00:00:00:00> # Destination MAC address - required when Rx flow_isolation=true
  ...
```

???+ abstract "See an example yaml"

    ```yaml hl_lines="3 5"
    bench_tx:
    - interface_name: "tx_port" # Name of the TX port from the daqiri config
      cpu_core: 10              # Benchmark application TX thread affinity
      ...
      eth_dst_addr: 48:b0:2d:ee:83:ad # Destination MAC address - required when Rx flow_isolation=true
      ...
    ```

??? info "Show explanation"

    - `eth_dst_addr` - the destination ethernet MAC address - will be embedded in the packet headers by the application. This is required here because the Rx interface above has `flow_isolation: true` (explained in more details below). In that configuration, only the packets listing the adequate destination MAC address will be accepted by the Rx interface.
    - `cpu_core` - the benchmark application's own TX worker thread affinity. Set the matching `bench_rx.cpu_core` for RX workers too; these app-thread fields are distinct from the DAQIRI queue `cpu_core` values that poll the NIC.
    - We ignore the IP fields (`ip_src_addr`, `ip_dst_addr`) for now, as we are testing on a layer 2 network by just connecting a cable between the two interfaces on our system, therefore having mock values has no impact.
    - You might have noted the lack of a `eth_src_addr` field in this `bench_tx` section. This is because the source Ethernet MAC address can be inferred automatically by the DAQIRI library from the PCIe address of the Tx interface referenced above.

## Run the loopback test

After having modified the configuration file, ensure you have connected an SFP cable between the two interfaces of your NIC, then run the application with the command below:

=== "Containerized"

    [Launch the DAQIRI container](#running-the-daqiri-container), then inside:

    ```bash
    /opt/daqiri/bin/daqiri_bench_raw_gpudirect /opt/daqiri/bin/daqiri_bench_raw_tx_rx.yaml
    ```

=== "From source"

    This assumes you have built DAQIRI and its dependencies locally on your system.

    ```bash
    sudo ./build/examples/daqiri_bench_raw_gpudirect examples/daqiri_bench_raw_tx_rx.yaml
    ```

By default the application runs for 10 seconds and then exits. You can change the duration by passing `--seconds <N>` after the YAML path, or stop it gracefully at any time with `Ctrl-C`.

`daqiri_bench_raw_gpudirect` and `daqiri_bench_raw_hds` also accept `--workload none|fft|gemm|gemm_fp16`, which runs a representative GPU workload once per received reorder window on the **actual received packet data**: `fft` (batched cuFFT C2C transform), `gemm` (FP32 `cublasSgemm`), or `gemm_fp16` (the same-size mixed-precision FP16/tensor-core matmul that models inference). Each received burst's payloads are first reordered by sequence number into a contiguous GPU buffer (`examples/bench_pipeline.{h,cu}`) that the compute then consumes. The same `--workload` flag is honoured by the RoCE bench (`daqiri_bench_rdma`, in-order gather) and the socket bench (`daqiri_bench_socket`, host→device stage then UDP reorder / TCP gather). See the [DGX Spark GPU-workload results](performance-dgx-spark.md#gpu-workloads-in-the-receive-path).

## Flow programming smoke test

Raw Ethernet flow rules are programmed into the NIC during `daqiri_init()`. Software loopback
(`loopback: "sw"`) skips NIC init entirely, so it is a build/runtime smoke test only — not a
flow programming test.

| Step | Command / action | Expected |
|------|------------------|----------|
| Build smoke | `daqiri_bench_raw_sw_loopback.yaml --seconds 5` | Init succeeds; no NIC flows created |
| Good NIC config | `daqiri_bench_raw_tx_rx.yaml` (filled placeholders, cabled NIC) | Init succeeds; RX and `tx_eth_src` flows programmed |
| Dynamic RX flow config | `daqiri_example_dynamic_rx_flow.yaml` with `daqiri_example_dynamic_rx_flow` | Starts with `flow_isolation: true` and no `rx.flows`, drops unmatched traffic, then adds/deletes runtime UDP steering rules |
| Bad queue ID | Copy `daqiri_bench_raw_tx_rx.yaml`, set `flows[0].action.id: 99` | Fails in `validate_config()` before EAL/NIC init with `references unknown RX queue` |
| Mixed flows (optional) | On one interface, add two of the three flow classes — standard UDP/IP, flex-item (see `rx.flex_items`), or eCPRI (`match.ecpri`) — in the [configuration reference](../api-reference/configuration.md) | Fails in `validate_config()` with `mixes standard (UDP/IP), flex-item and/or eCPRI` |
| eCPRI flow (optional) | On a cabled NIC, add a flow with `match: { ecpri: { msg_type: 0, pc_id: 1 } }` (see the [configuration reference](../api-reference/configuration.md)) | Init succeeds; eCPRI-over-Ethernet (EtherType 0xAEFE) frames matching the message type and pc_id steer to the flow's queue |

## Cap the transmit rate with packet pacing

To meter the transmit side at a fixed rate in hardware, set a per-queue `pacing_mbps` cap
on the TX queue. [`daqiri_bench_raw_tx_rx_pacing.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_tx_rx_pacing.yaml)
is the loopback config above with `pacing_mbps: 10000` (10 Gbps) on the TX queue. Pacing is
supported only on the default DPDK engine; the `ibverbs` engine does not support `pacing_mbps`
(init fails if it is set on an `ibverbs` queue). The NIC meters the queue out so its average
TX rate stays at or below the configured value; the limit is enforced on an average basis and
idle gaps do not accumulate burst credit.

```bash
/opt/daqiri/bin/daqiri_bench_raw_gpudirect /opt/daqiri/bin/daqiri_bench_raw_tx_rx_pacing.yaml --seconds 10
```

Validate the cap from the `RX complete:` line: `Gbps = bytes * 8 / seconds / 1e9`. With
`pacing_mbps: 10000` the achieved rate should sit at or just below 10 Gbps regardless of
link speed. Change `pacing_mbps` (or set it to `0` to disable pacing and send at line rate)
and re-run to see the cap move.

Pacing requires a Mellanox/mlx5 NIC with hardware send scheduling (ConnectX-7 or later). On
devices without it, `pacing_mbps` is ignored with a warning at init and TX runs at line rate.

## Tune RDMA SEND completion signaling

The RDMA engine signals every SEND work request by default. For `daqiri_bench_rdma`
runs where CQ polling overhead is part of the bottleneck investigation, set
`DAQIRI_RDMA_SEND_SIGNAL_EVERY=N` to request one signaled SEND every `N` posts:

```bash
DAQIRI_RDMA_SEND_SIGNAL_EVERY=16 \
  /opt/daqiri/bin/daqiri_bench_rdma /opt/daqiri/bin/daqiri_bench_rdma_tx_rx.yaml
```

Larger values reduce SEND completion traffic, but completions also drive
application credits and buffer recycling in the benchmark. Keep the variable
unset, or set it to `1`, for baseline measurements.

## Watch live OpenTelemetry metrics in Grafana

DAQIRI can expose the raw benchmark counters through OpenTelemetry when metrics
support is enabled at build time. The Grafana example uses the same benchmark
binary and YAML files as the loopback test above, then starts Prometheus and
Grafana beside the benchmark process.

Build the container with metrics enabled:

```bash
DAQIRI_ENABLE_OTEL_METRICS=ON DAQIRI_ENGINE="dpdk ibverbs" scripts/build-container.sh
```

Before starting the stack, fill in the required `<placeholders>` in the benchmark
YAML you plan to run. You can also pass a machine-local copy through
`DAQIRI_CONFIG` so the tracked example YAML keeps its placeholder syntax.

```bash
cd examples/grafana
DAQIRI_CONFIG=/workspace/daqiri/examples/daqiri_bench_raw_tx_rx.yaml \
DAQIRI_SECONDS=60 \
docker compose up
```

Prometheus scrapes `http://localhost:9464/metrics`, and Grafana serves the
`DAQIRI OpenTelemetry Metrics` dashboard at `http://localhost:3000`. The
throughput panel reports payload counter rates in `Gb/s` for each active
interface and queue.

??? abstract "See an example output"

    This is an illustrative excerpt; source line numbers and pointer values vary by build.

    ```log
    [INFO] /workspace/daqiri/src/../include/daqiri/common.h:1045: Finished reading DAQIRI configuration
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:1732: Attempting to use 2 ports for high-speed network
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:1741: Setting DPDK log level to: Info
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:1775: DPDK EAL arguments: operator --file-prefix=vwcrlqhfkb -l 3,11,9 --log-level=error --log-level=pmd.net.mlx5:info --iova-mode=va -a 0005:03:00.0,txq_inline_max=0,dv_flow_en=2 -a 0005:03:00.1,txq_inline_max=0,dv_flow_en=2 
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:1799: tx_port (0005:03:00.0): identified as port 0
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:1799: rx_port (0005:03:00.1): identified as port 1
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:1809: Creating dummy RX and TX queues
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:1591: Port 0 has no RX queues. Creating dummy queue.
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:1624: Port 1 has no TX queues. Creating dummy queue.
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:1542: Adjusting buffer size to 9228 for headroom
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:1542: Adjusting buffer size to 9228 for headroom
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:1542: Adjusting buffer size to 8192 for headroom
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:1542: Adjusting buffer size to 8192 for headroom
    [INFO] /workspace/daqiri/src/engine.cpp:175: Registering memory regions
    [INFO] /workspace/daqiri/src/engine.cpp:236: Successfully allocated memory region MR_Unused_TX_P1 at 0x16d9bf580 type 2 with 9100 bytes (32768 elements @ 9228 bytes total 302383104)
    [INFO] /workspace/daqiri/src/engine.cpp:236: Successfully allocated memory region MR_Unused_P0 at 0x15b95f500 type 2 with 9100 bytes (32768 elements @ 9228 bytes total 302383104)
    [INFO] /workspace/daqiri/src/engine.cpp:236: Successfully allocated memory region Data_RX_GPU at 0xffff34000000 type 3 with 8064 bytes (32768 elements @ 8192 bytes total 268435456)
    [INFO] /workspace/daqiri/src/engine.cpp:236: Successfully allocated memory region Data_TX_GPU at 0xffff24000000 type 3 with 8064 bytes (32768 elements @ 8192 bytes total 268435456)
    [INFO] /workspace/daqiri/src/engine.cpp:249: Finished allocating memory regions
    [INFO] /workspace/daqiri/src/engine.cpp:314: dma-buf supported for device 0
    [INFO] /workspace/daqiri/src/engine.cpp:324: dma-buf GPU buffer address at 0xffff24000000 aligned at 0xffff24000000 with aligned size 268435456
    [INFO] /workspace/daqiri/src/engine.cpp:364: Successfully registered external memory for Data_TX_GPU
    [INFO] /workspace/daqiri/src/engine.cpp:314: dma-buf supported for device 0
    [INFO] /workspace/daqiri/src/engine.cpp:324: dma-buf GPU buffer address at 0xffff34000000 aligned at 0xffff34000000 with aligned size 268435456
    [INFO] /workspace/daqiri/src/engine.cpp:364: Successfully registered external memory for Data_RX_GPU
    [INFO] /workspace/daqiri/src/engine.cpp:277: Mapped external memory descriptor for 0xffff34000000 to device 0
    [INFO] /workspace/daqiri/src/engine.cpp:277: Mapped external memory descriptor for 0xffff24000000 to device 0
    [INFO] /workspace/daqiri/src/engine.cpp:277: Mapped external memory descriptor for 0xffff34000000 to device 1
    [INFO] /workspace/daqiri/src/engine.cpp:277: Mapped external memory descriptor for 0xffff24000000 to device 1
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:1850: DPDK init (0005:03:00.0) -- RX: ENABLED TX: ENABLED
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:1863: Configuring RX queue: UNUSED_P0_Q0 (0) on port 0
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:1896: Created mempool RXP_P0_Q0_MR0 : mbufs=32768 elsize=9228 ptr=0x17fca4380
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:1955: Configuring TX queue: tx_q_0 (0) on port 0
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:1981: Created mempool TXP_P0_Q0_MR0 : mbufs=32768 elsize=8064 ptr=0x148cdc980
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:1995: Max frame needed - RX: 0 TX: 8064
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2012: Setting port config for port 0 mtu:8046
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2026: Enabling RX scatter offload for single-segment RX queues (min buffer size: 9100)
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2062: Initializing port 0 with 1 RX queues and 1 TX queues...
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2079: Successfully configured ethdev
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2091: Successfully set descriptors to 8192/8192
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2106: Port 0 not in isolation mode
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2115: Setting up port:0, queue:0, Num scatter:1 pool:0x17fca4380
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2136: Successfully setup RX port 0 queue 0
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2158: Successfully set up TX queue 0/0
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2163: Enabling promiscuous mode for port 0
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2177: Successfully started port 0
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2180: Port 0, MAC address: 48:B0:2D:F4:04:23
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2891: Applying tx_eth_src offload for port 0
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:1850: DPDK init (0005:03:00.1) -- RX: ENABLED TX: ENABLED
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:1863: Configuring RX queue: rq_q_0 (0) on port 1
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:1896: Created mempool RXP_P1_Q0_MR0 : mbufs=32768 elsize=8192 ptr=0x147d6a980
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:1908: Max packet size needed for RX: 8064
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:1955: Configuring TX queue: UNUSED_TX_P1_Q0 (0) on port 1
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:1981: Created mempool TXP_P1_Q0_MR0 : mbufs=32768 elsize=9100 ptr=0x1470e9e00
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:1995: Max frame needed - RX: 8064 TX: 0
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2012: Setting port config for port 1 mtu:8046
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2026: Enabling RX scatter offload for single-segment RX queues (min buffer size: 8064)
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2062: Initializing port 1 with 1 RX queues and 1 TX queues...
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2079: Successfully configured ethdev
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2091: Successfully set descriptors to 8192/8192
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2103: Port 1 in isolation mode
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2115: Setting up port:1, queue:0, Num scatter:1 pool:0x147d6a980
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2136: Successfully setup RX port 1 queue 0
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2158: Successfully set up TX queue 1/0
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2166: Not enabling promiscuous mode on port 1 since flow isolation is enabled
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2177: Successfully started port 1
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2180: Port 1, MAC address: 48:B0:2D:F4:04:24
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2192: Adding RX flow flow_0
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2670: Adding UDP port match for src 4096
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2677: Adding UDP port match for dst 4096
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2246: Setting up RX burst pool with 8191 batches of size 81920
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2265: Setting up RX burst pool with 8191 batches of size 20480
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2284: Setting up RX meta pool with 256 buffers
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2307: Setting up TX ring TX_RING_P0_Q0
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2333: Setting up TX burst pool TX_BURST_POOL_P0_Q0 with 10240 pointers at 0x14703e380
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2307: Setting up TX ring TX_RING_P1_Q0
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2333: Setting up TX burst pool TX_BURST_POOL_P1_Q0 with 10240 pointers at 0x1848bf700
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2340: Setting up TX meta pool with 256 buffers
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_stats.cpp:34: Initializing DPDK stats
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_stats.cpp:55: Port 0, Queue 0: Memory regions: MR_Unused_P0
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_stats.cpp:55: Port 1, Queue 0: Memory regions: Data_RX_GPU
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_stats.cpp:130: Found rx_q0_errors counter at index 10
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_stats.cpp:152: Initialized DPDK xstats for port 0, found 70 stats
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_stats.cpp:154: Found rx_missed counter at index 4
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_stats.cpp:160: Found mbuf allocation counter at index 7
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_stats.cpp:130: Found rx_q0_errors counter at index 10
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_stats.cpp:152: Initialized DPDK xstats for port 1, found 70 stats
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_stats.cpp:154: Found rx_missed counter at index 4
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_stats.cpp:160: Found mbuf allocation counter at index 7
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_stats.cpp:169: Initialized DPDK stats
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:3169: Config validated successfully
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:3182: Starting DAQIRI workers
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_stats.cpp:201: Starting stats thread on core 3
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:3287: Flushing packet on port 1
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:3525: Starting RX Core 9, port 1, queue 0, socket 0
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:3762: Starting TX Core 11, port 0, queue 0 socket 0 using burst pool 0x14703e380 ring 0x1852c8700
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:3277: Done starting workers
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:4239: daqiri DPDK engine stats
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2913: Port 0:
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2915:  - Received packets:    0
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2916:  - Transmit packets:    45722624
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2917:  - Received bytes:      0
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2918:  - Transmit bytes:      368707239936
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2919:  - Missed packets:      0
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2920:  - Errored packets:     0
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2921:  - RX out of buffers:   0
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2923:    ** Extended Stats **
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2953:       tx_good_packets:		45728768
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2953:       tx_good_bytes:		368756785152
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2953:       tx_q0_packets:		45728768
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2953:       tx_q0_bytes:		368756785152
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2953:       tx_unicast_bytes:		368681991552
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2953:       tx_unicast_packets:		45719493
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2953:       tx_phy_packets:		45719292
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2953:       tx_phy_bytes:		368863334632
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2913: Port 1:
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2915:  - Received packets:    45720948
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2916:  - Transmit packets:    0
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2917:  - Received bytes:      368693724672
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2918:  - Transmit bytes:      0
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2919:  - Missed packets:      0
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2920:  - Errored packets:     0
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2921:  - RX out of buffers:   0
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2923:    ** Extended Stats **
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2953:       rx_good_packets:		45726554
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2953:       rx_good_bytes:		368738931456
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2953:       rx_q0_packets:		45726554
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2953:       rx_q0_bytes:		368738931456
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2953:       rx_unicast_bytes:		368729399808
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2953:       rx_unicast_packets:		45725372
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2953:       rx_phy_packets:		45725224
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:2953:       rx_phy_bytes:		368911131436
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:4226: daqiri DPDK engine shutdown called 1
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:4229: daqiri DPDK engine shutting down
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:3676: Total packets received by application (port/queue 1/0): 45726776
    [INFO] /workspace/daqiri/src/engines/dpdk/daqiri_dpdk_engine.cpp:3814: Total packets transmitted by application (port/queue 0/0): 45731840
    RX complete: packets=45711360 bytes=368616407040 bursts=4464
    ```

To inspect the speed the data is moving through the NIC, run `mlnx_perf` on one of the interfaces in a separate terminal, concurrently with the application running:

```bash
sudo mlnx_perf -i $if_name
```

The `*_packets_phy` and `*_bytes_phy` counters are physical-link counters. They increase when packets cross the wire through the QSFP/SerDes side of the NIC. If a DGX Spark same-machine loopback uses two PFs that map to the same physical port, traffic can be switched on-chip and the vport counters may rise while the physical counters stay flat.

??? abstract "See an example output"

    On IGX with RTX 6000 Ada Generation, we saturate the 100 Gbps linerate with this configuration:
    ```log
          rx_vport_unicast_packets: 1,562,203
            rx_vport_unicast_bytes: 12,597,604,992 Bps   = 100,780.83 Mbps
                    rx_packets_phy: 1,562,198
                      rx_bytes_phy: 12,603,813,464 Bps   = 100,830.50 Mbps
         rx_4096_to_8191_bytes_phy: 1,562,186
                    rx_prio0_bytes: 12,603,256,772 Bps   = 100,826.5 Mbps
                  rx_prio0_packets: 1,562,128
    ```

??? tip "Troubleshooting"

    ??? failure "Cannot create HWS action since HWS is not supported"

        Example error:

        ```log
        mlx5_net: [mlx5dr_action_create_generic_bulk]: Cannot create HWS action since HWS is not supported
        mlx5_net: Failed to start port 0 0005:03:00.0: fail to configure port
        [CRITICAL] Cannot start device err=-95, port=0
        ```

        Raw Ethernet (DPDK-backed) uses Hardware Steering (HWS) via the `dv_flow_en=2` mlx5 device argument. HWS requires compatible versions of both the NIC firmware and the host's MLNX_OFED kernel modules. Per the [DPDK mlx5 documentation](https://doc.dpdk.org/guides/nics/mlx5.html), the minimum requirements are ConnectX-6 Dx or later with firmware `xx.35.1012`+, but the host's OFED/kernel driver must also support the HWS features expected by the DPDK version in use.

        Check your OFED and firmware versions:

        ```bash
        cat /sys/module/mlx5_core/version   # Host OFED kernel module version
        ethtool -i <interface_name> | grep firmware  # NIC firmware version
        ```

        To resolve, update your NIC firmware and OFED drivers, or contact the DAQIRI team for guidance on compatible version combinations.

    ??? failure "EAL: failed to parse device"

        Make sure to set valid PCIe addresses in the `address` fields in `interfaces`, per [instructions above](#configure-the-nic-for-tx-and-rx).

    ??? failure "Invalid MAC address format"

        Make sure to set a valid MAC address in the `eth_dst_addr` field in `bench_tx`, per [instructions above](#configure-the-application).

    ??? failure "mlx5_common: Fail to create MR for address [...] Could not DMA map EXT memory"

        Example error:

        ```log
        mlx5_common: Fail to create MR for address (0xffff2fc00000)
        mlx5_common: Device 0005:03:00.0 unable to DMA map
        [critical] [adv_network_dpdk_mgr.cpp:188] Could not DMA map EXT memory: -1 err=Invalid argument
        [critical] [adv_network_dpdk_mgr.cpp:430] Failed to map MRs
        ```

        Check the [GPUDirect setup](../tutorials/system_configuration.md#enable-gpudirect) for your
        deployment. Some host builds use `nvidia-peermem`; the container path uses
        dma-buf support from the patched DPDK build.

    ??? failure "EAL: Couldn't get fd on hugepage file [..] error allocating rte services array"

        Example error:

        ```log
        EAL: get_seg_fd(): open '/mnt/huge/nwlrbbmqbhmap_0' failed: Permission denied
        EAL: Couldn't get fd on hugepage file
        EAL: error allocating rte services array
        EAL: FATAL: rte_service_init() failed
        EAL: rte_service_init() failed
        ```

        Ensure you run as root, using `sudo`.

    ??? failure "EAL: Cannot get hugepage information."

        ```log
        EAL: x hugepages of size x reserved, no mounted hugetlbfs found for that size
        ```

        Ensure your [hugepages are mounted](../tutorials/system_configuration.md#step-4-enable-huge-pages).

        ```log
        EAL: No free x kB hugepages reported on node 0
        ```

        Reachable only when the in-process preflight is bypassed (e.g. running an older binary against a host with hugepages reserved but not mounted). Mount per [System Configuration: Step 4](../tutorials/system_configuration.md#step-4-enable-huge-pages) and re-run.

    ??? failure "Stale `<file-prefix>map_*` files in /dev/hugepages after a SIGKILL"

        Init- and shutdown-path cleanup is automatic. Files only leak if the process is `SIGKILL`ed (OOM, container hard-stop). Symptom: `HugePages_Free: 0` with no bench running.

        ```bash
        pgrep -af daqiri_bench   # confirm nothing is running
        sudo rm -f /dev/hugepages/*map_* /mnt/huge/*map_*
        ```

    ??? failure "Could not allocate x MB of GPU memory [...] Failed to allocate GPU memory"

        Check your GPU utilization:

        ```bash
        nvidia-smi pmon -c 1
        ```

        You might need to kill some of the listed processes to free up GPU VRAM.

---
**Previous:** [Benchmarking](benchmarks.md)<br>
**Next:** [Understanding the Configuration File](../tutorials/configuration-walkthrough.md) — deep dive into the YAML parameters
