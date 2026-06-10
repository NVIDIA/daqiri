# AGENTS.md

This file provides guidance to coding agents when working with code in this repository.

## Build & run

```bash
# Configure and build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DDAQIRI_BUILD_PYTHON=OFF -DDAQIRI_MGR="dpdk socket rdma"
cmake --build build -j
cmake --install build --prefix /opt/daqiri

# Container build (compiles patched DPDK from source)
BASE_TARGET=dpdk DAQIRI_MGR="dpdk socket rdma" scripts/build-container.sh
```

CMake options (full table in `docs/getting-started.md`):
- `DAQIRI_MGR` — space-separated backend list. Valid values: `dpdk`, `socket`, `rdma`. Default in `src/CMakeLists.txt:137` is `"dpdk socket"` (which, due to the rule below, effectively builds all three).
- `DAQIRI_BUILD_PYTHON` — builds `pybind11` bindings from `python/`.
- `DAQIRI_BUILD_EXAMPLES` — builds the benchmark executables (default `ON`).
- `DAQIRI_ENABLE_OTEL_METRICS` — enables OpenTelemetry metrics instrumentation (default `OFF`).
- `DAQIRI_REORDER_GPU_PROFILE` — enable CUDA event timing in the DPDK reorder kernels (off by default).

CUDA architectures default to `80;90` (A100, H100), with `121` (GB10) added when configuring with CUDA Toolkit 13.0 or newer. Override `CMAKE_CUDA_ARCHITECTURES` when targeting other GPUs.

**Socket → RDMA dependency**: the socket backend reuses the RoCE transport from the RDMA implementation, so `src/CMakeLists.txt:144-152` automatically prepends `rdma` to `DAQIRI_MGR_LIST` whenever `socket` is requested. The reverse is not true — listing `rdma` alone does not pull in `socket`.

## Benchmarks (the "tests")

There is no unit test suite. Verification is done via the benchmark executables in `examples/`, driven by YAML configs. Build outputs (`examples/CMakeLists.txt:59-71`):

| Executable | Source | Typical config |
|---|---|---|
| `daqiri_bench_raw_gpudirect` | `raw_gpudirect_bench.cpp` | `daqiri_bench_raw_tx_rx.yaml`, `daqiri_bench_raw_tx_rx_4q.yaml`, `daqiri_bench_raw_tx_rx_spark.yaml`, `daqiri_bench_raw_{tx,rx}_spark_xhost.yaml`, `daqiri_bench_raw_sw_loopback.yaml`, `daqiri_bench_raw_rx_multi_q.yaml`, `daqiri_bench_raw_tx_rx_spark_mq_{1t1r,1t2r,2t1r,2t2r}.yaml` |
| `daqiri_bench_raw_hds` | `raw_hds_bench.cpp` | `daqiri_bench_raw_tx_rx_hds.yaml` |
| `daqiri_bench_raw_reorder_seq` | `raw_reorder_seq_bench.cpp` | `daqiri_bench_raw_tx_rx_reorder_seq_1024*.yaml`, `daqiri_bench_raw_rx_reorder_seq_*.yaml` |
| `daqiri_bench_raw_reorder_quantize` | `raw_reorder_quantize_bench.cpp` | `daqiri_bench_raw_tx_rx_reorder_quantize_seq_batch.yaml` |
| `daqiri_bench_rdma` | `rdma_bench.cpp` | `daqiri_bench_rdma_tx_rx.yaml`, `daqiri_bench_rdma_tx_rx_spark.yaml`, `daqiri_bench_rdma_tx_rx_spark_xhost.yaml` |
| `daqiri_bench_socket` | `socket_bench.cpp` | `daqiri_bench_socket_{udp,tcp}_tx_rx.yaml` |

The four `raw_*` benches share `raw_bench_common.{cpp,h}` and accept `--seconds N`. `daqiri_bench_rdma` and `daqiri_bench_socket` also take `--mode {tx,rx,both}`.

```bash
./build/examples/daqiri_bench_raw_gpudirect ./build/examples/daqiri_bench_raw_tx_rx.yaml --seconds 10
./build/examples/daqiri_bench_socket        ./build/examples/daqiri_bench_socket_udp_tx_rx.yaml --seconds 10 --mode both
```

YAML files contain `<angle-bracket>` placeholders (PCIe addresses, CPU cores, MACs, IPs) that **must** be replaced for your system. `daqiri_bench_raw_sw_loopback.yaml` requires no physical link and is the fastest way to smoke-test a build.

Configs named `raw_rx_*` are RX-only — they initialize the RX path and wait for external traffic, so a standalone run can exit cleanly with `0` packets. Use the `tx_rx` configs for closed-loop smoke tests.

## Formatting

`clang-format` is required for contributions (CONTRIBUTING.md):

```bash
git-clang-format --style file              # format staged changes
clang-format -style=file -i -fallback-style=none <files>
```

## Architecture

**Single C++/CUDA shared library** (`libdaqiri.so`) exposing a C++ API through `#include <daqiri/daqiri.h>`. The public surface is intentionally flat free-function helpers (`get_rx_burst`, `get_packet_ptr`, `set_udp_header`, …) that all operate on opaque DAQIRI-owned buffers. Applications never touch backend types directly.

### Manager abstraction
`src/manager.h` defines `daqiri::Manager` — an (almost) ABC with ~50 virtual methods covering init, RX/TX burst dequeue/enqueue, header-fill helpers, buffer free, and RDMA connection setup. Backends live in `src/managers/<name>/` (`dpdk/`, `rdma/`, `socket/`) and are selected at CMake configure time via `DAQIRI_MGR`. Each backend produces its own static library (`daqiri_dpdk`, `daqiri_rdma`, `daqiri_socket`) linked into `daqiri_common`, and each adds a `DAQIRI_MGR_<NAME>=1` compile definition (see `src/CMakeLists.txt:156-183`).

`ManagerFactory` (also in `manager.h`) is a singleton that instantiates the active backend. `daqiri_init(...)` resolves which backend to use from the `NetworkConfig` and then delegates everything through the `Manager` vtable. There is only ever **one** active `Manager` per process.

### Zero-copy / BurstParams
All packet data flows through `BurstParams`, a batch of packets. Only pointers are passed between NIC, DAQIRI internals, and the application — the caller reads directly from the buffers the NIC DMA'd into. **The caller must explicitly free bursts**; a missed free drains the pool and produces `NO_FREE_BURST_BUFFERS` / `NO_FREE_PACKET_BUFFERS` errors and NIC drops. See `docs/concepts.md` (Zero-Copy Ownership) and `docs/api-reference/cpp.md` (free-function call patterns).

### Segments & HDS
A single packet can span multiple **segments** (contiguous memory regions), each in CPU or GPU memory. The header-data split (HDS) mode puts headers in segment 0 (CPU) and payload in segment 1 (GPU), enabling GPUDirect zero-copy payload paths. Batched-GPU and CPU-only modes use a single segment.

### DPDK is load-bearing for the common library
`src/manager.h` and `src/common.cpp` use `rte_ring` / `rte_mbuf` directly, so **DPDK is a build dependency of `daqiri_common` even when building only the RDMA or socket backend**. `src/CMakeLists.txt:60-67` requires `pkg-config` + `libdpdk` and falls back to `/opt/mellanox/dpdk/...`. The container build uses patched DPDK from `dpdk_patches/` (`dmabuf.patch`, `dpdk.nvidia.patch`) — the `dmabuf` patch removes the peermem kernel-module requirement for GPUDirect.

### Reorder & quantize kernels
`src/kernels.cu` hosts the CUDA reorder paths used by the `raw_reorder_*` benches. Compile with `-DDAQIRI_REORDER_GPU_PROFILE=ON` to instrument them with CUDA event timing.

### Third-party dependencies
Vendored under `third_party/` as submodules (`.gitmodules`): `yaml-cpp` (config parser) and `spdlog` (logging). CMake prefers these over system copies. Missing them is a fatal error.

### Current limitations
- TX header fill currently supports UDP only (see README).
- No CI yet — contributors and reviewers verify manually (CONTRIBUTING.md).

## Documentation

The web docs live in `docs/` and are built with [MkDocs Material](https://squidfunk.github.io/mkdocs-material/). The site config is `mkdocs.yml`.

**Structure:**
- `docs/index.html` — custom HTML landing page (not generated by MkDocs, hand-maintained)
- `docs/getting-started.md` — system requirements, build instructions, CMake options
- `docs/concepts.md` — terminology glossary (stream types and protocols, GPUDirect, packet/burst/segment, flow/queue, memory region, zero-copy ownership, RX reorder). Meant to be opened in parallel with the rest of the docs.
- `docs/api-reference/index.md` — API guide (6-step application lifecycle, configuration-first model)
- `docs/api-reference/configuration.md`, `docs/api-reference/cpp.md`, `docs/api-reference/python.md` — YAML schema, C++ API, and Python bindings docs
- `docs/performance-dgx-spark.md` — per-platform performance report for DGX Spark stream/protocol combinations
- `docs/tutorials/` — tutorial walkthroughs (system config, config-file walkthrough)
- `docs/benchmarks/` — benchmark guide pages, surfaced as a top-level "Benchmarking" nav section in `mkdocs.yml` and `docs/index.html`:
  - `docs/benchmarks/benchmarks.md` — overview and backend-selection decision tree
  - `docs/benchmarks/socket_benchmarking.md` — "Socket and RDMA Benchmarking" (TCP/UDP and RoCE/RDMA)
  - `docs/benchmarks/raw_benchmarking.md` — "Raw Ethernet Benchmarking" (DPDK `raw_*` benches)
- `docs/stylesheets/extra.css` — custom theme overrides

**User-facing vocabulary:** docs and the YAML schema use `stream_type` (`raw`, `socket`, future `pcie`) and `protocol` (`udp`, `tcp`, `roce`). The word "backend" is internal-only — accurate for `src/managers/<name>/`, the `Manager` ABC, CMake `DAQIRI_MGR`, and API-reference function blurbs, but should not appear in tutorials, the landing page, or concept pages. The mapping: `stream_type: "raw"` is implemented by the `dpdk` manager; `stream_type: "socket"` with `protocol: "udp"` / `"tcp"` is implemented by the `socket` manager; `stream_type: "socket"` with `protocol: "roce"` is implemented by the `rdma` manager.

**Keeping docs in sync with code:** before committing changes, scan for the recurring drift hotspots:
- **Stream-type list** (`src/managers/*/`) — README Backends table, `docs/getting-started.md`, `docs/concepts.md` (Stream Types section + Support and testing admonition), `docs/api-reference/configuration.md`
- **CMake options / `DAQIRI_MGR` default** (`src/CMakeLists.txt:137`) — README Quick Start, `docs/getting-started.md`, this file's Build & run section
- **Benchmark binary or YAML names** (`examples/`) — the benchmark table above, `docs/benchmarks/raw_benchmarking.md`, the "Choosing an example config" decision tree in `docs/tutorials/configuration-walkthrough.md` (every YAML must have a leaf; CI's `scripts/check_doc_refs.py` enforces coverage), and per-platform performance docs (`docs/performance-*.md`)
- **Public API include** (`#include <daqiri/daqiri.h>`; source files under `include/daqiri/`) — `docs/api-reference/index.md`, `docs/api-reference/cpp.md`, `docs/api-reference/python.md`; if the change adds or renames a user-facing concept, also `docs/concepts.md`
- **Python bindings** (`python/daqiri_common_pybind.cpp`) — `docs/api-reference/python.md` (function reference tables, enums/classes tables, GIL Behavior section)
- **Bench CLI flags or output format** (`examples/raw_bench_common.{h,cpp}`, `*_bench.cpp`) — per-platform performance docs' Methodology section, `examples/run_spark_bench.sh` parsing logic
- **Doc reorganization** (any rename in `docs/`) — `docs/index.html` landing page, `mkdocs.yml` nav, README Documentation table

The full mapping with rationale lives in the docs-sync agent rule. Internal-link, anchor, and nav drift is enforced by CI (`.github/workflows/docs.yml`); content drift (stale binary names, defaults) is still a manual check at commit time.

**Deployment:** `.github/workflows/docs.yml` runs `mkdocs gh-deploy --force` on pushes to `main`, publishing to the `gh-pages` branch. GitHub Pages serves from `gh-pages`.

## Contribution rules

From `CONTRIBUTING.md`:
- **DCO sign-off required** — every commit must have `Signed-off-by:` (use `git commit -s`). Unsigned commits will be rejected.
- Commit titles in imperative mood, prefixed with the GitHub issue number: `#<Issue Number> - <Title>`.
- An issue must exist and be approved before coding.
- Prefer toggling features via new CMake options (with backward-compatible defaults) rather than wrapping entire files in `#if` guards. Use `#if` only for minor in-file changes.
- Keep PRs narrowly scoped — one concern per PR, dependencies noted in the description.
- When opening a PR that touches `src/`, `examples/`, or `mkdocs.yml`, scan the doc-sync agent rule and update affected docs in the same PR.

## Compiling and Running

Compiling should always be done inside of the container built from the project's Dockerfile. The container should be started in privileged mode with all GPUs passed though. Hugepages mounted on the host should be passed through into the container via a volume mount. When compiling the container should be started with the current user. When running the  
container should run as root.
