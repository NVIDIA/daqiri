#!/usr/bin/env python3
"""Verify that every daqiri_(bench|example)_<name> binary and *.yaml
config referenced in user-facing docs actually exists in examples/, and
that every YAML in examples/ is reachable from the use-case decision
tree at docs/tutorials/configuration-walkthrough.md.

Catches:
  * #37-class drift: tutorials referencing daqiri_bench_default when
    the actual binary is daqiri_bench_raw_gpudirect.
  * #65-class drift: a new YAML added to examples/ without a leaf in
    the "Choosing an example config" section. The decision tree's
    coverage promise is enforced here, not by mkdocs-strict.

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

Exit codes:
  0  all references resolve and the decision tree covers every YAML
  1  one or more references do not match a real binary or config, or
     a YAML in examples/ is not referenced from the decision tree
  2  could not parse the canonical lists (missing CMakeLists.txt etc.)

Usage: python scripts/check_doc_refs.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
EXAMPLES = REPO / "examples"
DOCS = REPO / "docs"
README = REPO / "README.md"
DECISION_TREE = DOCS / "tutorials" / "configuration-walkthrough.md"

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

    if errors or coverage_errors:
        if errors:
            print("Stale benchmark/config references in docs:", file=sys.stderr)
            for e in errors:
                print(f"  {e}", file=sys.stderr)
        if coverage_errors:
            print("Decision tree missing YAML coverage:", file=sys.stderr)
            for e in coverage_errors:
                print(f"  {e}", file=sys.stderr)
        return 1

    print(
        f"All daqiri_(bench|example)_* references resolve and the decision "
        f"tree covers every YAML. Checked {len(bins)} binaries and "
        f"{len(yamls)} YAML configs across {len(doc_files())} doc files."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
