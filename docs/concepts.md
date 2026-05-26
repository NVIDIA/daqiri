---
hide:
  - navigation
---

# Concepts

This page is the DAQIRI glossary. It defines the terminology used everywhere
else in the docs — **kernel bypass**, **GPUDirect**, **packet / burst /
segment**, **flow / queue**, **memory region**, **zero-copy ownership**, and
**RX reorder** — and explains how those pieces fit together.

It is not a tutorial. There is no plot to follow. Keep this page open in a
second tab while you read the [API Guide](api-reference/index.md), the
[Configuration YAML Reference](api-reference/configuration.md), or the
[tutorials](tutorials/system_configuration.md), and jump back here whenever a
term needs grounding.

## Kernel Bypass

In this context, **kernel bypass** means bypassing the operating system's
kernel to talk directly to the network interface (NIC). That removes the
latency and overhead of the Linux network stack and lets the application work
with NIC ring buffers in user space.

DAQIRI is a thin, common interface over multiple kernel-bypass technologies.
All of its backends are Ethernet-based, but they differ in their model,
features, and footprint:

- **DPDK** — the [Data Plane Development Kit](https://www.dpdk.org/) is a
  Linux Foundation project with strong, long-running community support. Its
  RTE Flow capability is generally considered the most flexible solution for
  splitting ingress and egress data into per-queue streams.
- **RDMA** — Remote Direct Memory Access, using the open-source
  [`rdma-core`](https://github.com/linux-rdma/rdma-core) library. RDMA
  differs from the other Ethernet-based backends with its server/client
  model and **RoCE** (RDMA over Converged Ethernet) protocol. It costs more
  to set up on both ends but offers a simpler user interface, orders packets
  on arrival, and is the only backend with a high-reliability mode.
- **Socket** — a socket-oriented interface (UDP and TCP via
  the Linux kernel, plus a RoCE path that delegates to the RDMA backend).
  Useful as a comparison baseline against DPDK and RDMA, and as a path to
  first results when no NVIDIA NIC is available.

Which backend is best for your use case depends on multiple factors — packet
size, batch size, data type, whether you need ordering or reliability, and
whether both ends of the link are under your control. DAQIRI's goal is to
abstract the interface to these backends so developers can focus on
application logic and experiment with different configurations to find the
best technology for their use case.

??? example "Backend maturity"

    The DAQIRI library integration testing infrastructure is under active
    development. As such:

    - The **DPDK** backend is supported and distributed with the DAQIRI
      library, and is the only backend actively tested at this time.
    - The **RDMA / RoCE** backend is supported and distributed with the
      DAQIRI library; integration testing is under development.
    - The **Socket** backend (UDP/TCP via the Linux kernel, plus the RoCE
      path that delegates to RDMA) is supported and distributed; integration
      testing is under development.

## GPUDirect

**GPUDirect** allows the NIC to read and write data from/to a GPU without
having to first stage it through system memory. That decreases CPU overhead
and significantly reduces latency. An implementation of GPUDirect is
supported by every DAQIRI backend.

!!! warning

    GPUDirect is only supported on Workstation/Quadro/RTX GPUs and Data
    Center GPUs. It is not supported on GeForce cards.

??? info "How does that relate to peermem or dma-buf?"

    There are two interfaces to enable GPUDirect:

    - The [`nvidia-peermem`](https://docs.nvidia.com/cuda/gpudirect-rdma/)
      kernel module, distributed with the NVIDIA DKMS GPU drivers.
        - Supported on Ubuntu kernels 5.4+, deprecated starting with kernel
          6.8.
        - Supported on NVIDIA-optimized Linux kernels, including IGX OS and
          DGX OS.
        - Supported by all MOFED drivers (requires rebuilding `nvidia-dkms`
          drivers afterwards).
    - [`DMA Buf`](https://docs.kernel.org/driver-api/dma-buf.html),
      supported on Linux kernels 5.12+ with NVIDIA open-source drivers
      515+ and CUDA toolkit 11.7+.

    The DPDK that ships in the DAQIRI container is patched with dma-buf
    support, so the `nvidia-peermem` kernel module is **not required**
    inside the container.

For step-by-step system setup, see the
[System Configuration tutorial](tutorials/system_configuration.md#enable-gpudirect).

## Packets, Bursts, and Segments

These three terms are the unit of data that flows through DAQIRI. They show
up in every code path, every configuration option, and every API call.

### Packet

A **packet** is a single Ethernet frame the way the wire and the NIC see it
— headers and payload, as one logical unit. DAQIRI never delivers packets
one at a time; the unit of delivery is a *burst*.

### Burst (`BurstParams`)

A **burst** is a batch of packets grouped together for efficient transfer
between DAQIRI internals and the application. Bursts are the way the
application receives, transmits, and frees packets.

The C++ type for a burst is `BurstParams`. A burst carries:

- Pointers to the underlying packet buffers
- Packet count, port ID, queue ID, segment count
- Per-packet byte totals and lengths
- Flow IDs (when flow steering is configured)
- Optional RX hardware timestamps

`BurstParams` is meant to be opaque — applications use helper functions
(`get_packet_ptr`, `get_packet_length`, `get_num_packets`, ...) to inspect
or modify it rather than touching its fields directly.

### Segment

A **segment** is one contiguous memory region inside a packet. A packet can
have one segment or multiple segments. The number of segments a packet has
is set by the receive mode configured in the YAML:

- **Single segment** — used for CPU-only or batched-GPU paths that do not
  split headers from payloads.
- **Two segments (header-data split)** — segment 0 holds headers in CPU
  memory, segment 1 holds payload data in GPU memory.

### Header-Data Split (HDS)

**Header-data split** is the most common multi-segment configuration:
headers go to CPU memory (segment 0), payload goes to GPU memory
(segment 1). This keeps the GPU payload path zero-copy for downstream GPU
workloads while still letting the CPU parse and steer on the headers.

HDS is what makes "Ethernet straight to GPU" practical for protocols where
the application logic needs to look at headers (UDP source/destination
ports, sequence numbers in the application layer, etc.) but the bulk of
the data is meant for the GPU.

## Flows and Queues

These two terms describe how packets get routed from the wire into the
right application buffer.

### Queue

A **queue** is the NIC-side buffer that an application reads from (RX) or
writes to (TX). Each queue is bound to a CPU core in the YAML and is the
unit of parallelism: more queues mean more cores can do packet work in
parallel.

A queue points at one or more *memory regions* — that is where its packet
buffers actually live (CPU hugepages, GPU device memory, or pinned host
memory).

### Flow

A **flow** is a rule that tells the NIC "packets matching *this pattern*
should go to *this queue*". A flow has a match (e.g. UDP destination port
4096, IPv4 length 1050) and an action (e.g. *queue 0*). Flows are
configured under `rx.flows` in the YAML.

### Flow Steering

**Flow steering** is the NIC-level mechanism that classifies an incoming
packet against the configured flows and writes it into the matching
queue's buffer — all in NIC silicon, before any software runs. It is what
makes multi-queue RX scale: each flow can be pinned to its own queue (and
therefore its own core).

For DPDK, flow steering is implemented on top of RTE Flow. The YAML
options are documented in
[Configuration YAML Reference → Flows](api-reference/configuration.md#flows).

## Memory Regions

A **memory region** is a named pool of buffers where packet data lives.
Memory regions are declared at the top of the YAML and referenced by name
from each queue.

The kind of a memory region determines whether packet data ends up on the
CPU or the GPU:

- `huge` — CPU hugepages (recommended for CPU buffers).
- `device` — GPU VRAM (discrete GPUs; requires GPUDirect via peermem or
  DMA-BUF).
- `host_pinned` — pinned CPU pages allocated via `cudaHostAlloc`.
  Recommended on integrated GPUs (NVIDIA GB10 / DGX Spark), where the NIC
  cannot peer-DMA into device memory.
- `host` — regular CPU memory (not recommended for hot paths).

Combining memory regions on a single queue is how *header-data split* is
expressed in the YAML: queue 0's first memory region is a `huge` CPU pool
(for headers, segment 0); its second region is a `device` GPU pool (for
payload, segment 1).

## Zero-Copy Ownership

DAQIRI is designed around zero-copy packet delivery. When a receive API
returns packet data, the application is reading the buffers the NIC DMA'd
into — the API passes pointers and metadata, not copies.

That zero-copy model makes **buffer release part of the API contract**.
Applications must free RX bursts after processing and free or send TX
bursts after allocation. Holding bursts indefinitely drains DAQIRI's
buffer pools and can lead to `NO_FREE_BURST_BUFFERS`,
`NO_FREE_PACKET_BUFFERS`, queue drops, or stalled TX.

The mechanics — which `free_*` function to call when — live in the
[C++ API Usage page](api-reference/cpp.md#rx-step-3-free-buffers).

## RX Packet Aggregation and Reorder

DAQIRI can perform GPU- or CPU-side packet aggregation and reordering
on RX through `rx.reorder_configs`:

- **GPU reorder configs** copy selected packet payloads into a configured
  output memory region and deliver the result as one *reordered
  aggregate burst*.
- **CPU reorder configs** provide the same aggregate-burst model for
  CPU-addressable packet and output memory.

This is the path to use when packets arrive out of order (e.g. across
multiple NIC queues) and need to be reassembled into a single, contiguous
GPU buffer before downstream processing.

Each reorder config currently operates on a single memory domain, either GPU-only or
CPU-only. Reordering packets whose segments span two memory regions
(for example, an HDS pair with CPU-side headers and GPU-side payloads) is
not yet supported, but it will be in the future.

See [Configuration YAML Reference → RX Reorder Configs](api-reference/configuration.md#rx-reorder-configs-dpdk-v1)
for the configuration constraints and
[C++ API Usage → Reordered RX bursts](api-reference/cpp.md#reordered-rx-bursts)
for how to consume them from C++.

## See also

- [API Guide](api-reference/index.md) — the 6-step DAQIRI application
  lifecycle, with links into the language API.
- [Configuration YAML Reference](api-reference/configuration.md) — every
  YAML key, its type, and its valid values.
- [C++ API Usage](api-reference/cpp.md) — initialization, RX/TX, file
  writes, utilities, and the C++ function reference.
- [System Configuration tutorial](tutorials/system_configuration.md) —
  the real-world hardware and OS setup that makes the concepts above
  actually work.