#!/usr/bin/env python3
"""
Generates packaging/AppIcon.icns (macOS) and packaging/AppIcon.ico (Windows,
used by the Inno Setup installer) for BeamCommander3: a dark "stage"
backdrop with a multicolor laser fan, echoing the app's actual purpose
(laser show control) and its purple brand color from the frontend favicon.
Run once locally; the resulting files are committed to the repo and just
copied into the packages by the release workflow (no icon-generation
tooling needed in CI).

Usage: /path/to/venv/bin/python3 packaging/make_icon.py
"""
import math
import os
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFilter

SIZE = 1024
OUT_DIR = os.path.dirname(os.path.abspath(__file__))


def rounded_square_mask(size, radius):
    mask = Image.new("L", (size, size), 0)
    d = ImageDraw.Draw(mask)
    d.rounded_rectangle([0, 0, size - 1, size - 1], radius=radius, fill=255)
    return mask


def make_base():
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))

    # Background gradient: near-black at top -> deep violet at bottom.
    bg = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 255))
    top = (10, 8, 18)
    bottom = (46, 12, 82)
    for y in range(SIZE):
        t = y / (SIZE - 1)
        r = int(top[0] + (bottom[0] - top[0]) * t)
        g = int(top[1] + (bottom[1] - top[1]) * t)
        b = int(top[2] + (bottom[2] - top[2]) * t)
        ImageDraw.Draw(bg).line([(0, y), (SIZE, y)], fill=(r, g, b, 255))

    mask = rounded_square_mask(SIZE, int(SIZE * 0.225))
    img.paste(bg, (0, 0), mask)
    return img, mask


def add_laser_fan(img):
    beams_layer = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    origin = (SIZE * 0.5, SIZE * 0.92)
    length = SIZE * 1.25
    beams = [
        (-52, (255, 59, 59)),    # red
        (-26, (255, 43, 214)),   # magenta
        (0,   (134, 59, 255)),   # violet (brand color)
        (26,  (43, 245, 255)),   # cyan
        (52,  (57, 255, 106)),   # green
    ]
    half_width_deg = 3.2
    for angle_deg, color in beams:
        draw = ImageDraw.Draw(beams_layer, "RGBA")
        a1 = math.radians(angle_deg - half_width_deg - 90)
        a2 = math.radians(angle_deg + half_width_deg - 90)
        p1 = origin
        p2 = (origin[0] + length * math.cos(a1), origin[1] + length * math.sin(a1))
        p3 = (origin[0] + length * math.cos(a2), origin[1] + length * math.sin(a2))
        draw.polygon([p1, p2, p3], fill=color + (150,))

    beams_layer = beams_layer.filter(ImageFilter.GaussianBlur(SIZE * 0.012))
    img.alpha_composite(beams_layer)

    # Bright emitter glow at the origin point.
    glow = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    r = SIZE * 0.10
    gd.ellipse([origin[0] - r, origin[1] - r, origin[0] + r, origin[1] + r],
               fill=(255, 255, 255, 235))
    glow = glow.filter(ImageFilter.GaussianBlur(SIZE * 0.02))
    img.alpha_composite(glow)

    core = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    cd = ImageDraw.Draw(core)
    r2 = SIZE * 0.035
    cd.ellipse([origin[0] - r2, origin[1] - r2, origin[0] + r2, origin[1] + r2],
               fill=(255, 255, 255, 255))
    img.alpha_composite(core)
    return img


def main():
    img, mask = make_base()
    img = add_laser_fan(img)
    img.putalpha(mask)

    iconset = os.path.join(OUT_DIR, "AppIcon.iconset")
    os.makedirs(iconset, exist_ok=True)
    sizes = [16, 32, 128, 256, 512]
    for s in sizes:
        img.resize((s, s), Image.LANCZOS).save(os.path.join(iconset, f"icon_{s}x{s}.png"))
        img.resize((s * 2, s * 2), Image.LANCZOS).save(os.path.join(iconset, f"icon_{s}x{s}@2x.png"))

    icns_path = os.path.join(OUT_DIR, "AppIcon.icns")
    subprocess.run(["iconutil", "-c", "icns", iconset, "-o", icns_path], check=True)

    ico_path = os.path.join(OUT_DIR, "AppIcon.ico")
    ico_sizes = [(16, 16), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
    img.save(ico_path, sizes=ico_sizes)

    preview_path = os.path.join(OUT_DIR, "AppIcon.png")
    img.save(preview_path)

    print(f"Wrote {icns_path}, {ico_path}, and {preview_path}")


if __name__ == "__main__":
    sys.exit(main())
