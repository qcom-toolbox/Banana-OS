#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Intel command descriptors, bounded RX event parsing and ALIVE notifications."""
import argparse
from pathlib import Path
import subprocess
r = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--32', dest='bits32', action='store_true')
p.add_argument('--sanitize', action='store_true')
a = p.parse_args()
out = r/'build/test/iwx-msg'
out.mkdir(parents=True, exist_ok=True)
flags = ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Ikernel', '-Inet',
         '-include', 'web/test/shim/types.h']
if a.bits32:
    flags += ['-m32']
if a.sanitize:
    flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-pie', '-no-pie']
exe = out/'test'
subprocess.run(['cc', *flags, 'tools/tests/iwx_msg.c', 'net/iwx_msg.c', 'net/iwx_fw.c',
                '-o', str(exe)], cwd=r, check=True)
subprocess.run([str(exe)], cwd=r, check=True)
