from __future__ import annotations

import math
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

from incoming_wire import draw_rx_wire, draw_rx_wire_label


ROOT = Path(__file__).resolve().parent
OUTPUT_DIR = ROOT / "hds"

WIDTH = 1180
HEIGHT = 660
SCALE = 2
DURATION_MS = 55

TOTAL_BYTES = 128
HEADER_BYTES = 64
PAYLOAD_BYTES = 64

PACKET_COUNT = 5
WIRE_Y = 330
WIRE_FRAMES = 34
ARRIVAL_GAP = 16
SPLIT_DELAY = 8
HEADER_TRAVEL_FRAMES = 36
PAYLOAD_TRAVEL_FRAMES = 42
PACKET_BAR_WIDTH = 132
PACKET_BAR_HEIGHT = 34

LAYOUT_SHIFT_Y = 0


def dy(y: float) -> float:
    return y + LAYOUT_SHIFT_Y


def shift_rect(rect: tuple[float, float, float, float]) -> tuple[float, float, float, float]:
    x1, y1, x2, y2 = rect
    return x1, y1 + LAYOUT_SHIFT_Y, x2, y2 + LAYOUT_SHIFT_Y


NIC_RECT = shift_rect((300, 250, 500, 410))
HOST_RECT = shift_rect((670, 86, 1040, 266))
GPU_RECT = shift_rect((670, 394, 1040, 574))
KERNEL_WIDTH = 130
KERNEL_CX = (NIC_RECT[2] + HOST_RECT[0]) / 2
KERNEL_BOX = shift_rect((KERNEL_CX - KERNEL_WIDTH / 2, 22, KERNEL_CX + KERNEL_WIDTH / 2, 92))
HEADER_ROW = shift_rect((694, 174, 1016, 228))
PAYLOAD_ROW = shift_rect((694, 482, 1016, 536))

TRANSPARENT = (0, 0, 0, 0)
GIF_TRANSPARENCY_INDEX = 255

ACCENTS = {
    "nvidia": "#76b900",
    "header": "#ffcf5a",
    "payload": "#78e08f",
    "host": "#9b8cff",
    "gpu": "#76b900",
    "kernel": "#3b82f6",
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
        nic_arrive = cursor + WIRE_FRAMES
        header_start = nic_arrive + SPLIT_DELAY
        payload_start = nic_arrive + SPLIT_DELAY + 6
        header_end = header_start + HEADER_TRAVEL_FRAMES
        payload_end = payload_start + PAYLOAD_TRAVEL_FRAMES
        packets.append(PacketSpec(num, cursor, nic_arrive, header_start, header_end, payload_start, payload_end))
        cursor += ARRIVAL_GAP
    return tuple(packets)


PACKETS = make_packets()
FRAMES = max(190, max(pkt.payload_end for pkt in PACKETS) + 36)


def rgb(hex_color: str) -> tuple[int, int, int]:
    hex_color = hex_color.strip("#")
    return tuple(int(hex_color[i : i + 2], 16) for i in (0, 2, 4))


def rgba(hex_color: str, alpha: int = 255) -> tuple[int, int, int, int]:
    r, g, b = rgb(hex_color)
    return r, g, b, alpha


def font(size: int, *, bold: bool = False, mono: bool = False) -> ImageFont.FreeTypeFont:
    if mono:
        candidates = [
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf" if bold else "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
            "/usr/share/fonts/truetype/noto/NotoSansMono-Bold.ttf" if bold else "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
            "C:/Windows/Fonts/consolab.ttf" if bold else "C:/Windows/Fonts/consola.ttf",
        ]
    elif bold:
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
}


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


def point_on_path(points: list[tuple[float, float]], t: float) -> tuple[float, float]:
    t = clamp(t)
    idx = t * (len(points) - 1)
    i = int(idx)
    j = min(i + 1, len(points) - 1)
    local = idx - i
    return lerp(points[i][0], points[j][0], local), lerp(points[i][1], points[j][1], local)


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
    colors = [COLORS["header"], COLORS["payload"]]
    names = ["Hdr", "Payload"]
    sizes = [HEADER_BYTES, PAYLOAD_BYTES]
    draw.rounded_rectangle(box((x - 3, y - 3, x + w + 3, y + h + 3)), radius=s(11), fill=rgba(str(COLORS["bar_glow"]), 22 * alpha // 255))
    cursor = x
    for i, (name, byte_count, color) in enumerate(zip(names, sizes, colors)):
        seg_w = w * byte_count / TOTAL_BYTES
        rect = (cursor, y, cursor + seg_w, y + h)
        radius = 9 if i in (0, len(sizes) - 1) else 2
        draw.rounded_rectangle(box(rect), radius=s(radius), fill=rgba(color, alpha), outline=rgba(COLORS["ink"], alpha), width=s(2))
        if label and seg_w > 52:
            centered_text(draw, rect, name, FONTS["tiny"], fill=rgba(COLORS["ink"], alpha))
        cursor += seg_w
    if packet_num is not None:
        draw_text(draw, (x + w / 2, y - 5), f"P{packet_num}", FONTS["tiny"], fill=COLORS["text"], anchor="mb")


def draw_memory_row(
    draw: ImageDraw.ImageDraw,
    rect: tuple[float, float, float, float],
    color: str,
    label: str,
    completed_slots: set[int],
    slot_prefix: str,
) -> None:
    x1, y1, x2, y2 = rect
    draw_text(draw, (x1, y1 - 13), label, FONTS["tiny"], fill=COLORS["muted"], anchor="lt")
    draw.rounded_rectangle(box(rect), radius=s(8), fill=rgba(COLORS["row_bg"], 235), outline=rgba(color, 145), width=s(2))

    pad = 8
    gap = 6
    inner_w = x2 - x1 - 2 * pad
    slot_w = (inner_w - gap * (PACKET_COUNT - 1)) / PACKET_COUNT
    slot_h = y2 - y1 - 14
    slot_y1 = y1 + 7
    for idx in range(PACKET_COUNT):
        sx1 = x1 + pad + idx * (slot_w + gap)
        slot = (sx1, slot_y1, sx1 + slot_w, slot_y1 + slot_h)
        filled = idx in completed_slots
        draw.rounded_rectangle(
            box(slot),
            radius=s(7),
            fill=rgba(color, 220) if filled else rgba(COLORS["panel"], 180),
            outline=rgba(color, 160) if filled else rgba(COLORS["line"], 170),
            width=s(1),
        )
        text = f"{slot_prefix}{idx}"
        fill = rgba(COLORS["ink"], 255) if filled else COLORS["muted"]
        centered_text(draw, slot, text, FONTS["tiny"], fill=fill)


def draw_packet_segment(
    draw: ImageDraw.ImageDraw,
    center: tuple[float, float],
    width: float,
    color: str,
    label: str,
    byte_text: str,
    alpha: int = 255,
) -> None:
    cx, cy = center
    rect = rect_at_center(cx, cy, width, 34)
    shadow = (rect[0] + 5, rect[1] + 6, rect[2] + 5, rect[3] + 6)
    draw.rounded_rectangle(box(shadow), radius=s(9), fill=rgba(COLORS["shadow"], 80 * alpha // 255))
    draw.rounded_rectangle(box(rect), radius=s(9), fill=rgba(color, alpha), outline=rgba(str(COLORS["stroke"]), 70 * alpha // 255), width=s(1))
    draw_text(draw, (cx, cy - 3), label, FONTS["tiny"], fill=rgba(COLORS["ink"], alpha), anchor="mm")
    draw_text(draw, (cx, cy + 10), byte_text, FONTS["tiny"], fill=rgba(COLORS["ink"], alpha), anchor="mm")


def draw_chip(
    draw: ImageDraw.ImageDraw,
    rect: tuple[float, float, float, float],
    title: str,
    subtitle: str,
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
    centered_text(draw, (x1 + 18, y1 + 39, x2 - 18, y1 + 75), title, FONTS["chip"], fill=COLORS["text"])
    if subtitle:
        centered_text(draw, (x1 + 20, y1 + 80, x2 - 20, y1 + 111), subtitle, FONTS["small"], fill=COLORS["muted"])
    draw.rounded_rectangle(box((x1 + 18, y1 + 82, x2 - 18, y1 + 110)), radius=s(8), fill=rgba(accent, 215))
    centered_text(draw, (x1 + 18, y1 + 82, x2 - 18, y1 + 110), "buffer splitting engine", FONTS["tiny"], fill=COLORS["ink"])


def draw_wire(draw: ImageDraw.ImageDraw, frame: int) -> None:
    y = dy(WIRE_Y)
    draw_rx_wire_label(lambda xy, text: draw_text(draw, xy, text, FONTS["label"], fill=COLORS["text"]), y)
    draw_rx_wire(draw, frame, 48, NIC_RECT[0], y, COLORS["wire"], COLORS["header"], pt, s, rgba, box)


def draw_device_memory(draw: ImageDraw.ImageDraw, frame: int) -> None:
    for rect, title, accent in ((HOST_RECT, "Host memory", COLORS["host"]), (GPU_RECT, "NVIDIA GPU memory", COLORS["gpu"])):
        x1, y1, x2, y2 = rect
        draw.rounded_rectangle(box(rect), radius=s(18), fill=COLORS["panel_2"], outline=rgba(accent, 170), width=s(3))
        draw_text(draw, (x1 + 22, y1 + 24), title, FONTS["label"], fill=COLORS["text"], anchor="lt")

    header_slots = {pkt.num for pkt in PACKETS if frame >= pkt.header_end}
    payload_slots = {pkt.num for pkt in PACKETS if frame >= pkt.payload_end}
    draw_memory_row(draw, HEADER_ROW, COLORS["header"], "header slots", header_slots, "h")
    draw_memory_row(draw, PAYLOAD_ROW, COLORS["payload"], "payload slots", payload_slots, "p")


def draw_static_background(draw: ImageDraw.ImageDraw) -> None:
    pass


def draw_kernel_bypass(draw: ImageDraw.ImageDraw) -> None:
    nic_top = ((NIC_RECT[0] + NIC_RECT[2]) / 2, NIC_RECT[1])
    host_entry = ((HOST_RECT[0] + HOST_RECT[2]) / 2, HOST_RECT[1])
    bus_y = (KERNEL_BOX[1] + KERNEL_BOX[3]) / 2
    kernel_in = (KERNEL_BOX[0], bus_y)
    kernel_out = (KERNEL_BOX[2], bus_y)

    stack_color = rgba(str(COLORS["bypass"]), 210)
    to_kernel = rectangular_path((nic_top, (nic_top[0], bus_y), kernel_in))
    from_kernel = rectangular_path((kernel_out, (host_entry[0], bus_y), host_entry))
    draw_dotted_polyline(draw, to_kernel, stack_color, width=2, dash=10, gap=16)
    draw_dotted_polyline(draw, from_kernel, stack_color, width=2, dash=10, gap=16)

    x1, y1, x2, y2 = KERNEL_BOX
    draw.rounded_rectangle(box(KERNEL_BOX), radius=s(12), fill=rgba(COLORS["kernel_fill"], 235), outline=rgba(COLORS["kernel"], 210), width=s(2))
    centered_text(draw, (x1, y1 + 8, x2, y2 - 8), "Linux kernel", FONTS["label"], fill=COLORS["text"])


def draw_dma_paths(base: Image.Image, draw: ImageDraw.ImageDraw, frame: int) -> dict[str, list[tuple[float, float]]]:
    nic_out_x = NIC_RECT[2]
    header_y = (HEADER_ROW[1] + HEADER_ROW[3]) / 2
    payload_y = (PAYLOAD_ROW[1] + PAYLOAD_ROW[3]) / 2
    host_in_x = HEADER_ROW[0]
    bend_x = nic_out_x + 88

    nic_header_y = NIC_RECT[1] + 38
    nic_payload_y = NIC_RECT[3] - 38

    paths = {
        "header": rectangular_path(
            (
                (nic_out_x, nic_header_y),
                (bend_x, nic_header_y),
                (bend_x, header_y),
                (host_in_x, header_y),
            )
        ),
        "payload": rectangular_path(
            (
                (nic_out_x, nic_payload_y),
                (bend_x, nic_payload_y),
                (bend_x, payload_y),
                (host_in_x, payload_y),
            )
        ),
    }
    for key, color in (("header", COLORS["header"]), ("payload", COLORS["payload"])):
        draw_arrow(draw, paths[key], rgba(str(COLORS["arrow"]), 130), 3, 12)
    return paths


def draw_flowing_segments(
    base: Image.Image,
    draw: ImageDraw.ImageDraw,
    frame: int,
    paths: dict[str, list[tuple[float, float]]],
) -> None:
    for pkt in PACKETS:
        if pkt.start <= frame < pkt.header_start:
            incoming = progress_linear(frame, pkt.start, pkt.header_start)
            x = lerp(58, NIC_RECT[2] - PACKET_BAR_WIDTH + 16, incoming)
            y = dy(WIRE_Y - PACKET_BAR_HEIGHT / 2)
            draw_packet_bar(draw, x, y, PACKET_BAR_WIDTH, PACKET_BAR_HEIGHT, alpha=255, packet_num=pkt.num)

        header_p = progress_linear(frame, pkt.header_start, pkt.header_end)
        if 0 < header_p < 1:
            draw_glow_line(base, paths["header"][: max(2, int(len(paths["header"]) * header_p))], COLORS["header"], 70, 8)
            draw_packet_segment(draw, point_on_path(paths["header"], header_p), 76, COLORS["header"], f"h{pkt.num}", f"{HEADER_BYTES} B")

        payload_p = progress_linear(frame, pkt.payload_start, pkt.payload_end)
        if 0 < payload_p < 1:
            draw_glow_line(base, paths["payload"][: max(2, int(len(paths["payload"]) * payload_p))], COLORS["payload"], 70, 8)
            draw_packet_segment(draw, point_on_path(paths["payload"], payload_p), 82, COLORS["payload"], f"p{pkt.num}", f"{PAYLOAD_BYTES} B")


def rgba_frame_to_palette(frame: Image.Image) -> Image.Image:
    rgba = frame.convert("RGBA")
    alpha = rgba.getchannel("A")
    rgb = Image.new("RGB", rgba.size, (0, 0, 0))
    rgb.paste(rgba, mask=alpha)
    palette = rgb.quantize(colors=254, method=Image.Quantize.MEDIANCUT)
    palette.paste(GIF_TRANSPARENCY_INDEX, mask=alpha.point(lambda a: 255 if a < 128 else 0))
    return palette


def render_frame(frame: int) -> Image.Image:
    bg = COLORS.get("bg")
    img = Image.new("RGBA", (WIDTH * SCALE, HEIGHT * SCALE), rgba(str(bg), 255) if bg else TRANSPARENT)
    draw = ImageDraw.Draw(img)
    draw_static_background(draw)
    draw_wire(draw, frame)
    draw_kernel_bypass(draw)
    draw_device_memory(draw, frame)
    paths = draw_dma_paths(img, draw, frame)
    nic_active = any(pkt.nic_arrive - 3 <= frame < pkt.payload_start for pkt in PACKETS)
    draw_chip(draw, NIC_RECT, "NVIDIA NIC", "", COLORS["nvidia"], pulse=0.8 if nic_active else 0.0)
    draw_flowing_segments(img, draw, frame, paths)
    return img.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)


def render_theme(theme: str) -> None:
    global COLORS
    COLORS = THEMES[theme]
    suffix = THEME_OUTPUT_SUFFIX[theme]
    gif_path = OUTPUT_DIR / f"header-data-split{suffix}.gif"
    poster_path = OUTPUT_DIR / f"header-data-split{suffix}-poster.png"

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
    render_frame(min(FRAMES - 1, max(pkt.payload_end for pkt in PACKETS) + 10)).save(poster_path, optimize=True)
    print(f"Wrote {gif_path}")
    print(f"Wrote {poster_path}")


def main() -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    for theme in THEMES:
        render_theme(theme)


if __name__ == "__main__":
    main()
