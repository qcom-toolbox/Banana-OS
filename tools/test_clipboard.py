#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Actual clipboard history under cache pressure, malformed files and concurrent changes."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess

root = Path(__file__).resolve().parents[1]
cases = ['normal', 'pinned', 'race', 'evict', 'io', 'null', 'oom', 'missing', 'limit',
         'overflow', 'huge', 'nodigit', 'truncated', 'nonl', 'badnl', 'oversize']
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--32', dest='bits32', action='store_true')
parser.add_argument('--sanitize', action='store_true')
parser.add_argument('--case', choices=['all', *cases], default='all')
args = parser.parse_args()
out = root / 'build/test/clipboard'
out.mkdir(parents=True, exist_ok=True)
flags = ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Ikernel',
         '-include', 'web/test/shim/types.h']
if args.bits32:
    flags += ['-m32']
if args.sanitize:
    flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all',
              '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
exe = out / 'test'
subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), *flags,
                'tools/tests/clipboard.c', '-o', str(exe)], cwd=root, check=True)
for case in cases if args.case == 'all' else [args.case]:
    subprocess.run([str(exe), case], cwd=root, check=True)
