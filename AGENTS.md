# AGENTS.md

This file provides guidance to coding agents when working with code in this repository.

## Build & run

```bash
# Configure and build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DDAQIRI_BUILD_PYTHON=OFF -DDAQIRI_ENGINE="dpdk ibverbs"
cmake --build build -j
cmake --install build --prefix /opt/daqiri

# Container build (compiles patched DPDK from source)
BASE_TARGET=dpdk DAQIRI_ENGINE="dpdk ibverbs" scripts/build-container.sh

# PCIe-only container (no DPDK or ibverbs engine)
BASE_TARGET=base-deps DAQIRI_ENABLE_PCIE=ON DAQIRI_ENGINE="" scripts/build-container.sh
```

CMake options (full table in `docs/getting-started.md`):
- `DAQIRI_ENGINE` — space-separated list of optional engines to compile. Valid values: `dpdk` (raw Ethernet) and `ibverbs` (RDMA/RoCE). Linux sockets (UDP/TCP) are always built in, so there is no `socket` value. Default is `"dpdk ibverbs"`.
- `DAQIRI_ENABLE_PCIE` — builds the `stream_type: "pcie"` implementation and benchmark independently of `DAQIRI_ENGINE` (default `OFF`; requires CUDA Toolkit 12.8+).
- `DAQIRI_BUILD_PYTHON` — builds `pybind11` bindings from `python/`.
- `DAQIRI_BUILD_EXAMPLES` — builds the benchmark executables (default `ON`).
- `DAQIRI_ENABLE_OTEL_METRICS` — enables OpenTelemetry metrics instrumentation (default `OFF`).
- `DAQIRI_REORDER_GPU_PROFILE` — enable CUDA event timing in the DPDK reorder kernels (off by default).
- `DAQIRI_ENABLE_S3` — enable AWS SDK-backed asynchronous raw packet writes to S3 (off by default).
- `DAQIRI_PREFER_SYSTEM_YAML_CPP` — prefer system `yaml-cpp` over the vendored `third_party/yaml-cpp` submodule (off by default; keep off when a conda/miniforge env is on `PATH`).

Package versions use CalVer (`YYYY.MM.PATCH`) from the top-level `VERSION` file. CMake reads that value into `project(daqiri VERSION ...)`, generates `daqiri/version.h`, feeds pkg-config/CMake package metadata, and exposes the same value in Python. `DAQIRI_ABI_VERSION` is separate and currently `0`; do not tie ABI policy to the CalVer year.

CUDA architectures default to `80;90` (A100, H100), with `121` (GB10) added when configuring with CUDA Toolkit 13.0 or newer. Override `CMAKE_CUDA_ARCHITECTURES` when targeting other GPUs.

**Socket / ibverbs relationship**: the socket engine is always built and provides UDP/TCP directly; its RoCE path delegates to the `ibverbs` engine (internally the `rdma` engine — `src/engines/rdma/`, target `daqiri_rdma`, define `DAQIRI_ENGINE_RDMA`; `ibverbs` is only the user-facing name). `src/CMakeLists.txt` builds the ibverbs/rdma engine only when `ibverbs` is in `DAQIRI_ENGINE`, and the socket engine links it conditionally — so `socket` RoCE is available only when `ibverbs` was built, while plain UDP/TCP always works.

## Benchmarks (the "tests")

There is no unit test suite. Verification is done via the benchmark executables in `examples/`, driven by YAML configs. Build outputs (`examples/CMakeLists.txt:59-71`):

| Executable | Source | Typical config |
|---|---|---|
| `daqiri_bench_raw_gpudirect` | `raw_gpudirect_bench.cpp` | `daqiri_bench_raw_tx_rx.yaml`, `daqiri_bench_raw_tx_rx_4q.yaml`, `daqiri_bench_raw_tx_rx_spark.yaml`, `daqiri_bench_raw_{tx,rx}_spark_xhost.yaml`, `daqiri_bench_raw_sw_loopback.yaml`, `daqiri_bench_raw_rx_multi_q.yaml`, `daqiri_bench_raw_tx_rx_vxlan.yaml`, `daqiri_bench_raw_tx_rx_vlan.yaml`, `daqiri_bench_raw_tx_rx_gre.yaml`, `daqiri_bench_raw_tx_rx_nvgre.yaml`, `daqiri_bench_raw_tx_rx_spark_mq.yaml` (mq base; `run_spark_mq_bench.sh` derives the 4 cells via `scripts/gen_spark_mq_config.py`), `daqiri_bench_raw_tx_rx_pacing.yaml` (per-queue `pacing_mbps`; DPDK engine only) |
| `daqiri_example_dynamic_rx_flow` | `dynamic_rx_flow_example.cpp` | `daqiri_example_dynamic_rx_flow.yaml` — `flow_isolation: true` startup followed by runtime RX queue-steering and raw-engine decap/pop flow add/delete |
| `daqiri_bench_raw_hds` | `raw_hds_bench.cpp` | `daqiri_bench_raw_tx_rx_hds.yaml` |
| `daqiri_bench_raw_reorder_seq` | `raw_reorder_seq_bench.cpp` | `daqiri_bench_raw_tx_rx_reorder_seq_1024*.yaml`, `daqiri_bench_raw_rx_reorder_seq_*.yaml` |
| `daqiri_bench_raw_reorder_quantize` | `raw_reorder_quantize_bench.cpp` | `daqiri_bench_raw_tx_rx_reorder_quantize_seq_batch.yaml` |
| `daqiri_bench_rdma` | `rdma_bench.cpp` | `daqiri_bench_rdma_tx_rx.yaml`, `daqiri_bench_rdma_tx_rx_spark.yaml`, `daqiri_bench_rdma_tx_rx_spark_xhost.yaml`, `daqiri_bench_rdma_tx_rx_spark_netns.yaml` (combined-role netns base; `run_spark_bench.sh` splits per role via `scripts/gen_spark_netns_config.py`) |
| `daqiri_bench_socket` | `socket_bench.cpp` | `daqiri_bench_socket_{udp,tcp}_tx_rx.yaml`, `daqiri_bench_socket_{udp,tcp}_tx_rx_spark_netns.yaml` (combined-role netns bases) |
| `daqiri_bench_pcie` | `pcie_bench.cpp` | `daqiri_bench_pcie_sw_loopback.yaml` — deterministic GPU-buffer validation through the PCIe software provider; built with `DAQIRI_ENABLE_PCIE=ON` |
| `daqiri_pool_ring_bench` | `pool_ring_bench.cpp` | none — microbenchmark comparing `daqiri::Ring`/`daqiri::ObjectPool` vs DPDK `rte_ring`/`rte_mempool` (SPSC/MPMC, single/bulk, thread sweep). The `rte_*` comparison arm compiles only in a DPDK-enabled build; takes no YAML/CLI args |

The four `raw_*` benches share `raw_bench_common.{cpp,h}` and accept `--seconds N`. `daqiri_bench_rdma` and `daqiri_bench_socket` also take `--mode {tx,rx,both}`. `daqiri_bench_raw_gpudirect`, `daqiri_bench_raw_hds`, `daqiri_bench_rdma`, and `daqiri_bench_socket` additionally accept `--workload none|fft|gemm|gemm_fp16` — a reusable representative GPU workload (`examples/bench_workload.{h,cu}`, cuFFT/cuBLAS) run once per received reorder window on the **actual received payload**. Each backend first assembles the burst's payloads into one contiguous GPU buffer via `examples/bench_pipeline.{h,cu}` (`ReorderPipeline`): a sequence-number reorder kernel for the out-of-order transports (DPDK raw, UDP) and an arrival-order gather for the in-order ones (RoCE RC, TCP); sockets stage host→device first since their payloads land in pageable host memory. The reorder/gather kernels (`packet_reorder_copy_payload_by_sequence`, `packet_gather_copy_payload`) live in `src/kernels.cu`. `gemm` is FP32 `cublasSgemm`; `gemm_fp16` is the same-size mixed-precision FP16/tensor-core `cublasGemmEx` (inference-style); the contiguous buffer supplies the FFT input / GEMM A operand. `--workload-batch-bytes N` decouples the compute working set from the I/O unit (RoCE sub-chunks each message; raw sizes its reorder window), enabling a batch-size sweep. Used by `run_spark_bench.sh`'s `WORKLOAD` / `WORKLOAD_BATCH` env (all backends) to fill the CSV `post_process` / `post_process_batch` columns (issue #15).

```bash
./build/examples/daqiri_bench_raw_gpudirect ./build/examples/daqiri_bench_raw_tx_rx.yaml --seconds 10
./build/examples/daqiri_bench_socket        ./build/examples/daqiri_bench_socket_udp_tx_rx.yaml --seconds 10 --mode both
./build/examples/daqiri_bench_pcie          ./build/examples/daqiri_bench_pcie_sw_loopback.yaml --seconds 10 --mode both
```

YAML files contain `<angle-bracket>` placeholders (PCIe addresses, CPU cores, MACs, IPs) that **must** be replaced for your system. `daqiri_bench_raw_sw_loopback.yaml` requires no physical link; `daqiri_bench_pcie_sw_loopback.yaml` requires no FPGA or board driver.

Configs named `raw_rx_*` are RX-only — they initialize the RX path and wait for external traffic, so a standalone run can exit cleanly with `0` packets. Use the `tx_rx` configs for closed-loop smoke tests. NIC flow programming (RX flows, dynamic RX flows, `tx_eth_src`) requires a hardware loopback or cross-host wire test — see `docs/benchmarks/raw_benchmarking.md` (Flow programming smoke test); `daqiri_bench_raw_sw_loopback.yaml` only smoke-tests the build/runtime path.

When determining throughput for a benchmark use the `mlnx_perf` utility in the background to view transmit and receive rates. Using application run time with packet counts is usually not accurate enough due to startup inconsistencies.

## Formatting

`clang-format` is required for contributions (CONTRIBUTING.md):

```bash
git-clang-format --style file              # format staged changes
clang-format -style=file -i -fallback-style=none <files>
```

## Architecture

**Single C++/CUDA shared library** (`libdaqiri.so`) exposing a C++ API through `#include <daqiri/daqiri.h>`. The public surface is intentionally flat free-function helpers (`get_rx_burst`, `get_packet_ptr`, `set_udp_header`, `socket_setsockopt`, …) that all operate on opaque DAQIRI-owned buffers or connection IDs. Applications never touch engine types directly.

`include/daqiri/daqiri.h` also includes the generated `daqiri/version.h`, exposing `DAQIRI_VERSION`, CalVer component macros, `DAQIRI_ABI_VERSION`, and inline helpers such as `daqiri::version_string()` and `daqiri::abi_version()`.

### Engine abstraction
`src/engine.h` defines `daqiri::Engine` — an (almost) ABC with ~50 virtual methods covering init, RX/TX burst dequeue/enqueue, header-fill helpers, buffer free, socket connection helpers, runtime TCP/UDP `setsockopt` passthrough, and RDMA connection setup. Engines live in `src/engines/<name>/` (`dpdk/`, `rdma/`, `socket/`, `ibverbs/`). `DAQIRI_ENGINE` selects the optional `dpdk` and `ibverbs` engines at CMake configure time; the `socket` engine is always built. The user-facing value `ibverbs` builds **two** internal engines that both use libibverbs: `rdma` (`src/engines/rdma/`, `DAQIRI_ENGINE_RDMA`, RoCE/InfiniBand for socket `roce://`) and `ibverbs` (`src/engines/ibverbs/`, `DAQIRI_ENGINE_IBVERBS`, the pure-DevX MPRQ raw-Ethernet engine). Each engine produces its own static library (`daqiri_dpdk`, `daqiri_rdma`, `daqiri_socket`, `daqiri_ibverbs`) linked into `daqiri_common`, and each adds a `DAQIRI_ENGINE_<NAME>=1` compile definition.

`EngineType` (`include/daqiri/types.h`) is resolved from `(stream_type, engine)`: `raw` defaults to `EngineType::DPDK`; `raw` + `engine: "ibverbs"` selects `EngineType::IBVERBS` (the MPRQ engine); `socket` + a `roce://` endpoint (or `engine: "ibverbs"`) selects `EngineType::RDMA`. The stream-aware `config_engine_from_string(str, stream_type)` overload encodes the `ibverbs`→`{IBVERBS for raw, RDMA for socket}` split. `EngineFactory` (in `engine.h`) is a singleton that instantiates the active engine. `daqiri_init(...)` resolves which engine to use from the `NetworkConfig` and then delegates everything through the `Engine` vtable. There is only ever **one** active `Engine` per process.

PCIe is the exception to public engine selection: `stream_type: "pcie"` directly constructs the internal `PcieEngine`, rejects any YAML `engine:` key, and reports `EngineType::DEFAULT`. It is enabled by `DAQIRI_ENABLE_PCIE`, not `DAQIRI_ENGINE`. DMA-BUF is its v1 GPU-registration mechanism, not an engine; `nvidia-peermem` is not used for custom FPGA registration. The stable C driver/RTL ABI is `include/daqiri/pcie_abi.h`; hardware operation needs a board-specific driver and FPGA implementation, while `loopback: "sw"` selects the included software provider. PCIe permits one RX and one TX queue per interface (ID 0), dedicated `MemoryKind::DEVICE` regions, and no flows, HDS, reorder, timestamps, offloads, or scheduled send.

The always-built socket engine implements Linux UDP/TCP streams directly. Applications that need kernel socket tuning call `socket_setsockopt(conn_id, level, optname, optval, optlen)` after resolving a TCP/UDP connection ID; DAQIRI passes the numeric Linux constants through without maintaining a symbolic option map. `socket_setsockopt` is not supported for `roce://` connections, which delegate to the RDMA/ibverbs path.

The default `dpdk` raw engine (`src/engines/dpdk/`, `DpdkEngine`) programs RX steering, send-to-kernel fallbacks (`flow_isolation: true`), and `tx_eth_src` TX offloads via DPDK RTE Flow during `daqiri_init()`. Standard UDP/IP (group 3), flex-item (group 1) and eCPRI-over-Ethernet (group 2, EtherType `0xAEFE`, via `RTE_FLOW_ITEM_TYPE_ECPRI` matching message type and pc_id/rtc_id) RX flows use separate flow groups; `validate_config()` rejects mixing these flow classes per interface, duplicate `flex_item_id` values, and unknown `action.id` / `flex_item_id` values before NIC programming. The mlx5 PMD honors the native eCPRI flow item only under **firmware steering** (`dv_flow_en=1`); under HW steering (`dv_flow_en=2`, the default) the rule installs but silently never matches on ConnectX-class NICs. So `initialize()` auto-switches any interface carrying eCPRI RX flows to `dv_flow_en=1` (with a `WARN`), which means the async/template dynamic-RX-flow path is unavailable on that port. Flex-item parser handles are created per `(port, flex_item_id)` (scoped per interface). All programmed `rte_flow` rules and flex-item handles are tracked and destroyed in order on shutdown, init failure, and engine teardown (programmed flows → flex items → group-0 ETH jump rules). See `docs/benchmarks/raw_benchmarking.md` (Flow programming smoke test) for manual verification steps.

The `ibverbs` raw engine (`src/engines/ibverbs/`, `IbverbsEngine`) drives a Mellanox/mlx5 Multi-Packet (striding) Receive Queue via **DevX** (`mlx5dv_devx_obj_create` against vendored PRM structs in `mlx5_prm_min.h`): a DevX CQ + striding RQ + TIR + `mlx5dv_dr` flow steering, with manual WQE/doorbell management and worker-driven cyclic refill. RX packets DMA strided into one pre-posted MR (host or GPU via `ibv_reg_dmabuf_mr`); a queue with >1 memory region instead uses a non-striding DevX *regular* RQ with multi-segment scatter WQEs for **physical** header-data split (header → CPU MR, payload → GPU MR). TX builds mlx5 send WQEs directly on a raw-packet QP's SQ (via `mlx5dv_init_obj`, bypassing `ibv_post_send`) from a slab of registered slots tracked by cyclic index counters, with NIC checksum offload and a `tx_eth_src` offload. It uses the libdpdk-free `daqiri::Ring`/`daqiri::ObjectPool` for the worker→app burst handoff (like the rdma engine — neither links DPDK) and drives the NIC through libibverbs/mlx5dv directly. Feature set: RX (MPRQ), TX, GPUDirect, physical/logical HDS, multi-queue 5-tuple flow steering with per-packet flow IDs, flex-item arbitrary-offset, IPv4-total-length and eCPRI-over-Ethernet (EtherType `0xAEFE`, message type + pc_id/rtc_id) flow matching (mlx5 flex parser / `misc_parameters_4`), per-packet RX hardware timestamps, accurate TX send scheduling (wait-on-time WAIT WQE), and GPU reorder/quantize. Because it uses the kernel netdev directly, `ensure_port_mtus` raises the netdev MTU at init to cover the configured frame size (jumbo frames silently drop on RX otherwise). Queues sharing a `cpu_core` are serviced round-robin by one poller thread.

### Zero-copy / BurstParams
All packet data flows through `BurstParams`, a batch of packets. Only pointers are passed between NIC, DAQIRI internals, and the application — the caller reads directly from the buffers the NIC DMA'd into. **The caller must explicitly free bursts**; a missed free drains the pool and produces `NO_FREE_BURST_BUFFERS` / `NO_FREE_PACKET_BUFFERS` errors and NIC drops. See `docs/concepts.md` (Zero-Copy Ownership) and `docs/api-reference/cpp.md` (free-function call patterns).

### Segments & HDS
A single packet can span multiple **segments** (contiguous memory regions), each in CPU or GPU memory. The header-data split (HDS) mode puts headers in segment 0 (CPU) and payload in segment 1 (GPU), enabling GPUDirect zero-copy payload paths. Batched-GPU and CPU-only modes use a single segment.

### DPDK is required only by the `dpdk` engine
DPDK is **not** a dependency of `daqiri_common` or of the `rdma`/`ibverbs`/`socket` engines. Those use the libdpdk-free `daqiri::Ring` (`src/daqiri_ring.h`) and `daqiri::ObjectPool` (`src/daqiri_pool.h`) — header-only replacements for the generic `rte_ring` (a bounded MPMC/SPSC pointer ring, the DPDK C11 4-cursor algorithm) and `rte_mempool` (a fixed-size slab + free-ring) usage. `src/CMakeLists.txt` parses `DAQIRI_ENGINE` first, sets `DAQIRI_BUILD_DPDK`, and only then runs `pkg_check_modules(DPDK REQUIRED libdpdk)` and links it into `daqiri_common`. The only `rte_*` code left in `daqiri_common` lives in `dpdk_log.cpp` and `src/engine_dpdk.cpp` (the DPDK-only `Engine` base-class methods: mbuf/extmem registration, mbuf packet pools, EAL/hugepage preflight + cleanup), both compiled in **only** when `dpdk ∈ DAQIRI_ENGINE`. The `rdma`/`ibverbs` engines no longer call `rte_eal_init` at all (they only needed EAL to back the rings/pools); the `dpdk` engine still does. `MemoryKind::HUGE` allocation goes through the virtual `Engine::alloc_huge` hook — `mmap(MAP_HUGETLB)` in the base, overridden by `DpdkEngine` to use `rte_malloc_socket`. A build with `DAQIRI_ENGINE="ibverbs"` (or `""`) links no `librte_*`. **libnuma is an optional dependency** (`DAQIRI_HAVE_NUMA`, auto-detected): when present, `daqiri::Ring`/`daqiri::ObjectPool` and `alloc_huge` pin their memory to the NUMA node DPDK's socket-aware allocators used (rings/pools → the master core's node, mirroring `rte_socket_id()`; HUGE MRs → `mr.affinity_`, mirroring `rte_malloc_socket`); without it they fall back to first-touch placement. `python/tune_system.py --check numa` warns when libnuma is absent on a multi-socket host. The container build still uses patched DPDK from `dpdk_patches/` (`dmabuf.patch`, `dpdk.nvidia.patch`) for the `dpdk` engine — the `dmabuf` patch removes the peermem kernel-module requirement for GPUDirect.

### Reorder & quantize kernels
`src/kernels.cu` hosts the CUDA reorder paths used by the `raw_reorder_*` benches. Compile with `-DDAQIRI_REORDER_GPU_PROFILE=ON` to instrument them with CUDA event timing.

### Third-party dependencies
Vendored under `third_party/` as submodules (`.gitmodules`): `yaml-cpp` (config parser) and `spdlog` (logging). CMake prefers these over system copies. Missing them is a fatal error.

### Current limitations
- TX header fill currently supports UDP only (see README).
- Raw Ethernet RX flow legacy `action.id` or final `actions:` queue action must match an `rx.queues` ID, and flex-item flows must reference a valid `flex_item_id` on the same interface; `daqiri_init()` aborts if RX flow rules, send-to-kernel fallbacks (`flow_isolation: true`), transform flow actions, or `tx_eth_src` offload rules cannot be programmed on the NIC.
- Raw Ethernet tunnel/VLAN transform flows are hardware-only on the DPDK and ibverbs raw engines. TX flows may contain only push/encap transform actions and RX transform flows must use pop/decap actions ending in a queue; socket/RDMA engines reject these actions instead of adding a software fallback.
- Raw Ethernet RX flow steering: a single interface cannot mix standard (UDP/IP) and
  flex-item flows, and flex-item flows cannot combine with tunnel/VLAN transform actions; `DpdkEngine::validate_config()` rejects mixed configs at init.
- No CI yet — contributors and reviewers verify manually (CONTRIBUTING.md).

## Documentation

The web docs live in `docs/` and are built with [MkDocs Material](https://squidfunk.github.io/mkdocs-material/). The site config is `mkdocs.yml`.

**Structure:**
- `docs/index.md` — landing page orchestrator (includes `docs/landing/*.html` snippets via pymdownx snippets)
- `docs/landing/` — landing section HTML fragments (hero, features, quick start, examples, tutorials, news, CTA, footer, overlay)
- `overrides/home.html` — Material theme override for the landing layout
- `docs/getting-started.md` — system requirements, build instructions, CMake options
- `docs/concepts.md` — terminology glossary (stream types and endpoint URI schemes, GPUDirect, packet/burst/segment, flow/queue, memory region, zero-copy ownership, RX reorder). Meant to be opened in parallel with the rest of the docs.
- `docs/api-reference/index.md` — API guide (6-step application lifecycle, configuration-first model)
- `docs/api-reference/configuration.md`, `docs/api-reference/cpp.md`, `docs/api-reference/python.md` — YAML schema, C++ API, and Python bindings docs
- `docs/tutorials/` — tutorial walkthroughs (system config, config-file walkthrough, Holoscan integration)
- `docs/benchmarks/` — benchmark guide pages, surfaced as a top-level "Benchmarking" nav section in `mkdocs.yml` and the landing page (`docs/index.md`):
  - `docs/benchmarks/benchmarks.md` — overview and engine-selection decision tree
  - `docs/benchmarks/socket_benchmarking.md` — "Socket and RDMA Benchmarking" (TCP/UDP and RoCE/RDMA)
  - `docs/benchmarks/raw_benchmarking.md` — "Raw Ethernet Benchmarking" (DPDK `raw_*` benches)
  - `docs/benchmarks/pcie_benchmarking.md` — PCIe software loopback plus driver/FPGA ABI and ownership contract
  - `docs/benchmarks/performance-dgx-spark.md` — per-platform performance report for DGX Spark stream/protocol combinations (the long internal report lives outside the repo in `projects/daqiri-notes/`)
- `docs/stylesheets/extra.css` — custom theme overrides

**User-facing vocabulary:** the YAML schema uses `stream_type` (`raw`, `socket`, `pcie`); for socket streams the transport is encoded in the endpoint URI scheme (`udp://`, `tcp://`, `roce://`) in `socket_config.local_addr`/`remote_addr`, **not** a separate `protocol` field. (`SocketProtocol` still exists internally, derived from the scheme.) **"Engine"** is the standard term for the specific library backing a network implementation. The mapping: `stream_type: "raw"` is implemented by the `dpdk` engine; `stream_type: "socket"` with `udp://`/`tcp://` endpoints by the always-built `socket` engine; `stream_type: "socket"` with `roce://` endpoints by the `ibverbs` engine. `stream_type: "pcie"` has no user-selectable engine and rejects `engine:`.

**Keeping docs in sync with code:** before committing changes, scan for the recurring drift hotspots:
- **Stream-type list** (`src/engines/*/`) — README Engines table, `docs/getting-started.md`, `docs/concepts.md` (Stream Types section + Support and testing admonition), `docs/api-reference/configuration.md`
- **CMake options / `DAQIRI_ENGINE` default** (`src/CMakeLists.txt`) — README Quick Start, `docs/getting-started.md`, this file's Build & run section
- **Benchmark binary or YAML names** (`examples/`) — the benchmark table above, `docs/benchmarks/raw_benchmarking.md`, the "Choosing an example config" decision tree in `docs/tutorials/configuration-walkthrough.md` (every YAML must have a leaf; CI's `scripts/check_doc_refs.py` enforces coverage), and per-platform performance docs (`docs/benchmarks/performance-*.md`)
- **Public API include** (`#include <daqiri/daqiri.h>`; source files under `include/daqiri/`) — `docs/api-reference/index.md`, `docs/api-reference/cpp.md`, `docs/api-reference/python.md`; if the change adds or renames a user-facing concept, also `docs/concepts.md`
- **Python bindings** (`python/daqiri_common_pybind.cpp`) — `docs/api-reference/python.md` (function reference tables, enums/classes tables, GIL Behavior section)
- **Bench CLI flags or output format** (`examples/raw_bench_common.{h,cpp}`, `*_bench.cpp`) — per-platform performance docs' Methodology section, `examples/run_spark_bench.sh` parsing logic
- **Doc reorganization** (any rename in `docs/`) — `docs/index.md` landing page, `mkdocs.yml` nav, README Documentation table

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
