#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Test the actual interactive editor harness with delayed PTY mode/redraw."""
import argparse
from pathlib import Path
import subprocess
r = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--32', dest='bits32', action='store_true')
p.add_argument('--sanitize', action='store_true')
a = p.parse_args()
out = r / 'build/test/editor-sync'
out.mkdir(parents=True, exist_ok=True)
flags = ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=3']
if a.bits32:
    flags += ['-m32']
if a.sanitize:
    flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all',
              '-fno-pie', '-no-pie']
exe = out / 'test'
subprocess.run(['cc', *flags, 'tools/tests/editor_sync.c', '-lutil', '-o', str(exe)],
               cwd=r, check=True)
subprocess.run([str(exe)], cwd=r, check=True)
