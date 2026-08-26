# RTX PRO 6000 benchmark baseline

**The measured results live in the docs:
[Performance: RTX PRO 6000](../docs/benchmarks/performance-rtx-pro-6000.md)**
([published](https://nvidia.github.io/daqiri/benchmarks/performance-rtx-pro-6000/)).
That page is the single home for every number, so they cannot drift between two
copies. This file is the host-specific record that belongs next to the scripts:
the topology, what has to be true before a run means anything, and how to
re-run it.

Measured 2026-08-25 on `sa-era-rtx03`, branch `ccrozier-rtx-pro-6000-bench`.
Every cell is a closed loop over one 400 GbE cable between two ports of this
host, and every rate is read from the NIC's own counters on those two ports.
Nothing is a software loopback.

## Topology

```
GPU0 03:00.0 ─┐
              ├─ PIX ─ ens15f0np0  0000:05:00.0  400 GbE ─┐
GPU1 04:00.0 ─┘                                           │ loopback cable
GPU2 73:00.0 ─┐                                           │
              ├─ PIX ─ ens16f0np0  0000:75:00.0  400 GbE ─┘
GPU3 74:00.0 ─┘
```

Both ports are on NUMA 0. `scripts/discover_rtx_pro_topology.sh` finds this pair
from mutual LLDP neighbours (or, while the socket/RoCE namespaces are up, from
the namespaces themselves) and fills in the PCIe addresses, destination MAC, PIX
GPU ordinals, and poll cores. Do not assume port names: `61:00.0`/`61:00.1` on
this host are a second, uncabled card, and `carrier=1` on both of them makes
them look like a loopback pair when they are not. If LLDP has nothing to say —
`lldpd` will not re-learn a port that has been moved into a namespace and back
without a restart — the pairing is still checked on every run, because a wrong
pair means no frames arrive at the receiving port and the run fails.

| | |
|---|---|
| Host | 2 x AMD EPYC 9555 64-core, 2 NUMA nodes, no `isolcpus` |
| GPU | NVIDIA RTX PRO 6000 Blackwell Server Edition, `CMAKE_CUDA_ARCHITECTURES=120` |
| NIC | Mellanox MT43244 BlueField-3 integrated ConnectX-7, firmware 32.42.1000 |
| Link | 400 GbE; the wire budget is 400 Gb/s of frames plus preamble and interframe gap, which is what the PHY byte counters measure |
| Build | Release, `DAQIRI_ENGINE="dpdk ibverbs"`, in the project container |

## Prerequisites, or the numbers are wrong

- **802.3x pause off** on both ports (`ethtool -A <if> rx off tx off`). Pause
  costs over 20% of line rate here and leaves every drop counter at zero, so a
  flow-controlled run looks clean and simply reports less.
- **MTU 9000** on both ports; the sweep sends 8000-byte payloads. Note that a
  `dpdk` or `ibverbs` run leaves the netdev MTU at the frame size *it* needed —
  8046 after an 8000-byte run — because those engines set it rather than raise
  it. That is fine for the run itself, but it silently changes what a later
  `socket-*` or `rdma` run does, since those segment to whatever the MTU is now.
  `socket-tcp` reaches 64 Gb/s with 9012-byte frames and would be measuring
  something else at 8046. Set the MTU back to 9000 between backends:

```bash
for i in ens15f0np0 ens16f0np0; do echo 9000 > /sys/class/net/$i/mtu; done
```

- **A receive buffer for `socket-udp`.** The config asks for 64 MiB, and the
  kernel clamps that to `net.core.rmem_max` — which, unlike most of `net.core`,
  is not per-namespace, so the host value governs even inside the wire-loopback
  namespaces. At the stock 256 KB the receiving socket holds under four of the
  65507-byte datagrams this sweep sends and discards the rest *after* they have
  crossed the cable, where no NIC counter and no sender ever sees it:

```bash
sysctl -w net.core.rmem_max=67108864
```

- **Namespaces for `rdma` and `socket-*` only.** Both ports belong to this host,
  so with both IPs in one namespace the kernel routes between them locally and
  the NIC eswitch short-cuts RoCE — the frames never reach the cable, and
  RDMA-CM fails outright on an address it sees as local. Put each port in its
  own namespace first, and take them back down before any `dpdk`/`ibverbs` run:

```bash
CLIENT_IF=ens15f0np0 SERVER_IF=ens16f0np0 \
  sudo -E scripts/setup_spark_wire_loopback_netns.sh up
sudo scripts/setup_spark_wire_loopback_netns.sh down
```

  Name both ports explicitly, as above. Left to autodetect, the setup script
  groups the carrier-up RoCE netdevs by switch ID and takes one per physical
  port, which picks the wrong server port on this host — there is a second
  ConnectX-7 whose two ports are also cabled and jumbo, and it wins the
  grouping. The symptom is that the namespaces come up clean and
  `setup_spark_wire_loopback_netns.sh verify` then loses 100% of its pings.

`examples/run_rtx_pro_bench.sh` checks pause, MTU, receive-buffer ceiling, and
namespace state up front, and fails any run whose frames did not actually reach
the far port. It also puts `build/src` ahead of `/opt/daqiri/lib` on the library
path, so a run measures the library that was just compiled rather than the copy
baked into the container image.

## What each benchmark does

Every one of these is the same physical journey — out of `ens15f0np0`, down the
cable, into `ens16f0np0` — and they differ in who builds the packets, where the
data sits at each end, and how much software touches it in between. There is also
a software-loopback config (`daqiri_bench_raw_sw_loopback*.yaml`) that never
touches a port; it exists to smoke-test a build, and no published number uses it.

- **`dpdk` GPUDirect** — one process is both the sender and the receiver. It
  fills 8000-byte UDP packets in GPU 0's memory, the NIC pulls them straight out
  of that GPU and onto the cable, and the receiving port writes them straight
  into GPU 2's memory. No CPU ever reads or copies a payload. This is the
  headline number.
- **`dpdk-hds`** — the same journey, except the receiving NIC cuts each arriving
  packet in two: the header goes to CPU memory and the 8000-byte payload to GPU
  memory. This is what you want when software has to read sequence numbers or
  timestamps while the payload never leaves the GPU.
- **`issue17`** — the same journey as `dpdk`, plus real GPU work on each window of
  packets as it arrives: an FFT or a matrix multiply (1024-point and 1024x1024 by
  default), run on the bytes that actually came off the wire. It answers "how much
  of the link do I keep once the GPU is also busy".
- **payload and batch sweep** — the same journey repeated with payloads from
  8000 bytes down to 64, and several batch sizes. Smaller packets mean far more
  packets per second for the same bitrate, so this finds the size at which a
  single receive core, rather than the cable, becomes the limit.
- **multi-queue matrix** — the same journey with one or two send queues and one or
  two receive queues, each pinned to its own CPU core. It separates "the link is
  full" from "one core is full".
- **`rdma` (RoCE)** — two separate processes on one queue pair. The client posts
  8 MB RC SENDs; the NIC splits each into 4 KB frames (the RoCE path MTU under a
  9000-byte netdev MTU), sends them across, and the receiving HCA puts the bytes
  directly into the server's pre-posted receive memory. Nothing runs per-packet
  on either CPU.
- **`socket-udp`** — two processes again, but ordinary Linux sockets: the client
  sends 65507-byte datagrams and the server reads them. Every datagram is split
  by the kernel into 8 IP fragments to fit the 9000-byte MTU and reassembled on
  the far side. UDP never retransmits, so anything the receiver cannot keep up
  with is simply gone. Run twice: once with the sender flat out, and once as a
  ladder of paced rates (`drop-curve`) to find the rate the receiver can take
  without losing anything.
- **`socket-tcp`** — the same, as a TCP stream written in 1 MiB chunks. One kernel
  thread per side does the segmenting, checksums and copies.
- **`ibverbs`** — talks to the NIC through libibverbs and a multi-packet receive
  queue instead of DPDK, as a DPDK sender feeding an MPRQ receiver.

**Why the socket and RoCE benchmarks need namespaces.** Both ports belong to this
one machine, so the kernel would recognise the destination address as its own and
hand the data over internally. The packets would never touch the cable and the
result would be a memory-copy benchmark wearing a network label — and RDMA-CM
refuses to connect to an address it believes is local. Putting each port in its
own network namespace hides that from the kernel, so the only route from one to
the other is out the wire and back.

## What each run leaves behind

Every run writes `wire_validation.txt` next to its CSV, holding the NIC's frame
and byte counts for that run, what it lost, and any pause frames — the raw
evidence behind the published tables. `wire_tx_gbps` and `wire_rx_gbps` in
`runs.csv` carry the same figures per cell.

Rates come from `ethtool -S` on both ports, before and after each run, from the
`*_phy` counter family: what the hardware itself put on and took off the cable,
independent of the benchmark's own bookkeeping or when it started its timer. Two
conditions fail a run outright rather than recording it as a slow result, because
both produce a believable-looking number that does not mean what it says:

- frames the sending port emitted that never arrived at the receiving port, and
- any 802.3x pause frame, which throttles the sender without incrementing a
  single drop counter.

A *paced* run gets one more check. If the NIC's transmit rate comes in more than
5% under the rate the sender was asked to hold, the cell is flagged and left out
of the medians. Without it, a cell that lost its transmit core for fifteen seconds
and only managed 15 Gb/s still reports zero loss under a "paced to 20" label —
because the receiver was never pushed — and reads as the cleanest point on the
curve. One repetition in this campaign did exactly that.

`bench-results/aggregate.py` and `bench-results/aggregate_curve.py` turn the
per-run CSVs into the medians the docs page publishes.

## The ibverbs library-path mistake, kept as a warning

Earlier revisions of the results recorded ibverbs RX as a hard failure on this
NIC: the MPRQ engine built its striding RQ and CQ and then aborted with
`dr_rule_create failed (port 0 catch-all): Cannot allocate memory`, which was
attributed to the driver refusing a software-steering rule on this BlueField-3
integrated ConnectX-7.

That conclusion was wrong, and the reason is worth keeping: the benchmark was
loading the container image's installed `libdaqiri.so` rather than the one built
from this branch. The image predated a merge from `origin/main` that rewrote about
a thousand lines of the ibverbs engine, flow steering included — so the fix was in
the source and never in the binary under test. Hence `build/src` ahead of
`/opt/daqiri/lib` on the library path in every runner. Against the branch's own
library the receive side comes up and takes traffic, though not at the rate the
transmit side sends; the current figures are on the docs page.

## Reproducing

```bash
# Namespaces down:
sudo ./examples/run_rtx_pro_bench.sh dpdk     nic-smoke --seconds 30
sudo ./examples/run_rtx_pro_bench.sh dpdk     sweep     --seconds 15
sudo ./examples/run_rtx_pro_bench.sh dpdk     issue17   --seconds 20
sudo ./examples/run_rtx_pro_bench.sh dpdk-hds nic-smoke --seconds 30
sudo ./examples/run_rtx_pro_bench.sh ibverbs  nic-smoke --seconds 30
sudo ./examples/run_rtx_pro_mq_bench.sh

# Namespaces up (see the prerequisites above):
sudo ./examples/run_rtx_pro_bench.sh rdma       nic-smoke  --seconds 20
sudo ./examples/run_rtx_pro_bench.sh socket-tcp nic-smoke  --seconds 20
sudo ./examples/run_rtx_pro_bench.sh socket-udp nic-smoke  --seconds 20

# The UDP loss-free rate: a ladder of paced sender rates, ending unpaced.
sudo ./examples/run_rtx_pro_bench.sh socket-udp drop-curve --seconds 15
```

Inside the project container, launched with
`./examples/run_rtx_pro_container.sh <name> "<command>"` — this host has one
faulted GPU, which makes `nvidia-container-cli` fail NVML enumeration and breaks
`docker run --gpus` for every container, so the launcher falls back to passing
the healthy device nodes directly.

## Follow-ups

- 800 Gbps is only available here as a bidirectional aggregate: one cable and
  two 400 GbE ports is 400 Gbps per direction. A TX+RX-on-both-ports config
  would reach it; nothing in the harness does that today.
- Raise the UDP receive ceiling in the socket engine. The receive path allocates
  a buffer per datagram and copies each one out of the kernel's staging buffer,
  on top of the copy the kernel already did, and it ignores the memory regions
  the config declares for the queue. Receiving straight into pooled buffers would
  remove both the allocation and one of the two copies. That is an engine change
  rather than a benchmark one, so it is not in this PR.
- Work out why `ibverbs` RX consumes far less than its own transmit side holds,
  and give the ibverbs cell a single-process form so its whole-run wire rate is a
  measurement rather than a lower bound.
- Account for the frames the GPU-workload cells leave on the receive queue. The
  engine's `imissed`/`ierrors`/`nombuf` counters stay near zero, so the ~22
  million unconsumed packets per run are not attributed to anything.
- Set `isolcpus` on this host. Run-to-run spread is a few percent on the fast
  backends and far wider on the `socket-udp` read rate, which is what poll
  threads sharing cores with the scheduler look like. It also cost one paced cell
  25% of its transmit rate outright, which is now caught and discarded rather
  than believed. Until then, treat differences of a few percent between runs as
  noise, and prefer the paced `socket-udp` row, which lost nothing in any
  repetition.
- Give the `socket-udp` ladder more repetitions, and finer rungs between 25 and
  30 Gb/s. Four is enough to show that 28 is not dependable but not enough to say
  where the reliable ceiling actually sits.
