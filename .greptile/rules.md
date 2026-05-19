# Greptile reviewer rules — DAQIRI

These notes are the long-form context that pairs with the structured rules in
`.greptile/config.json`. Read both before commenting on a PR.

## What DAQIRI is

DAQIRI is a single C++/CUDA shared library (`libdaqiri.so`) that provides
GPU-direct packet I/O for RF / scientific instrument capture. It exposes a flat
free-function C++ API (in `src/common.h`) on top of an opaque `BurstParams*`
batch type, and dispatches to one of three backends (DPDK, RDMA, sockets) via
the `daqiri::Manager` virtual interface in `src/manager.h`. Applications never
touch backend types directly.

This is a low-level networking library with hardware dependencies — be biased
toward fewer, higher-confidence comments. Pieces of code that look "wrong" by
generic best-practice standards (manual pool management, raw pointers, locked
memory layouts, platform-specific defines, custom alignment) are usually load-
bearing on purpose. When in doubt, ask rather than block.

## The four invariants worth blocking on

### 1. BurstParams is zero-copy and the caller frees

A `BurstParams` is a batch of packets whose payload buffers live in DMA-capable
memory the NIC wrote (RX) or will read (TX). Pointers — never copies — flow
between NIC, library, and application.

Because nothing else owns the batch, **the caller must explicitly free every
burst**. A missed free does not leak silently — the underlying mempool drains,
`get_*_burst` starts returning `NO_FREE_BURST_BUFFERS` /
`NO_FREE_PACKET_BUFFERS`, and the NIC drops packets. By the time the operator
sees it, it usually looks like a NIC/driver problem rather than an application
bug.

When reviewing any new code path that calls `get_rx_burst`, `get_tx_burst`,
`create_burst_params`, or `create_tx_burst_params`, walk every exit path
(success, error, early return, exception, `goto cleanup`) and verify there is
a matching free. This is high-severity. See `docs/api-guide.md` and
`src/common.h`.

### 2. Backend selection lives behind the Manager vtable

The active backend is chosen at `daqiri_init()` time from `NetworkConfig`, and
`ManagerFactory` instantiates exactly one `daqiri::Manager` per process. Every
piece of packet logic — RX/TX dequeue, header fill, segment access, RDMA
connection setup — is dispatched through that vtable.

Reject PRs that:

- Branch on backend type in common code (`if (backend == "dpdk") …`).
- Reach into a backend-specific struct (`DpdkManager*`, `RdmaManager*`,
  `SocketManager*`) from `src/common*` or from application code.
- Add a new backend by extending `src/common.cpp` instead of adding a
  `src/managers/<name>/` directory and a new `Manager` subclass.
- Expose backend types in the public API surface in `src/common.h`.

The point of the abstraction is that an application written against the
free-function API can switch backends with a config change. New code must
preserve that.

### 3. `DAQIRI_MGR` socket → rdma rule

The socket backend reuses the RoCE transport from the rdma backend. As a
result, `src/CMakeLists.txt` always prepends `rdma` to `DAQIRI_MGR_LIST`
whenever `socket` is requested:

```cmake
list(FIND DAQIRI_MGR_LIST "socket" DAQIRI_HAS_SOCKET_IDX)
if(NOT DAQIRI_HAS_SOCKET_IDX EQUAL -1)
  list(FIND DAQIRI_MGR_LIST "rdma" DAQIRI_HAS_RDMA_IDX)
  if(DAQIRI_HAS_RDMA_IDX EQUAL -1)
    list(APPEND DAQIRI_MGR_LIST "rdma")
  endif()
  list(REMOVE_ITEM DAQIRI_MGR_LIST "rdma")
  list(INSERT DAQIRI_MGR_LIST 0 "rdma")
endif()
```

The reverse is **not** true — listing `rdma` alone does not pull in `socket`.
The `rdma` entry must be first in the list so it builds first.

If a PR touches this block, or refactors how `DAQIRI_MGR_LIST` is consumed
later (the `foreach` that emits `DAQIRI_MGR_<NAME>=1` compile definitions),
verify the invariant still holds. A regression here turns into a confusing
link error in the socket backend that won't reproduce on a `dpdk socket`
build because the developer happened to have rdma enabled.

DPDK is also load-bearing for the common library itself — `src/common.cpp`
uses `rte_ring` / `rte_mbuf` directly, so DPDK is a build dependency even of
`rdma`-only or `socket`-only configurations. Do not "remove DPDK" without
removing those uses.

### 4. Doc-sync on code changes

There is no automated doc-sync gate beyond `mkdocs --strict` link checks. If
the code in a PR changes but the corresponding docs don't, drift goes
unnoticed until the next external user files an issue.

The mapping (mirrored from `.claude/rules/docs-sync.md`):

| Source path | Docs to update in the same PR |
| --- | --- |
| `src/common.h` | `docs/api-guide.md`, `docs/daqiri-api.html`, `AGENTS.md` (Architecture / API summary) |
| `src/types.h` | `docs/api-guide.md`, `docs/daqiri-api.html`, `AGENTS.md` (Architecture / BurstParams) |
| `src/manager.h` | `docs/api-guide.md`, `AGENTS.md` (Manager abstraction) |
| `src/managers/*/` | `docs/getting-started.md`, `docs/configuration.md`, `docs/tutorials/configuration-walkthrough.md`, `README.md` (Backends), `AGENTS.md` |
| `src/CMakeLists.txt` (CMake options, `DAQIRI_MGR` default, CUDA arch) | `docs/getting-started.md`, `AGENTS.md` (Build & run), `README.md` (Quick Start) |
| `src/kernels.cu` / `src/kernels.h` | `docs/tutorials/benchmarking_examples.md`, `AGENTS.md` (Reorder & quantize kernels) |
| `examples/*.cpp`, `examples/*.yaml` (new bench, new CLI flag, new YAML key) | `docs/tutorials/benchmarking_examples.md`, `docs/tutorials/configuration-walkthrough.md`, `AGENTS.md` (benchmark table) |
| `mkdocs.yml` nav | `docs/index.html` (landing page links) |
| Any `docs/*` rename or move | `README.md` (Documentation table), `AGENTS.md` (Documentation section), `mkdocs.yml`, `docs/index.html` |

When a PR touches a source path on the left but does not touch the matching
docs on the right, post a single comment listing the specific docs to update.
One consolidated comment, not one per file.

## CONTRIBUTING.md essentials

These are the contributor-facing rules that show up most often as PR review
findings. Greptile should catch them so a human reviewer doesn't have to.

- **DCO sign-off on every commit.** `Signed-off-by: Name <email>` trailer is
  required on every commit (CONTRIBUTING.md "Signing Your Work"). Missing
  trailer → ask the contributor to amend with `git commit -s --amend` or
  rebase with `git rebase --signoff`. Unsigned commits will be rejected, so
  flagging this early saves a round trip.

- **Commit title format.** Imperative mood, prefixed with the GitHub issue
  number: `#<Issue Number> - <Title>` (e.g. `#42 - Add socket TCP backend`).
  An issue must exist and be approved before coding starts; PRs without a
  referenced issue should be flagged.

- **clang-format clean.** Run `git-clang-format --style file` (staged
  changes) or `clang-format -style=file -i -fallback-style=none <files>`
  (specific files). The `.clang-format` in the repo root is the source of
  truth — do not introduce a different style.

- **Prefer CMake options over `#ifdef`-walling whole files.** New optional
  features should land as a CMake option with a backward-compatible default
  (typically the existing behavior). Whole-file inclusion/exclusion is done
  by editing `CMakeLists.txt`. Use `#if` only for minor in-file changes.

- **Narrow PRs, single concern.** If a PR mixes refactor + feature + doc
  reorganization, ask the contributor to split it. Note dependencies in the
  PR description rather than bundling.

- **Builds clean.** No new warnings. No commented-out code. Each new
  component ships with a README and an accompanying test/benchmark.

## Things to *not* nit on

- Manual pool / mempool management, explicit free calls, raw pointers passed
  through the API. This is the BurstParams contract — that's the point.
- Hardcoded CUDA arch list (`80;90;121`). It's intentional; there's a
  separate rule for callouts when a PR actually requires bumping it.
- `#if DAQIRI_MGR_<NAME>` guards in backend-specific files. They're how
  per-backend code is selected at compile time.
- DPDK-style header-fill APIs (`set_udp_header`, etc.) only supporting UDP.
  Documented limitation; flag only if a PR claims to fix it but forgets to
  update `AGENTS.md` / `README.md`.
- `<angle-bracket>` placeholders in `examples/*.yaml`. They're meant to be
  filled in per-host by the operator.
