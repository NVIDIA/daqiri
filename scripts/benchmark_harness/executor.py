# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"""Safe local/SSH executor for the benchmark lifecycle."""

from __future__ import annotations

import base64
import json
import shlex
import subprocess
import time
from pathlib import Path
from typing import Any


class ExecutorError(RuntimeError):
    """Raised when a host worker cannot complete an orchestration operation."""


class HostExecutor:
    """Call the constant remote worker without interpolating dynamic role arguments."""

    def __init__(self, host_id: str, config: dict[str, Any], worker_path: Path):
        self.host_id = host_id
        self.config = config
        self.worker_path = worker_path
        self.worker_source = worker_path.read_text(encoding="utf-8")

    def _command(self, mode: str) -> list[str]:
        prefix = list(self.config["worker_prefix"])
        python = self.config["python"]
        if self.config["transport"] == "local":
            return [*prefix, python, str(self.worker_path), mode]
        remote_command = shlex.join([*prefix, python, "-c", self.worker_source, mode])
        return ["ssh", self.config["destination"], remote_command]

    def rpc(
        self, mode: str, payload: dict[str, Any], *, timeout_seconds: float = 30
    ) -> dict[str, Any]:
        try:
            completed = subprocess.run(
                self._command(mode),
                input=json.dumps(payload, sort_keys=True, separators=(",", ":")),
                capture_output=True,
                text=True,
                timeout=timeout_seconds,
                check=False,
            )
        except (OSError, subprocess.SubprocessError) as exc:
            raise ExecutorError(
                f"{self.host_id} {mode} transport failed: {exc}"
            ) from exc
        response_line = completed.stdout.strip().splitlines()
        if completed.returncode != 0 or not response_line:
            raise ExecutorError(
                f"{self.host_id} {mode} worker failed rc={completed.returncode}: "
                f"{completed.stderr.strip() or completed.stdout.strip()}"
            )
        try:
            response = json.loads(response_line[-1])
        except json.JSONDecodeError as exc:
            raise ExecutorError(
                f"{self.host_id} {mode} returned invalid JSON: {response_line[-1]}"
            ) from exc
        if not response.get("ok"):
            raise ExecutorError(
                f"{self.host_id} {mode} failed: {response.get('error', response)}"
            )
        return response["result"]

    def preflight(self, payload: dict[str, Any]) -> dict[str, Any]:
        return self.rpc("preflight", payload)

    def write_text(self, path: str, content: str) -> dict[str, Any]:
        return self.rpc(
            "write",
            {
                "path": path,
                "content_base64": base64.b64encode(content.encode()).decode(),
                "mode": 0o444,
            },
        )

    def read_bytes(self, path: str) -> bytes:
        result = self.rpc("read", {"path": path})
        return base64.b64decode(result["content_base64"])

    def run(
        self,
        argv: list[str],
        *,
        cwd: str | None = None,
        environment: dict[str, str] | None = None,
        timeout_seconds: float = 30,
    ) -> dict[str, Any]:
        return self.rpc(
            "run",
            {
                "argv": argv,
                "cwd": cwd,
                "environment": environment or {},
                "timeout_seconds": timeout_seconds,
            },
            timeout_seconds=timeout_seconds + 5,
        )

    def launch(self, role: dict[str, Any]) -> dict[str, Any]:
        payload = {
            "worker_source": self.worker_source,
            "run_id": role["run_id"],
            "role_id": role["role_id"],
            "state_path": role["state_path"],
            "exit_path": role["exit_path"],
            "stdout_path": role["stdout_path"],
            "stderr_path": role["stderr_path"],
            "argv": role["command"],
            "cwd": role["cwd"],
            "environment": role.get("environment", {}),
            "container_name": role.get("container_name"),
        }
        return self.rpc("launch", payload, timeout_seconds=10)

    def status(self, state_path: str) -> dict[str, Any]:
        return self.rpc("status", {"state_path": state_path})

    def wait(self, state_path: str, timeout_seconds: float) -> dict[str, Any]:
        deadline = time.monotonic() + timeout_seconds
        while time.monotonic() < deadline:
            status = self.status(state_path)
            if status["status"] != "running":
                return status
            time.sleep(0.1)
        raise ExecutorError(f"{self.host_id} timed out waiting for {state_path}")

    def wait_udp_ports(
        self, ports: list[int], state_paths: list[str], timeout_seconds: float
    ) -> None:
        deadline = time.monotonic() + timeout_seconds
        while time.monotonic() < deadline:
            for state_path in state_paths:
                status = self.status(state_path)
                if status["status"] != "running":
                    raise ExecutorError(
                        f"{self.host_id} receiver exited before readiness: {state_path}"
                    )
                if status["identity_errors"]:
                    raise ExecutorError(
                        f"{self.host_id} receiver identity failed: {status['identity_errors']}"
                    )
            in_use = set(self.rpc("ports", {"ports": ports})["ports_in_use"])
            if set(ports).issubset(in_use):
                return
            time.sleep(0.1)
        raise ExecutorError(f"{self.host_id} UDP readiness timed out for ports {ports}")

    def wait_log_contains(
        self, path: str, pattern: str, state_path: str, timeout_seconds: float
    ) -> None:
        deadline = time.monotonic() + timeout_seconds
        while time.monotonic() < deadline:
            status = self.status(state_path)
            if status["status"] != "running":
                raise ExecutorError(
                    f"{self.host_id} process exited before log readiness: {path}"
                )
            try:
                if pattern in self.read_bytes(path).decode(errors="replace"):
                    return
            except ExecutorError:
                pass
            time.sleep(0.1)
        raise ExecutorError(
            f"{self.host_id} readiness marker {pattern!r} not found in {path}"
        )

    def terminate(
        self, state_path: str, timeout_seconds: float, *, initial_signal: str = "TERM"
    ) -> dict[str, Any]:
        return self.rpc(
            "terminate",
            {
                "state_path": state_path,
                "timeout_seconds": timeout_seconds,
                "initial_signal": initial_signal,
            },
            timeout_seconds=(3 * timeout_seconds) + 5,
        )

    def snapshot(self, netdevs: list[str], cpus: list[int]) -> dict[str, Any]:
        return self.rpc("snapshot", {"netdevs": netdevs, "cpus": cpus})


def build_executors(plan: dict[str, Any], worker_path: Path) -> dict[str, HostExecutor]:
    return {
        host_id: HostExecutor(host_id, host, worker_path)
        for host_id, host in plan["site"]["hosts"].items()
    }
