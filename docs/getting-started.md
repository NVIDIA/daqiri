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

Two benchmark executables are built when `DAQIRI_BUILD_EXAMPLES=ON`:

- **`daqiri_bench_raw`** — raw DPDK TX/RX throughput and latency benchmark
- **`daqiri_bench_rdma`** — RDMA-specific benchmark

Both are config-driven. Example configs are in the `examples/` directory:

| Config file | Description |
|-------------|-------------|
| `daqiri_bench_raw_tx_rx.yaml` | DPDK TX/RX, CPU-only |
| `daqiri_bench_raw_tx_rx_hds.yaml` | DPDK TX/RX with header-data split (GPUDirect) |
| `daqiri_bench_raw_tx_rx_reorder_seq_1024.yaml` | DPDK TX/RX with GPU RX reorder (1024 packets per batch) using a 32-bit sequence in UDP payload |
| `daqiri_bench_raw_tx_rx_reorder_seq_1024_cpu.yaml` | DPDK TX/RX with CPU RX reorder (1024 packets per batch) using `memcpy` over CPU-accessible buffers |
| `daqiri_bench_raw_sw_loopback_reorder_seq_1024.yaml` | DPDK software-loopback TX/RX with GPU RX reorder (1024 packets per batch) and TX-injected 32-bit UDP payload sequence |
| `daqiri_bench_raw_rx_multi_q.yaml` | DPDK multi-queue RX |
| `daqiri_bench_raw_sw_loopback.yaml` | DPDK software loopback (no physical link needed) |
| `daqiri_bench_rdma_tx_rx.yaml` | RDMA client/server TX/RX |

Edit the YAML files to match your system (PCIe addresses, CPU cores, IP addresses) before
running. Fields marked with `<angle brackets>` are placeholders that must be replaced.

## Formatting

Run clang-format before committing:

```bash
# Format staged changes
git-clang-format --style file

# Format specific files
clang-format -style=file -i -fallback-style=none <files>
```
