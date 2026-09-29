# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"""Platform coverage for raw-ibverbs runtime named-endpoint transmission."""

from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

import pytest


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
CONFIG_TEMPLATE = (
    REPOSITORY_ROOT / "examples/daqiri_example_named_endpoints_tx_rx.yaml"
)
DEFAULT_EXECUTABLE = (
    REPOSITORY_ROOT / "build/examples/daqiri_example_named_endpoints"
)
TX_PACKETS = re.compile(r"^TX complete:.*\bpackets=(\d+)\b", re.MULTILINE)


def _required_platform_test() -> bool:
    return os.environ.get("DAQIRI_PLATFORM_REQUIRE_NAMED_ENDPOINTS", "").lower() in {
        "1",
        "true",
        "yes",
        "on",
    }


def _unavailable(message: str) -> None:
    if _required_platform_test():
        pytest.fail(message)
    pytest.skip(message)


def _platform_cpus() -> list[int]:
    configured = os.environ.get("DAQIRI_PLATFORM_CPU_CORES")
    if configured:
        try:
            cpus = [int(value.strip()) for value in configured.split(",")]
        except ValueError:
            pytest.fail("DAQIRI_PLATFORM_CPU_CORES must contain comma-separated integers")
    else:
        cpus = sorted(os.sched_getaffinity(0))
    if len(cpus) < 5 or len(set(cpus[:5])) != 5:
        _unavailable("named-endpoint platform test requires five distinct allowed CPUs")
    return cpus[:5]


def _device_mac(bdf: str) -> str:
    configured = os.environ.get("DAQIRI_PLATFORM_NAMED_ENDPOINT_DST_MAC")
    if configured:
        return configured
    addresses = sorted(Path(f"/sys/bus/pci/devices/{bdf}/net").glob("*/address"))
    if not addresses:
        _unavailable(
            f"no netdev MAC found for {bdf}; set DAQIRI_PLATFORM_NAMED_ENDPOINT_DST_MAC"
        )
    return addresses[0].read_text(encoding="utf-8").strip()


def _materialize_config(tmp_path: Path, bdf: str, mac: str, cpus: list[int]) -> Path:
    text = CONFIG_TEMPLATE.read_text(encoding="utf-8")
    replacements = {
        "<0000:00:00.0>": bdf,
        "<00:00:00:00:00:00>": mac,
        "<1.2.3.4>": "192.0.2.1",
        "<5.6.7.8>": "192.0.2.2",
        "<3>": str(cpus[0]),
        "<11>": str(cpus[1]),
        "<9>": str(cpus[2]),
        "<8>": str(cpus[3]),
        "<10>": str(cpus[4]),
    }
    for placeholder, value in replacements.items():
        if placeholder not in text:
            pytest.fail(f"named-endpoint config placeholder changed: {placeholder}")
        text = text.replace(placeholder, value)

    # Keep the checked-in topology and endpoint fields while bounding platform-test
    # allocation and runtime. The example remains a device-memory/GPUDirect send.
    text = text.replace("num_bufs: 51200", "num_bufs: 512")
    text = text.replace("batch_size: 10240", "batch_size: 8")
    config = tmp_path / CONFIG_TEMPLATE.name
    config.write_text(text, encoding="utf-8")
    return config


@pytest.mark.platform
def test_named_endpoint_initialization_and_send(tmp_path: Path) -> None:
    """Initialize the checked-in config and complete at least one endpoint-aware send."""

    bdf = os.environ.get("DAQIRI_PLATFORM_IBVERBS_BDF")
    if not bdf:
        _unavailable("set DAQIRI_PLATFORM_IBVERBS_BDF to an mlx5 PCI BDF")
    bdf = bdf.removeprefix("/sys/bus/pci/devices/")

    executable = Path(
        os.environ.get("DAQIRI_NAMED_ENDPOINTS_EXAMPLE", str(DEFAULT_EXECUTABLE))
    )
    if not executable.is_file():
        _unavailable(f"named-endpoint example executable not found: {executable}")

    config = _materialize_config(tmp_path, bdf, _device_mac(bdf), _platform_cpus())
    environment = os.environ.copy()
    build_root = executable.parent.parent
    build_library = build_root / "src/libdaqiri.so"
    if build_library.exists():
        library_dirs = [build_root / "src", build_root / "src/third_party/yaml-cpp"]
        prior = environment.get("LD_LIBRARY_PATH")
        environment["LD_LIBRARY_PATH"] = ":".join(
            [*(str(path) for path in library_dirs), *([prior] if prior else [])]
        )
    result = subprocess.run(
        [str(executable), str(config), "--seconds", "1", "--target-gbps", "0.01"],
        check=False,
        capture_output=True,
        env=environment,
        text=True,
        timeout=30,
    )
    output = result.stdout + result.stderr
    assert result.returncode == 0, output
    assert "daqiri_init failed" not in output, output
    assert "named send_tx_burst failed" not in output, output
    assert "Named-endpoint TX completion drain succeeded" in output, output
    packet_counts = [int(count) for count in TX_PACKETS.findall(output)]
    assert packet_counts and max(packet_counts) > 0, output
