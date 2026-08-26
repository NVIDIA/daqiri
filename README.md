# DAQIRI - Data Acquisition for Integrated Real-time Instruments

<img src="docs/images/logo.svg" alt="DAQIRI" width="220"/>

**Send and receive Ethernet packets into CPU and GPU memory at hundreds of Gbps per GPU with a simple API.** 

DAQIRI (Data Acquisition for Integrated Real-time Instruments) connects data acquisition systems to NVIDIA GPUs for real-time processing and AI, paving the way for autonomy of the next generation of scientific and industrial instruments.


DAQIRI provides direct NIC hardware access in userspace, bypassing the Linux kernel network stack to achieve the highest possible throughput and lowest latency for Ethernet frame transmission and reception. It targets NVIDIA ConnectX-6 Dx and later NICs and supports GPU direct memory access (GPUDirect) for zero-copy data paths between the NIC and GPU.

<table>
<tr><td align="center">📖</td><td><strong>Docs &amp; Website:</strong> <a href="https://nvidia.github.io/daqiri/">nvidia.github.io/daqiri</a></td></tr>
<tr><td align="center">⚡</td><td><strong>Peak performance</strong> requires an NVIDIA SmartNIC (ConnectX-6 Dx or later) and a GPUDirect-capable NVIDIA GPU</td></tr>
<tr><td align="center">🖥️</td><td><strong>Supported hardware:</strong> NVIDIA DGX Spark, NVIDIA IGX, and NVIDIA RTX Pro Servers</td></tr>
<tr><td align="center">🔌</td><td><strong>Works with any NIC and NVIDIA GPU</strong> via DAQIRI's built-in Linux Sockets engine</td></tr>
<tr><td align="center">🚀</td><td><strong>Getting Started:</strong> <a href="https://nvidia.github.io/daqiri/getting-started/">nvidia.github.io/daqiri/getting-started</a></td></tr>
</table>

## Table of Contents

- [Features](#features)
- [Benchmarking](#benchmarking)
- [Documentation](#documentation)
- [Tutorials](#tutorials)
- [License](#license)

## Features

- **High Throughput** — Sustained line rate with proper hardware and tuning.
- **Low Latency** — Direct access to NIC ring buffers; most latency is PCIe transit only.
- **GPUDirect** — Receive data directly into GPU memory via two modes:
  - *Header-Data Split*: Headers to CPU, payload to GPU (recommended for most workloads).
  - *Batched GPU*: Entire packets to GPU memory (maximum bandwidth, GPU-side parsing required).
- **Burst file writes** — Write received bursts as raw packet files or appendable PCAP
  captures. Host-backed buffers use POSIX writes; CUDA device-backed buffers can use cuFile/GDS.
- **AI/ML integration** — Optional `daqiri_resnet50_inference` application
  (`-DDAQIRI_BUILD_APPLICATIONS=ON`, TensorRT): GPUDirect RX → reorder → ResNet-50
  feature extraction with headless PC1/PC2 output. See
  [DAQIRI + TensorRT Inference](https://nvidia.github.io/daqiri/tutorials/daqiri-resnet-inference/).
- **S3 raw object writes** — Optionally upload raw burst packets to Amazon S3 or an
  S3-compatible object store through the AWS SDK for C++.
- **Flow Steering** — Configure the NIC's hardware flow engine to route packets by UDP
  source/destination port or flex-item payload fields. Raw RX flows can be configured
  statically in YAML or added/deleted dynamically after `daqiri_init()`. A scalar
  queue target steers directly to one queue; `ids: [0, 1, ...]` automatically enables
  flow-affine IPv4/UDP Toeplitz RSS across the listed queues on the raw DPDK and
  ibverbs engines. One unchanged five-tuple remains on one queue, so balanced packet
  counts require varied tuples. Per RX interface, use standard UDP/IP flows or
  flex-item flows, not both. Raw DPDK and raw ibverbs flows can also use hardware-only
  VLAN push/pop and VXLAN, GRE, or NVGRE encap/decap actions; socket/RDMA streams reject
  those tunnel actions.
- **RDMA** — RDMA verbs (READ, WRITE, SEND) over RoCE on Ethernet NICs or InfiniBand.
- **Linux socket control** — TCP/UDP socket streams expose connection IDs and
  `socket_setsockopt()` for native Linux `setsockopt` tuning without YAML option
  name mappings. The two buffer sizes that must be set before the first packet
  arrives, `rx_buffer_size` and `tx_buffer_size`, are YAML fields.
- **Optional OpenTelemetry metrics** — Expose per-interface or per-queue packet,
  byte, and drop counters when built with `DAQIRI_ENABLE_OTEL_METRICS=ON`.

## Benchmarking

Consult the [Benchmarking overview](https://nvidia.github.io/daqiri/benchmarks/) to learn more about generating and optimizing benchmarking on the NVIDIA platform, including:
- [Socket and RDMA Benchmarking](https://nvidia.github.io/daqiri/benchmarks/socket_benchmarking/) for the full namespace setup and YAML templates
- [Raw Ethernet Benchmarking](https://nvidia.github.io/daqiri/benchmarks/raw_benchmarking/) for DPDK/raw Ethernet loopback tests

### DGX Spark Result Summary

| Stream / Protocol        | Best case      | Throughput        | Drops     | Notes                                           |
|:-------------------------|:---------------|:------------------|:----------|:------------------------------------------------|
| Raw Ethernet / GPUDirect | 4 KB packet    | **105.5 ±0.9 Gb/s** | 0      | 98.5 Gb/s single-queue at the 8 KB native shape |
| Socket / RoCE (SEND)     | 8 MB message   | **102.2 ±0.3 Gb/s** | 0      | Single QP, batch 1                              |
| Socket / TCP             | 8 KB × 4 pairs | **97.2 ±2.8 Gb/s**  | ~0     | Flow-controlled (App TX = App RX)               |
| Socket / UDP             | 8 KB × 4 pairs | **29.8 ±0.2 Gb/s**  | ~51% loss | Receiver goodput; unpaced sender             |

Each transport at its best-case operation size on a single DGX Spark (GB10), driven over a physical cabled loopback on one ConnectX-7. Full methodology and per-transport breakdowns at [Performance: DGX Spark](https://nvidia.github.io/daqiri/benchmarks/performance-dgx-spark/). These tests were run using a 200G cable, which allowed transfers to reach PCIe limitations slightly over 100Gbps.

### RTX PRO 6000 Result Summary

| Stream / Protocol        | Best case      | Throughput          | Drops     | Notes                                              |
|:-------------------------|:---------------|:--------------------|:----------|:---------------------------------------------------|
| Socket / RoCE (SEND)     | 8 MB message   | **396.9 Gb/s**      | 0         | 99.2% of the port; NIC-driven sender               |
| Raw Ethernet / GPUDirect | 8000 B packet  | **390.5 Gb/s**      | 0         | GPU to GPU, no CPU touches a payload               |
| Raw Ethernet / HDS       | 8000 B payload | **389.0 Gb/s**      | 0         | Header to CPU, payload to GPU                      |
| Socket / TCP             | 1 MiB write    | **64.3 Gb/s**       | 0         | One kernel thread per side                         |
| Socket / UDP             | 65507 B datagram | **25.1 Gb/s**     | 0         | Paced; loss-free in every repetition               |

Each transport over a **400 GbE loopback cable** between two ports of one x86_64 RTX PRO 6000 Blackwell host, every rate read from the NIC's own PHY counters. Full methodology, the payload and multi-queue sweeps, the UDP loss curve, and the GPU-workload results at [Performance: RTX PRO 6000](https://nvidia.github.io/daqiri/benchmarks/performance-rtx-pro-6000/).

## Documentation

Reference material for the DAQIRI codebase:

- [Getting Started](https://nvidia.github.io/daqiri/getting-started/) — System requirements, build/install instructions, and CMake options
- [Concepts](https://nvidia.github.io/daqiri/concepts/) — Glossary of DAQIRI terminology (kernel bypass, GPUDirect, packet/burst/segment, flow/queue, memory region, zero-copy ownership, RX reorder). Meant to be opened in parallel with the rest of the docs.
- [API Guide](https://nvidia.github.io/daqiri/api-reference/) — Six-step DAQIRI application lifecycle and configuration-first model
- [Configuration YAML Reference](https://nvidia.github.io/daqiri/api-reference/configuration/) — Full YAML config reference for all engines
- [C++ API Usage](https://nvidia.github.io/daqiri/api-reference/cpp/) — C++ RX/TX workflows, buffer lifecycle, file writing, utilities, and status codes
- [Python API Usage](https://nvidia.github.io/daqiri/api-reference/python/) — Python bindings, workflow examples, enums, config classes, and helper functions
- [Performance: DGX Spark](https://nvidia.github.io/daqiri/benchmarks/performance-dgx-spark/) — Per-platform throughput, drop, and utilization numbers for stream/protocol combinations on DGX Spark
- [Performance: RTX PRO 6000](https://nvidia.github.io/daqiri/benchmarks/performance-rtx-pro-6000/) — The same for a 400 GbE loopback on an x86_64 RTX PRO 6000 Blackwell host, measured from NIC counters
- [Contributing](CONTRIBUTING.md) — Contribution guidelines, coding standards, DCO sign-off

## Tutorials

Step-by-step walkthroughs to get hands-on:

- [System Configuration](https://nvidia.github.io/daqiri/tutorials/system_configuration/) — NIC drivers, link layers, GPUDirect, hugepages, CPU isolation, GPU clocks
- [Benchmarking Overview](https://nvidia.github.io/daqiri/benchmarks/) — choose between Linux sockets, RoCE/RDMA, and raw Ethernet benchmarks
- [Socket and RDMA Benchmarking](https://nvidia.github.io/daqiri/benchmarks/socket_benchmarking/) — run TCP/UDP sockets and RoCE/RDMA with matching namespace isolation
- [Raw Ethernet Benchmarking](https://nvidia.github.io/daqiri/benchmarks/raw_benchmarking/) — run `daqiri_bench_raw_gpudirect` with a physical loopback test
- [Dynamic RX Flow Example](https://nvidia.github.io/daqiri/tutorials/configuration-walkthrough/#choosing-an-example-config) — start with RX queues only, then add and delete flow-steering rules at runtime
- [Understanding the Configuration File](https://nvidia.github.io/daqiri/tutorials/configuration-walkthrough/) — annotated YAML walkthrough
- [DAQIRI + Holoscan Integration](https://nvidia.github.io/daqiri/tutorials/daqiri-holoscan-integration/) — use DAQIRI RX bursts from a Holoscan source operator
- [DAQIRI + TensorRT Inference](https://nvidia.github.io/daqiri/tutorials/daqiri-resnet-inference/) — packet ingest → ResNet-50 feature extraction with TensorRT

## License

Apache 2.0 — see [LICENSE](LICENSE) for details.
