from __future__ import annotations

import math
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw

from anim_common import font_scheme, save_webp_animation
from incoming_wire import draw_rx_wire, draw_rx_wire_label


ROOT = Path(__file__).resolve().parent
OUTPUT_DIR = ROOT / "hds"

WIDTH = 1180
HEIGHT = 660
SCALE = 2
DURATION_MS = 40

TOTAL_BYTES = 1064
HEADER_BYTES = 64
PAYLOAD_BYTES = 1000

PACKET_COUNT = 5
WIRE_Y = 330
ARRIVAL_GAP = 42
PIXELS_PER_FRAME = 12.0
PACKET_BAR_WIDTH = 132
PACKET_BAR_HEIGHT = 34
HEADER_SEGMENT_WIDTH = 68
PAYLOAD_SEGMENT_WIDTH = 190
PAYLOAD_SEGMENT_HEIGHT = 38
HEADER_SEGMENT_HEIGHT = PAYLOAD_SEGMENT_HEIGHT
SLOT_GAP = 8

LAYOUT_SHIFT_Y = 0


def dy(y: float) -> float:
    return y + LAYOUT_SHIFT_Y


def shift_rect(rect: tuple[float, float, float, float]) -> tuple[float, float, float, float]:
    x1, y1, x2, y2 = rect
    return x1, y1 + LAYOUT_SHIFT_Y, x2, y2 + LAYOUT_SHIFT_Y


NIC_RECT = shift_rect((300, 250, 500, 410))
HOST_RECT = shift_rect((744, 74, 1150, 234))
GPU_RECT = shift_rect((770, 324, 1110, 610))
KERNEL_WIDTH = 130
KERNEL_CX = NIC_RECT[2] + 102
HEADER_SLOT_AREA = shift_rect((756, 128, 1140, 180))
PAYLOAD_SLOT_AREA = shift_rect((832, 370, 1048, 604))
KERNEL_BOX = shift_rect((KERNEL_CX - KERNEL_WIDTH / 2, 22, KERNEL_CX + KERNEL_WIDTH / 2, 92))

TRANSPARENT = (0, 0, 0, 0)

ACCENTS = {
    "nvidia": "#76b900",
    "header": "#ffcf5a",
    "payload": "#78e08f",
    "host": "#9b8cff",
    "gpu": "#76b900",
    "kernel": "#64748b",
    "ink": "#07111f",
}

THEMES: dict[str, dict[str, str | None]] = {
    "default": {
        **ACCENTS,
        "bg": None,
        "panel": "#0d1b2e",
        "panel_2": "#101f34",
        "line": "#35516f",
        "line_soft": "#20384f",
        "text": "#f6fbff",
        "muted": "#afc0cf",
        "wire": "#6d7e92",
        "white": "#ffffff",
        "shadow": "#03101d",
        "row_bg": "#061220",
        "kernel_fill": "#0f2744",
        "arrow": "#ffffff",
        "stroke": "#ffffff",
        "bar_glow": "#ffffff",
        "bypass": "#afc0cf",
    },
    "light": {
        **ACCENTS,
        "bg": "#ffffff",
        "panel": "#f5f5f5",
        "panel_2": "#eeeeee",
        "line": "#1a1a1a",
        "line_soft": "#cccccc",
        "text": "#1a1a1a",
        "muted": "#404040",
        "wire": "#404040",
        "white": "#ffffff",
        "shadow": "#bdbdbd",
        "row_bg": "#e8e8e8",
        "kernel_fill": "#f0f4ff",
        "arrow": "#1a1a1a",
        "stroke": "#1a1a1a",
        "bar_glow": "#cccccc",
        "bypass": "#404040",
    },
}

THEME_OUTPUT_SUFFIX = {
    "default": "",
    "light": "-light",
}

COLORS = THEMES["default"]


@dataclass(frozen=True)
class PacketSpec:
    num: int
    start: int
    nic_arrive: int
    header_start: int
    header_end: int
    payload_start: int
    payload_end: int


def make_packets() -> tuple[PacketSpec, ...]:
    packets: list[PacketSpec] = []
    cursor = 8
    for num in range(PACKET_COUNT):
        header_start = cursor + frames_for_path(segment_wire_path("header"))
        nic_arrive = cursor + frames_for_path(rectangular_path((header_wire_start(), (NIC_RECT[0], dy(WIRE_Y)))))
        payload_start = header_start
        header_end = header_start + frames_for_path(segment_post_split_path("header", num))
        payload_end = payload_start + frames_for_path(segment_post_split_path("payload", num))
        packets.append(PacketSpec(num, cursor, nic_arrive, header_start, header_end, payload_start, payload_end))
        cursor += ARRIVAL_GAP
    return tuple(packets)


def rgb(hex_color: str) -> tuple[int, int, int]:
    hex_color = hex_color.strip("#")
    return tuple(int(hex_color[i : i + 2], 16) for i in (0, 2, 4))


def rgba(hex_color: str, alpha: int = 255) -> tuple[int, int, int, int]:
    r, g, b = rgb(hex_color)
    return r, g, b, alpha


FONTS = font_scheme(SCALE)


def s(value: float) -> int:
    return int(round(value * SCALE))


def pt(point: tuple[float, float]) -> tuple[int, int]:
    return s(point[0]), s(point[1])


def box(rect: tuple[float, float, float, float]) -> tuple[int, int, int, int]:
    return tuple(s(v) for v in rect)


def text_size(draw: ImageDraw.ImageDraw, text: str, face: ImageFont.FreeTypeFont) -> tuple[int, int]:
    bbox = draw.textbbox((0, 0), text, font=face)
    return bbox[2] - bbox[0], bbox[3] - bbox[1]


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


def lerp(a: float, b: float, t: float) -> float:
    return a + (b - a) * t


def clamp(value: float, lo: float = 0.0, hi: float = 1.0) -> float:
    return max(lo, min(hi, value))


def smoothstep(t: float) -> float:
    t = clamp(t)
    return t * t * (3 - 2 * t)


def progress(frame: int, start: int, end: int) -> float:
    return smoothstep((frame - start) / max(1, end - start))


def progress_linear(frame: int, start: int, end: int) -> float:
    return clamp((frame - start) / max(1, end - start))


def bezier_point(
    p0: tuple[float, float],
    p1: tuple[float, float],
    p2: tuple[float, float],
    p3: tuple[float, float],
    t: float,
) -> tuple[float, float]:
    u = 1 - t
    x = u**3 * p0[0] + 3 * u**2 * t * p1[0] + 3 * u * t**2 * p2[0] + t**3 * p3[0]
    y = u**3 * p0[1] + 3 * u**2 * t * p1[1] + 3 * u * t**2 * p2[1] + t**3 * p3[1]
    return x, y


def bezier_path(points: tuple[tuple[float, float], ...], steps: int = 80) -> list[tuple[float, float]]:
    p0, p1, p2, p3 = points
    return [bezier_point(p0, p1, p2, p3, i / steps) for i in range(steps + 1)]


def rectangular_path(waypoints: tuple[tuple[float, float], ...], steps: int = 14) -> list[tuple[float, float]]:
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


def draw_polyline(
    draw: ImageDraw.ImageDraw,
    points: list[tuple[float, float]],
    color: str | tuple[int, int, int, int],
    width: int = 4,
) -> None:
    draw.line([pt(p) for p in points], fill=color, width=s(width))


def draw_dotted_segment(
    draw: ImageDraw.ImageDraw,
    start: tuple[float, float],
    end: tuple[float, float],
    color: str | tuple[int, int, int, int],
    *,
    width: int = 2,
    dash: float = 7,
    gap: float = 5,
) -> None:
    x0, y0 = start
    x1, y1 = end
    length = math.hypot(x1 - x0, y1 - y0)
    if length == 0:
        return
    dx = (x1 - x0) / length
    dy = (y1 - y0) / length
    pos = 0.0
    while pos < length:
        seg_end = min(pos + dash, length)
        draw.line(
            (pt((x0 + dx * pos, y0 + dy * pos)), pt((x0 + dx * seg_end, y0 + dy * seg_end))),
            fill=color,
            width=s(width),
        )
        pos += dash + gap


def draw_dotted_polyline(
    draw: ImageDraw.ImageDraw,
    points: list[tuple[float, float]],
    color: str | tuple[int, int, int, int],
    *,
    width: int = 2,
    dash: float = 8,
    gap: float = 6,
) -> None:
    if len(points) < 2:
        return
    drawing = True
    remaining = dash
    for i in range(len(points) - 1):
        x0, y0 = points[i]
        x1, y1 = points[i + 1]
        seg_len = math.hypot(x1 - x0, y1 - y0)
        if seg_len == 0:
            continue
        dx = (x1 - x0) / seg_len
        dy = (y1 - y0) / seg_len
        pos = 0.0
        while pos < seg_len:
            step = min(remaining, seg_len - pos)
            if drawing:
                draw.line(
                    (
                        pt((x0 + dx * pos, y0 + dy * pos)),
                        pt((x0 + dx * (pos + step), y0 + dy * (pos + step))),
                    ),
                    fill=color,
                    width=s(width),
                )
            pos += step
            remaining -= step
            if remaining <= 0:
                drawing = not drawing
                remaining = dash if drawing else gap


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


def path_lengths(points: list[tuple[float, float]]) -> tuple[list[float], float]:
    lengths = [0.0]
    for i in range(1, len(points)):
        x0, y0 = points[i - 1]
        x1, y1 = points[i]
        lengths.append(lengths[-1] + math.hypot(x1 - x0, y1 - y0))
    return lengths, lengths[-1]


def path_total(points: list[tuple[float, float]]) -> float:
    return path_lengths(points)[1]


def frames_for_path(points: list[tuple[float, float]]) -> int:
    return max(1, int(math.ceil(path_total(points) / PIXELS_PER_FRAME)))


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


def rect_at_center(cx: float, cy: float, w: float, h: float) -> tuple[float, float, float, float]:
    return cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2


def draw_packet_bar(
    draw: ImageDraw.ImageDraw,
    x: float,
    y: float,
    w: float,
    h: float,
    alpha: int = 255,
    label: bool = True,
    packet_num: int | None = None,
) -> None:
    draw.rounded_rectangle(box((x - 3, y - 3, x + w + 3, y + h + 3)), radius=s(11), fill=rgba(str(COLORS["bar_glow"]), 22 * alpha // 255))
    header_w = min(HEADER_SEGMENT_WIDTH, w * 0.45)
    outer = (x, y, x + w, y + h)
    payload_rect = (x + header_w, y, x + w, y + h)
    header_rect = (x, y, x + header_w, y + h)
    draw.rounded_rectangle(box(outer), radius=s(9), fill=rgba(COLORS["payload"], alpha), outline=rgba(COLORS["ink"], alpha), width=s(2))
    draw.rectangle(box(header_rect), fill=rgba(COLORS["header"], alpha))
    draw.line((s(x + header_w), s(y + 2), s(x + header_w), s(y + h - 2)), fill=rgba(COLORS["ink"], alpha), width=s(1))
    if label:
        centered_text(draw, payload_rect, "Payload", FONTS["tiny"], fill=rgba(COLORS["ink"], alpha))
    if packet_num is not None:
        draw_text(draw, (x + w / 2, y - 5), f"P{packet_num}", FONTS["tiny"], fill=COLORS["text"], anchor="mb")


def segment_slot_rect(
    area: tuple[float, float, float, float],
    idx: int,
    width: float,
    height: float,
    orientation: str,
) -> tuple[float, float, float, float]:
    x1, y1, x2, y2 = area
    if orientation == "horizontal":
        total_w = PACKET_COUNT * width + (PACKET_COUNT - 1) * SLOT_GAP
        left = x1 + (x2 - x1 - total_w) / 2
        x = left + idx * (width + SLOT_GAP)
        cy = (y1 + y2) / 2
        return rect_at_center(x + width / 2, cy, width, height)

    total_h = PACKET_COUNT * height + (PACKET_COUNT - 1) * SLOT_GAP
    top = y1 + (y2 - y1 - total_h) / 2
    y = top + idx * (height + SLOT_GAP)
    cx = (x1 + x2) / 2
    return rect_at_center(cx, y + height / 2, width, height)


def rect_center(rect: tuple[float, float, float, float]) -> tuple[float, float]:
    x1, y1, x2, y2 = rect
    return (x1 + x2) / 2, (y1 + y2) / 2


def header_slot_rect(idx: int) -> tuple[float, float, float, float]:
    return segment_slot_rect(HEADER_SLOT_AREA, idx, HEADER_SEGMENT_WIDTH, HEADER_SEGMENT_HEIGHT, "horizontal")


def payload_slot_rect(idx: int) -> tuple[float, float, float, float]:
    return segment_slot_rect(PAYLOAD_SLOT_AREA, idx, PAYLOAD_SEGMENT_WIDTH, PAYLOAD_SEGMENT_HEIGHT, "vertical")


def draw_segment_slots(
    draw: ImageDraw.ImageDraw,
    area: tuple[float, float, float, float],
    color: str,
    completed_slots: set[int],
    width: float,
    height: float,
    slot_label_prefix: str,
    orientation: str,
) -> None:
    draw.rounded_rectangle(box(area), radius=s(8), fill=rgba(COLORS["row_bg"], 235), outline=rgba(color, 145), width=s(2))

    for idx in range(PACKET_COUNT):
        slot = segment_slot_rect(area, idx, width, height, orientation)
        label = f"{slot_label_prefix} {idx + 1}"
        if idx in completed_slots:
            draw_packet_segment(draw, rect_center(slot), width, height, color, label)
            continue
        draw.rounded_rectangle(
            box(slot),
            radius=s(7),
            fill=rgba(COLORS["panel"], 180),
            outline=rgba(COLORS["line"], 170),
            width=s(1),
        )
        centered_text(draw, slot, label, FONTS["slot"], fill=COLORS["muted"])


def draw_packet_segment(
    draw: ImageDraw.ImageDraw,
    center: tuple[float, float],
    width: float,
    height: float,
    color: str,
    label: str,
    alpha: int = 255,
) -> None:
    cx, cy = center
    rect = rect_at_center(cx, cy, width, height)
    draw.rounded_rectangle(box(rect), radius=s(9), fill=rgba(color, alpha), outline=rgba(str(COLORS["stroke"]), 70 * alpha // 255), width=s(1))
    draw_text(draw, (cx, cy), label, FONTS["packet"], fill=rgba(COLORS["ink"], alpha), anchor="mm")


def draw_path_tag(
    draw: ImageDraw.ImageDraw,
    xy: tuple[float, float],
    text: str,
    color: str,
    *,
    anchor: str = "mm",
) -> None:
    tx, ty = xy
    tw, th = text_size(draw, text, FONTS["tiny"])
    pad_x = 8
    pad_y = 5
    x1 = tx - tw / (2 * SCALE) - pad_x if anchor == "mm" else tx
    y1 = ty - th / (2 * SCALE) - pad_y if anchor == "mm" else ty
    tag = (x1, y1, x1 + tw / SCALE + pad_x * 2, y1 + th / SCALE + pad_y * 2)
    draw.rounded_rectangle(box(tag), radius=s(6), fill=rgba(COLORS["panel"], 235), outline=rgba(color, 180), width=s(1))
    centered_text(draw, tag, text, FONTS["tiny"], fill=COLORS["text"])


def draw_nic_chip_base(
    draw: ImageDraw.ImageDraw,
    rect: tuple[float, float, float, float],
    accent: str,
    pulse: float = 0.0,
) -> None:
    x1, y1, x2, y2 = rect
    glow_alpha = int(55 + 95 * pulse)
    draw.rounded_rectangle(box((x1 - 10, y1 - 10, x2 + 10, y2 + 10)), radius=s(24), fill=rgba(accent, 16 + glow_alpha // 4))
    draw.rounded_rectangle(box(rect), radius=s(18), fill=COLORS["panel"], outline=rgba(accent, 190), width=s(3))
    for i in range(7):
        y = lerp(y1 + 18, y2 - 18, i / 6)
        draw.line((s(x1 - 14), s(y), s(x1), s(y)), fill=rgba(COLORS["line"], 220), width=s(3))
        draw.line((s(x2), s(y), s(x2 + 14), s(y)), fill=rgba(COLORS["line"], 220), width=s(3))
    for i in range(5):
        x = lerp(x1 + 25, x2 - 25, i / 4)
        draw.line((s(x), s(y1 - 12), s(x), s(y1)), fill=rgba(COLORS["line"], 220), width=s(3))
        draw.line((s(x), s(y2), s(x), s(y2 + 12)), fill=rgba(COLORS["line"], 220), width=s(3))
    centered_text(draw, (x1 + 18, y1 + 4, x2 - 18, y1 + 38), "NVIDIA NIC", FONTS["chip"], fill=COLORS["text"])


def draw_nic_engine_box(draw: ImageDraw.ImageDraw) -> None:
    x1, y1, x2, _ = NIC_RECT
    engine = (x1 + 12, y1 + 38, x2 - 12, y1 + 122)
    draw.rounded_rectangle(box(engine), radius=s(9), fill=rgba(COLORS["nvidia"], 245), outline=rgba(COLORS["nvidia"], 220), width=s(2))
    centered_multiline_text(draw, engine, "buffer splitting\nengine", FONTS["nic_center"], fill=COLORS["ink"])


def draw_wire(draw: ImageDraw.ImageDraw, frame: int) -> None:
    y = dy(WIRE_Y)
    draw_rx_wire_label(lambda xy, text: draw_text(draw, xy, text, FONTS["label"], fill=COLORS["text"]), y)
    draw_rx_wire(draw, frame, 48, NIC_RECT[0], y, COLORS["wire"], COLORS["header"], pt, s, rgba, box)


def draw_device_memory(draw: ImageDraw.ImageDraw, frame: int) -> None:
    panels = (
        (HOST_RECT, "Host memory", COLORS["host"]),
        (GPU_RECT, "GPU memory", COLORS["gpu"]),
    )
    for rect, title, accent in panels:
        x1, y1, x2, y2 = rect
        draw.rounded_rectangle(box(rect), radius=s(18), fill=COLORS["panel_2"], outline=rgba(accent, 170), width=s(3))
        draw_text(draw, (x1 + 22, y1 + 24), title, FONTS["label"], fill=COLORS["text"], anchor="lt")

    header_slots = {pkt.num for pkt in PACKETS if frame >= pkt.header_end}
    payload_slots = {pkt.num for pkt in PACKETS if frame >= pkt.payload_end}
    draw_segment_slots(draw, HEADER_SLOT_AREA, COLORS["header"], header_slots, HEADER_SEGMENT_WIDTH, HEADER_SEGMENT_HEIGHT, "hdr", "horizontal")
    draw_segment_slots(draw, PAYLOAD_SLOT_AREA, COLORS["payload"], payload_slots, PAYLOAD_SEGMENT_WIDTH, PAYLOAD_SEGMENT_HEIGHT, "payload", "vertical")


def draw_static_background(draw: ImageDraw.ImageDraw) -> None:
    pass


def draw_kernel_bypass(draw: ImageDraw.ImageDraw) -> None:
    x1, y1, x2, y2 = KERNEL_BOX
    draw.rounded_rectangle(box(KERNEL_BOX), radius=s(12), fill=rgba(COLORS["kernel_fill"], 150), outline=rgba(COLORS["kernel"], 120), width=s(2))
    draw_text(draw, ((x1 + x2) / 2, y1 + 23), "Linux kernel", FONTS["small"], fill=COLORS["muted"], anchor="mm")
    draw_text(draw, ((x1 + x2) / 2, y1 + 47), "bypassed", FONTS["small"], fill=COLORS["muted"], anchor="mm")


def split_origin() -> tuple[float, float]:
    return NIC_RECT[2], dy(WIRE_Y)


def segment_pair_gap() -> float:
    return (HEADER_SEGMENT_WIDTH + PAYLOAD_SEGMENT_WIDTH) / 2


def payload_wire_start() -> tuple[float, float]:
    return 58 + PAYLOAD_SEGMENT_WIDTH / 2, dy(WIRE_Y)


def header_wire_start() -> tuple[float, float]:
    x, y = payload_wire_start()
    return x + segment_pair_gap(), y


def payload_split_origin() -> tuple[float, float]:
    x, y = split_origin()
    return x - segment_pair_gap(), y


def header_split_origin() -> tuple[float, float]:
    return split_origin()


def segment_split_origin(kind: str) -> tuple[float, float]:
    return header_split_origin() if kind == "header" else payload_split_origin()


def segment_hub(kind: str) -> tuple[float, float]:
    if kind == "header":
        return HEADER_SLOT_AREA[0] - 20, (HEADER_SLOT_AREA[1] + HEADER_SLOT_AREA[3]) / 2
    return payload_boundary_point()


def payload_boundary_point() -> tuple[float, float]:
    return PAYLOAD_SLOT_AREA[0], (PAYLOAD_SLOT_AREA[1] + PAYLOAD_SLOT_AREA[3]) / 2


def segment_trunk_path(kind: str) -> list[tuple[float, float]]:
    if kind == "payload":
        return payload_trunk_path()
    start_x, start_y = split_origin()
    hub_x, hub_y = segment_hub(kind)
    bend_x = start_x + 88
    return rectangular_path(
        (
            (start_x, start_y),
            (bend_x, start_y),
            (bend_x, hub_y),
            (hub_x, hub_y),
        )
    )


def segment_wire_path(kind: str) -> list[tuple[float, float]]:
    start = header_wire_start() if kind == "header" else payload_wire_start()
    end = segment_split_origin(kind)
    return rectangular_path((start, end))


def segment_slot_center(kind: str, slot_idx: int) -> tuple[float, float]:
    return rect_center(header_slot_rect(slot_idx) if kind == "header" else payload_slot_rect(slot_idx))


def header_post_split_path(slot_idx: int) -> list[tuple[float, float]]:
    start_x, start_y = header_split_origin()
    slot_x, slot_y = segment_slot_center("header", slot_idx)
    bend_x = start_x + 88
    return rectangular_path(
        (
            (start_x, start_y),
            (bend_x, start_y),
            (bend_x, slot_y),
            (slot_x, slot_y),
        )
    )


def payload_trunk_path() -> list[tuple[float, float]]:
    start_x, start_y = payload_split_origin()
    boundary_x, boundary_y = payload_boundary_point()
    bend_x = start_x + segment_pair_gap() + 88
    return rectangular_path(
        (
            (start_x, start_y),
            (start_x + segment_pair_gap(), start_y),
            (bend_x, start_y),
            (bend_x, boundary_y),
            (boundary_x, boundary_y),
        )
    )


def payload_post_split_path(slot_idx: int) -> list[tuple[float, float]]:
    slot_x, slot_y = segment_slot_center("payload", slot_idx)
    trunk = payload_trunk_path()
    diagonal = rectangular_path((trunk[-1], (slot_x, slot_y)))
    return trunk + diagonal[1:]


def segment_path(kind: str, slot_idx: int) -> list[tuple[float, float]]:
    wire = segment_wire_path(kind)
    if kind == "header":
        post_split = header_post_split_path(slot_idx)
        return wire + post_split[1:]
    if kind == "payload":
        post_split = payload_post_split_path(slot_idx)
        return wire + post_split[1:]
    trunk = segment_trunk_path(kind)
    branch = rectangular_path((trunk[-1], segment_slot_center(kind, slot_idx)))
    return wire + trunk[1:] + branch[1:]


def segment_post_split_path(kind: str, slot_idx: int) -> list[tuple[float, float]]:
    if kind == "header":
        return header_post_split_path(slot_idx)
    if kind == "payload":
        return payload_post_split_path(slot_idx)
    trunk = segment_trunk_path(kind)
    branch = rectangular_path((trunk[-1], segment_slot_center(kind, slot_idx)))
    return trunk + branch[1:]


def segment_position(kind: str, pkt: PacketSpec, frame: int) -> tuple[float, float]:
    if frame < pkt.header_start:
        return point_on_path(segment_wire_path(kind), progress_linear(frame, pkt.start, pkt.header_start))

    end = pkt.header_end if kind == "header" else pkt.payload_end
    return point_on_path(segment_post_split_path(kind, pkt.num), progress_linear(frame, pkt.header_start, end))


def draw_dma_paths(base: Image.Image, draw: ImageDraw.ImageDraw, frame: int) -> None:
    header_path = segment_trunk_path("header")
    payload_path = segment_trunk_path("payload")
    draw_arrow(draw, header_path, rgba(str(COLORS["arrow"]), 130), 3, 12)
    draw_arrow(draw, payload_path, rgba(str(COLORS["arrow"]), 130), 3, 12)
    bend_x = NIC_RECT[2] + 88
    draw_path_tag(draw, (bend_x + 14, (HEADER_SLOT_AREA[1] + HEADER_SLOT_AREA[3]) / 2 - 30), "header DMA", COLORS["header"])
    draw_path_tag(draw, (bend_x + 14, (PAYLOAD_SLOT_AREA[1] + PAYLOAD_SLOT_AREA[3]) / 2 + 32), "GPUDirect payload DMA", COLORS["payload"])


def draw_flowing_segments(
    base: Image.Image,
    draw: ImageDraw.ImageDraw,
    frame: int,
) -> None:
    for pkt in PACKETS:
        if pkt.start <= frame < pkt.payload_end:
            draw_packet_segment(
                draw,
                segment_position("payload", pkt, frame),
                PAYLOAD_SEGMENT_WIDTH,
                PAYLOAD_SEGMENT_HEIGHT,
                COLORS["payload"],
                f"payload {pkt.num + 1}",
            )

        if pkt.start <= frame < pkt.header_end:
            draw_packet_segment(
                draw,
                segment_position("header", pkt, frame),
                HEADER_SEGMENT_WIDTH,
                HEADER_SEGMENT_HEIGHT,
                COLORS["header"],
                f"hdr {pkt.num + 1}",
            )


PACKETS = make_packets()
FRAMES = max(190, max(pkt.payload_end for pkt in PACKETS) + 36)


def render_frame(frame: int) -> Image.Image:
    bg = COLORS.get("bg")
    img = Image.new("RGBA", (WIDTH * SCALE, HEIGHT * SCALE), rgba(str(bg), 255) if bg else TRANSPARENT)
    draw = ImageDraw.Draw(img)
    draw_static_background(draw)
    draw_wire(draw, frame)
    draw_kernel_bypass(draw)
    draw_dma_paths(img, draw, frame)
    draw_device_memory(draw, frame)
    nic_active = any(pkt.nic_arrive - 3 <= frame < pkt.payload_start for pkt in PACKETS)
    draw_nic_chip_base(draw, NIC_RECT, COLORS["nvidia"], pulse=0.8 if nic_active else 0.0)
    draw_flowing_segments(img, draw, frame)
    draw_nic_engine_box(draw)
    return img.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)


def render_theme(theme: str) -> None:
    global COLORS
    COLORS = THEMES[theme]
    suffix = THEME_OUTPUT_SUFFIX[theme]
    animation_path = OUTPUT_DIR / f"header-data-split{suffix}.webp"
    poster_path = OUTPUT_DIR / f"header-data-split{suffix}-poster.png"

    frames = [render_frame(i) for i in range(FRAMES)]
    save_webp_animation(frames, animation_path, DURATION_MS)
    render_frame(min(FRAMES - 1, max(pkt.payload_end for pkt in PACKETS) + 10)).save(poster_path, optimize=True)
    print(f"Wrote {animation_path}")
    print(f"Wrote {poster_path}")


def main() -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    for theme in THEMES:
        render_theme(theme)


if __name__ == "__main__":
    main()
