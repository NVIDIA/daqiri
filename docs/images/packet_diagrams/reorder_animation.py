from __future__ import annotations

import math
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw

from anim_common import (
    CANVAS_FILL,
    CANVAS_HEIGHT,
    CANVAS_SCALE,
    CANVAS_WIDTH,
    FRAME_DURATION_MS,
    NIC_CORNER_RADIUS,
    PANEL_CORNER_RADIUS,
    PANEL_OUTLINE_WIDTH,
    PANEL_TITLE_X,
    PANEL_TITLE_Y,
    ROUTE_ARROW_SIZE,
    ROUTE_LINE_WIDTH,
    WIRE_DOT_COLOR,
    diagram_colors,
    font_scheme,
    output_paths,
    save_gif_animation,
    save_webp_animation,
)
from incoming_wire import draw_incoming_wire


ROOT = Path(__file__).resolve().parent

WIDTH = CANVAS_WIDTH
HEIGHT = CANVAS_HEIGHT
SCALE = CANVAS_SCALE
DURATION_MS = FRAME_DURATION_MS

PACKET_COUNT = 5
ARRIVAL_ORDER = (2, 0, 4, 1, 3)
WIRE_FRAMES = 34
STAGE_TRAVEL_FRAMES = 20
ARRIVAL_GAP = 16
KERNEL_DELAY = 22
WRITE_FRAMES = 42
WRITE_STAGGER = 2

NIC_RECT = (300, 250, 500, 410)
REORDER_RECT = (610, 150, 940, 546)
REORDER_STAGING_COL = (635, 260, 755, 512)
REORDER_QUEUE_COL = (790, 260, 910, 512)
CONVERT_STAGING_COL = REORDER_STAGING_COL
CONVERT_QUEUE_COL = REORDER_QUEUE_COL

WIRE_Y = 330

TRANSPARENT = (0, 0, 0, 0)

PACKET_GRAD_START = "#59d4ff"
PACKET_GRAD_END = "#9b8cff"

COLORS = diagram_colors(reorder="#76b900")


@dataclass(frozen=True)
class VisualConfig:
    output_dir: Path
    base_name: str
    title: str
    subtitle: str
    output_heading: str
    nic_idle: str
    nic_tracking_prefix: str
    staging_col: tuple[float, float, float, float]
    queue_col: tuple[float, float, float, float]
    convert_payload: bool = False


VISUALIZATIONS = (
    VisualConfig(
        output_dir=ROOT / "reorder",
        base_name="packet-reorder",
        title="GPU reorder",
        subtitle="batch staging -> fixed slots",
        output_heading="slot = seq % N",
        nic_idle="staging",
        nic_tracking_prefix="stage seq",
        staging_col=REORDER_STAGING_COL,
        queue_col=REORDER_QUEUE_COL,
    ),
    VisualConfig(
        output_dir=ROOT / "reorder_quantize",
        base_name="packet-reorder-quantize",
        title="GPU reorder + convert",
        subtitle="batch staging -> fixed slots\nint4 -> fp32 conversion",
        output_heading="fp32 slots",
        nic_idle="int4 staging",
        nic_tracking_prefix="stage int4 seq",
        staging_col=CONVERT_STAGING_COL,
        queue_col=CONVERT_QUEUE_COL,
        convert_payload=True,
    ),
)

CURRENT_VISUAL = VISUALIZATIONS[0]


@dataclass(frozen=True)
class PacketSpec:
    num: int
    start: int
    nic_arrive: int
    stage_end: int
    write_start: int
    write_end: int


NIC = NIC_RECT
REORDER = REORDER_RECT
NIC_CX = (NIC[0] + NIC[2]) / 2
NIC_CY = (NIC[1] + NIC[3]) / 2
REORDER_CY = (REORDER[1] + REORDER[3]) / 2
REORDER_ENTRY = (REORDER[0], NIC_CY)


def rgb(hex_color: str) -> tuple[int, int, int]:
    hex_color = hex_color.strip("#")
    return tuple(int(hex_color[i : i + 2], 16) for i in (0, 2, 4))


def rgb_to_hex(r: int, g: int, b: int) -> str:
    return f"#{r:02x}{g:02x}{b:02x}"


def rgba(hex_color: str, alpha: int = 255) -> tuple[int, int, int, int]:
    r, g, b = rgb(hex_color)
    return r, g, b, alpha


def lerp(a: float, b: float, t: float) -> float:
    return a + (b - a) * t


def lerp_hex(a: str, b: str, t: float) -> str:
    r1, g1, b1 = rgb(a)
    r2, g2, b2 = rgb(b)
    return rgb_to_hex(
        int(lerp(r1, r2, t)),
        int(lerp(g1, g2, t)),
        int(lerp(b1, b2, t)),
    )


def clamp(value: float, lo: float = 0.0, hi: float = 1.0) -> float:
    return max(lo, min(hi, value))


def progress_linear(frame: int, start: int, end: int) -> float:
    return clamp((frame - start) / max(1, end - start))


FONTS = font_scheme(SCALE)


def s(value: float) -> int:
    return int(round(value * SCALE))


def pt(point: tuple[float, float]) -> tuple[int, int]:
    return s(point[0]), s(point[1])


def box(rect: tuple[float, float, float, float]) -> tuple[int, int, int, int]:
    return tuple(s(v) for v in rect)


def draw_text(
    draw: ImageDraw.ImageDraw,
    xy: tuple[float, float],
    text: str,
    face: ImageFont.FreeTypeFont,
    fill: str | tuple[int, int, int, int] = COLORS["text"],
    anchor: str = "la",
) -> None:
    draw.text(pt(xy), text, font=face, fill=fill, anchor=anchor)


def centered_text(
    draw: ImageDraw.ImageDraw,
    rect: tuple[float, float, float, float],
    text: str,
    face: ImageFont.FreeTypeFont,
    fill: str | tuple[int, int, int, int] = COLORS["text"],
) -> None:
    x1, y1, x2, y2 = rect
    draw.text((s((x1 + x2) / 2), s((y1 + y2) / 2)), text, font=face, fill=fill, anchor="mm")


def centered_multiline_text(
    draw: ImageDraw.ImageDraw,
    rect: tuple[float, float, float, float],
    text: str,
    face: ImageFont.FreeTypeFont,
    fill: str | tuple[int, int, int, int] = COLORS["text"],
) -> None:
    x1, y1, x2, y2 = rect
    draw.multiline_text(
        (s((x1 + x2) / 2), s((y1 + y2) / 2)),
        text,
        font=face,
        fill=fill,
        anchor="mm",
        align="center",
        spacing=s(4),
    )


def rectangular_path(waypoints: tuple[tuple[float, float], ...], steps: int = 28) -> list[tuple[float, float]]:
    if len(waypoints) < 2:
        return list(waypoints)
    path: list[tuple[float, float]] = []
    for i in range(len(waypoints) - 1):
        x0, y0 = waypoints[i]
        x1, y1 = waypoints[i + 1]
        for step in range(steps + 1):
            if i > 0 and step == 0:
                continue
            t = step / steps
            path.append((lerp(x0, x1, t), lerp(y0, y1, t)))
    return path


def path_lengths(points: list[tuple[float, float]]) -> tuple[list[float], float]:
    lengths = [0.0]
    for i in range(1, len(points)):
        x0, y0 = points[i - 1]
        x1, y1 = points[i]
        lengths.append(lengths[-1] + math.hypot(x1 - x0, y1 - y0))
    return lengths, lengths[-1]


def draw_polyline(
    draw: ImageDraw.ImageDraw,
    points: list[tuple[float, float]],
    color: str | tuple[int, int, int, int],
    width: int = 4,
) -> None:
    draw.line([pt(p) for p in points], fill=color, width=s(width))


def draw_glow_line(
    base: Image.Image,
    points: list[tuple[float, float]],
    color: str,
    alpha: int,
    width: int,
) -> None:
    layer = Image.new("RGBA", base.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    draw_polyline(d, points, rgba(color, alpha), width)
    base.alpha_composite(layer)


def draw_arrow_head(
    draw: ImageDraw.ImageDraw,
    end: tuple[float, float],
    prev: tuple[float, float],
    color: str | tuple[int, int, int, int],
    size: float = 12,
) -> None:
    angle = math.atan2(end[1] - prev[1], end[0] - prev[0])
    wing = math.radians(28)
    p1 = (end[0] - size * math.cos(angle - wing), end[1] - size * math.sin(angle - wing))
    p2 = (end[0] - size * math.cos(angle + wing), end[1] - size * math.sin(angle + wing))
    draw.polygon([pt(end), pt(p1), pt(p2)], fill=color)


def draw_arrow(
    draw: ImageDraw.ImageDraw,
    points: list[tuple[float, float]],
    color: str | tuple[int, int, int, int],
    width: int = 4,
    arrow_size: float = 13,
) -> None:
    draw_polyline(draw, points, color, width)
    if len(points) >= 2:
        prev_idx = max(0, len(points) - 5)
        draw_arrow_head(draw, points[-1], points[prev_idx], color, arrow_size)


def point_on_path_distance(points: list[tuple[float, float]], dist: float) -> tuple[float, float]:
    if len(points) < 2:
        return points[0] if points else (0.0, 0.0)
    lengths, total = path_lengths(points)
    if total <= 0:
        return points[0]
    target = clamp(dist, 0.0, total)
    for i in range(1, len(points)):
        if lengths[i] >= target:
            seg_len = lengths[i] - lengths[i - 1]
            local = 0.0 if seg_len <= 0 else (target - lengths[i - 1]) / seg_len
            x0, y0 = points[i - 1]
            x1, y1 = points[i]
            return lerp(x0, x1, local), lerp(y0, y1, local)
    return points[-1]


def draw_path_arrows(
    draw: ImageDraw.ImageDraw,
    points: list[tuple[float, float]],
    color: str | tuple[int, int, int, int],
    *,
    width: int = 3,
    arrow_size: float = 11,
    spacing: float = 34,
) -> None:
    if len(points) < 2:
        return
    _, total = path_lengths(points)
    draw_polyline(draw, points, color, width)
    dist = spacing * 0.6
    while dist < total - arrow_size:
        tip = point_on_path_distance(points, dist)
        prev = point_on_path_distance(points, max(0.0, dist - 10))
        draw_arrow_head(draw, tip, prev, color, arrow_size)
        dist += spacing
    if total > arrow_size:
        draw_arrow_head(draw, points[-1], points[-2], color, arrow_size + 2)


def draw_glow_arrows(
    base: Image.Image,
    points: list[tuple[float, float]],
    color: str,
    alpha: int,
    width: int,
) -> None:
    layer = Image.new("RGBA", base.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    draw_path_arrows(d, points, rgba(color, alpha), width=width, arrow_size=11, spacing=32)
    base.alpha_composite(layer)


def column_row_layout(rect: tuple[float, float, float, float]) -> list[tuple[float, float, float, float]]:
    x1, y1, x2, y2 = rect
    cx = (x1 + x2) / 2
    pad_y = 6
    gap = 6
    inner_h = y2 - y1 - 2 * pad_y
    chip_h = (inner_h - gap * (PACKET_COUNT - 1)) / PACKET_COUNT
    chip_w = x2 - x1 - 8
    rows: list[tuple[float, float, float, float]] = []
    y = y1 + pad_y + chip_h / 2
    for _ in range(PACKET_COUNT):
        rows.append((cx, y, chip_w, chip_h))
        y += chip_h + gap
    return rows


def staging_col_rect() -> tuple[float, float, float, float]:
    return CURRENT_VISUAL.staging_col


def queue_col_rect() -> tuple[float, float, float, float]:
    return CURRENT_VISUAL.queue_col


def staging_row_pos(row: int) -> tuple[float, float, float, float]:
    return column_row_layout(staging_col_rect())[row]


def queue_row_pos(row: int) -> tuple[float, float, float, float]:
    return column_row_layout(queue_col_rect())[row]


def slot_index(num: int) -> int:
    return num % PACKET_COUNT


def packet_order_index(pkt: PacketSpec) -> int:
    return PACKETS.index(pkt)


def completed_nums(frame: int) -> list[int]:
    return [p.num for p in PACKETS if frame >= p.write_end]


def staged_packets(frame: int) -> list[PacketSpec]:
    return [p for p in PACKETS if p.stage_end <= frame < p.write_start]


def writing_packet(frame: int) -> PacketSpec | None:
    for pkt in PACKETS:
        if pkt.write_start <= frame < pkt.write_end:
            return pkt
    return None


def queue_display(frame: int) -> list[int | None]:
    row: list[int | None] = [None] * PACKET_COUNT
    for num in completed_nums(frame):
        row[slot_index(num)] = num
    return row


def packet_label(num: int, *, output: bool = False) -> str:
    if CURRENT_VISUAL.convert_payload:
        return f"fp32 s{num}" if output else f"int4 s{num}"
    return f"seq {num}"


def before_placement(pkt: PacketSpec) -> list[int]:
    return [p.num for p in PACKETS if p.write_end <= pkt.write_start]


def after_placement(pkt: PacketSpec) -> list[int]:
    return before_placement(pkt) + [pkt.num]


def placed_packet_position(frame: int, num: int) -> tuple[float, float, float, float] | None:
    if num not in completed_nums(frame):
        return None
    return queue_row_pos(slot_index(num))


def shuffle_entry_for_row(row: int) -> tuple[float, float]:
    _, cy, _, _ = staging_row_pos(row)
    return staging_col_rect()[0] + 12, cy


def build_paths() -> dict[str, list[tuple[float, float]]]:
    wire = rectangular_path(((48, WIRE_Y), (NIC[0], WIRE_Y)))
    nic_route = rectangular_path(((NIC[2], NIC_CY), REORDER_ENTRY))
    to_reorder = rectangular_path(
        (
            (48, WIRE_Y),
            (NIC[0], WIRE_Y),
            (NIC[2], NIC_CY),
            REORDER_ENTRY,
        )
    )
    return {
        "wire": wire,
        "to_reorder": to_reorder,
        "nic_route": nic_route,
    }


PATHS = build_paths()


def make_packets() -> tuple[PacketSpec, ...]:
    staged: list[tuple[int, int, int, int]] = []
    cursor = 8
    for num in ARRIVAL_ORDER:
        nic_arrive = cursor + WIRE_FRAMES
        stage_end = nic_arrive + STAGE_TRAVEL_FRAMES
        staged.append((num, cursor, nic_arrive, stage_end))
        cursor += ARRIVAL_GAP

    kernel_start = max(stage_end for _, _, _, stage_end in staged) + KERNEL_DELAY
    packets: list[PacketSpec] = []
    for idx, (num, start, nic_arrive, stage_end) in enumerate(staged):
        write_start = kernel_start + idx * WRITE_STAGGER
        write_end = write_start + WRITE_FRAMES
        packets.append(PacketSpec(num, start, nic_arrive, stage_end, write_start, write_end))
    return tuple(packets)


PACKETS = make_packets()
KERNEL_START = min(p.write_start for p in PACKETS)
KERNEL_END = max(p.write_end for p in PACKETS)
FRAMES = KERNEL_END + 40


def active_packet(frame: int) -> PacketSpec | None:
    for pkt in PACKETS:
        if pkt.nic_arrive <= frame < pkt.stage_end:
            return pkt
    return None


def nic_pulse(frame: int, pkt: PacketSpec) -> float:
    return max(
        progress_linear(frame, pkt.nic_arrive, pkt.nic_arrive + 6),
        1.0 - progress_linear(frame, pkt.nic_arrive + 8, pkt.nic_arrive + 14),
    )


def packet_base_color(num: int) -> str:
    if PACKET_COUNT <= 1:
        return PACKET_GRAD_START
    t = clamp(num / (PACKET_COUNT - 1))
    return lerp_hex(PACKET_GRAD_START, PACKET_GRAD_END, t)


def packet_gradient_ends(num: int) -> tuple[str, str]:
    base = packet_base_color(num)
    return lerp_hex(base, "#ffffff", 0.22), lerp_hex(base, PACKET_GRAD_END, 0.3)


def point_on_path(points: list[tuple[float, float]], t: float) -> tuple[float, float]:
    if len(points) < 2:
        return points[0] if points else (0.0, 0.0)
    t = clamp(t)
    lengths, total = path_lengths(points)
    if total <= 0:
        return points[0]
    target = t * total
    for i in range(1, len(points)):
        if lengths[i] >= target:
            seg_len = lengths[i] - lengths[i - 1]
            local = 0.0 if seg_len <= 0 else (target - lengths[i - 1]) / seg_len
            x0, y0 = points[i - 1]
            x1, y1 = points[i]
            return lerp(x0, x1, local), lerp(y0, y1, local)
    return points[-1]


def stage_path(pkt: PacketSpec) -> list[tuple[float, float]]:
    _, sy, _, _ = staging_row_pos(packet_order_index(pkt))
    staging_col = staging_col_rect()
    return rectangular_path(
        (
            (NIC[2], NIC_CY),
            (REORDER[0], NIC_CY),
            (staging_col[0] + 10, sy),
            ((staging_col[0] + staging_col[2]) / 2, sy),
        ),
        steps=16,
    )


def packet_stage_path(pkt: PacketSpec) -> list[tuple[float, float]]:
    _, sy, _, _ = staging_row_pos(packet_order_index(pkt))
    staging_col = staging_col_rect()
    return rectangular_path(
        (
            (48, WIRE_Y),
            (NIC[0], WIRE_Y),
            (NIC[2], NIC_CY),
            (REORDER[0], NIC_CY),
            (staging_col[0] + 10, sy),
            ((staging_col[0] + staging_col[2]) / 2, sy),
        ),
        steps=16,
    )


def write_path(pkt: PacketSpec) -> list[tuple[float, float]]:
    sx, sy, _, _ = staging_row_pos(packet_order_index(pkt))
    tx, ty, _, _ = queue_row_pos(slot_index(pkt.num))
    return rectangular_path(((sx, sy), (tx, ty)), steps=18)


def packet_screen_pos(frame: int, pkt: PacketSpec) -> tuple[float, float, float] | None:
    if frame < pkt.start or frame >= pkt.write_end:
        return None
    width = 92.0
    if frame < pkt.stage_end:
        t = progress_linear(frame, pkt.start, pkt.stage_end)
        cx, cy = point_on_path(packet_stage_path(pkt), t)
        return cx, cy, width
    if frame < pkt.write_start:
        return None
    t = progress_linear(frame, pkt.write_start, pkt.write_end)
    sx, sy, sw, _ = staging_row_pos(packet_order_index(pkt))
    tx, ty, tw, _ = queue_row_pos(slot_index(pkt.num))
    return lerp(sx, tx, t), lerp(sy, ty, t), lerp(sw - 6, tw - 6, t)


def received_count(frame: int) -> int:
    return sum(1 for p in PACKETS if frame >= p.stage_end)


def draw_gradient_packet(
    base: Image.Image,
    draw: ImageDraw.ImageDraw,
    cx: float,
    cy: float,
    width: float,
    num: int,
    alpha: int = 255,
    *,
    label: str | None = None,
    height: float = 28,
) -> None:
    h = height
    x1, y1 = cx - width / 2, cy - h / 2
    x2, y2 = cx + width / 2, cy + h / 2
    radius = s(6)
    left, right = packet_gradient_ends(num)
    accent = packet_base_color(num)

    chip = Image.new("RGBA", (max(1, s(x2 - x1)), max(1, s(y2 - y1))), (0, 0, 0, 0))
    cd = ImageDraw.Draw(chip)
    inset = s(1)
    cw, ch = chip.size
    steps = 16
    for i in range(steps):
        t0 = i / steps
        t1 = (i + 1) / steps
        color = lerp_hex(left, right, (t0 + t1) / 2)
        sx1 = int(inset + (cw - 2 * inset) * t0)
        sx2 = int(inset + (cw - 2 * inset) * t1)
        cd.rectangle((sx1, inset, sx2, ch - inset), fill=rgba(color, alpha))
    mask = Image.new("L", chip.size, 0)
    md = ImageDraw.Draw(mask)
    md.rounded_rectangle((0, 0, cw - 1, ch - 1), radius=radius, fill=255)
    chip.putalpha(mask)
    base.paste(chip, (s(x1), s(y1)), chip)
    draw.rounded_rectangle(
        box((x1, y1, x2, y2)),
        radius=radius,
        outline=rgba(accent, min(255, 170 + alpha // 3)),
        width=s(2),
    )
    if label:
        if len(label) > 10:
            face = FONTS["tiny"]
        else:
            face = FONTS["packet"]
        centered_text(draw, (x1, y1, x2, y2), label, face, fill=rgba(COLORS["ink"], alpha))


def draw_wires(draw: ImageDraw.ImageDraw, frame: int) -> None:
    draw_incoming_wire(
        draw,
        frame,
        WIRE_Y,
        NIC[0],
        str(COLORS["wire"]),
        WIRE_DOT_COLOR,
        COLORS["canvas_text"],
        FONTS["label"],
        pt,
        s,
        rgba,
        box,
    )


def draw_static_route(draw: ImageDraw.ImageDraw) -> None:
    draw_arrow(draw, PATHS["nic_route"], rgba(COLORS["route"], 255), ROUTE_LINE_WIDTH, ROUTE_ARROW_SIZE)


def draw_sequence_queue(base: Image.Image, draw: ImageDraw.ImageDraw, frame: int) -> None:
    row = queue_display(frame)
    for idx, value in enumerate(row):
        cx, cy, chip_w, chip_h = queue_row_pos(idx)
        slot_box = (cx - chip_w / 2, cy - chip_h / 2, cx + chip_w / 2, cy + chip_h / 2)
        draw.rounded_rectangle(
            box(slot_box),
            radius=s(7),
            fill=COLORS["row_bg"],
            outline=rgba(COLORS["line"], 210),
            width=s(1),
        )
        if value is None:
            centered_text(draw, slot_box, f"slot {idx}", FONTS["slot"], fill=COLORS["slot_empty"])
            continue
        draw_gradient_packet(
            base,
            draw,
            cx,
            cy,
            chip_w - 6,
            value,
            label=packet_label(value, output=True),
            height=chip_h - 6,
        )


def draw_staging_list(base: Image.Image, draw: ImageDraw.ImageDraw, frame: int) -> None:
    staged_by_row = {packet_order_index(pkt): pkt for pkt in staged_packets(frame)}
    for idx in range(PACKET_COUNT):
        cx, cy, chip_w, chip_h = staging_row_pos(idx)
        row_box = (cx - chip_w / 2, cy - chip_h / 2, cx + chip_w / 2, cy + chip_h / 2)
        draw.rounded_rectangle(
            box(row_box),
            radius=s(7),
            fill=COLORS["row_bg"],
            outline=rgba(COLORS["line"], 190),
            width=s(1),
        )
        pkt = staged_by_row.get(idx)
        if pkt is None:
            centered_text(draw, row_box, "ptr", FONTS["tiny"], fill=COLORS["slot_empty"])
            continue
        draw_gradient_packet(base, draw, cx, cy, chip_w - 6, pkt.num, label=packet_label(pkt.num), height=chip_h - 6)


def draw_reorder_panel(base: Image.Image, draw: ImageDraw.ImageDraw, frame: int) -> None:
    x1, y1, _, _ = REORDER
    staging_col = staging_col_rect()
    queue_col = queue_col_rect()
    kernel_active = KERNEL_START <= frame < KERNEL_END
    pulse = 0.55 if kernel_active else 0.0
    glow = int(40 + 55 * pulse)
    draw.rounded_rectangle(box(REORDER), radius=s(PANEL_CORNER_RADIUS), fill=COLORS["panel_2"], outline=rgba(COLORS["reorder"], 170 + glow), width=s(PANEL_OUTLINE_WIDTH))
    draw_text(draw, (x1 + PANEL_TITLE_X, y1 + PANEL_TITLE_Y), CURRENT_VISUAL.title, FONTS["label"], fill=COLORS["text"], anchor="lt")
    for idx, subtitle in enumerate(CURRENT_VISUAL.subtitle.splitlines()):
        draw_text(draw, (x1 + PANEL_TITLE_X, y1 + PANEL_TITLE_Y + 24 + idx * 17), subtitle, FONTS["small"], fill=COLORS["muted"], anchor="lt")
    draw_text(draw, (staging_col[0], staging_col[1] - 20), "staged ptrs", FONTS["tiny"], fill=COLORS["muted"], anchor="lt")
    draw_text(draw, (queue_col[0], queue_col[1] - 20), CURRENT_VISUAL.output_heading, FONTS["tiny"], fill=COLORS["muted"], anchor="lt")
    draw_staging_list(base, draw, frame)
    draw_sequence_queue(base, draw, frame)


def nic_status(frame: int) -> tuple[str, str, str, float]:
    pkt = active_packet(frame)
    tracking = pkt is not None
    pulse = nic_pulse(frame, pkt) if tracking and pkt else 0.0
    if tracking and pkt:
        if CURRENT_VISUAL.convert_payload:
            return f"stage int4\nseq {pkt.num}", packet_base_color(pkt.num), "#ffffff", pulse
        return f"stage seq\n{pkt.num}", packet_base_color(pkt.num), "#ffffff", pulse

    count = received_count(frame)
    if count:
        return f"staged\n{count}/{PACKET_COUNT}", COLORS["nvidia"], COLORS["ink"], pulse
    idle = "int4\nstaging" if CURRENT_VISUAL.convert_payload else CURRENT_VISUAL.nic_idle
    return idle, COLORS["nvidia"], COLORS["ink"], pulse


def draw_nic_status_pill(draw: ImageDraw.ImageDraw, frame: int) -> None:
    x1, y1, x2, _ = NIC
    pill_text, pill_accent, pill_ink, pulse = nic_status(frame)
    pill = (x1 + 12, y1 + 38, x2 - 12, y1 + 122)
    draw.rounded_rectangle(
        box(pill),
        radius=s(8),
        fill=rgba(pill_accent, 245),
        outline=rgba(pill_accent, 220),
        width=s(2),
    )
    centered_multiline_text(draw, pill, pill_text, FONTS["nic_center"], fill=pill_ink)


def draw_nic_chip_base(draw: ImageDraw.ImageDraw, frame: int) -> None:
    x1, y1, x2, y2 = NIC
    pkt = active_packet(frame)
    tracking = pkt is not None
    pulse = nic_pulse(frame, pkt) if tracking and pkt else 0.0
    accent = COLORS["nvidia"]

    glow_alpha = int(55 + 120 * pulse) if tracking else 55
    draw.rounded_rectangle(box((x1 - 10, y1 - 10, x2 + 10, y2 + 10)), radius=s(24), fill=rgba(accent, 16 + glow_alpha // 3))
    draw.rounded_rectangle(box(NIC), radius=s(NIC_CORNER_RADIUS), fill=COLORS["nic_panel"], outline=rgba(accent, 190 + int(65 * pulse)), width=s(3 + int(2 * pulse)))
    for i in range(7):
        y = lerp(y1 + 18, y2 - 18, i / 6)
        draw.line((s(x1 - 14), s(y), s(x1), s(y)), fill=rgba(COLORS["nic_line"], 220), width=s(3))
        draw.line((s(x2), s(y), s(x2 + 14), s(y)), fill=rgba(COLORS["nic_line"], 220), width=s(3))
    for i in range(5):
        x = lerp(x1 + 25, x2 - 25, i / 4)
        draw.line((s(x), s(y1 - 12), s(x), s(y1)), fill=rgba(COLORS["nic_line"], 220), width=s(3))
        draw.line((s(x), s(y2), s(x), s(y2 + 12)), fill=rgba(COLORS["nic_line"], 220), width=s(3))
    centered_text(draw, (x1 + 18, y1 + 4, x2 - 18, y1 + 38), "NVIDIA NIC", FONTS["chip"], fill=COLORS["nic_text"])


def draw_flowing_packets(base: Image.Image, draw: ImageDraw.ImageDraw, frame: int) -> None:
    for pkt in PACKETS:
        if frame < pkt.start or frame >= pkt.write_end:
            continue
        pos = packet_screen_pos(frame, pkt)
        if pos is None:
            continue
        cx, cy, w = pos
        if frame < pkt.stage_end:
            chip_h = 32
            label = None if NIC[0] <= cx <= NIC[2] and NIC[1] <= cy <= NIC[3] else packet_label(pkt.num)
        else:
            _, _, _, chip_h = queue_row_pos(slot_index(pkt.num))
            chip_h -= 6
            label = packet_label(pkt.num, output=frame >= pkt.write_end - 4)
        draw_gradient_packet(
            base,
            draw,
            cx,
            cy,
            w,
            pkt.num,
            label=label,
            height=chip_h,
        )


def draw_route_glow(base: Image.Image, frame: int) -> None:
    for pkt in PACKETS:
        if pkt.nic_arrive <= frame < pkt.stage_end:
            glow = progress_linear(frame, pkt.nic_arrive, pkt.stage_end)
            draw_glow_line(base, stage_path(pkt), COLORS["reorder"], int(45 + 45 * glow), 6)
        elif pkt.write_start <= frame < pkt.write_end:
            glow = progress_linear(frame, pkt.write_start, pkt.write_end)
            draw_glow_line(base, write_path(pkt), COLORS["reorder"], int(45 + 45 * glow), 5)
            if CURRENT_VISUAL.convert_payload:
                sx, sy, _, _ = staging_row_pos(packet_order_index(pkt))
                tx, ty, _, _ = queue_row_pos(slot_index(pkt.num))
                draw = ImageDraw.Draw(base)
                draw_text(
                    draw,
                    (lerp(sx, tx, glow), lerp(sy, ty, glow) - 24),
                    "int4 -> fp32",
                    FONTS["tiny"],
                    fill=COLORS["reorder"],
                    anchor="mm",
                )


def render_frame(frame: int) -> Image.Image:
    img = Image.new("RGBA", (WIDTH * SCALE, HEIGHT * SCALE), CANVAS_FILL)
    draw = ImageDraw.Draw(img)
    draw_wires(draw, frame)
    draw_static_route(draw)
    draw_reorder_panel(img, draw, frame)
    draw_nic_chip_base(draw, frame)
    draw_route_glow(img, frame)
    draw_flowing_packets(img, draw, frame)
    draw_nic_status_pill(draw, frame)
    return img


def render(visual: VisualConfig) -> None:
    global CURRENT_VISUAL
    CURRENT_VISUAL = visual
    animation_path, gif_path, poster_path = output_paths(visual.output_dir, visual.base_name)

    visual.output_dir.mkdir(parents=True, exist_ok=True)
    frames = [render_frame(i) for i in range(FRAMES)]
    save_webp_animation(frames, animation_path, DURATION_MS)
    save_gif_animation(frames, gif_path, DURATION_MS)
    poster_frame = min(FRAMES - 1, KERNEL_END + 12)
    render_frame(poster_frame).save(poster_path, optimize=True)
    print(f"Wrote {animation_path}")
    print(f"Wrote {gif_path}")
    print(f"Wrote {poster_path}")


def main() -> None:
    for visual in VISUALIZATIONS:
        render(visual)


if __name__ == "__main__":
    main()
