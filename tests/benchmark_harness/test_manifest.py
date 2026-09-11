# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import copy
from pathlib import Path

import pytest
from benchmark_harness.cli import main
from benchmark_harness.manifest import (
    ManifestError,
    expand_matrix,
    resolve_plan,
    validate_combination,
    validate_rdma_capacity,
    validate_site,
    validate_suite,
)


def test_resolution_is_deterministic_and_expands_exact_commands(suite, site):
    first = resolve_plan(suite, site, "fixed-run")
    second = resolve_plan(copy.deepcopy(suite), copy.deepcopy(site), "fixed-run")
    assert first == second
    assert first["plan_sha256"] == second["plan_sha256"]
    role = first["cells"][0]["repetitions"][0]["roles"][1]
    assert role["command"][-2:] == ["--target-gbps", "25"]
    assert role["placement"]["io_cpu"] != role["placement"]["worker_cpu"]
    assert role["command"][role["command"].index("--seconds") + 1] == "1"
    assert "num_bufs: 4096" in role["config_yaml"]
    assert "iterations: 0" in role["config_yaml"]
    assert "192.0.2.1" in role["config_yaml"]


def test_every_role_and_collector_has_a_unique_process_ownership_id(suite, site):
    suite["repetitions"] = 3
    plan = resolve_plan(suite, site, "ownership-ids")
    process_ids = [
        role["process_run_id"]
        for cell in plan["cells"]
        for repetition in cell["repetitions"]
        for role in [*repetition["roles"], *repetition["collectors"]]
    ]
    assert len(process_ids) == len(set(process_ids))


def test_docker_plan_uses_immutable_image_and_unique_owned_containers(suite, site):
    digest = "sha256:" + ("a" * 64)
    for host in site["hosts"].values():
        host["benchmark_binary"] = "/opt/daqiri/bin/daqiri_bench_socket"
        host["container"] = {
            "runtime": "docker",
            "image": "daqiri:harness-test",
            "digest": digest,
            "workdir": "/opt/daqiri",
            "gpus": True,
        }
    validate_site(site)

    plan = resolve_plan(suite, site, "docker-plan")
    roles = plan["cells"][0]["repetitions"][0]["roles"]
    names = [role["container_name"] for role in roles]
    assert len(names) == len(set(names))
    for role in roles:
        command = role["command"]
        assert command[:5] == ["docker", "run", "--rm", "--pull", "never"]
        assert command[command.index("--name") + 1] == role["container_name"]
        assert (
            command[command.index("--label") + 1]
            == "com.nvidia.daqiri.benchmark.process_id=" + role["process_run_id"]
        )
        assert command[command.index("--gpus") + 1] == "all"
        assert f"DAQIRI_BENCH_RUN_ID={role['process_run_id']}" in command
        image_index = command.index(digest)
        assert command[image_index + 1 :] == role["benchmark_command"]
        assert role["benchmark_command"][0] == host["benchmark_binary"]


def test_matrix_order_is_stable(suite):
    suite["matrix"]["message_size_bytes"] = [1000, 8000]
    suite["matrix"]["requested_rate_gbps"] = [5, 10]
    assert [cell["message_size_bytes"] for cell in expand_matrix(suite)] == [
        1000,
        1000,
        8000,
        8000,
    ]
    assert [cell["requested_rate_gbps"] for cell in expand_matrix(suite)] == [
        5,
        10,
        5,
        10,
    ]


@pytest.mark.parametrize("scope", [None, "", "per_stream", "linkish"])
def test_rate_scope_must_be_explicit_and_unambiguous(suite, scope):
    suite["rate_scope"] = scope
    with pytest.raises(ManifestError, match="rate_scope"):
        validate_suite(suite)


def test_unknown_schema_field_is_rejected(suite):
    suite["surprise"] = True
    with pytest.raises(ManifestError, match="unknown field"):
        validate_suite(suite)


def test_workload_is_explicitly_unsupported_in_v1(suite):
    suite["workload"] = "gemm_fp16"
    with pytest.raises(ManifestError, match="workload"):
        validate_suite(suite)


def test_duplicate_matrix_values_are_rejected(suite):
    suite["matrix"]["requested_rate_gbps"] = [25, 25.0]
    with pytest.raises(ManifestError, match="duplicate"):
        validate_suite(suite)


def test_cpu_overlap_is_rejected(suite, site):
    site["pairs"][0]["rx"]["worker_cpu"] = site["pairs"][0]["rx"]["io_cpu"]
    with pytest.raises(ManifestError, match="CPU overlap"):
        validate_combination(suite, site)


def test_cpu_overlap_can_be_declared(suite, site):
    site["pairs"][0]["rx"]["worker_cpu"] = site["pairs"][0]["rx"]["io_cpu"]
    suite["allow_cpu_overlap"] = True
    validate_combination(suite, site)


def test_buffer_must_cover_message(suite, site):
    suite["matrix"]["buffer_size_bytes"] = [1024]
    with pytest.raises(ManifestError, match="does not cover"):
        validate_combination(suite, site)


def test_queue_capacity_covers_pairs(suite, site):
    site["pairs"].append(copy.deepcopy(site["pairs"][0]))
    site["pairs"][1]["id"] = "pair-1"
    site["pairs"][1]["tx"].update(
        local_port=5102,
        remote_port=5002,
        queue_id=1,
        io_cpu=4,
        worker_cpu=5,
        master_cpu=6,
    )
    site["pairs"][1]["rx"].update(
        local_port=5002,
        remote_port=5102,
        queue_id=1,
        io_cpu=4,
        worker_cpu=5,
        master_cpu=6,
    )
    suite["matrix"]["pair_count"] = [2]
    site["links"]["link-0"]["rx"]["queue_capacity"] = 1
    with pytest.raises(ManifestError, match="capacity"):
        validate_combination(suite, site)


def test_queue_ids_must_be_unique_per_link_direction(suite, site):
    second = copy.deepcopy(site["pairs"][0])
    second["id"] = "pair-1"
    second["tx"].update(
        local_port=5102, remote_port=5002, io_cpu=4, worker_cpu=5, master_cpu=6
    )
    second["rx"].update(
        local_port=5002, remote_port=5102, io_cpu=4, worker_cpu=5, master_cpu=6
    )
    site["pairs"].append(second)
    suite["matrix"]["pair_count"] = [2]
    with pytest.raises(ManifestError, match="queue 0 is assigned twice"):
        validate_combination(suite, site)


def test_local_udp_ports_must_be_unique_per_host(suite, site):
    second = copy.deepcopy(site["pairs"][0])
    second["id"] = "pair-1"
    second["tx"].update(
        remote_port=5002, queue_id=1, io_cpu=4, worker_cpu=5, master_cpu=6
    )
    second["rx"].update(
        local_port=5002, queue_id=1, io_cpu=4, worker_cpu=5, master_cpu=6
    )
    site["pairs"].append(second)
    suite["matrix"]["pair_count"] = [2]
    with pytest.raises(ManifestError, match="local port 5101 is assigned twice"):
        validate_combination(suite, site)


def test_mtu_must_cover_udp_payload(suite, site):
    site["links"]["link-0"]["rx"]["mtu"] = 8000
    with pytest.raises(ManifestError, match="does not cover UDP payload"):
        validate_combination(suite, site)


def test_requested_rate_cannot_exceed_declared_link_capacity(suite, site):
    suite["matrix"]["requested_rate_gbps"] = [101]
    with pytest.raises(ManifestError, match="above declared"):
        validate_combination(suite, site)


def test_rdma_shared_pool_covers_sum_of_depths():
    roles = [
        {
            "rx_depth": 512,
            "tx_depth": 128,
            "shared_rx_pool": 1024,
            "shared_tx_pool": 256,
        },
        {
            "rx_depth": 512,
            "tx_depth": 128,
            "shared_rx_pool": 1024,
            "shared_tx_pool": 256,
        },
    ]
    validate_rdma_capacity(roles)
    roles[0]["shared_rx_pool"] = roles[1]["shared_rx_pool"] = 512
    with pytest.raises(ManifestError, match="depth sum"):
        validate_rdma_capacity(roles)


def test_site_rejects_topology_mutation_hooks(site):
    site["setup_hooks"] = []
    with pytest.raises(ManifestError, match="unknown field"):
        validate_site(site)


def test_local_host_rejects_unused_ssh_destination(site):
    site["hosts"]["tx-host"]["transport"] = "local"
    with pytest.raises(ManifestError, match="must be omitted"):
        validate_site(site)


def test_dry_run_does_not_create_output(tmp_path):
    root = Path(__file__).resolve().parents[2]
    output = tmp_path / "must-not-exist"
    rc = main(
        [
            "run",
            "--suite",
            str(
                root
                / "scripts/benchmark_harness/sample-configs/physical-udp-suite.yaml"
            ),
            "--site",
            str(
                root
                / "scripts/benchmark_harness/sample-configs/physical-udp-site.example.yaml"
            ),
            "--run-id",
            "dry-run",
            "--output",
            str(output),
            "--dry-run",
        ]
    )
    assert rc == 0
    assert not output.exists()
