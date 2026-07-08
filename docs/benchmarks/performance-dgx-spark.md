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

## Results Summary (C++ loopback)

Each transport at its best-case **native operation size**. Raw/RoCE are
single-stream; socket TCP/UDP scale with the number of client/server pairs, so the
four-pair aggregate is shown.

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

## GPU workloads in the receive path

A common question for a GPU-attached receiver is how much line rate it holds while
the GPU also crunches the incoming data. The benchmarks accept
`--workload none|fft|gemm|gemm_fp16`, exposed by `run_spark_bench.sh` as the
`WORKLOAD` env var (recorded in the CSV `post_process` column); more workload kinds
can be added to the same reusable component over time. The workload runs on the
**actual received packet data** — every backend first assembles the burst's
payloads into one contiguous GPU buffer (a sequence-number **reorder** on the
out-of-order transports, an arrival-order **gather** on the in-order ones) and the
compute consumes that buffer.

**What the two workloads compute** (the reusable component
`examples/bench_workload.{h,cu}`):

- **FFT** — a batched 1-D **complex-to-complex forward FFT** via cuFFT
  (`cufftExecC2C`). The reordered buffer is treated as an array of single-precision
  complex samples and transformed as many independent length-1024 FFTs, batched so
  the transforms cover the whole reorder window. This models a streaming
  signal-processing receiver — channelization or spectral analysis that FFTs every
  frame as it arrives.
- **GEMM** — a dense **matrix multiply** `C = A·B` via cuBLAS on square *n×n*
  matrices, with the reordered buffer supplying the *A* operand and *n* chosen so
  the matrices match the working-set size. Two precisions: **`gemm`** is FP32
  (`cublasSgemm`); **`gemm_fp16`** is the *same-size* mixed-precision matmul (FP16
  inputs, FP32 accumulate) on the **tensor cores** (`cublasGemmEx`,
  `CUBLAS_GEMM_DEFAULT_TENSOR_OP`) — the core op of GPU inference. Running both at
  one matrix size isolates the precision / tensor-core effect. This models a
  receiver feeding incoming data into a dense linear-algebra or neural-network
  stage (beamforming, correlation, an inference layer).

**The reorder/gather step is per-backend** (`examples/bench_pipeline.{h,cu}`),
chosen to be representative for each transport:

| Backend | Payload source | Pre-workload step |
| ------- | -------------- | ----------------- |
| Raw / GPUDirect (DPDK) | GPU-accessible RX buffers | **seq reorder** kernel → contiguous device buffer (out-of-order capable) |
| RoCE (RC) | GPU-accessible recv MR | **gather** (in-order); one large message is a zero-copy pass-through |
| UDP sockets | host RX buffers | **host→device stage**, then **seq reorder** |
| TCP sockets | host RX buffers | **host→device stage**, then **gather** (in-order stream) |

Each compute runs **once per reorder window** on a dedicated CUDA stream (shared
with the reorder/gather kernel so the two serialize without an extra sync), with up
to two kernels in flight (sync depth 2) to overlap GPU work with ingest. The
reorder window is sized so the contiguous buffer is ~8 MB on every backend, giving
a comparable GPU working set across transports. GPU SM% is from `nvidia-smi dmon`
across the run. Throughput is mean ± std over 3 reps, 30 s each.

!!! note "Where the data lives, per backend"
    On the integrated GB10 the GPU shares memory with the CPU, so the raw and RoCE
    receive buffers (`host_pinned`) are GPU-accessible with **no copy** — the
    reorder/gather kernel reads them in place. Sockets are different: the kernel
    hands received bytes to the application in pageable host memory, so the socket
    path must **stage each payload host→device** before the GPU can touch it — a
    copy on the measured path that the raw/RoCE paths avoid. Lost packets (raw/UDP)
    leave their reorder slots zero-filled; the FLOP/copy volume is unchanged.

Raw / GPUDirect, 8 KB native shape (batch 10240), GPU-resident payloads, seq reorder:

| Workload | Throughput | Drops | GPU SM% | Notes |
| -------- | ---------: | ----- | ------: | ----- |
| none (baseline) | 98.4 ±0.2 Gb/s | 0 | ~0 | Bare loopback (no GPU compute) |
| FFT | 95.5 ±1.7 Gb/s | 0 | 16.4% | Light compute; line rate held within ~3% |
| GEMM (FP32) | 94.9 ±0.0 Gb/s | 0 | 55.9% | FP32 cores; GPU-bound end, still **drop-free** |
| GEMM (FP16 tensor) | 97.7 ±0.2 Gb/s | 0 | 16.8% | Same matrix, tensor cores; near line rate at a third of the SM |

RoCE, 8 MB native message (single QP, batch 1), gather pass-through:

| Workload | Throughput | Drops | GPU SM% | Notes |
| -------- | ---------: | ----- | ------: | ----- |
| none (baseline) | 101.7 ±0.2 Gb/s | 0 | ~0 | Pass-through, no compute |
| FFT | 93.6 ±1.6 Gb/s | 0 | 36.0% | Light compute; ~8% off line rate |
| GEMM (FP32) | 35.0 ±0.3 Gb/s | 0 | 79.0% | GPU-bound at this batch (one GEMM/message); see sweep |
| GEMM (FP16 tensor) | 85.8 ±0.2 Gb/s | 0 | 53.6% | Same matrix, tensor cores |

Both paths drive the GPU the same way (each holds a batch of received data and drains
the GPU stream **once per batch** — the raw path a burst of ~10 reorder windows, the
RoCE path a small batch of messages whose recv buffers stay live during the pass-through
compute), so compute overlaps with receive on both and every cell is drop-free. The
fixed-batch rows above are the 8 MB native operating point, batch-matched to the raw
path's ~8.19 MB reorder window (1024 packets × 8000 B).

### Fixed-size GEMM comparison

The tables above size the GEMM from the working set, so sweeping the batch also changes
the matrix dimension (FLOPs ∝ n³) and conflates problem size with transport. To compare
transports cleanly we **pin the GEMM dimension** with `--workload-gemm-n 1024`
(env `GEMM_N` in `run_spark_bench.sh`): every call is an identical **1024×1024×1024
matmul = 2.15 GFLOP**, and both transports consume **exactly 4 MB per GEMM** (one GEMM
per 4 MB window), so throughput *and* GEMMs/s are directly comparable. Fixed n=1024,
3 reps, 30 s each; RoCE at its 8 MB message (2 GEMMs/message), raw at a 4 MB reorder
window (4096 B × 1024 packets).

| Transport | Workload | Throughput | GEMMs/s | GPU SM% |
| --------- | -------- | ---------: | ------: | ------: |
| RoCE (RC)  | GEMM FP32          | 48.1 Gb/s  | 1434 | 78% |
| Raw (DPDK) | GEMM FP32          | 103.8 Gb/s | 3095 | 40% |
| RoCE (RC)  | GEMM FP16 (tensor) | 88.3 Gb/s  | 2631 | 56% |
| Raw (DPDK) | GEMM FP16 (tensor) | 105.6 Gb/s | 3148 | 15% |

Three things fall out, and they revise the "pipelining depth" reading of the earlier
batch sweep:

- **The RoCE↔raw gap is real, and it is not the GPU.** At the *identical* fixed workload
  raw sustains **2.2× the GEMM rate at FP32** (and ~1.2× at FP16) while using *less* than
  half the SM. Raw has GPU headroom (15–40% SM) and is wire-limited (~104–106 Gb/s for
  both precisions); RoCE is the side leaving the GPU idle-but-stalled.
- **GEMMs/s is the transport-fair metric, not Gb/s** — because an engine that consumes more
  bytes per GEMM carries more throughput at the same compute rate. (Matching the window to
  4 MB on both sides is what makes the two columns comparable.) Achieved rates are
  ~3.1 TFLOP/s (FP32) / ~5.7 TFLOP/s (FP16), only ~10% of peak, so the small matrix is
  **launch/sync-latency-bound**, not FLOP-bound.
- **GPU SM% is a duty-cycle metric, anti-correlated with throughput here.** RoCE is
  "stall-busy" (78% SM at 48 Gb/s); raw is efficient (40% SM at 104 Gb/s). High SM% means
  the GPU work is *stretched thin* against a slower receive path, not that more useful work
  is done — so SM% should not be read as compute efficiency.

The bottleneck is the **single-threaded RoCE receive+compute loop**: one thread runs
`poll completion → gather → issue GEMM → stream-sync → free → repeat`, and the periodic
`cudaStreamSynchronize` (required before a received buffer can be safely reused) blocks
that thread — so while the GPU drains, no receives are posted and the message rate falls.
The heavier the GEMM, the longer each stall, which is why FP32 (48 Gb/s) suffers more than
FP16 (88 Gb/s) while raw stays wire-limited for both. Raw proves one thread *can* feed the
GPU at ~3100 GEMM/s, so the remedy is to decouple receive from compute (a dedicated
GPU-worker thread) or thin the stalls (`--workload-sync-interval`); a sync-interval sweep
quantifying the latter is in progress.

### Workload batch-size sweep

!!! warning "Superseded — conflates problem size with pipelining"
    This sweep varies `--workload-batch-bytes`, which also sets the GEMM dimension
    (`n = √(batch/4)`), so each column changes the FLOP count *and* the pipelining depth
    at once. The [fixed-size GEMM comparison](#fixed-size-gemm-comparison) above pins n and
    shows the RoCE↔raw gap is a receive-path / threading effect, not pipelining depth. The
    curve is kept here for transparency.

The workload's compute working set is **decoupled from the I/O unit** via
`--workload-batch-bytes` (env `WORKLOAD_BATCH` in `run_spark_bench.sh`): it sets the
bytes fed to one compute call — the GEMM dimension is `n = √(batch/4)` — independent of
the 8 MB RoCE message or 8 KB raw frame. RoCE sub-divides each message into batch-sized
slices (one compute per slice); raw sizes its reorder window to the batch. Sweep run on
`gemm_fp16` (cuBLAS takes the dimension per call); the tables above are the fixed-batch
(8 MB) operating points.

The two paths respond very differently, which is the whole point. **Raw is flat at line
rate (~98 Gb/s) across every batch** — a raw burst always packs ~10 reorder windows, so
the GPU stays fed regardless of the per-window size; the workload is never the bottleneck.
**RoCE is strongly batch-sensitive** and **non-monotonic with a sweet spot at 2–4 MB**:
one 8 MB message yields a single GEMM with nothing to overlap (underpipelined, 85 Gb/s),
sub-dividing it to 2–4 MB packs 2–4 GEMMs per message and recovers **~92 Gb/s — on par
with raw**, and tiny 512 KB slices collapse to 37 Gb/s on per-call launch/sync overhead
(16 GEMMs/message). The apparent RoCE recovery here is partly an artifact of the growing
matrix (bigger batch → bigger n → a more efficient, more GPU-bound GEMM on both paths); the
[fixed-size GEMM comparison](#fixed-size-gemm-comparison) above pins n to separate that from
the transport effect and shows the gap is the RoCE receive path, not pipelining depth.

| Batch | RoCE Gb/s | RoCE GPU SM% | Raw Gb/s | Raw GPU SM% |
| ----: | --------: | -----------: | -------: | ----------: |
| 512 KB | 37.0 | 76.9% | 98.0 | 24.8% |
| 1 MB | 76.9 | 75.9% | 98.3 | 16.8% |
| 2 MB | 90.8 | 54.0% | 98.2 | 13.6% |
| 4 MB | 92.5 | 40.4% | 97.8 | 15.1% |
| 8 MB | 85.3 | 52.2% | 96.7 | 16.0% |

(`gemm_fp16`, single rep per point. Raw cells use the nearest whole-packet window to the
batch size; RoCE caps the batch at the 8 MB message.)

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

**Workload batch-size sweep** decouples the compute working set from the I/O unit
by also exporting `WORKLOAD_BATCH` (bytes); it lands in the CSV `post_process_batch`
column:

```bash
for B in 262144 524288 1048576 2097152 4194304 8388608; do
  WORKLOAD=gemm_fp16 WORKLOAD_BATCH=$B ./examples/run_spark_bench.sh rdma smoke   # netns up
done
# raw: netns down, ETH_DST_ADDR exported; same loop with `dpdk`
```

**Fixed-size GEMM comparison** pins the matrix dimension with `GEMM_N`
(`--workload-gemm-n`) so the FLOP count is constant across transports; both sides run one
GEMM per 4 MB window. Lands in the CSV `post_process_gemm_n` column:

```bash
# RoCE (netns up): 8 MB message, 4 MB chunk -> 2 fixed 1024^3 GEMMs/message
for WL in gemm gemm_fp16; do
  WORKLOAD=$WL GEMM_N=1024 WORKLOAD_BATCH=4194304 REPEATS=3 \
    PAYLOADS_OVERRIDE="8388608" ./examples/run_spark_bench.sh rdma sweep
done
# Raw (netns down, ETH_DST_ADDR exported): 4096 B x 1024 packets = 4 MB reorder window
for WL in gemm gemm_fp16; do
  WORKLOAD=$WL GEMM_N=1024 WORKLOAD_BATCH=4194304 REPEATS=3 \
    PAYLOADS_OVERRIDE="4096" BATCHES_OVERRIDE="1024" ./examples/run_spark_bench.sh dpdk sweep
done
```

Each run writes `bench-results/<timestamp>-<backend>-<mode>/runs.csv`. See
[Socket and RDMA Benchmarking](socket_benchmarking.md) and
[Raw Ethernet Benchmarking](raw_benchmarking.md) for the namespace setup and
per-transport details.

---
**Previous:** [Raw Ethernet Benchmarking](raw_benchmarking.md)
