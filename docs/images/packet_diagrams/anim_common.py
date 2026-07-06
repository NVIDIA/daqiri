from __future__ import annotations

from pathlib import Path
from collections.abc import Sequence

from PIL import Image, ImageFont, features


WEBP_QUALITY = 80
WEBP_METHOD = 0


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
    if not features.check("webp") or not features.check("webp_anim"):
        raise RuntimeError("Pillow must be built with animated WebP support to render packet diagrams")


def save_webp_animation(frames: Sequence[Image.Image], path: Path, duration_ms: int) -> None:
    if not frames:
        raise ValueError("cannot save animation with no frames")
    require_webp_animation_support()

    rgba_frames = [frame.convert("RGBA") for frame in frames]
    tmp_path = path.with_name(f".{path.name}.tmp")
    if tmp_path.exists():
        tmp_path.unlink()
    # libwebp may coalesce identical consecutive frames; verify outputs with
    # Makefile's reproducibility check rather than decoded frame counts.
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
