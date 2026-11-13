#!/usr/bin/env python3
"""Analyze packet-matched hardware TX/RX deadline measurements (stdlib only)."""

import argparse
import csv
import json
import math
from collections import defaultdict
from pathlib import Path
from statistics import NormalDist


def percentile(values, fraction):
    values = sorted(values)
    return values[max(0, math.ceil(len(values) * fraction) - 1)] if values else None


def wilson(k, n, z):
    if not n:
        return 0.0, 1.0
    p = k / n
    denominator = 1 + z * z / n
    center = (p + z * z / (2 * n)) / denominator
    radius = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / denominator
    return max(0.0, center - radius), min(1.0, center + radius)


def load(paths, tolerance_ns, rx_path_ns):
    rows = []
    seen = set()
    for path in paths:
        file_start = len(rows)
        with Path(path).open(newline="") as handle:
            for line, raw in enumerate(csv.DictReader(handle), 2):
                row = dict(raw)
                for key in raw.keys() - {"method", "status"}:
                    row[key] = int(raw[key])
                identity = (row["method"], row["sequence"])
                if identity in seen:
                    raise ValueError(f"{path}:{line}: duplicate method/sequence")
                seen.add(identity)
                if not 0 <= row["clock_uncertainty_ns"] <= tolerance_ns:
                    raise ValueError(f"{path}:{line}: clock uncertainty exceeds deadline tolerance")
                if row["status"] == "ok" and (not row["tx_completion_ns"] or not row["rx_hw_ns"]):
                    raise ValueError(f"{path}:{line}: successful row is missing hardware timestamps")
                if not row["method"].startswith(("daqiri-", "gpunetio")):
                    raise ValueError(f"{path}:{line}: unknown transmitter")
                row["tx_error_ns"] = (
                    row["tx_completion_ns"] - row["tx_clock_offset_ns"] - row["deadline_ns"]
                    if row["tx_completion_ns"] else None
                )
                row["rx_error_ns"] = (
                    row["rx_hw_ns"] - row["rx_clock_offset_ns"] - row["deadline_ns"] - rx_path_ns
                    if row["rx_hw_ns"] else None
                )
                row["rx_minus_tx_ns"] = (
                    row["rx_error_ns"] + rx_path_ns - row["tx_error_ns"]
                    if row["rx_error_ns"] is not None and row["tx_error_ns"] is not None else None
                )
                row["missing"] = row["status"] != "ok" or not row["tx_completion_ns"] or not row["rx_hw_ns"]
                row["tx_late"] = row["tx_error_ns"] is not None and row["tx_error_ns"] > tolerance_ns
                row["rx_late"] = row["rx_error_ns"] is not None and row["rx_error_ns"] > tolerance_ns
                row["miss"] = bool(row["missing"] or row["tx_late"] or row["rx_late"])
                uncertainty = row["clock_uncertainty_ns"]
                errors = (row["tx_error_ns"], row["rx_error_ns"])
                row["possible_miss"] = bool(row["missing"] or any(
                    error is not None and error + uncertainty > tolerance_ns for error in errors
                ))
                row["certain_miss"] = bool(row["missing"] or any(
                    error is not None and error - uncertainty > tolerance_ns for error in errors
                ))
                rows.append(row)
        local = rows[file_start:]
        # Current files include planned dimensions. Reject interrupted runs
        # even when the conditions already written happened to look good.
        if local and "expected_samples" in local[0]:
            fields = ("expected_methods", "expected_leads", "expected_repeats", "expected_samples")
            plan = tuple(local[0][key] for key in fields)
            if any(tuple(row[key] for key in fields) != plan for row in local):
                raise ValueError(f"{path}: inconsistent run plan")
            methods, leads, repeats, samples = plan
            counts = defaultdict(int)
            for row in local:
                counts[(row["method"], row["lead_ns"], row["repeat"])] += 1
            if (min(plan) <= 0 or len({row["method"] for row in local}) != methods
                    or len({row["lead_ns"] for row in local}) != leads
                    or {row["repeat"] for row in local} != set(range(repeats))
                    or len(counts) != methods * leads * repeats
                    or any(count != samples for count in counts.values())):
                raise ValueError(f"{path}: incomplete benchmark; discard the interrupted run")
    if not rows:
        raise ValueError("no samples")
    if len({row.get("scheduled_commands", 0) for row in rows}) != 1:
        raise ValueError("incompatible command timing origins")
    return rows


def summarize(rows):
    result = {"samples": len(rows)}
    for field in ("missing", "tx_late", "rx_late", "miss"):
        result[field] = sum(bool(row[field]) for row in rows)
    for field in ("possible_miss", "certain_miss"):
        result[field] = sum(bool(row.get(field, row["miss"])) for row in rows)
    result["miss_rate"] = result["miss"] / len(rows)
    for field in ("tx_error_ns", "rx_error_ns", "rx_minus_tx_ns"):
        values = [row[field] for row in rows if row[field] is not None]
        result[field] = {
            "min": min(values) if values else None,
            "p50": percentile(values, 0.5),
            "p99": percentile(values, 0.99),
            "p999": percentile(values, 0.999),
            "max": max(values) if values else None,
            "early": sum(value < 0 for value in values),
        }
    return result


def analyze(rows, margin, confidence):
    # Select on the first repetition only; all later repetitions are held out.
    # This avoids choosing the method on the data used to claim noninferiority.
    repeats = sorted({row["repeat"] for row in rows})
    groups = defaultdict(list)
    for row in rows:
        groups[(row["method"], row["packet_size"], row["lead_ns"], row["repeat"], row["seed"])].append(row)
    training = defaultdict(list)
    for row in rows:
        if row["repeat"] == repeats[0] and row["method"].startswith("daqiri-"):
            training[row["method"]].append(row)
    if not training:
        raise ValueError("no DAQIRI notification methods")

    def score(method):
        per_condition = defaultdict(list)
        for row in training[method]:
            per_condition[(row["packet_size"], row["lead_ns"])].append(row)
        stats = [summarize(data) for data in per_condition.values()]
        return (
            sum(s["miss_rate"] for s in stats) / len(stats),
            max(s["rx_error_ns"]["p999"] if s["rx_error_ns"]["p999"] is not None else math.inf for s in stats),
            method,
        )

    # All candidates must have the same conditions and number of training
    # samples, otherwise a method measured only at easy leads could win.
    signatures = []
    for data in training.values():
        signature = defaultdict(int)
        for row in data:
            signature[(row["packet_size"], row["lead_ns"], row["seed"])] += 1
        signatures.append(dict(signature))
    if any(s != signatures[0] for s in signatures[1:]):
        raise ValueError("notification methods have unequal training conditions/sample counts")
    winner = min(training, key=score)
    # Simultaneous approximate one-sided bounds across all validation
    # conditions and candidates. Bonferroni also accounts for selection.
    conditions = [key for key in groups if key[0].startswith("daqiri-") and key[3] != repeats[0]]
    z = NormalDist().inv_cdf(1 - (1 - confidence) / (2 * max(1, len(conditions))))
    comparisons = []
    for key in sorted(conditions):
        method, size, lead, repeat, seed = key
        baseline = (
            "gpunetio-prebuilt-persistent" if method == "daqiri-gdrcopy-prebuilt"
            else "gpunetio-persistent" if "persistent" in method else "gpunetio"
        )
        base_key = (baseline, size, lead, repeat, seed)
        candidate = summarize(groups[key])
        if base_key not in groups:
            comparisons.append({"method": method, "packet_size": size, "lead_ns": lead,
                                "repeat": repeat, "seed": seed, "verdict": "missing GPUNetIO baseline"})
            continue
        base = summarize(groups[base_key])
        upper = (wilson(candidate["possible_miss"], candidate["samples"], z)[1]
                 - wilson(base["certain_miss"], base["samples"], z)[0])
        comparisons.append({
            "method": method, "baseline": baseline, "packet_size": size, "lead_ns": lead,
            "repeat": repeat, "seed": seed, "daqiri": candidate, "gpunetio": base,
            "observed_excess_miss_rate": candidate["miss_rate"] - base["miss_rate"],
            "approx_excess_miss_rate_upper": upper,
            "verdict": "within margin" if upper <= margin else "not demonstrated",
        })
    winner_checks = [c for c in comparisons if c["method"] == winner]
    expected = {(size, lead, seed) for method, size, lead, repeat, seed in groups
                if method == winner and repeat == repeats[0]}
    complete = all(
        {(c["packet_size"], c["lead_ns"], c["seed"]) for c in winner_checks if c["repeat"] == repeat} == expected
        for repeat in repeats[1:]
    )
    supported = (len(repeats) >= 2 and complete and bool(winner_checks)
                 and all(c["verdict"] == "within margin" for c in winner_checks))
    return {
        "selected_method": winner, "selection_repeat": repeats[0], "validation_repeats": repeats[1:],
        "allowed_excess_miss_rate": margin, "confidence": confidence,
        "verdict": "within margin" if supported else "not demonstrated",
        "inference": "Bonferroni-adjusted Wilson bounds; approximate, assumes independent packet misses within each repetition. Inspect per-repeat tails for correlated stalls.",
        "comparisons": comparisons,
        "summaries": [dict(method=k[0], packet_size=k[1], lead_ns=k[2], repeat=k[3], seed=k[4], **summarize(v))
                      for k, v in sorted(groups.items())],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", nargs="+")
    parser.add_argument("--tolerance-ns", type=int, required=True)
    parser.add_argument("--rx-path-ns", type=int, required=True,
                        help="fixed RX path allowance measured independently; never fit per method")
    parser.add_argument("--margin", type=float, default=0.0, help="allowed excess miss probability (0.0001 = 100 ppm)")
    parser.add_argument("--confidence", type=float, default=0.95)
    parser.add_argument("--json", type=Path, required=True)
    args = parser.parse_args()
    if args.tolerance_ns <= 0 or args.rx_path_ns < 0 or not 0 <= args.margin < 1 or not 0.5 < args.confidence < 1:
        parser.error("invalid tolerance, path allowance, margin, or confidence")
    try:
        rows = load(args.csv, args.tolerance_ns, args.rx_path_ns)
        report = analyze(rows, args.margin, args.confidence)
    except (ValueError, KeyError) as error:
        parser.error(str(error))
    report.update(tolerance_ns=args.tolerance_ns, rx_path_ns=args.rx_path_ns)
    args.json.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Selected on repeat {report['selection_repeat']}: {report['selected_method']}")
    print(f"Held-out comparison: {report['verdict']} (allowed excess={args.margin:g})")
    return 0 if report["verdict"] == "within margin" else 2


if __name__ == "__main__":
    raise SystemExit(main())
