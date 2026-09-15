# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import pytest

from python import tune_system


@pytest.mark.parametrize(
    ("address", "expected"),
    [
        ("0005:03:00.1", "0005:03:00.1"),
        ("00000005:0A:0B.7", "0005:0a:0b.7"),
        ("/sys/bus/pci/devices/0000:17:00.0", "0000:17:00.0"),
        ("17:00.0", None),
        ("0000:17:00.8", None),
    ],
)
def test_normalize_pci_address(address: str, expected: str | None) -> None:
    assert tune_system.normalize_pci_address(address) == expected


def test_parse_lspci_link_line() -> None:
    line = "LnkSta: Speed 32GT/s, Width x8 (downgraded)"
    assert tune_system.parse_lspci_link_line(line) == ("32GT/s", "x8")


@pytest.mark.parametrize(
    ("speed", "width", "generation", "bandwidth"),
    [
        ("16GT/s", "x16", "Gen4", pytest.approx(31.504)),
        ("32GT/s", "x8", "Gen5", pytest.approx(31.504)),
        ("unknown", "x8", "", None),
    ],
)
def test_pcie_link_calculations(
    speed: str,
    width: str,
    generation: str,
    bandwidth: float | None,
) -> None:
    assert tune_system.pcie_generation(speed) == generation
    assert tune_system.pcie_bandwidth_gb_per_sec(speed, width) == bandwidth
