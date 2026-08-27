---
hide:
  - navigation
---

# Performance: RTX PRO 6000

Measured throughput for each stream type on a single x86_64 RTX PRO 6000 Blackwell
server, over a **400 GbE loopback cable** joining two ports of one ConnectX-7.
Every rate on this page is read from the NIC's own PHY byte counters on those two
ports, so it is what the hardware put on and took off the cable rather than what
the benchmark believed it did.

Nothing here is a software loopback. The `raw` benchmarks drive the two physical
ports directly; RoCE and sockets cross the same cable through a pair of network
namespaces, because both ports belong to one host and the kernel would otherwise
route between them internally and never touch the wire.

!!! warning "Not comparable to the DGX Spark page"
    [Performance: DGX Spark](performance-dgx-spark.md) is a 100 GbE cable on an
    integrated GB10, where the *cable* is the ceiling for every large-transfer
    result. This is a 400 GbE cable on a discrete-GPU x86_64 host, so the results
    separate out by transport instead of all converging at the link. Read the two
    as different platforms, not as a before-and-after.

All backends are measured **one-way**: one side sends, the other receives.
400 Gb/s is the per-direction budget of one cable, and nothing in this harness
drives both directions at once.

## System under test

| Component | Detail |
| --------- | ------ |
| Platform | 2 × AMD EPYC 9555 64-core, 2 NUMA nodes, **no `isolcpus`** |
| GPU | NVIDIA RTX PRO 6000 Blackwell Server Edition, built with [`CMAKE_CUDA_ARCHITECTURES=120`](../tutorials/bare-metal-cmake-build.md) |
| NIC | Mellanox MT43244 BlueField-3 integrated ConnectX-7, firmware 32.42.1000 |
| Link | Two 400 GbE ports on the same host joined by one loopback cable, MTU 9000, 802.3x pause **off** |
| Build | Release (`-DCMAKE_BUILD_TYPE=Release`), `DAQIRI_ENGINE="dpdk ibverbs"`, in the project container |
| Loopback | `raw` (DPDK, ibverbs) drives the two physical ports directly; RoCE and sockets use the `dq_wire_*` network-namespace wire loopback |
| Core pinning | Each queue gets a busy-spin poller core and the bench worker a separate core, both on the NUMA node local to their port. Without `isolcpus` these share the machine with the scheduler, which is the dominant source of run-to-run spread |

Both ports sit on NUMA 0, each PIX to its own pair of GPUs:

```
GPU0 03:00.0 ─┐
              ├─ PIX ─ ens15f0np0  0000:05:00.0  400 GbE ─┐
GPU1 04:00.0 ─┘                                           │ loopback cable
GPU2 73:00.0 ─┐                                           │
              ├─ PIX ─ ens16f0np0  0000:75:00.0  400 GbE ─┘
GPU3 74:00.0 ─┘
```

`carrier=1` on two ports does not make them a pair — this host has a second,
uncabled ConnectX-7 whose ports also show carrier.
[`scripts/discover_rtx_pro_topology.sh`](https://github.com/nvidia/daqiri/blob/main/scripts/discover_rtx_pro_topology.sh)
identifies the cabled pair from mutual LLDP neighbours (or, while the namespaces
are up, from the namespaces themselves) and fills the PCIe addresses, destination
MAC, PIX GPU ordinals, and poll cores into each config.

## Results summary

Median of 3 repetitions per cell (4 for the UDP ladder). **Throughput** is what
the link carried; **goodput** is what the receiving application read out of it.

| Stream / Protocol | Best case | Throughput | Goodput | Lost packets |
| ----------------- | --------- | ---------: | ------: | ------------ |
| Socket / RoCE (SEND/RECV) | 8 MB message | **396.9 Gb/s** (99.2% of the port) | 383.5 | 0 |
| Raw Ethernet / ibverbs | 8000 B packet | **390.4 Gb/s** (97.6%) | 260.8 | 0 |
| Raw Ethernet / GPUDirect (DPDK) | 8000 B packet | **390.5 Gb/s** (97.6%) | 373.1 | 0 |
| Raw Ethernet / header-data split (DPDK) | 8000 B payload | **389.0 Gb/s** (97.3%) | 375.2 | 0 |
| Socket / TCP | 1 MiB write | **64.3 Gb/s** | 63.2 | 0 |
| Socket / UDP, unpaced | 65507 B datagram | **47.1 Gb/s** | 31.1 | 0 |
| Socket / UDP, paced | 65507 B datagram | **25.1 Gb/s** | 25.0 | 0 |

Every run above carried every frame it sent and saw no pause frames — both are
enforced as gates, so a run that loses frames on the cable or gets flow-controlled
is recorded as a failure rather than as a slow result. Where throughput and
goodput disagree the difference was discarded inside the host, *after* the wire
had already delivered it: that is the whole story of the `ibverbs` and unpaced UDP
rows, and it is never a property of the link.

!!! warning "The transfer sizes are not equal, so read down the column with care"
    Each row is that backend's best case at its own natural transfer size, which
    is what you would pick building on it — not a controlled comparison. RoCE
    tops the table with an 8 MB message, and its per-message cost is amortised
    over ~2,000 frames the NIC generates itself; handed 8 KB messages instead it
    would pay that cost per frame and drop well down the table. A future revision
    should hold the transfer size equal across backends before these rows are
    read against each other.

**Frame size is not what separates these results either.** RoCE holds the highest
rate with the *smallest* frames of the fast backends (4057 B on the wire, against
8068 B for the DPDK path), and `socket-tcp` uses the largest (9012 B) and is six
times slower. What separates them is how often the sender has a frame ready:
RoCE's transmit engine runs entirely inside the NIC, working from one large
message, and leaves fewer gaps than a software sender polling a queue, while one
kernel thread per side cannot fill a 400 GbE pipe at all.

## How the numbers are measured

Each run snapshots `ethtool -S` on both ports before and after, and reports from
the `*_phy` counter family. Those are the hardware's own tallies, so they do not
depend on the benchmark's bookkeeping or on when it started its timer.

| Figure | What it is |
| ------ | ---------- |
| **Throughput** | The sending port's byte counter sampled once a second and averaged over the run, discarding the first and last (partial) samples. A mean, not a peak: a maximum is one sample and says nothing about what the link held. It counts the frame, CRC, preamble and interframe gap — everything that occupies the link — so it is directly comparable to the port's 400 Gb/s. |
| **Goodput** | What the receiving program itself reported: payload bytes it actually got to use. Shown next to throughput so the two can be compared — where they disagree, the difference is being lost inside the host, after the wire already delivered it. |
| **Lost packets** | Frames the sending port emitted minus frames the receiving port took off. Any non-zero value implicates the link itself; host-side losses do not appear here. |
| **Pause frames** | 802.3x flow control seen by either port. Pause throttles a sender without incrementing any drop counter, so a paused run looks clean and is simply slow. One pause frame fails the run. |

!!! warning "The tables are not all on the same throughput metric yet"
    The summary above uses the sampled per-second mean. The payload sweep,
    multi-queue and GPU-workload tables below still divide each run's total bytes
    by its duration, which folds in engine bring-up and reads a few percent lower
    for the same cell. Compare throughput against goodput *within* a row, which is
    what each table is for; do not read a throughput figure from one table against
    one from another. The harness now records the sampled mean for every cell, so
    a re-measurement will put all of them on one metric.

A **paced** run gets one more check, in both directions. If the NIC's transmit
rate lands more than 5% under the rate the sender was told to hold, the cell is
discarded rather than averaged in: a cell that lost its transmit core and only
managed 15 Gb/s otherwise reports zero loss under a "paced to 20" label — because
the receiver was never pushed — and reads as the cleanest point on the curve. More
than 5% *over* the target fails the same way, since a sender that overshot was not
holding the rate the row claims either. Framing overhead alone puts the wire
figure a few percent above a payload-rate target, which is what the 5% allows for.

## Raw Ethernet / ibverbs

The [ibverbs engine](raw_benchmarking.md) drives the NIC through libibverbs and
mlx5dv directly, with no DPDK. Its receive side is a multi-packet (striding)
receive queue. The cell is two processes: a DPDK sender on one port feeding the
ibverbs receiver on the other.

Sending is not the problem — the transmit port holds **390.4 Gb/s**, matching
DPDK. Goodput is **260.8**, dropping 65.5 M of 146 M packets. Every frame the
sender emitted arrived, with no pause frames, so the shortfall is inside the
receive path and not on the link.

!!! warning "This row is an open item and understates the engine"
    The receive region here is host hugepages, so the payload lands in the CPU and
    not the GPU — that alone is not the comparison the GPUDirect row below makes.
    A separate investigation points at per-packet cost in the poll loop rather
    than queue depth or memory placement, but publishing that means re-measuring
    this section, which has not been done yet. Read this row as a floor, not as
    the engine's ceiling.

!!! note "This row's whole-window rate is not usable, and is not what is quoted"
    Every other backend here is one process whose own timer bounds the traffic, so
    dividing the NIC's byte totals by it gives the rate. This cell is two
    processes, and the sender both starts earlier and outlives the receiver's
    measured window — dividing all the bytes by only the receiver's 20 s produces
    472 Gb/s on a 400 Gb/s port. The runner rejects any rate above line rate and
    falls back to the full snapshot window, which spans start-up and teardown and
    therefore understates instead. The sampled throughput above is immune to both
    and is the figure used.

## Raw Ethernet / GPUDirect (DPDK)

One process is both sender and receiver. It fills 8000-byte UDP packets in GPU 0's
memory, the NIC pulls them straight out of that GPU onto the cable, and the
receiving port writes them straight into GPU 2's memory. No CPU reads or copies a
payload.

Throughput is **390.5 Gb/s**, 97.6% of the port. Over 20 s the sending port put
115,783,681 frames on the wire and the receiving port took exactly 115,783,681
off, and the three repetitions held 390.5, 390.4 and 390.5 Gb/s with the two ports
agreeing to within 0.01 Gb/s in each. The receiver reports ~5,500 dropped packets
out of 115 million (0.005%), all of it host-side.

### Payload sweep, one core per direction (best batch per payload, 15 s per cell)

One send queue on one core, one receive queue on one core. Everything below 4096 B
in this table is a **single-core** result, not a property of the transport — the
[multi-queue table](#multi-queue-core-scaling-one-core-per-queue-20-s-per-cell) is the same sweep
with cores added.

| Payload | Throughput | Goodput | At batch |
| ------- | ---------: | ------: | -------- |
| 8000 B | 372.3 | 371.9 | 1024 |
| 4096 B | 372.0 | 371.5 | 1024 |
| 1024 B | 355.5 | 259.2 | 10240 |
| 256 B  | 100.6 | 77.8  | 1024 |
| 64 B   | 33.7  | 31.3  | 10240 |

Down to 4096 B goodput tracks throughput closely and the link is the limit. Below
that the two part company — and throughput falls as well, because with one queue
per direction small packets bottleneck the *sender* too. That is why 64 B reaches
only 33.7 Gb/s on the wire here while two send queues push 111.8 at the same size.
At 1024 B the gap is purely on the receive side: the link carries 355.5 and one
receive core takes 259.2.

Because one process owns both directions in these cells, a sender that runs out of
core caps the row before the receiver is ever tested. Splitting the two across
hosts, so the transmit side can always outrun the receiver under test, would
measure the receive path on its own; that is a change to the harness rather than a
re-analysis of these runs.

### Multi-queue core scaling, one core per queue (20 s per cell)

Each cell is **throughput / goodput** — what the link carried, and what the
receiving program read out of it. `2t1r` is two send queues and one receive queue.
Every queue gets its own busy-spin poller core, so `2t1r` occupies three poller
cores in total and `2t2r` four; adding a queue here always means adding a core, and
none of these rows describe several queues sharing one. Cells are derived from the
single base
[`daqiri_bench_raw_tx_rx_rtx_pro_6000_mq.yaml`](https://github.com/nvidia/daqiri/blob/main/examples/daqiri_bench_raw_tx_rx_rtx_pro_6000_mq.yaml)
by [`run_rtx_pro_mq_bench.sh`](https://github.com/nvidia/daqiri/blob/main/examples/run_rtx_pro_mq_bench.sh).

| Payload | 1t1r | 1t2r | 2t1r | 2t2r |
| ------- | ---- | ---- | ---- | ---- |
| 64 B   | 57.1 / 31.7   | 57.2 / 57.1   | 112.3 / 31.0  | 111.8 / 52.0  |
| 256 B  | 137.3 / 77.8  | 137.4 / 136.0 | 254.0 / 76.6  | 253.3 / 130.6 |
| 1024 B | 355.8 / 262.2 | 355.8 / 355.7 | 372.2 / 255.7 | 373.8 / 372.2 |
| 4096 B | 368.4 / 368.3 | 368.4 / 368.3 | 370.7 / 370.5 | 371.0 / 370.9 |
| 8000 B | 369.6 / 369.4 | 369.6 / 369.4 | 371.6 / 371.2 | 371.5 / 371.1 |

The two halves of each cell are the point. At 8000 B they agree, so the link is
the limit and there is nothing to tune. Below ~4096 B they diverge, always on the
receive side: at 1024 B with two send queues the link carried 372.2 while one
receive core took 255.7, and a second receive core recovers nearly all of it
(372.2). A second *send* core alone only widens the gap, since it pushes harder at
a receiver that already could not keep up.

So a low number at a small payload is not the link running out. It is one core
running out, and the frames it could not take are dropped inside the host after
the wire had already delivered them.

### GPU work on the received data (8000 B, 3 × 20 s)

The same journey plus real GPU work on each window of packets as it arrives, run
on the bytes that actually came off the wire (`--workload`, see
[Raw Ethernet Benchmarking](raw_benchmarking.md#run-the-loopback-test)). It
answers how much of the link you keep once the GPU is also busy.

| Workload | Throughput | Goodput |
| -------- | ---------: | ------: |
| `none` (baseline) | 373.4 | 373.1 |
| `fft` (batched length-1024 C2C) | 360.8 | 288.5 |
| `gemm` (1024³ FP32) | 361.7 | 289.2 |

With no workload the two columns agree, so the link is the limit. Add the FFT or
the matrix multiply and the link still carries ~361 Gb/s while the application
consumes ~289: the GPU has become the bottleneck, and about a fifth of the frames
that reached the port are never taken off the receive queue.

!!! note "These cells should be re-run at 8192 B"
    8000 B is not a power of two, so cuFFT and cuBLAS both pay for an awkward
    length here and the GPU is charged more than the transport costs it. The
    runner now selects 8192 B for this mode; the table above predates that and is
    the 8000 B measurement.

## Raw Ethernet / header-data split

The same journey, except the receiving NIC cuts each arriving packet in two — the
header to CPU memory, the 8000-byte payload to GPU memory. This is what you want
when software has to read sequence numbers or timestamps while the payload never
leaves the GPU. It costs almost nothing: **389.0 Gb/s** throughput against
GPUDirect's 390.5 (0.4% less), and goodput of 375.2 against 373.1 — the split
receive path is, if anything, marginally ahead on what the application gets.

## Socket / RoCE

Two processes over the namespace wire loopback, one queue pair. The client posts
8 MB RC **SENDs** and the server pre-posts matching **RECEIVEs**; the NIC splits
each message into 4 KB frames (the RoCE path MTU under a 9000-byte netdev MTU),
and the receiving HCA places the bytes directly into the memory behind those
receive requests. Nothing runs per-packet on either CPU, which is why this is the
fastest backend on the page: **396.9 Gb/s** throughput, 99.2% of the port, with
383.5 goodput.

Note what the NIC is being given here: one 8 MB message becomes ~2,000 frames
generated by the transmit engine itself, so the per-transfer cost is paid once and
amortised across all of them. That is the RoCE result, not a like-for-like against
the 8 KB packets the raw backends are handed.

## Socket / TCP

Ordinary Linux TCP, written in 1 MiB chunks. **64.3 Gb/s** throughput and 63.2
goodput — TCP self-paces, so the two track each other and nothing is lost.

This is 16% of the port with the largest frames on the page. It is a single
*stream* result rather than a single *core* one: the application uses one sending
and one receiving thread, but the segmentation, checksums and copies are not
confined to them — the NIC's TSO and GRO do part of it and the kernel does the
rest in softirq context on whatever cores the IRQs land, which is why the poll
cores this harness pins read as idle for these cells. Where a single stream lands
is very sensitive to the host: the same benchmark with one client/server pair on
DGX Spark reaches
[32.1 Gb/s at the same 1 MiB message](performance-dgx-spark.md#socket-tcp), and
the EPYC 9555 here is a much faster core with a ConnectX-7's offloads behind it.
That doc also shows the axis this page does not sweep — a second and fourth pair
take Spark to 51.5 and 83.7 — so read 64.3 as one stream's worth, not the port's.
The comparison worth drawing is against RoCE on the same cable, six times faster
with smaller frames.

## Socket / UDP

Ordinary Linux UDP: the client sends 65507-byte datagrams, the kernel splits each
into 8 IP fragments to fit the 9000-byte MTU, and the far side reassembles. UDP
never retransmits, so whatever the receiver cannot keep up with is gone.

This is the one backend whose sender comfortably outruns its own receiver. Flat
out, the cable carries 46.5 Gb/s and the receiving *port* takes all of it, but the
application reads **31.1** — the kernel's `Udp: InErrors` accounts for the entire
difference, 520,430 datagrams of 1.77 M sent, every one discarded *after* the
frames were safely off the wire.

UDP landing *below* TCP on the same cable is backwards from what the protocols
would suggest: UDP does none of TCP's ordering or retransmission work, and TCP
here has NIC segmentation offload helping it while every UDP datagram is
fragmented by the kernel into 8 pieces and reassembled on the far side. The cause
is on the receive side and is per-datagram cost in DAQIRI's own socket engine
rather than anything intrinsic to UDP; it is listed under
[known limitations](#known-limitations) as an open item. Expect this row to move
once the engine's receive path stops allocating and copying per datagram.

### How much UDP this receiver can take (4 × 15 s)

The sender is held to a fixed rate and the ladder climbed until datagrams start
disappearing. **Clean reps** counts the repetitions that lost nothing at all,
which is the column that matters: a rate is only safe if it is safe every time.

| Sender held to | On the wire | Read | Median lost | Clean reps | Worst rep |
| -------------- | ----------: | ---: | ----------- | ---------- | --------- |
| 5 Gb/s | 5.0 | 5.0 | 0 | 4 of 4 | 0 |
| 10 Gb/s | 10.0 | 10.0 | 0 | 4 of 4 | 0 |
| 20 Gb/s | 20.1 | 20.0 | 0 | 3 of 3 [^pace] | 0 |
| **25 Gb/s** | **25.1** | **25.0** | **0** | **4 of 4** | **0** |
| 28 Gb/s | 28.1 | 28.0 | 0 | 3 of 4 | 130,371 |
| 30 Gb/s | 30.0 | 24.3 | 19% | 0 of 4 | 193,792 |
| 35 Gb/s | 35.2 | 30.2 | 14% | 0 of 4 | 320,108 |
| unpaced | 46.5 | 35.4 | 23% | 0 of 4 | 364,807 |

[^pace]: The 20 Gb/s rung has three repetitions because the fourth put only
15.2 Gb/s on the wire. The sender never held the rate, so the cell was not a
measurement of 20 Gb/s and the pacing check discarded it.

**25 Gb/s is the rate to quote** — maximum-size datagrams, nothing lost, in every
repetition. 28 Gb/s is where it stops being dependable: three repetitions lost
nothing and the fourth lost 130,371 datagrams, 16% of that run. Nothing
distinguished the bad repetition except when it happened to run, which is exactly
why the clean-reps column exists — the median for that rung is a flat zero and
would have hidden it.

Two things worth knowing about the shape of this curve:

- **The receive buffer is not what limits it.** The socket asks for 64 MiB via
  `rx_buffer_size`, about a thousand datagrams of headroom. Raising it from the
  stock 256 KB moved the unpaced read rate from 29 to 33 Gb/s and no further: a
  buffer absorbs a burst, it cannot make a receiver faster. What limits this
  receiver is per-datagram work, and across payloads of 1472, 8192 and 65507 bytes
  the cost fits **1.26 µs + 0.256 ns/byte** almost exactly. That per-byte term is
  the ceiling near 31 Gb/s, regardless of datagram size. Sizing the buffer is
  still necessary — see
  [Size the receive buffer](socket_benchmarking.md#size-the-receive-buffer-or-lose-datagrams-you-already-paid-for)
  — it just is not sufficient.
- **The ceiling wanders, so it is a range and not a line.** The unpaced row reads
  35.4 Gb/s, *more* than the 24.3 the receiver manages when the sender is paced to
  30. The receive path takes up to 32 datagrams per `recvmmsg` call, so an
  unpaced sender leaves a deep queue and amortises the syscall across a full
  batch, while a smoothly paced sender delivers closer to one at a time and pays
  the per-call cost on each. Across all seven unpaced repetitions the read rate
  came out at 25.6, 31.1, 32.7, 33.5, 34.7, 36.1 and 45.9 Gb/s. That spread is
  wider than the gap between two rungs of the ladder, which is why the
  recommendation is 25 rather than the highest number that ever worked.

## Known limitations

- **`ibverbs` goodput of 260.8 Gb/s** while its own transmit side holds 390.4.
  Open item; the cable is not implicated. The row was measured with a host-memory
  receive region, so it is not the same configuration as the GPUDirect row, and it
  needs re-measuring against one before the two are compared.
- **The UDP receive ceiling is per-datagram cost in the socket engine**, which
  allocates a buffer per datagram and copies it out of the kernel's staging
  buffer, on top of the copy the kernel already did. Receiving into pooled buffers
  would remove the allocation and one of the two copies. That is an engine change,
  tracked separately from the benchmark harness, and it is also why UDP sits below
  TCP here.
- **The GPU-workload cells leave frames unconsumed** — the link carries 361 Gb/s
  and the application takes 289 — but the engine's `imissed`/`ierrors`/`nombuf`
  counters stay near zero, so those ~22 M packets per run are not attributed to
  anything.
- **Sender and receiver share a host on the raw backends.** One process owns both
  directions, so a send side that runs out of core caps a row before the receive
  side is tested — visible in the small-payload sweep rows. Separating the two
  across hosts would let the transmit side always outrun the receiver under test.
- **The rows are not a controlled comparison.** Each is its own best case at its
  own transfer size; RoCE's 8 MB message in particular flatters it against the
  8 KB packets the raw backends get.
- **No `isolcpus` on this host.** Run-to-run spread is a few percent on the fast
  backends and far wider on `socket-udp`. Treat small differences between runs as
  noise, and prefer the paced UDP row.
- **800 Gb/s is only a bidirectional aggregate here.** One cable and two ports is
  400 Gb/s per direction; nothing in this harness drives both ways.

## Reproduce

Run inside the project container as root, from a Release build. The runner puts
`build/src` ahead of `/opt/daqiri/lib` on the library path, so a run measures the
library just compiled rather than the copy baked into the container image — a
distinction that cost an earlier revision of this page one wrong conclusion about
`ibverbs`.

```bash
./examples/run_rtx_pro_container.sh <name> "<command>"
```

Three prerequisites, or the numbers mean something other than what they say:

```bash
# Pause off on both ports -- pause costs >20% of line rate and leaves every drop
# counter at zero, so a flow-controlled run looks clean and is simply slower.
for i in ens15f0np0 ens16f0np0; do ethtool -A $i rx off tx off; done

# MTU 9000. A dpdk/ibverbs run leaves the netdev at the frame size *it* needed
# (8046 after an 8000 B run), which silently changes what a later socket/rdma run
# segments to. Reset it between backends.
for i in ens15f0np0 ens16f0np0; do echo 9000 > /sys/class/net/$i/mtu; done

# A receive buffer ceiling for socket-udp. net.core.rmem_max is not
# per-namespace, so the host value governs inside the wire-loopback namespaces.
sysctl -w net.core.rmem_max=67108864
```

The `raw` backends need the namespaces **down** (they capture the ports and hide
them from DPDK); RoCE and sockets need them **up**. Name both ports explicitly —
left to autodetect, the setup script groups carrier-up RoCE netdevs by switch ID
and picks this host's second, uncabled card:

```bash
CLIENT_IF=ens15f0np0 SERVER_IF=ens16f0np0 \
  ./scripts/setup_spark_wire_loopback_netns.sh up      # not Spark-specific
./scripts/setup_spark_wire_loopback_netns.sh verify
./scripts/setup_spark_wire_loopback_netns.sh down
```

Then the cells themselves. Each writes `bench-results/<timestamp>-<backend>-<mode>/`
with a `runs.csv` carrying `wire_tx_sampled_gbps` / `wire_rx_sampled_gbps` (the
published figures) alongside the whole-window `wire_tx_gbps` / `wire_rx_gbps`, and
a `wire_validation.txt` holding the raw NIC frame and byte counts, what was lost,
and any pause frames:

```bash
# Namespaces down:
./examples/run_rtx_pro_bench.sh dpdk     nic-smoke    --seconds 20
./examples/run_rtx_pro_bench.sh dpdk     sweep        --seconds 15
./examples/run_rtx_pro_bench.sh dpdk     gpu-workload --seconds 20
./examples/run_rtx_pro_bench.sh dpdk-hds nic-smoke    --seconds 20
./examples/run_rtx_pro_bench.sh ibverbs  nic-smoke    --seconds 20
RUN_SECONDS=20 REPEATS=3 ./examples/run_rtx_pro_mq_bench.sh

# Namespaces up:
./examples/run_rtx_pro_bench.sh rdma       nic-smoke  --seconds 20
./examples/run_rtx_pro_bench.sh socket-tcp nic-smoke  --seconds 20
./examples/run_rtx_pro_bench.sh socket-udp nic-smoke  --seconds 20
./examples/run_rtx_pro_bench.sh socket-udp drop-curve --seconds 15
```

For the configs themselves see the
[RTX PRO 6000 profile callout](raw_benchmarking.md#update-the-loopback-configuration)
and [Socket and RDMA Benchmarking](socket_benchmarking.md).
