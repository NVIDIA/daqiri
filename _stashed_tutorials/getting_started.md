# Getting Started

## System Requirements

Achieving high-performance networking with DAQIRI requires a system with an [**NVIDIA SmartNIC**](https://www.nvidia.com/en-us/networking/ethernet-adapters/) (ConnectX-6 Dx or later) and a [**discrete GPU**](https://www.nvidia.com/en-us/design-visualization/desktop-graphics/). This includes [NVIDIA Data Center](https://www.nvidia.com/en-us/data-center/) systems, edge systems like the [NVIDIA IGX](https://www.nvidia.com/en-us/edge-computing/products/igx/) platform and [NVIDIA Project DIGITS](https://www.nvidia.com/en-us/project-digits/), and `x86_64` systems equipped with these components.

| Component | Requirement |
|-----------|-------------|
| **OS** | Linux (kernel 5.4+), Ubuntu 22.04 recommended |
| **NIC** | NVIDIA ConnectX-6 Dx or later, with MLNX_OFED or inbox drivers |
| **GPU** | Workstation/Quadro/RTX or Data Center GPU (GPUDirect-capable) |
| **CUDA** | CUDA Toolkit 11.7+ |
| **DPDK** | Included in the DAQIRI container; see [Dockerfile](../../Dockerfile) for bare-metal deps |
| **RDMA** | `libibverbs` and `librdmacm` (for the RDMA backend) |

!!! note

    If you use the DPDK bundled in the DAQIRI container, it is patched with dmabuf support and the `nvidia-peermem` kernel module is **not required**.

Once you have the hardware in place, the [System Requirements tutorial](system_requirements.md) walks through verifying your NIC drivers, switching link layers to Ethernet, configuring IP addresses, and enabling GPUDirect. The [System Optimization tutorial](system_optimization.md) covers additional performance tuning (hugepages, CPU isolation, GPU clocks, etc.).

The `python/tune_system.py` script can help diagnose common configuration issues at any time.

## Building from Source

First, add the [DOCA apt repository](https://developer.nvidia.com/doca-downloads?deployment_platform=Host-Server&deployment_package=DOCA-Host&target_os=Linux) which holds some of DAQIRI's dependencies:

=== "IGX OS 1.1"

    ```bash
    export DOCA_URL="https://linux.mellanox.com/public/repo/doca/2.8.0/ubuntu22.04/arm64-sbsa/"
    wget -qO- https://linux.mellanox.com/public/repo/doca/GPG-KEY-Mellanox.pub | gpg --dearmor - | sudo tee /etc/apt/trusted.gpg.d/GPG-KEY-Mellanox.pub > /dev/null
    echo "deb [signed-by=/etc/apt/trusted.gpg.d/GPG-KEY-Mellanox.pub] $DOCA_URL ./"  | sudo tee /etc/apt/sources.list.d/doca.list > /dev/null

    sudo apt update
    ```

=== "SBSA (Ubuntu 22.04)"

    ```bash
    export DOCA_URL="https://linux.mellanox.com/public/repo/doca/2.8.0/ubuntu22.04/arm64-sbsa/"
    wget -qO- https://linux.mellanox.com/public/repo/doca/GPG-KEY-Mellanox.pub | gpg --dearmor - | sudo tee /etc/apt/trusted.gpg.d/GPG-KEY-Mellanox.pub > /dev/null
    echo "deb [signed-by=/etc/apt/trusted.gpg.d/GPG-KEY-Mellanox.pub] $DOCA_URL ./"  | sudo tee /etc/apt/sources.list.d/doca.list > /dev/null

    # Also need the CUDA repository for holoscan: https://developer.nvidia.com/cuda-downloads?target_os=Linux
    wget https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2204/sbsa/cuda-keyring_1.1-1_all.deb
    sudo dpkg -i cuda-keyring_1.1-1_all.deb

    sudo apt update
    ```

=== "x86_64 (Ubuntu 22.04)"

    ```bash
    export DOCA_URL="https://linux.mellanox.com/public/repo/doca/2.8.0/ubuntu22.04/x86_64/"
    wget -qO- https://linux.mellanox.com/public/repo/doca/GPG-KEY-Mellanox.pub | gpg --dearmor - | sudo tee /etc/apt/trusted.gpg.d/GPG-KEY-Mellanox.pub > /dev/null
    echo "deb [signed-by=/etc/apt/trusted.gpg.d/GPG-KEY-Mellanox.pub] $DOCA_URL ./"  | sudo tee /etc/apt/sources.list.d/doca.list > /dev/null

    # Also need the CUDA repository for holoscan: https://developer.nvidia.com/cuda-downloads?target_os=Linux
    wget https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2204/x86_64/cuda-keyring_1.1-1_all.deb
    sudo dpkg -i cuda-keyring_1.1-1_all.deb

    sudo apt update
    ```

You can then build the DAQIRI library:

=== "Container build (recommended)"

    The container approach bundles all user-space libraries for each networking backend, avoiding dependency issues on the host:

    ```bash
    git clone git@github.com:nvidia-holoscan/daqiri.git
    cd daqiri
    BASE_TARGET=dpdk DAQIRI_MGR="dpdk rdma" scripts/build-container.sh
    ```

    !!! note

        Running the container with DPDK and GPU support requires specific flags and host system configuration. See [Running the DAQIRI container](benchmarking_examples.md#running-the-daqiri-container) for details.

=== "CMake build (bare-metal)"

    ```bash
    git clone git@github.com:nvidia-holoscan/daqiri.git
    cd daqiri
    cmake -S . -B build -DBUILD_SHARED_LIBS=ON -DDAQIRI_BUILD_PYTHON=OFF -DDAQIRI_MGR="dpdk rdma"
    cmake --build build -j
    cmake --install build --prefix /opt/daqiri
    ```

    !!! note

        Inspect the [Dockerfile](https://github.com/nvidia-holoscan/daqiri/blob/main/Dockerfile) to see the full list of user-space dependencies needed for a bare-metal build.

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `DAQIRI_MGR` | `"dpdk rdma"` | Space-separated list of backends to build. Valid values: `dpdk`, `rdma`. |
| `DAQIRI_BUILD_PYTHON` | `OFF` | Build pybind11 Python bindings. |
| `DAQIRI_BUILD_EXAMPLES` | `ON` | Build benchmark executables. |
| `BUILD_SHARED_LIBS` | — | Build as shared library. |

CUDA architectures are hardcoded to `80;90` (A100, H100) in `src/CMakeLists.txt`.

Refer to the [DAQIRI README](https://github.com/nvidia-holoscan/daqiri/blob/main/README.md) for more information on configuration and API usage.

## Running the Benchmarks

Two benchmark executables are built when `DAQIRI_BUILD_EXAMPLES=ON`:

- **`daqiri_bench_default`** — DPDK TX/RX throughput and latency benchmark
- **`daqiri_bench_rdma`** — RDMA-specific benchmark

Both are config-driven. Example configs are in the `examples/` directory:

| Config file | Description |
|-------------|-------------|
| `daqiri_bench_default_tx_rx.yaml` | DPDK TX/RX, CPU-only |
| `daqiri_bench_default_tx_rx_hds.yaml` | DPDK TX/RX with header-data split (GPUDirect) |
| `daqiri_bench_default_rx_multi_q.yaml` | DPDK multi-queue RX |
| `daqiri_bench_default_sw_loopback.yaml` | DPDK software loopback (no physical link needed) |
| `daqiri_bench_rdma_tx_rx.yaml` | RDMA client/server TX/RX |

Edit the YAML files to match your system (PCIe addresses, CPU cores, IP addresses) before running. Fields marked with `<angle brackets>` are placeholders that must be replaced.

For a full walkthrough of running your first benchmark, see the [Benchmarking Examples](benchmarking_examples.md) tutorial. For a detailed explanation of every YAML parameter, see [Understanding the Configuration File](configuration.md).

---

**Next:** [System Requirements](system_requirements.md) — verify your NIC drivers, link layers, and GPUDirect setup
