#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Users on Linux: kernel/fs.c's permissions, users.c, passwd.c, elevate.c
(tools/tests/users.c)."""
import argparse, os, shlex, subprocess
from pathlib import Path
root = Path(__file__).resolve().parents[1]
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--32', dest='bits32', action='store_true')
ap.add_argument('--sanitize', action='store_true')
a = ap.parse_args()
out = root / 'build/test/users'; out.mkdir(parents=True, exist_ok=True)
cc = shlex.split(os.environ.get('CC', 'cc'))
flags = ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections',
         '-Ikernel', '-include', 'web/test/shim/types.h']
if a.bits32: flags += ['-m32']
if a.sanitize: flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-pie', '-no-pie']
objs = []
# (the kernel's string routines under other names: the host's memcpy stays the sanitizer's)
for src, extra in (('kernel/kstring.c', ['-Dmemcpy=kernel_memcpy', '-Dmemmove=kernel_memmove', '-Dmemset=kernel_memset', '-Dmemcmp=kernel_memcmp']),
                   ('kernel/users.c', []), ('kernel/passwd.c', []), ('kernel/elevate.c', []), ('crypto/sha256.c', [])):
    o = out / (Path(src).stem + '.o')
    subprocess.run([*cc, *flags, *extra, '-c', src, '-o', str(o)], cwd=root, check=True)
    objs.append(str(o))
exe = out / 'test'
subprocess.run([*cc, *flags, 'tools/tests/users.c', *objs, '-Wl,--gc-sections', '-o', str(exe)], cwd=root, check=True)
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
subprocess.run([str(exe)], cwd=root, check=True, env=env)
