#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"""Run reproducible DAQIRI bandwidth and latency tests."""

from __future__ import annotations

import argparse
import contextlib
import csv
import fcntl
import ipaddress
import json
import math
import os
import re
import shutil
import socket
import statistics
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

import yaml

HERE = Path(__file__).resolve().parent
for hint in ("@DAQIRI_CONFIG_GENERATOR_MODULE_HINT@", "@CMAKE_CURRENT_SOURCE_DIR@/scripts"):
    if not hint.startswith("@") and (HERE / hint / "daqiri_config").is_dir():
        sys.path.insert(0, str(HERE / hint))
        break

from daqiri_config.core import ConfigError, render_document
from daqiri_config.perftest import build_config, frame_size, region_arg, resolve

MAX_CONTROL_BYTES = 64 * 1024 * 1024
TIMESTAMP_UNAVAILABLE = 3
PHC_SKEW_NS = 1000
# Linux PTP_SYS_OFFSET_EXTENDED for x86_64 and aarch64. The buffer is a
# 16-byte header followed by up to 25 system/PHC/system timestamp triplets.
PTP_SYS_OFFSET_EXTENDED = 0xC4C03D09
PTP_MAX_SAMPLES = 25
PTP_CLOCK_TIME = struct.Struct("=qII")
FINDING = re.compile(r" - (?:WARNING|ERROR|CRITICAL) - ")
COUNTER = re.compile(r"^\s*(tx_bytes_phy|rx_bytes_phy|vport_loopback_bytes):\s+([\d,.]+)\s+Bps")
PHY_PACKETS = ("tx_packets_phy", "rx_packets_phy")
LATENCY_LABELS = {"application_rtt": "SW RTT", "tx_submit_to_rx_delivery": "SW one-way",
                  "reply_rx_hw_minus_request_tx_cqe": "HW RTT", "rx_hw_minus_tx_cqe": "HW one-way"}


def warn(message: str) -> None:
    print(f"WARNING: {message}", file=sys.stderr, flush=True)


def integer(value: str) -> int:
    result = int(value)
    if result < 0 or result > 2**31 - 1:
        raise argparse.ArgumentTypeError("expected an integer between 0 and 2147483647")
    return result


def positive(value: str) -> int:
    result = integer(value)
    if not result:
        raise argparse.ArgumentTypeError("expected a positive integer")
    return result


def port(value: str) -> int:
    result = positive(value)
    if result > 65535:
        raise argparse.ArgumentTypeError("port must not exceed 65535")
    return result


def ipv4(value: str) -> str:
    try:
        return str(ipaddress.IPv4Address(value))
    except ipaddress.AddressValueError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc


def control_address(value: str) -> tuple[str, int]:
    host, sep, service = value.partition(":")
    if not sep:
        raise argparse.ArgumentTypeError("expected an IPv4 address and port")
    return ipv4(host), port(service)


def region(value: str):
    try:
        return region_arg(value)
    except ConfigError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc


def parse_args(argv: list[str] | None = None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("test", choices=("bw", "lat"))
    parser.add_argument("--transport", choices=("raw", "roce", "tcp", "udp"), required=True)
    parser.add_argument("--engine", choices=("ibverbs", "dpdk"))
    parser.add_argument("--topology", choices=("hw-loopback", "sw-loopback", "same-host", "two-host"),
                        required=True)
    parser.add_argument("--role", choices=("local", "server", "client"), default="local")
    parser.add_argument("--interface", help="local netdev or PCI BDF for a paired or HW loopback test")
    parser.add_argument("--client-interface")
    parser.add_argument("--server-interface")
    parser.add_argument("--peer-mac", help="peer Ethernet address for paired raw tests")
    parser.add_argument("--client-address", type=ipv4)
    parser.add_argument("--server-address", type=ipv4)
    parser.add_argument("--client-port", type=port, default=19515)
    parser.add_argument("--server-port", type=port, default=18515)
    parser.add_argument("--control", type=control_address, metavar="IP:PORT",
                        help="paired TCP control endpoint on the server (default: server address:18500)")
    parser.add_argument("--bidirectional", action="store_true")
    parser.add_argument("--queues", type=positive, default=1)
    parser.add_argument("--batch-size", type=positive)
    parser.add_argument("--master-core", type=integer)
    parser.add_argument("--app-cores", type=lambda s: [integer(v) for v in s.split(",")])
    parser.add_argument("--rx-app-cores", type=lambda s: [integer(v) for v in s.split(",")],
                        help="dedicated application readers for bidirectional TCP bandwidth")
    for direction in ("rx", "tx"):
        parser.add_argument(f"--{direction}-mr", action="append", type=region,
                            metavar="KIND:BYTES:COUNT[:AFFINITY]")
        parser.add_argument(f"--{direction}-cores", type=lambda s: [integer(v) for v in s.split(",")])
        parser.add_argument(f"--{direction}-poll", choices=("direct", "indirect"),
                            help="defaults to direct for raw ibverbs and RoCE latency, indirect otherwise")
        depth_help = ("RoCE receive work-request depth, defaults to up to 1024 per queue "
                      "for bandwidth and 1 for latency" if direction == "rx" else
                      "RoCE send work-request depth, defaults to 128")
        parser.add_argument(f"--{direction}-depth", type=positive, help=depth_help)
    sizes = parser.add_mutually_exclusive_group()
    sizes.add_argument("--size", type=positive, help="raw frame bytes or transport message bytes")
    sizes.add_argument("--sizes", type=lambda s: [positive(v) for v in s.split(",")],
                       help="comma-separated sizes using the same units as --size")
    sizes.add_argument("--packet-size", type=positive, help="raw L2 frame size excluding FCS")
    sizes.add_argument("--packet-sizes", type=lambda s: [positive(v) for v in s.split(",")],
                       help="comma-separated raw L2 frame sizes")
    sizes.add_argument("--message-size", type=positive,
                       help="message payload bytes. Raw ibverbs uses a named sender")
    sizes.add_argument("--message-sizes", type=lambda s: [positive(v) for v in s.split(",")],
                       help="comma-separated message payload sizes")
    parser.add_argument("--seconds", type=positive, default=10)
    parser.add_argument("--warmup-seconds", type=integer, default=2)
    parser.add_argument("--samples", type=positive, default=10_000)
    parser.add_argument("--warmup-samples", type=integer, default=1_000)
    parser.add_argument("--hardware-rtt", action="store_true",
                        help="time raw ibverbs echoes from the initiator's TX CQE to its reply RX timestamp")
    parser.add_argument("--realtime-priority", type=positive,
                        help="SCHED_FIFO priority for latency application threads (1–98)")
    parser.add_argument("--timeout", type=positive, default=5, help="per-probe and phase timeout in seconds")
    parser.add_argument("--peer-timeout", type=positive, default=60,
                        help="seconds to wait for the peer process")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--json", type=Path, metavar="PATH")
    parser.add_argument("--samples-csv", type=Path, metavar="PATH")
    parser.add_argument("--build-dir", type=Path, help="build directory when running from source")
    args = parser.parse_args(argv)
    if not 1 <= args.queues <= 512:
        parser.error("--queues must be in 1..512")
    if args.samples_csv and args.test != "lat":
        parser.error("--samples-csv requires lat")
    if args.realtime_priority and (args.test != "lat" or args.realtime_priority > 98):
        parser.error("--realtime-priority requires lat and a priority from 1 to 98")
    if args.transport != "roce" and (args.rx_depth or args.tx_depth):
        parser.error("RX and TX depth options require RoCE")
    if args.timeout > 300:
        parser.error("--timeout must be at most 300 seconds")
    return args


def executable(args, name: str, subdir: str) -> Path:
    base = args.build_dir.resolve() if args.build_dir else HERE
    for path in (base / name, base / subdir / name):
        if path.is_file() and os.access(path, os.X_OK):
            return path
    raise ConfigError(f"cannot find {name} under {base}; build DAQIRI or use --build-dir")


def tuning_preflight(args, warn_fn=warn) -> list[str]:
    script = next((p for p in (HERE / "tune_system.py",
                               HERE.parent / "python/tune_system.py") if p.is_file()), None)
    if script is None:
        message = "tune_system.py is missing. Tuning could not be checked."
        warn_fn(message)
        return [message]
    kinds = {r.kind for r in args.rx_mr + args.tx_mr}
    checks = ["cpu-freq", "numa"]
    if "huge" in kinds:
        checks.append("hugepages")
    if any(intf.bdf for intf in args.interfaces.values()):
        checks.extend(("mrrs", "mps", "mtu", "pause"))
    if "device" in kinds:
        checks.extend(("gpudirect", "gpu-clocks", "bar1-size", "topo", "cmdline"))
    findings = []
    for check in checks:
        try:
            result = subprocess.run([sys.executable, str(script), "--check", check], timeout=10,
                                    capture_output=True, text=True)
            lines = (result.stdout + result.stderr).splitlines()
            findings.extend(line for line in lines if FINDING.search(line))
            if result.returncode:
                details = " ".join(line for line in lines if not FINDING.search(line))
                findings.append(f"tune_system.py --check {check} exited {result.returncode}"
                                + (f" ({details})" if details else ""))
        except (OSError, subprocess.SubprocessError) as exc:
            findings.append(f"tune_system.py --check {check} failed ({exc})")
    for finding in findings:
        warn_fn(finding)
    return findings


def firmware_state(output: str) -> bool | None:
    """Read the Current column of mlxconfig, when the device exposes it."""
    columns = []
    for line in output.splitlines():
        if line.lstrip().startswith("Configurations:"):
            columns = re.findall(r"Default|Current|Next Boot", line)
            continue
        fields = line.strip().lstrip("*").split()
        if fields and fields[0] == "REAL_TIME_CLOCK_ENABLE":
            if "Current" not in columns or len(fields) != len(columns) + 1:
                return None
            value = fields[columns.index("Current") + 1].lower()
            if value in ("1", "true(1)"):
                return True
            if value in ("0", "false(0)"):
                return False
            return None
    return False if "Current" in columns else None


def sample_phc_offset(fd: int) -> dict:
    header_size = 16
    buffer = bytearray(header_size + 3 * PTP_MAX_SAMPLES * PTP_CLOCK_TIME.size)
    struct.pack_into("=I", buffer, 0, PTP_MAX_SAMPLES)
    try:
        fcntl.ioctl(fd, PTP_SYS_OFFSET_EXTENDED, buffer)
    except OSError:
        readings = []
        for _ in range(9):
            before = time.time_ns()
            device = time.clock_gettime_ns((~fd << 3) | 3)
            after = time.time_ns()
            readings.append((before, device, after))
        method = "userspace_bracket"
    else:
        stamps = (seconds * 10**9 + nanoseconds for seconds, nanoseconds, _ in
                  PTP_CLOCK_TIME.iter_unpack(memoryview(buffer)[header_size:]))
        readings = zip(stamps, stamps, stamps)
        method = "kernel_extended"
    interval, offset = min((after - before, device - (before + after) // 2)
                           for before, device, after in readings if after > before and device > 0)
    return {"offset": offset, "uncertainty": (interval + 1) // 2, "method": method}


def read_phc(name: str, bdf: str) -> dict:
    paths = (Path("/sys/class/net") / name / "device/ptp").glob("ptp[0-9]*")
    names = sorted({path.name for path in paths})
    clock = names[0] if len(names) == 1 else ""
    identity = clock or bdf
    try:
        fd = os.open(f"/dev/{clock}", os.O_RDONLY)
        try:
            return {"identity": identity} | sample_phc_offset(fd)
        finally:
            os.close(fd)
    except (OSError, ValueError):
        return {"identity": identity}


def hardware_info(args, *, check_firmware=True) -> dict:
    info = {}
    firmware = {}
    for role in args.roles:
        intf = args.interfaces.get(role)
        if intf is None or not intf.bdf:
            info[role] = {"enabled": False}
            continue
        if intf.bdf not in firmware and check_firmware:
            try:
                query = subprocess.run(["mlxconfig", "--enable_verbosity", "-d", intf.bdf, "query"],
                                       text=True, capture_output=True, timeout=10)
                firmware[intf.bdf] = firmware_state(query.stdout) if query.returncode == 0 else None
            except (OSError, subprocess.SubprocessError):
                firmware[intf.bdf] = None
            active = firmware[intf.bdf]
            if active is False:
                warn(f"{intf.name} has no active REAL_TIME_CLOCK_ENABLE setting. "
                     "hardware latency is unavailable")
            elif active is None:
                warn(f"Could not verify current REAL_TIME_CLOCK_ENABLE on {intf.name}. "
                     "Checking runtime timestamp support if the clocks can be compared.")
        info[role] = read_phc(intf.name, intf.bdf)
        if check_firmware:
            info[role]["enabled"] = firmware[intf.bdf]
    return info


def comparable_hardware(info: dict) -> bool:
    first = info.get("client", {})
    second = info.get("server", {})
    if any(entry.get("enabled") is False or not entry.get("identity") for entry in (first, second)):
        return False
    if first["identity"] == second["identity"]:
        return True
    if any("offset" not in entry for entry in (first, second)):
        return False
    skew = abs(first["offset"] - second["offset"])
    return skew + first["uncertainty"] + second["uncertainty"] <= PHC_SKEW_NS


def cpu_clock() -> dict:
    try:
        boot = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
    except OSError:
        boot = None
    try:
        fields = Path("/proc/self/timens_offsets").read_text().splitlines()
        offset = next(line.split()[1:] for line in fields if line.startswith("monotonic "))
    except FileNotFoundError:
        offset = ["0", "0"]
    except (OSError, StopIteration):
        offset = None
    return {"boot": boot, "offset": offset}


def shared_cpu_clock(local: dict, peer: dict) -> bool:
    return (bool(local.get("boot")) and local["boot"] == peer.get("boot")
            and local.get("offset") is not None and local["offset"] == peer.get("offset"))


def recv_exact(conn: socket.socket, size: int) -> bytes:
    result = bytearray()
    timeout = conn.gettimeout()
    deadline = time.monotonic() + timeout
    try:
        while len(result) < size:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("timed out receiving a benchmark control message")
            conn.settimeout(remaining)
            chunk = conn.recv(min(size - len(result), 65536))
            if not chunk:
                raise ConfigError("peer closed the control connection")
            result.extend(chunk)
    finally:
        conn.settimeout(timeout)
    return bytes(result)


class Peer:
    def __init__(self, args, stack: contextlib.ExitStack):
        self.server = args.role == "server"
        self.address = args.control
        self.timeout = args.peer_timeout
        self.case_timeout = 0
        self.listener = None
        if self.server:
            self.listener = stack.enter_context(socket.socket())
            self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.listener.bind(self.address)
            self.listener.listen(1)
            self.listener.settimeout(self.timeout)
        self.session = stack.enter_context(self.connect())

    def connect(self) -> socket.socket:
        if self.listener is not None:
            conn, _ = self.listener.accept()
        else:
            deadline = time.monotonic() + self.timeout
            while True:
                try:
                    conn = socket.create_connection(self.address, timeout=min(1, self.timeout))
                    break
                except (ConnectionRefusedError, TimeoutError):
                    if time.monotonic() >= deadline:
                        raise ConfigError(f"timed out connecting to peer {self.address}")
                    time.sleep(0.1)
        conn.settimeout(max(self.timeout, self.case_timeout))
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        return conn

    def exchange(self, value):
        payload = json.dumps(value, separators=(",", ":")).encode()
        if len(payload) > MAX_CONTROL_BYTES:
            raise ConfigError("control message exceeds the size limit")

        def send():
            self.session.sendall(struct.pack("!I", len(payload)) + payload)

        def receive():
            size, = struct.unpack("!I", recv_exact(self.session, 4))
            if size > MAX_CONTROL_BYTES:
                raise ConfigError("peer control message exceeds the size limit")
            return json.loads(recv_exact(self.session, size))

        if self.server:
            value = receive()
            send()
            return value
        send()
        return receive()


def settings(args) -> dict:
    # Memory, engine, cores and polling may differ for a deliberate asymmetric test.
    keys = ("transport", "topology", "test", "queues", "sizes", "size_kind", "bidirectional",
            "seconds", "warmup_seconds", "samples", "warmup_samples", "server_port",
            "client_port", "client_address", "server_address")
    result = {"protocol": 1, **{key: getattr(args, key) for key in keys}}
    if args.hardware_rtt:
        result["hardware_rtt"] = True
    return result


def phy_counters(name: str) -> dict:
    try:
        proc = subprocess.run(["ethtool", "-S", name], text=True, capture_output=True,
                              check=True, timeout=5)
    except (OSError, subprocess.SubprocessError):
        return {}
    return {key: int(value) for key, value in
            re.findall(r"^\s*(\w+):\s+(\d+)\s*$", proc.stdout, re.M)
            if key in (*PHY_PACKETS, "vport_loopback_bytes")}


class Monitor:
    def __init__(self, name: str):
        self.name = name
        self.before = phy_counters(name)
        self.rows = []
        self.proc = None
        monitor = shutil.which("mlnx_perf")
        if monitor is None:
            return
        try:
            self.proc = subprocess.Popen([monitor, "-i", name, "-t", "1"],
                                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                         text=True, env=os.environ | {"PYTHONUNBUFFERED": "1"})
        except OSError:
            return
        self.thread = threading.Thread(target=self.read, daemon=True)
        self.thread.start()

    def read(self):
        for line in self.proc.stdout:
            if match := COUNTER.match(line):
                self.rows.append((time.monotonic_ns(), match[1], float(match[2].replace(",", ""))))

    def close(self):
        if self.proc is not None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
            self.thread.join()
            self.proc.stdout.close()

    def result(self, window: dict, loopback: bool) -> dict:
        after = phy_counters(self.name)
        delta = {key: value - self.before[key] for key, value in after.items()
                 if key in self.before and value >= self.before[key]}
        rows = {}
        if window:
            for timestamp, counter, rate in self.rows:
                if window["start_ns"] + 1_000_000_000 <= timestamp <= window["end_ns"]:
                    if (counter == "vport_loopback_bytes") == loopback:
                        rows.setdefault(counter, []).append(rate * 8 / 1e9)
        return {"interface": self.name, "phy_packet_delta": {
                    name: delta.get(name) for name in PHY_PACKETS},
                "stable_gbps_samples": rows, "available": bool(rows),
                "label": "hardware-loopback throughput" if loopback else "physical-port throughput"}


def distribution(values: list[int]) -> dict:
    values = sorted(values)
    if not values:
        return {"samples": 0}
    middle = len(values) // 2
    median = values[middle] if len(values) % 2 else (values[middle - 1] + values[middle]) / 2
    return {"samples": len(values), "min_ns": values[0], "median_ns": median,
            "p95_ns": values[math.ceil(len(values) * 0.95) - 1],
            "p99_ns": values[math.ceil(len(values) * 0.99) - 1], "max_ns": values[-1]}


def case_timeout(args) -> int:
    if args.test == "bw":
        return 4 * args.timeout + args.warmup_seconds + args.seconds + 15
    return max(60, 4 * args.timeout + math.ceil((args.samples + args.warmup_samples)
                                                * args.queues * (1 + args.bidirectional) / 100))


def run_case(args, worker, path, output, peer, hardware, case):
    timeout = case_timeout(args)
    if peer:
        peer.case_timeout = timeout + args.peer_timeout
        peer.session.settimeout(peer.case_timeout)
    monitor_names = {x.name for x in args.interfaces.values() if x.bdf}
    monitors = [Monitor(name) for name in sorted(monitor_names)] if args.test == "bw" else []
    try:
        with peer.connect() if peer else contextlib.nullcontext() as connection:
            fd = connection.fileno() if connection else -1
            completed = subprocess.run([str(worker), str(path), str(output), str(fd)],
                                       pass_fds=(fd,) if connection else (),
                                       timeout=timeout, check=False)
            code = completed.returncode
    except (OSError, subprocess.SubprocessError) as exc:
        warn(f"worker failed for {args.sizes[case]} bytes ({exc})")
        code = 1
    finally:
        for monitor in monitors:
            monitor.close()
    status = peer.exchange(code) if peer else code
    if not code and not status:
        if hardware and not args.hardware_rtt:
            clocks = hardware_info(args, check_firmware=False)
            if peer:
                clocks |= peer.exchange(clocks)
            if not comparable_hardware(clocks):
                return TIMESTAMP_UNAVAILABLE, []
        result = yaml.safe_load(output.read_text())
        result["local_route"] = args.local_route
        result["configuration"] = yaml.safe_load(path.read_text())
        result["interfaces"] = {role: interface.name for role, interface in args.interfaces.items()}
        env_names = ("DAQIRI_IBV_STRIDING",) if args.engine == "ibverbs" else (
            ("DAQIRI_RDMA_SEND_SIGNAL_EVERY",) if args.roce else ())
        result["environment"] = {name: os.environ[name] for name in env_names if name in os.environ}
        result["counters"] = [
            monitor.result(result.get("window"), args.topology == "hw-loopback") | {
                "endpoint_roles": [role for role, intf in args.interfaces.items()
                                   if intf.name == monitor.name],
            } for monitor in monitors
        ]
        return 0, [result, peer.exchange(result)] if peer else [result]
    if hardware and TIMESTAMP_UNAVAILABLE in (code, status):
        return TIMESTAMP_UNAVAILABLE, []
    raise ConfigError(f"data-path worker failed for {args.sizes[case]} bytes "
                      f"(local {code}, peer {status})")


def collect(args, peers, size, rtt):
    results = []
    samples = []
    counters = []
    software_method = "application_rtt" if rtt else "tx_submit_to_rx_delivery"
    hardware_method = ("reply_rx_hw_minus_request_tx_cqe" if args.hardware_rtt
                       else "rx_hw_minus_tx_cqe")
    for peer in peers:
        counters.extend(dict(row, role=peer["role"]) for row in peer.get("counters", []))
        if args.test == "bw":
            for row in peer["bandwidth"]:
                elapsed = peer["window"]["elapsed_ns"] / 1e9
                row.update(seconds=elapsed, gbps=row["bytes"] * 8 / (elapsed * 1e9),
                           messages_per_second=row["packets"] / elapsed)
                results.append(row)
        else:
            for test in peer.get("latency", []):
                rows = test.pop("probes")
                samples.extend({"size": size, "direction": test["direction"], "queue": test["queue"],
                                "software_method": software_method,
                                "hardware_method": hardware_method if hw is not None else None,
                                "sequence": seq, "software_ns": sw, "hardware_ns": hw}
                               for seq, sw, hw in rows)
                test["software_method"] = software_method
                test["software"] = distribution([sw for _, sw, _ in rows])
                test["hardware"] = distribution([hw for _, _, hw in rows if hw is not None])
                test["hardware_method"] = hardware_method if test["hardware"]["samples"] else None
                results.append(test)
    if not results:
        raise ConfigError(f"no measurements for {size} bytes")
    if args.test == "bw":
        for row in results:
            if not row["packets"]:
                raise ConfigError(f"zero {row['kind']} traffic in {row['direction']} queue {row['queue']}")
    else:
        for row in results:
            if not row["software"]["samples"]:
                raise ConfigError(f"no matching probes in {row['direction']} queue {row['queue']} "
                                  f"({row['timeouts']} timeouts)")
    return results, samples, counters


def print_table(title, headings, rows, *, left=()):
    data = [headings, *(tuple(map(str, row)) for row in rows)]
    widths = [max(map(len, column)) for column in zip(*data)]
    formats = tuple(f"{{:{'<' if name in left else '>'}{width}}}"
                    for name, width in zip(headings, widths))
    print(f"\n{title}")
    for index, row in enumerate(data):
        print("  ".join(fmt.format(value) for fmt, value in zip(formats, row)).rstrip())
        if index == 0:
            print("  ".join("-" * width for width in widths))


def print_configuration(report):
    settings = report["settings"]
    lat = settings["test"] == "lat"
    size_unit = "frame B excluding FCS" if settings["size_kind"] == "packet" else "message B"
    sizes = ", ".join(map(str, settings["sizes"]))
    workload = (f"{settings['samples']:,} measured + {settings['warmup_samples']:,} warmup probes"
                if lat else f"{settings['seconds']} s measured + {settings['warmup_seconds']} s warmup")
    direction = "bidirectional" if settings["bidirectional"] else "client->server"
    print("DAQIRI perftest")
    print(f"  Test           {'latency' if lat else 'bandwidth'}")
    print(f"  Engine         {report['engine']} ({settings['transport']})")
    print(f"  Topology       {settings['topology']} ({report['role']})")
    print(f"  Sizes          {sizes} {size_unit}")
    print(f"  Workload       {direction}, {settings['queues']} queue pair(s), {workload} per case")
    if report.get("control"):
        print(f"  Control        {report['control']} (peer timeout {report['peer_timeout']} s)")
    print(f"  IP addresses   client {settings['client_address']}, server {settings['server_address']}")
    if settings["transport"] == "raw":
        print(f"  UDP flow base  {settings['server_port']}")
    elif settings["transport"] == "roce":
        print(f"  Server port    {settings['server_port']}")
    else:
        print(f"  Base ports     client {settings['client_port']}, server {settings['server_port']}")
    if lat:
        timings = dict.fromkeys(LATENCY_LABELS[method] for case in report["cases"]
                               for row in case["measurements"]
                               for method in (row["software_method"], row["hardware_method"]) if method)
        print(f"  Timing         {', '.join(timings)}")
        validation = "receive metadata" if report.get("opaque_payload") else "payload token"
        print(f"  Probe matching {validation}, one probe in flight")
    for role, clock in report.get("hardware_preflight", {}).items():
        firmware = {True: "enabled", False: "disabled", None: "unverified"}[clock.get("enabled")]
        print(f"  PHC {role:<10} {clock.get('identity', 'unavailable')}, firmware {firmware}")

    queues = []
    for peer in report["cases"][0]["participants"]:
        config = peer["configuration"]
        if not config:
            continue
        native = config["daqiri"]["cfg"]
        worker = config["perftest"]
        role = peer["role"]
        scheduling = (f"FIFO {worker['realtime_priority']}" if worker['realtime_priority']
                      else "default scheduling")
        print(f"  {role.capitalize():<14} DAQIRI {peer['daqiri_version']}, master CPU {native['master_core']}, "
              f"{scheduling}, timeout {worker['timeout']} s")
        for name, value in peer.get("environment", {}).items():
            print(f"                 {name}={value}")
        if worker["named_sender"]:
            print("                 raw ibverbs named sender")
        if worker.get("prepared_device_frames") and any("tx" in lane for lane in worker["lanes"]):
            print("                 prebuilt device frames, eligible for mlx5 eMPW")
        if settings["transport"] == "roce":
            print(f"                 RC work-request depth TX {worker['tx_depth']}, RX {worker['rx_depth']}")
        interfaces = {entry["name"]: entry for entry in native["interfaces"]}
        raw_endpoints = set()
        for lane in worker["lanes"]:
            interface = interfaces[lane["interface"]]
            name = peer.get("interfaces", {}).get(lane["role"])
            address = f"{name} ({interface['address']})" if name else interface["address"]
            if settings["transport"] == "raw":
                endpoint = f"{address}, MAC {lane['src_mac']}, peer {lane['dst_mac']}"
                if endpoint not in raw_endpoints:
                    print(f"                 {endpoint}")
                    raw_endpoints.add(endpoint)
            for direction in ("tx", "rx"):
                section = interface.get(direction, {})
                for queue in section.get("queues", []):
                    if queue["id"] != lane["qid"]:
                        continue
                    regions = "+".join(f"{mr['kind']}:{mr['buf_size']}:{mr['num_bufs']}:{mr['affinity']}"
                                       for mr in lane[direction])
                    app_cpu = lane.get("rx_app_cpu", lane["cpu"]) if direction == "rx" else lane["cpu"]
                    queues.append((lane["role"], lane["queue"], direction.upper(), address, app_cpu,
                                   queue.get("poll_mode", "indirect"), queue.get("cpu_core", "-"),
                                   queue.get("batch_size", 1), regions))
    if queues:
        print_table("Queue configuration (MR kind:bytes:count:affinity)",
                    ("Role", "Q", "Side", "Interface", "App CPU", "Polling", "Poll CPU", "Batch", "Memory"),
                    queues, left=("Role", "Side", "Interface", "Polling", "Memory"))


def print_report(report):
    print_configuration(report)
    cases = report["cases"]
    packets = report["settings"]["size_kind"] == "packet"
    size_column = "Frame B" if packets else "Msg B"
    rows = [(case["size"], row) for case in cases for row in case["measurements"]]
    if report["settings"]["test"] == "lat":
        results = []
        for size, row in rows:
            for mode in ("software", "hardware"):
                data = row[mode]
                if data["samples"]:
                    times = tuple(f"{data[key] / 1000:.3f}" for key in
                                  ("min_ns", "median_ns", "p95_ns", "p99_ns", "max_ns"))
                    results.append((size, row["direction"], row["queue"], LATENCY_LABELS[row[f"{mode}_method"]],
                                    f"{data['samples']:,}", *times, row["timeouts"]))
        print_table("Latency (us)", (size_column, "Direction", "Q", "Timing", "Samples",
                                      "Min", "Median", "P95", "P99", "Max", "Timeouts"),
                    results, left=("Direction", "Timing"))
        return

    results = [(size, row["direction"], row["queue"], row["kind"], f"{row['packets']:,}",
                f"{row['messages_per_second'] / 1e6:.3f}", f"{row['gbps']:.3f}",
                row["errors"], f"{row['drain_packets']:,}") for size, row in rows]
    print_table("Application bandwidth", (size_column, "Direction", "Q", "Side",
                "Frames" if packets else "Messages", "Mpps" if packets else "Mmsg/s", "Gb/s", "Errors", "Drain"),
                results, left=("Direction", "Side"))
    counters = []
    for case in cases:
        for counter in case["counters"]:
            rates = counter["stable_gbps_samples"]
            counters.extend((case["size"], counter["interface"], name, f"{statistics.median(values):.3f}",
                             len(values)) for name, values in rates.items())
            if not rates:
                counters.append((case["size"], counter["interface"], "-", "unavailable", 0))
    if counters:
        print_table("NIC throughput", (size_column, "Interface", "Counter", "Median Gb/s", "1s samples"),
                    counters, left=("Interface", "Counter"))
    else:
        print("\nHardware rate counters unavailable")
    paths = [(case["size"], case["path"]) for case in cases]
    if len({path for _, path in paths}) == 1:
        print(f"\nTraffic path  {paths[0][1]}")
    else:
        print_table("Traffic path", (size_column, "Path"), paths, left=("Path",))


def path_label(args, counters, measurements, local_route) -> str:
    if args.topology == "sw-loopback":
        return "DAQIRI software loopback"
    valid = [row["phy_packet_delta"] for row in counters]
    if args.topology == "hw-loopback":
        return ("NIC hardware loopback (PHY counters flat)" if valid and
                all(x.get(name) == 0 for x in valid for name in PHY_PACKETS)
                else "NIC hardware loopback (PHY counter check unavailable or nonzero)")
    if local_route:
        return "local kernel path"
    directions = {m["direction"] for m in measurements}
    for direction in directions:
        direction_roles = direction.split("->")
        for role, name in zip(direction_roles, PHY_PACKETS):
            if not any(role in row["endpoint_roles"] and row["phy_packet_delta"].get(name)
                       for row in counters):
                return "wire path unverified"
    return "physical path verified by directional PHY counters"


def main(argv=None) -> int:
    args = parse_args(argv)
    try:
        notices = resolve(args)
        for notice in notices:
            warn(notice)
        validator = executable(args, "daqiri_config_validate", "tools")
        engine = args.engine or ("ibverbs" if args.roce else "socket")
        engines = subprocess.check_output([validator, "--list-engines"], text=True, timeout=5)
        if engine not in engines.split():
            raise ConfigError(f"this build does not include the {engine} engine")
        candidate = args.hardware_rtt or (args.test == "lat" and args.topology != "two-host"
                                          and engine == "ibverbs")
        rtt = args.hardware_rtt or (args.test == "lat" and args.topology == "two-host")
        initiators = ("client", "server") if args.bidirectional else ("client",)
        with tempfile.TemporaryDirectory(prefix="daqiri-perftest-") as tmp, contextlib.ExitStack() as stack:
            work = Path(tmp)
            configs = []
            for case in range(len(args.sizes)):
                document = build_config(args, case, hardware=candidate, rtt=rtt)
                path = work / f"case-{case}.yaml"
                path.write_text(render_document(document))
                configs.append(path)
            validated = subprocess.run([validator, *configs], stdout=subprocess.PIPE, timeout=15,
                                       text=True, check=False)
            if validated.returncode:
                raise ConfigError("generated configuration failed native validation")
            if args.named_sender:
                message = ("Each message is one IPv4/UDP payload." if args.size_kind == "message"
                           else "The named sender supplies the header. Packet sizes count full frames.")
                print(f"Using the raw ibverbs named sender. {message}", file=sys.stderr, flush=True)
            elif args.prepared_device_frames:
                print("Preparing complete device frames at initialization for compatible mlx5 eMPW bursts.",
                      file=sys.stderr, flush=True)
            if args.dry_run:
                for path in configs:
                    print(path.read_text(), end="")
                return 0
            worker = executable(args, "daqiri_perftest_worker", "examples")
            print(f"{args.transport} {args.topology} {args.role} ports "
                  f"{', '.join(role + '=' + intf.name for role, intf in args.interfaces.items()) or 'none'}",
                  file=sys.stderr, flush=True)
            findings = tuning_preflight(args)
            stamps_locally = candidate and (not args.hardware_rtt or args.role in initiators)
            clocks = hardware_info(args) if stamps_locally else {}
            local_clock = cpu_clock() if args.role != "local" else {}
            peer = Peer(args, stack) if args.role != "local" else None
            same_boot = True
            peer_findings = []
            peer_notices = []
            if peer:
                hello = peer.exchange({"settings": settings(args), "clock": local_clock,
                                       "hardware": clocks, "role": args.role,
                                       "findings": findings, "notices": notices,
                                       "benchmark_cpus": sorted(args.benchmark_cpus),
                                       "opaque_payload": args.opaque_payload,
                                       "refresh_phc": True})
                if hello["settings"] != settings(args) or hello["role"] == args.role:
                    raise ConfigError("benchmark peer settings or roles do not match")
                if args.opaque_payload and "opaque_payload" not in hello:
                    raise ConfigError("the benchmark peer does not support opaque device payloads")
                args.opaque_payload |= hello.get("opaque_payload", False)
                same_boot = bool(local_clock["boot"]) and local_clock["boot"] == hello["clock"].get("boot")
                clocks |= hello["hardware"]
                if same_boot and candidate and not args.hardware_rtt and hello.get("refresh_phc"):
                    # Clock reads must be close together even if peer startup was staggered.
                    fresh = hardware_info(args, check_firmware=False)
                    fresh |= peer.exchange(fresh)
                    clocks = {role: {"enabled": clocks[role].get("enabled")} | sample
                              for role, sample in fresh.items()}
                if same_boot:
                    shared = sorted(args.benchmark_cpus.intersection(hello.get("benchmark_cpus", ())))
                    if shared:
                        cpus = ", ".join(map(str, shared))
                        notice = f"Benchmark peers share CPUs on this host {cpus}"
                        notices.append(notice)
                        warn(notice)
                if args.test == "lat" and args.topology == "same-host" and not args.hardware_rtt:
                    rtt = not shared_cpu_clock(local_clock, hello["clock"])
                    if rtt:
                        warn("Could not verify a shared CPU clock. Using application RTT.")
                peer_findings = hello["findings"]
                peer_notices = hello["notices"]
                for finding in peer_findings + peer_notices:
                    warn(f"peer: {finding}")
            if args.hardware_rtt:
                # Each initiator stamps its request and reply on the same NIC.
                hardware = all(clocks.get(role, {}).get("identity")
                               and clocks[role].get("enabled") is not False for role in initiators)
            else:
                hardware = candidate and same_boot and comparable_hardware(clocks)
            if args.test == "lat":
                if candidate and not hardware:
                    warn("hardware latency is unavailable with the selected firmware or clock domains")
                warn("software latencies include TX and RX processing")
                if args.opaque_payload:
                    print("Matching opaque probes by receive queue, length, and one probe in flight.",
                          file=sys.stderr, flush=True)
            print("Preflight complete. Starting benchmark.", file=sys.stderr, flush=True)
            report = {"schema": 1, "settings": settings(args), "role": args.role,
                      "engine": engine, "opaque_payload": args.opaque_payload,
                      "control": f"{args.control[0]}:{args.control[1]}" if args.control else None,
                      "peer_timeout": args.peer_timeout if peer else None,
                      "preflight": findings, "notices": notices,
                      "peer_preflight": peer_findings, "peer_notices": peer_notices,
                      "hardware_preflight": clocks, "cases": []}
            all_samples = []
            try:
                for case, (size, path) in enumerate(zip(args.sizes, configs)):
                    if len(args.sizes) > 1:
                        print(f"Measuring {size} B {args.size_kind}s", file=sys.stderr, flush=True)
                    current_hw = hardware
                    for _ in range(2):
                        document = build_config(args, case, hardware=current_hw, rtt=rtt)
                        path.write_text(render_document(document))
                        status, results = run_case(args, worker, path, work / f"result-{case}.yaml",
                                                   peer, current_hw, case)
                        if not status:
                            break
                        current_hw = False
                        warn(f"Hardware timestamps unavailable for {size} bytes. Repeating with CPU timing.")
                    measurements, samples, counters = collect(args, results, size, rtt)
                    local_route = any(item["local_route"] for item in results)
                    result = {"size": size, "size_kind": args.size_kind,
                              "measurements": measurements, "counters": counters,
                              "path": path_label(args, counters, measurements, local_route)
                              if args.test == "bw" else None,
                              "participants": [
                                  {"role": item["role"], "daqiri_version": item["version"],
                                   "configuration": item["configuration"],
                                   "interfaces": item.get("interfaces", {}),
                                   "environment": item.get("environment", {})}
                                  for item in results
                              ]}
                    if args.raw:
                        result["frame_size"] = frame_size(args, size)
                    for sample in samples:
                        sample["size_kind"] = args.size_kind
                        if args.raw:
                            sample["frame_size"] = result["frame_size"]
                    report["cases"].append(result)
                    all_samples.extend(samples)
            finally:
                if report["cases"]:
                    print_report(report)
            if args.json:
                args.json.write_text(json.dumps(report, indent=2) + "\n")
            if args.samples_csv and all_samples:
                with args.samples_csv.open("w", newline="") as stream:
                    writer = csv.DictWriter(stream, fieldnames=list(all_samples[0]))
                    writer.writeheader()
                    writer.writerows(all_samples)
        return 0
    except (ConfigError, OSError, subprocess.SubprocessError, ValueError) as exc:
        print(f"daqiri_perftest: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
