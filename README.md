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
- **UCX GPU egress example** — Optional containerized raw-Ethernet ingress →
  batched CUDA processing → UCP Active Message egress application. Build with
  `BASE_TARGET=ucx DAQIRI_BUILD_APPLICATIONS=ON DAQIRI_BUILD_RESNET50_INFERENCE=OFF
  DAQIRI_BUILD_UCX_GPU_EGRESS=ON scripts/build-container.sh`. See
  [DAQIRI + UCX GPU Egress](https://nvidia.github.io/daqiri/tutorials/daqiri-ucx-gpu-egress/).
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
  name mappings.
- **Flow-control telemetry** — Raw Ethernet streams warn at `daqiri_init()` when 802.3x
  pause is enabled on a port and report the pause frames exchanged during the run with the
  shutdown stats. A paused link throttles the sender instead of dropping, so it caps
  throughput with every drop counter at zero. Check a host up front with
  `python/tune_system.py --check pause`.
- **Optional OpenTelemetry metrics** — Expose per-interface or per-queue packet,
  byte, and drop counters when built with `DAQIRI_ENABLE_OTEL_METRICS=ON`.

## Benchmarking

Consult the [Benchmarking overview](https://nvidia.github.io/daqiri/benchmarks/) to learn more about generating and optimizing benchmarking on the NVIDIA platform, including:
- [Socket and RDMA Benchmarking](https://nvidia.github.io/daqiri/benchmarks/socket_benchmarking/) for the full namespace setup and YAML templates
- [Raw Ethernet Benchmarking](https://nvidia.github.io/daqiri/benchmarks/raw_benchmarking/) for DPDK/raw Ethernet loopback tests
- [Performance: DGX Spark](https://nvidia.github.io/daqiri/benchmarks/performance-dgx-spark/) for measured benchmarks on a single DGX Spark (GB10) in cabled loopback to use as a reference target

## Documentation

Reference material for the DAQIRI codebase:

- [Getting Started](https://nvidia.github.io/daqiri/getting-started/) — System requirements, build/install instructions, and CMake options
- [Concepts](https://nvidia.github.io/daqiri/concepts/) — Glossary of DAQIRI terminology (kernel bypass, GPUDirect, packet/burst/segment, flow/queue, memory region, zero-copy ownership, RX reorder). Meant to be opened in parallel with the rest of the docs.
- [API Guide](https://nvidia.github.io/daqiri/api-reference/) — Six-step DAQIRI application lifecycle and configuration-first model
- [Configuration YAML Reference](https://nvidia.github.io/daqiri/api-reference/configuration/) — Full YAML config reference for all engines
- [C++ API Usage](https://nvidia.github.io/daqiri/api-reference/cpp/) — C++ RX/TX workflows, buffer lifecycle, file writing, utilities, and status codes
- [Python API Usage](https://nvidia.github.io/daqiri/api-reference/python/) — Python bindings, workflow examples, enums, config classes, and helper functions
- [Performance: DGX Spark](https://nvidia.github.io/daqiri/benchmarks/performance-dgx-spark/) — Per-platform throughput, drop, and utilization numbers for stream/protocol combinations on DGX Spark
- [DAQIRI + UCX GPU Egress](https://nvidia.github.io/daqiri/tutorials/daqiri-ucx-gpu-egress/) — Raw Ethernet ingress → CUDA assembly/processing → UCP Active Message egress
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
- [DAQIRI + UCX GPU Egress](https://nvidia.github.io/daqiri/tutorials/daqiri-ucx-gpu-egress/) — packetized images → batched CUDA transform → remote GPU-accessible receive slots over UCP

## License

Apache 2.0 — see [LICENSE](LICENSE) for details.
