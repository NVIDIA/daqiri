from __future__ import annotations

import math
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

from incoming_wire import center_wire_y, draw_rx_wire, draw_rx_wire_label, path_point_and_tangent, wire_t_end


ROOT = Path(__file__).resolve().parent
OUTPUT_DIR = ROOT / "flow_steering"

WIDTH = 1180
HEIGHT = 660
SCALE = 2
DURATION_MS = 55

GPU_TRAVEL_FRAMES = 76
ARRIVAL_GAP = 14

LAYOUT_SHIFT_Y = 0

NIC_RECT = (300, 250, 500, 410)
HOST_RECT = (670, 86, 1040, 266)
GPU_RECT = (670, 394, 1040, 574)
HOST_ROW = (694, 174, 1016, 228)
GPU_ROW = (694, 482, 1016, 536)

WIRE_Y = 330
KERNEL_WIDTH = 130
HOST_SLOT_COUNT = 3
GPU_SLOT_COUNT = 6

TRANSPARENT = (0, 0, 0, 0)
GIF_TRANSPARENCY_INDEX = 255

ACCENTS = {
    "nvidia": "#76b900",
    "host": "#9b8cff",
    "gpu": "#59d4ff",
    "kernel": "#3b82f6",
    "host_pkt": "#9b8cff",
    "gpu_pkt": "#59d4ff",
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
    dest: str  # "host" for non-matching packets, "gpu" for flow matches.
    matched: bool
    start: int
    decide: int
    end: int


def shift_y(y: float) -> float:
    return y + LAYOUT_SHIFT_Y


def shift_rect(rect: tuple[float, float, float, float]) -> tuple[float, float, float, float]:
    x1, y1, x2, y2 = rect
    return x1, y1 + LAYOUT_SHIFT_Y, x2, y2 + LAYOUT_SHIFT_Y


NIC = shift_rect(NIC_RECT)
HOST = shift_rect(HOST_RECT)
GPU = shift_rect(GPU_RECT)
HOST_ROW_R = shift_rect(HOST_ROW)
GPU_ROW_R = shift_rect(GPU_ROW)
WIRE = shift_y(WIRE_Y)
KERNEL_CX = (NIC_RECT[2] + HOST_RECT[0]) / 2
KERNEL = shift_rect((KERNEL_CX - KERNEL_WIDTH / 2, 22, KERNEL_CX + KERNEL_WIDTH / 2, 92))
NIC_CX = (NIC[0] + NIC[2]) / 2


def rgb(hex_color: str) -> tuple[int, int, int]:
    hex_color = hex_color.strip("#")
    return tuple(int(hex_color[i : i + 2], 16) for i in (0, 2, 4))


def rgba(hex_color: str, alpha: int = 255) -> tuple[int, int, int, int]:
    r, g, b = rgb(hex_color)
    return r, g, b, alpha


def font(size: int, *, bold: bool = False) -> ImageFont.FreeTypeFont:
    if bold:
        candidates = [
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
            "C:/Windows/Fonts/segoeuib.ttf",
        ]
    else:
        candidates = [
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            "C:/Windows/Fonts/segoeui.ttf",
        ]
    for path in candidates:
        if Path(path).exists():
            return ImageFont.truetype(path, size * SCALE)
    return ImageFont.load_default()


FONTS = {
    "label": font(18, bold=True),
    "small": font(14),
    "tiny": font(12),
    "chip": font(21, bold=True),
    "badge": font(16, bold=True),
}


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


def panel_left_center(rect: tuple[float, float, float, float]) -> tuple[float, float]:
    x1, y1, x2, y2 = rect
    return x1, (y1 + y2) / 2


def panel_top_center(rect: tuple[float, float, float, float]) -> tuple[float, float]:
    x1, y1, x2, _ = rect
    return (x1 + x2) / 2, y1


def build_paths() -> dict[str, list[tuple[float, float]]]:
    host_y = (HOST_ROW_R[1] + HOST_ROW_R[3]) / 2
    gpu_y = (GPU_ROW_R[1] + GPU_ROW_R[3]) / 2
    bus_y = (KERNEL[1] + KERNEL[3]) / 2
    kernel_in = (KERNEL[0], bus_y)
    kernel_out = (KERNEL[2], bus_y)
    nic_top = (NIC_CX, NIC[1])
    nic_bottom = (NIC_CX, NIC[3])
    host_entry = panel_top_center(HOST)
    gpu_entry = panel_left_center(GPU)

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
    gpu_tail = rectangular_path(
        (
            (NIC_CX, WIRE),
            nic_bottom,
            (nic_bottom[0], gpu_entry[1]),
            gpu_entry,
            (gpu_entry[0], gpu_y),
            (GPU_ROW_R[0], gpu_y),
        )
    )
    return {
        "wire": wire,
        "host": wire + host_tail[1:],
        "gpu": wire + gpu_tail[1:],
        "host_nic_to_kernel": host_nic_to_kernel,
        "host_kernel_to_queue": host_kernel_to_queue,
        "host_direct": host_direct,
        "gpu_direct": rectangular_path(
            (
                nic_bottom,
                (nic_bottom[0], gpu_entry[1]),
                gpu_entry,
            )
        ),
    }


PATHS = build_paths()


def path_total(points: list[tuple[float, float]]) -> float:
    total = 0.0
    for i in range(1, len(points)):
        x0, y0 = points[i - 1]
        x1, y1 = points[i]
        total += math.hypot(x1 - x0, y1 - y0)
    return total


def make_packets() -> tuple[PacketSpec, ...]:
    gpu_len = path_total(PATHS["gpu"])
    host_len = path_total(PATHS["host"])
    wire_len = path_total(PATHS["wire"])
    px_per_frame = gpu_len / GPU_TRAVEL_FRAMES
    wire_frames = max(1, int(math.ceil(wire_len / px_per_frame)))

    def travel_frames(dest: str) -> int:
        length = host_len if dest == "host" else gpu_len
        return max(1, int(math.ceil(length / px_per_frame)))

    routing = (
        (1, "gpu", True),
        (2, "gpu", True),
        (3, "host", False),
        (4, "gpu", True),
        (5, "gpu", True),
        (6, "host", False),
        (7, "gpu", True),
        (8, "gpu", True),
        (9, "host", False),
    )
    packets: list[PacketSpec] = []
    cursor = 8
    for num, dest, matched in routing:
        duration = travel_frames(dest)
        packets.append(PacketSpec(num, dest, matched, cursor, cursor + wire_frames, cursor + duration))
        cursor += ARRIVAL_GAP
    return tuple(packets)


PACKETS = make_packets()
FRAMES = max(320, PACKETS[-1].end + 24)


def decision_packet(frame: int) -> PacketSpec | None:
    for pkt in reversed(PACKETS):
        if pkt.decide <= frame < pkt.decide + 18:
            return pkt
    return None


def decision_pulse(frame: int, pkt: PacketSpec) -> float:
    return max(progress_linear(frame, pkt.decide, pkt.decide + 8), 1.0 - progress_linear(frame, pkt.decide + 10, pkt.decide + 18))


def queued_packets(frame: int) -> tuple[list[int], list[int]]:
    host_nums: list[int] = []
    gpu_nums: list[int] = []
    for pkt in PACKETS:
        if frame >= pkt.end:
            if pkt.dest == "host":
                host_nums.append(pkt.num)
            elif pkt.dest == "gpu":
                gpu_nums.append(pkt.num)
    return host_nums, gpu_nums


def draw_whole_packet(
    draw: ImageDraw.ImageDraw,
    cx: float,
    cy: float,
    width: float,
    color: str,
    alpha: int = 255,
    *,
    label: str | None = None,
) -> None:
    h = 32
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
        centered_text(draw, (x1, y1, x2, y2), label, FONTS["tiny"], fill=rgba(COLORS["ink"], alpha))


def packet_color(_num: int, dest: str) -> str:
    return COLORS["gpu_pkt"] if dest == "gpu" else COLORS["host_pkt"]


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


def packet_dest(num: int) -> str:
    for pkt in PACKETS:
        if pkt.num == num:
            return pkt.dest
    return "host"


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
        slot_rect = (cx - chip_w / 2, cy - 16, cx + chip_w / 2, cy + 16)
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
        color = packet_color(num, packet_dest(num))
        draw_whole_packet(draw, cx, cy, chip_w - 4, color, label=f"P{num}")


def draw_host_route(draw: ImageDraw.ImageDraw) -> None:
    color = rgba(COLORS["host_pkt"], 255)
    draw_polyline(draw, PATHS["host_nic_to_kernel"], color, 3)
    draw_polyline(draw, PATHS["host_kernel_to_queue"], color, 3)


def draw_kernel_bypass(draw: ImageDraw.ImageDraw) -> None:
    x1, y1, x2, y2 = KERNEL
    draw.rounded_rectangle(box(KERNEL), radius=s(12), fill=rgba(COLORS["kernel_fill"], 245), outline=rgba(COLORS["kernel"], 210), width=s(2))
    centered_text(draw, (x1, y1 + 8, x2, y2 - 8), "Linux kernel", FONTS["label"], fill=COLORS["text"])


def draw_matched_routes(draw: ImageDraw.ImageDraw) -> None:
    draw_polyline(draw, PATHS["gpu_direct"], rgba(COLORS["gpu_pkt"], 255), 3)


def nic_status(frame: int) -> tuple[str, str, str, float]:
    pkt = decision_packet(frame)
    deciding = pkt is not None
    pulse = decision_pulse(frame, pkt) if deciding and pkt else 0.0
    if deciding and pkt:
        if pkt.matched:
            return "match -> GPU", COLORS["gpu_pkt"], "#ffffff", pulse
        return "no match: to kernel", COLORS["host_pkt"], "#ffffff", pulse
    return "flow match rule", COLORS["nvidia"], COLORS["ink"], pulse


def draw_nic_status_pill(draw: ImageDraw.ImageDraw, frame: int) -> None:
    x1, y1, x2, _ = NIC
    pill_text, pill_accent, pill_ink, pulse = nic_status(frame)
    pill = (x1 + 18, y1 + 82, x2 - 18, y1 + 110)
    draw.rounded_rectangle(
        box(pill),
        radius=s(8),
        fill=rgba(pill_accent, 255),
        outline=rgba(pill_accent, 220),
        width=s(2),
    )
    centered_text(draw, pill, pill_text, FONTS["badge"], fill=pill_ink)


def draw_nic_chip(draw: ImageDraw.ImageDraw, frame: int) -> None:
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
    centered_text(draw, (x1 + 18, y1 + 38, x2 - 18, y1 + 78), "NVIDIA NIC", FONTS["chip"], fill=COLORS["text"])
    draw_nic_status_pill(draw, frame)


def draw_device_memory(draw: ImageDraw.ImageDraw, frame: int) -> None:
    host_nums, gpu_nums = queued_packets(frame)
    panels = (
        (HOST, HOST_ROW_R, "Host memory", "kernel path packets", COLORS["host"], host_nums, COLORS["host_pkt"], HOST_SLOT_COUNT),
        (GPU, GPU_ROW_R, "NVIDIA GPU memory", "flow-matched packets", COLORS["gpu"], gpu_nums, COLORS["gpu_pkt"], GPU_SLOT_COUNT),
    )
    for panel_rect, row_rect, title, subtitle, accent, nums, pkt_color, slot_count in panels:
        x1, y1, _, _ = panel_rect
        draw.rounded_rectangle(box(panel_rect), radius=s(18), fill=COLORS["panel_2"], outline=rgba(accent, 170), width=s(3))
        draw_text(draw, (x1 + 22, y1 + 22), title, FONTS["label"], fill=COLORS["text"], anchor="lt")
        draw_text(draw, (x1 + 22, y1 + 46), subtitle, FONTS["small"], fill=COLORS["muted"], anchor="lt")
        draw_queue_row(draw, row_rect, nums, pkt_color, slot_count)


def draw_wires(draw: ImageDraw.ImageDraw, frame: int) -> None:
    draw_rx_wire_label(lambda xy, text: draw_text(draw, xy, text, FONTS["label"], fill=COLORS["text"]), WIRE)
    draw_rx_wire(draw, frame, 48, NIC[0], WIRE, str(COLORS["wire"]), "#ffcf5a", pt, s, rgba, box)


def draw_route_glow(base: Image.Image, frame: int) -> None:
    pkt = decision_packet(frame)
    if pkt and frame >= pkt.decide:
        glow = progress_linear(frame, pkt.decide, pkt.end)
        if pkt.dest == "host":
            draw_glow_line(base, PATHS["host_direct"], COLORS["host_pkt"], int(50 + 40 * glow), 7)
        else:
            draw_glow_line(base, PATHS["gpu_direct"], COLORS["gpu_pkt"], int(50 + 40 * glow), 7)


def draw_flowing_packets(draw: ImageDraw.ImageDraw, frame: int) -> None:
    for pkt in PACKETS:
        if not (pkt.start <= frame < pkt.end):
            continue
        path = PATHS["gpu"] if pkt.dest == "gpu" else PATHS["host"]
        color = str(packet_color(pkt.num, pkt.dest))
        t = progress_linear(frame, pkt.start, pkt.end)
        if 0 < t < 1:
            on_wire = t <= wire_t_end(PATHS["wire"], path)
            cx, cy, _ = path_point_and_tangent(path, t)
            if on_wire:
                cy = center_wire_y(cx, 48, WIRE, frame)
            inside_nic = NIC[0] <= cx <= NIC[2] and NIC[1] <= cy <= NIC[3]
            draw_whole_packet(draw, cx, cy, 76, color, label=None if inside_nic else f"P{pkt.num}")


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


def rgba_frame_to_palette(frame: Image.Image) -> Image.Image:
    rgba_img = frame.convert("RGBA")
    alpha = rgba_img.getchannel("A")
    rgb_img = Image.new("RGB", rgba_img.size, (0, 0, 0))
    rgb_img.paste(rgba_img, mask=alpha)
    palette = rgb_img.quantize(colors=254, method=Image.Quantize.MEDIANCUT)
    palette.paste(GIF_TRANSPARENCY_INDEX, mask=alpha.point(lambda a: 255 if a < 128 else 0))
    return palette


def render_frame(frame: int) -> Image.Image:
    bg = COLORS.get("bg")
    img = Image.new("RGBA", (WIDTH * SCALE, HEIGHT * SCALE), rgba(str(bg), 255) if bg else TRANSPARENT)
    draw = ImageDraw.Draw(img)
    draw_wires(draw, frame)
    draw_device_memory(draw, frame)
    draw_host_route(draw)
    draw_kernel_bypass(draw)
    draw_nic_chip(draw, frame)
    draw_matched_routes(draw)
    draw_route_glow(img, frame)
    draw_flowing_packets(draw, frame)
    draw_nic_status_pill(draw, frame)
    return img.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)


def render_theme(theme: str) -> None:
    global COLORS
    COLORS = THEMES[theme]
    suffix = THEME_OUTPUT_SUFFIX[theme]
    gif_path = OUTPUT_DIR / f"flow-steering{suffix}.gif"
    poster_path = OUTPUT_DIR / f"flow-steering{suffix}-poster.png"

    frames = [render_frame(i) for i in range(FRAMES)]
    gif_frames = [rgba_frame_to_palette(f) for f in frames]
    gif_frames[0].save(
        gif_path,
        save_all=True,
        append_images=gif_frames[1:],
        optimize=True,
        duration=DURATION_MS,
        loop=0,
        disposal=2,
        transparency=GIF_TRANSPARENCY_INDEX,
    )
    render_frame(min(FRAMES - 1, PACKETS[-1].end + 10)).save(poster_path, optimize=True)
    print(f"Wrote {gif_path}")
    print(f"Wrote {poster_path}")


def main() -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    for theme in THEMES:
        render_theme(theme)


if __name__ == "__main__":
    main()
