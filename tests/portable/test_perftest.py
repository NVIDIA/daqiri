# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import csv
import importlib
import json
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
LOCAL_UDP = ["--transport", "udp", "--topology", "same-host",
             "--client-address", "127.0.0.1", "--server-address", "127.0.0.1"]
RAW_IBVERBS = ["--transport", "raw", "--engine", "ibverbs", "--topology", "hw-loopback",
               "--interface", "p0"]


@pytest.fixture
def frontend(monkeypatch):
    monkeypatch.syspath_prepend(str(ROOT / "scripts"))
    monkeypatch.setattr("os.sched_getaffinity", lambda _: set(range(64)))
    monkeypatch.setattr("daqiri_config.perftest.isolated_cpus", lambda: set())
    return importlib.import_module("daqiri_perftest")


def test_socket_queue_pairs_have_distinct_ports_and_regions(frontend):
    args = frontend.parse_args(["bw", *LOCAL_UDP, "--queues", "2", "--sizes", "64,1024"])
    frontend.resolve(args)
    document = frontend.build_config(args, 1, hardware=False, rtt=False)
    config = document["daqiri"]["cfg"]
    assert [x["socket_config"]["local_addr"] for x in config["interfaces"]] == [
        "udp://127.0.0.1:18517", "udp://127.0.0.1:18518",
        "udp://127.0.0.1:19517", "udp://127.0.0.1:19518",
    ]
    assert len({r["name"] for r in config["memory_regions"]}) == 8
    assert all(r["buf_size"] == 1024 for r in config["memory_regions"])


@pytest.mark.parametrize("extra, match", [
    (["--rx-poll", "direct"], "direct polling requires"),
    (["--queues", "2", "--client-port", "18516"], "port ranges overlap"),
    (["--tx-mr", "device:1024:4096"], "host memory"),
    (["--interface", "lo"], "local same-host runs use"),
])
def test_invalid_topology_and_queue_options(frontend, extra, match):
    with pytest.raises(frontend.ConfigError, match=match):
        frontend.resolve(frontend.parse_args(["bw", *LOCAL_UDP, *extra]))


@pytest.mark.parametrize(("command", "tx_direct", "rx_direct"), [
    (["lat", *RAW_IBVERBS], True, True),
    (["lat", *RAW_IBVERBS, "--rx-poll", "indirect", "--rx-cores", "5"], True, False),
    (["lat", *RAW_IBVERBS, "--tx-poll", "indirect"], False, True),
    (["bw", *RAW_IBVERBS], False, False),
    (["lat", "--transport", "roce", "--topology", "same-host", "--client-address", "192.0.2.1",
      "--server-address", "192.0.2.2"], True, True),
    (["lat", "--transport", "raw", "--engine", "dpdk", "--topology", "sw-loopback"], False, False),
    (["lat", *LOCAL_UDP], False, False),
])
def test_polling_defaults_and_overrides(frontend, monkeypatch, command, tx_direct, rx_direct):
    topology = importlib.import_module("daqiri_config.perftest")
    monkeypatch.setattr(topology, "resolve_interface",
                        lambda *_, **kw: topology.Interface("p0", "0000:03:00.0", "02:00:00:00:00:01"))
    monkeypatch.setattr(topology, "socket_interfaces", lambda *_: {})
    monkeypatch.setattr(topology, "run_ip", lambda *_: [{}])
    args = frontend.parse_args(command)
    frontend.resolve(args)
    config = frontend.build_config(args, 0, hardware=False, rtt=False)
    assert (config["perftest"]["tx_direct"], config["perftest"]["rx_direct"]) == (tx_direct, rx_direct)
    for direction, direct in (("tx", tx_direct), ("rx", rx_direct)):
        for interface in config["daqiri"]["cfg"]["interfaces"]:
            for queue in interface.get(direction, {}).get("queues", []):
                assert (queue.get("poll_mode") == "direct") is direct


@pytest.mark.parametrize(("test", "flags", "depth"), [
    ("bw", [], 1024),
    ("bw", ["--rx-mr", "host_pinned:64:512"], 512),
    ("bw", ["--rx-depth", "128"], 128),
    ("bw", ["--queues", "2"], 1019),
    ("bw", ["--role", "server", "--queues", "2", "--bidirectional"], 1019),
    ("bw", ["--queues", "2", "--bidirectional", "--tx-poll", "direct"], 383),
    ("lat", [], 1),
])
def test_roce_receive_depth(frontend, monkeypatch, test, flags, depth):
    topology = importlib.import_module("daqiri_config.perftest")
    monkeypatch.setattr(topology, "socket_interfaces", lambda *_: {})
    monkeypatch.setattr(topology, "run_ip", lambda *_: [{}])
    args = frontend.parse_args([test, "--transport", "roce", "--topology", "same-host",
                               "--client-address", "192.0.2.1", "--server-address", "192.0.2.2",
                               "--size", "64", *flags])
    frontend.resolve(args)
    config = frontend.build_config(args, 0, hardware=False, rtt=False)
    assert config["perftest"]["rx_depth"] == depth


def test_rejects_ambiguous_bdf_without_netdev(frontend, tmp_path):
    topology = importlib.import_module("daqiri_config.perftest")
    for name in ("p1", "p2"):
        (tmp_path / "bus/pci/devices/0000:03:00.0/net" / name).mkdir(parents=True)
    with pytest.raises(frontend.ConfigError, match="multiple netdevs"):
        topology.resolve_interface("03:00.0", tmp_path, raw=True)


def test_hardware_loopback_uses_one_port_and_disjoint_flows(frontend, monkeypatch):
    topology = importlib.import_module("daqiri_config.perftest")
    monkeypatch.setattr(topology, "resolve_interface",
                        lambda *_, **kw: topology.Interface("p1", "0000:03:00.0", "02:00:00:00:00:01"))
    args = frontend.parse_args([
        "lat", "--transport", "raw", "--engine", "ibverbs", "--topology", "hw-loopback",
        "--interface", "p1", "--tx-poll", "direct", "--rx-poll", "direct",
        "--queues", "2", "--bidirectional",
        "--tx-mr", "host:64:1024", "--tx-mr", "device:1024:1024",
    ])
    frontend.resolve(args)
    config = frontend.build_config(args, 0, hardware=True, rtt=False)["daqiri"]["cfg"]
    (interface,) = config["interfaces"]
    assert interface["rx"]["hardware_timestamps"]
    assert interface["tx"]["accurate_send"]
    assert len({flow["match"]["udp_dst"] for flow in interface["rx"]["flows"]}) == 4
    assert {queue["id"] for queue in interface["rx"]["queues"]} == {0, 1, 2, 3}
    tx = interface["tx"]["queues"][0]
    assert tx["poll_mode"] == "direct"
    assert "cpu_core" not in tx and "batch_size" not in tx
    assert len(tx["memory_regions"]) == 2
    (fallback,) = frontend.build_config(args, 0, hardware=False, rtt=False)["daqiri"]["cfg"]["interfaces"]
    assert "hardware_timestamps" not in fallback["rx"]
    assert "accurate_send" not in fallback["tx"]


@pytest.mark.parametrize("bidirectional", [False, True])
def test_hardware_rtt_enables_timestamps_on_initiators(frontend, monkeypatch, bidirectional):
    topology = importlib.import_module("daqiri_config.perftest")
    monkeypatch.setattr(topology, "resolve_interface",
                        lambda *_, **kw: topology.Interface("p0", "0000:03:00.0", "02:00:00:00:00:01"))
    command = ["lat", "--transport", "raw", "--engine", "ibverbs", "--topology", "two-host",
               "--interface", "p0", "--peer-mac", "02:00:00:00:00:02", "--control", "127.0.0.1:18500",
               "--hardware-rtt", "--tx-poll", "direct", "--rx-poll", "direct"]
    if bidirectional:
        command.append("--bidirectional")
    for role in ("client", "server"):
        args = frontend.parse_args([*command, "--role", role])
        frontend.resolve(args)
        assert frontend.settings(args)["hardware_rtt"] is True
        document = frontend.build_config(args, 0, hardware=True, rtt=True)
        (interface,) = document["daqiri"]["cfg"]["interfaces"]
        timestamped = role == "client" or bidirectional
        assert document["perftest"]["hardware_rtt"] is True
        assert document["perftest"]["hardware"] is timestamped
        assert interface["rx"].get("hardware_timestamps", False) is timestamped
        assert interface["tx"].get("accurate_send", False) is timestamped


def test_hardware_rtt_rejects_socket_transport(frontend):
    with pytest.raises(frontend.ConfigError, match="requires paired raw ibverbs latency roles"):
        frontend.resolve(frontend.parse_args(["lat", *LOCAL_UDP, "--role", "client", "--hardware-rtt"]))


def test_realtime_latency_reports_application_cpu_isolation(frontend, monkeypatch):
    monkeypatch.setattr("daqiri_config.perftest.isolated_cpus", lambda: {6, 7, 8, 10})
    command = ["lat", *LOCAL_UDP, "--realtime-priority", "80"]
    args = frontend.parse_args([*command, "--app-cores", "7,8"])
    baseline = frontend.resolve(args)
    assert all("kernel isolation mask" not in warning for warning in baseline)
    config = frontend.build_config(args, 0, hardware=False, rtt=False)
    assert config["perftest"]["realtime_priority"] == 80

    args = frontend.parse_args([*command, "--app-cores", "7,9"])
    assert frontend.resolve(args) == [*baseline, "Explicit benchmark CPUs outside the kernel isolation mask 9"]


def test_cpu_defaults_assign_distinct_isolated_cpus_to_paired_roles(frontend, monkeypatch):
    monkeypatch.setattr("daqiri_config.perftest.isolated_cpus", lambda: {6, 7, 8, 9})
    command = ["lat", *LOCAL_UDP]
    server = frontend.parse_args([*command, "--role", "server"])
    client = frontend.parse_args([*command, "--role", "client"])
    for args in (server, client):
        frontend.resolve(args)
        assert args.master_core not in {6, 7, 8, 9}
    assert server.benchmark_cpus == {6, 7}
    assert client.benchmark_cpus == {8, 9}


@pytest.mark.parametrize(("isolated", "app", "rx", "warning"), [
    ({6, 7, 8, 9, 20}, [6, 7], [8, 9], None),
    ({6, 7, 20}, [6, 7], [1, 2], "Too few isolated CPUs for the benchmark layout (2 available, 4 requested)."),
    ({20}, [1, 2], [3, 4], "No isolated CPUs are available in the process affinity mask."),
    (set(), [1, 2], [3, 4], "No isolated CPUs detected. Using the process's allowed CPUs."),
])
def test_cpu_defaults_respect_affinity_and_warn_about_isolation(frontend, monkeypatch,
                                                               isolated, app, rx, warning):
    monkeypatch.setattr("daqiri_config.perftest.isolated_cpus", lambda: isolated)
    monkeypatch.setattr("os.sched_getaffinity", lambda _: set(range(10)))
    args = frontend.parse_args(["bw", *LOCAL_UDP])
    notices = frontend.resolve(args)
    assert args.master_core == 0
    assert args.app_cores == app
    assert args.rx_cores == rx
    assert notices == ["IPv4 loopback endpoints use the local kernel path"] + ([warning] if warning else [])


@pytest.mark.parametrize("test, priority", [("lat", "0"), ("lat", "99"), ("bw", "80")])
def test_invalid_realtime_priority(frontend, test, priority):
    with pytest.raises(SystemExit):
        frontend.parse_args([test, *LOCAL_UDP, "--realtime-priority", priority])


def test_raw_ibverbs_message_sizes_use_payload_only_tx(frontend, monkeypatch):
    topology = importlib.import_module("daqiri_config.perftest")
    monkeypatch.setattr(topology, "resolve_interface",
                        lambda *_, **kw: topology.Interface("p0", "0000:03:00.0", "02:00:00:00:00:01"))
    command = ["lat", "--transport", "raw", "--engine", "ibverbs", "--topology", "hw-loopback",
               "--interface", "p0"]
    args = frontend.parse_args([*command, "--message-sizes", "8,1358"])
    frontend.resolve(args)
    assert args.size_kind == "message"
    assert args.tx_mr[0].buf_size == 1358
    assert args.rx_mr[0].buf_size == 1400
    first = frontend.build_config(args, 0, hardware=True, rtt=False)["perftest"]
    assert (first["size"], first["message_size"]) == (50, 8)
    assert first["named_sender"]
    second = frontend.build_config(args, 1, hardware=True, rtt=False)["perftest"]
    assert (second["size"], second["message_size"]) == (1400, 1358)

    args = frontend.parse_args([*command, "--packet-size", "1400"])
    frontend.resolve(args)
    assert args.size_kind == "packet"
    assert args.tx_mr[0].buf_size == args.rx_mr[0].buf_size == 1400
    packet = frontend.build_config(args, 0, hardware=True, rtt=False)["perftest"]
    assert (packet["size"], packet["message_size"]) == (1400, 0)


def test_raw_gpu_packet_uses_named_header_and_opaque_buffers(frontend, monkeypatch):
    topology = importlib.import_module("daqiri_config.perftest")
    monkeypatch.setattr(topology, "resolve_interface",
                        lambda *_, **kw: topology.Interface("p0", "0000:03:00.0", "02:00:00:00:00:01"))
    args = frontend.parse_args([
        "lat", "--transport", "raw", "--engine", "ibverbs", "--topology", "hw-loopback",
        "--interface", "p0", "--packet-size", "64", "--tx-mr", "device:22:4096",
        "--rx-mr", "device:1400:4096",
    ])
    frontend.resolve(args)
    document = frontend.build_config(args, 0, hardware=True, rtt=False)
    worker = document["perftest"]
    assert worker["named_sender"] and worker["opaque_payload"]
    assert (worker["size"], worker["message_size"]) == (64, 0)
    server, client = worker["lanes"]
    assert client["tx"][0]["buf_size"] == 22 and client["tx"][0]["zero_init"]
    assert "zero_init" not in server["rx"][0]
    (interface,) = document["daqiri"]["cfg"]["interfaces"]
    assert server["rx_flow_id"] == interface["rx"]["flows"][0]["id"]


@pytest.mark.parametrize(("selector", "region", "polling", "batch", "prepared"), [
    (["--packet-sizes", "64,512"], "device:512:4096", "indirect", "32", True),
    (["--packet-sizes", "64,512"], "device:470:4096", "indirect", "32", False),
    (["--packet-size", "64"], "device:64:4096", "indirect", "1", False),
    (["--packet-size", "64"], "device:64:4096", "direct", "32", False),
    (["--message-size", "22"], "device:64:4096", "indirect", "32", False),
])
def test_raw_device_bandwidth_prepares_complete_frames_when_eligible(
        frontend, monkeypatch, selector, region, polling, batch, prepared):
    topology = importlib.import_module("daqiri_config.perftest")
    monkeypatch.setattr(topology, "resolve_interface",
                        lambda *_, **kw: topology.Interface("p0", "0000:03:00.0", "02:00:00:00:00:01"))
    args = frontend.parse_args([
        "bw", *RAW_IBVERBS, *selector, "--tx-mr", region, "--rx-mr", "device:1400:4096",
        "--tx-poll", polling, "--batch-size", batch,
    ])
    frontend.resolve(args)
    for case, size in enumerate(args.sizes):
        document = frontend.build_config(args, case, hardware=False, rtt=False)
        worker = document["perftest"]
        assert worker["prepared_device_frames"] is prepared
        assert worker["named_sender"] is not prepared
        assert worker["size"] == (size if args.size_kind == "packet" else size + 42)
        _, client = worker["lanes"]
        assert client["tx"][0]["zero_init"]


def test_raw_dpdk_device_payload_requires_host_header(frontend, monkeypatch):
    topology = importlib.import_module("daqiri_config.perftest")
    monkeypatch.setattr(topology, "resolve_interface",
                        lambda name, **_: topology.Interface(name, "0000:03:00.0", "02:00:00:00:00:01"))
    command = ["bw", "--transport", "raw", "--engine", "dpdk", "--topology", "two-host",
               "--interface", "p0", "--role", "client", "--peer-mac", "02:00:00:00:00:02",
               "--control", "127.0.0.1:18500", "--packet-size", "64"]
    with pytest.raises(frontend.ConfigError, match="CPU-accessible bytes"):
        frontend.resolve(frontend.parse_args([*command, "--tx-mr", "device:64:4096"]))
    args = frontend.parse_args([*command, "--tx-mr", "huge:42:4096", "--tx-mr", "device:22:4096"])
    frontend.resolve(args)
    assert args.opaque_payload and not args.named_sender


def test_named_sender_rx_storage_covers_ethernet_padding(frontend, monkeypatch):
    topology = importlib.import_module("daqiri_config.perftest")
    monkeypatch.setattr(topology, "resolve_interface",
                        lambda *_, **kw: topology.Interface("p0", "0000:03:00.0", "02:00:00:00:00:01"))
    args = frontend.parse_args([
        "lat", "--transport", "raw", "--engine", "ibverbs", "--topology", "hw-loopback",
        "--interface", "p0", "--message-size", "8", "--rx-mr", "host:50:1024",
    ])
    with pytest.raises(frontend.ConfigError, match="RX memory must cover 60 bytes"):
        frontend.resolve(args)


def test_explicit_size_flags_follow_transport(frontend):
    message = frontend.parse_args(["bw", *LOCAL_UDP, "--message-size", "1400"])
    legacy = frontend.parse_args(["bw", *LOCAL_UDP, "--size", "1400"])
    for args in (message, legacy):
        frontend.resolve(args)
    assert frontend.settings(message) == frontend.settings(legacy)
    assert frontend.build_config(message, 0, hardware=False, rtt=False)["perftest"]["size"] == 1400

    with pytest.raises(frontend.ConfigError, match="require raw Ethernet"):
        frontend.resolve(frontend.parse_args(["bw", *LOCAL_UDP, "--packet-size", "1400"]))
    dpdk = frontend.parse_args([
        "bw", "--transport", "raw", "--engine", "dpdk", "--topology", "sw-loopback",
        "--message-size", "1400",
    ])
    frontend.resolve(dpdk)
    config = frontend.build_config(dpdk, 0, hardware=False, rtt=False)["perftest"]
    assert (config["size"], config["message_size"], config["named_sender"]) == (1442, 1400, False)
    assert dpdk.tx_mr[0].buf_size == dpdk.rx_mr[0].buf_size == 1442
    with pytest.raises(SystemExit):
        frontend.parse_args(["bw", *LOCAL_UDP, "--packet-size", "128", "--message-size", "64"])


def test_timestamp_capabilities_and_clock_domains(frontend):
    header = "Configurations:           Default    Current    Next Boot\n"
    assert frontend.firmware_state(header + "* REAL_TIME_CLOCK_ENABLE False(0) False(0) True(1)") is False
    assert frontend.firmware_state(header + "REAL_TIME_CLOCK_ENABLE False(0) True(1) False(0)") is True
    assert frontend.firmware_state(header + "OTHER_SETTING True(1) True(1) True(1)") is False
    assert frontend.firmware_state("Configurations: Next Boot\nREAL_TIME_CLOCK_ENABLE True(1)") is None
    clocks = {"client": {"identity": "ptp0", "offset": 0, "uncertainty": 100},
              "server": {"identity": "ptp1", "offset": 500, "uncertainty": 100}}
    assert frontend.comparable_hardware(clocks)
    clocks["server"]["offset"] = 900
    assert not frontend.comparable_hardware(clocks)
    assert frontend.shared_cpu_clock({"boot": "a", "offset": ["0", "0"]},
                                     {"boot": "a", "offset": ["0", "0"]})
    assert not frontend.shared_cpu_clock({"boot": "a", "offset": ["0", "0"]},
                                         {"boot": "a", "offset": ["7", "0"]})


def test_phc_sampling_uses_shortest_kernel_bracket(frontend, monkeypatch):
    def ioctl(fd, request, buffer):
        assert fd == 4 and request == frontend.PTP_SYS_OFFSET_EXTENDED
        for index, timestamp in enumerate((100, 241, 380, 400, 462, 500)):
            frontend.PTP_CLOCK_TIME.pack_into(buffer, 16 + 16 * index, 1, timestamp, 0)

    monkeypatch.setattr(frontend.fcntl, "ioctl", ioctl)
    assert frontend.sample_phc_offset(4) == {"offset": 12, "uncertainty": 50, "method": "kernel_extended"}


def test_phc_sampling_falls_back_when_kernel_brackets_are_unavailable(frontend, monkeypatch):
    def ioctl(*_):
        raise OSError("extended timestamps unavailable")

    clock = iter([100, 300] * 9)
    monkeypatch.setattr(frontend.fcntl, "ioctl", ioctl)
    monkeypatch.setattr(frontend.time, "time_ns", lambda: next(clock))
    monkeypatch.setattr(frontend.time, "clock_gettime_ns", lambda _: 220)
    assert frontend.sample_phc_offset(4) == {"offset": 20, "uncertainty": 100, "method": "userspace_bracket"}


def test_tuning_collects_warnings_and_failed_checks(frontend, monkeypatch):
    args = frontend.parse_args(["bw", *LOCAL_UDP])
    frontend.resolve(args)

    def run(command, **kwargs):
        assert command[-2] == "--check"
        if command[-1] == "numa":
            return subprocess.CompletedProcess(command, 1, "", "missing dependency\n")
        return subprocess.CompletedProcess(command, 0, "", "stamp - INFO - good\nstamp - WARNING - slow\n")

    monkeypatch.setattr(frontend.subprocess, "run", run)
    warnings = []
    assert frontend.tuning_preflight(args, warnings.append) == warnings
    assert warnings == ["stamp - WARNING - slow",
                        "tune_system.py --check numa exited 1 (missing dependency)"]


def test_software_and_hardware_share_probe_rows(frontend):
    args = frontend.parse_args(["lat", *LOCAL_UDP])
    measurement = {"direction": "client->server", "queue": 0, "timeouts": 1,
                   "probes": [[0, 100, -4], [2, 200, 10]]}
    rows, probes, _ = frontend.collect(args, [{"role": "local", "latency": [measurement]}], 64, False)
    assert rows[0]["software"]["median_ns"] == 150
    assert rows[0]["hardware"]["min_ns"] == -4
    assert rows[0]["hardware"]["median_ns"] == 3
    assert rows[0]["software"]["samples"] == rows[0]["hardware"]["samples"] == 2
    assert [(p["software_ns"], p["hardware_ns"]) for p in probes] == [(100, -4), (200, 10)]


def test_hardware_rtt_labels_reply_timestamp_in_reports(frontend, capsys):
    args = frontend.parse_args(["lat", *LOCAL_UDP, "--hardware-rtt"])
    args.raw = True
    args.size_kind = "packet"
    latency = {"direction": "client->server", "queue": 0, "timeouts": 0, "probes": [[0, 100, 80]]}
    rows, probes, _ = frontend.collect(args, [{"role": "client", "latency": [latency]}], 64, True)
    frontend.print_case(args, {"size": 64, "measurements": rows})
    method = "reply_rx_hw_minus_request_tx_cqe"
    assert rows[0]["hardware_method"] == probes[0]["hardware_method"] == method
    assert probes[0]["software_method"] == "application_rtt"
    assert f"{method}: min 80" in capsys.readouterr().out


def test_hardware_failure_reruns_after_preflight(frontend, monkeypatch, tmp_path, capsys):
    topology = importlib.import_module("daqiri_config.perftest")
    monkeypatch.setattr(topology, "resolve_interface",
                        lambda *_, **kw: topology.Interface("p0", "0000:03:00.0", "02:00:00:00:00:01"))
    monkeypatch.setattr(frontend, "executable", lambda *args: Path("/unused"))
    monkeypatch.setattr(frontend.subprocess, "check_output", lambda *a, **kw: "ibverbs")
    monkeypatch.setattr(frontend.subprocess, "run",
                        lambda *a, **kw: subprocess.CompletedProcess(a, 0))
    monkeypatch.setattr(frontend, "hardware_info",
                        lambda *a, **kw: dict.fromkeys(("client", "server"),
                                                      {"identity": "ptp0", "enabled": True}))

    def preflight(args):
        frontend.warn("advisory tuning finding")
        return ["advisory tuning finding"]

    monkeypatch.setattr(frontend, "tuning_preflight", preflight)
    attempts = []

    def run(args, worker, path, output, peer, hardware, case):
        stderr = capsys.readouterr().err
        attempts.append(hardware)
        if hardware:
            assert stderr.index("advisory tuning finding") < stderr.index("Starting benchmark")
            return frontend.TIMESTAMP_UNAVAILABLE, []
        assert "Repeating with CPU timing" in stderr
        measurement = {"direction": "client->server", "queue": 0,
                       "timeouts": 0, "probes": [[0, 100, None]]}
        return 0, [{"version": "test", "role": "local", "latency": [measurement],
                    "local_route": False, "configuration": {}}]

    monkeypatch.setattr(frontend, "run_case", run)
    report, samples = tmp_path / "data.json", tmp_path / "probes.csv"
    assert frontend.main(["lat", "--transport", "raw", "--engine", "ibverbs",
                          "--topology", "hw-loopback", "--interface", "p0",
                          "--json", str(report), "--samples-csv", str(samples)]) == 0
    assert attempts == [True, False]
    result = json.loads(report.read_text())["cases"][0]
    assert result["measurements"][0]["hardware"] == {"samples": 0}
    with samples.open() as stream:
        assert list(csv.DictReader(stream))[0]["hardware_ns"] == ""
