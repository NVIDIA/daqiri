---
hide:
  - navigation
---

# Bare-Metal CMake Build

This tutorial walks you through a complete bare-metal DAQIRI build on a Linux host, from verifying the kernel, driver, NIC, and CUDA prerequisites, through building a patched DPDK from source, configuring DAQIRI with CMake, installing the library, and recovering from the most common failure modes.

It is the long-form companion to the five-line `cmake` snippet in [Getting Started](../getting-started.md#build-the-daqiri-library).

!!! tip "Prefer the container build"

    The [container build](../getting-started.md#build-the-daqiri-library) ships a dmabuf-patched DPDK and all user-space dependencies pre-installed. It is the recommended path for most users. Use this bare-metal tutorial when:

    - your host distribution or CUDA stack is fixed and you cannot run a container;
    - you are packaging DAQIRI into another product that already provides a runtime image;
    - you are debugging a build problem inside the container's `daqiri-build` stage and need to reproduce it on the host.

    If none of those apply, follow [System Configuration](system_configuration.md) and then [Raw Ethernet Benchmarking](../benchmarks/raw_benchmarking.md) instead.

## Prerequisite verification

Before installing anything, run the checks below and resolve any failures. They all run as a regular user.

| Check | Command | Expected |
|---|---|---|
| NVIDIA driver + GPU | `nvidia-smi` | Driver version printed; one or more GPUs listed |
| CUDA Toolkit (11.7+) | `nvcc --version` | `Cuda compilation tools, release 11.7` or later |
| NIC kernel drivers | `lsmod \| grep ib_core` | `ib_core` listed |
| NIC user-space tools | `ibv_devinfo` | At least one `mlx5_*` HCA in `PORT_ACTIVE` state |
| ConnectX device on PCIe | `lspci -d 15b3:` | One or more Mellanox/NVIDIA networking entries |
| Kernel dma-buf support | `zgrep CONFIG_DMA_SHARED_BUFFER /proc/config.gz` | `=y` (built-in) |

??? failure "`nvidia-smi: command not found`"

    Install the NVIDIA driver. On Ubuntu the simplest path is `sudo ubuntu-drivers install`. The driver must be a recent branch with GPUDirect support; see the [system requirements table](../getting-started.md#system-requirements).

??? failure "`nvcc: command not found` (but `nvidia-smi` works)"

    The runtime driver is installed but the CUDA Toolkit is not. Add the CUDA APT repository for your distribution from [NVIDIA's CUDA downloads page](https://developer.nvidia.com/cuda-downloads?target_os=Linux) and `sudo apt install cuda-toolkit`. Then put `nvcc` on `PATH` (typically `/usr/local/cuda/bin`).

??? failure "`lsmod | grep ib_core` is empty"

    The NIC kernel drivers are not loaded. Follow [Check your NIC drivers](system_configuration.md#check-your-nic-drivers) in System Configuration to install `doca-ofed` (or the inbox `rdma-core` stack) and reboot.

??? failure "`ibv_devinfo` prints `libvmw_pvrdma-rdmav34.so` warning"

    `couldn't load driver 'libvmw_pvrdma-rdmav34.so'` is harmless. It refers to a VMware paravirtual RDMA userspace provider that is not relevant on bare-metal systems. As long as `ibv_devinfo` still lists your `mlx5_*` HCAs after the warning, you can ignore it. See [System Configuration → Warning about `libvmw_pvrdma-rdmav34.so`](system_configuration.md#check-your-nic-drivers) for more context.

??? failure "`/proc/config.gz` does not exist"

    Some kernels don't expose `/proc/config.gz` (notably the IGX/Tegra kernel). Try `grep CONFIG_DMA_SHARED_BUFFER /boot/config-$(uname -r)` instead. The expected value is `=y`. Without dma-buf support the patched DPDK built in [Step 3](#step-3-build-dpdk-with-daqiri-patches) cannot register GPU memory regions and DAQIRI will fall back to the legacy peermem path.

## Step 1: Configure the DOCA APT repository

DAQIRI's RDMA/ibverbs dependencies (`libibverbs-dev`, `librdmacm-dev`, `libmlx5-1`, `mlnx-ofed-kernel-utils`, `mft`) come from the DOCA repository. The exact apt setup commands for IGX OS 1.1, SBSA Ubuntu 22.04, and x86_64 Ubuntu 22.04 are in [Getting Started → Build the DAQIRI library](../getting-started.md#build-the-daqiri-library). Run the tab that matches your platform, then return here.

!!! note

    The container build uses DOCA `3.2.1` (see `DOCA_VERSION` in the [Dockerfile](https://github.com/NVIDIA/daqiri/blob/main/Dockerfile)). Earlier DOCA releases also work; the version pinning is for build reproducibility, not a strict requirement.

## Step 2: Install build tooling and RDMA libraries

The package list below mirrors the `base`, `base-deps`, and `dpdk` stages of the [Dockerfile](https://github.com/NVIDIA/daqiri/blob/main/Dockerfile) and is the minimum needed for the recipe that follows.

```bash
sudo apt update
sudo apt install -y --no-install-recommends \
    build-essential \
    git \
    curl \
    ca-certificates \
    gnupg \
    pkg-config \
    ninja-build \
    meson \
    python3-pyelftools \
    libnuma-dev \
    libibverbs-dev \
    librdmacm-dev \
    libmlx5-1 \
    ibverbs-utils \
    mlnx-ofed-kernel-utils \
    mft
```

!!! note "apt may select `ibverbs-providers` instead of `libmlx5-1`"

    On many host configurations (always on Ubuntu 24.04 with DOCA 3.2.1, where `libmlx5-1` has no DOCA candidate) apt resolves `libmlx5-1` to the `ibverbs-providers` virtual package (the inbox bundle of mlx5/mlx4 user-space providers). This is fine: `libmlx5.so.1` ends up installed either way and DAQIRI links against it through `libibverbs`. If you want the DOCA-provided build specifically, install `libmlx5-1` and `libmlx5-dev` after the DOCA repo from [Step 1](#step-1-configure-the-doca-apt-repository) is configured.

DAQIRI's top-level CMake requires version 3.20 or newer (see [CMakeLists.txt](https://github.com/NVIDIA/daqiri/blob/main/CMakeLists.txt)). On Ubuntu 22.04 the distribution package is too old; install a current build from the Kitware APT repository:

```bash
OS_CODENAME=$(. /etc/os-release && echo "$VERSION_CODENAME")
KITWARE_LINE="deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ ${OS_CODENAME} main"

curl -fsSL https://apt.kitware.com/keys/kitware-archive-latest.asc \
    | sudo gpg --dearmor -o /usr/share/keyrings/kitware-archive-keyring.gpg
echo "${KITWARE_LINE}" | sudo tee /etc/apt/sources.list.d/kitware.list

sudo apt update
sudo apt install -y --no-install-recommends kitware-archive-keyring cmake cmake-data
cmake --version   # expect >= 3.20
```

!!! tip "Copy/paste safety"

    The `KITWARE_LINE` shell variable above (and the `DISABLE_DRIVERS` variable in [Step 3.3](#33-configure-build-and-install)) keep long single-line strings off the terminal so a copy/paste that wraps a line break into the middle of the string does not silently truncate the argument. If you paste the literal `echo ...` or `meson setup ...` commands instead, verify the resulting file/command actually contains what you intended before continuing.

??? note "Optional: Python bindings"

    Skip this if you do not need `import daqiri` from Python. To build the pybind11 bindings later with `-DDAQIRI_BUILD_PYTHON=ON`:

    ```bash
    sudo apt install -y --no-install-recommends python3-dev pybind11-dev
    ```

??? note "Optional: GDS (cuFile) for device-memory file writes"

    Skip this unless you plan to enable `-DDAQIRI_ENABLE_GDS=ON`. The cuFile headers and `libcufile` are installed as part of the CUDA Toolkit (`cuda-toolkit-*` metapackage) and need no separate apt step. The runtime requirements (`nvidia-fs` kernel module, supported destination filesystem) are documented in [Getting Started](../getting-started.md#cmake-options).

## Step 3: Build DPDK with DAQIRI patches

DAQIRI requires DPDK with the two patches in [`dpdk_patches/`](https://github.com/NVIDIA/daqiri/blob/main/dpdk_patches): `dmabuf.patch` enables GPUDirect through the kernel dma-buf interface (so `nvidia-peermem` is **not** required), and `dpdk.nvidia.patch` adds the NVIDIA-specific changes consumed by the DPDK engine. The recipe below is the literal sequence run by the `dpdk` stage of the [Dockerfile](https://github.com/NVIDIA/daqiri/blob/main/Dockerfile).

### 3.1 Clone DAQIRI

You need the repository for `dpdk_patches/` regardless of whether you also build DAQIRI from this clone in [Step 4](#step-4-configure-daqiri-with-cmake).

```bash
git clone https://github.com/NVIDIA/daqiri.git
cd daqiri
```

### 3.2 Download and patch DPDK 25.11

```bash
DPDK_VERSION=25.11
DAQIRI_REPO_DIR=$PWD   # remember the daqiri clone path before we cd away

curl -fsSL https://fast.dpdk.org/rel/dpdk-${DPDK_VERSION}.tar.xz -o /tmp/dpdk-${DPDK_VERSION}.tar.xz
tar xf /tmp/dpdk-${DPDK_VERSION}.tar.xz -C /tmp
cd /tmp/dpdk-${DPDK_VERSION}

# `git apply` is used instead of `patch -p1` because:
#   1. `dmabuf.patch` carries `.mailmap` and release-notes hunks that conflict on
#      a stock tarball, and they must be excluded. GNU `patch` 2.7.x does not
#      accept `--exclude=`; only `git apply` does.
#   2. `git apply` does not actually require the working directory to be a git
#      repo, so it works on an extracted tarball as-is.
git apply \
    --exclude=.mailmap \
    --exclude='doc/guides/rel_notes/release_26_03.rst' \
    "${DAQIRI_REPO_DIR}/dpdk_patches/dmabuf.patch"
git apply "${DAQIRI_REPO_DIR}/dpdk_patches/dpdk.nvidia.patch"
```

### 3.3 Configure, build, and install

The `meson setup` flags below match the container build. The `disable_drivers` list trims drivers DAQIRI never uses, cutting build time and the installed footprint. It is kept in a shell variable so the long comma-separated string is not at risk of being truncated by terminal copy/paste line-wrapping.

```bash
DPDK_BUILD_DIR=/tmp/dpdk-build
DPDK_INSTALL_PREFIX=/usr/local

DISABLE_DRIVERS='baseband/*,bus/ifpga/*,common/cpt,common/dpaax,common/iavf,common/octeontx,common/octeontx2,crypto/nitrox,net/ark,net/atlantic,net/avp,net/axgbe,net/bnx2x,net/bnxt,net/cxgbe,net/e1000,net/ena,net/enic,net/fm10k,net/hinic,net/hns3,net/i40e,net/ixgbe,vdpa/ifc,net/igc,net/liquidio,net/mana,net/netvsc,net/nfp,net/qede,net/sfc,net/thunderx,net/vdev_netvsc,net/vmxnet3,regex/octeontx2'

meson setup ${DPDK_BUILD_DIR} \
    --prefix=${DPDK_INSTALL_PREFIX} \
    -Dtests=false \
    -Dplatform=generic \
    -Denable_docs=false \
    -Ddisable_drivers="${DISABLE_DRIVERS}"

cd ${DPDK_BUILD_DIR}
ninja
sudo meson install
sudo ldconfig
```

### 3.4 Verify

```bash
pkg-config --modversion libdpdk   # expect 25.11.x
pkg-config --libs libdpdk         # should print -lrte_eal -lrte_mbuf ... etc.
```

If `pkg-config` cannot find `libdpdk`, add the install location to its search path:

```bash
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/usr/local/lib/x86_64-linux-gnu/pkgconfig:${PKG_CONFIG_PATH:-}
```

(Replace `x86_64-linux-gnu` with `aarch64-linux-gnu` on ARM hosts.)

## Step 4: Configure DAQIRI with CMake

From the `daqiri` repository root (the clone you made in [Step 3.1](#31-clone-daqiri)):

```bash
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=ON \
    -DDAQIRI_ENGINE="dpdk ibverbs" \
    -DDAQIRI_BUILD_EXAMPLES=ON \
    -DDAQIRI_BUILD_PYTHON=OFF
```

!!! warning "Deactivate conda before configuring"

    If a conda environment (including `(base)`) is active, `find_package(yaml-cpp)` may silently pick up conda's vendored yaml-cpp 0.8 at configure time and then fail at link time with `cannot find -lyaml-cpp`. Run `conda deactivate` (or invoke CMake under `env -i PATH=/usr/bin:/bin /usr/bin/cmake ...`) before the configure step. The same `(base)` contamination also breaks `meson setup` for DPDK in [Step 3.3](#33-configure-build-and-install).

The sections below explain each option you can flip from the default, with explicit "when to use" guidance. The full reference is the [CMake Options table](../getting-started.md#cmake-options).

### `DAQIRI_ENGINE`: engine selection

`DAQIRI_ENGINE` is a space-separated list controlling which **optional** engines are compiled into `libdaqiri.so`. Linux UDP/TCP sockets are always built in, so the only values are `dpdk` and `ibverbs`. Three recipes cover most use cases:

| Recipe | What you get | When to use |
|---|---|---|
| `-DDAQIRI_ENGINE="dpdk"` | Raw Ethernet (DPDK) + built-in UDP/TCP sockets | No RoCE/RDMA; smallest NIC build |
| `-DDAQIRI_ENGINE="ibverbs"` | RDMA/RoCE (ibverbs) + built-in UDP/TCP sockets | RDMA-only; no raw Ethernet |
| `-DDAQIRI_ENGINE="dpdk ibverbs"` | Raw Ethernet + RDMA/RoCE + built-in UDP/TCP sockets | Recommended default; matches the container |

!!! note "Sockets are always available; `ibverbs` backs RoCE"

    The socket engine (Linux UDP/TCP) is always built, so it never appears in `DAQIRI_ENGINE`. Its RoCE path delegates to the `ibverbs` engine, so `roce://` endpoints work only when `ibverbs` is in the list. Internally the ibverbs engine lives under [`src/engines/rdma`](https://github.com/NVIDIA/daqiri/blob/main/src/engines/rdma) (target `daqiri_rdma`); `ibverbs` is just the user-facing name, chosen so an alternative RDMA engine (such as DOCA) could be added later.

### `DAQIRI_BUILD_PYTHON`: pybind11 bindings

`-DDAQIRI_BUILD_PYTHON=ON` builds the `daqiri` Python module from `python/`. It requires the `python3-dev` and `pybind11-dev` packages from the optional step in [Step 2](#step-2-install-build-tooling-and-rdma-libraries). Leave it `OFF` (the default) if you only consume DAQIRI from C++.

### `DAQIRI_ENABLE_GDS`: cuFile burst writes

`-DDAQIRI_ENABLE_GDS=ON` enables the `cuFile`-backed file-write path for bursts whose payload lives in CUDA device memory. The build requires `cufile.h` and `libcufile` (both shipped with the CUDA Toolkit). At runtime you also need the `nvidia-fs` kernel module loaded and a GDS-supported destination filesystem; verify with the snippet in [Getting Started](../getting-started.md#cmake-options). Without this flag, device-memory burst writes return `NOT_SUPPORTED`; host-memory writes are unaffected.

### `CMAKE_CUDA_ARCHITECTURES`: GPU compute capability

The default CUDA architectures DAQIRI compiles for are `80;90` (A100, H100), with `121` (GB10) appended automatically when the configuring CUDA compiler is 13.0 or newer. The logic lives in [`src/CMakeLists.txt`](https://github.com/NVIDIA/daqiri/blob/main/src/CMakeLists.txt) (search for `CMAKE_CUDA_ARCHITECTURES`).

If your GPU is a different generation, override it on the CMake command line:

```bash
# Example: Ada only (sm_89, e.g. RTX 6000 Ada)
cmake -S . -B build ... -DCMAKE_CUDA_ARCHITECTURES=89

# Example: A100 + Ada + Hopper, skip GB10
cmake -S . -B build ... -DCMAKE_CUDA_ARCHITECTURES="80;89;90"

# Example: RTX PRO 6000 Blackwell Server/Workstation Edition only (sm_120)
cmake -S . -B build ... -DCMAKE_CUDA_ARCHITECTURES=120
```

The override is honored because `src/CMakeLists.txt` only sets `CMAKE_CUDA_ARCHITECTURES` when it is not already defined; a user-supplied `-D` value takes precedence. Common values:

| GPU family | Architecture |
|---|---|
| A100 | `80` |
| RTX 30xx, A40 | `86` |
| RTX 40xx, RTX 6000 Ada, L40 | `89` |
| H100, H200 | `90` |
| Blackwell datacenter (B100, B200, GB100, GB200) | `100` |
| Blackwell consumer / workstation (RTX PRO 6000 Blackwell, RTX 5090) | `120` |
| GB10 (DGX Spark) | `121` |

!!! warning "Default list omits sm_86, sm_89, sm_100, and sm_120"

    If your GPU is Ampere (RTX 30xx, A40), Ada (RTX 40xx, RTX 6000 Ada), datacenter Blackwell (B100, B200, GB100, GB200), or workstation Blackwell (RTX PRO 6000 Blackwell, RTX 5090), the default build will not contain native code for your card. The binary may still load via PTX JIT for some architectures, but for production use pass the matching `sm_*` value via `-DCMAKE_CUDA_ARCHITECTURES=...`.

!!! warning "`sm_121` requires a recent CUDA Toolkit"

    `sm_121` (GB10 / DGX Spark) is only known to CUDA Toolkit 13.0 and newer. On CUDA 12.x the automatic append is suppressed, but if you explicitly pass `-DCMAKE_CUDA_ARCHITECTURES="...;121"` on an older toolkit you will get `nvcc fatal: Unsupported gpu architecture 'compute_121'`. Drop the `121` entry or upgrade the toolkit.

### Other flags

- `-DBUILD_SHARED_LIBS=ON`: produces `libdaqiri.so` (recommended). With `OFF`, you get a static library.
- `-DDAQIRI_BUILD_EXAMPLES=ON`: builds the `daqiri_bench_*` executables under `build/examples/`. Required for the smoke test in [Step 5.3](#53-smoke-test). On by default.
- `-DDAQIRI_REORDER_GPU_PROFILE=ON`: instruments the CUDA reorder kernels with CUDA event timing. Off by default; turn on only when profiling.

## Step 5: Build, install, and verify

### 5.1 Build

```bash
cmake --build build -j"$(nproc)"
```

A clean build on a modern workstation takes a few minutes; the slowest single translation unit is `src/kernels.cu`.

### 5.2 Install

Install to a prefix of your choice. `/opt/daqiri` matches both the container and the [Getting Started](../getting-started.md) recipe:

```bash
sudo cmake --install build --prefix /opt/daqiri
```

Confirm the expected artifacts are in place:

```bash
ls /opt/daqiri/include/daqiri/daqiri.h
ls /opt/daqiri/lib/libdaqiri.so
ls /opt/daqiri/lib/cmake/daqiri/
ls /opt/daqiri/lib/pkgconfig/daqiri.pc
ldd /opt/daqiri/lib/libdaqiri.so | head
```

`ldd` should show `librte_*` (DPDK), `libibverbs.so`, `librdmacm.so`, `libcudart.so`, and `libyaml-cpp.so` resolving. Unresolved entries usually mean either DPDK was installed to a prefix not on the dynamic loader path (add it to `/etc/ld.so.conf.d/` and re-run `sudo ldconfig`), or the RDMA libraries from [Step 2](#step-2-install-build-tooling-and-rdma-libraries) were not actually installed.

### 5.3 Smoke test

Verify the build with the standard two-port TX/RX loopback. This requires a NIC with two ports connected to each other by a physical SFP cable, and that you replace the `<angle-bracket>` placeholders in the YAML (PCIe BDFs, CPU cores, destination MAC) for your system. The walkthrough for those edits lives in [Raw Ethernet Benchmarking → Update the loopback configuration](../benchmarks/raw_benchmarking.md#update-the-loopback-configuration); do that first, then run:

```bash
sudo ./build/examples/daqiri_bench_raw_gpudirect \
    ./build/examples/daqiri_bench_raw_tx_rx.yaml \
    --seconds 5
```

A successful run prints a stream of `[INFO]` lines followed by an RX/TX rate summary with non-zero packet counts on both sides. If the program aborts immediately with `EAL: No free hugepages reported`, see [Step 6: Troubleshooting](#step-6-troubleshooting) below.

!!! tip "DGX Spark"

    On DGX Spark, use the prefilled `daqiri_bench_raw_tx_rx_spark.yaml` instead; only `eth_dst_addr` needs an edit. See the [DGX Spark profile callout](../benchmarks/raw_benchmarking.md#update-the-loopback-configuration) for the exact MAC-lookup command.

!!! note "No NIC available?"

    If you don't have a NIC at all and just want to verify the binary starts cleanly, you can substitute [`daqiri_bench_raw_sw_loopback.yaml`](https://github.com/NVIDIA/daqiri/blob/main/examples/daqiri_bench_raw_sw_loopback.yaml). It runs an in-process TX/RX through DPDK's software loopback with no hardware required. It is **not** representative of production performance and should not replace the real loopback above for build acceptance.

## Step 6: Troubleshooting

??? failure "CMake: `Could not find a package configuration file provided by ... libdpdk`"

    `pkg-config` cannot locate the DPDK installation. Either point it at the prefix you used in [Step 3.3](#33-configure-build-and-install):

    ```bash
    export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/usr/local/lib/x86_64-linux-gnu/pkgconfig
    ```

    or, if you installed DPDK from MLNX_OFED to `/opt/mellanox/dpdk`, rely on the fallback in [`src/CMakeLists.txt`](https://github.com/NVIDIA/daqiri/blob/main/src/CMakeLists.txt) (search for `mellanox`); no extra environment variable is needed, but the path must exist.

??? failure "CMake: `Could not find libibverbs` / `librdmacm`"

    The RDMA development headers are missing. Re-run the apt install from [Step 2](#step-2-install-build-tooling-and-rdma-libraries):

    ```bash
    sudo apt install -y libibverbs-dev librdmacm-dev libmlx5-1
    ```

    If apt reports the packages are "installed" but the `-dev` symlinks are missing on disk (this happens on some base images such as `nvcr.io/nvidia/pytorch`), force a reinstall from the DOCA repository configured in [Step 1](#step-1-configure-the-doca-apt-repository):

    ```bash
    sudo apt install --reinstall libibverbs-dev librdmacm-dev libmlx5-1
    ```

??? failure "`nvcc fatal: Unsupported gpu architecture 'compute_121'`"

    Your CUDA Toolkit predates GB10 / Blackwell support but `sm_121` ended up in the architecture list. Either omit `121` from `CMAKE_CUDA_ARCHITECTURES` or pin an explicit list that matches your GPU:

    ```bash
    cmake -S . -B build ... -DCMAKE_CUDA_ARCHITECTURES="80;90"
    ```

    The default in `src/CMakeLists.txt` already suppresses the `121` auto-append on CUDA < 13.0, so this error generally only appears when the override was provided explicitly. See the [GPU compute capability table](#cmake_cuda_architectures-gpu-compute-capability) for valid values.

??? failure "Runtime: `EAL: Cannot get hugepage information` or `EAL: No free hugepages reported`"

    DPDK could not allocate hugepages because none are reserved (`Cannot get hugepage information` when `nr_hugepages` is zero, or `No free hugepages reported` when the reserved pool is exhausted). Hugepage configuration is part of system setup, not the build. Follow [System Configuration → Step 4: Enable Huge pages](system_configuration.md#step-4-enable-huge-pages) to add a persistent reservation, or reserve some temporarily with:

    ```bash
    echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
    ```

??? failure "Runtime: `nvidia-fs not loaded` (with `DAQIRI_ENABLE_GDS=ON`)"

    The cuFile path needs the `nvidia-fs` kernel module loaded and a GDS-supported destination filesystem. Verify with:

    ```bash
    lsmod | grep nvidia_fs
    /usr/local/cuda/gds/tools/gdscheck.py -p
    ```

    Full setup notes are in [Getting Started → CMake Options](../getting-started.md#cmake-options). If `nvidia-fs` cannot be loaded on this host, drop `-DDAQIRI_ENABLE_GDS=ON` from the CMake configure step; host-memory burst writes do not require it.

??? failure "`pkg-config: command not found`"

    ```bash
    sudo apt install -y pkgconf
    ```

??? failure "DPDK `meson setup`: `ERROR: Dependency \"libnuma\" not found`"

    The Dockerfile installs `libnuma-dev` in the same apt batch as the DPDK build tooling. If you skipped it:

    ```bash
    sudo apt install -y libnuma-dev
    ```

??? failure "Build succeeds but `ibv_devinfo` reports `0 HCAs found`"

    The user-space libraries are installed but the kernel drivers are not; `lsmod | grep mlx5` will be empty. Follow [Check your NIC drivers](system_configuration.md#check-your-nic-drivers) to install `doca-ofed` and reboot.

## Platform-specific notes

The build recipe above is the same on every supported host. The notes below cover the few places where defaults or expectations diverge. Open the tab that matches your hardware.

=== "DGX Spark (GB10)"

    - The integrated **ConnectX-7** appears in `ibv_devinfo` as one or two `mlx5_*` HCAs depending on link configuration; no separate driver install beyond the [DOCA repository setup](#step-1-configure-the-doca-apt-repository) is needed.
    - GB10 is **compute capability 12.1** (`sm_121`). DAQIRI's default arch list adds `121` automatically when configuring with **CUDA Toolkit 13.0 or newer**; on those toolkits no override is needed. On older toolkits, GB10 is not supported.
    - DGX Spark uses **NVLink-C2C unified memory** and has no separate GPU BAR1, so data buffers in YAML configs use `kind: host_pinned` rather than `kind: device`. The DGX-Spark-prefilled YAMLs in `examples/*_spark.yaml` already encode this.
    - `nvidia-peermem` is not used; GPUDirect goes through the dma-buf path enabled by the DPDK patches in [Step 3](#step-3-build-dpdk-with-daqiri-patches).
    - For a runnable end-to-end test after the build completes, follow the [DGX Spark profile callout](../benchmarks/raw_benchmarking.md#update-the-loopback-configuration) in Raw Ethernet Benchmarking: the prefilled `daqiri_bench_raw_tx_rx_spark.yaml` and `daqiri_bench_rdma_tx_rx_spark.yaml` need only an `eth_dst_addr` edit.

=== "IGX Orin + dGPU"

    - The reference dGPU is the **RTX 6000 Ada** (compute capability **8.9**), which is not in the default arch list. Pass `-DCMAKE_CUDA_ARCHITECTURES=89` on the CMake configure step:

        ```bash
        cmake -S . -B build ... -DCMAKE_CUDA_ARCHITECTURES=89
        ```

        Use a different value (or extra entries) if you have a different dGPU installed.
    - On IGX Orin, several system settings (BAR1 size, MRRS, NIC link layer) need to be applied **before** DAQIRI will run end-to-end. Follow the [IGX Orin tab in System Configuration](system_configuration.md) for the full sequence; the build itself does not require those changes.
    - GPUDirect can use either the patched DPDK's dma-buf path (recommended) or the legacy `nvidia-peermem` module. The patched DPDK built in [Step 3](#step-3-build-dpdk-with-daqiri-patches) removes the peermem dependency.
    - For ARM64 (`aarch64`) hosts, set `PKG_CONFIG_PATH` to the aarch64 directory:

        ```bash
        export PKG_CONFIG_PATH=/usr/local/lib/aarch64-linux-gnu/pkgconfig:${PKG_CONFIG_PATH:-}
        ```

    - The IGX/Tegra kernel does not expose `/proc/config.gz`; use the `/boot/config-$(uname -r)` fallback shown in [Prerequisite verification](#prerequisite-verification) to confirm `CONFIG_DMA_SHARED_BUFFER=y`.

=== "x86_64 RTX Pro Server"

    - "RTX Pro Server" covers any `x86_64` workstation or server with a ConnectX-6 Dx (or later) NIC and an RTX Pro / Workstation GPU. Confirm `nvidia-smi` reports a GPUDirect-capable GPU (any RTX Pro / Quadro / Data Center class card; **not** GeForce; see the warning in [Concepts → GPUDirect](../concepts.md#gpudirect)).
    - Set `-DCMAKE_CUDA_ARCHITECTURES` to match the installed card. RTX PRO 6000 Blackwell Server/Workstation Edition is `120` (workstation Blackwell, `sm_120`); RTX 6000 Ada is `89`; RTX A6000 is `86`. Confirm with `nvidia-smi --query-gpu=compute_cap --format=csv,noheader` (e.g. `12.0` → `120`):

        ```bash
        cmake -S . -B build ... -DCMAKE_CUDA_ARCHITECTURES=120
        ```

    - x86_64 hosts use `/usr/local/lib/x86_64-linux-gnu/pkgconfig` for the DPDK `.pc` file; that's the default `PKG_CONFIG_PATH` entry shown in [Step 3.4](#34-verify).
    - All other steps are identical to the generic recipe above.

## Next steps

Once `libdaqiri.so` is installed and the [smoke test](#53-smoke-test) passes:

1. [**System Configuration**](system_configuration.md): tune the host (hugepages, NIC link layer, GPU BAR1, CPU isolation) for production performance.
2. [**Raw Ethernet Benchmarking**](../benchmarks/raw_benchmarking.md): run `daqiri_bench_raw_gpudirect` over a physical loopback.
3. [**Understanding the Configuration File**](configuration-walkthrough.md): pick the right starter YAML for your use case from the decision tree.

---

## Cleanup

To remove the CMake install while keeping every prerequisite (DPDK, DOCA libraries, CUDA, hugepages, NIC drivers) in place, run [`scripts/cleanup.sh`](https://github.com/NVIDIA/daqiri/blob/main/scripts/cleanup.sh) with the `cmake` target:

```bash
scripts/cleanup.sh cmake             # interactive, with manifest preview
scripts/cleanup.sh cmake --dry-run   # show what would be removed
scripts/cleanup.sh cmake --yes       # non-interactive
```

The script reads `build/install_manifest.txt` (written by [Step 5.2](#52-install)) for the canonical list of installed paths, then refuses to remove anything unless every manifest entry is under `DAQIRI_PREFIX`. Manifest entries that are already absent are reported and skipped so cleanup can be rerun after a partial removal; verification still runs and decides the final exit status. Override the install prefix with `DAQIRI_PREFIX=...` if you installed somewhere other than `/opt/daqiri`, or `BUILD_DIR=...` if your build tree is named something other than `build`. When the manifest is missing, the script falls back to a name-scoped scan that auto-removes only DAQIRI-owned artifacts and flags vendored `spdlog/`, `yaml-cpp/`, and `libyaml-cpp.so*` for manual review. The final step runs verification (`ls /opt/daqiri`, `pkg-config --modversion daqiri`, `ldconfig -p | grep daqiri`) and exits non-zero if any DAQIRI artifact is still present.

Pass `all` instead of `cmake` to also remove the container image (`docker image rm "$IMAGE_TAG"`) in the same run.
