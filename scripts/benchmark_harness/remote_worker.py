# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"""Constant JSON-over-stdin worker used identically on local and SSH hosts.

The controller never interpolates benchmark arguments into a remote shell.
Each launched role has a persisted run ID, supervisor PID/process group, and
Linux process start tick. Cleanup verifies that identity before signalling the
exact process group.
"""

from __future__ import annotations

import base64
import datetime as dt
import hashlib
import json
import math
import os
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

CONTAINER_LABEL = "com.nvidia.daqiri.benchmark.process_id"


def _reply(value: dict[str, Any]) -> None:
    print(json.dumps(value, sort_keys=True, separators=(",", ":")), flush=True)


def _read_payload() -> dict[str, Any]:
    value = json.load(sys.stdin)
    if not isinstance(value, dict):
        raise TypeError("worker payload must be a JSON object")
    return value


def _atomic_json(path: Path, value: dict[str, Any], *, exclusive: bool = False) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    with temporary.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, sort_keys=True, separators=(",", ":"))
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    if exclusive:
        try:
            os.link(temporary, path)
        finally:
            temporary.unlink(missing_ok=True)
    else:
        os.replace(temporary, path)


def _proc_stat(pid: int) -> dict[str, Any] | None:
    try:
        value = Path(f"/proc/{pid}/stat").read_text(encoding="utf-8")
    except (FileNotFoundError, ProcessLookupError):
        return None
    suffix = value.rsplit(")", 1)[1].split()
    return {
        "state": suffix[0],
        "ppid": int(suffix[1]),
        "pgid": int(suffix[2]),
        "start_ticks": int(suffix[19]),
    }


def _proc_run_id(pid: int) -> str | None:
    try:
        environ = Path(f"/proc/{pid}/environ").read_bytes().split(b"\0")
    except (FileNotFoundError, PermissionError, ProcessLookupError):
        return None
    prefix = b"DAQIRI_BENCH_RUN_ID="
    for item in environ:
        if item.startswith(prefix):
            return item[len(prefix) :].decode(errors="replace")
    return None


def _group_members(pgid: int) -> list[dict[str, Any]]:
    members: list[dict[str, Any]] = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        stat = _proc_stat(int(entry.name))
        if stat and stat["pgid"] == pgid and stat["state"] != "Z":
            members.append({"pid": int(entry.name), **stat})
    return sorted(members, key=lambda member: member["pid"])


def _load_state(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as stream:
        state = json.load(stream)
    if not isinstance(state, dict):
        raise TypeError(f"invalid process state: {path}")
    return state


def _owned_members(state: dict[str, Any]) -> tuple[list[dict[str, Any]], list[str]]:
    observed = _group_members(int(state["pgid"]))
    members: list[dict[str, Any]] = []
    errors: list[str] = []
    for member in observed:
        run_id = _proc_run_id(member["pid"])
        if run_id != state["run_id"]:
            current = _proc_stat(member["pid"])
            if current is None or current["state"] == "Z":
                continue
            errors.append(
                f"pid {member['pid']} in pgid {state['pgid']} does not carry run id {state['run_id']}"
            )
        members.append(member)
    supervisor = _proc_stat(int(state["pid"]))
    if supervisor and supervisor["state"] != "Z":
        if supervisor["start_ticks"] != int(state["start_ticks"]):
            errors.append(f"supervisor pid {state['pid']} start time changed")
        if supervisor["pgid"] != int(state["pgid"]):
            errors.append(f"supervisor pid {state['pid']} process group changed")
    return members, errors


def _inspect_container(name: str) -> dict[str, Any] | None:
    completed = subprocess.run(
        ["docker", "container", "inspect", name],
        capture_output=True,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        if (
            "No such object" in completed.stderr
            or "No such container" in completed.stderr
        ):
            return None
        raise RuntimeError(f"docker inspect {name} failed: {completed.stderr.strip()}")
    values = json.loads(completed.stdout)
    if len(values) != 1:
        raise RuntimeError(f"docker inspect {name} returned {len(values)} objects")
    return values[0]


def _owned_container(
    state: dict[str, Any],
) -> tuple[dict[str, Any] | None, list[str]]:
    expected = state.get("container")
    if not expected:
        return None, []
    current = _inspect_container(expected["id"])
    if current is None:
        return None, []
    errors: list[str] = []
    labels = current.get("Config", {}).get("Labels") or {}
    if labels.get(CONTAINER_LABEL) != state["run_id"]:
        errors.append(
            f"container {expected['name']} does not carry run id {state['run_id']}"
        )
    if current.get("Id") != expected["id"]:
        errors.append(f"container {expected['name']} identity changed")
    return {
        "name": expected["name"],
        "id": current.get("Id"),
        "status": current.get("State", {}).get("Status"),
    }, errors


def _wait_for_owned_exit(
    state: dict[str, Any], timeout_seconds: float
) -> tuple[list[dict[str, Any]], dict[str, Any] | None, list[str]]:
    deadline = time.monotonic() + timeout_seconds
    while True:
        # Ownership is checked immediately before the first signal. During exit,
        # Linux can retain a runnable /proc stat briefly after environ is gone,
        # so rereading the marker here would create a false identity failure.
        members = _group_members(int(state["pgid"]))
        container, container_errors = _owned_container(state)
        if (
            container_errors
            or (not members and container is None)
            or time.monotonic() >= deadline
        ):
            return members, container, container_errors
        time.sleep(0.05)


def _signal_container(
    state: dict[str, Any], action: str, timeout_seconds: float
) -> tuple[bool, list[str]]:
    container, errors = _owned_container(state)
    if errors or container is None:
        return False, errors
    container_id = state["container"]["id"]
    if action == "stop":
        argv = [
            "docker",
            "container",
            "stop",
            "--signal",
            "TERM",
            "--time",
            str(max(1, math.ceil(timeout_seconds))),
            container_id,
        ]
    elif action == "kill":
        argv = ["docker", "container", "kill", container_id]
    else:
        raise ValueError(f"unsupported container action: {action}")
    completed = subprocess.run(argv, capture_output=True, text=True, check=False)
    if completed.returncode != 0:
        remaining, remaining_errors = _owned_container(state)
        if remaining_errors:
            return True, remaining_errors
        if remaining is not None:
            return True, [
                f"docker {action} {container_id} failed: {completed.stderr.strip()}"
            ]
    return True, []


def _supervise(payload: dict[str, Any]) -> int:
    state_path = Path(payload["state_path"])
    exit_path = Path(payload["exit_path"])
    stdout_path = Path(payload["stdout_path"])
    stderr_path = Path(payload["stderr_path"])
    for path in (state_path, exit_path, stdout_path, stderr_path):
        path.parent.mkdir(parents=True, exist_ok=True)

    os.environ["DAQIRI_BENCH_RUN_ID"] = payload["run_id"]
    received_signal: int | None = None

    def remember_signal(signum: int, _frame: Any) -> None:
        nonlocal received_signal
        received_signal = signum

    signal.signal(signal.SIGTERM, remember_signal)
    signal.signal(signal.SIGINT, remember_signal)
    started_utc = dt.datetime.now(dt.timezone.utc).isoformat()
    try:
        with (
            stdout_path.open("x", encoding="utf-8") as stdout,
            stderr_path.open("x", encoding="utf-8") as stderr,
        ):
            environment = os.environ.copy()
            environment.update(payload.get("environment", {}))
            child = subprocess.Popen(
                payload["argv"],
                cwd=payload["cwd"],
                env=environment,
                stdin=subprocess.DEVNULL,
                stdout=stdout,
                stderr=stderr,
                text=True,
                close_fds=True,
            )
            stat = _proc_stat(os.getpid())
            child_stat = _proc_stat(child.pid)
            if stat is None or child_stat is None:
                raise RuntimeError("cannot read launched process identity")
            container_state: dict[str, str] | None = None
            container_name = payload.get("container_name")
            if container_name:
                deadline = time.monotonic() + 5.0
                inspected = None
                while time.monotonic() < deadline and child.poll() is None:
                    inspected = _inspect_container(container_name)
                    if inspected is not None:
                        break
                    time.sleep(0.02)
                if inspected is None:
                    raise RuntimeError(
                        f"container {container_name} did not publish its identity"
                    )
                labels = inspected.get("Config", {}).get("Labels") or {}
                if labels.get(CONTAINER_LABEL) != payload["run_id"]:
                    raise RuntimeError(
                        f"container {container_name} has the wrong ownership label"
                    )
                container_state = {"name": container_name, "id": inspected["Id"]}
            state = {
                "schema_version": "daqiri.remote-process/v1",
                "run_id": payload["run_id"],
                "role_id": payload["role_id"],
                "pid": os.getpid(),
                "pgid": os.getpgrp(),
                "start_ticks": stat["start_ticks"],
                "child_pid": child.pid,
                "child_start_ticks": child_stat["start_ticks"],
                "argv": payload["argv"],
                "cwd": payload["cwd"],
                "started_utc": started_utc,
                "exit_path": str(exit_path),
                "container": container_state,
            }
            _atomic_json(state_path, state, exclusive=True)
            while child.poll() is None:
                time.sleep(0.05)
            exit_code = child.returncode
    except BaseException as exc:  # noqa: BLE001 - persist every supervisor exit
        exit_code = 127
        with stderr_path.open("a", encoding="utf-8") as stderr:
            stderr.write(f"remote supervisor failure: {type(exc).__name__}: {exc}\n")

    _atomic_json(
        exit_path,
        {
            "schema_version": "daqiri.remote-exit/v1",
            "run_id": payload["run_id"],
            "role_id": payload["role_id"],
            "exit_code": exit_code,
            "signal": received_signal,
            "started_utc": started_utc,
            "ended_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        },
        exclusive=True,
    )
    return 0


def _launch(payload: dict[str, Any]) -> dict[str, Any]:
    source = payload.pop("worker_source")
    state_path = Path(payload["state_path"])
    if state_path.exists() or Path(payload["exit_path"]).exists():
        raise FileExistsError(f"refusing to reuse process identity {state_path}")
    encoded = base64.b64encode(
        json.dumps(payload, separators=(",", ":")).encode()
    ).decode()
    environment = os.environ.copy()
    environment["DAQIRI_REMOTE_SUPERVISOR_PAYLOAD"] = encoded
    environment["DAQIRI_BENCH_RUN_ID"] = payload["run_id"]
    supervisor = subprocess.Popen(
        [sys.executable, "-c", source, "supervise"],
        env=environment,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
        close_fds=True,
    )
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        if state_path.exists():
            return _load_state(state_path)
        if Path(payload["exit_path"]).exists():
            break
        time.sleep(0.02)
    cleanup_errors: list[str] = []
    container_name = payload.get("container_name")
    if container_name:
        container = _inspect_container(container_name)
        if container is not None:
            labels = container.get("Config", {}).get("Labels") or {}
            if labels.get(CONTAINER_LABEL) != payload["run_id"]:
                cleanup_errors.append(
                    f"container {container_name} has an unexpected ownership label"
                )
            else:
                completed = subprocess.run(
                    ["docker", "container", "kill", container["Id"]],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                if (
                    completed.returncode != 0
                    and _inspect_container(container["Id"]) is not None
                ):
                    cleanup_errors.append(
                        f"cannot kill unpublished container {container['Id']}: "
                        f"{completed.stderr.strip()}"
                    )
                deadline = time.monotonic() + 1.0
                while (
                    time.monotonic() < deadline
                    and _inspect_container(container["Id"]) is not None
                ):
                    time.sleep(0.05)
                if _inspect_container(container["Id"]) is not None:
                    cleanup_errors.append(
                        f"unpublished container {container['Id']} did not exit"
                    )
    members = _group_members(supervisor.pid)
    identity_errors = [
        f"pid {member['pid']} in unpublished pgid {supervisor.pid} has the wrong run id"
        for member in members
        if _proc_run_id(member["pid"]) != payload["run_id"]
    ]
    cleanup_errors.extend(identity_errors)
    if members and not identity_errors:
        try:
            os.killpg(supervisor.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        deadline = time.monotonic() + 1.0
        while time.monotonic() < deadline and _group_members(supervisor.pid):
            time.sleep(0.05)
        if _group_members(supervisor.pid):
            try:
                os.killpg(supervisor.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            deadline = time.monotonic() + 1.0
            while time.monotonic() < deadline and _group_members(supervisor.pid):
                time.sleep(0.05)
        if _group_members(supervisor.pid):
            cleanup_errors.append("unpublished supervisor process group did not exit")
    try:
        supervisor.wait(timeout=0.1)
    except subprocess.TimeoutExpired:
        pass
    reason = (
        "role exited before publishing process state"
        if Path(payload["exit_path"]).exists()
        else "role did not publish process state"
    )
    if cleanup_errors:
        reason += "; cleanup: " + "; ".join(cleanup_errors)
    raise RuntimeError(f"{reason}: {payload['role_id']}")


def _status(payload: dict[str, Any]) -> dict[str, Any]:
    state = _load_state(Path(payload["state_path"]))
    exit_path = Path(state["exit_path"])
    members, process_errors = _owned_members(state)
    container, container_errors = _owned_container(state)
    errors = process_errors + container_errors
    result: dict[str, Any] = {
        "state": state,
        "members": members,
        "container": container,
        "identity_errors": errors,
    }
    if exit_path.exists():
        result["status"] = "completed"
        result["exit"] = _load_state(exit_path)
    elif members or container is not None:
        result["status"] = "running"
    else:
        result["status"] = "lost"
    return result


def _terminate(payload: dict[str, Any]) -> dict[str, Any]:
    state = _load_state(Path(payload["state_path"]))
    members, process_errors = _owned_members(state)
    container, container_errors = _owned_container(state)
    errors = process_errors + container_errors
    if errors:
        return {
            "ok": False,
            "errors": errors,
            "members": members,
            "container": container,
            "escalated_to_term": False,
            "escalated_to_kill": False,
        }
    if not members and container is None:
        return {
            "ok": True,
            "errors": [],
            "members": [],
            "container": None,
            "escalated_to_term": False,
            "escalated_to_kill": False,
        }

    initial_name = payload.get("initial_signal", "TERM")
    initial_signal = {"INT": signal.SIGINT, "TERM": signal.SIGTERM}.get(initial_name)
    if initial_signal is None:
        raise ValueError(f"unsupported initial signal: {initial_name}")
    try:
        os.killpg(int(state["pgid"]), initial_signal)
    except ProcessLookupError:
        pass
    timeout_seconds = float(payload["timeout_seconds"])
    members, container, errors = _wait_for_owned_exit(state, timeout_seconds)
    if errors:
        return {
            "ok": False,
            "errors": errors,
            "members": members,
            "container": container,
            "initial_signal": initial_name,
            "escalated_to_term": False,
            "escalated_to_kill": False,
        }
    escalated_to_term = bool(members or container is not None)
    container_stopped = False
    if escalated_to_term:
        container_stopped, container_signal_errors = _signal_container(
            state, "stop", timeout_seconds
        )
        errors.extend(container_signal_errors)
        try:
            os.killpg(int(state["pgid"]), signal.SIGTERM)
        except ProcessLookupError:
            pass
        members, container, wait_errors = _wait_for_owned_exit(state, timeout_seconds)
        errors.extend(wait_errors)
    escalated_to_kill = bool(members or container is not None)
    container_killed = False
    if escalated_to_kill:
        container_killed, container_signal_errors = _signal_container(
            state, "kill", timeout_seconds
        )
        errors.extend(container_signal_errors)
        try:
            os.killpg(int(state["pgid"]), signal.SIGKILL)
        except ProcessLookupError:
            pass
        members, container, wait_errors = _wait_for_owned_exit(state, timeout_seconds)
        errors.extend(wait_errors)
    remaining = _group_members(int(state["pgid"]))
    remaining_container, container_errors = _owned_container(state)
    errors.extend(container_errors)
    return {
        "ok": not remaining and remaining_container is None and not errors,
        "errors": errors,
        "members": remaining,
        "container": remaining_container,
        "initial_signal": initial_name,
        "escalated_to_term": escalated_to_term,
        "escalated_to_kill": escalated_to_kill,
        "container_stopped": container_stopped,
        "container_killed": container_killed,
    }


def _write(payload: dict[str, Any]) -> dict[str, Any]:
    path = Path(payload["path"])
    path.parent.mkdir(parents=True, exist_ok=True)
    content = base64.b64decode(payload["content_base64"])
    with path.open("xb") as stream:
        stream.write(content)
        stream.flush()
        os.fsync(stream.fileno())
    os.chmod(path, int(payload.get("mode", 0o444)))
    return {"path": str(path), "size": len(content)}


def _read(payload: dict[str, Any]) -> dict[str, Any]:
    content = Path(payload["path"]).read_bytes()
    return {"content_base64": base64.b64encode(content).decode(), "size": len(content)}


def _run(payload: dict[str, Any]) -> dict[str, Any]:
    completed = subprocess.run(
        payload["argv"],
        cwd=payload.get("cwd"),
        env={**os.environ, **payload.get("environment", {})},
        stdin=subprocess.DEVNULL,
        capture_output=True,
        text=True,
        timeout=float(payload.get("timeout_seconds", 30)),
        check=False,
    )
    return {
        "returncode": completed.returncode,
        "stdout": completed.stdout,
        "stderr": completed.stderr,
    }


def _parse_snmp() -> dict[str, dict[str, int]]:
    lines = Path("/proc/net/snmp").read_text(encoding="utf-8").splitlines()
    parsed: dict[str, dict[str, int]] = {}
    for header, values in zip(lines[0::2], lines[1::2], strict=False):
        header_parts = header.split()
        value_parts = values.split()
        if not header_parts or not value_parts or header_parts[0] != value_parts[0]:
            continue
        section = header_parts[0].rstrip(":")
        parsed[section] = {
            name: int(value)
            for name, value in zip(header_parts[1:], value_parts[1:], strict=False)
        }
    return parsed


def _parse_cpu_stats(selected: list[int]) -> dict[str, dict[str, int]]:
    wanted = {f"cpu{cpu}" for cpu in selected}
    result: dict[str, dict[str, int]] = {}
    for line in Path("/proc/stat").read_text(encoding="utf-8").splitlines():
        parts = line.split()
        if not parts or parts[0] not in wanted:
            continue
        values = [int(value) for value in parts[1:]]
        total = sum(values)
        idle = sum(values[3:5])
        result[parts[0]] = {"total": total, "busy": total - idle}
    return result


def _ethtool_stats(netdev: str) -> dict[str, int]:
    completed = subprocess.run(
        ["ethtool", "-S", netdev], capture_output=True, text=True, check=False
    )
    if completed.returncode != 0:
        raise RuntimeError(f"ethtool -S {netdev} failed: {completed.stderr.strip()}")
    values: dict[str, int] = {}
    for line in completed.stdout.splitlines()[1:]:
        if ":" not in line:
            continue
        key, raw_value = line.strip().split(":", 1)
        raw_value = raw_value.strip()
        if raw_value.isdigit():
            values[key] = int(raw_value)
    return values


def _snapshot(payload: dict[str, Any]) -> dict[str, Any]:
    return {
        "schema_version": "daqiri.counter-snapshot/v1",
        "utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "monotonic_ns": time.monotonic_ns(),
        "snmp": _parse_snmp(),
        "cpu": _parse_cpu_stats([int(cpu) for cpu in payload["cpus"]]),
        "netdevs": {netdev: _ethtool_stats(netdev) for netdev in payload["netdevs"]},
    }


def _ports_in_use(ports: list[int]) -> list[int]:
    wanted = {f"{port:04X}" for port in ports}
    found: set[int] = set()
    for table in ("/proc/net/udp", "/proc/net/udp6"):
        try:
            lines = Path(table).read_text(encoding="utf-8").splitlines()[1:]
        except FileNotFoundError:
            continue
        for line in lines:
            fields = line.split()
            if len(fields) > 1 and ":" in fields[1]:
                port_hex = fields[1].rsplit(":", 1)[1].upper()
                if port_hex in wanted:
                    found.add(int(port_hex, 16))
    return sorted(found)


def _binary_processes(binary: str) -> list[int]:
    matches: list[int] = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            argv = Path(entry, "cmdline").read_bytes().split(b"\0")
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue
        if any(arg.decode(errors="replace") == binary for arg in argv if arg):
            matches.append(int(entry.name))
    return sorted(matches)


def _container_available_cpus(docker: str, digest: str) -> list[int]:
    completed = subprocess.run(
        [
            docker,
            "run",
            "--rm",
            "--pull",
            "never",
            "--privileged",
            "--entrypoint",
            "python3",
            digest,
            "-c",
            "import json,os; print(json.dumps(sorted(os.sched_getaffinity(0))))",
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            "cannot inspect container CPU affinity: " + completed.stderr.strip()
        )
    try:
        cpus = json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError("container CPU affinity returned invalid JSON") from exc
    if not isinstance(cpus, list) or any(
        isinstance(cpu, bool) or not isinstance(cpu, int) or cpu < 0 for cpu in cpus
    ):
        raise RuntimeError("container CPU affinity returned invalid CPU indices")
    return sorted(set(cpus))


def _preflight(payload: dict[str, Any]) -> dict[str, Any]:
    errors: list[str] = []
    facts: dict[str, Any] = {}
    worktree = Path(payload["worktree"])
    binary = Path(payload["binary"])
    container = payload["container"]
    if not worktree.is_dir():
        errors.append(f"worktree is missing: {worktree}")
    if container["runtime"] == "docker":
        docker = shutil.which("docker")
        if docker is None:
            errors.append("docker is required but not available")
        else:
            image = subprocess.run(
                [docker, "image", "inspect", container["image"]],
                capture_output=True,
                text=True,
                check=False,
            )
            if image.returncode != 0:
                errors.append(
                    f"container image is missing: {container['image']}: {image.stderr.strip()}"
                )
            else:
                image_data = json.loads(image.stdout)
                image_id = image_data[0].get("Id") if image_data else None
                facts["container_image_id"] = image_id
                if image_id != container["digest"]:
                    errors.append(
                        f"container image mismatch: expected {container['digest']}, "
                        f"got {image_id or 'unavailable'}"
                    )
            binary_check = subprocess.run(
                [
                    docker,
                    "run",
                    "--rm",
                    "--pull",
                    "never",
                    "--entrypoint",
                    "sha256sum",
                    container["digest"],
                    str(binary),
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            if binary_check.returncode != 0:
                errors.append(
                    f"container benchmark binary is missing: {binary}: "
                    f"{binary_check.stderr.strip()}"
                )
            else:
                facts["binary_sha256"] = binary_check.stdout.split()[0]
            workdir_check = subprocess.run(
                [
                    docker,
                    "run",
                    "--rm",
                    "--pull",
                    "never",
                    "--entrypoint",
                    "test",
                    container["digest"],
                    "-d",
                    container["workdir"],
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            if workdir_check.returncode != 0:
                errors.append(
                    "container work directory is missing: "
                    f"{container['workdir']}: {workdir_check.stderr.strip()}"
                )
    elif not binary.is_file() or not os.access(binary, os.X_OK):
        errors.append(f"benchmark binary is missing or not executable: {binary}")
    else:
        facts["binary_size"] = binary.stat().st_size
        binary_hash = hashlib.sha256()
        with binary.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                binary_hash.update(chunk)
        facts["binary_sha256"] = binary_hash.hexdigest()
    if (
        "binary_sha256" in facts
        and facts["binary_sha256"] != payload["expected_binary_sha256"].lower()
    ):
        errors.append(
            "binary SHA-256 mismatch: expected "
            f"{payload['expected_binary_sha256']}, "
            f"got {facts['binary_sha256']}"
        )

    if worktree.is_dir():
        head = subprocess.run(
            ["git", "-C", str(worktree), "rev-parse", "HEAD"],
            capture_output=True,
            text=True,
            check=False,
        )
        facts["commit"] = head.stdout.strip()
        if head.returncode != 0 or facts["commit"] != payload["expected_commit"]:
            errors.append(
                f"commit mismatch: expected {payload['expected_commit']}, got {facts['commit'] or 'unavailable'}"
            )
        dirty = subprocess.run(
            ["git", "-C", str(worktree), "status", "--porcelain=v1"],
            capture_output=True,
            text=True,
            check=False,
        )
        facts["dirty"] = dirty.stdout.splitlines()
        if dirty.returncode != 0:
            errors.append("cannot inspect remote worktree dirty state")
        elif payload["require_clean"] and facts["dirty"]:
            errors.append("remote worktree is dirty")

    worker_available_cpus = sorted(os.sched_getaffinity(0))
    facts["worker_available_cpus"] = worker_available_cpus
    available_cpus = worker_available_cpus
    if container["runtime"] == "docker" and docker is not None:
        try:
            available_cpus = _container_available_cpus(docker, container["digest"])
        except RuntimeError as exc:
            available_cpus = []
            errors.append(str(exc))
    facts["available_cpus"] = available_cpus
    missing_cpus = sorted(cpu for cpu in payload["cpus"] if cpu not in available_cpus)
    if missing_cpus:
        errors.append(
            f"CPU assignments do not exist or are unavailable: {missing_cpus}"
        )

    for netdev in payload["netdevs"]:
        if not Path("/sys/class/net", netdev).exists():
            errors.append(f"network interface is missing: {netdev}")
    for endpoint in payload["endpoints"]:
        netdev = endpoint["netdev"]
        base = Path("/sys/class/net", netdev)
        if not base.exists():
            continue
        actual_mac = (base / "address").read_text(encoding="utf-8").strip().lower()
        actual_mtu = int((base / "mtu").read_text(encoding="utf-8").strip())
        actual_state = (base / "operstate").read_text(encoding="utf-8").strip()
        try:
            actual_speed = int((base / "speed").read_text(encoding="utf-8").strip())
        except (FileNotFoundError, ValueError, OSError):
            actual_speed = -1
        try:
            actual_pci = (base / "device").resolve().name
        except OSError:
            actual_pci = ""
        facts.setdefault("endpoints", {})[
            f"{endpoint['link_id']}.{endpoint['direction']}"
        ] = {
            "netdev": netdev,
            "mac": actual_mac,
            "mtu": actual_mtu,
            "operstate": actual_state,
            "speed_mbps": actual_speed,
            "pci": actual_pci,
        }
        if actual_mac != endpoint["mac"].lower():
            errors.append(
                f"{netdev} MAC mismatch: expected {endpoint['mac']}, got {actual_mac}"
            )
        if actual_mtu != endpoint["mtu"]:
            errors.append(
                f"{netdev} MTU mismatch: expected {endpoint['mtu']}, got {actual_mtu}"
            )
        if actual_state != "up":
            errors.append(f"{netdev} link state is {actual_state}, expected up")
        if actual_speed != endpoint["speed_mbps"]:
            errors.append(
                f"{netdev} speed mismatch: expected {endpoint['speed_mbps']}, got {actual_speed}"
            )
        if actual_pci != endpoint["pci"]:
            errors.append(
                f"{netdev} PCI mismatch: expected {endpoint['pci']}, got {actual_pci}"
            )
    addresses = subprocess.run(
        ["ip", "-j", "address", "show"], capture_output=True, text=True, check=False
    )
    if addresses.returncode != 0:
        errors.append(f"cannot inspect network addresses: {addresses.stderr.strip()}")
    else:
        assigned = {
            info.get("local")
            for interface in json.loads(addresses.stdout)
            for info in interface.get("addr_info", [])
        }
        facts["assigned_addresses"] = sorted(value for value in assigned if value)
        for address in payload["addresses"]:
            if address not in assigned:
                errors.append(f"required address is not assigned: {address}")
    route_facts: list[dict[str, Any]] = []
    for route in payload["routes"]:
        check = subprocess.run(
            [
                "ip",
                "-j",
                "route",
                "get",
                route["peer_ip"],
                "from",
                route["local_ip"],
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        if check.returncode != 0:
            errors.append(
                f"route {route['local_ip']} -> {route['peer_ip']} failed: {check.stderr.strip()}"
            )
            continue
        resolved = json.loads(check.stdout)
        route_facts.extend(resolved)
        if not resolved or resolved[0].get("dev") != route["netdev"]:
            errors.append(
                f"route {route['local_ip']} -> {route['peer_ip']} does not use {route['netdev']}"
            )
    facts["routes"] = route_facts

    occupied = _ports_in_use([int(port) for port in payload["ports"]])
    facts["occupied_udp_ports"] = occupied
    if occupied:
        errors.append(f"UDP ports already in use: {occupied}")
    contaminating = _binary_processes(str(binary))
    facts["benchmark_processes"] = contaminating
    if contaminating:
        errors.append(f"unrelated benchmark processes are running: {contaminating}")

    if payload["require_mlnx_perf"]:
        executable = shutil.which("mlnx_perf")
        if executable is None:
            errors.append("mlnx_perf is required but not available")
        else:
            facts["mlnx_perf"] = executable
    if payload["requirements"]["gpu"]:
        check = subprocess.run(
            ["nvidia-smi", "-L"], capture_output=True, text=True, check=False
        )
        if check.returncode != 0:
            errors.append("GPU requirement is not satisfied")
    if payload["requirements"]["hugepages"]:
        meminfo = Path("/proc/meminfo").read_text(encoding="utf-8")
        total_line = next(
            (
                line
                for line in meminfo.splitlines()
                if line.startswith("HugePages_Total:")
            ),
            "",
        )
        if not total_line or int(total_line.split()[1]) <= 0:
            errors.append("hugepage requirement is not satisfied")
    return {"ok": not errors, "errors": errors, "facts": facts}


def main() -> int:
    if len(sys.argv) != 2:
        _reply({"ok": False, "error": "worker mode is required"})
        return 2
    mode = sys.argv[1]
    try:
        if mode == "supervise":
            encoded = os.environ.pop("DAQIRI_REMOTE_SUPERVISOR_PAYLOAD")
            payload = json.loads(base64.b64decode(encoded))
            return _supervise(payload)
        payload = _read_payload()
        handlers = {
            "launch": _launch,
            "status": _status,
            "terminate": _terminate,
            "write": _write,
            "read": _read,
            "run": _run,
            "snapshot": _snapshot,
            "preflight": _preflight,
            "ports": lambda value: {
                "ports_in_use": _ports_in_use([int(port) for port in value["ports"]])
            },
        }
        if mode not in handlers:
            raise ValueError(f"unsupported worker mode: {mode}")
        _reply({"ok": True, "result": handlers[mode](payload)})
        return 0
    except BaseException as exc:  # noqa: BLE001 - always return a JSON RPC error
        _reply({"ok": False, "error": f"{type(exc).__name__}: {exc}"})
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
