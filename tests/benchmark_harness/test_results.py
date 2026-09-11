# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import copy

import pytest
from benchmark_harness.manifest import ManifestError, load_resolved, resolve_plan
from benchmark_harness.results import (
    RESULT_VERSION,
    append_jsonl,
    initialize_run,
    render_summary,
    verify_run_identity,
)


def result_for(plan):
    cell = plan["cells"][0]
    return {
        "schema_version": RESULT_VERSION,
        "run_id": plan["run_id"],
        "plan_sha256": plan["plan_sha256"],
        "cell_id": cell["cell_id"],
        "repetition": 1,
        "topology": "physical_cross_host",
        "evidence_class": "physical_cross_host",
        "adapter": "udp_socket",
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
        "started_utc": "2026-09-10T00:00:00+00:00",
        "ended_utc": "2026-09-10T00:00:01+00:00",
        "state": "valid",
        "reasons": [],
        "role_exits": {},
        "artifacts": {"repetition_dir": "cells/example/repetition-1"},
        "evidence": {
            "requested_rate": {"aggregate_gbps": 25.0},
            "achieved_rate": {"mlnx_rx_aggregate_gbps": 25.0},
            "application": {
                "rx_payload_gbps": 25.0,
                "delivery_loss_packets": 0,
            },
            "kernel_udp": {"InErrors": 0, "RcvbufErrors": 0, "ReasmFails": 0},
            "nic_rx_discards": {"link-0.rx_out_of_buffer": 0},
            "active_duration_seconds": 30.0,
        },
    }


def test_derived_views_are_stable_and_do_not_change_authoritative_jsonl(
    tmp_path, suite, site
):
    plan = resolve_plan(suite, site, "stable-results")
    run_dir = tmp_path / "run"
    initialize_run(run_dir, plan)
    append_jsonl(run_dir / "results.jsonl", result_for(plan))
    authoritative = (run_dir / "results.jsonl").read_bytes()

    render_summary(run_dir, plan)
    first_csv = (run_dir / "summary.csv").read_bytes()
    first_report = (run_dir / "report.md").read_bytes()
    render_summary(run_dir, plan)

    assert (run_dir / "results.jsonl").read_bytes() == authoritative
    assert (run_dir / "summary.csv").read_bytes() == first_csv
    assert (run_dir / "report.md").read_bytes() == first_report


def test_resolved_manifest_hash_detects_resume_tampering(tmp_path, suite, site):
    plan = resolve_plan(suite, site, "immutable-resume")
    run_dir = tmp_path / "run"
    initialize_run(run_dir, plan)
    resolved = run_dir / "resolved-manifest.yaml"
    resolved.chmod(0o644)
    resolved.write_text(
        resolved.read_text().replace("duration_seconds: 1", "duration_seconds: 2", 1)
    )

    with pytest.raises(ManifestError, match="hash"):
        load_resolved(resolved)


def test_run_directory_identity_must_match_resolved_plan(tmp_path, suite, site):
    plan = resolve_plan(suite, site, "identity-check")
    run_dir = tmp_path / "run"
    initialize_run(run_dir, plan)
    changed = copy.deepcopy(plan)
    changed["plan_sha256"] = "0" * 64

    with pytest.raises(ValueError, match="plan hash"):
        verify_run_identity(run_dir, changed)
