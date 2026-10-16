# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

from pathlib import Path
import subprocess

import pytest

from scripts.daqiri_config import load_document

from scripts.resolve_benchmark_hardware import (
    PLATFORM_DEFAULTS,
    RAW_TEMPLATE,
    Gpu,
    Port,
    ResolveError,
    fill_raw_template,
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


def test_platform_defaults_match_report_settings() -> None:
    platforms = PLATFORM_DEFAULTS
    assert set(platforms) == {"dgx-spark", "igx-thor"}
    assert platforms["dgx-spark"]["dpdk_memory_kind"] == "host_pinned"
    assert platforms["igx-thor"]["dpdk_memory_kind"] == "device"
    assert platforms["igx-thor"]["rdma_memory_kind"] == "host_pinned"
    assert platforms["igx-thor"]["isolated_cpus"] == "8-13"
    assert platforms["igx-thor"]["socket_client_cores"] == [9, 11, 13]
    assert platforms["igx-thor"]["socket_pair_counts"] == [1, 2, 3]


def test_platform_selection_is_explicit() -> None:
    platforms = PLATFORM_DEFAULTS
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
        PLATFORM_DEFAULTS,
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


@pytest.mark.parametrize("engine", ("dpdk", "ibverbs"))
@pytest.mark.parametrize("platform,gpu_name", (
    ("igx-thor", "NVIDIA RTX PRO 6000 Blackwell Max-Q Workstation Edition"),
    ("dgx-spark", "NVIDIA GB10"),
))
def test_fill_existing_raw_template(tmp_path, monkeypatch, platform, gpu_name, engine):
    for name in ("DAQIRI_GPU_UUID", "DPDK_TX_PCI", "DPDK_RX_PCI"):
        monkeypatch.delenv(name, raising=False)
    source = RAW_TEMPLATE.read_text()
    original = load_document(RAW_TEMPLATE)
    path = tmp_path / "raw.yaml"
    path.write_text(source)
    values = resolve(
        PLATFORM_DEFAULTS, platform, [Gpu(gpu_name, "GPU-TEST")],
        [port("tx", "0004:03:00.0", "p0"), port("rx", "0004:03:00.1", "p1")],
        skip_nic=False,
    )
    fill_raw_template(path, values, engine)
    document = load_document(path)
    cfg = document["daqiri"]["cfg"]
    assert cfg["engine"] == engine
    assert cfg["master_core"] == values["DEFAULT_MASTER_CORE"]
    assert cfg["interfaces"][0]["address"] == "0004:03:00.0"
    assert cfg["interfaces"][1]["address"] == "0004:03:00.1"
    assert document["bench_tx"][0]["eth_dst_addr"] == values["DEFAULT_ETH_DST_ADDR"]
    assert document["bench_tx"][0]["eth_src_addr"] == values["DEFAULT_ETH_SRC_ADDR"]
    assert document["bench_tx"][0]["ip_src_addr"] == "192.0.2.1"
    assert document["bench_tx"][0]["payload_size"] == original["bench_tx"][0]["payload_size"]
    assert document["bench_tx"][0]["header_size"] == original["bench_tx"][0]["header_size"]
    assert cfg["memory_regions"][0]["buf_size"] == original["daqiri"]["cfg"]["memory_regions"][0]["buf_size"]
    assert cfg["memory_regions"][0]["kind"] == values[f"DEFAULT_{engine.upper()}_MEMORY_KIND"]
    for index, direction in enumerate(("tx", "rx")):
        queue = cfg["interfaces"][index][direction]["queues"][0]
        assert queue["cpu_core"] == values[f"DEFAULT_DPDK_{direction.upper()}_QUEUE_CORE"]
        assert document[f"bench_{direction}"][0]["cpu_core"] == values[f"DEFAULT_DPDK_{direction.upper()}_WORKER_CORE"]
        if engine == "ibverbs":
            assert queue["batch_size"] == values["DEFAULT_IBVERBS_BATCH_SIZE"]
    if engine == "ibverbs":
        assert cfg["interfaces"][1]["rx"]["flows"][0]["id"] > 0


def test_fill_invalid_template_leaves_file_unchanged(tmp_path: Path) -> None:
    path = tmp_path / "invalid.yaml"
    source = "daqiri: {}\n"
    path.write_text(source)
    with pytest.raises(ResolveError, match="cannot fill raw TX/RX template"):
        fill_raw_template(path, {}, "dpdk")
    assert path.read_text() == source


@pytest.mark.parametrize("arguments,message", (
    (["--payload-size"], "--payload-size requires a positive integer"),
    (["--payload-size", "0"], "--payload-size requires a positive integer"),
    (["--pairs", "abc"], "--pairs requires a positive integer"),
    (["ibverbs", "smoke", "--pairs", "2"], "--pairs is supported only for socket backends"),
    (["ibverbs", "latency", "--payload-size", "64"], "--payload-size does not apply to latency mode"),
))
def test_harness_rejects_invalid_selections_before_hardware_discovery(arguments, message):
    result = subprocess.run(
        ["bash", str(RAW_TEMPLATE.parent / "run_cabled_loopback_bench.sh"),
         "--platform", "igx-thor", *arguments],
        capture_output=True, text=True,
    )
    assert result.returncode != 0
    assert message in result.stderr


def test_harness_help_does_not_require_hardware():
    result = subprocess.run(
        ["bash", str(RAW_TEMPLATE.parent / "run_cabled_loopback_bench.sh"), "--help"],
        capture_output=True, text=True,
    )
    assert result.returncode == 0
    assert "--payload-size" in result.stdout
    assert "--pairs" in result.stdout


@pytest.mark.parametrize("target,expected", (
    ("2.25", "2250"), ("1.75", "1750"), ("0.5", "500"),
    ("0.001", "1"), ("200", "200000"), ("0.0", "0"),
    ("-1", None), ("nan", None), ("inf", None),
    ("invalid", None), ("0.0005", None),
))
def test_raw_pacing_converts_fractional_gbps_without_hardware(target, expected):
    harness = (RAW_TEMPLATE.parent / "run_cabled_loopback_bench.sh").read_text()
    body = harness.split("gbps_to_mbps() {", 1)[1].split("\n}\n", 1)[0]
    result = subprocess.run(
        ["bash", "-c", "gbps_to_mbps() {" + body + "\n}\ngbps_to_mbps \"$1\"",
         "pacing-test", target],
        capture_output=True, text=True,
    )
    if expected is None:
        assert result.returncode != 0
        assert "resolving to whole Mbps" in result.stderr
    else:
        assert result.returncode == 0
        assert result.stdout.strip() == expected


def latency_namespace(tmp_path, monkeypatch):
    import sys

    harness = (RAW_TEMPLATE.parent / "run_cabled_loopback_bench.sh").read_text()
    source = harness.split("<<'LATENCY_PY'\n", 1)[1].split("\nLATENCY_PY", 1)[0]
    monkeypatch.setattr(sys, "argv", ["latency", str(RAW_TEMPLATE.parent.parent),
                                     str(tmp_path / "build"), str(tmp_path)])
    monkeypatch.setattr(sys, "path", sys.path.copy())
    namespace = {}
    exec(compile(source.rsplit("sys.exit(main())", 1)[0], "latency-runner", "exec"), namespace)
    return namespace


def test_latency_uses_destination_and_rx_device_overrides(tmp_path, monkeypatch):
    import json

    namespace = latency_namespace(tmp_path, monkeypatch)
    monkeypatch.setenv("DAQIRI_ETH_DST_ADDR", "02:aa:bb:cc:dd:ee")
    monkeypatch.setenv("DAQIRI_RX_NETDEV", "override-rx")
    monkeypatch.setenv("DAQIRI_MASTER_CORE", "0")
    monkeypatch.setenv("DAQIRI_DPDK_TX_WORKER_CORE", "1")
    monkeypatch.setenv("DAQIRI_DPDK_TX_QUEUE_CORE", "2")
    monkeypatch.setenv("DAQIRI_DPDK_RX_QUEUE_CORE", "3")
    monkeypatch.setenv("LATENCY_GENERATE_ONLY", "1")
    (tmp_path / "resolved-hardware.json").write_text(json.dumps({
        "DEFAULT_DPDK_TX_PCI": "0000:01:00.0",
        "DEFAULT_DPDK_RX_PCI": "0000:01:00.1",
        "DEFAULT_ETH_DST_ADDR": "02:00:00:00:00:02",
        "DEFAULT_DPDK_RX_NETDEV": "default-rx",
    }))
    requested_clocks = []
    original_glob = Path.glob

    def clock_glob(path, pattern):
        if str(path).startswith("/sys/class/net/"):
            requested_clocks.append(str(path))
            return iter([path / "ptp7"])
        return original_glob(path, pattern)

    monkeypatch.setattr(Path, "glob", clock_glob)
    assert namespace["main"]() == 0
    assert requested_clocks == ["/sys/class/net/override-rx/device/ptp"]
    for memory in ("host_pinned", "device"):
        for mode in ("direct", "indirect"):
            config = load_document(tmp_path / f"{memory}-{mode}.yaml")
            assert config["bench_latency"]["eth_dst_addr"] == "02:aa:bb:cc:dd:ee"
            assert config["bench_latency"]["ptp_device"] == "/dev/ptp7"


def test_latency_captures_priority_buffer_drop_counters(tmp_path, monkeypatch):
    namespace = latency_namespace(tmp_path, monkeypatch)
    requested = []

    def counters(command, **kwargs):
        requested.append(command)
        return """rx_packets_phy: 100
rx_prio0_buf_discard: 2
rx_prio7_buf_discard_packets: 3
rx_prio3_discards: 4
rx_prio0_bytes: 999
"""

    monkeypatch.setattr(subprocess, "check_output", counters)
    assert namespace["nic_counters"]("override-rx") == {
        "rx_packets_phy": 100, "rx_prio0_buf_discard": 2,
        "rx_prio7_buf_discard_packets": 3, "rx_prio3_discards": 4,
    }
    assert requested == [["ethtool", "-S", "override-rx"]]


@pytest.mark.parametrize("payload,settings,override,expected", (
    (8192, "", "", [32, 64, 128, 256]),
    (8192, "64:1024", "", [32, 64, 128, 256]),
    (64, "64:1024", "", [1024]),
    (64, "64:1024", "32 64", [32, 64, 128, 256]),
))
def test_dpdk_sweep_preserves_batches_without_matching_platform_setting(
    payload, settings, override, expected,
):
    harness = (RAW_TEMPLATE.parent / "run_cabled_loopback_bench.sh").read_text()
    function = harness.split("payload_setting() {", 1)[1].split("\n}\n", 1)[0]
    sweep = harness.split('case "$MODE" in\n', 1)[1].split("  sweep)\n", 1)[1].split("    ;;", 1)[0]
    program = "payload_setting() {" + function + "\n}\n" + '''
BACKEND=dpdk
PAYLOADS_SWEEP=("$1")
BATCHES_SWEEP=(32 64 128 256)
PAIRS_SWEEP=(1)
DPDK_PAYLOAD_BATCHES="$2"
BATCHES_OVERRIDE="$3"
DPDK_PAYLOAD_PACING_GBPS=""
run_cell_or_record_failure() { echo "$3"; }
''' + sweep
    result = subprocess.run(["bash", "-eu", "-c", program, "sweep-test", str(payload),
                             settings, override], capture_output=True, text=True, check=True)
    assert [int(batch) for batch in result.stdout.split()] == expected


@pytest.mark.parametrize("termination", ("normal", "TERM", "INT"))
def test_capture_is_stopped_and_reaped_on_exit(termination):
    import os
    import signal

    harness = (RAW_TEMPLATE.parent / "run_cabled_loopback_bench.sh").read_text()
    cleanup = harness.split('dmon_pid=""\n', 1)[1].split("\nrun_cell() {", 1)[0]
    program = 'dmon_pid=""\n' + cleanup + '''
sleep 60 &
mlnx_perf_pid=$!
sleep 60 &
dmon_pid=$!
echo "$mlnx_perf_pid $dmon_pid"
'''
    program += "exit 0\n" if termination == "normal" else 'wait "$mlnx_perf_pid"\n'
    process = subprocess.Popen(["bash", "-eu", "-c", program], stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True)
    monitor_pids = [int(pid) for pid in process.stdout.readline().split()]
    try:
        if termination != "normal":
            process.send_signal(getattr(signal, f"SIG{termination}"))
        process.communicate(timeout=5)
        assert process.returncode == {"normal": 0, "TERM": 143, "INT": 130}[termination]
        for pid in monitor_pids:
            with pytest.raises(ProcessLookupError):
                os.kill(pid, 0)
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()
        for pid in monitor_pids:
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
