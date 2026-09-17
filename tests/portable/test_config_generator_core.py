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


def test_unknown_tx_offload_is_rejected() -> None:
    document = minimal_document()
    interface = document["daqiri"]["cfg"]["interfaces"][0]
    interface.pop("rx")
    interface["tx"] = {
        "queues": [
            {
                "name": "txq0",
                "id": 0,
                "cpu_core": 2,
                "batch_size": 4,
                "memory_regions": ["RX"],
                "offloads": ["tx_eth_scr"],
            }
        ]
    }
    with pytest.raises(ConfigError, match=r"offloads\[0\].*tx_eth_scr"):
        validate_document(document)


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("tx_meta_buffers", 1 << 32),
        ("rx_meta_buffers", 1 << 32),
        ("master_core", 1 << 31),
    ],
)
def test_fixed_width_top_level_overflow_is_rejected(field: str, value: int) -> None:
    document = minimal_document()
    document["daqiri"]["cfg"][field] = value
    with pytest.raises(ConfigError, match=field):
        validate_document(document)


def test_memory_affinity_overflow_is_rejected() -> None:
    document = minimal_document()
    config = document["daqiri"]["cfg"]
    config["memory_regions"][0]["affinity"] = 1 << 16
    with pytest.raises(ConfigError, match="affinity"):
        validate_document(document)


def test_dynamic_flow_capacity_overflow_is_rejected() -> None:
    document = minimal_document()
    config = document["daqiri"]["cfg"]
    config["interfaces"][0]["rx"]["dynamic_flow_capacity"] = 1 << 32
    with pytest.raises(ConfigError, match="dynamic_flow_capacity"):
        validate_document(document)


def test_socket_fixed_width_overflow_is_rejected() -> None:
    document = minimal_document()
    document["daqiri"]["cfg"]["interfaces"][0]["socket_config"] = {
        "mode": "client",
        "min_ipg_ns": 1 << 32,
    }
    with pytest.raises(ConfigError, match="min_ipg_ns"):
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
    schema_dir = prefix / "libdata" / "daqiri" / "schemas"
    bindir.mkdir(parents=True)
    schema_dir.mkdir(parents=True)
    shutil.copytree(
        REPOSITORY_ROOT / "scripts/daqiri_config",
        module_root / "daqiri_config",
        ignore=shutil.ignore_patterns("__pycache__"),
    )
    shutil.copy2(
        REPOSITORY_ROOT / "schemas/daqiri-config-v1.schema.json",
        schema_dir,
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
