#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Render a DPDK multi-queue payload sweep as a line plot.

Reads a runs.csv produced by examples/run_spark_mq_bench.sh (columns:
cell,tx_cores,rx_cores,payload,gbps,pps,drops,cpu8,cpu16,cpu17,cpu18,cpu19)
and plots achieved Gb/s vs payload size, one line per (TX,RX) core cell.

Usage:
    scripts/plot_mq_payload_sweep.py <runs.csv> [output.svg] [chart title]

Default output: docs/images/spark-mq-payload-sweep.svg (relative to repo root).
The chart title defaults to the DGX Spark wording; pass a third argument to
override it for another platform (e.g. "IGX Orin — DPDK multi-queue throughput
vs payload"). Requires matplotlib (not a runtime dependency of DAQIRI; install
in a venv to regenerate, e.g. `python3 -m venv .venv && .venv/bin/pip install
matplotlib`).
"""

import csv
import sys
from collections import OrderedDict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")  # headless: write a file, never open a window
import matplotlib.pyplot as plt  # noqa: E402

# Fixed cell order + human labels, so the legend reads (1,1) -> (2,2).
CELL_ORDER = ["1t1r", "1t2r", "2t1r", "2t2r"]
CELL_LABEL = {
    "1t1r": "1 TX / 1 RX core",
    "1t2r": "1 TX / 2 RX cores",
    "2t1r": "2 TX / 1 RX core",
    "2t2r": "2 TX / 2 RX cores",
}


def load(csv_path):
    """cell -> list of (payload_bytes, gbps), sorted by payload."""
    series = OrderedDict((c, []) for c in CELL_ORDER)
    with open(csv_path, newline="") as fh:
        for row in csv.DictReader(fh):
            cell = row["cell"].strip()
            if cell not in series:
                series[cell] = []  # tolerate unexpected cells
            series[cell].append((int(row["payload"]), float(row["gbps"])))
    for cell in series:
        series[cell].sort(key=lambda pt: pt[0])
    return series


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    csv_path = Path(argv[1])
    if len(argv) >= 3:
        out_path = Path(argv[2])
    else:
        repo_root = Path(__file__).resolve().parent.parent
        out_path = repo_root / "docs" / "images" / "spark-mq-payload-sweep.svg"
    title = argv[3] if len(argv) >= 4 else \
        "DGX Spark — DPDK multi-queue throughput vs payload"

    series = load(csv_path)

    fig, ax = plt.subplots(figsize=(7.0, 4.3))
    payload_ticks = set()
    for cell in CELL_ORDER:
        pts = series.get(cell) or []
        if not pts:
            continue
        xs = [p for p, _ in pts]
        ys = [g for _, g in pts]
        payload_ticks.update(xs)
        ax.plot(xs, ys, marker="o", linewidth=1.8, markersize=4,
                label=CELL_LABEL.get(cell, cell))

    ax.set_xscale("log", base=2)
    ax.set_xlabel("UDP payload size (bytes)")
    ax.set_ylabel("Achieved throughput (Gb/s)")
    ax.set_title(title)
    ax.set_ylim(bottom=0)
    if payload_ticks:
        ticks = sorted(payload_ticks)
        ax.set_xticks(ticks)
        ax.set_xticklabels([str(t) for t in ticks])
    ax.grid(True, which="both", linewidth=0.4, alpha=0.5)
    ax.legend(title="Cores (per side)", frameon=False)
    fig.tight_layout()

    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path)
    print(f"wrote {out_path}")


if __name__ == "__main__":
    main(sys.argv)
