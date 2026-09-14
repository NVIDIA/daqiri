# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import pytest
import yaml


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]

from scripts.daqiri_config import (
    ConfigError,
    apply_overrides,
    render_document,
    validate_document,
)


def minimal_document() -> dict:
    return {
        "daqiri": {
            "cfg": {
                "version": 1,
                "stream_type": "raw",
                "master_core": 1,
                "memory_regions": [
                    {
                        "name": "RX",
                        "kind": "host",
                        "affinity": 0,
                        "num_bufs": 8,
                        "buf_size": 2048,
                    }
                ],
                "interfaces": [
                    {
                        "name": "rx",
                        "address": "0005:03:00.1",
                        "rx": {
                            "queues": [
                                {
                                    "name": "rxq0",
                                    "id": 0,
                                    "cpu_core": 2,
                                    "batch_size": 4,
                                    "memory_regions": ["RX"],
                                }
                            ]
                        },
                    }
                ],
            }
        },
        "application_owned": {"arbitrary": True},
    }


def test_application_owned_sections_are_not_interpreted() -> None:
    validate_document(minimal_document())


def test_unknown_nested_daqiri_key_is_rejected_with_path() -> None:
    document = minimal_document()
    queue = document["daqiri"]["cfg"]["interfaces"][0]["rx"]["queues"][0]
    queue["batch_sise"] = queue.pop("batch_size")
    with pytest.raises(ConfigError, match=r"interfaces\[0\].rx.queues\[0\].*batch_sise"):
        validate_document(document)


def test_overrides_fill_typed_placeholders() -> None:
    document = minimal_document()
    document["daqiri"]["cfg"]["master_core"] = "<core>"
    apply_overrides(document, ["/daqiri/cfg/master_core=3"])
    validate_document(document)
    assert document["daqiri"]["cfg"]["master_core"] == 3


def test_override_rejects_unknown_path() -> None:
    with pytest.raises(ConfigError, match="does not exist"):
        apply_overrides(minimal_document(), ["/daqiri/cfg/master_cores=3"])


def test_render_rejects_application_owned_placeholders() -> None:
    document = minimal_document()
    document["application_owned"]["address"] = "<peer-address>"
    with pytest.raises(ConfigError, match="/application_owned/address"):
        render_document(document)


def test_render_is_byte_deterministic_and_quotes_pci_bdf() -> None:
    first = render_document(minimal_document())
    second = render_document(minimal_document())
    assert first == second
    assert first.startswith("%YAML 1.2\n---\n")
    assert "address: '0005:03:00.1'" in first
    assert yaml.safe_load(first)["application_owned"] == {"arbitrary": True}


def test_render_cli_accepts_bare_network_mapping(tmp_path: Path) -> None:
    bare = minimal_document()["daqiri"]["cfg"]
    source = tmp_path / "input.yaml"
    source.write_text(yaml.safe_dump(bare, sort_keys=False), encoding="utf-8")
    result = subprocess.run(
        [
            sys.executable,
            str(REPOSITORY_ROOT / "scripts/gen_daqiri_config.py"),
            "render",
            str(source),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    assert yaml.safe_load(result.stdout) == bare
