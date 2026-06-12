#!/usr/bin/env python3
"""Render favicon assets from docs/images/logo.svg.

Requires: pip install cairosvg pillow

Outputs:
  - favicon.svg: rounded teal plate, transparent outside (best for Safari tabs)
  - favicon-32.png / favicon-16.png: PNG fallback for older browsers
  - apple-touch-icon.png: full-bleed teal square for iOS (no transparent corners)

Run from repo root:
  python3 scripts/gen_favicon_assets.py
"""

from __future__ import annotations

import base64
import io
from pathlib import Path

import cairosvg
from PIL import Image, ImageDraw

REPO_ROOT = Path(__file__).resolve().parents[1]
SVG = REPO_ROOT / "docs/images/logo.svg"
OUT_DIR = REPO_ROOT / "docs/images"

CORNER_RATIO = 0.22
LOGO_SCALE = 0.72
PLATE_HEX = "#1a3a48"
PLATE_RGBA = (26, 58, 72, 255)

TAB_ICONS = {
    "favicon-16.png": 16,
    "favicon-32.png": 32,
}
APPLE_TOUCH_SIZE = 180


def logo_raster(size: int) -> Image.Image:
    png = cairosvg.svg2png(
        url=str(SVG),
        output_width=size * 4,
        output_height=int(size * 4 * 220 / 280),
    )
    return Image.open(io.BytesIO(png)).convert("RGBA")


def rounded_png_icon(size: int, opaque_plate: bool) -> Image.Image:
    src = logo_raster(size)
    radius = max(2, int(size * CORNER_RATIO))
    bg = PLATE_RGBA if opaque_plate else (0, 0, 0, 0)
    canvas = Image.new("RGBA", (size, size), bg)
    plate = Image.new("RGBA", (size, size), PLATE_RGBA)
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, size - 1, size - 1), radius=radius, fill=255)
    canvas.paste(plate, (0, 0), mask)

    inner = int(size * LOGO_SCALE)
    inner_h = int(inner * src.height / src.width)
    if inner_h > inner:
        inner_h = inner
        inner = int(inner_h * src.width / src.height)
    resized = src.resize((inner, inner_h), Image.LANCZOS)
    x = (size - inner) // 2
    y = (size - inner_h) // 2
    canvas.paste(resized, (x, y), resized)
    return canvas


def apple_touch_icon(size: int) -> Image.Image:
    src = logo_raster(size)
    canvas = Image.new("RGB", (size, size), PLATE_RGBA[:3])

    inner = int(size * LOGO_SCALE)
    inner_h = int(inner * src.height / src.width)
    if inner_h > inner:
        inner_h = inner
        inner = int(inner_h * src.width / src.height)
    resized = src.resize((inner, inner_h), Image.LANCZOS)
    x = (size - inner) // 2
    y = (size - inner_h) // 2
    canvas.paste(resized, (x, y), resized)
    return canvas


def write_favicon_svg(path: Path, canvas_size: int = 32) -> None:
    icon = rounded_png_icon(canvas_size, opaque_plate=False)
    buf = io.BytesIO()
    icon.save(buf, format="PNG")
    b64 = base64.b64encode(buf.getvalue()).decode("ascii")
    radius = max(2, int(canvas_size * CORNER_RATIO))
    svg = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {canvas_size} {canvas_size}">\n'
        f'  <image width="{canvas_size}" height="{canvas_size}" '
        f'href="data:image/png;base64,{b64}"/>\n'
        "</svg>\n"
    )
    path.write_text(svg, encoding="utf-8")


def main() -> None:
    write_favicon_svg(OUT_DIR / "favicon.svg")
    for name, size in TAB_ICONS.items():
        rounded_png_icon(size, opaque_plate=False).save(OUT_DIR / name)
    apple_touch_icon(APPLE_TOUCH_SIZE).save(OUT_DIR / "apple-touch-icon.png")


if __name__ == "__main__":
    main()
