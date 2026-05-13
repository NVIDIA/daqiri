#!/usr/bin/env python3
"""Verify that every daqiri_(bench|example)_<name> binary and *.yaml
config referenced in user-facing docs actually exists in examples/, that
every YAML in examples/ is reachable from the use-case decision tree at
docs/tutorials/configuration-walkthrough.md, and that the annotated base
TX+RX walkthrough YAML block matches examples/daqiri_bench_raw_tx_rx.yaml
verbatim.

Catches:
  * #37-class drift: tutorials referencing daqiri_bench_default when
    the actual binary is daqiri_bench_raw_gpudirect.
  * #65-class drift: a new YAML added to examples/ without a leaf in
    the "Choosing an example config" section. The decision tree's
    coverage promise is enforced here, not by mkdocs-strict.
  * Base-walkthrough drift: the annotated YAML block under "### Base
    TX+RX config" diverging from daqiri_bench_raw_tx_rx.yaml on disk
    (e.g. stale buf_size, removed fields, renamed keys). The HDS and
    reorder diff snippets are intentional fragments and are not checked
    here — keep them in sync via the docs-sync rule.

Scope:
  * docs/**/*.md, docs/**/*.html
  * README.md
  * (CLAUDE.md is intentionally skipped — its benchmark table uses
    wildcards and brace expansion that literal grep would mis-flag.
    The .claude/rules/docs-sync.md rule covers CLAUDE.md drift via a
    soft commit-time check for Claude Code users.)

Sources of truth:
  * Binary names: add_executable() and add_daqiri_raw_bench() calls in
    examples/CMakeLists.txt.
  * YAML config names: actual *.yaml files in examples/.
  * Base walkthrough YAML: examples/daqiri_bench_raw_tx_rx.yaml.

Exit codes:
  0  all references resolve, the decision tree covers every YAML, and
     the base walkthrough YAML block matches its source
  1  one or more references do not match a real binary or config, a
     YAML in examples/ is not referenced from the decision tree, or
     the base walkthrough YAML block diverges from its source
  2  could not parse the canonical lists (missing CMakeLists.txt etc.)

Usage: python scripts/check_doc_refs.py
"""

from __future__ import annotations

import difflib
import re
import sys
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parent.parent
EXAMPLES = REPO / "examples"
DOCS = REPO / "docs"
README = REPO / "README.md"
DECISION_TREE = DOCS / "tutorials" / "configuration-walkthrough.md"
BASE_WALKTHROUGH_YAML = EXAMPLES / "daqiri_bench_raw_tx_rx.yaml"

# Match daqiri_(bench|example)_<name>.yaml — the full filename.
# The bench/example alternation covers both bench configs and the
# daqiri_example_* family (gds_write, pcap_writer).
YAML_RE = re.compile(r"\b(daqiri_(?:bench|example)_\w+\.yaml)\b")

# Match daqiri_(bench|example)_<name> as a binary. Anchored by word
# boundary on both sides; the negative lookahead prevents a YAML
# basename from being treated as a binary (the "\b" before ".yaml"
# would otherwise allow the binary regex to match the same characters).
BIN_RE = re.compile(r"\b(daqiri_(?:bench|example)_\w+)\b(?!\.yaml)")

# Lines in examples/CMakeLists.txt that declare a benchmark binary,
# either via the helper add_daqiri_raw_bench(name ...) or directly via
# add_executable(name ...).
DEFN_RE = re.compile(
    r"(?:add_executable|add_daqiri_raw_bench)\s*\(\s*(daqiri_(?:bench|example)_\w+)"
)

# Match the ```yaml block under "### Base TX+RX config" in
# configuration-walkthrough.md. The opening fence may carry an
# hl_lines="..." attribute (or any other code-fence attributes); we
# only care about the block body. DOTALL lets `.` cross newlines.
BASE_WALKTHROUGH_BLOCK_RE = re.compile(
    r"###\s+Base TX\+RX config\b.*?\n```yaml(?:\s+[^\n]*)?\n(.*?)\n```",
    re.DOTALL,
)

# Match an MkDocs Material annotation marker on a line, e.g. " # (12)!"
# at end of line. Greedy whitespace lets us also consume the space
# separating the YAML value from the marker.
ANNOTATION_MARKER_RE = re.compile(r"\s*#\s*\(\d+\)!\s*$", re.MULTILINE)


def known_binaries() -> set[str]:
    cml = (EXAMPLES / "CMakeLists.txt").read_text(encoding="utf-8")
    return set(DEFN_RE.findall(cml))


def known_yamls() -> set[str]:
    return {p.name for p in EXAMPLES.glob("*.yaml")}


def doc_files() -> list[Path]:
    files: list[Path] = []
    files.extend(sorted(DOCS.rglob("*.md")))
    files.extend(sorted(DOCS.rglob("*.html")))
    if README.exists():
        files.append(README)
    return files


def base_walkthrough_drift() -> list[str]:
    """The annotated ```yaml block under '### Base TX+RX config' in
    configuration-walkthrough.md must parse to the same structure as
    examples/daqiri_bench_raw_tx_rx.yaml on disk. Drift here means the
    tutorial is teaching a YAML that no longer exists in the repo.

    Only the base walkthrough is checked; the HDS and reorder snippets
    are intentional fragments.
    """
    if not DECISION_TREE.exists():
        return [
            f"{DECISION_TREE.relative_to(REPO)}: file not found "
            f"(base walkthrough drift cannot be checked)"
        ]
    if not BASE_WALKTHROUGH_YAML.exists():
        return [
            f"{BASE_WALKTHROUGH_YAML.relative_to(REPO)}: file not found "
            f"(base walkthrough drift cannot be checked)"
        ]
    md = DECISION_TREE.read_text(encoding="utf-8")
    match = BASE_WALKTHROUGH_BLOCK_RE.search(md)
    if not match:
        return [
            f"{DECISION_TREE.relative_to(REPO)}: could not locate the "
            f"```yaml block under '### Base TX+RX config'. Check that "
            f"the section heading and code fence are intact."
        ]
    walkthrough_text = ANNOTATION_MARKER_RE.sub("", match.group(1))
    source_text = BASE_WALKTHROUGH_YAML.read_text(encoding="utf-8")
    try:
        walkthrough_data = yaml.safe_load(walkthrough_text)
        source_data = yaml.safe_load(source_text)
    except yaml.YAMLError as e:
        return [
            f"{DECISION_TREE.relative_to(REPO)}: could not parse YAML "
            f"(walkthrough block or source file): {e}"
        ]
    if walkthrough_data == source_data:
        return []

    # Build a unified diff between normalized YAML dumps so the failure
    # message points at the exact divergence.
    dump_kwargs = {"sort_keys": False, "default_flow_style": False}
    walkthrough_dump = yaml.safe_dump(walkthrough_data, **dump_kwargs)
    source_dump = yaml.safe_dump(source_data, **dump_kwargs)
    diff = "".join(
        difflib.unified_diff(
            walkthrough_dump.splitlines(keepends=True),
            source_dump.splitlines(keepends=True),
            fromfile=f"{DECISION_TREE.relative_to(REPO)} (base walkthrough block)",
            tofile=f"{BASE_WALKTHROUGH_YAML.relative_to(REPO)}",
            n=2,
        )
    )
    return [
        f"{DECISION_TREE.relative_to(REPO)}: base walkthrough YAML block "
        f"has drifted from {BASE_WALKTHROUGH_YAML.relative_to(REPO)}. "
        f"Update the walkthrough block or the YAML so they agree.\n{diff}"
    ]


def decision_tree_coverage(yamls: set[str]) -> list[str]:
    """Every YAML in examples/ must appear at least once in the use-case
    decision tree. A YAML added to examples/ without a leaf is a silent
    gap that mkdocs-strict won't catch.
    """
    if not DECISION_TREE.exists():
        return [
            f"{DECISION_TREE.relative_to(REPO)}: file not found "
            f"(decision tree coverage cannot be checked)"
        ]
    content = DECISION_TREE.read_text(encoding="utf-8")
    rel = DECISION_TREE.relative_to(REPO)
    return [
        f"{rel}: YAML config {name!r} is in examples/ but is not referenced "
        f"in the decision tree at #choosing-an-example-config"
        for name in sorted(yamls)
        if name not in content
    ]


def main() -> int:
    if not (EXAMPLES / "CMakeLists.txt").exists():
        print(
            "examples/CMakeLists.txt not found — cannot determine canonical binaries.",
            file=sys.stderr,
        )
        return 2

    bins = known_binaries()
    yamls = known_yamls()

    if not bins:
        print(
            "No add_executable(daqiri_bench_*) / add_daqiri_raw_bench(daqiri_bench_*) "
            "calls found in examples/CMakeLists.txt.",
            file=sys.stderr,
        )
        return 2

    errors: list[str] = []
    for path in doc_files():
        try:
            content = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue

        rel = path.relative_to(REPO)

        for match in YAML_RE.finditer(content):
            name = match.group(1)
            if name not in yamls:
                errors.append(f"{rel}: unknown YAML config {name!r}")

        for match in BIN_RE.finditer(content):
            name = match.group(1)
            if name not in bins:
                errors.append(f"{rel}: unknown binary {name!r}")

    errors = sorted(set(errors))
    coverage_errors = decision_tree_coverage(yamls)
    walkthrough_errors = base_walkthrough_drift()

    if errors or coverage_errors or walkthrough_errors:
        if errors:
            print("Stale benchmark/config references in docs:", file=sys.stderr)
            for e in errors:
                print(f"  {e}", file=sys.stderr)
        if coverage_errors:
            print("Decision tree missing YAML coverage:", file=sys.stderr)
            for e in coverage_errors:
                print(f"  {e}", file=sys.stderr)
        if walkthrough_errors:
            print("Base walkthrough YAML drift:", file=sys.stderr)
            for e in walkthrough_errors:
                print(f"  {e}", file=sys.stderr)
        return 1

    print(
        f"All daqiri_(bench|example)_* references resolve, the decision "
        f"tree covers every YAML, and the base walkthrough YAML block "
        f"matches its source. Checked {len(bins)} binaries and "
        f"{len(yamls)} YAML configs across {len(doc_files())} doc files."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
