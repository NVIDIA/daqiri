from __future__ import annotations

from pathlib import Path
from collections.abc import Sequence

from PIL import Image, ImageFont, features


WEBP_QUALITY = 90
WEBP_METHOD = 0

CANVAS_WIDTH = 1180
CANVAS_HEIGHT = 660
CANVAS_SCALE = 2
OUTPUT_WIDTH = CANVAS_WIDTH * CANVAS_SCALE
OUTPUT_HEIGHT = CANVAS_HEIGHT * CANVAS_SCALE
FRAME_DURATION_MS = 40

ROUTE_LINE_WIDTH = 4
ROUTE_ARROW_SIZE = 12
PANEL_CORNER_RADIUS = 18
PANEL_OUTLINE_WIDTH = 3
PANEL_TITLE_X = 22
PANEL_TITLE_Y = 22
NIC_CORNER_RADIUS = 18
KERNEL_CORNER_RADIUS = 12
WIRE_DOT_COLOR = "#ffcf5a"

CANVAS_FILL: tuple[int, int, int, int] = (0, 0, 0, 0)

PANEL_FILL = "#152338"
PANEL_SLOT_FILL = "#1e3348"

SHARED_ACCENTS = {
    "nvidia": "#76b900",
    "host": "#9b8cff",
    "gpu": "#59d4ff",
    "kernel": "#3b82f6",
    "ink": "#07111f",
    "wire_dot": WIRE_DOT_COLOR,
}

_BASE = {
    "bg": "#ffffff",
    "panel": PANEL_FILL,
    "panel_2": PANEL_FILL,
    "panel_slot": PANEL_SLOT_FILL,
    "line": "#5b7390",
    "line_soft": "#4a5663",
    "text": "#f6fbff",
    "canvas_text": "#111827",
    "muted": "#b0bcc9",
    "wire": "#475569",
    "white": "#ffffff",
    "shadow": "#252c35",
    "row_bg": PANEL_FILL,
    "kernel_fill": PANEL_FILL,
    "route": "#334155",
    "arrow": "#334155",
    "stroke": "#334155",
    "bar_glow": "#64748b",
    "bypass": "#475569",
    "tag_fill": PANEL_FILL,
    "slot_empty": "#94a3b8",
    "nic_panel": PANEL_FILL,
    "nic_text": "#f6fbff",
    "nic_line": "#5b7390",
}


def diagram_colors(**accents: str) -> dict[str, str]:
    colors = dict(_BASE)
    colors.update(SHARED_ACCENTS)
    colors.update(accents)
    return colors


def output_paths(output_dir: Path, base_name: str) -> tuple[Path, Path, Path]:
    return (
        output_dir / f"{base_name}.webp",
        output_dir / f"{base_name}.gif",
        output_dir / f"{base_name}-poster.png",
    )


def font(size: int, *, scale: int = 2, bold: bool = False, mono: bool = False) -> ImageFont.FreeTypeFont:
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
            return ImageFont.truetype(path, size * scale)
    raise FileNotFoundError(f"missing required font; tried: {', '.join(candidates)}")


def font_scheme(scale: int = 2) -> dict[str, ImageFont.FreeTypeFont]:
    return {
        "label": font(18, scale=scale, bold=True),
        "small": font(14, scale=scale),
        "tiny": font(12, scale=scale),
        "chip": font(21, scale=scale, bold=True),
        "badge": font(15, scale=scale, bold=True),
        "nic_center": font(20, scale=scale, bold=True),
        "packet": font(18, scale=scale, bold=True),
        "queue": font(18, scale=scale, bold=True),
        "slot": font(18, scale=scale, bold=True),
    }


def require_webp_animation_support() -> None:
    if not features.check("webp"):
        raise RuntimeError("Pillow must be built with WebP support to render packet diagrams")


def save_webp_animation(frames: Sequence[Image.Image], path: Path, duration_ms: int) -> None:
    if not frames:
        raise ValueError("cannot save animation with no frames")
    require_webp_animation_support()

    rgba_frames = [frame.convert("RGBA") for frame in frames]
    tmp_path = path.with_name(f".{path.name}.tmp")
    if tmp_path.exists():
        tmp_path.unlink()
    rgba_frames[0].save(
        tmp_path,
        format="WEBP",
        save_all=True,
        append_images=rgba_frames[1:],
        duration=duration_ms,
        loop=0,
        lossless=False,
        quality=WEBP_QUALITY,
        method=WEBP_METHOD,
    )
    tmp_path.replace(path)


GIF_TRANSPARENT_INDEX = 255
GIF_MATTE = (255, 255, 255)


def _flatten_rgba_for_gif(frame: Image.Image) -> tuple[Image.Image, Image.Image]:
    rgba = frame.convert("RGBA")
    alpha = rgba.getchannel("A")
    flat = Image.new("RGB", rgba.size, GIF_MATTE)
    flat.paste(rgba, mask=alpha)
    return alpha, flat


def _build_gif_palette(frames: Sequence[Image.Image]) -> Image.Image:
    if not frames:
        raise ValueError("cannot build palette with no frames")
    sample_count = min(8, len(frames))
    if sample_count == 1:
        sample_indices = [0]
    else:
        sample_indices = sorted(
            {int(round(i * (len(frames) - 1) / (sample_count - 1))) for i in range(sample_count)}
        )
    flat_rgbs = [_flatten_rgba_for_gif(frames[i])[1] for i in sample_indices]
    width, height = flat_rgbs[0].size
    combined = Image.new("RGB", (width, height * len(flat_rgbs)), GIF_MATTE)
    for index, rgb in enumerate(flat_rgbs):
        combined.paste(rgb, (0, index * height))
    return combined.quantize(colors=254, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)


def rgba_to_gif_frame(frame: Image.Image, *, palette_frame: Image.Image) -> Image.Image:
    rgba = frame.convert("RGBA")
    alpha = rgba.getchannel("A")
    flat = Image.new("RGB", rgba.size, GIF_MATTE)
    flat.paste(rgba, mask=alpha)
    palette_img = flat.quantize(palette=palette_frame, dither=Image.Dither.NONE)
    palette_img = palette_img.convert("P")
    transparent_mask = alpha.point(lambda value: 255 if value < 128 else 0)
    palette_img.paste(GIF_TRANSPARENT_INDEX, mask=transparent_mask)
    return palette_img


def save_gif_animation(frames: Sequence[Image.Image], path: Path, duration_ms: int) -> None:
    if not frames:
        raise ValueError("cannot save animation with no frames")

    palette_frame = _build_gif_palette(frames)
    gif_frames = [rgba_to_gif_frame(frame, palette_frame=palette_frame) for frame in frames]
    tmp_path = path.with_name(f".{path.name}.tmp")
    if tmp_path.exists():
        tmp_path.unlink()
    durations = [duration_ms] * len(gif_frames)
    gif_frames[0].save(
        tmp_path,
        format="GIF",
        save_all=True,
        append_images=gif_frames[1:],
        duration=durations,
        loop=0,
        disposal=2,
        transparency=GIF_TRANSPARENT_INDEX,
        optimize=False,
    )
    tmp_path.replace(path)
