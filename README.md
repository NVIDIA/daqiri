# DAQIRI networking library

**From wire to GPU at line rate.** DAQIRI bypasses the Linux kernel to move raw sensor data directly into GPU memory at hundreds of gigabits per second.

> [!WARNING]
> The library is undergoing large improvements as we aim to better support it as an NVIDIA product.
> API breakages might be more frequent until we reach version 1.0.

DAQIRI provides direct NIC hardware access in userspace, bypassing the Linux kernel
network stack to achieve the highest possible throughput and lowest latency for Ethernet
frame transmission and reception. It targets NVIDIA ConnectX-6 Dx and later NICs and
supports GPU direct memory access (GPUDirect) for zero-copy data paths between the NIC
and GPU.

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
| DPDK | `dpdk` (default) | Userspace packet processing with DPDK mbufs and rings. |
| RDMA | `rdma` | InfiniBand RDMA via libibverbs (client/server model). |

### Limitations

- Only UDP fill mode is currently supported.

## Quick Start

```bash
cmake -S . -B build -DBUILD_SHARED_LIBS=ON -DDAQIRI_BUILD_PYTHON=OFF -DDAQIRI_MGR="dpdk rdma"
cmake --build build -j
cmake --install build --prefix /opt/daqiri
```

Container build:

```bash
BASE_TARGET=dpdk DAQIRI_MGR="dpdk rdma" scripts/build-container.sh
```

See [Getting Started](docs/getting-started.md) for requirements, CMake options, and
running the benchmarks.

## Documentation

| Document | Description |
|----------|-------------|
| [Getting Started](docs/getting-started.md) | Build, install, system requirements, benchmarks |
| [Configuration Reference](docs/configuration.md) | Full YAML config reference for all backends |
| [API Guide](docs/api-guide.md) | BurstParams, RX/TX workflows, buffer lifecycle, status codes |
| [Contributing](CONTRIBUTING.md) | Contribution guidelines, coding standards, DCO sign-off |

## License

Apache 2.0 — see [LICENSE](LICENSE) for details.
