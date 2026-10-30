#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Intel command/RX ownership tests against pinned firmware and register models."""
import argparse
from pathlib import Path
import subprocess
r = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--32', dest='bits32', action='store_true')
p.add_argument('--sanitize', action='store_true')
a = p.parse_args()
out = r/'build/test/iwx-queue'
out.mkdir(parents=True, exist_ok=True)
flags = ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Ikernel', '-Inet',
         '-include', 'web/test/shim/types.h']
if a.bits32:
    flags += ['-m32']
if a.sanitize:
    flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-pie', '-no-pie']
exe = out/'test'
subprocess.run(['cc', *flags, 'tools/tests/iwx_queue.c', 'net/iwx_queue.c', 'net/iwx_init.c', 'net/iwx_msg.c',
                'net/iwx_fw.c', 'net/iwx_boot.c', 'net/iwx_hw.c', 'net/iwx_nvm.c', 'net/iwx_pnvm.c',
                '-o', str(exe)], cwd=r, check=True)
for model, name in [(200, 'iwlwifi-cc-a0-77.ucode'), (210, 'iwlwifi-ty-a0-gf-a0-77.ucode')]:
    subprocess.run([str(exe), str(r/'firmware/intel'/name), str(model),
                    str(r/'firmware/intel/iwlwifi-ty-a0-gf-a0.pnvm')], cwd=r, check=True)
