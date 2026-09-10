---
hide:
  - navigation
---

# Performance: DGX Spark

This report covers DAQIRI receive performance in two configurations.
[Cross-host measurements](#cross-host-benchmarks) include [I/O throughput](#raw-ethernet-gpudirect),
[I/O with GPU workloads](#gpu-workloads-in-the-receive-path), and
[I/O with ResNet inference](#end-to-end-inference-pipeline-resnet).
[Single-host wire-loopback diagnostics](#single-host-loopback-benchmarks) cover I/O only.

## Cross-host benchmarks

### Cross-host setup

| Component | Detail |
| --------- | ------ |
| Platform | NVIDIA DGX Spark (receiver) |
| Data source | Another DGX Spark for these measurements. The report is about receiver performance; another source may be used when it can sustain the required offered load. |
| CPU | NVIDIA GB10 Armv9: 10 Cortex-X925 performance cores + 10 Cortex-A725 efficiency cores |
| GPU and memory | NVIDIA GB10 GPU; 120 GiB unified memory per host |
| NIC | NVIDIA ConnectX-7; two 100 GbE Ethernet ports per host |
| NIC firmware | 28.45.4028 |
| NIC attachment | Two independently attached NIC connections are used for the two-link 200 GbE tests |
| Network | Direct one- and two-link 100 GbE cross-host tests; MTU 9000 |
| Host software | Ubuntu 24.04.4 LTS; Linux 6.17.0-1014-nvidia |
| GPU software | NVIDIA driver 580.142; CUDA Toolkit 13.1.0 in the benchmark container |
| DAQIRI build | Release; `DAQIRI_ENGINE="dpdk ibverbs"`; `DAQIRI_BUILD_APPLICATIONS=ON` when running the inference pipeline |
| CPU placement | Dedicated isolated CPU placement for raw/RDMA pollers and workers. Socket worker and I/O-thread placement is stated with each scaling result. |

Rates report received payload unless a table explicitly labels a wire rate.
Each result is the average of three independent 30 s samples.

### Results summary

**Loss-free two-link receive throughput.**

| Stream / Protocol | Message size | Receive setup | Delivered <span class="unit">Gbps</span> |
| ----------------- | -----------: | ------------- | -------------: |
| Raw Ethernet / GPUDirect (DPDK) | 8 KB | 1 queue/link | 197.17 |
| Socket / RoCE (RC SEND) | 8 MB | 1 RX queue/link | **194.51 ±0.11** |
| Socket / TCP | 1 MiB | 4 RX cores/link | 174.3 ±0.9 |
| Socket / UDP (paced) | 8 KB | 4 RX cores/link | 95.98 ±0.00 |

### Raw Ethernet / GPUDirect

**DPDK receive throughput vs payload size. Average of three 30 s samples;
loss-free two-link results.**

| Payload | Wire <span class="unit">Gbps</span> | App <span class="unit">Gbps</span> | Mpps |
| ------- | --------: | -------: | ---: |
| 8000 B | **201.70** | **197.17** | 3.056 |
| 4096 B | 201.29 | 197.19 | 5.925 |
| 1024 B | 198.62 | 194.68 | 22.367 |
| 256 B  | 73.71 | 72.80 | 28.436 |
| 64 B   | 30.35 | 29.43 | 28.740 |

### Socket / RoCE

**RoCE RC SEND receive throughput vs message size. Average of three 30 s samples.**

| Message size | App <span class="unit">Gbps</span> |
| ------------ | -------: |
| 8 MB | **194.51 ±0.11** |
| 1 MB | 194.23 ±0.54 |
| 8 KB | 171.05 ±0.46 |
| 4 KB | 66.97 ±5.92 |

### Socket / TCP

#### Single Rx core, one link

**TCP receive throughput vs message size with one RX worker on one link.
Loss-free results.**

| Message size | App <span class="unit">Gbps</span> |
| ------------ | -------: |
| 1 MiB  | **55.7** |
| 8000 B | 52.9 |
| 1000 B | 18.3 |

#### Multiple Rx cores, two links

**TCP receive throughput vs RX workers per link at 1 MiB. Average of three 30 s
samples; `±` is the sample standard deviation. Loss-free two-link results.**

| Rx workers | Rx workers per link | App <span class="unit">Gbps</span> |
| ---------: | ------------------: | -------: |
| 2 | 1 | 110.7 ±6.6 |
| 4 | 2 | 156.9 ±1.7 |
| 8 | 4 | **174.3 ±0.9** |

### Socket / UDP

#### Single Rx core, one link

**Loss-free UDP receive throughput vs message size with one RX worker on one link.
Average of three 30 s samples.**

| Message size | Loss-free app <span class="unit">Gbps</span> |
| ------------ | -----------------: |
| 8000 B  | **25.00** |
| 65507 B | 15.00 |
| 1000 B  | 4.00 |

#### Multiple Rx cores, two links

**Loss-free UDP receive throughput vs RX cores per link at 8 KB. Average of three
30 s samples.**

| Rx workers | Rx cores per link | Pacer target / flow (<span class="unit">Gbps</span>) | App <span class="unit">Gbps</span> |
| ---------: | ----------------: | ------------------------------------------------------: | -------: |
| 2 | 1 | 25 | **50.00** |
| 4 | 2 | 20 | **79.99** |
| 8 | 4 | 12 | **95.98** |

For TCP and UDP scaling, use isolated Cortex-X925 performance cores. Pin the
socket I/O thread and application worker to separate cores in the same CPU cluster,
and distribute workers evenly across the two links.

### GPU workloads in the receive path

Each workload runs once per ~8 MB received-data window after DAQIRI assembles it
into a contiguous GPU buffer. Raw and RoCE receive buffers are GPU-accessible;
sockets are not included in this comparison.

- **FFT** — batched FP32, length-1024 complex-to-complex forward transforms.
- **GEMM** — FP32 1024×1024 matrix multiply (2.15 GFLOP; 4 MB input).

DPDK uses 8 KB packets and RoCE uses 8 MB messages, yielding the same approximate
working-set size per workload invocation.

**Cross-host, one-link receive throughput with GPU workloads. Average of three
30 s samples; `±` is the sample standard deviation.**

| Workload | DPDK <span class="unit">Gbps</span> | RoCE <span class="unit">Gbps</span> |
| -------- | ------------------------------------: | -----------------------------------: |
| none (baseline) | 97.918 ±0.008 | 96.633 ±0.265 |
| FFT             | 97.904 ±0.002 | 95.953 ±1.041 |
| GEMM (FP32)     | 97.906 ±0.013 | 91.468 ±4.516 |

### End-to-end inference pipeline (ResNet)

The [ResNet pipeline](../tutorials/daqiri-resnet-inference.md) receives CIFAR-10
images, reassembles and converts them on the GPU, then runs TensorRT FP16
inference. The results cover ResNet-18 through ResNet-152.

```mermaid
flowchart LR
  TX["TX host<br/>CIFAR-10 int8 frames"] -->|"direct Ethernet"| N["RX host NIC DMA"]
  N --> R["RX buffers (host_pinned,<br/>GPU-accessible)"]
  R --> K["DAQIRI reorder kernel:<br/>reassemble + int8 to fp16"]
  K -->|"REORDERED burst + CUDA event"| Q["SPSC ring"]
  Q --> T["TensorRT FP16<br/>ResNet (18-152)"]
  T --> F["feature vectors"]
```

Each image is 224×224×3 signed int8 (150,528 B), sent as 128 frames. The GPU
reorder kernel reassembles and converts the input to FP16 for TensorRT.

**ResNet receive throughput at the highest loss-free offered rate for each model.
Batch 32, TensorRT FP16; average of three 30 s samples.**

| Model | img/s | p50 / p99 ms per batch | Delivered payload <span class="unit">Gbps</span> |
| ----- | ----: | ---------------------: | -----------------------------------------------: |
| ResNet-18  | **11,833** | 2.58 / 3.09 | 14.25 |
| ResNet-34  | 7,174  | 4.35 / 4.94 | 8.64 |
| ResNet-50  | 3,550  | 8.60 / 9.54 | 4.28 |
| ResNet-101 | 2,367  | 13.28 / 14.17 | 2.85 |
| ResNet-152 | 1,590  | 18.38 / 19.46 | 1.92 |

## Single-host loopback benchmarks

### Single-host loopback benchmark setup

| Component | Detail |
| --------- | ------ |
| Platform | NVIDIA DGX Spark |
| Data source | The same DGX Spark acts as sender and receiver |
| CPU | NVIDIA GB10 Armv9: 10 Cortex-X925 performance cores + 10 Cortex-A725 efficiency cores |
| GPU and memory | NVIDIA GB10 GPU; 120 GiB unified memory |
| NIC | NVIDIA ConnectX-7; two 100 GbE Ethernet ports |
| NIC firmware | 28.45.4028 |
| NIC attachment | The two ports are joined by a 100 GbE cable |
| Network | Single-host wire loopback; MTU 9000 |
| Host software | Ubuntu 24.04.4 LTS; Linux 6.17.0-1014-nvidia |
| GPU software | NVIDIA driver 580.142; CUDA Toolkit 13.1.0 in the benchmark container |
| DAQIRI build | Release; `DAQIRI_ENGINE="dpdk ibverbs"` |
| CPU placement | Dedicated isolated CPU placement for raw/RDMA pollers and workers. Socket worker and I/O-thread placement is stated with each scaling result. |
| Transport setup | Raw/DPDK uses the physical ports directly; sockets and RoCE use a network-namespace wire loopback |

These I/O-only diagnostics isolate polling, batching, queue, and CPU-placement
effects. They are not cross-host performance claims.

### Results summary

**Single-host loopback I/O diagnostics.**

| Stream / Protocol | Message size | Receive setup | Delivered <span class="unit">Gbps</span> |
| ----------------- | -----------: | ------------- | -----------------------------------------: |
| Raw Ethernet / GPUDirect (DPDK) | 8 KB | 1 RX queue | **98.65 ±0.01** |
| Raw Ethernet / GPUDirect (DPDK) | 256 B | 1 RX queue, paced | **46.11 ±0.08** |
| Socket / RoCE | 8 KB | one receive queue, TX depth 512 | **96.37 ±0.06** |
| Socket / TCP | 1 MiB | 4 RX workers | **97.68 ±0.17** |
| Socket / UDP | 8 KiB | 4 RX workers | **64.73 ±0.66** |

All results are three 30 s samples. UDP results are loss-free by physical and
kernel receive counters. The receiver may retain one final message-retrieval
batch when the timed window ends; that bounded accounting tail does not affect
the reported rate or loss classification.

### Raw Ethernet / GPUDirect

**DPDK loopback receive throughput vs payload. Average of three 30 s samples;
no hardware drops.**

| Payload | App <span class="unit">Gbps</span> |
| ------- | -----------------------------------: |
| 8000 B | **98.65 ±0.01** |
| 4096 B | 98.52 ±0.00 |
| 1024 B | 97.05 ±0.00 |

**Loss-free small-packet receive points. Average of three 30 s samples.**

| Payload | Offered rate | App <span class="unit">Gbps</span> |
| ------- | -----------: | -----------------------------------: |
| 256 B | 50 Gbps | **46.11 ±0.08** |
| 64 B  | 24.5 Gbps | **20.14 ±0.05** |

Unpaced 256 B and 64 B transmission overruns the receive buffer. The paced rows
are the highest offered rates with no reported DPDK hardware-buffer discards.

**CPU utilization** (8000 B / batch 10240, unpaced):

| Core            | Busy% | Note                            |
| --------------- | ----: | ------------------------------- |
| Master          |  3.7% | Orchestration only, mostly idle |
| TX queue poller |  ~92% | Poll-mode busy-spin             |
| RX queue poller |  ~92% | Poll-mode busy-spin             |

The GPU is a DMA target in this test (SM and memory-controller utilization ~0%).

#### Multi-queue core scaling

**DPDK loopback unpaced saturation at 256 B. Average of three 30 s samples.**

| Cell | TX pollers | RX pollers | App <span class="unit">Gbps</span> | Hardware discards / run |
| ---- | ---------- | ---------- | -----------------------------------: | ----------------------: |
| (1,1) | 1 | 1 | 48.77 | 511 M |
| (1,2) | 1 | 2 | **64.52** | 326 M |
| (2,1) | 2 | 1 | 47.96 | 518 M |
| (2,2) | 2 | 2 | 63.83 | 332 M |

The second RX poller raises unpaced delivered throughput, but every cell drops
packets. This is a saturation diagnostic, not a loss-free result.

### Socket / RoCE

**RoCE RC SEND loopback receive throughput vs message size. Average of three
30 s samples.**

| Message size | TX depth | App <span class="unit">Gbps</span> |
| -----------: | -------: | -----------------------------------: |
| 8 MiB | 128 | 96.79 ±0.09 |
| 1 MiB | 128 | 96.51 ±0.06 |
| 64 KiB | 128 | **97.65 ±0.05** |
| 8 KiB | 512 | 96.37 ±0.06 |
| 4 KiB | 512 | 56.80 ±0.76 |

### Socket / TCP

#### Single Rx core, one link

**TCP loopback receive throughput vs message size with one RX worker on one link.
Average of three 30 s samples.**

| Message size | App <span class="unit">Gbps</span> |
| ------------ | -----------------------------------: |
| 1 MiB | **52.52 ±3.63** |
| 8000 B | 49.11 ±0.31 |
| 1000 B | 13.16 ±0.05 |

#### Multiple Rx cores, one link

**TCP loopback receive throughput vs RX workers at 1 MiB. Average of three 30 s
samples.**

| Rx workers | App <span class="unit">Gbps</span> |
| ---------: | -----------------------------------: |
| 1 | 49.74 ±0.53 |
| 2 | 79.20 ±1.92 |
| 4 | **97.68 ±0.17** |

### Socket / UDP

**Loss-free UDP loopback receive throughput vs message size with one RX worker.
Average of three 30 s samples.**

| Message size | Pacer target | App <span class="unit">Gbps</span> |
| -----------: | -----------: | -----------------------------------: |
| 8000 B  | 22 Gbps | **20.64 ±0.19** |
| 1000 B  | 6 Gbps  | 4.96 ±0.09 |
| 65507 B | 15 Gbps | 15.00 ±0.00 |

**Loss-free UDP loopback receive throughput vs RX workers at an 8000 B message.
Average of three 30 s samples.**

| RX workers | Pacer target / worker | App <span class="unit">Gbps</span> |
| ---------: | ---------------------: | -----------------------------------: |
| 1 | 22 Gbps | 20.64 ±0.19 |
| 2 | 24 Gbps | 39.33 ±0.49 |
| 4 | 18 Gbps | **64.73 ±0.66** |

The source and receiver physical counters matched for every listed sample, and
the receiver reported no UDP kernel or IP-reassembly errors. Endpoint totals may
differ by up to one final 32-message retrieval batch at shutdown.
Pacer target is the requested software rate; scheduling delays are not recovered
by a catch-up burst, so measured application throughput can be lower.

## Reproduce

Run inside the project container (privileged, GPUs passed through, hugepages
mounted), as root. Build with `-DCMAKE_BUILD_TYPE=Release` and
`cmake --install build` so the bench loads the current `libdaqiri.so`.

### Uniform role controller

`scripts/run_crosshost_bench.sh` is the common controller for one measured cell.
It runs either `--topology crosshost` or `--topology loopback`; loopback uses
`local` for both roles. The testbed and role YAMLs remain external inputs, so the
controller contains no site-specific networking or CPU placement.

The same file also owns the complete loopback matrix through
`--suite spark-loopback-report`. `examples/run_spark_loopback_report.sh` is only
a compatibility forwarder; it contains no benchmark orchestration.

```bash
scripts/run_crosshost_bench.sh \
  --topology <crosshost|loopback> \
  --tx-host <ssh-destination|local> --rx-host <ssh-destination|local> \
  --workdir <repository-on-each-host> \
  --bench <benchmark-binary> \
  --tx-config <tx-role-config> --rx-config <rx-role-config> \
  --seconds 30 --repeats 3 \
  --protocol <raw|roce|tcp|udp> --tx-engine <engine> --rx-engine <engine> \
  --pace <unpaced|software|nic>
```

It starts RX first, uses the TX active window for the result, retains an RX drain
period, and writes an invocation manifest, one log per role and repetition, and a
normalized run-status CSV. Add `--tx-snapshot` and `--rx-snapshot` commands to
retain before/after application, kernel, NIC, and PHY counters with each
repetition; `--require-snapshots` makes a failed requested snapshot fail the run.

For UDP, pin the receive-I/O thread and application worker to separate cores in
the same performance cluster, and record both in the local profile. For raw
Ethernet, select TX and RX engines independently: on this platform DPDK is the
high-rate unpaced TX/RX choice, while ibverbs TX with NIC/QP pacing avoids
batch-sized software-pacer bursts at small payloads. This is a platform-profile
choice, not a general engine ranking.

Matching physical counters show wire transit, not application delivery. A
loss-free physical result also requires clean application, kernel, and NIC
counters. For UDP, a bounded final I/O-to-application retrieval batch may remain
at shutdown; record it separately from transport loss.

### Topology setup and reset

The controller never creates or changes host networking. Prepare the topology
before starting it, and restore the topology afterwards.

**Cross-host** uses normal host networking on both hosts; it does **not** use
network namespaces. For TCP, UDP, and RoCE, configure each selected link with
its local address, a route to the peer, and a static peer neighbor before running
the controller. `scripts/setup_spark_xhost_net.sh` can install the route and
neighbor after the local network profile has assigned the addresses:

```bash
# Run once on each host. Values belong in a local profile, not in this report.
SPARK_XHOST_IFACE=<local-data-interface> \
SPARK_TX_IP=<tx-benchmark-address> SPARK_RX_IP=<rx-benchmark-address> \
  sudo -E scripts/setup_spark_xhost_net.sh --role <tx|rx> \
  --peer-ip <peer-benchmark-address> --peer-mac <peer-data-mac>
```

Raw Ethernet uses the role configs and physical ports directly; it needs no IP
route or namespace. For every cross-host result, capture application, kernel,
NIC, and PHY counters on both hosts. To return an address-based cross-host setup
to its previous state, remove the benchmark-specific route and neighbor on each
host, then reapply the host's normal network profile:

```bash
sudo ip route del <peer-benchmark-address>/32 dev <local-data-interface>
sudo ip neigh del <peer-benchmark-address> dev <local-data-interface>
```

**Single-host loopback** uses two cabled ports. Raw phases require the default
host namespace. TCP, UDP, and RoCE phases use the `dq_wire_client` and
`dq_wire_server` namespaces so traffic cannot be shortcut locally. Run the full
suite as follows; it creates those namespaces for the socket/RoCE phases and
tears them down before it returns:

```bash
scripts/run_crosshost_bench.sh \
  --suite spark-loopback-report --topology loopback \
  --loopback-tx-netdev <cabled-transmit-interface> \
  --loopback-rx-netdev <cabled-receive-interface>
```

For an individual loopback socket or RoCE cell, create and later remove the
namespaces explicitly. Use the controller's `--tx-prefix` / `--rx-prefix` to run
the two roles in their respective namespaces. Do not leave the namespaces up
before a raw DPDK run.

```bash
CLIENT_IF=<cabled-transmit-interface> SERVER_IF=<cabled-receive-interface> \
  scripts/setup_spark_wire_loopback_netns.sh up

# Run the chosen --topology loopback controller cell here.

CLIENT_IF=<cabled-transmit-interface> SERVER_IF=<cabled-receive-interface> \
  scripts/setup_spark_wire_loopback_netns.sh down
```

`down` moves the ports and RDMA devices back to the host namespace and restores
shared RDMA namespace mode. The setup helper flushes the selected host-interface
addresses when it creates the namespaces, so `down` cannot recreate a prior host
network profile; reapply that profile after teardown if one was present.

### Spark loopback suite

The commands below are the underlying **single-host loopback** adapters. The
suite above is the normal report reproduction path. The `_xhost` configs
provide the paired roles for a manual cross-host smoke test:
(`examples/daqiri_bench_raw_tx_spark_xhost.yaml`,
`examples/daqiri_bench_raw_rx_spark_xhost.yaml`,
`examples/daqiri_bench_rdma_tx_rx_spark_xhost.yaml`), one role per host, with wire
rates read from physical counters at both ends — see
[Cross-host two-DGX-Spark loopback](raw_benchmarking.md#cross-host-two-dgx-spark-loopback).

```bash
export DAQIRI_BUILD_DIR=./build
export LD_LIBRARY_PATH=/opt/daqiri/lib:${LD_LIBRARY_PATH:-}
```

The base container does not ship the network tools the setup scripts and RoCE
baseline depend on. Install them first, or
`scripts/setup_spark_wire_loopback_netns.sh` fails with `ip: command not found`:

```bash
apt-get update
apt-get install -y iproute2 iputils-ping ethtool iperf3 rdma-core ibverbs-utils perftest
```

These provide `ip`/`nstat` (`iproute2`), `ethtool`, and `ib_send_bw` (`perftest`).

Each `run_spark_bench.sh <backend> <mode>` invocation takes a **mode** that sets
which cells run: `sweep` runs the full payload × batch × pairs matrix (the
per-transport message-size tables above), while `smoke` runs just the single
summary-table cell, one payload/batch/pairs operating point. `REPEATS=N` repeats
every cell N times for error bars.

**Raw Ethernet / GPUDirect (DPDK)** drives the two physical ports directly, so
the socket/RoCE network namespaces must **not** be up, since they capture the
ports and hide them from DPDK. Tear them down first (no-op if they were never
created). `<rx-iface>` below is the receive physical port:

```bash
./scripts/setup_spark_wire_loopback_netns.sh down       # ensure netns is torn down
export ETH_DST_ADDR=$(cat /sys/class/net/<rx-iface>/address)
./examples/run_spark_bench.sh dpdk sweep
```

The **multi-queue core-scaling matrix and payload sweep** run on the same
physical loopback (netns down). The four cells are generated from
`examples/daqiri_bench_raw_tx_rx_spark_mq.yaml` at run time, so just export the
rx-iface MAC as `ETH_DST_ADDR` (the script fills it into each generated config),
then run the sweep and render the plot:

```bash
export ETH_DST_ADDR=$(cat /sys/class/net/<rx-iface>/address)
./examples/run_spark_mq_bench.sh                       # 4 cells x payload sweep, 30 s each
# render the line plot (needs matplotlib in a venv -- not a runtime dependency):
./scripts/plot_mq_payload_sweep.py bench-results/<timestamp>-dpdk-mq/runs.csv
```

**Socket / RoCE and sockets** cross the cable through the network namespaces.
Bring the loopback up and confirm PHY counters move before running, and tear it
down when finished:

```bash
./scripts/setup_spark_wire_loopback_netns.sh up         # create the namespaces
./scripts/setup_spark_wire_loopback_netns.sh verify      # confirm wire traffic
./examples/run_spark_bench.sh rdma sweep
./examples/run_spark_bench.sh socket-tcp sweep
./examples/run_spark_bench.sh socket-udp sweep
./scripts/setup_spark_wire_loopback_netns.sh down        # tear down when done
```

That produces the **pair-scaling** matrices. The single-stream socket rows in the
summary are cross-host instead: one client and one server on separate hosts, no
namespaces, using the `_spark_xhost` configs after cross-host L3 connectivity is
configured on both hosts.

Sockets need **one config file per role**, unlike the RoCE cross-host config that
carries both. `daqiri_init` binds every interface listed in the file, so a
combined config fails on each host at the address it does not own.

```bash
# on the server host, started first and outliving the client
./build/examples/daqiri_bench_socket \
  examples/daqiri_bench_socket_udp_server_spark_xhost.yaml --mode server --seconds 42

# on the client host, paced
./build/examples/daqiri_bench_socket \
  examples/daqiri_bench_socket_udp_client_spark_xhost.yaml --mode client --seconds 30 --target-gbps 23
```

Swap in the `tcp_server` / `tcp_client` pair for the retained single-pair TCP rows and drop
`--target-gbps`, since TCP self-paces. For UDP that flag drives a token-bucket
pacer; find the loss-free rate by walking it up until the server's `recv_bytes`
stops tracking the client's `sent_bytes`. The configs ship at 8000 B (UDP) and
1 MiB (TCP); for the other published message sizes change `message_size` in
**both** files of the pair, since the two must agree.

To separate wire loss from host loss, snapshot `ethtool -S <iface>` on both hosts
immediately before and after the measured window and subtract, reading
`tx_packets_phy` on the client and `rx_packets_phy` on the server. These are
whole-run totals for the entire port, not per queue or per stream, so keep other
traffic off the port while measuring. When the two deltas match and the app still
lost datagrams, the drops are above the NIC.

Whichever setup you use, pin each pair's send and receive to **separate** cores in
the same CPU cluster. For UDP, queue `cpu_core` pins the socket receive-I/O
thread and `socket_bench_*.cpu_core` pins its application worker; both placements
must be recorded. TCP has no separate receive-I/O pin in this configuration.

Use the client's `active_seconds` to calculate both sent and received application
rates. The server deliberately outlives the client, so its whole-process
`seconds` includes an idle tail and is not a transfer-rate denominator.

**GPU workload (FFT / GEMM)** re-runs a backend with a representative GPU workload
in the receive path by exporting `WORKLOAD` (`none` | `fft` | `gemm` |
`gemm_fp16`), run once per received I/O unit on the real payload. Each call is a
fixed **1024³ GEMM** (override with `GEMM_DIM` / `--workload-gemm-dim`) or a batched
**length-1024 FFT** (override with `FFT_LEN` / `--workload-fft-len`). Both compute
sizes are held constant while the message size varies, so the FLOP count per call
is fixed. It composes with the same network-namespace setup as above (DPDK in the
default namespace). Use `smoke`, the single
summary-table cell that the fixed-n table reports, and run all three workloads
with error bars:

```bash
# RoCE (netns up); Raw is identical with `dpdk`, netns down, ETH_DST_ADDR exported.
for WL in none fft gemm; do
  WORKLOAD=$WL REPEATS=3 ./examples/run_spark_bench.sh rdma smoke
done
```

In the workload case the payload size is fixed per backend (8 KB for DPDK, 8 MB
message for RoCE), so a `sweep` only steps through batch size (DPDK) or
client/server pairs (sockets). The workload lands in the CSV `post_process` column
(with the GEMM dimension in `post_process_gemm_dim`); compare each `gbps` /
`gpu_sm_pct` against the `WORKLOAD=none` baseline from the same loop.

Each run writes `bench-results/<timestamp>-<backend>-<mode>/runs.csv`. The CSV
records the configured `batch`; for socket runs, `observed_max_rx_burst` reports
the largest burst returned to the application. Its CPU core columns identify the
actual sampled cores; socket runs with multiple pairs report pair 0 rather than
aggregate CPU utilization. See
[Socket and RDMA Benchmarking](socket_benchmarking.md) and
[Raw Ethernet Benchmarking](raw_benchmarking.md) for the namespace setup and
per-transport details.

**The ResNet pipeline** needs `-DDAQIRI_BUILD_APPLICATIONS=ON`, TensorRT (the
`BASE_IMAGE=torch` container), and the exported models plus packetized dataset.
The [tutorial](../tutorials/daqiri-resnet-inference.md) covers both. Given
passwordless `ssh` between the two hosts and a shared checkout,
`run_resnet_xhost.sh` starts the RX side, waits for `TrtRunner ready` (TensorRT
deserializes its plan *after* `daqiri_init`, so gating on the earlier reorder
line opens the run with an artificial drop burst), then launches TX:

```bash
# Three independent 30-second samples per model.
applications/resnet50_inference/tools/run_resnet_xhost.sh --seconds 30 --repeats 3
```

The wrapper runs whichever engine the RX config points at. The other four sizes
come from `--model resnet18|resnet34|resnet101|resnet152` on the RX binary, which
swaps the ONNX/engine paths and the feature dimension; the wrapper does not
forward that flag, so a model sweep drives the two sides directly.

The ingest ceiling is the same TX driving `daqiri_bench_raw_reorder_seq` on the RX
host instead of the app — identical wire format, sequence placement and batch
geometry, with only TensorRT removed. TX runs unthrottled in both arms, so the RX
NIC drop counter measures the offered-to-consumed ratio rather than loss in the
pipeline.

For a batch-size sweep, point the RX host at a config with a different
`images_per_batch` (and `packets_per_batch` scaled with it, 128 packets per
image) and re-run. Throughput comes from the RX process's own image counter;
wire rates come from `mlnx_perf` on both ports, since application run time over
packet counts is not accurate enough at these rates.

Rates are normalized on the TX window, because the RX process deliberately
outlives it by 15 s and counts that idle tail in its own `seconds=` field.
