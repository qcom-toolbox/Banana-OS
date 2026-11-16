#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build and run the browser's offline framework and CSS/JS regression tests.

No network, npm or QEMU needed. The framework suite is also selftest browser
inside Banana OS. Use --32 for the i686 host ABI, --sanitize for ASan/UBSan.
"""
from pathlib import Path
import argparse
import os
import hashlib
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--32', dest='bits32', action='store_true')
parser.add_argument('--sanitize', action='store_true')
parser.add_argument('--swm', action='store_true', help='also test live https://swm.mxtpasontop.fr/ (network + libcurl development files required)')
args = parser.parse_args()
out = root / 'build/test/browser' / ('32' if args.bits32 else '64')
if args.sanitize:
    out = out.with_name(out.name + '-sanitize')
out.mkdir(parents=True, exist_ok=True)
fixtures = root / 'web/fixtures'
for entry in (fixtures / 'SHA256SUMS').read_text().splitlines():
    digest, name = entry.split()
    if hashlib.sha256((fixtures / name).read_bytes()).hexdigest() != digest:
        raise RuntimeError(f'{name}: pinned upstream fixture changed')
flags = ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Wno-psabi',
         '-Iweb/test/shim', '-Iweb', '-Ikernel']
if args.bits32:
    flags += ['-m32', '-mstackrealign']
if args.sanitize:
    flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
env = dict(os.environ, FONTDIR=str(root / 'fonts'))
if args.sanitize:
    # The host font loader holds its font files until process exit.
    env['ASAN_OPTIONS'] = 'detect_leaks=0:halt_on_error=1'
    env['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'


def run(command, **kwargs):
    return subprocess.run([str(x) for x in command], cwd=root, env=env, check=True, **kwargs)


def build(name, sources, extra=()):
    binary = out / name
    run([os.environ.get('CC', 'cc'), *flags, *extra, *sources, '-lm', '-o', binary])
    return binary


for name, sources in (
    ('selectors', ['web/test/selectors_test.c', 'web/arena.c', 'web/html.c', 'web/css.c']),
    ('variables', ['web/test/css_vars_test.c', 'web/arena.c', 'web/html.c', 'web/css.c']),
    ('math', ['web/test/css_math_test.c', 'web/arena.c', 'web/html.c']),
):
    run([build(name, sources)])

scripts = ['web/script.c', 'web/script_lib.c', 'web/script_es.c',
           'web/script_bigint.c', 'web/script_typed.c', 'web/regex.c']
script = build('script', ['web/test/script_test.c', 'web/arena.c', *scripts])
for fixture in sorted((root / 'web/test/cases').glob('framework-*.js')):
    result = run([script, 'js', fixture], capture_output=True, text=True)
    expected = fixture.stem + ': PASS'
    if result.stdout.strip() != expected:
        raise RuntimeError(f'{fixture.name}: unexpected result: {result.stdout!r}')
    print(expected, flush=True)
for fixture in ('basic.js', 'es.js', 'basic.php'):
    run([script, 'php' if fixture.endswith('.php') else 'js', 'web/test/cases/' + fixture],
        stdout=subprocess.DEVNULL)

# Build the exact vendored scalar WebP decoder, never the host's libwebp.
webp_objects = []
for source in sorted((root / 'third_party/libwebp/src').glob('*/*.c')):
    obj = out / ('webp-' + source.parent.name + '-' + source.stem + '.o')
    run([os.environ.get('CC', 'cc'), *flags, '-DHAVE_CONFIG_H', '-DNDEBUG',
         '-Ithird_party/libwebp', '-c', source, '-o', obj])
    webp_objects.append(obj)
images = ['web/test/image_host.c', 'web/test/image_stb_host.c', 'web/image_decode.c', *webp_objects]

obj = out / 'frameworkbin.o'
run(['nasm', '-f', 'elf32' if args.bits32 else 'elf64', 'kernel/frameworkbin.asm', '-o', obj])
engine = ['web/test/framework_test.c', 'web/test/font_host.c', 'web/framework_test.c',
          'web/framework_fixtures.c', obj, 'web/arena.c', 'web/html.c', 'web/css.c',
          'web/layout.c', 'web/render.c', 'web/page.c', 'web/jsdom.c', 'web/canvas.c', *scripts, *images,
          'web/svg.c', 'kernel/font.c', 'kernel/ttf.c']
run([build('frameworks', engine, ['-DFONT_HOST'])])
if args.swm:
    curl_flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'libcurl'], text=True).split()
    live_sources = ['web/test/site_test.c', 'web/test/font_host.c', 'web/site_test.c',
                    'web/arena.c', 'web/html.c', 'web/css.c', 'web/layout.c', 'web/render.c',
                    'web/page.c', 'web/jsdom.c', 'web/canvas.c', *scripts, *images, 'web/svg.c', 'kernel/font.c', 'kernel/ttf.c']
    binary = out / 'swm'
    run([os.environ.get('CC', 'cc'), *flags, '-DFONT_HOST', *live_sources,
         '-lm', *curl_flags, '-o', binary])
    preview = out / 'swm.ppm'
    result = subprocess.run([str(binary), str(preview)], cwd=root, env=env)
    if preview.exists():
        import struct
        import zlib
        with preview.open('rb') as f:
            assert f.readline() == b'P6\n'
            width, height = map(int, f.readline().split())
            assert f.readline() == b'255\n'
            pixels = f.read()
        def chunk(kind, data):
            return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
        scanlines = b''.join(b'\0' + pixels[y*width*3:(y+1)*width*3] for y in range(height))
        png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
        png += chunk(b'IDAT', zlib.compress(scanlines)) + chunk(b'IEND', b'')
        preview.with_suffix('.png').write_bytes(png)
        print('SWM engine preview:', preview.with_suffix('.png'), flush=True)
    if result.returncode:
        raise SystemExit(result.returncode)
print('All browser checks passed (' + ('32' if args.bits32 else '64') + '-bit).')
