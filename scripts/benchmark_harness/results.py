# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"""Immutable raw artifacts and deterministic derived result views."""

from __future__ import annotations

import csv
import io
import json
import os
from pathlib import Path
from typing import Any

import yaml

from .manifest import canonical_json

RESULT_VERSION = "daqiri.benchmark-result/v1"


def write_raw(path: Path, content: str | bytes, mode: int = 0o444) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    binary = content if isinstance(content, bytes) else content.encode()
    with path.open("xb") as stream:
        stream.write(binary)
        stream.flush()
        os.fsync(stream.fileno())
    path.chmod(mode)


def initialize_run(run_dir: Path, plan: dict[str, Any]) -> None:
    run_dir.mkdir(parents=True, exist_ok=False)
    resolved = yaml.safe_dump(plan, sort_keys=True, default_flow_style=False)
    write_raw(run_dir / "resolved-manifest.yaml", resolved)
    write_raw(run_dir / "plan.json", json.dumps(plan, indent=2, sort_keys=True) + "\n")
    write_raw(run_dir / "plan.sha256", plan["plan_sha256"] + "\n")
    (run_dir / "provenance").mkdir()
    (run_dir / "cells").mkdir()


def verify_run_identity(run_dir: Path, plan: dict[str, Any]) -> None:
    try:
        recorded = (run_dir / "plan.sha256").read_text(encoding="utf-8").strip()
    except OSError as exc:
        raise ValueError(f"cannot read immutable run identity: {exc}") from exc
    if recorded != plan["plan_sha256"]:
        raise ValueError("run directory plan hash does not match resolved manifest")


def append_jsonl(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    line = canonical_json(value)
    with path.open("a", encoding="utf-8") as stream:
        stream.write(line)
        stream.flush()
        os.fsync(stream.fileno())


def load_results(path: Path) -> list[dict[str, Any]]:
    if not path.exists():
        return []
    results: list[dict[str, Any]] = []
    with path.open(encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            value = json.loads(line)
            if value.get("schema_version") != RESULT_VERSION:
                raise ValueError(f"invalid result schema on line {line_number}")
            results.append(value)
    return results


def _atomic_derived(path: Path, content: str) -> None:
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    temporary.write_text(content, encoding="utf-8")
    os.replace(temporary, path)


def render_summary(run_dir: Path, plan: dict[str, Any]) -> None:
    results = load_results(run_dir / "results.jsonl")
    output = io.StringIO()
    fields = [
        "cell_id",
        "repetition",
        "state",
        "message_size_bytes",
        "pair_count",
        "requested_rate_gbps",
        "rate_scope",
        "buffers_per_region",
        "requested_aggregate_gbps",
        "achieved_mlnx_rx_gbps",
        "application_rx_gbps",
        "delivery_loss_packets",
        "kernel_udp_errors",
        "nic_rx_discards",
        "active_duration_seconds",
        "reasons",
    ]
    writer = csv.DictWriter(output, fieldnames=fields, lineterminator="\n")
    writer.writeheader()
    for result in sorted(
        results, key=lambda item: (item["cell_id"], item["repetition"])
    ):
        evidence = result.get("evidence", {})
        kernel = evidence.get("kernel_udp", {})
        nic = evidence.get("nic_rx_discards", {})
        writer.writerow(
            {
                "cell_id": result["cell_id"],
                "repetition": result["repetition"],
                "state": result["state"],
                "message_size_bytes": result["parameters"]["message_size_bytes"],
                "pair_count": result["parameters"]["pair_count"],
                "requested_rate_gbps": result["parameters"]["requested_rate_gbps"],
                "rate_scope": result["parameters"]["rate_scope"],
                "buffers_per_region": result["parameters"]["buffers_per_region"],
                "requested_aggregate_gbps": evidence.get("requested_rate", {}).get(
                    "aggregate_gbps", ""
                ),
                "achieved_mlnx_rx_gbps": evidence.get("achieved_rate", {}).get(
                    "mlnx_rx_aggregate_gbps", ""
                ),
                "application_rx_gbps": evidence.get("application", {}).get(
                    "rx_payload_gbps", ""
                ),
                "delivery_loss_packets": evidence.get("application", {}).get(
                    "delivery_loss_packets", ""
                ),
                "kernel_udp_errors": (
                    sum(value for value in kernel.values() if isinstance(value, int))
                    if kernel
                    else ""
                ),
                "nic_rx_discards": (
                    sum(value for value in nic.values() if isinstance(value, int))
                    if nic
                    else ""
                ),
                "active_duration_seconds": evidence.get("active_duration_seconds", ""),
                "reasons": "; ".join(result.get("reasons", [])),
            }
        )
    _atomic_derived(run_dir / "summary.csv", output.getvalue())

    by_cell: dict[str, list[dict[str, Any]]] = {}
    for result in results:
        by_cell.setdefault(result["cell_id"], []).append(result)
    lines = [
        f"# Benchmark report: {plan['run_id']}",
        "",
        f"Plan SHA-256: `{plan['plan_sha256']}`",
        "",
        "Raw `results.jsonl` is authoritative; this report is a derived view.",
        "",
        "| Cell | Valid | Invalid | Failed | Publication ready |",
        "| --- | ---: | ---: | ---: | --- |",
    ]
    for cell in plan["cells"]:
        cell_results = by_cell.get(cell["cell_id"], [])
        counts = {
            state: sum(1 for result in cell_results if result["state"] == state)
            for state in ("valid", "invalid", "failed")
        }
        publication_ready = (
            plan["suite"]["repetitions"] >= 3
            and plan["suite"]["duration_seconds"] >= 30
            and len(cell_results) == plan["suite"]["repetitions"]
            and counts["valid"] == plan["suite"]["repetitions"]
        )
        lines.append(
            f"| `{cell['cell_id']}` | {counts['valid']} | {counts['invalid']} | "
            f"{counts['failed']} | {'yes' if publication_ready else 'no'} |"
        )
    lines.extend(["", "## Repetition verdicts", ""])
    for result in sorted(
        results, key=lambda item: (item["cell_id"], item["repetition"])
    ):
        lines.append(
            f"- `{result['cell_id']}` repetition {result['repetition']}: "
            f"**{result['state']}**"
        )
        for reason in result.get("reasons", []):
            lines.append(f"  - {reason}")
    _atomic_derived(run_dir / "report.md", "\n".join(lines) + "\n")


def result_key(result: dict[str, Any]) -> tuple[str, int]:
    return result["cell_id"], int(result["repetition"])
