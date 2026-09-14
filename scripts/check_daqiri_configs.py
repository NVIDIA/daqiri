#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"""Validate checked-in DAQIRI YAMLs against the machine-readable schema."""

from __future__ import annotations

import argparse
import copy
import re
import sys
from pathlib import Path
from typing import Any

from daqiri_config import ConfigError, load_document, validate_document


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
PLACEHOLDER = re.compile(r"^<[^>]+>$")
INTEGER_FIELDS = {"master_core", "cpu_core", "affinity"}


def materialize_typed_placeholders(value: Any, key: str | None = None) -> Any:
    """Replace teaching-template integer placeholders for structural validation."""

    if isinstance(value, dict):
        return {
            child_key: materialize_typed_placeholders(child, child_key)
            for child_key, child in value.items()
        }
    if isinstance(value, list):
        return [materialize_typed_placeholders(child, key) for child in value]
    if key in INTEGER_FIELDS and isinstance(value, str) and PLACEHOLDER.fullmatch(value):
        return 0
    return value


def default_paths() -> list[Path]:
    paths = sorted((REPOSITORY_ROOT / "examples").glob("daqiri_*.yaml"))
    paths.extend(
        sorted((REPOSITORY_ROOT / "applications").glob("**/configs/*.yaml"))
    )
    return paths


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*", type=Path)
    args = parser.parse_args(argv)
    paths = args.paths or default_paths()

    failures: list[str] = []
    for path in paths:
        try:
            document = load_document(path)
            validate_document(materialize_typed_placeholders(copy.deepcopy(document)))
        except ConfigError as exc:
            failures.append(f"{path}: {exc}")

    if failures:
        print("\n\n".join(failures), file=sys.stderr)
        return 1
    print(f"Validated {len(paths)} DAQIRI configuration file(s) against schema v1.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
