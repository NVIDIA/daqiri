# Documentation Sync Rules

These rules ensure documentation stays in sync with code changes in the DAQIRI repository.

> **Path freshness.** The doc paths listed below (e.g. `docs/api-reference/*.md`, `docs/getting-started.md`, `docs/tutorials/*.md`) reflect the repo layout at the time of writing. If a doc has been renamed, moved, or split, treat the path as a starting point: confirm the current location with `grep -r` or by reading `mkdocs.yml`'s nav before acting on it. The CI gate in `.github/workflows/docs.yml` enforces internal-link/anchor/nav correctness on every PR.

## When to check for doc impact

When the user is committing, pushing, or otherwise wrapping up a change that touches the files below, consider whether the related docs may need updating. The intent is a single check at the commit/PR checkpoint, not after every individual edit.

### API surface changes (high impact)
- `include/daqiri/common.h` / `include/daqiri/daqiri.h` — public free-function API (`get_rx_burst`, `get_packet_ptr`, `set_udp_header`, etc.). Any signature change, new function, removed function, or changed parameter semantics may need updating in:
  - `docs/api-reference/index.md` (API guide: 6-step application lifecycle)
  - `docs/api-reference/cpp.md` (C++ API usage guide and function reference)
  - `docs/concepts.md` (when the change introduces or renames a user-facing concept — burst, segment, flow, queue, memory region, etc.)
  - `AGENTS.md` (Architecture section's API summary)
- `include/daqiri/types.h` — public types (`BurstParams`, `Status`, `NetworkConfig`, enums like `MemoryKind`, `Direction`, `SocketProtocol`). Struct field changes, new enum values, or renamed types may need updating in the API docs, `docs/concepts.md` (terminology), and `AGENTS.md` (Architecture / BurstParams discussion).
- `src/manager.h` — `Manager` virtual interface and `ManagerFactory`. Changes here affect the backend abstraction docs in `docs/api-reference/cpp.md`, the backends section of `docs/concepts.md`, and the Manager-abstraction subsection in `AGENTS.md`.

### Backend and build changes (medium impact)
- `src/managers/*/` — adding a new backend or changing backend behavior may need updating in:
  - `docs/getting-started.md` (build instructions, backend selection)
  - `docs/concepts.md` (Kernel Bypass section: backend bullet and the Backend Maturity admonition)
  - `docs/api-reference/configuration.md` (YAML config options)
  - `docs/tutorials/configuration-walkthrough.md` (tutorial config walkthrough)
  - `README.md` (Backends table)
  - `AGENTS.md` (Manager abstraction / backend descriptions)
- `src/CMakeLists.txt` or `CMakeLists.txt` — changes to CMake options (`DAQIRI_MGR`, `DAQIRI_BUILD_PYTHON`, CUDA arch list, new dependencies) may need updating in:
  - `docs/getting-started.md` (build instructions)
  - `AGENTS.md` (build section)
  - `README.md` (Quick Start commands)
- `src/kernels.cu` / `src/kernels.h` — reorder or quantize kernel changes affect `docs/tutorials/benchmarking_examples.md` and the Reorder & quantize kernels subsection in `AGENTS.md`.

### Benchmark and example changes (medium impact)
- `examples/*.cpp` or `examples/*.yaml` — new benchmarks, changed CLI flags, or new YAML config keys may need updating in:
  - `docs/tutorials/benchmarking_examples.md`
  - `docs/tutorials/configuration-walkthrough.md` — when adding or removing a YAML, add or remove a leaf in the **"Choosing an example config"** decision tree (`#choosing-an-example-config`). CI's `scripts/check_doc_refs.py` enforces that every YAML in `examples/` is referenced in this file; a new config without a tree leaf will fail the check.
  - `AGENTS.md` (benchmark table)
- When adding or removing a benchmark executable, also update the benchmark table in `AGENTS.md`.
- **Annotated walkthrough pairing.** Three YAMLs in `examples/` are reproduced (in full or as diff snippets) inside the **"Annotated walkthrough"** section of `docs/tutorials/configuration-walkthrough.md`. Any edit to one of these files needs a paired edit to the walkthrough so the two stay in sync:
  - `examples/daqiri_bench_raw_tx_rx.yaml` ↔ `### Base TX+RX config` (full-file reproduction; CI's `check_doc_refs.py` does a structural equality check after stripping `# (N)!` annotation markers — any drift fails the build).
  - `examples/daqiri_bench_raw_tx_rx_hds.yaml` ↔ `### Header-data split (HDS)` (diff-only snippets; not enforced by CI — review by hand).
  - `examples/daqiri_bench_raw_tx_rx_reorder_seq_1024.yaml` ↔ `### Packet reordering on the GPU` (diff-only snippets; not enforced by CI — review by hand).

### Landing page and navigation (low frequency, high visibility)
- `mkdocs.yml` — nav entries should match actual files in `docs/` (the CI gate enforces this; the rule is here for awareness).
- `docs/index.html` — the landing page links to tutorials and API docs by relative path. If any doc file is renamed or moved, update the landing page links.

### Top-level README (low frequency, high visibility)

`README.md` summarizes the build flow, backend list, and links into the docs. It typically needs updating when:
- A backend is added or removed under `src/managers/*/` — update the **Backends** table.
- CMake options change in `src/CMakeLists.txt` — update the **Quick Start** commands.
- A doc file is added/removed/renamed in `docs/` — update the **Documentation** table.
- A major user-facing capability is added/removed (typically a public-API change in `include/daqiri/common.h`) — update the **Features** list.
- TX-fill mode or backend support changes — update the **Limitations** section.

### AGENTS.md (always-loaded developer onboarding)

`AGENTS.md` is the developer-facing onboarding doc that coding agents load automatically into every session in this repo. It typically needs updating when:
- CMake options or `DAQIRI_MGR` default change in `src/CMakeLists.txt` — update the **Build & run** section.
- A benchmark executable or its typical config is added/removed under `examples/` — update the **Benchmarks** table.
- Public API or types change in `include/daqiri/common.h` / `include/daqiri/types.h` / `src/manager.h` — update the **Architecture** section (Manager abstraction, BurstParams, public-API summary).
- A backend is added or its semantics change under `src/managers/*/` — update the Manager-abstraction discussion in **Architecture**.
- Reorder or quantize kernels change in `src/kernels.cu` — update the **Reorder & quantize kernels** subsection.
- A doc file is added/removed/renamed in `docs/` — update the **Documentation** section's layout list and the drift-hotspot paths.
- Public-API limitations change (e.g., TX-fill grows beyond UDP) — update the **Current limitations** list.

## Mapping quick reference

| Source file | Docs to check |
|---|---|
| `include/daqiri/common.h` / `include/daqiri/daqiri.h` | `docs/api-reference/index.md`, `docs/api-reference/cpp.md`, `docs/concepts.md`, `AGENTS.md` |
| `include/daqiri/types.h` | `docs/api-reference/index.md`, `docs/api-reference/cpp.md`, `docs/concepts.md`, `AGENTS.md` |
| `src/manager.h` | `docs/api-reference/cpp.md`, `docs/concepts.md`, `AGENTS.md` |
| `src/managers/*/` | `docs/getting-started.md`, `docs/concepts.md` (backend list + maturity), `docs/api-reference/configuration.md`, `docs/tutorials/configuration-walkthrough.md`, `README.md`, `AGENTS.md` |
| `src/CMakeLists.txt` | `docs/getting-started.md`, `AGENTS.md`, `README.md` |
| `src/kernels.cu` | `docs/tutorials/benchmarking_examples.md`, `AGENTS.md` |
| `examples/*.cpp` | `docs/tutorials/benchmarking_examples.md`, `docs/tutorials/configuration-walkthrough.md`, `AGENTS.md` |
| `examples/*.yaml` | `docs/tutorials/benchmarking_examples.md`, `docs/tutorials/configuration-walkthrough.md`, `AGENTS.md` |
| `mkdocs.yml` | `docs/index.html` (nav links) |
| Any `docs/*` rename/move | `README.md` (Documentation table), `AGENTS.md` (Documentation section), `mkdocs.yml`, `docs/index.html` |
