#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""usb/uvc.c (webcams) against a model camera: tools/tests/uvc.c. The
MJPEG picture is a real JPEG (Pillow) with its Huffman tables taken out,
as cameras send it."""
import argparse, io, os, shlex, subprocess
from pathlib import Path
root = Path(__file__).resolve().parents[1]
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--32', dest='bits32', action='store_true')
ap.add_argument('--sanitize', action='store_true')
a = ap.parse_args()
out = root / 'build/test/uvc'; out.mkdir(parents=True, exist_ok=True)

def jpeg():
    """a 640x480 baseline JPEG, 4:2:0, the standard tables (not optimized)"""
    from PIL import Image
    img = Image.new('RGB', (640, 480))
    px = img.load()
    for y in range(480):
        for x in range(640):
            px[x, y] = ((x * 255) // 639, (y * 255) // 479, ((x ^ y) * 7) & 255)
    b = io.BytesIO()
    img.save(b, 'JPEG', quality=85, optimize=False, subsampling=2)
    return b.getvalue()

def strip_dht(j):
    o, i = bytearray(j[:2]), 2
    while i < len(j):
        assert j[i] == 0xFF
        m = j[i + 1]
        if m == 0xDA:
            o += j[i:]
            break
        n = (j[i + 2] << 8) | j[i + 3]
        if m != 0xC4: o += j[i:i + 2 + n]
        i += 2 + n
    return bytes(o)

j = jpeg()
(out / 'orig.jpg').write_bytes(j)
(out / 'mjpeg.bin').write_bytes(strip_dht(j))
cc = shlex.split(os.environ.get('CC', 'cc'))
flags = ['-DUVC_HOST_TEST', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Ikernel', '-Iusb', '-fno-pie', '-no-pie']
if a.bits32: flags += ['-m32']
if a.sanitize: flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
host = out / 'host.o'
subprocess.run([*cc, *[f for f in flags if f not in ('-Ikernel', '-Iusb', '-no-pie')], '-w', '-c',
                'tools/tests/uvc_host.c', '-o', str(host)], cwd=root, check=True)
exe = out / 'test'
subprocess.run([*cc, *flags, 'tools/tests/uvc.c', str(host), '-lm', '-o', str(exe)], cwd=root, check=True)
subprocess.run([str(exe), str(out)], cwd=root, check=True)
