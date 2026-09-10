# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import itertools
import json

import pytest
from benchmark_harness.lifecycle import Harness
from benchmark_harness.manifest import resolve_plan
from benchmark_harness.results import initialize_run, load_results


@pytest.fixture(autouse=True)
def controlled_lifecycle_time(monkeypatch):
    ticks = itertools.count()
    monkeypatch.setattr(
        "benchmark_harness.lifecycle.time.monotonic", lambda: float(next(ticks))
    )
    monkeypatch.setattr("benchmark_harness.lifecycle.time.sleep", lambda _seconds: None)


def make_harness(tmp_path, suite, site, executors):
    plan = resolve_plan(suite, site, "lifecycle-test")
    run_dir = tmp_path / "run"
    initialize_run(run_dir, plan)
    return Harness(plan, run_dir, executors, tmp_path), plan, run_dir


def test_clean_repetition_is_valid_and_writes_canonical_views(
    tmp_path, suite, site, fake_executors
):
    harness, plan, run_dir = make_harness(tmp_path, suite, site, fake_executors())

    assert harness.run_all() == 0
    results = load_results(run_dir / "results.jsonl")
    assert [result["state"] for result in results] == ["valid"]
    assert results[0]["evidence"]["application"]["delivery_loss_packets"] == 0
    assert (run_dir / "summary.csv").read_text().startswith("cell_id,repetition,state,")
    report = (run_dir / "report.md").read_text()
    assert "Publication ready |" in report
    assert "| no |" in report
    repetition = run_dir / "cells" / plan["cells"][0]["cell_id"] / "repetition-1"
    assert (repetition / "snapshots" / "startup_begin-tx-host.json").exists()
    assert (repetition / "snapshots" / "active_start-rx-host.json").exists()
    assert (repetition / "snapshots" / "active_end-rx-host.json").exists()
    assert (repetition / "snapshots" / "drain_end-rx-host.json").exists()
    assert (repetition / "snapshots" / "shutdown_tail-rx-host.json").exists()


@pytest.mark.parametrize(
    ("failures", "reason"),
    [
        ({"readiness": True}, "readiness"),
        ({"tx_exit": 7}, "exited 7"),
        ({"rx_exit": 8}, "exited 8"),
        ({"snapshot": 3}, "snapshot failure"),
        ({"collector_ready": True}, "collector readiness"),
        ({"cleanup": True}, "cleanup failed"),
        ({"container_cleanup": True}, "container="),
    ],
)
def test_orchestration_failures_are_failed_and_cleanup_is_bounded(
    tmp_path, suite, site, fake_executors, failures, reason
):
    executors = fake_executors(**failures)
    harness, _plan, run_dir = make_harness(tmp_path, suite, site, executors)

    assert harness.run_all() == 1
    result = load_results(run_dir / "results.jsonl")[0]
    assert result["state"] == "failed"
    assert any(reason in item for item in result["reasons"])
    assert all(
        not process["running"]
        for executor in executors.values()
        for process in executor.processes.values()
    )


@pytest.mark.parametrize("failure", ["missing_summary", "collector_output"])
def test_missing_required_evidence_is_invalid_not_success(
    tmp_path, suite, site, fake_executors, failure
):
    harness, _plan, run_dir = make_harness(
        tmp_path, suite, site, fake_executors(**{failure: True})
    )

    assert harness.run_all() == 2
    result = load_results(run_dir / "results.jsonl")[0]
    assert result["state"] == "invalid"
    assert result["reasons"]


def test_interrupt_records_failed_result_then_reraises_after_cleanup(
    tmp_path, suite, site, fake_executors
):
    executors = fake_executors(interrupt_on_wait=True)
    harness, _plan, run_dir = make_harness(tmp_path, suite, site, executors)

    with pytest.raises(KeyboardInterrupt):
        harness.run_all()
    result = load_results(run_dir / "results.jsonl")[0]
    assert result["state"] == "failed"
    assert result["reasons"] == ["benchmark interrupted"]
    assert all(
        not process["running"]
        for executor in executors.values()
        for process in executor.processes.values()
    )


def test_initialization_failure_records_every_planned_repetition(
    tmp_path, suite, site, fake_executors
):
    suite["repetitions"] = 3
    harness, plan, run_dir = make_harness(tmp_path, suite, site, fake_executors())

    harness.record_initialization_failure("synthetic preflight failure")

    results = load_results(run_dir / "results.jsonl")
    assert len(results) == sum(len(cell["repetitions"]) for cell in plan["cells"])
    assert {result["state"] for result in results} == {"failed"}
    assert all(
        result["reasons"] == ["synthetic preflight failure"] for result in results
    )


def test_resume_marks_started_orphan_failed_without_rerunning(
    tmp_path, suite, site, fake_executors
):
    harness, plan, run_dir = make_harness(tmp_path, suite, site, fake_executors())
    cell_id = plan["cells"][0]["cell_id"]
    orphan = run_dir / "cells" / cell_id / "repetition-1"
    orphan.mkdir(parents=True)
    (orphan / "evidence-of-start").write_text("started\n")

    harness.recover_orphans()

    result = load_results(run_dir / "results.jsonl")[0]
    assert result["state"] == "failed"
    assert "was not rerun" in result["reasons"][0]
    assert harness.run_all() == 1


def test_results_jsonl_has_one_machine_readable_record_per_repetition(
    tmp_path, suite, site, fake_executors
):
    suite["repetitions"] = 3
    harness, _plan, run_dir = make_harness(tmp_path, suite, site, fake_executors())
    assert harness.run_all() == 0

    lines = (run_dir / "results.jsonl").read_text().splitlines()
    assert len(lines) == 3
    decoded = [json.loads(line) for line in lines]
    assert [(value["repetition"], value["state"]) for value in decoded] == [
        (1, "valid"),
        (2, "valid"),
        (3, "valid"),
    ]
