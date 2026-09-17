#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"""Validate checked-in DAQIRI YAMLs with the authoritative C++ decoder."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

import yaml

from daqiri_config import ConfigError, load_document


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
PLACEHOLDER = re.compile(r"^<[^>]+>$")
INTEGER_FIELDS = {"master_core", "cpu_core", "affinity"}


def materialize_typed_placeholders(value: Any, key: str | None = None) -> Any:
    """Replace teaching-template integer placeholders before runtime parsing."""

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
    parser.add_argument(
        "--validator",
        type=Path,
        required=True,
        help="built daqiri_config_validate binary",
    )
    parser.add_argument(
        "--exclude-dpdk",
        action="store_true",
        help="skip configurations that explicitly require DPDK",
    )
    parser.add_argument("paths", nargs="*", type=Path)
    args = parser.parse_args(argv)
    paths = args.paths or default_paths()

    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="daqiri-checked-configs-") as temp_dir:
        materialized_paths: list[Path] = []
        for index, path in enumerate(paths):
            try:
                document = load_document(path)
            except ConfigError as exc:
                failures.append(f"{path}: {exc}")
                continue
            engine = document.get("daqiri", {}).get("cfg", {}).get("engine")
            if args.exclude_dpdk and engine == "dpdk":
                continue
            document = materialize_typed_placeholders(document)
            output_path = Path(temp_dir) / f"{index}-{path.name}"
            output_path.write_text(
                "%YAML 1.2\n---\n"
                + yaml.safe_dump(document, sort_keys=False, width=1000),
                encoding="utf-8",
            )
            materialized_paths.append(output_path)

        if not failures and materialized_paths:
            result = subprocess.run(
                [str(args.validator), *(str(path) for path in materialized_paths)]
            )
            if result.returncode != 0:
                return result.returncode

    if failures:
        print("\n\n".join(failures), file=sys.stderr)
        return 1
    print(
        f"Validated {len(materialized_paths)} checked-in configurations "
        "with the C++ runtime decoder."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
