# Documentation Sync Rules

Keep documented behavior accurate while keeping introductory pages concise.
This file is the source of truth for doc-impact review; Greptile should apply
these conditions rather than require edits based only on changed file paths.

> **Path freshness.** Confirm the current location of a referenced page with
> `rg` or `mkdocs.yml` before editing it. CI checks internal links, anchors,
> navigation, example coverage, and the reproduced base walkthrough config.

## When to check for doc impact

Review doc impact once when wrapping up a change for a commit or PR. Source
paths below identify places to inspect, not a checklist of files to modify.
Update a page in the same PR only when the change makes an existing statement,
example, command, or documented contract inaccurate, or when a new user-facing
API/configuration option needs a reference entry. Internal refactors, bug fixes
that restore documented behavior, and implementation-only tuning can need no
doc edits. A doc file missing from the diff is not itself evidence of drift.

Put details in the page that owns the subject. Use a short link from another
page only when its reader needs that information to complete the page's task.
Avoid copying feature descriptions, option lists, and platform caveats across
introductory pages. No-op doc edits or mandatory doc-impact declarations are
not required.

## Page scope

- **`README.md`** is a short project overview and entry point. Change it only
  when its existing build commands, engine list, major capability summary,
  limitations, or documentation links become inaccurate. Small features, new
  optional flags, and additional platform reports belong in detailed docs;
  adding a page does not automatically require a README entry.
- **`docs/getting-started.md`** covers system requirements, library build steps,
  and the first benchmark smoke test. Only add information that directly affects
  those instructions. Engine implementation details, advanced tuning, optional
  features, and performance results belong in their subject pages.
- **`docs/api-reference/configuration.md`** owns YAML fields, defaults,
  constraints, and engine-specific configuration behavior.
- **`docs/api-reference/cpp.md` / `python.md`** own the public API and bindings.
  Update `docs/api-reference/index.md` only when the application lifecycle or
  configuration-first usage model changes.
- **`docs/concepts.md`** owns terminology and conceptual relationships. Update
  it for new or changed concepts or an inaccurate support summary, rather than
  every engine behavior change.
- **`docs/tutorials/configuration-walkthrough.md`** teaches configuration through
  selected examples. Apply the specific rules below; exhaustive option details
  belong in the configuration reference.
- **Build, system, and application tutorials** own their respective advanced
  instructions. For `scripts/` and `applications/` changes, correct existing
  documentation only; creating or expanding documentation requires a separate
  explicit request. A new script or application alone does not require a new
  page, section, tutorial, or overview entry. For example, the full CMake option table belongs in
  `docs/tutorials/bare-metal-cmake-build.md`, and config generator usage belongs
  in `docs/config-generation.md`.
- **`docs/benchmarks/`** owns run instructions, CLI flags, measurement methods,
  and performance reports. Use the raw guide for raw Ethernet, the socket guide
  for UDP/TCP/RDMA, and platform reports for platform-specific results.
- **`AGENTS.md`** owns developer workflow and architecture orientation. Update
  existing commands, benchmark entries, architectural contracts, and limitations
  when they change; keep small feature details in the relevant reference docs.

## Source changes and documentation to inspect

Each row is conditional on the affected page scope above.

| Source change | Primary documentation and conditions |
|---|---|
| Public functions in `include/daqiri/common.h` / `include/daqiri/daqiri.h` | C++ reference for signatures, semantics, and call patterns; Python reference only if exposed in bindings; API guide only if lifecycle changes; concepts only if terminology changes |
| Public types in `include/daqiri/types.h` | Relevant API/configuration reference for changed fields, enum values, or semantics; concepts only for changed terminology |
| `python/daqiri_common_pybind.cpp` | Python reference for functions, enums/classes, signatures, and GIL behavior |
| YAML parsing/defaults in `src/common.cpp`, shared validation in `src/engine.cpp` | Configuration reference when accepted fields, defaults, constraints, or validation behavior change |
| `src/engine.h` | C++ reference only for behavior visible through the public API; AGENTS architecture only for changed engine contracts |
| `src/engines/*/` | Configuration or API reference for changed user-facing behavior; concepts for changed engine/stream support; Getting Started and README only if their scoped instructions or summaries change |
| `CMakeLists.txt` / component `CMakeLists.txt` files | Full CMake tutorial for changed options/defaults/dependencies; Getting Started, README, and AGENTS only if their existing build instructions or requirements are affected |
| `src/kernels.cu` / `src/kernels.h` | Relevant configuration/API reference or benchmark guide for changed reorder/quantize behavior visible to users; no automatic update for kernel tuning |
| `examples/*.cpp`, `examples/*.yaml`, shared bench helpers | Relevant benchmark guide for changed binaries, flags, run instructions, or measurements; configuration reference for new library YAML keys; walkthrough only under the rules below; AGENTS table when its executable/config entries change |
| `examples/*.py` | Relevant example usage or Python reference only where the documented workflow changes |
| `scripts/`, including `scripts/gen_daqiri_config.py` and `scripts/daqiri_config/` | Correct existing documented commands, accepted inputs, generated output, or workflows when they become inaccurate; use the existing config-generation guide or relevant workflow page; no new documentation unless explicitly requested |
| `applications/` | Correct existing application documentation when dependencies, configuration, or run instructions become inaccurate; no new documentation unless explicitly requested |
| `mkdocs.yml`, docs additions/removals/renames/moves | Update affected nav entries and existing inbound links; README, AGENTS, `docs/index.md`, and `docs/landing/` only where they contain affected links or layout descriptions |

Adding a nav entry does not require adding the same link to the landing page,
README, or AGENTS. When a linked page moves, fix every actual affected link.
New tutorial pages should match sibling `hide: [navigation]` frontmatter.

## Configuration walkthrough rules

Keep the walkthrough focused on choosing a retained example, understanding its
fields, and adapting it successfully. A new engine capability, CLI flag, or YAML
key alone does not require a new tutorial paragraph or section. Update existing
explanations when their examples or instructions become inaccurate; use a brief
reference link for details needed to follow the example.

- **Retained example coverage.** Adding, removing, or renaming a YAML in
  `examples/` requires updating its entry under **Choosing an example config**
  (`#choosing-an-example-config`). Every retained YAML must have a decision-tree
  entry. `scripts/check_doc_refs.py` checks filename coverage across the whole
  tutorial; entry placement and consuming-binary accuracy need manual review.
  Keep entries short: use
  case, file, consuming binary, and a link to detailed run instructions where
  needed. Generated platform/queue/transform variants do not each need entries.
- **Base TX+RX pairing.** The full YAML block under **Base TX+RX config** must
  match `examples/daqiri_bench_raw_tx_rx.yaml`. CI compares parsed structure after
  stripping annotation markers. Pair source changes with the reproduced block
  and adjust annotations only where their explanation changes.
- **HDS pairing.** Manually check snippets and explanations under **Header-data
  split (HDS)** against `examples/daqiri_bench_raw_tx_rx_hds.yaml`. Update only
  the reproduced fields and explanations affected by the source change.
- **Reorder pairing.** Manually check snippets and explanations under
  **Hardware packet reordering** against
  `examples/daqiri_bench_raw_tx_rx_reorder_seq_1024.yaml`. Update only the
  reproduced fields and explanations affected by the source change. Confirm
  the current section title if the tutorial is reorganized.

The HDS and reorder snippets are partial examples, so their equality is not
checked by CI. Preserve the base pairing and retained example coverage checks;
relaxing tutorial scope does not waive these correctness requirements.

## Review findings

For a missing-doc finding, identify the changed user-facing behavior and the
specific stale statement, example, or missing reference entry, then request the
smallest necessary update to the page that owns it. Consolidate related drift
into one comment. Do not request every page mapped to a touched source path,
assign high priority merely because a mapped file was untouched, or demand
introductory/landing content for each feature or nav addition.
