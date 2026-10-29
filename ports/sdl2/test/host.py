#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the actual adapter with host libc and deterministic Banana stubs."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
sdk = root.parents[1] / 'sdk'
with tempfile.TemporaryDirectory(prefix='banana-sdl2-') as temp:
    for bits, sanitize in [(64, True), (32, False)]:
        flags = ['-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
                 '-I' + str(root / 'include'), '-idirafter', str(sdk / 'include'),
                 '-Dclock_gettime=sdl_test_clock_gettime', '-m' + str(bits)]
        if sanitize:
            flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
        exe = str(Path(temp) / ('test' + str(bits)))
        subprocess.run(['cc', *flags, str(root / 'src/sdl2_banana.c'),
                        str(root / 'test/host.c'), '-o', exe], check=True)
        subprocess.run([exe], check=True)
