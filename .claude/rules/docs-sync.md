# Documentation Sync Rules

These rules ensure documentation stays in sync with code changes in the DAQIRI repository.

> **Path freshness.** The doc paths listed below (e.g. `docs/api-guide.md`, `docs/getting-started.md`, `docs/tutorials/*.md`) reflect the repo layout at the time of writing. If a doc has been renamed, moved, or split, treat the path as a starting point: confirm the current location with `grep -r` or by reading `mkdocs.yml`'s nav before acting on it. The CI gate in `.github/workflows/docs.yml` enforces internal-link/anchor/nav correctness on every PR.

## When to check for doc impact

When the user is committing, pushing, or otherwise wrapping up a change that touches the files below, consider whether the related docs may need updating. The intent is a single check at the commit/PR checkpoint, not after every individual edit.

### API surface changes (high impact)
- `src/common.h` — public free-function API (`get_rx_burst`, `get_packet_ptr`, `set_udp_header`, etc.). Any signature change, new function, removed function, or changed parameter semantics may need updating in:
  - `docs/api-guide.md` (markdown reference)
  - `docs/daqiri-api.html` (standalone HTML API page)
  - `CLAUDE.md` (Architecture section's API summary)
- `src/types.h` — public types (`BurstParams`, `Status`, `NetworkConfig`, enums like `MemoryKind`, `Direction`, `SocketProtocol`). Struct field changes, new enum values, or renamed types may need updating in the API docs and `CLAUDE.md` (Architecture / BurstParams discussion).
- `src/manager.h` — `Manager` virtual interface and `ManagerFactory`. Changes here affect the backend abstraction docs in `docs/api-guide.md` and the Manager-abstraction subsection in `CLAUDE.md`.

### Backend and build changes (medium impact)
- `src/managers/*/` — adding a new backend or changing backend behavior may need updating in:
  - `docs/getting-started.md` (build instructions, backend selection)
  - `docs/configuration.md` (YAML config options)
  - `docs/tutorials/configuration-walkthrough.md` (tutorial config walkthrough)
  - `README.md` (Backends table)
  - `CLAUDE.md` (Manager abstraction / backend descriptions)
- `src/CMakeLists.txt` or `CMakeLists.txt` — changes to CMake options (`DAQIRI_MGR`, `DAQIRI_BUILD_PYTHON`, CUDA arch list, new dependencies) may need updating in:
  - `docs/getting-started.md` (build instructions)
  - `CLAUDE.md` (build section)
  - `README.md` (Quick Start commands)
- `src/kernels.cu` / `src/kernels.h` — reorder or quantize kernel changes affect `docs/tutorials/benchmarking_examples.md` and the Reorder & quantize kernels subsection in `CLAUDE.md`.

### Benchmark and example changes (medium impact)
- `examples/*.cpp` or `examples/*.yaml` — new benchmarks, changed CLI flags, or new YAML config keys may need updating in:
  - `docs/tutorials/benchmarking_examples.md`
  - `docs/tutorials/configuration-walkthrough.md` — when adding or removing a YAML, add or remove a leaf in the **"Choosing an example config"** decision tree (`#choosing-an-example-config`). CI's `scripts/check_doc_refs.py` enforces that every YAML in `examples/` is referenced in this file; a new config without a tree leaf will fail the check.
  - `CLAUDE.md` (benchmark table)
- When adding or removing a benchmark executable, also update the benchmark table in `CLAUDE.md`.

### Landing page and navigation (low frequency, high visibility)
- `mkdocs.yml` — nav entries should match actual files in `docs/` (the CI gate enforces this; the rule is here for awareness).
- `docs/index.html` — the landing page links to tutorials and API docs by relative path. If any doc file is renamed or moved, update the landing page links.

### Top-level README (low frequency, high visibility)

`README.md` summarizes the build flow, backend list, and links into the docs. It typically needs updating when:
- A backend is added or removed under `src/managers/*/` — update the **Backends** table.
- CMake options change in `src/CMakeLists.txt` — update the **Quick Start** commands.
- A doc file is added/removed/renamed in `docs/` — update the **Documentation** table.
- A major user-facing capability is added/removed (typically a public-API change in `src/common.h`) — update the **Features** list.
- TX-fill mode or backend support changes — update the **Limitations** section.

### CLAUDE.md (always-loaded developer onboarding)

`CLAUDE.md` is the developer-facing onboarding doc that Claude Code loads automatically into every session in this repo. It typically needs updating when:
- CMake options or `DAQIRI_MGR` default change in `src/CMakeLists.txt` — update the **Build & run** section.
- A benchmark executable or its typical config is added/removed under `examples/` — update the **Benchmarks** table.
- Public API or types change in `src/common.h` / `src/types.h` / `src/manager.h` — update the **Architecture** section (Manager abstraction, BurstParams, public-API summary).
- A backend is added or its semantics change under `src/managers/*/` — update the Manager-abstraction discussion in **Architecture**.
- Reorder or quantize kernels change in `src/kernels.cu` — update the **Reorder & quantize kernels** subsection.
- A doc file is added/removed/renamed in `docs/` — update the **Documentation** section's layout list and the drift-hotspot paths.
- Public-API limitations change (e.g., TX-fill grows beyond UDP) — update the **Current limitations** list.

## Mapping quick reference

| Source file | Docs to check |
|---|---|
| `src/common.h` | `docs/api-guide.md`, `docs/daqiri-api.html`, `CLAUDE.md` |
| `src/types.h` | `docs/api-guide.md`, `docs/daqiri-api.html`, `CLAUDE.md` |
| `src/manager.h` | `docs/api-guide.md`, `CLAUDE.md` |
| `src/managers/*/` | `docs/getting-started.md`, `docs/configuration.md`, `docs/tutorials/configuration-walkthrough.md`, `README.md`, `CLAUDE.md` |
| `src/CMakeLists.txt` | `docs/getting-started.md`, `CLAUDE.md`, `README.md` |
| `src/kernels.cu` | `docs/tutorials/benchmarking_examples.md`, `CLAUDE.md` |
| `examples/*.cpp` | `docs/tutorials/benchmarking_examples.md`, `docs/tutorials/configuration-walkthrough.md`, `CLAUDE.md` |
| `examples/*.yaml` | `docs/tutorials/benchmarking_examples.md`, `docs/tutorials/configuration-walkthrough.md`, `CLAUDE.md` |
| `mkdocs.yml` | `docs/index.html` (nav links) |
| Any `docs/*` rename/move | `README.md` (Documentation table), `CLAUDE.md` (Documentation section), `mkdocs.yml`, `docs/index.html` |
