from __future__ import annotations

import math
from typing import Callable

from PIL import ImageDraw

RX_WIRE_LABEL = "RX Packets over Wire"
WIRE_LABEL_OFFSET = 95.0

PtFn = Callable[[tuple[float, float]], tuple[int, int]]
ScaleFn = Callable[[float], int]
RgbaFn = Callable[[str, int], tuple[int, int, int, int]]
BoxFn = Callable[[tuple[float, float, float, float]], tuple[int, int, int, int]]


def center_wire_y(x: float, x0: float, y_center: float, frame: int, *, wave: bool = False) -> float:
    if not wave:
        return y_center
    i = (x - x0) / 3.55
    return y_center + math.sin((i + frame * 0.95) / 5.4) * 2.4


def draw_rx_wire_label(
    draw_text: Callable[..., None],
    wire_y: float,
    *,
    x: float = 48.0,
    text: str = RX_WIRE_LABEL,
) -> None:
    draw_text((x, wire_y - WIRE_LABEL_OFFSET), text)


def draw_rx_wire(
    draw: ImageDraw.ImageDraw,
    frame: int,
    x0: float,
    x1: float,
    y_center: float,
    wire_color: str,
    marker_color: str,
    pt: PtFn,
    s: ScaleFn,
    rgba: RgbaFn,
    box: BoxFn,
    *,
    ambient_markers: bool = True,
    animate_markers: bool = False,
    wave: bool = False,
) -> None:
    span = max(x1 - x0, 1.0)
    count = max(20, int(span / 3.55) + 1)
    for offset in (-13, 0, 13):
        pts: list[tuple[float, float]] = []
        for i in range(count):
            x = x0 + i * (span / max(1, count - 1))
            wave_offset = math.sin((i + frame * 0.95) / 5.4) * 2.4 if wave else 0.0
            pts.append((x, y_center + offset + wave_offset))
        draw.line([pt(p) for p in pts], fill=wire_color, width=s(2))

    if not ambient_markers:
        return
    loop = max(44.0, span - 44.0)
    marker_frame = frame if animate_markers else 0
    for i in range(5):
        x = x0 + 22 + ((marker_frame * 7 + i * 68) % loop)
        cy = center_wire_y(x, x0, y_center, frame, wave=wave)
        alpha = 160 - i * 16
        draw.ellipse(box((x - 4, cy - 4, x + 4, cy + 4)), fill=rgba(marker_color, alpha))


def draw_rx_packet_marker(
    draw: ImageDraw.ImageDraw,
    frame: int,
    cx: float,
    wire_y: float,
    wire_x0: float,
    color: str,
    rgba: RgbaFn,
    box: BoxFn,
    *,
    radius: float = 5.0,
    alpha: int = 255,
    snap_to_wire: bool = True,
    cy: float | None = None,
    wave: bool = False,
) -> None:
    marker_y = center_wire_y(cx, wire_x0, wire_y, frame, wave=wave) if snap_to_wire else (cy if cy is not None else wire_y)
    draw.ellipse(
        box((cx - radius, marker_y - radius, cx + radius, marker_y + radius)),
        fill=rgba(color, alpha),
    )


def path_lengths(points: list[tuple[float, float]]) -> tuple[list[float], float]:
    lengths = [0.0]
    for i in range(1, len(points)):
        x0, y0 = points[i - 1]
        x1, y1 = points[i]
        lengths.append(lengths[-1] + math.hypot(x1 - x0, y1 - y0))
    return lengths, lengths[-1]


def path_point_and_tangent(
    points: list[tuple[float, float]],
    t: float,
) -> tuple[float, float, float]:
    if len(points) < 2:
        p = points[0] if points else (0.0, 0.0)
        return p[0], p[1], 0.0
    t = max(0.0, min(1.0, t))
    lengths, total = path_lengths(points)
    if total <= 0:
        return points[0][0], points[0][1], 0.0
    target = t * total
    for i in range(1, len(points)):
        if lengths[i] >= target:
            seg_len = lengths[i] - lengths[i - 1]
            local = 0.0 if seg_len <= 0 else (target - lengths[i - 1]) / seg_len
            x0, y0 = points[i - 1]
            x1, y1 = points[i]
            cx = x0 + (x1 - x0) * local
            cy = y0 + (y1 - y0) * local
            return cx, cy, math.atan2(y1 - y0, x1 - x0)
    x0, y0 = points[-2]
    x1, y1 = points[-1]
    return x1, y1, math.atan2(y1 - y0, x1 - x0)


def wire_t_end(wire: list[tuple[float, float]], full: list[tuple[float, float]]) -> float:
    _, wire_total = path_lengths(wire)
    _, full_total = path_lengths(full)
    if full_total <= 0:
        return 1.0
    return min(1.0, wire_total / full_total)
