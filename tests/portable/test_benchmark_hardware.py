# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

from pathlib import Path

import pytest

from scripts.resolve_benchmark_hardware import (
    DEFAULTS_PATH,
    Gpu,
    Port,
    ResolveError,
    load_defaults,
    resolve,
    select_platform,
    select_port,
)


def port(netdev: str, bdf: str, phys_port: str, carrier: bool = True) -> Port:
    return Port(
        netdev=netdev,
        bdf=bdf,
        phys_port=phys_port,
        mac=f"02:00:00:00:00:{int(bdf[-1]) + 1:02x}",
        carrier=carrier,
    )


def test_checked_in_hardware_defaults_are_complete() -> None:
    platforms = load_defaults(DEFAULTS_PATH)
    assert set(platforms) == {"dgx-spark", "igx-thor"}
    assert platforms["dgx-spark"]["dpdk_memory_kind"] == "host_pinned"
    assert platforms["igx-thor"]["dpdk_memory_kind"] == "device"
    assert platforms["igx-thor"]["rdma_memory_kind"] == "host_pinned"
    assert platforms["igx-thor"]["isolated_cpus"] == "8-13"
    assert platforms["igx-thor"]["socket_client_cores"] == [9, 11, 13]
    assert platforms["igx-thor"]["socket_pair_counts"] == [1, 2, 3]


def test_platform_selection_is_explicit() -> None:
    platforms = load_defaults(DEFAULTS_PATH)
    assert select_platform("dgx-spark", platforms) == "dgx-spark"
    assert select_platform("igx-thor", platforms) == "igx-thor"
    with pytest.raises(ResolveError, match="unknown platform"):
        select_platform("generic", platforms)


def test_preferred_bdf_wins_with_duplicate_spark_port_representations() -> None:
    ports = [
        port("p0-low", "0000:01:00.0", "p0"),
        port("p0-high", "0002:01:00.0", "p0"),
        port("p1-low", "0000:01:00.1", "p1"),
        port("p1-high", "0002:01:00.1", "p1"),
    ]
    assert select_port(ports, "p0", "0000:01:00.0", None).netdev == "p0-low"
    assert select_port(ports, "p1", "0002:01:00.1", None).netdev == "p1-high"


def test_port_selection_falls_back_deterministically_and_requires_carrier() -> None:
    ports = [
        port("p0-high", "0002:01:00.0", "p0"),
        port("p0-low", "0000:01:00.0", "p0"),
        port("p1-down", "0000:01:00.1", "p1", carrier=False),
    ]
    assert select_port(ports, "p0", "missing", None).netdev == "p0-low"
    with pytest.raises(ResolveError, match="no carrier-up"):
        select_port(ports, "p1", "missing", None)
    with pytest.raises(ResolveError, match="requested BDF"):
        select_port(ports, "p0", "0000:01:00.0", "0009:09:00.0")


def test_thor_resolution_selects_discrete_gpu_and_device_memory(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.delenv("DAQIRI_GPU_UUID", raising=False)
    monkeypatch.delenv("DPDK_TX_PCI", raising=False)
    monkeypatch.delenv("DPDK_RX_PCI", raising=False)
    result = resolve(
        load_defaults(DEFAULTS_PATH),
        "igx-thor",
        [
            Gpu("NVIDIA Thor", "GPU-INTEGRATED"),
            Gpu("NVIDIA RTX PRO 6000 Blackwell Max-Q Workstation Edition", "GPU-DISCRETE"),
        ],
        [
            port("enP4p3s0f0np0", "0004:03:00.0", "p0"),
            port("enP4p3s0f1np1", "0004:03:00.1", "p1"),
        ],
        skip_nic=False,
    )
    assert result["BENCH_PLATFORM"] == "igx-thor"
    assert result["DEFAULT_GPU_UUID"] == "GPU-DISCRETE"
    assert result["DEFAULT_DPDK_MEMORY_KIND"] == "device"
    assert result["DEFAULT_DPDK_PAYLOAD_BATCHES"] == "256:1024 64:1024"
    assert result["DEFAULT_DPDK_PAYLOAD_PACING_GBPS"] == "256:50 64:20"
    assert result["DEFAULT_IBVERBS_MEMORY_KIND"] == "device"
    assert result["DEFAULT_IBVERBS_BATCH_SIZE"] == 4096
    assert result["DEFAULT_IBVERBS_NUM_BUFS"] == 8192
    assert result["DEFAULT_IBVERBS_PAYLOAD_BATCHES"] == "1024:512 256:256 64:512"
    assert result["DEFAULT_IBVERBS_PAYLOAD_PACING_GBPS"] == "256:125 64:65"
    assert result["DEFAULT_ETH_SRC_ADDR"] == "02:00:00:00:00:01"
    assert result["DEFAULT_ETH_DST_ADDR"] == "02:00:00:00:00:02"
    assert result["DEFAULT_DPDK_RX_NETDEV"] == "enP4p3s0f1np1"
    assert result["DEFAULT_SOCKET_HEADLINE_PAIRS"] == "3"


def test_defaults_loader_rejects_incomplete_document(tmp_path: Path) -> None:
    path = tmp_path / "defaults.yaml"
    path.write_text("version: 1\nplatforms:\n  broken: {}\n", encoding="utf-8")
    with pytest.raises(ResolveError, match="is missing"):
        load_defaults(path)
