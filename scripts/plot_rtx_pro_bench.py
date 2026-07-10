#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Plot RTX PRO 6000 benchmark CSV results from bench-results/."""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path
from typing import Iterable

try:
    import matplotlib.pyplot as plt
except ImportError as exc:  # pragma: no cover
    print("matplotlib is required: pip install matplotlib", file=sys.stderr)
    raise SystemExit(1) from exc


CSV_COLUMNS = [
    "lang",
    "backend",
    "post_process",
    "payload",
    "batch",
    "target_gbps",
    "seconds",
    "packets",
    "bytes",
    "pps",
    "gbps",
    "drops",
    "drops_kind",
    "cpu_master_pct",
    "cpu_tx_pct",
    "cpu_rx_pct",
    "gpu_sm_pct",
    "gpu_mem_pct",
]


def _to_float(value: str, default: float = 0.0) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def load_runs(path: Path) -> list[dict[str, str]]:
    csv_path = path / "runs.csv" if path.is_dir() else path
    if not csv_path.is_file():
        raise FileNotFoundError(f"runs.csv not found at {csv_path}")
    with csv_path.open(newline="") as fh:
        reader = csv.DictReader(fh)
        return list(reader)


def plot_series(
    runs: Iterable[dict[str, str]],
    *,
    x_key: str,
    y_key: str,
    series_key: str | None,
    title: str,
    out_path: Path,
) -> None:
    fig, ax = plt.subplots(figsize=(10, 6))
    grouped: dict[str, list[tuple[float, float]]] = {}
    for row in runs:
        x = _to_float(row.get(x_key, ""))
        y = _to_float(row.get(y_key, ""))
        label = row.get(series_key, "all") if series_key else "all"
        grouped.setdefault(str(label), []).append((x, y))
    for label, points in sorted(grouped.items(), key=lambda kv: kv[0]):
        points.sort(key=lambda p: p[0])
        xs, ys = zip(*points) if points else ([], [])
        ax.plot(xs, ys, marker="o", label=label)
    ax.set_xlabel(x_key)
    ax.set_ylabel(y_key)
    ax.set_title(title)
    ax.grid(True, alpha=0.3)
    if len(grouped) > 1:
        ax.legend()
    fig.tight_layout()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"wrote {out_path}")


def plot_drop_curve(
    runs: list[dict[str, str]], *, fixed_batch: int | None, out_path: Path
) -> None:
    filtered = runs
    if fixed_batch is not None:
        filtered = [r for r in runs if int(float(r.get("batch", 0))) == fixed_batch]
    plot_series(
        filtered,
        x_key="target_gbps",
        y_key="gbps",
        series_key="payload",
        title="Drop curve (achieved vs target Gbps)",
        out_path=out_path,
    )


def plot_cpu(runs: list[dict[str, str]], *, cores: list[str], out_path: Path) -> None:
    fig, ax = plt.subplots(figsize=(10, 6))
    col_map = {
        "master": "cpu_master_pct",
        "tx": "cpu_tx_pct",
        "rx": "cpu_rx_pct",
    }
    for idx, row in enumerate(runs):
        xs = list(range(len(cores)))
        ys = [_to_float(row.get(col_map.get(c, c), "")) for c in cores]
        ax.plot(xs, ys, marker="o", label=f"run {idx} p={row.get('payload')} b={row.get('batch')}")
    ax.set_xticks(range(len(cores)))
    ax.set_xticklabels(cores)
    ax.set_ylabel("CPU busy %")
    ax.set_title("CPU utilization by core role")
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=8)
    fig.tight_layout()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"wrote {out_path}")


def plot_compare(sw_dir: Path, nic_dir: Path, out_path: Path) -> None:
    sw = load_runs(sw_dir)
    nic = load_runs(nic_dir)
    fig, ax = plt.subplots(figsize=(8, 5))
    for label, rows in (("SW loopback (non-wire)", sw), ("NIC wire", nic)):
        if not rows:
            continue
        gbps = [_to_float(r.get("gbps", "")) for r in rows]
        ax.bar(label, sum(gbps) / len(gbps), alpha=0.8)
    ax.set_ylabel("Gbps (mean)")
    ax.set_title("SW loopback vs NIC wire (labeled comparison)")
    fig.tight_layout()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"wrote {out_path}")


def resolve_output_dir(csv_arg: Path) -> Path:
    if csv_arg.is_dir():
        return csv_arg
    return csv_arg.parent


def main() -> int:
    parser = argparse.ArgumentParser(description="Plot RTX PRO 6000 benchmark CSVs")
    parser.add_argument("--csv", type=Path, required=True, help="runs.csv or result directory")
    parser.add_argument("--x", default="payload", help="X axis column for generic plot")
    parser.add_argument("--y", default="gbps", help="Y axis column for generic plot")
    parser.add_argument("--series", default="batch", help="Series grouping column")
    parser.add_argument(
        "--plot",
        choices=("generic", "drop-curve", "cpu"),
        default="generic",
        help="Plot type",
    )
    parser.add_argument("--fixed-batch", type=int, default=None, help="Filter batch for drop-curve")
    parser.add_argument(
        "--cores",
        default="master,tx,rx",
        help="Comma-separated core roles for CPU plot",
    )
    parser.add_argument(
        "--compare",
        help="Compare SW vs NIC: sw:<dir>,nic:<dir>",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=None,
        help="Output directory for PNGs (default: <csv-dir>/plots)",
    )
    args = parser.parse_args()

    if args.compare:
        parts = dict(p.split(":", 1) for p in args.compare.split(",") if ":" in p)
        sw_dir = Path(parts["sw"])
        nic_dir = Path(parts["nic"])
        out_dir = args.out_dir or resolve_output_dir(args.csv) / "plots"
        plot_compare(sw_dir, nic_dir, out_dir / "compare_sw_vs_nic.png")
        return 0

    runs = load_runs(args.csv)
    out_dir = args.out_dir or resolve_output_dir(args.csv) / "plots"

    if args.plot == "drop-curve":
        plot_drop_curve(
            runs,
            fixed_batch=args.fixed_batch,
            out_path=out_dir / "drop_curve.png",
        )
    elif args.plot == "cpu":
        cores = [c.strip() for c in args.cores.split(",") if c.strip()]
        plot_cpu(runs, cores=cores, out_path=out_dir / "cpu_busy.png")
    else:
        plot_series(
            runs,
            x_key=args.x,
            y_key=args.y,
            series_key=args.series if args.series else None,
            title=f"{args.y} vs {args.x}",
            out_path=out_dir / f"{args.y}_vs_{args.x}.png",
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
