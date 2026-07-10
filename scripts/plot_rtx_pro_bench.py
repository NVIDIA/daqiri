#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""Render RTX PRO 6000 multi-queue sweep plots (stdlib only, writes SVG).

Reads runs.csv from examples/run_rtx_pro_mq_bench.sh and writes:
  gbps_vs_payload.svg, core_scaling.svg, cpu_utilization.svg, drops_vs_payload.svg

If matplotlib is installed, also writes PNG copies for convenience.
"""

from __future__ import annotations

import csv
import math
import sys
from collections import OrderedDict
from pathlib import Path
from xml.sax.saxutils import escape

CELL_ORDER = ["1t1r", "1t2r", "2t1r", "2t2r"]
CELL_LABEL = {
    "1t1r": "1 TX / 1 RX",
    "1t2r": "1 TX / 2 RX",
    "2t1r": "2 TX / 1 RX",
    "2t2r": "2 TX / 2 RX",
}
COLORS = ["#4C72B0", "#55A868", "#C44E52", "#8172B2"]

CPU_COLS = [
    ("cpu_master", "master"),
    ("cpu_tx_q0_poll", "TX q0 poll"),
    ("cpu_tx_q0_work", "TX q0 work"),
    ("cpu_tx_q1_poll", "TX q1 poll"),
    ("cpu_tx_q1_work", "TX q1 work"),
    ("cpu_rx_q0_poll", "RX q0 poll"),
    ("cpu_rx_q0_work", "RX q0 work"),
    ("cpu_rx_q1_poll", "RX q1 poll"),
    ("cpu_rx_q1_work", "RX q1 work"),
]


def load_rows(csv_path: Path) -> list[dict]:
    with open(csv_path, newline="") as fh:
        return list(csv.DictReader(fh))


def series_by_cell(rows: list[dict], key: str) -> OrderedDict[str, list[tuple[int, float]]]:
    out: OrderedDict[str, list[tuple[int, float]]] = OrderedDict((c, []) for c in CELL_ORDER)
    for row in rows:
        cell = row["cell"].strip()
        if cell not in out:
            out[cell] = []
        out[cell].append((int(row["payload"]), float(row[key])))
    for cell in out:
        out[cell].sort(key=lambda pt: pt[0])
    return out


def largest_payload(rows: list[dict]) -> int:
    return max(int(r["payload"]) for r in rows)


def rows_at_payload(rows: list[dict], payload: int) -> dict[str, dict]:
    picked: dict[str, dict] = {}
    for row in rows:
        if int(row["payload"]) != payload:
            continue
        cell = row["cell"].strip()
        if cell not in picked:
            picked[cell] = row
    return picked


class Svg:
  def __init__(self, w: float, h: float, bg: str = "#ffffff"):
    self.w, self.h = w, h
    self.parts: list[str] = []
    if bg:
      self.parts.append(
        f'<rect x="0" y="0" width="{w}" height="{h}" fill="{bg}"/>'
      )

  def text(self, x, y, s, size=12, anchor="start", weight="normal"):
    self.parts.append(
      f'<text x="{x:.1f}" y="{y:.1f}" font-family="sans-serif" font-size="{size}" '
      f'font-weight="{weight}" text-anchor="{anchor}">{escape(s)}</text>'
    )

  def line(self, x1, y1, x2, y2, color="#333", width=1):
    self.parts.append(
      f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" '
      f'stroke="{color}" stroke-width="{width}"/>'
    )

  def rect(self, x, y, w, h, fill="#4C72B0", stroke="#222", sw=0.6):
    self.parts.append(
      f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" '
      f'fill="{fill}" stroke="{stroke}" stroke-width="{sw}"/>'
    )

  def circle(self, x, y, r=4, fill="#4C72B0"):
    self.parts.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{r}" fill="{fill}"/>')

  def polyline(self, pts, color="#4C72B0", width=2):
    if len(pts) < 2:
      return
    d = " ".join(f"{x:.1f},{y:.1f}" for x, y in pts)
    self.parts.append(
      f'<polyline points="{d}" fill="none" stroke="{color}" stroke-width="{width}"/>'
    )

  def save(self, path: Path):
    body = "\n  ".join(self.parts)
    path.write_text(
      f'<?xml version="1.0" encoding="UTF-8"?>\n'
      f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.w}" height="{self.h}" '
      f'viewBox="0 0 {self.w} {self.h}">\n  {body}\n</svg>\n'
    )


def log_scale_x(payload: int, xmin: int, xmax: int, left: float, right: float) -> float:
    if xmin <= 0 or xmax <= xmin:
        return left
    return left + (math.log2(payload) - math.log2(xmin)) / (math.log2(xmax) - math.log2(xmin)) * (right - left)


def plot_lines(title, xlabel, ylabel, series, out_path: Path, log_x: bool = True):
    margin = dict(l=70, r=200, t=50, b=55)
    W, H = 900, 520
    svg = Svg(W, H)
    plot_l, plot_r = margin["l"], W - margin["r"]
    plot_t, plot_b = margin["t"], H - margin["b"]

    all_y = [v for pts in series.values() for _, v in pts]
    ymax = max(all_y) * 1.12 if all_y else 1
    payloads = sorted({p for pts in series.values() for p, _ in pts})
    xmin, xmax = payloads[0], payloads[-1]

    def x_map(p):
        return log_scale_x(p, xmin, xmax, plot_l, plot_r) if log_x else plot_l + (p - xmin) / max(xmax - xmin, 1) * (plot_r - plot_l)

    def y_map(v):
        return plot_b - (v / ymax) * (plot_b - plot_t)

    # grid + y ticks
    for i in range(6):
        yv = ymax * i / 5
        y = y_map(yv)
        svg.line(plot_l, y, plot_r, y, color="#ddd")
        svg.text(plot_l - 8, y + 4, f"{yv:.0f}", size=10, anchor="end")

    for p in payloads:
        x = x_map(p)
        svg.line(x, plot_t, x, plot_b, color="#eee")
        svg.text(x, plot_b + 18, str(p), size=10, anchor="middle")

    svg.line(plot_l, plot_b, plot_r, plot_b, color="#333")
    svg.line(plot_l, plot_t, plot_l, plot_b, color="#333")

    for idx, cell in enumerate(CELL_ORDER):
        pts = series.get(cell) or []
        if not pts:
            continue
        color = COLORS[idx % len(COLORS)]
        xy = [(x_map(p), y_map(v)) for p, v in pts]
        svg.polyline(xy, color=color, width=2.5)
        for x, y in xy:
            svg.circle(x, y, r=4, fill=color)

    svg.text(W / 2, 24, title, size=15, anchor="middle", weight="bold")
    svg.text(W / 2, H - 12, xlabel, size=12, anchor="middle")
    svg.text(16, H / 2, ylabel, size=12, anchor="middle")

    ly = plot_t + 10
    for idx, cell in enumerate(CELL_ORDER):
        if not series.get(cell):
            continue
        color = COLORS[idx % len(COLORS)]
        lx = plot_r + 20
        svg.line(lx, ly, lx + 24, ly, color=color, width=3)
        svg.circle(lx + 12, ly, r=4, fill=color)
        svg.text(lx + 32, ly + 4, CELL_LABEL[cell], size=11)
        ly += 22

    svg.save(out_path)


def plot_bars(title, labels, values, out_path: Path):
    W, H = 820, 480
    svg = Svg(W, H)
    margin = dict(l=60, r=30, t=50, b=90)
    plot_l, plot_r = margin["l"], W - margin["r"]
    plot_t, plot_b = margin["t"], H - margin["b"]
    ymax = max(values) * 1.15 if values else 1
    n = len(values)
    gap = 18
    bar_w = (plot_r - plot_l - gap * (n - 1)) / max(n, 1)

    for i in range(6):
        yv = ymax * i / 5
        y = plot_b - (yv / ymax) * (plot_b - plot_t)
        svg.line(plot_l, y, plot_r, y, color="#ddd")
        svg.text(plot_l - 8, y + 4, f"{yv:.0f}", size=10, anchor="end")

    for i, (lab, val) in enumerate(zip(labels, values)):
        x = plot_l + i * (bar_w + gap)
        h = (val / ymax) * (plot_b - plot_t)
        y = plot_b - h
        svg.rect(x, y, bar_w, h, fill=COLORS[i % len(COLORS)])
        svg.text(x + bar_w / 2, plot_b + 16, lab, size=10, anchor="middle")
        svg.text(x + bar_w / 2, y - 6, f"{val:.0f}", size=10, anchor="middle")

    svg.line(plot_l, plot_b, plot_r, plot_b, color="#333")
    svg.text(W / 2, 24, title, size=15, anchor="middle", weight="bold")
    svg.text(16, H / 2, "RX Gb/s", size=12, anchor="middle")
    svg.save(out_path)


def plot_cpu_panels(rows_at_max, max_payload: int, out_path: Path):
    W, H = 1000, 620
    svg = Svg(W, H)
    svg.text(W / 2, 24, f"CPU utilization at {max_payload} B payload", size=15, anchor="middle", weight="bold")
    panel_w = (W - 60) / 2
    panel_h = (H - 80) / 2
    origins = [(30, 50), (30 + panel_w + 20, 50), (30, 50 + panel_h + 20), (30 + panel_w + 20, 50 + panel_h + 20)]

    for idx, cell in enumerate(CELL_ORDER):
        row = rows_at_max.get(cell)
        if not row:
            continue
        ox, oy = origins[idx]
        labels, vals = [], []
        for col, label in CPU_COLS:
            raw = row.get(col, "")
            if not raw:
                continue
            v = float(raw)
            if v <= 0.2 and "q1" in col:
                continue
            labels.append(label)
            vals.append(v)
        if not vals:
            continue
        svg.text(ox, oy - 8, CELL_LABEL[cell], size=12, weight="bold")
        bar_h = min(22, (panel_h - 30) / max(len(vals), 1))
        for i, (lab, val) in enumerate(zip(labels, vals)):
            y = oy + 10 + i * (bar_h + 4)
            bw = (val / 100.0) * (panel_w - 120)
            svg.rect(ox + 110, y, bw, bar_h - 2, fill=COLORS[idx % len(COLORS)])
            svg.text(ox + 4, y + bar_h - 5, lab, size=9)
            svg.text(ox + 115 + bw + 4, y + bar_h - 5, f"{val:.0f}%", size=9)
        svg.line(ox + 110, oy + panel_h - 10, ox + panel_w - 10, oy + panel_h - 10, color="#333")
    svg.save(out_path)


def maybe_matplotlib_png(rows, out_dir: Path):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import numpy as np
    except ImportError:
        return

    gbps = series_by_cell(rows, "rx_gbps")
    max_p = largest_payload(rows)
    at_max = rows_at_payload(rows, max_p)

    fig, ax = plt.subplots(figsize=(8, 4.8))
    for idx, cell in enumerate(CELL_ORDER):
        pts = gbps.get(cell) or []
        if not pts:
            continue
        xs, ys = zip(*pts)
        ax.plot(xs, ys, marker="o", label=CELL_LABEL[cell], color=COLORS[idx])
    ax.set_xscale("log", base=2)
    ax.set_xlabel("Payload (bytes)")
    ax.set_ylabel("RX Gb/s")
    ax.set_title("RTX PRO 6000 — throughput vs payload")
    ax.grid(True, alpha=0.4)
    ax.legend()
    fig.tight_layout()
    fig.savefig(out_dir / "gbps_vs_payload.png", dpi=150, facecolor="white")
    plt.close(fig)

    labels = [CELL_LABEL[c] for c in CELL_ORDER if c in at_max]
    values = [float(at_max[c]["rx_gbps"]) for c in CELL_ORDER if c in at_max]
    fig, ax = plt.subplots(figsize=(7.5, 4.5))
    ax.bar(labels, values, color=COLORS[:len(values)])
    ax.set_ylabel("RX Gb/s")
    ax.set_title(f"Core scaling at {max_p} B")
    fig.tight_layout()
    fig.savefig(out_dir / "core_scaling.png", dpi=150, facecolor="white")
    plt.close(fig)


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 1
    csv_path = Path(argv[1])
    out_dir = Path(argv[2]) if len(argv) >= 3 else csv_path.parent / "plots"
    out_dir.mkdir(parents=True, exist_ok=True)

    rows = load_rows(csv_path)
    if not rows:
        print(f"no rows in {csv_path}", file=sys.stderr)
        return 1

    max_p = largest_payload(rows)
    at_max = rows_at_payload(rows, max_p)
    gbps = series_by_cell(rows, "rx_gbps")
    drops = series_by_cell(rows, "drops")

    plot_lines(
        "RTX PRO 6000 — wire RX throughput vs payload",
        "UDP payload size (bytes, log₂ scale)",
        "RX Gb/s",
        gbps,
        out_dir / "gbps_vs_payload.svg",
    )
    plot_bars(
        f"Core scaling at {max_p} B payload",
        [CELL_LABEL[c] for c in CELL_ORDER if c in at_max],
        [float(at_max[c]["rx_gbps"]) for c in CELL_ORDER if c in at_max],
        out_dir / "core_scaling.svg",
    )
    plot_cpu_panels(at_max, max_p, out_dir / "cpu_utilization.svg")
    plot_lines(
        "RTX PRO 6000 — DPDK drops vs payload",
        "UDP payload size (bytes, log₂ scale)",
        "drops",
        drops,
        out_dir / "drops_vs_payload.svg",
    )
    maybe_matplotlib_png(rows, out_dir)

    for p in sorted(out_dir.glob("*.svg")) + sorted(out_dir.glob("*.png")):
        print(f"wrote {p}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
