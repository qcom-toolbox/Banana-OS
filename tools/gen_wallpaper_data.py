#!/usr/bin/env python3
"""Regenerate kernel/wallpaper_data.c from the images in assets/wallpapers.

The kernel has no image decoder (JPEG or PNG - and freestanding, ring-0 C is
a bad place to write one), so wallpaper pixel data is decoded once here, at
build time, on the host, and baked directly into a C source file as plain
byte arrays. The kernel just blits it - no runtime image decoding at all.

Requires macOS's `sips` (used only to downsample + convert PNG/JPEG -> 24bpp
BMP, which stdlib alone can't do). If you're not on macOS, swap the sips call
below for an equivalent resize/convert step from any image tool - the BMP
parsing and C-array emission below are pure stdlib and don't care how the
BMP was produced.

Usage: python3 tools/gen_wallpaper_data.py
(run from the repo root; writes kernel/wallpaper_data.c in place)
"""
import os
import struct
import subprocess
import sys
import tempfile

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIR = os.path.join(REPO_ROOT, "assets", "wallpapers")
OUT_PATH = os.path.join(REPO_ROOT, "kernel", "wallpaper_data.c")

SOURCE_EXTS = (".png", ".jpg", ".jpeg")

# Must match WALLPAPER_IMG_W/H in kernel/wallpaper_data.h. 200x150 divides
# the 800x600 desktop exactly (4x upscale, no fractional scaling) - high
# enough that gui.c's bilinear upscale holds up for real photos/fine detail,
# while still keeping the embedded kernel image size reasonable.
IMG_W = 200
IMG_H = 150


def image_to_bmp(src_path, bmp_path):
    subprocess.run(
        ["sips", "-s", "format", "bmp", src_path,
         "--out", bmp_path, "--resampleHeightWidth", str(IMG_H), str(IMG_W)],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )


def read_bmp_rgb(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data[0:2] == b"BM", "not a BMP"
    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    width = struct.unpack_from("<i", data, 18)[0]
    height = struct.unpack_from("<i", data, 22)[0]
    bpp = struct.unpack_from("<H", data, 28)[0]
    assert bpp == 24, f"expected 24bpp BMP, got {bpp}"
    top_down = height < 0
    h = abs(height)
    row_size = ((width * 3 + 3) // 4) * 4  # BMP rows are padded to 4 bytes
    pixels = bytearray(width * h * 3)
    for row in range(h):
        src_row = row if top_down else (h - 1 - row)
        off = pixel_offset + src_row * row_size
        for x in range(width):
            b, g, r = data[off + x * 3], data[off + x * 3 + 1], data[off + x * 3 + 2]
            dst = (row * width + x) * 3
            pixels[dst], pixels[dst + 1], pixels[dst + 2] = r, g, b
    return width, h, bytes(pixels)


def c_ident(name):
    return name.replace("-", "_").lower()


def emit_array(lines, ident, pixels):
    lines.append(f"const uint8_t wallpaper_{ident}[{len(pixels)}] = {{")
    for i in range(0, len(pixels), 20):
        lines.append(",".join(str(b) for b in pixels[i:i + 20]) + ",")
    lines.append("};")
    lines.append("")


def find_sources():
    found = {}
    for f in os.listdir(SRC_DIR):
        base, ext = os.path.splitext(f)
        if ext.lower() in SOURCE_EXTS:
            found[base] = f
    return dict(sorted(found.items()))


def main():
    sources = find_sources()
    if not sources:
        print(f"no wallpaper images found in {SRC_DIR}", file=sys.stderr)
        sys.exit(1)

    lines = [
        "/* Auto-generated from the images in assets/wallpapers by",
        " * tools/gen_wallpaper_data.py - do not hand-edit. Re-run that",
        " * script after changing/adding wallpaper images. */",
        '#include "wallpaper_data.h"',
        "",
    ]

    with tempfile.TemporaryDirectory() as tmp:
        for name, filename in sources.items():
            src_path = os.path.join(SRC_DIR, filename)
            bmp_path = os.path.join(tmp, name + ".bmp")
            image_to_bmp(src_path, bmp_path)
            w, h, pixels = read_bmp_rgb(bmp_path)
            assert (w, h) == (IMG_W, IMG_H), f"{name}: got {w}x{h}, expected {IMG_W}x{IMG_H}"
            ident = c_ident(name)
            emit_array(lines, ident, pixels)
            print(f"  {filename}: wallpaper_{ident} {w}x{h} ({len(pixels)} bytes)")

    with open(OUT_PATH, "w") as f:
        f.write("\n".join(lines))
    print(f"wrote {OUT_PATH} ({len(sources)} wallpapers)")


if __name__ == "__main__":
    main()
