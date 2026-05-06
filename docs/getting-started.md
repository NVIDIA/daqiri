# Getting Started

## System Requirements

DAQIRI requires a system with an [**NVIDIA SmartNIC**](https://www.nvidia.com/en-us/networking/ethernet-adapters/) (ConnectX-6 Dx or later) and a [**discrete GPU**](https://www.nvidia.com/en-us/design-visualization/desktop-graphics/).

| Component | Requirement |
|-----------|-------------|
| **OS** | Linux (kernel 5.4+), Ubuntu 22.04 recommended |
| **NIC** | NVIDIA ConnectX-6 Dx or later, with MLNX_OFED or inbox drivers |
| **GPU** | Workstation/Quadro/RTX or Data Center GPU (GPUDirect-capable) |
| **CUDA** | CUDA Toolkit 11.7+ |
| **DPDK** | Included in the DAQIRI container; see [Dockerfile](https://github.com/NVIDIA/daqiri/blob/main/Dockerfile) for bare-metal deps |
| **RDMA** | `libibverbs` and `librdmacm` (for the RDMA backend) |
| **GDS** | Optional `cufile.h` and `libcufile` for file writes from CUDA device memory. Runtime device-memory writes require a working cuFile installation; for regular `nvidia-fs` mode, the `nvidia-fs` kernel module must be loaded and the destination storage stack must be supported. |

Supported platforms include [NVIDIA Data Center](https://www.nvidia.com/en-us/data-center/) systems, edge systems like [NVIDIA IGX](https://www.nvidia.com/en-us/edge-computing/products/igx/) and [NVIDIA Project DIGITS](https://www.nvidia.com/en-us/project-digits/), and `x86_64` systems with the above components.

!!! note

    If you use the DPDK bundled in the DAQIRI container, it is patched with dmabuf support and the `nvidia-peermem` kernel module is **not required**.

For detailed instructions on verifying NIC drivers, configuring link layers, enabling GPUDirect, and tuning your system for maximum performance, see the [System Configuration tutorial](tutorials/system_configuration.md).

## Build the DAQIRI Library

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

    # Also need the CUDA repository: https://developer.nvidia.com/cuda-downloads?target_os=Linux
    wget https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2204/sbsa/cuda-keyring_1.1-1_all.deb
    sudo dpkg -i cuda-keyring_1.1-1_all.deb

    sudo apt update
    ```

=== "x86_64 (Ubuntu 22.04)"

    ```bash
    export DOCA_URL="https://linux.mellanox.com/public/repo/doca/2.8.0/ubuntu22.04/x86_64/"
    wget -qO- https://linux.mellanox.com/public/repo/doca/GPG-KEY-Mellanox.pub | gpg --dearmor - | sudo tee /etc/apt/trusted.gpg.d/GPG-KEY-Mellanox.pub > /dev/null
    echo "deb [signed-by=/etc/apt/trusted.gpg.d/GPG-KEY-Mellanox.pub] $DOCA_URL ./"  | sudo tee /etc/apt/sources.list.d/doca.list > /dev/null

    # Also need the CUDA repository: https://developer.nvidia.com/cuda-downloads?target_os=Linux
    wget https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2204/x86_64/cuda-keyring_1.1-1_all.deb
    sudo dpkg -i cuda-keyring_1.1-1_all.deb

    sudo apt update
    ```

Then build the DAQIRI library:

=== "Container build (recommended)"

    The container bundles all user-space libraries for each networking backend, avoiding dependency issues on the host:

    ```bash
    git clone git@github.com:nvidia-holoscan/daqiri.git
    cd daqiri
    BASE_TARGET=dpdk DAQIRI_MGR="dpdk socket rdma" scripts/build-container.sh
    ```

    Set `BASE_IMAGE=torch` to build on top of NGC PyTorch instead of the default CUDA base — useful for Torch / TensorRT inference workflows that ingest packets directly into GPU memory:

    ```bash
    BASE_IMAGE=torch BASE_TARGET=dpdk DAQIRI_MGR="dpdk socket rdma" scripts/build-container.sh
    ```

=== "CMake build (bare-metal)"

    ```bash
    git clone git@github.com:nvidia-holoscan/daqiri.git
    cd daqiri
    cmake -S . -B build -DBUILD_SHARED_LIBS=ON -DDAQIRI_BUILD_PYTHON=OFF -DDAQIRI_MGR="dpdk socket rdma"
    cmake --build build -j
    cmake --install build --prefix /opt/daqiri
    ```

    Inspect the [Dockerfile](https://github.com/NVIDIA/daqiri/blob/main/Dockerfile) to see the full list of user-space dependencies needed for a bare-metal build.

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `DAQIRI_MGR` | `"dpdk socket rdma"` | Space-separated list of backends to build. Valid values: `dpdk`, `socket`, `rdma`. |
| `DAQIRI_BUILD_PYTHON` | `OFF` | Build pybind11 Python bindings. |
| `DAQIRI_BUILD_EXAMPLES` | `ON` | Build benchmark executables. |
| `DAQIRI_ENABLE_GDS` | `OFF` | Enable cuFile-backed burst file writes from CUDA device memory. Host-memory writes use POSIX APIs without GDS. |
| `BUILD_SHARED_LIBS` | — | Build as shared library. |

CUDA architectures are hardcoded to `80;90` (A100, H100) in `src/CMakeLists.txt`.

When using `DAQIRI_ENABLE_GDS=ON` for CUDA device-memory storage writes, verify the
runtime stack before running DAQIRI:

```bash
lsmod | grep nvidia_fs
/usr/local/cuda/gds/tools/gdscheck.py -p
```

For regular cuFile/GDS over local NVMe, `gdscheck.py -p` should report `NVMe :
Supported`, and ext4 destinations must be mounted with `data=ordered` or use another
GDS-supported filesystem such as XFS. If `nvidia-fs` is not loaded, or the destination
storage is not supported, DAQIRI returns `NOT_SUPPORTED` for CUDA device-backed burst
writes. Host-backed burst writes continue to use POSIX APIs and do not require GDS.

## Next Steps

Once DAQIRI is built, follow the tutorials to configure your system and run your first benchmark:

1. [**Background**](tutorials/background.md) — Kernel Bypass and GPUDirect concepts
2. [**System Configuration**](tutorials/system_configuration.md) — NIC drivers, link layers, GPUDirect, hugepages, CPU isolation, GPU clocks, and more
3. [**Benchmarking Examples**](tutorials/benchmarking_examples.md) — run `daqiri_bench_raw_gpudirect` with a loopback test
4. [**Understanding the Configuration File**](tutorials/configuration-walkthrough.md) — annotated YAML walkthrough
