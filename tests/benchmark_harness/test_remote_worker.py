# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path

from benchmark_harness import remote_worker
from benchmark_harness.executor import ExecutorError, HostExecutor
from benchmark_harness.remote_worker import _owned_container, _preflight, _terminate

WORKER = (
    Path(__file__).resolve().parents[2] / "scripts/benchmark_harness/remote_worker.py"
)


def executor() -> HostExecutor:
    return HostExecutor(
        "local-test",
        {
            "transport": "local",
            "python": sys.executable,
            "worker_prefix": [],
        },
        WORKER,
    )


def role(tmp_path, command):
    return {
        "run_id": "owned-process-test",
        "role_id": "test-role",
        "state_path": str(tmp_path / "process.json"),
        "exit_path": str(tmp_path / "exit.json"),
        "stdout_path": str(tmp_path / "stdout.log"),
        "stderr_path": str(tmp_path / "stderr.log"),
        "command": command,
        "container_name": None,
        "cwd": str(tmp_path),
        "environment": {},
    }


def test_terminate_targets_exact_owned_process_group(tmp_path):
    host = executor()
    launched = host.launch(
        role(tmp_path, [sys.executable, "-c", "import time; time.sleep(60)"])
    )
    assert launched["run_id"] == "owned-process-test"
    assert host.status(str(tmp_path / "process.json"))["status"] == "running"

    stopped = host.terminate(str(tmp_path / "process.json"), 1)

    assert stopped["ok"] is True, stopped
    assert stopped["members"] == []
    assert host.status(str(tmp_path / "process.json"))["members"] == []


def test_terminate_escalates_to_kill_and_verifies_no_members(tmp_path):
    host = executor()
    command = [
        sys.executable,
        "-c",
        (
            "import signal,time; "
            "signal.signal(signal.SIGTERM, signal.SIG_IGN); "
            "signal.signal(signal.SIGINT, signal.SIG_IGN); "
            "time.sleep(60)"
        ),
    ]
    host.launch(role(tmp_path, command))
    time.sleep(0.1)

    stopped = host.terminate(str(tmp_path / "process.json"), 0.1)

    assert stopped["ok"] is True, stopped
    assert stopped["escalated_to_kill"] is True
    assert stopped["members"] == []
    assert host.status(str(tmp_path / "process.json"))["members"] == []


def test_remote_write_is_immutable(tmp_path):
    host = executor()
    path = tmp_path / "resolved-config.yaml"
    host.write_text(str(path), "version: 1\n")
    assert path.read_text() == "version: 1\n"

    try:
        host.write_text(str(path), "version: 2\n")
    except ExecutorError as exc:
        assert "refusing" in str(exc) or "FileExistsError" in str(exc)
    else:
        raise AssertionError("worker unexpectedly overwrote an immutable artifact")


def test_preflight_verifies_expected_binary_hash(tmp_path):
    binary = Path("/bin/true")
    worktree = tmp_path / "source"
    subprocess.run(["git", "init", "-q", str(worktree)], check=True)
    (worktree / "tracked").write_text("identity\n")
    subprocess.run(["git", "-C", str(worktree), "add", "tracked"], check=True)
    subprocess.run(
        [
            "git",
            "-C",
            str(worktree),
            "-c",
            "user.name=Harness Test",
            "-c",
            "user.email=harness@example.invalid",
            "commit",
            "-q",
            "-m",
            "identity",
        ],
        check=True,
    )
    expected_commit = subprocess.run(
        ["git", "-C", str(worktree), "rev-parse", "HEAD"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
    payload = {
        "worktree": str(worktree),
        "binary": str(binary),
        "expected_binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "expected_commit": expected_commit,
        "container": {
            "runtime": "none",
            "image": "",
            "digest": "",
            "workdir": "",
            "gpus": False,
        },
        "require_clean": False,
        "cpus": [min(os.sched_getaffinity(0))],
        "netdevs": [],
        "addresses": [],
        "ports": [],
        "endpoints": [],
        "routes": [],
        "require_mlnx_perf": False,
        "requirements": {"gpu": False, "hugepages": False},
    }
    assert _preflight(payload)["ok"] is True

    payload["expected_binary_sha256"] = "0" * 64
    result = _preflight(payload)
    assert result["ok"] is False
    assert any("binary SHA-256 mismatch" in error for error in result["errors"])


def test_container_identity_uses_immutable_id_and_ownership_label(monkeypatch):
    expected_id = "sha256:" + ("a" * 64)
    inspected: list[str] = []

    def inspect(identity):
        inspected.append(identity)
        return {
            "Id": expected_id,
            "Config": {
                "Labels": {
                    remote_worker.CONTAINER_LABEL: "owned-process-test",
                }
            },
            "State": {"Status": "running"},
        }

    monkeypatch.setattr(remote_worker, "_inspect_container", inspect)
    container, errors = _owned_container(
        {
            "run_id": "owned-process-test",
            "container": {"name": "readable-name", "id": expected_id},
        }
    )

    assert inspected == [expected_id]
    assert container == {
        "name": "readable-name",
        "id": expected_id,
        "status": "running",
    }
    assert errors == []


def test_cleanup_refuses_container_with_failed_identity(tmp_path, monkeypatch):
    state_path = tmp_path / "process.json"
    state_path.write_text(
        json.dumps(
            {
                "run_id": "expected-run-id",
                "pid": 123,
                "pgid": 123,
                "start_ticks": 456,
                "container": {"name": "role", "id": "container-id"},
            }
        )
    )
    monkeypatch.setattr(remote_worker, "_owned_members", lambda _state: ([], []))
    monkeypatch.setattr(
        remote_worker,
        "_owned_container",
        lambda _state: (
            {"name": "role", "id": "different-id", "status": "running"},
            ["container identity changed"],
        ),
    )

    result = _terminate(
        {
            "state_path": str(state_path),
            "timeout_seconds": 0.01,
            "initial_signal": "TERM",
        }
    )

    assert result["ok"] is False
    assert result["errors"] == ["container identity changed"]
    assert result["escalated_to_kill"] is False
