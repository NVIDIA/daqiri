---
hide:
  - navigation
---

# Getting Started

## System Requirements

DAQIRI's baseline requirements depend on which [stream type](concepts.md#stream-types) you plan to use. The Linux Sockets path (`stream_type: "socket"` with `udp://` or `tcp://` endpoints) runs on any modern Linux box. Raw Ethernet, RoCE, and PCIe GPUDirect impose additional hardware requirements, listed below.

| Component | Requirement |
|-----------|-------------|
| **OS** | Linux (kernel 5.4+), Ubuntu 22.04 recommended |
| **CUDA** | CUDA Toolkit 12.2+ generally (the container ships CUDA 13.1); PCIe support requires CUDA Toolkit 12.8+ for the explicit PCIe DMA-BUF mapping flag. |
| **NIC** *(Raw Ethernet / GPUDirect / RoCE only)* | NVIDIA ConnectX-6 Dx or later. Default Ubuntu kernel drivers (inbox) are sufficient; we recommend also installing `doca-ofed` for the diagnostic utilities (`ibstat`, `ibv_devinfo`, `ibdev2netdev`, `mlnx_perf`, `mlxconfig`, …). |
| **GPU** *(GPUDirect only)* | RTX or Data Center GPU. GeForce is not supported. |
| **DPDK** | Included in the DAQIRI container (patched for dma-buf, so `nvidia-peermem` is **not required** inside the container); see [bare-metal dependencies](#bare-metal-dependencies) below for the host build. |
| **RoCE** | `libibverbs` and `librdmacm` (for `stream_type: "socket"` and `roce://` endpoints). |
| **PCIe FPGA** | A discrete GPUDirect-capable GPU and peer-capable PCIe topology. Software loopback needs no FPGA; hardware operation additionally needs a board-specific DAQIRI character driver and FPGA implementation of the completion ABI. |
| **GDS** | Optional `cufile.h` and `libcufile` for file writes from CUDA device memory. Runtime device-memory writes require a working cuFile installation; for regular `nvidia-fs` mode, the `nvidia-fs` kernel module must be loaded and the destination storage stack must be supported. |
| **S3** | Optional AWS SDK for C++ with the `s3` component for raw packet uploads to Amazon S3 or S3-compatible object stores. The DAQIRI container builds this SDK from source. |

Supported platforms include [NVIDIA Data Center](https://www.nvidia.com/en-us/data-center/) systems, edge systems like [NVIDIA IGX](https://www.nvidia.com/en-us/edge-computing/products/igx/) and [NVIDIA DGX Spark](https://www.nvidia.com/en-us/products/workstations/dgx-spark/), and `x86_64` systems with the above components.

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

=== "x86_64 (Ubuntu 24.04)"

    ```bash
    export DOCA_URL="https://linux.mellanox.com/public/repo/doca/3.2.1/ubuntu24.04/x86_64/"
    wget -qO- https://linux.mellanox.com/public/repo/doca/GPG-KEY-Mellanox.pub | gpg --dearmor - | sudo tee /etc/apt/trusted.gpg.d/GPG-KEY-Mellanox.pub > /dev/null
    echo "deb [signed-by=/etc/apt/trusted.gpg.d/GPG-KEY-Mellanox.pub] $DOCA_URL ./"  | sudo tee /etc/apt/sources.list.d/doca.list > /dev/null

    # Also need the CUDA repository: https://developer.nvidia.com/cuda-downloads?target_os=Linux
    wget https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/cuda-keyring_1.1-1_all.deb
    sudo dpkg -i cuda-keyring_1.1-1_all.deb

    sudo apt update
    ```

    DOCA `3.2.1` matches the `Dockerfile`'s `DOCA_VERSION`, so the bare-metal recipe stays in lockstep with the container build. Earlier DOCA releases that publish an `ubuntu24.04/x86_64` directory also work.

Then build the DAQIRI library:

### Container build {#container-build}

=== "Container build (recommended)"

    The container bundles all user-space libraries for each stream type, avoiding dependency issues on the host:

    ```bash
    git clone git@github.com:NVIDIA/daqiri.git
    cd daqiri
    BASE_TARGET=dpdk DAQIRI_ENGINE="dpdk ibverbs" scripts/build-container.sh
    ```

    For a PCIe-only image with no optional Ethernet engines:

    ```bash
    BASE_TARGET=base-deps DAQIRI_ENABLE_PCIE=ON DAQIRI_ENGINE="" \
      scripts/build-container.sh
    ```

    Set `BASE_IMAGE=torch` to build on top of NGC PyTorch instead of the default CUDA base — useful for Torch / TensorRT inference workflows that ingest packets directly into GPU memory:

    ```bash
    BASE_IMAGE=torch BASE_TARGET=dpdk DAQIRI_ENGINE="dpdk ibverbs" scripts/build-container.sh
    ```

    OpenTelemetry metrics are optional. Enable them with:

    ```bash
    DAQIRI_ENABLE_OTEL_METRICS=ON BASE_TARGET=dpdk DAQIRI_ENGINE="dpdk ibverbs" scripts/build-container.sh
    ```

=== "CMake build (bare-metal)"

    Install the dependencies listed under [Bare-metal dependencies](#bare-metal-dependencies) below first, then:

    ```bash
    git clone git@github.com:NVIDIA/daqiri.git
    cd daqiri
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DDAQIRI_BUILD_PYTHON=OFF -DDAQIRI_ENGINE="dpdk ibverbs"
    cmake --build build -j
    cmake --install build --prefix /opt/daqiri
    ```

### Bare-metal dependencies

The Ubuntu apt packages mirror the Dockerfile. Build DPDK from source with the patches under `dpdk_patches/` if you want GPUDirect without the `nvidia-peermem` kernel module.

```bash
# Core build deps
sudo apt install -y \
    build-essential cmake git curl ca-certificates gnupg \
    pkgconf ninja-build meson python3-pip python3-dev python3-pyelftools

# Raw Ethernet (DPDK) build deps
sudo apt install -y libnuma-dev

# RoCE / RDMA + diagnostic utilities (from the DOCA APT repo, see above)
sudo apt install -y \
    libibverbs-dev librdmacm-dev libmlx5-1 ibverbs-utils infiniband-diags \
    mlnx-ofed-kernel-utils mft

# Python bindings (only if -DDAQIRI_BUILD_PYTHON=ON)
sudo apt install -y pybind11-dev
```

### Cleanup

To remove DAQIRI's container image or bare-metal install without touching the build prerequisites (DPDK, DOCA libraries, CUDA, hugepages, NIC drivers), use [`scripts/cleanup.sh`](https://github.com/NVIDIA/daqiri/blob/main/scripts/cleanup.sh):

=== "Container"

    ```bash
    scripts/cleanup.sh container             # interactive
    scripts/cleanup.sh container --dry-run   # show what would be removed
    ```

    Override `IMAGE_TAG=` if you built with a non-default tag.

=== "CMake build (bare-metal)"

    ```bash
    scripts/cleanup.sh cmake             # interactive, manifest-driven
    scripts/cleanup.sh cmake --dry-run   # show what would be removed
    scripts/cleanup.sh cmake --yes       # non-interactive
    ```

    See [Cleanup](tutorials/bare-metal-cmake-build.md#cleanup) in the bare-metal tutorial for manifest semantics, the `DAQIRI_PREFIX` override, and verification details.

Pass `all` instead of `container` or `cmake` to remove both.

### Use an Installed Library

After installation, CMake consumers can link against the exported target:

```cmake
find_package(daqiri REQUIRED)
target_link_libraries(my_app PRIVATE daqiri::daqiri)
```

DAQIRI uses CalVer package versions in `YYYY.MM.PATCH` form. Consumers that need
a minimum DAQIRI release can request it from CMake:

```cmake
find_package(daqiri 2026.7.0 REQUIRED)
```

Pkg-config consumers can use the installed `daqiri.pc` file:

```bash
c++ my_app.cpp -o my_app $(pkg-config --cflags --libs daqiri)
pkg-config --modversion daqiri
```

Both methods use the same public C++ include:

```cpp
#include <daqiri/daqiri.h>
```

`daqiri/version.h` is included by `daqiri/daqiri.h` and provides
`DAQIRI_VERSION`, `daqiri::version_string()`, and related CalVer helpers.
DAQIRI's shared-library ABI version is tracked separately through
`DAQIRI_ABI_VERSION` / `daqiri::abi_version()`.

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `DAQIRI_ENGINE` | `"dpdk ibverbs"` | Space-separated list of optional engine implementations to compile in. Valid values: `dpdk` (Raw Ethernet) and `ibverbs`. `ibverbs` builds two libibverbs-based engines: RDMA/RoCE (for `stream_type: "socket"` with `roce://` endpoints) and a Mellanox/mlx5 Multi-Packet (striding) Receive Queue engine for `stream_type: "raw"` (opt in per stream with `engine: "ibverbs"`). Linux UDP/TCP sockets are always built in, so there is no `socket` value. |
| `DAQIRI_ENABLE_PCIE` | `OFF` | Build `stream_type: "pcie"`, its DMA-BUF provider, and `daqiri_bench_pcie`. This is independent of `DAQIRI_ENGINE` and requires CUDA Toolkit 12.8+. |
| `DAQIRI_BUILD_PYTHON` | `OFF` | Build pybind11 Python bindings. |
| `DAQIRI_BUILD_EXAMPLES` | `ON` | Build benchmark executables. |
| `DAQIRI_ENABLE_GDS` | `OFF` | Enable cuFile-backed burst file writes from CUDA device memory. Host-memory writes use POSIX APIs without GDS. |
| `DAQIRI_ENABLE_OTEL_METRICS` | `OFF` | Enable OpenTelemetry C++ metrics instrumentation. When enabled, OpenTelemetry C++ API package metadata must be available to CMake. |
| `DAQIRI_ENABLE_S3` | `OFF` | Enable AWS SDK-backed asynchronous raw packet writes to S3. |
| `DAQIRI_PREFER_SYSTEM_YAML_CPP` | `OFF` | Prefer a system-installed `yaml-cpp` over the vendored `third_party/yaml-cpp` submodule. Keep `OFF` if a conda/miniforge env is on `PATH`. |
| `BUILD_SHARED_LIBS` | — | Build as shared library. |

Linux UDP/TCP sockets are always available. Applications that need kernel socket
tuning can call `socket_setsockopt()` after resolving a TCP/UDP connection ID,
passing the numeric `level` and option constants from system headers; DAQIRI does
not maintain symbolic socket-option mappings in YAML.

A minimal PCIe-only build uses no optional Ethernet engines:

```bash
cmake -S . -B build \
  -DDAQIRI_ENABLE_PCIE=ON \
  -DDAQIRI_ENGINE="" \
  -DDAQIRI_BUILD_PYTHON=ON
cmake --build build -j
```

Run its self-contained protocol smoke test with
`daqiri_bench_pcie daqiri_bench_pcie_sw_loopback.yaml --seconds 10 --mode both`.
DMA-BUF is selected internally; do not add it to `DAQIRI_ENGINE` or an
`engine:` configuration field.

For Raw Ethernet (`stream_type: "raw"`), `daqiri_init()` validates that each `rx.flows`
entry's legacy `action.id` or final ordered `actions:` queue action matches an
`rx.queues` ID on the same interface, then programs flow rules into the NIC.
Initialization fails if any RX flow rule, TX transform flow, send-to-kernel fallback
(when `flow_isolation: true`), or `tx_eth_src` offload rule cannot be installed.
Raw DPDK and raw ibverbs can offload VLAN push/pop and VXLAN, GRE, or NVGRE
encap/decap through flow actions; socket/RDMA streams reject those actions.
`rx.flows` may also be omitted for queues-only startup; applications can then add and delete
RX flow rules at runtime with `add_rx_flow_async()` / `delete_flow_async()`. Dynamic
RX flows can use the same decap/pop action ordering as static RX flows. The DPDK template
fast path is enabled and sized by `rx.dynamic_flow_capacity` (default `0`, set a positive
value such as `1024` to create template tables); dynamic transform rules fall back to
regular hardware flow creation because packet reformat actions are not part of that
template fast path.

CUDA architectures default to `80;90` (A100, H100), with `121` (GB10) added
when configuring with CUDA Toolkit 13.0 or newer. Override
`CMAKE_CUDA_ARCHITECTURES` when targeting other GPUs.

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

OpenTelemetry metrics builds register observable counters for received packets,
transmitted packets, received bytes, transmitted bytes, and dropped packets. DAQIRI
does not configure an SDK reader or exporter; applications that want exported data
must configure the OpenTelemetry C++ SDK before or during DAQIRI initialization.

When using `DAQIRI_ENABLE_S3=ON`, the container build installs AWS SDK for C++
with S3 support. Bare-metal builds must provide `aws-cpp-sdk-core` and
`aws-cpp-sdk-s3` so CMake can resolve `find_package(AWSSDK COMPONENTS s3)`.
Configure credentials through the AWS SDK provider chain, such as environment
variables, a shared AWS profile, container credentials, or an EC2 instance role.
DAQIRI writes one object per packet with a single `PutObject`; multipart uploads
and PCAP output are not part of the S3 path.

### Raw Ethernet RX flows

On a single RX interface, use either standard UDP/IP flow rules or flex-item flow
rules, not both. Mixed configs are rejected at `daqiri_init`. See
[Configuration reference](api-reference/configuration.md#flows).

## Next Steps

Once DAQIRI is built, follow the tutorials to configure your system and run your first benchmark:

1. [**Concepts**](concepts.md) — terminology (stream types, engines, endpoint URI schemes, packet, burst, segment, flow, queue, memory region), GPUDirect, and zero-copy ownership. Keep this open in a second tab.
2. [**API Guide**](api-reference/index.md) — the six-step DAQIRI application lifecycle and configuration-first model
3. [**System Configuration**](tutorials/system_configuration.md) — NIC drivers, link layers, GPUDirect, hugepages, CPU isolation, GPU clocks, and more
4. [**Benchmarking**](benchmarks/benchmarks.md) — choose a stream type, then run its PCIe, socket/RDMA, or raw Ethernet benchmark
5. [**Understanding the Configuration File**](tutorials/configuration-walkthrough.md) — annotated YAML walkthrough
