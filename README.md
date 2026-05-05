# DAQIRI - Data Acquisition for Integrated Real-time Instruments

<img src="docs/logo.svg" alt="DAQIRI" width="220"/>

**Send and receive Ethernet packets into CPU and GPU memory at hundreds of Gbps with a simple API.** DAQIRI (Data Acquisition for Integrated Real-time Instruments) connects data acquisition systems to NVIDIA GPUs for real-time processing and AI, paving the way for autonomy of the next generation of scientific and industrial instruments.

> [!WARNING]
> The library is undergoing large improvements as we aim to better support it as an NVIDIA product.
> API breakages might be more frequent until we reach version 1.0.

DAQIRI provides direct NIC hardware access in userspace, bypassing the Linux kernel network stack to achieve the highest possible throughput and lowest latency for Ethernet frame transmission and reception. It targets NVIDIA ConnectX-6 Dx and later NICs and supports GPU direct memory access (GPUDirect) for zero-copy data paths between the NIC and GPU.

📖 **Live documentation: [nvidia.github.io/daqiri](https://nvidia.github.io/daqiri/)**

**Requires** an NVIDIA SmartNIC (ConnectX-6 Dx or later) and a discrete GPU. Tested on the NVIDIA DGX Spark, NVIDIA IGX platform, and an x86_64 RTX Pro server. See [Getting Started](docs/getting-started.md) for the full requirements list.

## Features

- **High Throughput** — Hundreds of gigabits per second with proper hardware and tuning.
- **Low Latency** — Direct access to NIC ring buffers; most latency is PCIe transit only.
- **GPUDirect** — Receive data directly into GPU memory via two modes:
  - *Header-data split*: Headers to CPU, payload to GPU (recommended for most workloads).
  - *Batched GPU*: Entire packets to GPU memory (maximum bandwidth, GPU-side parsing required).
- **Flow Steering** — Configure the NIC's hardware flow engine to route packets by UDP
  source/destination port.
- **RDMA** — InfiniBand RDMA operations (READ, WRITE, SEND) via the RDMA backend.

### Backends

| Backend | Config value | Description |
|---------|-------------|-------------|
| DPDK | `dpdk` | Userspace packet processing with DPDK mbufs and rings. |
| RDMA | `rdma` | InfiniBand RDMA via libibverbs (client/server model). |
| Socket | `socket` | Linux kernel sockets (UDP/TCP), plus a RoCE path that delegates to the RDMA backend. Selecting `socket` automatically builds `rdma`. |

### Limitations

- Only UDP fill mode is currently supported.

## Quick Start

Pick **one** of the two build paths below.

**Container build (recommended)** — bundles all user-space dependencies, including a patched DPDK with dmabuf support, so no host-side dependency setup is required:

```bash
BASE_TARGET=dpdk DAQIRI_MGR="dpdk socket rdma" scripts/build-container.sh
```

**Bare-metal CMake build** — use if you have all dependencies installed on the host (see the [Dockerfile](Dockerfile) for the full list):

```bash
cmake -S . -B build -DBUILD_SHARED_LIBS=ON -DDAQIRI_BUILD_PYTHON=OFF -DDAQIRI_MGR="dpdk socket rdma"
cmake --build build -j
cmake --install build --prefix /opt/daqiri
```

## Documentation

Reference material for the DAQIRI codebase:

- [Getting Started](docs/getting-started.md) — System requirements, build/install instructions, and CMake options
- [Configuration Reference](docs/configuration.md) — Full YAML config reference for all backends
- [API Guide](docs/api-guide.md) — BurstParams, RX/TX workflows, buffer lifecycle, status codes
- [Contributing](CONTRIBUTING.md) — Contribution guidelines, coding standards, DCO sign-off

## Tutorials

Step-by-step walkthroughs to get hands-on:

- [Background](docs/tutorials/background.md) — Kernel-bypass and GPUDirect concepts
- [System Configuration](docs/tutorials/system_configuration.md) — NIC drivers, link layers, GPUDirect, hugepages, CPU isolation, GPU clocks
- [Benchmarking Examples](docs/tutorials/benchmarking_examples.md) — run `daqiri_bench_raw_gpudirect` with a loopback test
- [Understanding the Configuration File](docs/tutorials/configuration-walkthrough.md) — annotated YAML walkthrough

## License

Apache 2.0 — see [LICENSE](LICENSE) for details.
