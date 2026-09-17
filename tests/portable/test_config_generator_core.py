# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import shutil
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


def test_overrides_fill_typed_placeholders() -> None:
    document = minimal_document()
    document["daqiri"]["cfg"]["master_core"] = "<core>"
    apply_overrides(document, ["/daqiri/cfg/master_core=3"])
    assert document["daqiri"]["cfg"]["master_core"] == 3


def test_override_rejects_unknown_path() -> None:
    with pytest.raises(ConfigError, match="does not exist"):
        apply_overrides(minimal_document(), ["/daqiri/cfg/master_cores=3"])


@pytest.mark.parametrize("index", ["-1", "01", "+1", "2"])
def test_override_rejects_invalid_array_index(index: str) -> None:
    with pytest.raises(ConfigError, match="does not exist"):
        apply_overrides(
            minimal_document(),
            [f"/daqiri/cfg/interfaces/{index}/name=changed"],
        )


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


def test_installed_launcher_supports_custom_gnu_data_directory(tmp_path: Path) -> None:
    prefix = tmp_path / "prefix"
    bindir = prefix / "tools"
    module_root = prefix / "libdata" / "daqiri" / "config-generator"
    bindir.mkdir(parents=True)
    shutil.copytree(
        REPOSITORY_ROOT / "scripts/daqiri_config",
        module_root / "daqiri_config",
        ignore=shutil.ignore_patterns("__pycache__"),
    )

    launcher = (REPOSITORY_ROOT / "scripts/gen_daqiri_config.py").read_text(
        encoding="utf-8"
    )
    launcher = launcher.replace(
        "@DAQIRI_CONFIG_GENERATOR_MODULE_HINT@",
        "../libdata/daqiri/config-generator",
    )
    launcher_path = bindir / "gen_daqiri_config.py"
    launcher_path.write_text(launcher, encoding="utf-8")

    source = tmp_path / "input.yaml"
    source.write_text(
        yaml.safe_dump(minimal_document(), sort_keys=False),
        encoding="utf-8",
    )

    result = subprocess.run(
        [sys.executable, str(launcher_path), "render", str(source)],
        check=True,
        capture_output=True,
        text=True,
    )
    assert yaml.safe_load(result.stdout) == minimal_document()


@pytest.mark.parametrize(
    "transport,extra,error",
    [
        ("roce", ["--rx-batch-size", "32"], "supported only for TCP/UDP"),
        ("roce", ["--iterations", "10"], "supported only for TCP/UDP"),
        ("udp", ["--rx-depth", "7"], "supported only for RoCE"),
        ("udp", ["--tx-depth", "9"], "supported only for RoCE"),
        ("tcp", ["--roce-transport-mode", "UD"], "supported only for RoCE"),
    ],
)
def test_socket_cli_rejects_inapplicable_transport_options(
    transport: str, extra: list[str], error: str
) -> None:
    result = subprocess.run(
        [
            sys.executable,
            str(REPOSITORY_ROOT / "scripts/gen_daqiri_config.py"),
            "socket-pair",
            "--transport",
            transport,
            "--client-address",
            "10.0.0.1",
            "--server-address",
            "10.0.0.2",
            "--client-port",
            "5002",
            "--server-port",
            "5001",
            "--client-master-core",
            "1",
            "--server-master-core",
            "2",
            "--client-rx-core",
            "3",
            "--client-tx-core",
            "4",
            "--server-rx-core",
            "5",
            "--server-tx-core",
            "6",
            "--client-worker-core",
            "7",
            "--server-worker-core",
            "8",
            "--message-size",
            "1024",
            "--buffer-size",
            "2048",
            "--num-bufs",
            "128",
            "--role",
            "tx",
            *extra,
        ],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 2
    assert error in result.stderr
