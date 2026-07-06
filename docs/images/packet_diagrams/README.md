# Packet Diagram Animations

These scripts generate the animated packet-path diagrams used by the DAQIRI docs.

## Requirements

- Python 3
- Pillow 11.1.x with animated WebP support
- DejaVu Sans or Liberation Sans fonts installed in a standard system font path

Check the encoder support with:

```bash
python3 - <<'PY'
from PIL import Image, features
print("Pillow", Image.__version__)
print("webp", features.check("webp"))
print("webp_anim", features.check("webp_anim"))
PY
```

## Regenerate

From the repository root:

```bash
make -C docs/images/packet_diagrams
```

Before committing, run:

```bash
make -C docs/images/packet_diagrams check
```

The check regenerates the assets and fails if any generated output directory
has uncommitted changes afterward.

Or run one generator directly:

```bash
python3 docs/images/packet_diagrams/hds_animation.py
python3 docs/images/packet_diagrams/flow_steering_animation.py
python3 docs/images/packet_diagrams/reorder_animation.py
```

Each generator writes animated WebP files and PNG posters into its adjacent output directory. The reorder generator emits both the reorder-only and reorder-plus-convert variants.

Animated WebP encoders may coalesce identical consecutive frames, so a decoded
WebP frame count can be lower than the script's logical frame count. Use
`make check` to verify reproducibility rather than comparing decoded frame
counts directly.

## Size Notes

`anim_common.py` currently uses `WEBP_METHOD = 0` to keep local regeneration
fast. Raising the method is still available when smaller artifacts matter more
than encoder time; expect roughly another 20-35% size reduction depending on the
animation. Run `make check` after changing it.

The PR history before the WebP conversion contains large GIF blobs across the
animation commits. Squash before merging so those transient GIF binaries do not
remain in the main branch history.
