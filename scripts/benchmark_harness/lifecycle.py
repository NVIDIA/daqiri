# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"""Explicit benchmark repetition lifecycle and fail-safe cleanup."""

from __future__ import annotations

import datetime as dt
import json
import os
import platform
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

from .executor import ExecutorError, HostExecutor
from .results import (
    RESULT_VERSION,
    append_jsonl,
    load_results,
    render_summary,
    result_key,
    write_raw,
)
from .udp import ResultParseError, build_udp_evidence, verdict_from_evidence


class HarnessFailure(RuntimeError):
    """A preflight, process, collector, or cleanup operation failed."""


def _utc() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


class Harness:
    def __init__(
        self,
        plan: dict[str, Any],
        run_dir: Path,
        executors: dict[str, HostExecutor],
        repository: Path,
    ):
        self.plan = plan
        self.run_dir = run_dir
        self.executors = executors
        self.repository = repository
        self.owned: list[tuple[str, str, str]] = []

    def _event(self, repetition_dir: Path, phase: str, **details: Any) -> None:
        append_jsonl(
            repetition_dir / "lifecycle.jsonl",
            {"utc": _utc(), "phase": phase, **details},
        )

    def _host_resources(self) -> dict[str, dict[str, Any]]:
        resources: dict[str, dict[str, Any]] = {
            host_id: {
                "netdevs": set(),
                "addresses": set(),
                "ports": set(),
                "cpus": set(),
                "endpoints": [],
                "routes": [],
            }
            for host_id in self.plan["site"]["hosts"]
        }
        max_pairs = max(cell["pair_count"] for cell in self.plan["cells"])
        for pair in self.plan["site"]["pairs"][:max_pairs]:
            link = self.plan["site"]["links"][pair["link"]]
            for direction in ("tx", "rx"):
                endpoint = link[direction]
                placement = pair[direction]
                target = resources[endpoint["host"]]
                target["netdevs"].add(endpoint["netdev"])
                target["addresses"].add(endpoint["ip"])
                target["ports"].add(placement["local_port"])
                target["cpus"].update(
                    [
                        placement["master_cpu"],
                        placement["io_cpu"],
                        placement["worker_cpu"],
                    ]
                )
                target["cpus"].update(endpoint["irq_cpus"])
                target["cpus"].update(endpoint["napi_cpus"])
                target["endpoints"].append(
                    {**endpoint, "link_id": pair["link"], "direction": direction}
                )
                peer_direction = "rx" if direction == "tx" else "tx"
                target["routes"].append(
                    {
                        "local_ip": endpoint["ip"],
                        "peer_ip": link[peer_direction]["ip"],
                        "netdev": endpoint["netdev"],
                    }
                )
        normalized: dict[str, dict[str, Any]] = {}
        for host_id, values in resources.items():
            normalized[host_id] = {
                "netdevs": sorted(values["netdevs"]),
                "addresses": sorted(values["addresses"]),
                "ports": sorted(values["ports"]),
                "cpus": sorted(values["cpus"]),
                "endpoints": sorted(
                    values["endpoints"],
                    key=lambda value: (value["link_id"], value["direction"]),
                ),
                "routes": sorted(
                    values["routes"],
                    key=lambda value: (value["netdev"], value["peer_ip"]),
                ),
            }
        return normalized

    def preflight(self, artifact_name: str = "preflight.json") -> dict[str, Any]:
        """Fail before load if identity, topology, placement, or contamination is wrong."""

        resources = self._host_resources()
        report: dict[str, Any] = {"schema_version": "daqiri.preflight/v1", "hosts": {}}
        errors: list[str] = []
        for host_id, host in self.plan["site"]["hosts"].items():
            result = self.executors[host_id].preflight(
                {
                    "worktree": host["worktree"],
                    "binary": (
                        host["benchmark_binary"]
                        if host["benchmark_binary"].startswith("/")
                        else str(Path(host["worktree"]) / host["benchmark_binary"])
                    ),
                    "expected_commit": host["expected_commit"],
                    "expected_binary_sha256": host["expected_binary_sha256"],
                    "container": host["container"],
                    "require_clean": host["require_clean"],
                    "cpus": resources[host_id]["cpus"],
                    "netdevs": resources[host_id]["netdevs"],
                    "addresses": resources[host_id]["addresses"],
                    "ports": resources[host_id]["ports"],
                    "endpoints": resources[host_id]["endpoints"],
                    "routes": resources[host_id]["routes"],
                    "require_mlnx_perf": "mlnx_perf"
                    in self.plan["suite"]["collectors"],
                    "requirements": self.plan["suite"]["requirements"],
                }
            )
            report["hosts"][host_id] = result
            errors.extend(f"{host_id}: {error}" for error in result["errors"])

        for check in self.plan["site"]["contamination_checks"]:
            result = self.executors[check["host"]].run(check["argv"])
            report.setdefault("contamination_checks", {})[check["name"]] = result
            if result["returncode"] != 0:
                errors.append(
                    f"contamination check {check['name']} failed with rc={result['returncode']}"
                )
            elif check["expect_stdout_empty"] and result["stdout"].strip():
                errors.append(
                    f"contamination check {check['name']} found conflicting state"
                )
        report["ok"] = not errors
        report["errors"] = errors
        write_raw(
            self.run_dir / "provenance" / artifact_name,
            json.dumps(report, indent=2, sort_keys=True) + "\n",
        )
        if errors:
            raise HarnessFailure("preflight failed: " + "; ".join(errors))
        return resources

    def capture_provenance(
        self, resources: dict[str, dict[str, Any]], suffix: str = ""
    ) -> None:
        controller_commands = [
            ["git", "-C", str(self.repository), "rev-parse", "HEAD"],
            ["git", "-C", str(self.repository), "status", "--porcelain=v1"],
        ]
        controller_results = []
        for argv in controller_commands:
            completed = subprocess.run(
                argv, capture_output=True, text=True, check=False
            )
            controller_results.append(
                {
                    "argv": argv,
                    "returncode": completed.returncode,
                    "stdout": completed.stdout,
                    "stderr": completed.stderr,
                }
            )
            if completed.returncode != 0:
                raise HarnessFailure(f"controller provenance command failed: {argv}")
        controller = {
            "schema_version": "daqiri.provenance/v1",
            "captured_utc": _utc(),
            "run_id": self.plan["run_id"],
            "plan_sha256": self.plan["plan_sha256"],
            "argv": sys.argv,
            "cwd": os.getcwd(),
            "python": sys.version,
            "platform": platform.platform(),
            "commands": controller_results,
        }
        write_raw(
            self.run_dir / "provenance" / f"controller{suffix}.json",
            json.dumps(controller, indent=2, sort_keys=True) + "\n",
        )

        for host_id, host in self.plan["site"]["hosts"].items():
            binary = (
                host["benchmark_binary"]
                if host["benchmark_binary"].startswith("/")
                else str(Path(host["worktree"]) / host["benchmark_binary"])
            )
            commands = [
                ["uname", "-a"],
                ["git", "-C", host["worktree"], "rev-parse", "HEAD"],
                ["git", "-C", host["worktree"], "status", "--porcelain=v1"],
                ["ip", "-j", "address", "show"],
                ["ip", "-j", "route", "show"],
                ["cat", "/proc/interrupts"],
                ["cat", "/proc/softirqs"],
            ]
            container = host["container"]
            if container["runtime"] == "docker":
                commands.extend(
                    [
                        ["docker", "image", "inspect", container["image"]],
                        [
                            "docker",
                            "run",
                            "--rm",
                            "--pull",
                            "never",
                            "--entrypoint",
                            "sha256sum",
                            container["digest"],
                            binary,
                        ],
                        [
                            "docker",
                            "run",
                            "--rm",
                            "--pull",
                            "never",
                            "--entrypoint",
                            "ldd",
                            container["digest"],
                            binary,
                        ],
                    ]
                )
            else:
                commands.extend([["sha256sum", binary], ["ldd", binary]])
            for netdev in resources[host_id]["netdevs"]:
                commands.append(["ethtool", "-i", netdev])
                commands.append(["ethtool", netdev])
            host_results = []
            for argv in commands:
                result = self.executors[host_id].run(argv, cwd=host["worktree"])
                host_results.append({"argv": argv, **result})
                if result["returncode"] != 0:
                    raise HarnessFailure(f"{host_id} provenance command failed: {argv}")
            gpu = self.executors[host_id].run(
                [
                    "nvidia-smi",
                    "--query-gpu=name,driver_version",
                    "--format=csv,noheader",
                ]
            )
            host_results.append(
                {
                    "argv": [
                        "nvidia-smi",
                        "--query-gpu=name,driver_version",
                        "--format=csv,noheader",
                    ],
                    **gpu,
                    "required": self.plan["suite"]["requirements"]["gpu"],
                }
            )
            if self.plan["suite"]["requirements"]["gpu"] and gpu["returncode"] != 0:
                raise HarnessFailure(f"{host_id} required GPU provenance failed")
            provenance = {
                "schema_version": "daqiri.provenance/v1",
                "captured_utc": _utc(),
                "run_id": self.plan["run_id"],
                "host_id": host_id,
                "executor": {
                    "transport": host["transport"],
                    "destination": host.get("destination"),
                    "worker_prefix": host["worker_prefix"],
                },
                "container": host["container"],
                "declared_placement": resources[host_id],
                "commands": host_results,
            }
            write_raw(
                self.run_dir / "provenance" / f"{host_id}{suffix}.json",
                json.dumps(provenance, indent=2, sort_keys=True) + "\n",
            )

    def snapshot_original(
        self, resources: dict[str, dict[str, Any]], suffix: str = ""
    ) -> None:
        for host_id, selected in resources.items():
            snapshot = self.executors[host_id].snapshot(
                selected["netdevs"], selected["cpus"]
            )
            write_raw(
                self.run_dir / "provenance" / f"{host_id}-original-state{suffix}.json",
                json.dumps(snapshot, indent=2, sort_keys=True) + "\n",
            )

    def prepare(self, resources: dict[str, dict[str, Any]]) -> None:
        """Prepare only immutable runtime files; v1 never owns or mutates topology."""

        state = {
            "topology_ownership": self.plan["site"]["topology_ownership"],
            "network_mutated": False,
            "runtime_files_owned": True,
            "prepared_utc": _utc(),
            "resources": resources,
        }
        write_raw(
            self.run_dir / "provenance" / "prepare.json",
            json.dumps(state, indent=2, sort_keys=True) + "\n",
        )

    def initialize(self) -> None:
        resources = self.preflight()
        self.capture_provenance(resources)
        self.snapshot_original(resources)
        self.prepare(resources)

    def initialize_resume(self) -> None:
        index = 1
        while (self.run_dir / "provenance" / f"preflight-resume-{index}.json").exists():
            index += 1
        suffix = f"-resume-{index}"
        resources = self.preflight(f"preflight{suffix}.json")
        self.capture_provenance(resources, suffix)
        self.snapshot_original(resources, suffix)

    def record_initialization_failure(self, reason: str) -> None:
        completed = {
            result_key(result)
            for result in load_results(self.run_dir / "results.jsonl")
        }
        for cell in self.plan["cells"]:
            for repetition_plan in cell["repetitions"]:
                key = (cell["cell_id"], repetition_plan["repetition"])
                if key in completed:
                    continue
                result = self._result_base(cell, repetition_plan["repetition"])
                result.update(
                    {
                        "started_utc": _utc(),
                        "ended_utc": _utc(),
                        "state": "failed",
                        "reasons": [reason],
                        "evidence": {},
                        "role_exits": {},
                        "artifacts": {"repetition_dir": ""},
                    }
                )
                append_jsonl(self.run_dir / "results.jsonl", result)
        render_summary(self.run_dir, self.plan)

    def _phase_snapshots(
        self,
        phase: str,
        repetition_dir: Path,
        cell: dict[str, Any],
    ) -> dict[str, dict[str, Any]]:
        resources = self._resources_for_cell(cell)
        snapshots: dict[str, dict[str, Any]] = {}
        for host_id, selected in resources.items():
            snapshots[host_id] = self.executors[host_id].snapshot(
                selected["netdevs"], selected["cpus"]
            )
            write_raw(
                repetition_dir / "snapshots" / f"{phase}-{host_id}.json",
                json.dumps(snapshots[host_id], indent=2, sort_keys=True) + "\n",
            )
        self._event(repetition_dir, phase)
        return snapshots

    def _resources_for_cell(self, cell: dict[str, Any]) -> dict[str, dict[str, Any]]:
        resources: dict[str, dict[str, set[Any]]] = {
            host_id: {"netdevs": set(), "cpus": set()}
            for host_id in self.plan["site"]["hosts"]
        }
        for pair in self.plan["site"]["pairs"][: cell["pair_count"]]:
            link = self.plan["site"]["links"][pair["link"]]
            for direction in ("tx", "rx"):
                endpoint = link[direction]
                placement = pair[direction]
                target = resources[endpoint["host"]]
                target["netdevs"].add(endpoint["netdev"])
                target["cpus"].update(
                    [
                        placement["master_cpu"],
                        placement["io_cpu"],
                        placement["worker_cpu"],
                    ]
                )
        return {
            host_id: {
                "netdevs": sorted(value["netdevs"]),
                "cpus": sorted(value["cpus"]),
            }
            for host_id, value in resources.items()
            if value["netdevs"]
        }

    def _launch(self, role: dict[str, Any], repetition_dir: Path) -> dict[str, Any]:
        launched = {**role, "run_id": role["process_run_id"]}
        executor = self.executors[role["host_id"]]
        state = executor.launch(launched)
        self.owned.append((role["host_id"], role["role_id"], role["state_path"]))
        write_raw(
            repetition_dir / "processes" / f"{role['role_id']}.json",
            json.dumps(state, indent=2, sort_keys=True) + "\n",
        )
        return state

    def _wait_transmitters(self, transmitters: list[dict[str, Any]]) -> dict[str, Any]:
        deadline = time.monotonic() + self.plan["suite"]["duration_seconds"] + 15
        pending = {role["role_id"]: role for role in transmitters}
        exits: dict[str, Any] = {}
        while pending:
            for role_id, transmitter in list(pending.items()):
                status = self.executors[transmitter["host_id"]].status(
                    transmitter["state_path"]
                )
                if status["identity_errors"]:
                    raise HarnessFailure(
                        f"transmitter {role_id} identity failed: {status['identity_errors']}"
                    )
                if status["status"] == "running":
                    continue
                exits[role_id] = status.get("exit")
                if status["status"] != "completed" or not status.get("exit"):
                    raise HarnessFailure(
                        f"transmitter {role_id} ended as {status['status']}"
                    )
                if status["exit"]["exit_code"] != 0:
                    raise HarnessFailure(
                        f"transmitter {role_id} exited {status['exit']['exit_code']}"
                    )
                del pending[role_id]
            if pending:
                if time.monotonic() >= deadline:
                    raise HarnessFailure(
                        "transmitter completion timed out: "
                        + ", ".join(sorted(pending))
                    )
                time.sleep(0.1)
        return exits

    def _copy_logs(
        self, roles: list[dict[str, Any]], repetition_dir: Path, subdir: str
    ) -> dict[str, dict[str, str]]:
        logs: dict[str, dict[str, str]] = {}
        for role in roles:
            executor = self.executors[role["host_id"]]
            stdout = executor.read_bytes(role["stdout_path"])
            stderr = executor.read_bytes(role["stderr_path"])
            write_raw(repetition_dir / subdir / f"{role['role_id']}.stdout.log", stdout)
            write_raw(repetition_dir / subdir / f"{role['role_id']}.stderr.log", stderr)
            logs[role["role_id"]] = {
                "stdout": stdout.decode(errors="replace"),
                "stderr": stderr.decode(errors="replace"),
            }
        return logs

    def _cleanup(
        self, timeout_seconds: float, *, normal_receivers: set[str]
    ) -> list[str]:
        errors: list[str] = []
        for host_id, role_id, state_path in reversed(self.owned):
            try:
                result = self.executors[host_id].terminate(
                    state_path,
                    timeout_seconds,
                    initial_signal="INT" if role_id in normal_receivers else "TERM",
                )
                if not result["ok"]:
                    errors.append(f"{host_id}/{role_id} cleanup failed: {result}")
            except ExecutorError as exc:
                errors.append(f"{host_id}/{role_id} cleanup failed: {exc}")
        for host_id, role_id, state_path in self.owned:
            try:
                status = self.executors[host_id].status(state_path)
            except ExecutorError as exc:
                errors.append(f"{host_id}/{role_id} cleanup verification failed: {exc}")
                continue
            if (
                status["members"]
                or status.get("container") is not None
                or status["identity_errors"]
            ):
                errors.append(
                    f"{host_id}/{role_id} still owned members={status['members']} "
                    f"container={status.get('container')} "
                    f"identity_errors={status['identity_errors']}"
                )
        self.owned.clear()
        return errors

    def _result_base(self, cell: dict[str, Any], repetition: int) -> dict[str, Any]:
        return {
            "schema_version": RESULT_VERSION,
            "run_id": self.plan["run_id"],
            "plan_sha256": self.plan["plan_sha256"],
            "cell_id": cell["cell_id"],
            "repetition": repetition,
            "topology": self.plan["suite"]["topology"],
            "evidence_class": self.plan["suite"]["evidence_class"],
            "adapter": self.plan["suite"]["adapter"],
            "parameters": {
                key: cell[key]
                for key in (
                    "message_size_bytes",
                    "pair_count",
                    "requested_rate_gbps",
                    "rate_scope",
                    "batch_size",
                    "buffer_size_bytes",
                    "buffers_per_region",
                )
            },
        }

    def run_repetition(
        self, cell: dict[str, Any], repetition_plan: dict[str, Any]
    ) -> dict[str, Any]:
        repetition = repetition_plan["repetition"]
        repetition_dir = (
            self.run_dir / "cells" / cell["cell_id"] / f"repetition-{repetition}"
        )
        repetition_dir.mkdir(parents=True, exist_ok=False)
        result = self._result_base(cell, repetition)
        result["started_utc"] = _utc()
        roles = repetition_plan["roles"]
        receivers = [role for role in roles if role["direction"] == "rx"]
        transmitters = [role for role in roles if role["direction"] == "tx"]
        collectors = repetition_plan["collectors"]
        snapshots: dict[str, dict[str, dict[str, Any]]] = {}
        role_exits: dict[str, Any] = {}
        normal_receivers: set[str] = set()
        primary_error: str | None = None
        interrupted = False
        try:
            self._event(repetition_dir, "preflight_complete")
            for role in roles:
                self.executors[role["host_id"]].write_text(
                    role["config_path"], role["config_yaml"]
                )
            self._event(repetition_dir, "prepare_complete", network_mutated=False)
            snapshots["startup_begin"] = self._phase_snapshots(
                "startup_begin", repetition_dir, cell
            )

            for receiver in receivers:
                self._launch(receiver, repetition_dir)
            self._event(repetition_dir, "receivers_launched")
            receivers_by_host: dict[str, list[dict[str, Any]]] = {}
            for receiver in receivers:
                receivers_by_host.setdefault(receiver["host_id"], []).append(receiver)
            for host_id, host_receivers in receivers_by_host.items():
                ports = [
                    next(
                        pair["rx"]["local_port"]
                        for pair in self.plan["site"]["pairs"]
                        if pair["id"] == receiver["pair_id"]
                    )
                    for receiver in host_receivers
                ]
                self.executors[host_id].wait_udp_ports(
                    ports,
                    [receiver["state_path"] for receiver in host_receivers],
                    self.plan["suite"]["readiness_timeout_seconds"],
                )
            self._event(repetition_dir, "all_roles_ready")

            for collector in collectors:
                self._launch(collector, repetition_dir)
            for collector in collectors:
                self.executors[collector["host_id"]].wait_log_contains(
                    collector["stdout_path"],
                    "Sampling started.",
                    collector["state_path"],
                    self.plan["suite"]["readiness_timeout_seconds"],
                )
            snapshots["active_start"] = self._phase_snapshots(
                "active_start", repetition_dir, cell
            )
            active_start = time.monotonic()

            for transmitter in transmitters:
                self._launch(transmitter, repetition_dir)
            self._event(repetition_dir, "transmitters_launched")
            role_exits.update(self._wait_transmitters(transmitters))
            active_end = time.monotonic()
            snapshots["active_end"] = self._phase_snapshots(
                "active_end", repetition_dir, cell
            )
            active_duration = active_end - active_start

            for collector in collectors:
                cleanup = self.executors[collector["host_id"]].terminate(
                    collector["state_path"],
                    self.plan["suite"]["cleanup_timeout_seconds"],
                )
                if not cleanup["ok"]:
                    raise HarnessFailure(
                        f"collector {collector['role_id']} cleanup failed"
                    )
            self._event(repetition_dir, "collectors_stopped")

            time.sleep(self.plan["suite"]["drain_seconds"])
            snapshots["drain_end"] = self._phase_snapshots(
                "drain_end", repetition_dir, cell
            )
            normal_receivers = {receiver["role_id"] for receiver in receivers}
            for receiver in receivers:
                cleanup = self.executors[receiver["host_id"]].terminate(
                    receiver["state_path"],
                    self.plan["suite"]["cleanup_timeout_seconds"],
                    initial_signal="INT",
                )
                if not cleanup["ok"]:
                    raise HarnessFailure(
                        f"receiver {receiver['role_id']} cleanup failed"
                    )
                status = self.executors[receiver["host_id"]].status(
                    receiver["state_path"]
                )
                role_exits[receiver["role_id"]] = status.get("exit")
                if status["status"] != "completed" or not status.get("exit"):
                    raise HarnessFailure(
                        f"receiver {receiver['role_id']} did not exit cleanly"
                    )
                if status["exit"]["exit_code"] != 0:
                    raise HarnessFailure(
                        f"receiver {receiver['role_id']} exited {status['exit']['exit_code']}"
                    )
            snapshots["shutdown_tail"] = self._phase_snapshots(
                "shutdown_tail", repetition_dir, cell
            )

            role_logs = self._copy_logs(roles, repetition_dir, "roles")
            collector_copies = self._copy_logs(collectors, repetition_dir, "collectors")
            collector_logs = {
                role_id: content["stdout"]
                for role_id, content in collector_copies.items()
            }
            try:
                evidence = build_udp_evidence(
                    cell=cell,
                    site=self.plan["site"],
                    roles=roles,
                    role_logs=role_logs,
                    snapshots=snapshots,
                    collector_logs=collector_logs,
                    active_duration_seconds=active_duration,
                    requested_duration_seconds=self.plan["suite"]["duration_seconds"],
                )
                state, reasons = verdict_from_evidence(
                    evidence, self.plan["suite"]["acceptance"]
                )
                result["evidence"] = evidence
                result["state"] = state
                result["reasons"] = reasons
            except ResultParseError as exc:
                result["evidence"] = {}
                result["state"] = "invalid"
                result["reasons"] = [f"result parser rejected role output: {exc}"]
            self._event(repetition_dir, "validation_complete", state=result["state"])
        except KeyboardInterrupt:
            interrupted = True
            primary_error = "benchmark interrupted"
        except Exception as exc:  # noqa: BLE001 - preserve a terminal result for code faults
            primary_error = f"{type(exc).__name__}: {exc}"
        finally:
            cleanup_errors = self._cleanup(
                self.plan["suite"]["cleanup_timeout_seconds"],
                normal_receivers=normal_receivers,
            )
            failure_reasons = (
                [primary_error] if primary_error else []
            ) + cleanup_errors
            if failure_reasons:
                result["state"] = "failed"
                result["reasons"] = failure_reasons
                result.setdefault("evidence", {})
            result["role_exits"] = role_exits
            result["ended_utc"] = _utc()
            result["artifacts"] = {
                "repetition_dir": str(repetition_dir.relative_to(self.run_dir))
            }
            append_jsonl(self.run_dir / "results.jsonl", result)
            self._event(repetition_dir, "rollback_complete", network_mutated=False)
            self._event(
                repetition_dir,
                "cleanup_verified",
                success=not cleanup_errors,
                errors=cleanup_errors,
            )
        if interrupted:
            raise KeyboardInterrupt
        return result

    def recover_orphans(self) -> None:
        completed = {
            result_key(result)
            for result in load_results(self.run_dir / "results.jsonl")
        }
        for cell in self.plan["cells"]:
            for repetition_plan in cell["repetitions"]:
                key = (cell["cell_id"], repetition_plan["repetition"])
                repetition_dir = (
                    self.run_dir
                    / "cells"
                    / cell["cell_id"]
                    / f"repetition-{repetition_plan['repetition']}"
                )
                if key in completed or not repetition_dir.exists():
                    continue
                cleanup_errors: list[str] = []
                for role in [*repetition_plan["roles"], *repetition_plan["collectors"]]:
                    try:
                        result = self.executors[role["host_id"]].terminate(
                            role["state_path"],
                            self.plan["suite"]["cleanup_timeout_seconds"],
                        )
                        if not result["ok"]:
                            cleanup_errors.append(f"{role['role_id']}: {result}")
                    except ExecutorError:
                        # The process may never have launched; absence is recorded below.
                        continue
                result = self._result_base(cell, repetition_plan["repetition"])
                result.update(
                    {
                        "started_utc": _utc(),
                        "ended_utc": _utc(),
                        "state": "failed",
                        "reasons": [
                            "resume found an orphaned started repetition; it was not rerun",
                            *cleanup_errors,
                        ],
                        "evidence": {},
                        "role_exits": {},
                        "artifacts": {
                            "repetition_dir": str(
                                repetition_dir.relative_to(self.run_dir)
                            )
                        },
                    }
                )
                append_jsonl(self.run_dir / "results.jsonl", result)

    def run_all(self) -> int:
        existing = {
            result_key(result)
            for result in load_results(self.run_dir / "results.jsonl")
        }
        any_invalid = False
        any_failed = False
        for cell in self.plan["cells"]:
            for repetition_plan in cell["repetitions"]:
                key = (cell["cell_id"], repetition_plan["repetition"])
                if key in existing:
                    continue
                result = self.run_repetition(cell, repetition_plan)
                any_invalid |= result["state"] == "invalid"
                any_failed |= result["state"] == "failed"
                if any_failed:
                    render_summary(self.run_dir, self.plan)
                    return 1
        render_summary(self.run_dir, self.plan)
        all_results = load_results(self.run_dir / "results.jsonl")
        any_failed |= any(result["state"] == "failed" for result in all_results)
        any_invalid |= any(result["state"] == "invalid" for result in all_results)
        return 1 if any_failed else 2 if any_invalid else 0
