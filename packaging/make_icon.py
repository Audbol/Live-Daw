#!/usr/bin/env python3
"""Generates Sidecar's app icon (original artwork): an "S" built from two fader
tracks joined by three bars, with a fader cap on each track.

Writes resources/icon/sidecar.svg and, rendered from the same geometry with
Pillow, sidecar-1024.png / sidecar-256.png (JUCE ICON_BIG / ICON_SMALL, which
JUCE turns into the .icns and the .exe icon), sidecar.ico (installer) and the
PNG sizes for the Linux hicolor theme. Run: python3 packaging/make_icon.py
"""
from pathlib import Path

from PIL import Image, ImageDraw

GRID = 1024
OUT = Path(__file__).resolve().parents[1] / "resources" / "icon"

BACKGROUND_TOP, BACKGROUND_BOTTOM = (52, 58, 66), (24, 27, 31)
TRACK = (255, 140, 30)      # the S
CAP = (226, 229, 233)       # fader caps
CAP_LINE = (40, 44, 50)

# Rounded square behind everything: x, y, width, height, corner radius (1024 grid).
BACKGROUND = (40, 40, 944, 944, 200)
# x, y, width, height, corner radius, colour.
SHAPES = [
    (250, 210, 524, 92, 46, TRACK),    # top bar
    (250, 210, 92, 352, 46, TRACK),    # upper-left track
    (250, 470, 524, 92, 46, TRACK),    # middle bar
    (682, 470, 92, 352, 46, TRACK),    # lower-right track
    (250, 730, 524, 92, 46, TRACK),    # bottom bar
    (186, 342, 220, 96, 22, CAP),      # cap on the upper track
    (618, 598, 220, 96, 22, CAP),      # cap on the lower track
    (206, 384, 180, 12, 6, CAP_LINE),  # cap centre lines
    (638, 640, 180, 12, 6, CAP_LINE),
]


def hex_colour(c):
    return "#%02x%02x%02x" % c


def write_svg():
    x, y, w, h, r = BACKGROUND
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {GRID} {GRID}" width="{GRID}" height="{GRID}">',
        '  <defs><linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">'
        f'<stop offset="0" stop-color="{hex_colour(BACKGROUND_TOP)}"/>'
        f'<stop offset="1" stop-color="{hex_colour(BACKGROUND_BOTTOM)}"/></linearGradient></defs>',
        f'  <rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{r}" fill="url(#bg)"/>',
    ]
    for x, y, w, h, r, c in SHAPES:
        parts.append(f'  <rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{r}" fill="{hex_colour(c)}"/>')
    parts.append("</svg>")
    (OUT / "sidecar.svg").write_text("\n".join(parts) + "\n")


def render(size):
    scale = 4  # supersample, then downscale for anti-aliasing
    s = size * scale / GRID
    img = Image.new("RGBA", (size * scale, size * scale), (0, 0, 0, 0))

    # Vertical gradient, masked to the rounded square.
    x, y, w, h, r = BACKGROUND
    gradient = Image.new("RGBA", img.size)
    gd = ImageDraw.Draw(gradient)
    for row in range(img.size[1]):
        t = min(max((row / s - y) / h, 0.0), 1.0)
        colour = tuple(round(a + (b - a) * t) for a, b in zip(BACKGROUND_TOP, BACKGROUND_BOTTOM))
        gd.line([(0, row), (img.size[0], row)], fill=colour + (255,))
    mask = Image.new("L", img.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([x * s, y * s, (x + w) * s - 1, (y + h) * s - 1], radius=r * s, fill=255)
    img.paste(gradient, (0, 0), mask)

    d = ImageDraw.Draw(img)
    for x, y, w, h, r, c in SHAPES:
        box = [x * s, y * s, max(x * s, (x + w) * s - 1), max(y * s, (y + h) * s - 1)]
        d.rounded_rectangle(box, radius=r * s, fill=c + (255,))
    return img.resize((size, size), Image.LANCZOS)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    write_svg()
    big = render(1024)
    big.save(OUT / "sidecar-1024.png", optimize=True)
    for size in (16, 32, 48, 64, 128, 256, 512):
        render(size).save(OUT / f"sidecar-{size}.png", optimize=True)
    big.save(OUT / "sidecar.ico", sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])


if __name__ == "__main__":
    main()
