# Performance: DGX Spark

Measured C++-loopback throughput for each stream/protocol on a single DGX Spark
(GB10), driven over a physical cabled loopback on one ConnectX-7. Numbers are
from a Release build via `examples/run_spark_bench.sh` (30 s per cell).

## System under test

| Component | Detail |
| --------- | ------ |
| Platform | DGX Spark (GB10), 20 cores, isolcpus `16-19` |
| NIC | ConnectX-7, ports p0 ↔ p1 cross-cabled (single-host loopback), MTU 9000 |
| Build | Release (`-DCMAKE_BUILD_TYPE=Release`), `DAQIRI_MGR="dpdk socket rdma"` |
| Loopback | Raw/DPDK uses the two physical ports directly; socket/RoCE use the `dq_wire_*` network-namespace wire loopback |

## Headline — native-shape peak (C++ loopback)

Each transport at its best-case operation size. Raw/RoCE are single-stream;
socket TCP/UDP scale with the number of client/server pairs, so the four-pair
aggregate is shown.

| Stream / Protocol | Best case | Throughput | Drops |
| ----------------- | --------- | ---------: | ----- |
| Raw Ethernet / GPUDirect | 4 KB packet | **106.4 Gb/s** (98.5 at 8 KB native) | 0 |
| Socket / RoCE (SEND) | 8 MB message | **94.8 Gb/s** | 0 |
| Socket / TCP | 8 KB × 4 pairs | **87.6 Gb/s** | ~0 (flow-controlled) |
| Socket / UDP | 8 KB × 4 pairs | **34.0 Gb/s** goodput | unpaced, ~57% app-loss |

Each transport is best read at its own native operation size (see the per-transport
tables below); a single cross-transport unit of work isn't meaningful here, since
RoCE collapses at 8 KB and TCP has no operation boundary.

## Raw Ethernet / GPUDirect (DPDK)

Physical port-to-port loopback, GPU-resident payloads. Native 8 KB packets run
at **98.5 Gb/s** drop-free across all batch sizes; the throughput peak is
**106.4 Gb/s** at a 4 KB payload. The TX and RX poll-mode cores run ~93% busy
(CPU-bound packet handling), and the GPU sits near-idle (it is a DMA target, not
a compute engine).

Achieved Gb/s, unpaced, 0 drops in every cell:

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
    <tr><th>8000 B</th><td>98.2</td><td>98.6</td><td>98.6</td><td>98.5</td></tr>
    <tr><th>4096 B</th><td>106.3</td><td>104.8</td><td>106.4</td><td>106.2</td></tr>
    <tr><th>1024 B</th><td>80.4</td><td>75.5</td><td>82.8</td><td>90.9</td></tr>
    <tr><th>256 B</th><td>20.3</td><td>19.5</td><td>20.7</td><td>21.3</td></tr>
    <tr><th>64 B</th><td>8.3</td><td>10.5</td><td>11.3</td><td>10.0</td></tr>
  </tbody>
</table>

At ≥4 KB the link saturates (~98–106 Gb/s) regardless of batch; the 1 KB row is
the transition; ≤256 B is packet-rate-bound (~10 M pps ceiling) and noisy
run-to-run. Every cell is drop-free, so the achieved rate is also the no-drop
rate — pacing the sender below it simply hits the target with zero drops, so a
paced payload × rate sweep adds nothing here (unlike UDP, which drops as offered
rate climbs).

## Socket / RoCE

RoCE SEND over the netns wire loopback, single queue-pair, batch 1. Large
messages saturate the path; below ~1 MB throughput falls off a small-message
cliff (sweep below).

**Message-size sweep (single QP, batch 1, 0 drops)**

| Message size | Gb/s |
| ------------ | ---: |
| 8 MB  | **94.8** |
| 1 MB  | 94.4 |
| 64 KB | 0.109 |
| 8 KB  | 0.004 |
| 4 KB  | 0.000 |

Large messages (≥1 MB) hold the ~95 Gb/s wire ceiling; below ~1 MB the
RDMA-path small-message cliff collapses throughput.

## Socket / TCP

Four one-way TCP client/server pairs over the netns wire loopback, each pair
pinned to one isolated core (16–19). TCP self-paces via flow control, so App TX
equals App RX with effectively no app-level loss. `message_size` is the per-send
byte count of a stream (no datagram boundary, no fragmentation).

Throughput in Gb/s (App TX = App RX):

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
    <tr><th>1000 B</th><td>16.9</td><td>32.8</td><td>60.0</td></tr>
    <tr><th>8000 B</th><td>28.7</td><td>79.2</td><td>87.6</td></tr>
    <tr><th>1 MiB</th><td>37.3</td><td>72.2</td><td>84.4</td></tr>
  </tbody>
</table>

Throughput scales with the pair count; retransmits stay negligible over the run.

## Socket / UDP

Four one-way UDP client/server pairs, same one-core-per-pair pinning. UDP has no
flow control, so each sender runs flat-out and the receiver drops whatever it
cannot drain — the loss column is an inherent property of unpaced UDP, not a
fault. App RX is the delivered goodput; App-level loss is `(App TX − App RX) /
App TX`.

Each cell shows **receiver goodput in Gb/s** with the **app-level loss %** dimmed
beneath it:

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
    <tr><th>1000 B</th><td>4.4<small>11% loss</small></td><td>9.5<small>24% loss</small></td><td>14.1<small>23% loss</small></td></tr>
    <tr><th>8000 B</th><td>12.9<small>57% loss</small></td><td>22.7<small>59% loss</small></td><td>34.0<small>57% loss</small></td></tr>
  </tbody>
</table>

The sweep stops at 8000 B (single Ethernet frame). Larger UDP datagrams
fragment above the ~8972 B MTU payload; reassembly is all-or-nothing out of a
shared per-namespace pool, so under multi-pair unpaced load delivery collapses
(≈100% loss at 65507 B / 4 pairs). The wire itself is loss-free here; the loss
is host-side socket-buffer and reassembly pressure.

## Reproduce

Run inside the project container (privileged, GPUs passed through, hugepages
mounted), as root. Build with `-DCMAKE_BUILD_TYPE=Release` and
`cmake --install build` so the bench loads the current `libdaqiri.so`.

```bash
export DAQIRI_BUILD_DIR=./build
export LD_LIBRARY_PATH=/opt/daqiri/lib:${LD_LIBRARY_PATH:-}

# Raw Ethernet / GPUDirect — physical port loopback (netns must be torn down)
export ETH_DST_ADDR=$(cat /sys/class/net/<rx-iface>/address)
./examples/run_spark_bench.sh dpdk sweep

# Socket / RoCE and sockets — bring up the netns wire loopback first
./scripts/setup_spark_wire_loopback_netns.sh up
./examples/run_spark_bench.sh rdma sweep
./examples/run_spark_bench.sh socket-tcp sweep
./examples/run_spark_bench.sh socket-udp sweep
```

Each run writes `bench-results/<timestamp>-<backend>-<mode>/runs.csv`. See
[Socket and RDMA Benchmarking](socket_benchmarking.md) and
[Raw Ethernet Benchmarking](raw_benchmarking.md) for the namespace setup and
per-transport details.

---
**Previous:** [Raw Ethernet Benchmarking](raw_benchmarking.md)
