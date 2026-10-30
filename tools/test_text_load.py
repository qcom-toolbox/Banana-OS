#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise real text buffers and Notepad file loading under faults."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
root = Path(__file__).resolve().parents[1]
cases = ['normal', 'bounds', 'oom', 'limits', 'cache', 'io', 'null', 'binary', 'load-oom', 'new-oom']
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--32', dest='bits32', action='store_true')
p.add_argument('--sanitize', action='store_true')
p.add_argument('--case', choices=['all', *cases], default='all')
a = p.parse_args()
out = root / 'build/test/text-load'
out.mkdir(parents=True, exist_ok=True)
flags = ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-ffunction-sections', '-fdata-sections', '-Ikernel', '-Inet',
         '-include', 'web/test/shim/types.h']
if a.bits32:
    flags += ['-m32']
if a.sanitize:
    flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all',
              '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
exe = out / 'test'
subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), *flags,
                'tools/tests/text_load.c', '-Wl,--gc-sections', '-o', str(exe)],
               cwd=root, check=True)
for case in cases if a.case == 'all' else [a.case]:
    subprocess.run([str(exe), case], cwd=root, check=True)
