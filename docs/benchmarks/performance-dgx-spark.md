---
hide:
  - navigation
---

# Performance: DGX Spark

Measured C++-loopback throughput for each stream/protocol on a single DGX Spark
(GB10), driven over a physical cabled loopback on one ConnectX-7. Numbers are
from a Release build via `examples/run_spark_bench.sh` (30 s per cell).

For the loopback setup these numbers depend on and the per-transport
benchmarking procedure, see [Socket and RDMA Benchmarking](socket_benchmarking.md)
(the `dq_wire_*` network-namespace wire loopback used by RoCE and sockets) and
[Raw Ethernet Benchmarking](raw_benchmarking.md) (the two-physical-port DPDK
loopback). The exact commands are collected under [Reproduce](#reproduce) below.

## System under test

| Component | Detail |
| --------- | ------ |
| Platform | DGX Spark (GB10), 20 cores, isolcpus `16-19` (the multi-queue sweep expands this; see [Multi-queue core scaling](#multi-queue-core-scaling)) |
| NIC | ConnectX-7, ports p0 ↔ p1 cross-cabled (single-host loopback), MTU 9000 |
| Build | Release (`-DCMAKE_BUILD_TYPE=Release`), `DAQIRI_ENGINE="dpdk ibverbs"` |
| Loopback | Raw/DPDK uses the two physical ports directly; socket/RoCE use the `dq_wire_*` network-namespace wire loopback |
| Core pinning | Each direction has a busy-spin queue poller and an app worker on separate isolated X925 cores (PR #149). Single-queue: DPDK pollers 17/18, workers 16/19. Multi-queue: TX pollers 16/19, RX pollers 18/9, each with its own worker core, master 8 (with `isolcpus=5-9,15-19`). |

## Results Summary — native-shape peak (C++ loopback)

Each transport at its best-case operation size. Raw/RoCE are single-stream;
socket TCP/UDP scale with the number of client/server pairs, so the four-pair
aggregate is shown.

| Stream / Protocol | Best case | Throughput | Drops | Notes |
| ----------------- | --------- | ---------: | ----- | ----- |
| Raw Ethernet / GPUDirect | 4 KB packet | **105.5 ±0.9 Gb/s** | 0 | 98.5 Gb/s single-queue at the 8 KB native shape |
| Socket / RoCE (SEND) | 8 MB message | **102.2 ±0.3 Gb/s** | 0 | Single QP, batch 1 |
| Socket / TCP | 8 KB × 4 pairs | **97.2 ±2.8 Gb/s** | ~0 | Flow-controlled (App TX = App RX) |
| Socket / UDP | 8 KB × 4 pairs | **29.8 ±0.2 Gb/s** | ~51% loss | Receiver goodput; unpaced sender |

Each transport is best read at its own native operation size (see the per-transport
tables below); a single cross-transport unit of work isn't meaningful here, since
RoCE at 8 KB is op-rate-bound well below its large-message peak and TCP has no
operation boundary.

## Raw Ethernet / GPUDirect (DPDK)

Physical port-to-port loopback, GPU-resident payloads. Native 8 KB packets run
at **98.5 Gb/s** drop-free across all batch sizes; the throughput peak is
**105.5 Gb/s** at a 4 KB payload. Packet handling is CPU-bound (see the CPU
utilization table below). Throughput is flat across batch size and stable
run-to-run (3 reps per cell, ≤1% spread).

Achieved Gb/s measured at App RX (equal to App TX, since every cell is
drop-free), unpaced, mean of 3 reps; run-to-run spread ≤0.9 Gb/s (<1%):

<table class="perf-matrix" markdown="0">
  <thead>
    <tr>
      <th rowspan="2">Payload</th>
      <th colspan="4">Batch size (packets per burst)</th>
    </tr>
    <tr>
      <th>256</th><th>1024</th><th>4096</th><th>10240</th>
    </tr>
  </thead>
  <tbody>
    <tr><th>8000 B</th><td>98.5</td><td>98.0</td><td>98.0</td><td>98.5</td></tr>
    <tr><th>4096 B</th><td>105.2</td><td>105.3</td><td>105.5</td><td>105.1</td></tr>
    <tr><th>1024 B</th><td>92.1</td><td>91.8</td><td>91.7</td><td>91.7</td></tr>
    <tr><th>256 B</th><td>50.0</td><td>49.8</td><td>49.8</td><td>49.8</td></tr>
    <tr><th>64 B</th><td>20.4</td><td>20.5</td><td>20.5</td><td>20.4</td></tr>
  </tbody>
</table>

At ≥4 KB the link saturates (~98–105 Gb/s) regardless of batch. Below that the
path is packet-rate-bound: 1 KB ~92 Gb/s (10.5 M pps), 256 B ~50 Gb/s (19.5 M pps),
64 B ~20 Gb/s (20 M pps) — a ~20 M pps single-queue ceiling (the multi-queue
section lifts it). Gb/s here is the L2 frame rate including the 64 B header, so
pps ≈ Gb/s ÷ ((payload + 64) × 8). These small-payload cells are flat across batch
size and stable run-to-run. Because every cell is drop-free, the achieved rate is
also the no-drop rate: pacing the sender below it hits the target with zero drops.

**CPU utilization** (headline cell, 8000 B / batch 10240, unpaced):

| Core                     | Busy% | Note                                  |
| ------------------------ | ----: | ------------------------------------- |
| Master (CPU 8)           |  3.7% | Orchestration only; mostly idle       |
| TX queue poller (CPU 17) |  ~92% | Poll-mode spin; rate-independent      |
| RX queue poller (CPU 18) |  ~92% | Poll-mode spin; rate-independent      |

The benchmark app workers run on their own cores (TX 16, RX 19) alongside these
pollers; this run sampled only the poller cores.
The pollers stay near 92% across every drop-curve step from 1 Gb/s to line rate —
DPDK's poll-mode driver spins regardless of offered load. The GPU stays idle (SM
and memory-controller utilization both ~0%): it is a DMA target for the payload,
not a compute engine.

### Multi-queue core scaling

Each packet-handling core spins in poll-mode. At the native 8 KB shape a single
TX core caps throughput near ~98 Gb/s while a single RX core already drains the
line, so the second TX core is the lever: it scales `(1,1)` to `(2,1)` from
97.5 to 108.8 Gb/s, while a second RX core adds little. The matrix sweeps
(TX cores, RX cores) over `(1,1)`, `(1,2)`, `(2,1)`, `(2,2)`.

Each queue is served by a poll-mode driver core plus a separate bench-worker
core, paired within one CPU cluster where possible so the poller→worker handoff
stays local. The four-queue matrix uses the expanded isolated-core budget
(`isolcpus=5-9,15-19`): TX pollers on 16/19, RX pollers on 18/9, each with its
own worker core, and the master on core 8. Configs are derived from the single
base `daqiri_bench_raw_tx_rx_spark_mq.yaml` (the balanced 2,2 superset) by
`scripts/gen_spark_mq_config.py`; generated by `examples/run_spark_mq_bench.sh`,
30 s per cell, 0 drops.

| Cell | TX pollers | RX pollers | Achieved <span style="text-transform: none">Gb/s</span> |
| ---- | ---------- | ---------- | ------------: |
| (1,1) | 16    | 18   | 97.5  |
| (1,2) | 16    | 18,9 | 99.0  |
| (2,1) | 16,19 | 18   | **108.8** |
| (2,2) | 16,19 | 18,9 | **108.8** |

Which core is the bottleneck flips with payload size. Sweeping each cell from
64 B to 8 KB:

![DPDK multi-queue throughput vs UDP payload size on DGX Spark, one line per (TX,RX) core count](../images/spark-mq-payload-sweep.svg)

At small payloads the path is packet-rate-bound, so **RX cores** are the lever:
at 64 B a second RX core lifts throughput from 20.3 to 26.8 Gb/s (~20 M → ~26 M
pps) while a second TX core does nothing. At large payloads it inverts to the
byte/line-rate-bound regime where **TX cores** are the lever: at 8 KB the second
TX core takes 97.5 to 108.8 Gb/s (the native-shape result above) while a second
RX core adds nothing. The curves cross around 1–4 KB, where the link saturates
and all four cells converge near ~104–109 Gb/s. Every cell is drop-free.
Generated by `examples/run_spark_mq_bench.sh` (30 s per point) and
`scripts/plot_mq_payload_sweep.py`.

## Socket / RoCE

RoCE SEND over the netns wire loopback, single queue-pair, batch 1. Throughput
is App RX goodput, equal to App TX with 0 drops. Large messages up to 64 KB
saturate the wire; the smallest messages are bound by per-operation software
overhead.

**Message-size sweep (single QP, batch 1, 0 drops).** Mean of 3 reps; run-to-run
spread ≤0.8 Gb/s (<2%) in every cell.

| Message size | <span style="text-transform: none">Gb/s</span> |
| ------------ | ---: |
| 8 MB  | **102.2** |
| 1 MB  | 101.3 |
| 64 KB | 101.6 |
| 8 KB  | 60.7 |
| 4 KB  | 38.0 |

Messages ≥64 KB hold ~101–102 Gb/s at the wire ceiling. Below that the path is
operation-rate-bound (per-operation software overhead, not a stall) rather than
wire-bound, and every cell is drop-free. At 8 KB (60.7 Gb/s) and 4 KB (38.0 Gb/s)
a dedicated bench-worker core, separate from the RoCE engine thread, sustains the
operation rate, as it does for small DPDK packets. A per-message flow-control
window keeps enough operations in flight to amortize that overhead: it pre-posts
`rx_depth` receives before sending and caps the transmit side at `tx_depth`, each
sized to the message so the in-flight window stays full.

**CPU utilization** (headline cell, 8 MB message, batch 1, unpaced):

| Core                | Busy% | Note                                            |
| ------------------- | ----: | ----------------------------------------------- |
| Master (CPU 8)      |  5.9% | Orchestration only                              |
| Client TX (CPU 17)  | 74.9% | Post-and-poll spin; rate-independent            |
| Server RX (CPU 18)  |  0.0% | HCA writes straight to memory; CPU uninvolved   |

The idle RX core is the expected RoCE RC signature — the HCA places incoming
data directly into registered memory with no CPU involvement. The GPU stays
idle here too (SM and memory-controller ~0%; DMA target, not a compute engine).

## Socket / TCP

Four one-way TCP client/server pairs over the netns wire loopback, each pair
pinned to one isolated core (16–19). TCP self-paces via flow control, so App TX
equals App RX with effectively no app-level loss. `message_size` is the per-send
byte count of a stream (no datagram boundary, no fragmentation).

Throughput in Gb/s (App TX = App RX), mean ± std over 3 reps:

<table class="perf-matrix" markdown="0">
  <thead>
    <tr>
      <th rowspan="2">Message size</th>
      <th colspan="3">Number of client/server pairs</th>
    </tr>
    <tr>
      <th>1</th><th>2</th><th>4</th>
    </tr>
  </thead>
  <tbody>
    <tr><th>1000 B</th><td>13.5<small>±0.2</small></td><td>27.3<small>±0.2</small></td><td>54.8<small>±0.2</small></td></tr>
    <tr><th>8000 B</th><td>30.8<small>±5.8</small></td><td>68.0<small>±9.7</small></td><td>97.2<small>±2.8</small></td></tr>
    <tr><th>1 MiB</th><td>31.6<small>±0.1</small></td><td>58.7<small>±0.3</small></td><td>93.7<small>±1.3</small></td></tr>
  </tbody>
</table>

Throughput scales with the pair count; retransmits stay negligible over the run.

## Socket / UDP

Four one-way UDP client/server pairs, same one-core-per-pair pinning. UDP has no
flow control, so each sender runs flat-out and the receiver drops whatever it
cannot drain — the loss column is an inherent property of unpaced UDP, not a
fault. App RX is the delivered goodput; App-level loss is `(App TX − App RX) /
App TX`.

Each cell shows **receiver goodput in Gb/s** (mean ± std over 3 reps) with the
**app-level loss %** dimmed beneath it:

<table class="perf-matrix" markdown="0">
  <thead>
    <tr>
      <th rowspan="2">Message size</th>
      <th colspan="3">Number of client/server pairs</th>
    </tr>
    <tr>
      <th>1</th><th>2</th><th>4</th>
    </tr>
  </thead>
  <tbody>
    <tr><th>1000 B</th><td>3.6 ±0.0<small>11% loss</small></td><td>7.7 ±0.0<small>16% loss</small></td><td>13.5 ±0.1<small>6% loss</small></td></tr>
    <tr><th>8000 B</th><td>9.6 ±0.5<small>56% loss</small></td><td>20.9 ±1.3<small>57% loss</small></td><td>29.8 ±0.2<small>51% loss</small></td></tr>
  </tbody>
</table>

The sweep stops at 8000 B (single Ethernet frame). Larger UDP datagrams
fragment above the ~8972 B MTU payload; reassembly is all-or-nothing out of a
shared per-namespace pool, so under multi-pair unpaced load delivery collapses
(≈100% loss at 65507 B / 4 pairs). The wire itself is loss-free here; the loss
is host-side socket-buffer and reassembly pressure.

## GPU workload (FFT / GEMM in the receive path)

Issue #15 asks for each backend at bare loopback, loopback **+ FFT**, and
loopback **+ GEMM** — i.e. how much line rate the receiver holds while a GPU also
crunches the incoming data. The benchmarks accept `--workload none|fft|gemm`, and
`run_spark_bench.sh` exposes it as the `WORKLOAD` env var (recorded in the CSV
`post_process` column).

**What the two workloads compute** (the reusable component
`examples/bench_workload.{h,cu}`):

- **FFT** — a batched 1-D **complex-to-complex forward FFT** via cuFFT
  (`cufftExecC2C`). The burst's bytes are treated as an array of single-precision
  complex samples and transformed as many independent length-1024 FFTs, batched so
  the transforms cover the whole burst (e.g. ~10000 × 1024-point FFTs for an 82 MB
  burst). This models a streaming signal-processing receiver — channelization or
  spectral analysis that FFTs every frame as it arrives.
- **GEMM** — a single-precision dense **matrix multiply** `C = A·B` via cuBLAS
  (`cublasSgemm`) on square *n×n* matrices, with *n* chosen so the three matrices
  together match the burst's data volume (e.g. 4520×4520 for an 82 MB burst). This
  models a receiver that feeds incoming data into a dense linear-algebra stage —
  beamforming, correlation, or a neural-network layer.

Each is run **once per received burst** on a dedicated CUDA stream, with up to two
kernels kept in flight (sync depth 2) to overlap GPU work with ingest while
bounding the queue so it cannot run unboundedly ahead. The problem is sized to the
**whole burst's data volume** (`batch × payload`) so the GPU load scales with the
receive data rate; a smaller batch or payload moves the operating point back toward
line rate. GPU SM% is from `nvidia-smi dmon` across the run. Single 30 s run per
cell.

!!! note "Representative compute, not a data transform"
    The workload runs on its own GPU scratch buffers, **not** on the received
    packet bytes. This keeps it a true drop-in across every stream_type / engine
    (raw, HDS, RoCE) — RoCE in particular exposes no public payload device
    pointer — and means it measures the **GPU-load headroom of the receive path**
    (does sustained GPU compute on the receiver steal enough SM / PCIe / host
    cycles to dent line rate?), not the cost of transforming the actual data. The
    FLOP profile and memory footprint match a real per-burst transform; only the
    input bytes differ.

Raw / GPUDirect, 8 KB native shape (batch 10240), GPU-resident payloads:

| Workload | Throughput | Drops | GPU SM% | Notes |
| -------- | ---------: | ----- | ------: | ----- |
| none (baseline) | 98.5 Gb/s | 0 | ~0 | Bare loopback |
| FFT | 94.2 Gb/s | 0 | 8.9% | Light compute; line rate essentially held (−4%) |
| GEMM | 62.3 Gb/s | 0 | 82.4% | GPU-bound; throughput backpressures, still **drop-free** |

The headline is the **drops column**: even when the per-burst SGEMM saturates the
GB10 to ~82% SM and pulls effective throughput down to 62 Gb/s, the receive path
paces against the GPU and **drops zero packets** — backpressure, not loss. A
lighter workload (FFT, ~9% SM) holds line rate within 4%. Because the workload
scales with `batch × payload`, a smaller batch or payload moves the operating
point back toward line rate; this cell deliberately picks a heavy per-burst GEMM
to show the GPU-bound end of the curve.

Socket / RoCE, 8 MB native message (single QP, batch 1), GPU-resident payloads:

| Workload | Throughput | Drops | GPU SM% | Notes |
| -------- | ---------: | ----- | ------: | ----- |
| none (baseline) | 101.4 Gb/s | 0 | ~0 | Bare loopback |
| FFT | 98.7 Gb/s | 0 | 26.9% | Line rate held within ~3% |
| GEMM | 79.3 Gb/s | 0 | 76.6% | GPU-bound; throughput backpressures, still **drop-free** |

The 8 MB message gives a larger per-burst working set than the Raw 8 KB shape, so
both workloads register more GPU utilization here, but the shape is identical:
light compute (FFT) holds line rate, a heavy per-message SGEMM drives the GB10 to
~77% SM and pulls throughput to 79 Gb/s with zero CQ errors / drops.

## Reproduce

Run inside the project container (privileged, GPUs passed through, hugepages
mounted), as root. Build with `-DCMAKE_BUILD_TYPE=Release` and
`cmake --install build` so the bench loads the current `libdaqiri.so`.

```bash
export DAQIRI_BUILD_DIR=./build
export LD_LIBRARY_PATH=/opt/daqiri/lib:${LD_LIBRARY_PATH:-}
```

The base container does not ship the network tools the setup scripts and RoCE
baseline depend on; install them first, or
`scripts/setup_spark_wire_loopback_netns.sh` fails with `ip: command not found`:

```bash
apt-get update
apt-get install -y iproute2 iputils-ping ethtool iperf3 rdma-core ibverbs-utils perftest
```

These provide `ip`/`nstat` (`iproute2`), `ethtool`, and `ib_send_bw` (`perftest`).

**Raw Ethernet / GPUDirect (DPDK)** drives the two physical ports directly, so
the `dq_wire_*` namespaces must **not** be up — they capture the ports and
hide them from DPDK. Tear them down first (no-op if they were never created).
`<rx-iface>` below is the RX physical port (p1 in the p0→p1 loopback):

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

**Socket / RoCE and sockets** cross the cable through the `dq_wire_client` →
`dq_wire_server` namespaces. Bring the loopback up and confirm PHY counters move
before running; tear it down when finished:

```bash
./scripts/setup_spark_wire_loopback_netns.sh up         # create the namespaces
./scripts/setup_spark_wire_loopback_netns.sh verify      # confirm wire traffic
./examples/run_spark_bench.sh rdma sweep
./examples/run_spark_bench.sh socket-tcp sweep
./examples/run_spark_bench.sh socket-udp sweep
./scripts/setup_spark_wire_loopback_netns.sh down        # tear down when done
```

**GPU workload (FFT / GEMM)** re-runs a backend with a representative GPU
workload in the receive path by exporting `WORKLOAD`. It composes with the same
modes and netns setup as above (dpdk in the default namespace, rdma in the
`dq_wire_*` namespaces):

```bash
# Raw / GPUDirect (netns down, ETH_DST_ADDR exported as above)
WORKLOAD=fft  ./examples/run_spark_bench.sh dpdk smoke
WORKLOAD=gemm ./examples/run_spark_bench.sh dpdk smoke
# Socket / RoCE (netns up)
WORKLOAD=fft  ./examples/run_spark_bench.sh rdma smoke
WORKLOAD=gemm ./examples/run_spark_bench.sh rdma smoke
```

The chosen workload lands in the CSV `post_process` column; compare `gbps` /
`gpu_sm_pct` against the matching `WORKLOAD=none` baseline.

Each run writes `bench-results/<timestamp>-<backend>-<mode>/runs.csv`. See
[Socket and RDMA Benchmarking](socket_benchmarking.md) and
[Raw Ethernet Benchmarking](raw_benchmarking.md) for the namespace setup and
per-transport details.

---
**Previous:** [Raw Ethernet Benchmarking](raw_benchmarking.md)
