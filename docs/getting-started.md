# Getting Started

## Requirements

### Hardware

- An NVIDIA NIC with a **ConnectX-6 Dx or later** chip

### Software

- **Linux** (kernel 5.4+)
- **CUDA Toolkit**
- **MLNX5/InfiniBand drivers** with peermem support, via one of:
  - Inbox (standard) drivers on Ubuntu kernel versions >= 5.4 and [< 6.8](https://discourse.ubuntu.com/t/nvidia-gpudirect-over-infiniband-migration-paths/44425)
  - NVIDIA optimized kernels (IGX OS, DGX BaseOS)
  - OFED drivers from [DOCA-Host](https://developer.nvidia.com/doca-archive) 2.8 or later
    (install the `mlnx-ofed-kernel-dkms` package or the `doca-ofed` meta-package)

  > **Note:** If you use the DPDK bundled in the DAQIRI container, all patches in
  > [`dpdk_patches/`](../dpdk_patches) are applied at build time (including dma-buf
  > support), so **peermem is not required**.
- **DPDK** (for the DPDK backend) — userspace libraries are included in the
  [Dockerfile](../Dockerfile). Inspect it if building on bare metal.
- **libibverbs** and **librdmacm** (for the RDMA backend)

### System Tuning

For best performance, the system must be tuned for high-performance networking (CPU
isolation, hugepages, NUMA awareness, etc.).

<!-- TODO: Port the high-performance networking tutorial to this repository -->
> A system tuning guide will be available in a future update.

The `python/tune_system.py` script can help diagnose common configuration issues.

## Building from Source

### CMake

```bash
cmake -S . -B build -DBUILD_SHARED_LIBS=ON -DDAQIRI_BUILD_PYTHON=OFF -DDAQIRI_MGR="dpdk rdma"
cmake --build build -j
cmake --install build --prefix /opt/daqiri
```

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `DAQIRI_MGR` | `"dpdk rdma"` | Space-separated list of backends to build. Valid values: `dpdk`, `rdma`. |
| `DAQIRI_BUILD_PYTHON` | `OFF` | Build pybind11 Python bindings. |
| `DAQIRI_BUILD_EXAMPLES` | `ON` | Build benchmark executables. |
| `BUILD_SHARED_LIBS` | — | Build as shared library. |

CUDA architectures are hardcoded to `80;90` (A100, H100) in `src/CMakeLists.txt`.

### Container Build

The repository includes a Dockerfile and build script that compiles DPDK from source:

```bash
BASE_TARGET=dpdk DAQIRI_MGR="dpdk rdma" scripts/build-container.sh
```

## Running the Benchmarks

Several benchmark executables are built when `DAQIRI_BUILD_EXAMPLES=ON`:

- **`daqiri_bench_raw_gpudirect`** — raw DPDK TX/RX using device packet memory
- **`daqiri_bench_raw_hds`** — raw DPDK TX/RX with header-data split
- **`daqiri_bench_raw_reorder_seq`** — raw DPDK RX sequence-number reorder benchmark
- **`daqiri_bench_rdma`** — RDMA-specific benchmark
- **`daqiri_bench_socket`** — TCP/UDP socket benchmark

They are config-driven. Example configs are in the `examples/` directory:

| Config file | Benchmark | Description |
|-------------|-----------|-------------|
| `daqiri_bench_raw_tx_rx.yaml` | `daqiri_bench_raw_gpudirect` | DPDK TX/RX with one device-memory packet segment |
| `daqiri_bench_raw_tx_rx_hds.yaml` | `daqiri_bench_raw_hds` | DPDK TX/RX with CPU headers and device payloads |
| `daqiri_bench_raw_tx_rx_reorder_seq_1024.yaml` | `daqiri_bench_raw_reorder_seq` | DPDK RX GPU reorder with 1024 packets per batch and a 32-bit UDP payload sequence |
| `daqiri_bench_raw_tx_rx_reorder_seq_1024_cpu.yaml` | `daqiri_bench_raw_reorder_seq` | DPDK RX CPU reorder with 1024 packets per batch |
| `daqiri_bench_raw_sw_loopback_reorder_seq_1024.yaml` | `daqiri_bench_raw_reorder_seq` | DPDK software-loopback RX CPU reorder |
| `daqiri_bench_raw_rx_reorder_seq_ppb.yaml` | `daqiri_bench_raw_reorder_seq` | RX-only GPU reorder using sequence packets per batch |
| `daqiri_bench_raw_rx_reorder_seq_batch.yaml` | `daqiri_bench_raw_reorder_seq` | RX-only GPU reorder using sequence and batch-number fields |
| `daqiri_bench_raw_rx_multi_q.yaml` | `daqiri_bench_raw_gpudirect` | DPDK multi-queue RX with device packet memory |
| `daqiri_bench_raw_sw_loopback.yaml` | `daqiri_bench_raw_gpudirect` | DPDK software loopback with device packet memory |
| `daqiri_bench_rdma_tx_rx.yaml` | `daqiri_bench_rdma` | RDMA client/server TX/RX |
| `daqiri_bench_socket_udp_tx_rx.yaml` | `daqiri_bench_socket` | UDP socket TX/RX |
| `daqiri_bench_socket_tcp_tx_rx.yaml` | `daqiri_bench_socket` | TCP socket TX/RX |

Edit the YAML files to match your system (PCIe addresses, CPU cores, IP addresses) before
running. Fields marked with `<angle brackets>` are placeholders that must be replaced.

Configs named `raw_rx_*` are RX-only. They initialize the RX path and wait for matching
external traffic; when run by themselves they can exit cleanly with `0` packets. Use the
TX/RX configs for closed-loop smoke tests. The CPU reorder config is a throughput stress
case, so dropped-packet counters can increase when TX outruns CPU reorder.

## Formatting

Run clang-format before committing:

```bash
# Format staged changes
git-clang-format --style file

# Format specific files
clang-format -style=file -i -fallback-style=none <files>
```
