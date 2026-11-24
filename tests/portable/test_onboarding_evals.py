# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Protect log format detection and packet/counter accounting."""
import importlib.util
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PACKAGE = ROOT / "skills/daqiri-onboarding"


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


helper = module("onboarding_log_helper", PACKAGE / "scripts/summarize_raw_run.py")


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
    assert value["end_to_end_losslessness"].startswith("not_established")
    counters = value["nic_counters"]
    assert counters["unchanged_positive_selected_levels"]["rx_steer_missed_packets"] == 38
    assert counters["selected_deltas"]["rx_packets_phy"] == 10007
