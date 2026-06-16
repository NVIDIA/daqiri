from __future__ import annotations

import math
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw

from anim_common import font_scheme, save_webp_animation
from incoming_wire import center_wire_y, draw_rx_wire, draw_rx_wire_label, path_point_and_tangent, wire_t_end


ROOT = Path(__file__).resolve().parent
OUTPUT_DIR = ROOT / "flow_steering"

WIDTH = 1180
HEIGHT = 660
SCALE = 2
DURATION_MS = 40

QUEUE_TRAVEL_FRAMES = 76
ARRIVAL_GAP = 14

LAYOUT_SHIFT_Y = 0

NIC_RECT = (300, 250, 500, 410)
HOST_RECT = (750, 86, 1120, 282)
GPU_RECT = (750, 360, 1120, 620)
HOST_ROW = (774, 170, 1096, 238)
GPU_QUEUE_AREA = (774, 430, 1096, 596)

WIRE_Y = 330
KERNEL_WIDTH = 130
HOST_SLOT_COUNT = 3
QUEUE_COUNT = 3
QUEUE_SLOT_COUNT = 2
WIRE_PACKET_WIDTH = 86
WIRE_PACKET_HEIGHT = 38
HOST_PACKET_HEIGHT = 38
QUEUE_PACKET_HEIGHT = 32
FLOW_RULES = {
    4096: 0,
    4097: 1,
    4098: 2,
}

TRANSPARENT = (0, 0, 0, 0)

ACCENTS = {
    "nvidia": "#76b900",
    "host": "#9b8cff",
    "gpu": "#59d4ff",
    "kernel": "#3b82f6",
    "host_pkt": "#9b8cff",
    "gpu_pkt": "#59d4ff",
    "queue_0": "#59d4ff",
    "queue_1": "#78e08f",
    "queue_2": "#ffcf5a",
    "ink": "#07111f",
}

THEMES: dict[str, dict[str, str | None]] = {
    "default": {
        **ACCENTS,
        "bg": None,
        "panel": "#0d1b2e",
        "panel_2": "#101f34",
        "line": "#35516f",
        "text": "#f6fbff",
        "muted": "#afc0cf",
        "wire": "#6d7e92",
        "shadow": "#03101d",
        "row_bg": "#061220",
        "arrow": "#ffffff",
        "stroke": "#ffffff",
        "bar_glow": "#ffffff",
        "kernel_fill": "#0f2744",
        "bypass": "#64748b",
        "match_ok": "#76b900",
        "match_no": "#ef4444",
    },
    "light": {
        **ACCENTS,
        "bg": "#ffffff",
        "panel": "#f5f5f5",
        "panel_2": "#eeeeee",
        "line": "#1a1a1a",
        "text": "#1a1a1a",
        "muted": "#404040",
        "wire": "#404040",
        "shadow": "#bdbdbd",
        "row_bg": "#e8e8e8",
        "arrow": "#1a1a1a",
        "stroke": "#1a1a1a",
        "bar_glow": "#cccccc",
        "kernel_fill": "#f0f4ff",
        "bypass": "#64748b",
        "match_ok": "#15803d",
        "match_no": "#dc2626",
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
    udp_port: int
    queue_id: int | None
    start: int
    decide: int
    end: int

    @property
    def matched(self) -> bool:
        return self.queue_id is not None


def shift_y(y: float) -> float:
    return y + LAYOUT_SHIFT_Y


def shift_rect(rect: tuple[float, float, float, float]) -> tuple[float, float, float, float]:
    x1, y1, x2, y2 = rect
    return x1, y1 + LAYOUT_SHIFT_Y, x2, y2 + LAYOUT_SHIFT_Y


NIC = shift_rect(NIC_RECT)
HOST = shift_rect(HOST_RECT)
GPU = shift_rect(GPU_RECT)
HOST_ROW_R = shift_rect(HOST_ROW)
GPU_QUEUE_AREA_R = shift_rect(GPU_QUEUE_AREA)
WIRE = shift_y(WIRE_Y)
KERNEL_CX = NIC_RECT[2] + 102
KERNEL = shift_rect((KERNEL_CX - KERNEL_WIDTH / 2, 22, KERNEL_CX + KERNEL_WIDTH / 2, 92))
NIC_CX = (NIC[0] + NIC[2]) / 2


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


def progress_linear(frame: int, start: int, end: int) -> float:
    return clamp((frame - start) / max(1, end - start))


def rectangular_path(waypoints: tuple[tuple[float, float], ...], steps: int = 32) -> list[tuple[float, float]]:
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


def draw_dashed_polyline(
    draw: ImageDraw.ImageDraw,
    points: list[tuple[float, float]],
    color: str | tuple[int, int, int, int],
    width: int = 3,
    dash: float = 10,
    gap: float = 14,
) -> None:
    for i in range(len(points) - 1):
        x0, y0 = points[i]
        x1, y1 = points[i + 1]
        seg_len = math.hypot(x1 - x0, y1 - y0)
        if seg_len <= 0:
            continue
        ux, uy = (x1 - x0) / seg_len, (y1 - y0) / seg_len
        pos = 0.0
        draw_on = True
        while pos < seg_len:
            step = dash if draw_on else gap
            end = min(seg_len, pos + step)
            if draw_on:
                draw.line(
                    (pt((x0 + ux * pos, y0 + uy * pos)), pt((x0 + ux * end, y0 + uy * end))),
                    fill=color,
                    width=s(width),
                )
            pos = end
            draw_on = not draw_on


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


def draw_path_tag(
    draw: ImageDraw.ImageDraw,
    xy: tuple[float, float],
    text: str,
    color: str,
) -> None:
    tx, ty = xy
    tw, th = text_size(draw, text, FONTS["tiny"])
    pad_x = 8
    pad_y = 5
    tag = (
        tx - tw / (2 * SCALE) - pad_x,
        ty - th / (2 * SCALE) - pad_y,
        tx + tw / (2 * SCALE) + pad_x,
        ty + th / (2 * SCALE) + pad_y,
    )
    draw.rounded_rectangle(box(tag), radius=s(6), fill=rgba(COLORS["panel"], 235), outline=rgba(color, 180), width=s(1))
    centered_text(draw, tag, text, FONTS["tiny"], fill=COLORS["text"])


def panel_left_center(rect: tuple[float, float, float, float]) -> tuple[float, float]:
    x1, y1, x2, y2 = rect
    return x1, (y1 + y2) / 2


def panel_top_center(rect: tuple[float, float, float, float]) -> tuple[float, float]:
    x1, y1, x2, _ = rect
    return (x1 + x2) / 2, y1


def flow_queue_rows() -> list[tuple[float, float, float, float]]:
    x1, y1, x2, y2 = GPU_QUEUE_AREA_R
    gap = 8
    row_h = (y2 - y1 - gap * (QUEUE_COUNT - 1)) / QUEUE_COUNT
    rows: list[tuple[float, float, float, float]] = []
    y = y1
    for _ in range(QUEUE_COUNT):
        rows.append((x1, y, x2, y + row_h))
        y += row_h + gap
    return rows


QUEUE_ROWS = flow_queue_rows()


def queue_center_y(queue_id: int) -> float:
    row = QUEUE_ROWS[queue_id]
    return (row[1] + row[3]) / 2


def queue_route_path(queue_id: int) -> list[tuple[float, float]]:
    qy = queue_center_y(queue_id)
    tap_x = NIC[2] + 54 + (QUEUE_COUNT - 1 - queue_id) * 44
    return rectangular_path(
        (
            (NIC[2], WIRE),
            (tap_x, WIRE),
            (tap_x, qy),
            (GPU_QUEUE_AREA_R[0], qy),
        )
    )


def build_paths() -> dict[str, list[tuple[float, float]]]:
    host_y = (HOST_ROW_R[1] + HOST_ROW_R[3]) / 2
    bus_y = (KERNEL[1] + KERNEL[3]) / 2
    kernel_in = (KERNEL[0], bus_y)
    kernel_out = (KERNEL[2], bus_y)
    nic_top = (NIC_CX, NIC[1])
    host_entry = panel_top_center(HOST)

    host_nic_to_kernel = rectangular_path((nic_top, (nic_top[0], bus_y), kernel_in))
    host_kernel_to_queue = rectangular_path((kernel_out, (host_entry[0], bus_y), host_entry))
    host_direct = host_nic_to_kernel + host_kernel_to_queue[1:]

    wire = rectangular_path(((48, WIRE), (NIC[0], WIRE)))
    host_tail = rectangular_path(
        (
            (NIC_CX, WIRE),
            nic_top,
            (nic_top[0], bus_y),
            kernel_in,
            kernel_out,
            (host_entry[0], bus_y),
            host_entry,
            (host_entry[0], host_y),
            (HOST_ROW_R[0], host_y),
        )
    )
    paths = {
        "wire": wire,
        "host": wire + host_tail[1:],
        "host_nic_to_kernel": host_nic_to_kernel,
        "host_kernel_to_queue": host_kernel_to_queue,
        "host_direct": host_direct,
    }
    for queue_id in range(QUEUE_COUNT):
        route = queue_route_path(queue_id)
        paths[f"queue{queue_id}_route"] = route
        paths[f"queue{queue_id}"] = wire + route[1:]
    return paths


PATHS = build_paths()


def path_total(points: list[tuple[float, float]]) -> float:
    total = 0.0
    for i in range(1, len(points)):
        x0, y0 = points[i - 1]
        x1, y1 = points[i]
        total += math.hypot(x1 - x0, y1 - y0)
    return total


def make_packets() -> tuple[PacketSpec, ...]:
    queue_len = max(path_total(PATHS[f"queue{queue_id}"]) for queue_id in range(QUEUE_COUNT))
    host_len = path_total(PATHS["host"])
    wire_len = path_total(PATHS["wire"])
    px_per_frame = queue_len / QUEUE_TRAVEL_FRAMES
    wire_frames = max(1, int(math.ceil(wire_len / px_per_frame)))

    def travel_frames(queue_id: int | None) -> int:
        length = host_len if queue_id is None else path_total(PATHS[f"queue{queue_id}"])
        return max(1, int(math.ceil(length / px_per_frame)))

    routing = (
        (1, 4096),
        (2, 4097),
        (3, 5000),
        (4, 4098),
        (5, 4096),
        (6, 4097),
        (7, 5001),
        (8, 4098),
        (9, 5002),
    )
    packets: list[PacketSpec] = []
    cursor = 8
    for num, udp_port in routing:
        queue_id = FLOW_RULES.get(udp_port)
        duration = travel_frames(queue_id)
        packets.append(PacketSpec(num, udp_port, queue_id, cursor, cursor + wire_frames, cursor + duration))
        cursor += ARRIVAL_GAP
    return tuple(packets)


PACKETS = make_packets()
FRAMES = PACKETS[-1].end + 24


def decision_packet(frame: int) -> PacketSpec | None:
    for pkt in reversed(PACKETS):
        if pkt.decide <= frame < pkt.decide + 18:
            return pkt
    return None


def decision_pulse(frame: int, pkt: PacketSpec) -> float:
    return max(progress_linear(frame, pkt.decide, pkt.decide + 8), 1.0 - progress_linear(frame, pkt.decide + 10, pkt.decide + 18))


def queued_packets(frame: int) -> tuple[list[int], list[list[int]]]:
    host_nums: list[int] = []
    queue_nums: list[list[int]] = [[] for _ in range(QUEUE_COUNT)]
    for pkt in PACKETS:
        if frame >= pkt.end:
            if pkt.queue_id is None:
                host_nums.append(pkt.num)
            else:
                queue_nums[pkt.queue_id].append(pkt.num)
    return host_nums, queue_nums


def draw_whole_packet(
    draw: ImageDraw.ImageDraw,
    cx: float,
    cy: float,
    width: float,
    color: str,
    alpha: int = 255,
    *,
    label: str | None = None,
    height: float = HOST_PACKET_HEIGHT,
) -> None:
    h = height
    x1, y1 = cx - width / 2, cy - h / 2
    x2, y2 = cx + width / 2, cy + h / 2
    draw.rounded_rectangle(
        box((x1, y1, x2, y2)),
        radius=s(6),
        fill=rgba(color, alpha),
        outline=rgba(str(COLORS["stroke"]), 100 * alpha // 255),
        width=s(2),
    )
    if label:
        centered_text(draw, (x1, y1, x2, y2), label, FONTS["packet"], fill=rgba(COLORS["ink"], alpha))


def queue_color(queue_id: int) -> str:
    return str(COLORS[f"queue_{queue_id}"])


def packet_by_num(num: int) -> PacketSpec | None:
    for pkt in PACKETS:
        if pkt.num == num:
            return pkt
    return None


def packet_color(num: int) -> str:
    pkt = packet_by_num(num)
    if pkt is None or pkt.queue_id is None:
        return str(COLORS["host_pkt"])
    return queue_color(pkt.queue_id)


def packet_label(num: int) -> str:
    return f"PKT {num}"


def queue_chip_layout(rect: tuple[float, float, float, float], count: int) -> list[tuple[float, float, float]]:
    x1, y1, x2, y2 = rect
    if count <= 0:
        return []
    pad = 8
    gap = 8
    inner_w = x2 - x1 - 2 * pad
    chip_w = (inner_w - gap * (count - 1)) / count
    cy = (y1 + y2) / 2
    slots: list[tuple[float, float, float]] = []
    x = x1 + pad
    for _ in range(count):
        slots.append((x + chip_w / 2, cy, chip_w))
        x += chip_w + gap
    return slots


def host_slot_index(pkt: PacketSpec) -> int:
    return sum(1 for other in PACKETS if other.queue_id is None and other.end <= pkt.end) - 1


def queue_slot_index(pkt: PacketSpec) -> int:
    if pkt.queue_id is None:
        return 0
    return sum(1 for other in PACKETS if other.queue_id == pkt.queue_id and other.end <= pkt.end) - 1


def host_slot_target(pkt: PacketSpec) -> tuple[float, float, float, float]:
    slots = queue_chip_layout(HOST_ROW_R, HOST_SLOT_COUNT)
    idx = max(0, min(HOST_SLOT_COUNT - 1, host_slot_index(pkt)))
    cx, cy, chip_w = slots[idx]
    return cx, cy, chip_w - 4, HOST_PACKET_HEIGHT


def flow_queue_slots_rect(row: tuple[float, float, float, float]) -> tuple[float, float, float, float]:
    x1, y1, x2, y2 = row
    return x1 + 112, y1 + 3, x2 - 8, y2 - 3


def queue_slot_target(pkt: PacketSpec) -> tuple[float, float, float, float]:
    queue_id = 0 if pkt.queue_id is None else pkt.queue_id
    slots = queue_chip_layout(flow_queue_slots_rect(QUEUE_ROWS[queue_id]), QUEUE_SLOT_COUNT)
    idx = max(0, min(QUEUE_SLOT_COUNT - 1, queue_slot_index(pkt)))
    cx, cy, chip_w = slots[idx]
    return cx, cy, chip_w - 4, QUEUE_PACKET_HEIGHT


def packet_target(pkt: PacketSpec) -> tuple[float, float, float, float]:
    return host_slot_target(pkt) if pkt.queue_id is None else queue_slot_target(pkt)


def packet_memory_entry(pkt: PacketSpec) -> tuple[float, float]:
    if pkt.queue_id is None:
        return panel_top_center(HOST)
    return PATHS[f"queue{pkt.queue_id}_route"][-1]


def packet_draw_dimensions(pkt: PacketSpec, cx: float, cy: float) -> tuple[float, float]:
    target_x, target_y, target_w, target_h = packet_target(pkt)
    entry_x, entry_y = packet_memory_entry(pkt)
    if pkt.queue_id is None:
        in_memory = HOST[0] <= cx <= HOST[2] and HOST[1] <= cy <= HOST[3]
    else:
        in_memory = GPU[0] <= cx <= GPU[2] and GPU[1] <= cy <= GPU[3]
    if not in_memory:
        return WIRE_PACKET_WIDTH, WIRE_PACKET_HEIGHT

    total = math.hypot(target_x - entry_x, target_y - entry_y)
    covered = math.hypot(cx - entry_x, cy - entry_y)
    t = clamp(covered / total if total else 1.0)
    return lerp(WIRE_PACKET_WIDTH, target_w, t), lerp(WIRE_PACKET_HEIGHT, target_h, t)


def packet_path(pkt: PacketSpec) -> list[tuple[float, float]]:
    target_x, target_y, _, _ = packet_target(pkt)
    wire = PATHS["wire"]
    if pkt.queue_id is None:
        bus_y = (KERNEL[1] + KERNEL[3]) / 2
        nic_top = (NIC_CX, NIC[1])
        host_entry = packet_memory_entry(pkt)
        tail = rectangular_path(
            (
                (NIC_CX, WIRE),
                nic_top,
                (nic_top[0], bus_y),
                (KERNEL[0], bus_y),
                (KERNEL[2], bus_y),
                (host_entry[0], bus_y),
                host_entry,
                (target_x, target_y),
            )
        )
        return wire + tail[1:]

    queue_path = PATHS[f"queue{pkt.queue_id}"]
    branch = rectangular_path((queue_path[-1], (target_x, target_y)))
    return queue_path + branch[1:]


def draw_queue_row(
    draw: ImageDraw.ImageDraw,
    rect: tuple[float, float, float, float],
    packet_nums: list[int],
    accent: str,
    slot_count: int,
) -> None:
    x1, y1, x2, y2 = rect
    draw.rounded_rectangle(box(rect), radius=s(8), fill=rgba(COLORS["row_bg"], 235), outline=rgba(accent, 145), width=s(2))
    slots = queue_chip_layout(rect, slot_count)
    for idx, (cx, cy, chip_w) in enumerate(slots):
        slot_rect = (cx - chip_w / 2, cy - HOST_PACKET_HEIGHT / 2, cx + chip_w / 2, cy + HOST_PACKET_HEIGHT / 2)
        draw.rounded_rectangle(
            box(slot_rect),
            radius=s(7),
            fill=rgba(COLORS["panel"], 165),
            outline=rgba(accent, 135),
            width=s(1),
        )
        if idx >= len(packet_nums):
            continue
        num = packet_nums[idx]
        color = packet_color(num)
        draw_whole_packet(draw, cx, cy, chip_w - 4, color, label=packet_label(num), height=HOST_PACKET_HEIGHT)


def draw_flow_queue_row(
    draw: ImageDraw.ImageDraw,
    rect: tuple[float, float, float, float],
    queue_id: int,
    packet_nums: list[int],
) -> None:
    x1, y1, x2, y2 = rect
    accent = queue_color(queue_id)
    draw.rounded_rectangle(box(rect), radius=s(8), fill=rgba(COLORS["row_bg"], 235), outline=rgba(accent, 145), width=s(2))
    label_rect = (x1 + 8, y1 + 3, x1 + 104, y2 - 3)
    centered_text(draw, label_rect, f"queue {queue_id + 1}", FONTS["slot"], fill=COLORS["muted"])
    slots_rect = flow_queue_slots_rect(rect)
    slots = queue_chip_layout(slots_rect, QUEUE_SLOT_COUNT)
    for idx, (cx, cy, chip_w) in enumerate(slots):
        slot_rect = (cx - chip_w / 2, cy - QUEUE_PACKET_HEIGHT / 2, cx + chip_w / 2, cy + QUEUE_PACKET_HEIGHT / 2)
        draw.rounded_rectangle(
            box(slot_rect),
            radius=s(6),
            fill=rgba(COLORS["panel"], 165),
            outline=rgba(accent, 135),
            width=s(1),
        )
        if idx >= len(packet_nums):
            continue
        num = packet_nums[idx]
        draw_whole_packet(draw, cx, cy, chip_w - 4, accent, label=packet_label(num), height=QUEUE_PACKET_HEIGHT)


def draw_host_route(draw: ImageDraw.ImageDraw) -> None:
    color = rgba(COLORS["host_pkt"], 255)
    draw_arrow(draw, PATHS["host_nic_to_kernel"], color, 3, 12)
    draw_arrow(draw, PATHS["host_kernel_to_queue"], color, 3, 12)


def draw_kernel_bypass(draw: ImageDraw.ImageDraw) -> None:
    x1, y1, x2, y2 = KERNEL
    draw.rounded_rectangle(box(KERNEL), radius=s(12), fill=rgba(COLORS["kernel_fill"], 245), outline=rgba(COLORS["kernel"], 210), width=s(2))
    centered_text(draw, (x1, y1 + 8, x2, y2 - 8), "Linux kernel", FONTS["label"], fill=COLORS["text"])


def draw_matched_routes(draw: ImageDraw.ImageDraw) -> None:
    for queue_id in range(QUEUE_COUNT):
        draw_arrow(draw, PATHS[f"queue{queue_id}_route"], rgba(str(COLORS["arrow"]), 130), 3, 12)
    draw_path_tag(draw, (600, 416), "flow match -> queue", COLORS["gpu"])


def nic_status(frame: int) -> tuple[str, str, str, float]:
    pkt = decision_packet(frame)
    deciding = pkt is not None
    pulse = decision_pulse(frame, pkt) if deciding and pkt else 0.0
    if deciding and pkt:
        if pkt.queue_id is not None:
            return f"{packet_label(pkt.num)}\nqueue {pkt.queue_id + 1}", queue_color(pkt.queue_id), "#ffffff", pulse
        return f"{packet_label(pkt.num)}\nto kernel", COLORS["host_pkt"], "#ffffff", pulse
    return "flow\nsteering", COLORS["nvidia"], COLORS["ink"], pulse


def draw_nic_status_pill(draw: ImageDraw.ImageDraw, frame: int) -> None:
    x1, y1, x2, _ = NIC
    pill_text, pill_accent, pill_ink, pulse = nic_status(frame)
    pill = (x1 + 12, y1 + 38, x2 - 12, y1 + 122)
    draw.rounded_rectangle(
        box(pill),
        radius=s(8),
        fill=rgba(pill_accent, 255),
        outline=rgba(pill_accent, 220),
        width=s(2),
    )
    centered_multiline_text(draw, pill, pill_text, FONTS["nic_center"], fill=pill_ink)


def draw_nic_chip_base(draw: ImageDraw.ImageDraw, frame: int) -> None:
    x1, y1, x2, y2 = NIC
    pkt = decision_packet(frame)
    deciding = pkt is not None
    pulse = decision_pulse(frame, pkt) if deciding and pkt else 0.0
    accent = COLORS["nvidia"]

    glow_alpha = int(55 + 120 * pulse) if deciding else 55
    draw.rounded_rectangle(box((x1 - 10, y1 - 10, x2 + 10, y2 + 10)), radius=s(24), fill=rgba(accent, 16 + glow_alpha // 3))
    draw.rounded_rectangle(box(NIC), radius=s(18), fill=COLORS["panel"], outline=rgba(accent, 190 + int(65 * pulse)), width=s(3 + int(2 * pulse)))
    for i in range(7):
        y = lerp(y1 + 18, y2 - 18, i / 6)
        draw.line((s(x1 - 14), s(y), s(x1), s(y)), fill=rgba(COLORS["line"], 220), width=s(3))
        draw.line((s(x2), s(y), s(x2 + 14), s(y)), fill=rgba(COLORS["line"], 220), width=s(3))
    for i in range(5):
        x = lerp(x1 + 25, x2 - 25, i / 4)
        draw.line((s(x), s(y1 - 12), s(x), s(y1)), fill=rgba(COLORS["line"], 220), width=s(3))
        draw.line((s(x), s(y2), s(x), s(y2 + 12)), fill=rgba(COLORS["line"], 220), width=s(3))
    centered_text(draw, (x1 + 18, y1 + 4, x2 - 18, y1 + 38), "NVIDIA NIC", FONTS["chip"], fill=COLORS["text"])


def draw_device_memory(draw: ImageDraw.ImageDraw, frame: int) -> None:
    host_nums, queue_nums = queued_packets(frame)
    panels = (
        (HOST, "Host memory", "kernel fallback packets", COLORS["host"]),
        (GPU, "GPU memory", "RX queues selected by flow rules", COLORS["gpu"]),
    )
    for panel_rect, title, subtitle, accent in panels:
        x1, y1, _, _ = panel_rect
        draw.rounded_rectangle(box(panel_rect), radius=s(18), fill=COLORS["panel_2"], outline=rgba(accent, 170), width=s(3))
        draw_text(draw, (x1 + 22, y1 + 22), title, FONTS["label"], fill=COLORS["text"], anchor="lt")
        draw_text(draw, (x1 + 22, y1 + 46), subtitle, FONTS["small"], fill=COLORS["muted"], anchor="lt")
    draw_queue_row(draw, HOST_ROW_R, host_nums, COLORS["host_pkt"], HOST_SLOT_COUNT)
    for queue_id, row in enumerate(QUEUE_ROWS):
        draw_flow_queue_row(draw, row, queue_id, queue_nums[queue_id])


def draw_wires(draw: ImageDraw.ImageDraw, frame: int) -> None:
    draw_rx_wire_label(lambda xy, text: draw_text(draw, xy, text, FONTS["label"], fill=COLORS["text"]), WIRE)
    draw_rx_wire(draw, frame, 48, NIC[0], WIRE, str(COLORS["wire"]), "#ffcf5a", pt, s, rgba, box)


def draw_route_glow(base: Image.Image, frame: int) -> None:
    return


def draw_flowing_packets(draw: ImageDraw.ImageDraw, frame: int) -> None:
    for pkt in PACKETS:
        if not (pkt.start <= frame < pkt.end):
            continue
        path = packet_path(pkt)
        color = packet_color(pkt.num)
        t = progress_linear(frame, pkt.start, pkt.end)
        if 0 <= t < 1:
            on_wire = t <= wire_t_end(PATHS["wire"], path)
            cx, cy, _ = path_point_and_tangent(path, t)
            if on_wire:
                cy = center_wire_y(cx, 48, WIRE, frame)
            inside_nic = NIC[0] <= cx <= NIC[2] and NIC[1] <= cy <= NIC[3]
            label = None if inside_nic else packet_label(pkt.num)
            packet_w, packet_h = packet_draw_dimensions(pkt, cx, cy)
            draw_whole_packet(draw, cx, cy, packet_w, color, label=label, height=packet_h)


def path_lengths(points: list[tuple[float, float]]) -> tuple[list[float], float]:
    lengths = [0.0]
    for i in range(1, len(points)):
        x0, y0 = points[i - 1]
        x1, y1 = points[i]
        lengths.append(lengths[-1] + math.hypot(x1 - x0, y1 - y0))
    return lengths, lengths[-1]


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


def render_frame(frame: int) -> Image.Image:
    bg = COLORS.get("bg")
    img = Image.new("RGBA", (WIDTH * SCALE, HEIGHT * SCALE), rgba(str(bg), 255) if bg else TRANSPARENT)
    draw = ImageDraw.Draw(img)
    draw_wires(draw, frame)
    draw_device_memory(draw, frame)
    draw_host_route(draw)
    draw_kernel_bypass(draw)
    draw_nic_chip_base(draw, frame)
    draw_matched_routes(draw)
    draw_route_glow(img, frame)
    draw_flowing_packets(draw, frame)
    draw_nic_status_pill(draw, frame)
    return img.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)


def render_theme(theme: str) -> None:
    global COLORS
    COLORS = THEMES[theme]
    suffix = THEME_OUTPUT_SUFFIX[theme]
    animation_path = OUTPUT_DIR / f"flow-steering{suffix}.webp"
    poster_path = OUTPUT_DIR / f"flow-steering{suffix}-poster.png"

    frames = [render_frame(i) for i in range(FRAMES)]
    save_webp_animation(frames, animation_path, DURATION_MS)
    render_frame(min(FRAMES - 1, PACKETS[-1].end + 10)).save(poster_path, optimize=True)
    print(f"Wrote {animation_path}")
    print(f"Wrote {poster_path}")


def main() -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    for theme in THEMES:
        render_theme(theme)


if __name__ == "__main__":
    main()
