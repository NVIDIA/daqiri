# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Protect log format detection and packet/counter accounting."""
import importlib.util
import json
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
PACKAGE = ROOT / "skills/daqiri-onboarding"


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


helper = module("onboarding_log_helper", PACKAGE / "scripts/summarize_raw_run.py")


def test_external_eval_cases_have_routing_labels_and_outcome_assertions():
    dataset = json.loads((PACKAGE / "evals/evals.json").read_text())
    cases = dataset["evals"]
    assert cases and len({case["id"] for case in cases}) == len(cases)
    for case in cases:
        assert all(isinstance(case[field], str) and case[field].strip()
                   for field in ("id", "prompt", "expected_output"))
        assert "expected_skill" in case
        assert case["expected_skill"] in (None, dataset["skill_name"])
        assert isinstance(case["assertions"], list) and case["assertions"]
        assert all(isinstance(value, str) and value.strip() for value in case["assertions"])


def test_counter_baseline_and_delta_remain_distinct():
    value = helper.reconcile_counters('{"receiver":{"port0":{"rx_steer_missed_packets":38}}}',
                                     '{"receiver":{"port0":{"rx_steer_missed_packets":38}}}')
    assert value["status"] == "parsed"
    assert value["selected_deltas"]["receiver/port0/rx_steer_missed_packets"] == 0
    assert value["unchanged_positive_selected_levels"]["receiver/port0/rx_steer_missed_packets"] == 38


def test_reset_and_duplicate_counter_inputs_are_not_silent_zeros():
    reset = helper.reconcile_counters('{"rx_packets_phy":10}', '{"rx_packets_phy":2}')
    assert reset["status"] == "ambiguous" and "rx_packets_phy" not in reset["selected_deltas"]
    duplicate = helper.reconcile_counters('{"rx_packets_phy":1,"rx_packets_phy":2}', '{"rx_packets_phy":3}')
    assert duplicate["status"] == "unsupported"


def test_structured_evidence_is_not_misparsed_as_a_raw_log(tmp_path):
    for name, text, kind in [("counters.csv", "time,rx_packets_phy\n0,10\n", "structured_csv"),
                             ("run.json", '{"rx_packets":10}', "structured_json")]:
        path = tmp_path / name
        path.write_text(text)
        value = helper.summarize_bundle(path)
        assert value["status"] == "unsupported" and value["format"] == kind


def test_probe_helper_preserves_deficit_and_positive_counter_baseline(tmp_path):
    for role, count_field, count in [("tx", "accepted", 10007), ("rx", "received", 9984)]:
        row = {"role": role, "expected": 10007, count_field: count}
        (tmp_path / (role + ".log")).write_text("INTEGRITY_RESULT " + json.dumps(row) + "\n")
    for name, packets in [("before.json", 0), ("after.json", 10007)]:
        counters = {"rx_packets_phy": packets, "rx_steer_missed_packets": 38}
        (tmp_path / name).write_text(json.dumps(counters))
    value = helper.summarize_bundle(tmp_path)
    assert value["format"] == "integrity_probe"
    assert value["tx_minus_application_rx"] == 23
    assert value["application_rx_packets"] == 9984
    assert value["application_rx_count_unit"] == "source_packets"
    assert value["end_to_end_losslessness"].startswith("not_established")
    counters = value["nic_counters"]
    assert counters["unchanged_positive_selected_levels"]["rx_steer_missed_packets"] == 38
    assert counters["selected_deltas"]["rx_packets_phy"] == 10007


@pytest.mark.parametrize("marker", [
    "Reorder plan 'ordered' on q0: ppb=32 offset=42 out_bufs=4",
    "ibverbs HW reorder port 0 q0: packets=128 placements=128 completed_batches=4",
    "Initialized 1 DPDK reorder queue state entries",
    "",  # A truncated summary alone does not establish RX units.
])
def test_native_aggregate_or_unknown_units_do_not_report_false_deficits(marker):
    text = (marker + "\nTX complete: interface=tx queue=0 packets=128\n"
            "RX complete: interface=rx queue=0 packets=4\n")
    result = helper.summarize(text, rx_source_packets=bool(marker))
    assert result["application_rx_outputs"] == 4
    assert result["application_rx_packets"] is None
    assert result["tx_minus_application_rx"] is None
    assert not any("124" in line for line in result["observations"])


def test_reorder_benchmark_unscoped_summary_preserves_unknown_units(tmp_path):
    # Even an unrelated ordinary-raw config cannot qualify this unscoped count.
    cfg = {"daqiri": {"cfg": {"stream_type": "raw", "interfaces": [{"rx": {"queues": [{"id": 0}]}}]}}}
    (tmp_path / "config.used.yaml").write_text(json.dumps(cfg))
    (tmp_path / "bench.log").write_text(
        "TX complete: interface=tx queue=0 packets=128\n"
        "RX complete: packets=128 bytes=131072 bursts=4 aggregated_batches=4 "
        "direct_placed_batches=0 timeout_batches=0 aggregated_packets=128 passthrough_packets=0\n")
    result = helper.summarize_bundle(tmp_path)
    assert result["format"] == "native_raw"
    assert result["application_tx_packets"] == 128
    assert result["application_rx_outputs"] == 128
    assert result["application_rx_count_unit"] == "unknown"
    assert result["application_rx_packets"] is None
    assert result["tx_minus_application_rx"] is None
    assert result["application_summaries"][-1]["bytes"] == 131072


def test_hardware_reorder_statistics_alone_are_recognized(tmp_path):
    path = tmp_path / "bench.log"
    path.write_text("ibverbs HW reorder port 0 q0: packets=128 placements=128 completed_batches=4\n")
    result = helper.summarize_bundle(path)
    assert result["format"] == "native_raw"
    assert result["hardware_reorder_snapshots"][0]["reported_counters"] == {
        "packets": 128, "placements": 128, "completed_batches": 4}
    assert result["application_rx_outputs"] is None
    assert result["application_rx_packets"] is None
    assert result["tx_minus_application_rx"] is None
    assert result["end_to_end_losslessness"] == "not_established_by_counts"


@pytest.mark.parametrize("extra", [
    "RX complete: packets=4\n",
    "RX complete: interface=rx queue=0 packets=4\n",
])
def test_unscoped_and_other_summaries_are_not_added(extra):
    result = helper.summarize("TX complete: interface=tx queue=0 packets=128\n"
                              "RX complete: packets=4\n" + extra, rx_source_packets=True)
    assert result["application_rx_outputs"] is None
    assert result["application_rx_packets"] is None
    assert result["tx_minus_application_rx"] is None
    assert any("Unscoped RX" in warning for warning in result["warnings"])


@pytest.mark.parametrize("reorder,marker,expected_gap", [
    (False, "", 4),
    (True, "", None),  # The config also protects captures lacking startup logs.
    (False, "Reorder plan 'ordered' on q0: ppb=32 offset=42 out_bufs=4", None),
])
def test_matching_raw_config_establishes_units_unless_log_conflicts(tmp_path, reorder, marker, expected_gap):
    rx = {"queues": [{"id": 0}]}
    if reorder:
        rx["reorder_configs"] = [{"reorder_engine": "sw"}]
    cfg = {"daqiri": {"cfg": {"stream_type": "raw", "interfaces": [{"rx": rx}]}}}
    (tmp_path / "config.used.yaml").write_text(json.dumps(cfg))
    (tmp_path / "bench.log").write_text(
        marker + "\nTX complete: interface=tx queue=0 packets=128\n"
        "RX complete: interface=rx queue=0 packets=124\n")
    result = helper.summarize_bundle(tmp_path)
    assert result["tx_minus_application_rx"] == expected_gap
    assert result["application_rx_count_unit"] == ("source_packets" if expected_gap == 4 else "unknown")
