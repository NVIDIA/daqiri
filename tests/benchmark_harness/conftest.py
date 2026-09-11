# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import copy
import sys
from pathlib import Path

import pytest
import yaml

REPOSITORY = Path(__file__).resolve().parents[2]
SCRIPTS = REPOSITORY / "scripts"
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))


@pytest.fixture
def suite() -> dict:
    path = SCRIPTS / "benchmark_harness" / "sample-configs" / "physical-udp-suite.yaml"
    value = yaml.safe_load(path.read_text(encoding="utf-8"))
    value["duration_seconds"] = 1
    value["repetitions"] = 1
    value["drain_seconds"] = 1
    value["readiness_timeout_seconds"] = 1
    value["cleanup_timeout_seconds"] = 1
    value["acceptance"]["minimum_mlnx_samples"] = 2
    return value


@pytest.fixture
def site() -> dict:
    path = (
        SCRIPTS
        / "benchmark_harness"
        / "sample-configs"
        / "physical-udp-site.example.yaml"
    )
    return yaml.safe_load(path.read_text(encoding="utf-8"))


@pytest.fixture
def cloned():
    return copy.deepcopy


def snapshot(host_id: str, index: int) -> dict:
    completed_repetitions = (index - 1) // 5
    phase = ((index - 1) % 5) + 1
    traffic_repetitions = completed_repetitions + (1 if phase >= 3 else 0)
    netdev_stats = {
        "rx_out_of_buffer": 0,
        "rx_prio0_buf_discard_packets": 0,
        "rx_discards_phy": 0,
        "tx_packets_phy": (traffic_repetitions * 1000 if host_id == "tx-host" else 0),
        "rx_packets_phy": (traffic_repetitions * 1000 if host_id == "rx-host" else 0),
        "tx_bytes_phy": (
            traffic_repetitions * 8_000_000 if host_id == "tx-host" else 0
        ),
        "rx_bytes_phy": (
            traffic_repetitions * 8_000_000 if host_id == "rx-host" else 0
        ),
    }
    return {
        "schema_version": "daqiri.counter-snapshot/v1",
        "utc": "2026-09-10T00:00:00+00:00",
        "monotonic_ns": index * 1_000_000_000,
        "snmp": {
            "Udp": {"InErrors": 0, "RcvbufErrors": 0},
            "Ip": {"ReasmFails": 0},
        },
        "cpu": {
            f"cpu{cpu}": {"total": index * 100, "busy": index * 50} for cpu in (1, 2, 3)
        },
        "netdevs": {"enp1s0f0np0": netdev_stats},
    }


class FakeExecutor:
    def __init__(self, host_id: str, **failures):
        self.host_id = host_id
        self.failures = failures
        self.processes: dict[str, dict] = {}
        self.files: dict[str, bytes] = {}
        self.snapshot_count = 0
        self.terminate_calls: list[tuple[str, str]] = []

    def preflight(self, _payload):
        return {"ok": True, "errors": [], "facts": {}}

    def run(self, argv, **_kwargs):
        return {"returncode": 0, "stdout": "ok\n", "stderr": ""}

    def write_text(self, path, content):
        self.files[path] = content.encode()
        return {"path": path, "size": len(content)}

    def launch(self, role):
        state = {
            "run_id": role["run_id"],
            "role_id": role["role_id"],
            "pid": len(self.processes) + 100,
            "pgid": len(self.processes) + 100,
            "start_ticks": len(self.processes) + 1000,
        }
        self.processes[role["state_path"]] = {
            "state": state,
            "running": True,
            "exit": None,
        }
        return state

    def status(self, state_path):
        process = self.processes[state_path]
        role_id = process["state"]["role_id"]
        if process["running"] and role_id.startswith("tx-"):
            if self.failures.get("interrupt_on_wait"):
                raise KeyboardInterrupt
            process["running"] = False
            process["exit"] = {
                "exit_code": self.failures.get("tx_exit", 0),
                "signal": None,
            }
        return {
            "status": "running" if process["running"] else "completed",
            "state": process["state"],
            "exit": process["exit"],
            "members": [{"pid": process["state"]["pid"]}] if process["running"] else [],
            "container": (
                {"id": "owned-container", "status": "running"}
                if self.failures.get("container_cleanup")
                else None
            ),
            "identity_errors": [],
        }

    def wait(self, state_path, _timeout_seconds):
        process = self.processes[state_path]
        process["running"] = False
        exit_code = self.failures.get("tx_exit", 0)
        process["exit"] = {"exit_code": exit_code, "signal": None}
        return self.status(state_path)

    def wait_udp_ports(self, _ports, _state_paths, _timeout_seconds):
        if self.failures.get("readiness"):
            from benchmark_harness.executor import ExecutorError

            raise ExecutorError("synthetic readiness timeout")

    def wait_log_contains(self, _path, _pattern, _state_path, _timeout_seconds):
        if self.failures.get("collector_ready"):
            from benchmark_harness.executor import ExecutorError

            raise ExecutorError("synthetic collector readiness failure")

    def terminate(self, state_path, _timeout_seconds, *, initial_signal="TERM"):
        if state_path not in self.processes:
            from benchmark_harness.executor import ExecutorError

            raise ExecutorError("synthetic process state is absent")
        self.terminate_calls.append((state_path, initial_signal))
        process = self.processes[state_path]
        process["running"] = False
        role_id = process["state"]["role_id"]
        exit_code = self.failures.get("rx_exit", 0) if role_id.startswith("rx-") else 0
        process["exit"] = {"exit_code": exit_code, "signal": initial_signal}
        if self.failures.get("cleanup"):
            return {
                "ok": False,
                "members": [{"pid": process["state"]["pid"]}],
                "errors": [],
            }
        return {"ok": True, "members": [], "errors": []}

    def snapshot(self, _netdevs, _cpus):
        self.snapshot_count += 1
        if self.failures.get("snapshot") == self.snapshot_count:
            from benchmark_harness.executor import ExecutorError

            raise ExecutorError("synthetic snapshot failure")
        return snapshot(self.host_id, self.snapshot_count)

    def read_bytes(self, path):
        if "mlnx-" in path:
            if self.failures.get("collector_output"):
                return b""
            counter = "tx_bytes_phy" if "mlnx-tx-" in path else "rx_bytes_phy"
            if path.endswith("stderr.log"):
                return b""
            lines = [
                "Initializing mlnx_perf...",
                "Sampling started.",
                *[f"{counter}: 3,125,000,000 Bps = 25,000 Mbps" for _ in range(5)],
            ]
            return ("\n".join(lines) + "\n").encode()
        if path.endswith("stderr.log"):
            if "/tx-" in path:
                return (
                    b"[INFO] Socket engine stats: tx_pkts=1000 tx_bytes=8000000 "
                    b"rx_pkts=0 rx_bytes=0\n"
                )
            return (
                b"[INFO] Socket engine stats: tx_pkts=0 tx_bytes=0 "
                b"rx_pkts=1000 rx_bytes=8000000\n"
            )
        if "/tx-" in path:
            if self.failures.get("missing_summary"):
                return b"benchmark exited without a summary\n"
            return (
                b"Client complete: sent_packets=1000 recv_packets=0 sent_bytes=8000000 "
                b"recv_bytes=0 max_rx_burst=0 seconds=1.0\n"
            )
        if "/rx-" in path:
            if self.failures.get("missing_summary"):
                return b"benchmark exited without a summary\n"
            return (
                b"Server complete: sent_packets=0 recv_packets=1000 sent_bytes=0 "
                b"recv_bytes=8000000 max_rx_burst=32 seconds=2.0\n"
            )
        return self.files[path]


@pytest.fixture
def fake_executors():
    def factory(**failures):
        return {
            "tx-host": FakeExecutor("tx-host", **failures),
            "rx-host": FakeExecutor("rx-host", **failures),
        }

    return factory
