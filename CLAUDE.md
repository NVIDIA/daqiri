# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & run

```bash
# Configure and build (both backends)
cmake -S . -B build -DBUILD_SHARED_LIBS=ON -DDAQIRI_BUILD_PYTHON=OFF -DDAQIRI_MGR="dpdk rdma"
cmake --build build -j
cmake --install build --prefix /opt/daqiri

# Container build (compiles patched DPDK from source)
BASE_TARGET=dpdk DAQIRI_MGR="dpdk rdma" scripts/build-container.sh
```

CMake options (see `docs/getting-started.md` for the full table):
- `DAQIRI_MGR` — space-separated backend list. Valid values: `dpdk`, `rdma`.
- `DAQIRI_BUILD_PYTHON` — builds `pybind11` bindings from `python/`.
- `DAQIRI_BUILD_EXAMPLES` — builds the two benchmark executables.

CUDA architectures are hardcoded to `80;90` (A100, H100) in `src/CMakeLists.txt:25`. Change this when targeting other GPUs.

## Benchmarks (the "tests")

There is no unit test suite. Verification is done via the benchmark executables in `examples/`, driven by YAML configs:

```bash
./build/examples/daqiri_bench_default examples/daqiri_bench_default_tx_rx.yaml
./build/examples/daqiri_bench_rdma    examples/daqiri_bench_rdma_tx_rx.yaml
```

Example YAML files contain `<angle-bracket>` placeholders (PCIe addresses, CPU cores, IPs) that **must** be replaced for your system. `daqiri_bench_default_sw_loopback.yaml` requires no physical link and is the fastest way to smoke-test a build.

## Formatting

`clang-format` is required for contributions (CONTRIBUTING.md):

```bash
git-clang-format --style file              # format staged changes
clang-format -style=file -i -fallback-style=none <files>
```

## Architecture

**Single C++/CUDA shared library** (`libdaqiri.so`) exposing a C++ API declared in `src/common.h`. The public surface is intentionally flat free-function helpers (`get_rx_burst`, `get_packet_ptr`, `set_udp_header`, …) that all operate on an opaque `BurstParams*`. Applications never touch backend types directly.

### Manager abstraction
`src/manager.h` defines `daqiri::Manager` — an (almost) ABC with ~50 virtual methods covering init, RX/TX burst dequeue/enqueue, header-fill helpers, buffer free, and RDMA connection setup. Backends live in `src/managers/<name>/` and are selected at CMake configure time via `DAQIRI_MGR`. Each backend produces its own static library (`daqiri_dpdk`, `daqiri_rdma`) linked into `daqiri_common`, and each adds a `DAQIRI_MGR_<NAME>=1` compile definition.

`ManagerFactory` (also in `manager.h`) is a singleton that instantiates the active backend. `daqiri_init(...)` resolves which backend to use from the `NetworkConfig` and then delegates everything through the `Manager` vtable. There is only ever **one** active `Manager` per process.

### Zero-copy / BurstParams
All packet data flows through `BurstParams`, a batch of packets. Only pointers are passed between NIC, DAQIRI internals, and the application — the caller reads directly from the buffers the NIC DMA'd into. **The caller must explicitly free bursts**; a missed free drains the pool and produces `NO_FREE_BURST_BUFFERS` / `NO_FREE_PACKET_BUFFERS` errors and NIC drops. See `docs/api-guide.md`.

### Segments & HDS
A single packet can span multiple **segments** (contiguous memory regions), each in CPU or GPU memory. The header-data split (HDS) mode puts headers in segment 0 (CPU) and payload in segment 1 (GPU), enabling GPUDirect zero-copy payload paths. Batched-GPU and CPU-only modes use a single segment.

### DPDK is load-bearing for the common library
`src/manager.h` and `src/common.cpp` use `rte_ring` / `rte_mbuf` directly, so **DPDK is a build dependency of `daqiri_common` even when building only the RDMA backend**. The container build uses patched DPDK from `dpdk_patches/` (`dmabuf.patch`, `dpdk.nvidia.patch`) — the `dmabuf` patch removes the peermem kernel-module requirement for GPUDirect.

### Third-party dependencies
Vendored under `third_party/` as submodules (`.gitmodules`): `yaml-cpp` (config parser) and `spdlog` (logging). CMake prefers these over system copies. Missing them is a fatal error.

### Current limitations
- Only UDP fill mode is supported for TX (see README).
- No CI yet — contributors and reviewers verify manually (CONTRIBUTING.md).

## Contribution rules

From `CONTRIBUTING.md`:
- **DCO sign-off required** — every commit must have `Signed-off-by:` (use `git commit -s`). Unsigned commits will be rejected.
- Commit titles in imperative mood, prefixed with the GitHub issue number: `#<Issue Number> - <Title>`.
- An issue must exist and be approved before coding.
- Prefer toggling features via new CMake options (with backward-compatible defaults) rather than wrapping entire files in `#if` guards. Use `#if` only for minor in-file changes.
- Keep PRs narrowly scoped — one concern per PR, dependencies noted in the description.

## Compiling and Running

Compiling should always be done inside of the container built from the project's Dockerfile. The container should be started in privileged mode with all GPUs passed though. Hugepages mounted on the host should be passed through into the container via a volume mount. When compiling the container should be started with the current user. When running the  
container should run as root.
