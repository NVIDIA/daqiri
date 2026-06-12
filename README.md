# DAQIRI - Data Acquisition for Integrated Real-time Instruments

<img src="docs/images/logo.svg" alt="DAQIRI" width="220"/>

**Send and receive Ethernet packets into CPU and GPU memory at hundreds of Gbps with a simple API.** DAQIRI (Data Acquisition for Integrated Real-time Instruments) connects data acquisition systems to NVIDIA GPUs for real-time processing and AI, paving the way for autonomy of the next generation of scientific and industrial instruments.

> [!WARNING]
> The library is undergoing large improvements as we aim to better support it as an NVIDIA product.
> API breakages might be more frequent until we reach version 1.0.

DAQIRI provides direct NIC hardware access in userspace, bypassing the Linux kernel network stack to achieve the highest possible throughput and lowest latency for Ethernet frame transmission and reception. It targets NVIDIA ConnectX-6 Dx and later NICs and supports GPU direct memory access (GPUDirect) for zero-copy data paths between the NIC and GPU.

📖 **Live documentation: [nvidia.github.io/daqiri](https://nvidia.github.io/daqiri/)**

**Requires** an NVIDIA SmartNIC (ConnectX-6 Dx or later) and a GPUDirect-capable NVIDIA GPU. Tested on the NVIDIA DGX Spark, NVIDIA IGX platform, and an x86_64 RTX Pro server. See [Getting Started](https://nvidia.github.io/daqiri/getting-started/) for the full requirements list.

## Features

- **High Throughput** — Sustained line rate with proper hardware and tuning.
- **Low Latency** — Direct access to NIC ring buffers; most latency is PCIe transit only.
- **GPUDirect** — Receive data directly into GPU memory via two modes:
  - *Header-Data Split*: Headers to CPU, payload to GPU (recommended for most workloads).
  - *Batched GPU*: Entire packets to GPU memory (maximum bandwidth, GPU-side parsing required).
- **Burst file writes** — Write received bursts as raw packet files or appendable PCAP
  captures. Host-backed buffers use POSIX writes; CUDA device-backed buffers can use cuFile/GDS.
- **S3 raw object writes** — Optionally upload raw burst packets to Amazon S3 or an
  S3-compatible object store through the AWS SDK for C++.
- **Flow Steering** — Configure the NIC's hardware flow engine to route packets by UDP
  source/destination port or flex-item payload fields. Per RX interface, use standard
  UDP/IP flows or flex-item flows, not both.
- **RDMA** — RDMA verbs (READ, WRITE, SEND) over RoCE on Ethernet NICs or InfiniBand.
- **Optional OpenTelemetry metrics** — Expose per-interface or per-queue packet,
  byte, and drop counters when built with `DAQIRI_ENABLE_OTEL_METRICS=ON`.

### Engines

An *engine* is the library that implements a [stream type](docs/concepts.md#stream-types). You configure the stream type and endpoint URIs; the engine is chosen for you by default. `DAQIRI_ENGINE` selects which **optional** engines are compiled in — Linux sockets are always available and need no engine value.

| `DAQIRI_ENGINE` value | Implements | Description |
|---------|-------------|-------------|
| `dpdk` | `stream_type: "raw"` (default) | Userspace kernel-bypass packet processing with DPDK mbufs and rings. Init aborts if RX flow rules or TX offload rules cannot be programmed on the NIC. |
| `ibverbs` | `stream_type: "raw"` with `engine: "ibverbs"`, and `stream_type: "socket"` with `roce://` endpoints | Two libibverbs-based engines built from one value: a pure-DevX Mellanox/mlx5 Multi-Packet (striding) Receive Queue (MPRQ) engine for raw Ethernet (opt in per stream with `engine: "ibverbs"`), and RDMA verbs over RoCE/InfiniBand for socket `roce://` endpoints (also backs the socket engine's RoCE path). The MPRQ engine eliminates DPDK's per-packet mbuf alloc/free; packets DMA strided into one pre-posted buffer (host or GPU via GPUDirect). |
| *(built in)* | `stream_type: "socket"` with `tcp://`/`udp://` endpoints | Linux kernel UDP/TCP sockets — always available, no build flag required. |

For `stream_type: "raw"` the engine defaults to `dpdk`; set `engine: "ibverbs"` on the
stream to use the MPRQ engine instead. Build it by including `ibverbs` in `DAQIRI_ENGINE`
(the default `"dpdk ibverbs"` already does).

### Limitations

- TX header-fill helpers currently support UDP only.
- Raw Ethernet configs must reference valid RX queue IDs in `rx.flows` `action.id`; init fails if flow rules cannot be installed on the NIC.
- The `ibverbs` raw (MPRQ) engine requires a Mellanox/mlx5 NIC (ConnectX-6 Dx or
  later, BlueField); it is DevX-based and not portable to other vendors.

## Quick Start

Pick **one** of the two build paths below.

**Container build (recommended)** — bundles all user-space dependencies, including a patched DPDK with dmabuf support, so no host-side dependency setup is required:

```bash
BASE_TARGET=dpdk DAQIRI_ENGINE="dpdk ibverbs" scripts/build-container.sh
```

Set `BASE_IMAGE=torch` to build on top of NGC PyTorch instead of the default CUDA base — useful for Torch / TensorRT inference workflows that ingest packets directly into GPU memory.

**Bare-metal CMake build** — use if you have all dependencies installed on the host (see the [Dockerfile](Dockerfile) for the full list):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DDAQIRI_BUILD_PYTHON=OFF -DDAQIRI_ENGINE="dpdk ibverbs"
cmake --build build -j
cmake --install build --prefix /opt/daqiri
```

Host-memory burst file writes do not require GPUDirect Storage. Enable cuFile support for
CUDA device-memory file writes with `-DDAQIRI_ENABLE_GDS=ON`; this requires `cufile.h`
and `libcufile` in the build environment. At runtime, regular GDS writes through
NVIDIA's `nvidia-fs` path require the `nvidia-fs` kernel module to be loaded and the
target storage stack to be reported as supported by `gdscheck.py -p`.

Enable raw packet uploads to S3 with `-DDAQIRI_ENABLE_S3=ON`. The recommended
container build installs AWS SDK for C++ with S3 support; bare-metal builds need
`aws-cpp-sdk-core` and `aws-cpp-sdk-s3` discoverable by CMake. S3 credentials are
resolved through the AWS SDK provider chain.

Container build:

```bash
BASE_TARGET=dpdk DAQIRI_ENGINE="dpdk ibverbs" scripts/build-container.sh
DAQIRI_ENABLE_S3=ON DAQIRI_BUILD_PYTHON=ON BASE_TARGET=dpdk scripts/build-container.sh
```

OpenTelemetry metrics are opt-in. Build with `-DDAQIRI_ENABLE_OTEL_METRICS=ON`
for CMake builds or `DAQIRI_ENABLE_OTEL_METRICS=ON` for container builds. DAQIRI
registers the instruments, while applications configure the OpenTelemetry SDK and
exporters.

See [Getting Started](https://nvidia.github.io/daqiri/getting-started/) for requirements, CMake options, and
running the benchmarks.

## Benchmarking

Start with the [Benchmarking overview](https://nvidia.github.io/daqiri/benchmarks/benchmarks/) to choose between Linux sockets, RoCE/RDMA, and raw Ethernet.

For Spark-style on-wire tests, use the same client/server namespace shape for Linux sockets and RDMA/RoCE: put the client-facing NIC in one namespace, the server-facing NIC in another, pin routes and neighbors to those interfaces, then verify `tx_packets_phy` on the client and `rx_packets_phy` on the server before trusting bandwidth numbers.

```bash
# Linux TCP/UDP sockets, split by namespace
ip netns exec dq_wire_server ./build/examples/daqiri_bench_socket \
  /tmp/socket-server.yaml --seconds 10 --mode server &
ip netns exec dq_wire_client ./build/examples/daqiri_bench_socket \
  /tmp/socket-client.yaml --seconds 10 --mode client
wait

# RoCE/RDMA, using the same namespace pair
ip netns exec dq_wire_server ./build/examples/daqiri_bench_rdma \
  /tmp/rdma-server.yaml --seconds 10 --mode server &
ip netns exec dq_wire_client ./build/examples/daqiri_bench_rdma \
  /tmp/rdma-client.yaml --seconds 10 --mode client
wait
```

See [Socket and RDMA Benchmarking](https://nvidia.github.io/daqiri/benchmarks/socket_benchmarking/) for the full namespace setup and YAML templates. See [Raw Ethernet Benchmarking](https://nvidia.github.io/daqiri/benchmarks/raw_benchmarking/) for DPDK/raw Ethernet loopback tests.

## Documentation

Reference material for the DAQIRI codebase:

- [Getting Started](https://nvidia.github.io/daqiri/getting-started/) — System requirements, build/install instructions, and CMake options
- [Concepts](https://nvidia.github.io/daqiri/concepts/) — Glossary of DAQIRI terminology (kernel bypass, GPUDirect, packet/burst/segment, flow/queue, memory region, zero-copy ownership, RX reorder). Meant to be opened in parallel with the rest of the docs.
- [API Guide](https://nvidia.github.io/daqiri/api-reference/) — Six-step DAQIRI application lifecycle and configuration-first model
- [Configuration YAML Reference](https://nvidia.github.io/daqiri/api-reference/configuration/) — Full YAML config reference for all engines
- [C++ API Usage](https://nvidia.github.io/daqiri/api-reference/cpp/) — C++ RX/TX workflows, buffer lifecycle, file writing, utilities, and status codes
- [Python API Usage](https://nvidia.github.io/daqiri/api-reference/python/) — Python bindings, workflow examples, enums, config classes, and helper functions
- [Performance: DGX Spark](https://nvidia.github.io/daqiri/benchmarks/performance-dgx-spark/) — Per-platform throughput, drop, and utilization numbers for stream/protocol combinations on DGX Spark
- [Contributing](CONTRIBUTING.md) — Contribution guidelines, coding standards, DCO sign-off

## Tutorials

Step-by-step walkthroughs to get hands-on:

- [System Configuration](https://nvidia.github.io/daqiri/tutorials/system_configuration/) — NIC drivers, link layers, GPUDirect, hugepages, CPU isolation, GPU clocks
- [Benchmarking Overview](https://nvidia.github.io/daqiri/benchmarks/benchmarks/) — choose between Linux sockets, RoCE/RDMA, and raw Ethernet benchmarks
- [Socket and RDMA Benchmarking](https://nvidia.github.io/daqiri/benchmarks/socket_benchmarking/) — run TCP/UDP sockets and RoCE/RDMA with matching namespace isolation
- [Raw Ethernet Benchmarking](https://nvidia.github.io/daqiri/benchmarks/raw_benchmarking/) — run `daqiri_bench_raw_gpudirect` with a physical loopback test
- [Understanding the Configuration File](https://nvidia.github.io/daqiri/tutorials/configuration-walkthrough/) — annotated YAML walkthrough
- [DAQIRI + Holoscan Integration](https://nvidia.github.io/daqiri/tutorials/daqiri-holoscan-integration/) — use DAQIRI RX bursts from a Holoscan source operator

## License

Apache 2.0 — see [LICENSE](LICENSE) for details.
