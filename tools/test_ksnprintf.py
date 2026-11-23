#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""kernel/kstring.c (ksnprintf, memcpy, memmove, memset, memcmp) on Linux,
checked against the C library: tools/tests/ksnprintf.c."""
import argparse, os, shlex, subprocess
from pathlib import Path
root = Path(__file__).resolve().parents[1]
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--32', dest='bits32', action='store_true')
ap.add_argument('--sanitize', action='store_true')
a = ap.parse_args()
out = root / 'build/test/ksnprintf'; out.mkdir(parents=True, exist_ok=True)
cc = shlex.split(os.environ.get('CC', 'cc'))
arch = ['-m32'] if a.bits32 else []
# the kernel's own flags (freestanding, its own types.h); then every symbol gets a kt_ prefix
kflags = [*arch, '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-ffreestanding', '-fno-builtin',
          '-nostdinc', '-fno-pie', '-fno-stack-protector', '-Ikernel']
obj = out / 'kstring.o'
subprocess.run([*cc, *kflags, '-c', 'kernel/kstring.c', '-o', str(obj)], cwd=root, check=True)
subprocess.run(['objcopy', '--prefix-symbols=kt_', str(obj)], check=True)
tflags = [*arch, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-format', '-fno-pie', '-no-pie']
if a.sanitize:
    tflags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
exe = out / 'test'
subprocess.run([*cc, *tflags, 'tools/tests/ksnprintf.c', str(obj), '-o', str(exe)], cwd=root, check=True)
subprocess.run([str(exe)], cwd=root, check=True)
