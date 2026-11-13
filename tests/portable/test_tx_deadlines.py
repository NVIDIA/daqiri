"""Deadline analysis must retain loss and avoid claiming equality from zero misses."""

import csv
import importlib.util
from pathlib import Path

import pytest

SPEC = importlib.util.spec_from_file_location(
    "tx_deadlines", Path(__file__).parents[2] / "scripts" / "analyze_tx_deadlines.py"
)
analysis = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(analysis)


def row(method="daqiri-mapped-flag", repeat=0, miss=False, sequence=1):
    return dict(method=method, repeat=repeat, seed=1, sequence=sequence,
                packet_size=256, lead_ns=10000, missing=False,
                tx_late=miss, rx_late=miss, miss=miss,
                tx_error_ns=2000 if miss else -30,
                rx_error_ns=2100 if miss else 20, rx_minus_tx_ns=50)


def test_zero_observed_misses_does_not_prove_zero_excess():
    rows = [row(method, repeat, sequence=i) for method in ("daqiri-mapped-flag", "gpunetio")
            for repeat in (0, 1) for i in range(100)]
    report = analysis.analyze(rows, margin=0, confidence=0.95)
    assert report["verdict"] == "not demonstrated"
    assert report["comparisons"][0]["approx_excess_miss_rate_upper"] > 0


def test_selection_does_not_use_validation_outcomes():
    rows = []
    for repeat in (0, 1):
        for i in range(100):
            rows += [row("daqiri-mapped-flag", repeat, miss=repeat == 1, sequence=i),
                     row("daqiri-event-poll", repeat, miss=repeat == 0, sequence=i),
                     row("gpunetio", repeat, sequence=i)]
    report = analysis.analyze(rows, margin=0.1, confidence=0.95)
    assert report["selected_method"] == "daqiri-mapped-flag"
    assert report["verdict"] == "not demonstrated"


def test_persistent_producer_requires_persistent_baseline():
    rows = [row(method, repeat, sequence=i)
            for method in ("daqiri-persistent-flag", "gpunetio")
            for repeat in (0, 1) for i in range(10)]
    report = analysis.analyze(rows, margin=0.9, confidence=0.95)
    assert report["verdict"] == "not demonstrated"
    assert report["comparisons"][0]["verdict"] == "missing GPUNetIO baseline"


def test_unequal_training_conditions_rejected():
    with pytest.raises(ValueError, match="unequal training"):
        analysis.analyze([row(), row("daqiri-event-poll"), row("daqiri-event-poll", sequence=2)], 0, 0.95)


def test_missing_validation_condition_cannot_pass():
    rows = [row(method, repeat, sequence=i)
            for method in ("daqiri-mapped-flag", "gpunetio")
            for repeat in (0, 1) for i in range(100)]
    harder = row(sequence=1000)
    harder["lead_ns"] = 2000
    rows.append(harder)
    assert analysis.analyze(rows, 0.9, 0.95)["verdict"] == "not demonstrated"


def test_clock_correction_negative_completion_delta_and_loss(tmp_path):
    fields = dict(method="daqiri-mapped-flag", status="ok", repeat=0, seed=1, sequence=1,
                  packet_size=256, lead_ns=10000, deadline_ns=100000,
                  tx_completion_ns=100050, rx_hw_ns=100230,
                  tx_clock_offset_ns=100, rx_clock_offset_ns=200, clock_uncertainty_ns=20)
    path = tmp_path / "samples.csv"
    lost = dict(fields, sequence=2, status="rx_timeout", rx_hw_ns=0)
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows([fields, lost])
    rows = analysis.load([path], tolerance_ns=100, rx_path_ns=10)
    assert rows[0]["tx_error_ns"] == -50
    assert rows[0]["rx_error_ns"] == 20
    assert rows[0]["rx_minus_tx_ns"] == 80
    assert rows[1]["miss"]
    assert analysis.summarize(rows)["samples"] == 2
    assert analysis.summarize(rows)["missing"] == 1
    with pytest.raises(ValueError, match="uncertainty"):
        analysis.load([path], tolerance_ns=10, rx_path_ns=10)


def test_repeated_input_is_rejected(tmp_path):
    path = tmp_path / "samples.csv"
    # Include the fields required to interpret a complete row.
    with path.open("w", newline="") as handle:
        data = dict(method="daqiri-mapped-flag", status="ok", sequence=1, clock_uncertainty_ns=0,
                    tx_completion_ns=1, rx_hw_ns=1, tx_clock_offset_ns=0, rx_clock_offset_ns=0, deadline_ns=1)
        writer = csv.DictWriter(handle, fieldnames=data)
        writer.writeheader()
        writer.writerow(data)
    with pytest.raises(ValueError, match="duplicate"):
        analysis.load([path, path], tolerance_ns=1, rx_path_ns=0)


def test_clock_uncertainty_cannot_improve_acceptance():
    rows = [row(method, repeat, sequence=i)
            for method in ("daqiri-mapped-flag", "gpunetio")
            for repeat in (0, 1) for i in range(100)]
    for value in rows:
        value["possible_miss"] = True
        value["certain_miss"] = False
    report = analysis.analyze(rows, margin=0.1, confidence=0.95)
    assert report["verdict"] == "not demonstrated"


def test_scheduled_and_observed_command_origins_cannot_be_pooled(tmp_path):
    data = dict(method="daqiri-mapped-flag", status="ok", sequence=1, clock_uncertainty_ns=0,
                tx_completion_ns=1, rx_hw_ns=1, tx_clock_offset_ns=0, rx_clock_offset_ns=0,
                deadline_ns=1, scheduled_commands=0)
    path = tmp_path / "different-origins.csv"
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=data)
        writer.writeheader()
        writer.writerows([data, dict(data, sequence=2, scheduled_commands=1)])
    with pytest.raises(ValueError, match="command timing origins"):
        analysis.load([path], tolerance_ns=1, rx_path_ns=0)


def test_interrupted_benchmark_is_rejected(tmp_path):
    path = tmp_path / "interrupted.csv"
    data = dict(method="daqiri-mapped-flag", status="ok", sequence=1, clock_uncertainty_ns=0,
                tx_completion_ns=1, rx_hw_ns=1, tx_clock_offset_ns=0, rx_clock_offset_ns=0,
                deadline_ns=1, lead_ns=10000, repeat=0, expected_methods=2,
                expected_leads=1, expected_repeats=3, expected_samples=10000)
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=data)
        writer.writeheader()
        writer.writerow(data)
    with pytest.raises(ValueError, match="incomplete benchmark"):
        analysis.load([path], tolerance_ns=1, rx_path_ns=0)


@pytest.mark.parametrize("baseline,expected", [
    ("gpunetio", "missing GPUNetIO baseline"),
    ("gpunetio-persistent", "missing GPUNetIO baseline"),
    ("gpunetio-prebuilt-persistent", "within margin"),
])
def test_gdrcopy_prebuilt_requires_prebuilt_sdk_baseline(baseline, expected):
    rows = [row(method, repeat, sequence=i)
            for method in ("daqiri-gdrcopy-prebuilt", baseline)
            for repeat in (0, 1) for i in range(100)]
    report = analysis.analyze(rows, margin=0.9, confidence=0.95)
    comparison = report["comparisons"][0]
    assert comparison["verdict"] == expected
    if expected == "within margin":
        assert comparison["baseline"] == baseline
