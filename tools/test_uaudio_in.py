#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""USB microphones (usb/uaudio.c's recording) and kernel/audioin.c against
model devices: tools/tests/uaudio_in.c."""
import argparse, os, shlex, subprocess
from pathlib import Path
root = Path(__file__).resolve().parents[1]
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--32', dest='bits32', action='store_true')
ap.add_argument('--sanitize', action='store_true')
a = ap.parse_args()
out = root / 'build/test/uaudio_in'; out.mkdir(parents=True, exist_ok=True)
cc = shlex.split(os.environ.get('CC', 'cc'))
flags = ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Ikernel', '-Iusb', '-fno-pie', '-no-pie']
if a.bits32: flags += ['-m32']
if a.sanitize: flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
exe = out / 'test'
subprocess.run([*cc, *flags, 'tools/tests/uaudio_in.c', '-lm', '-o', str(exe)], cwd=root, check=True)
# (an unplugged device's buffers are kept on purpose: the controller may
# still hold its packets - not a leak to report)
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
subprocess.run([str(exe)], cwd=root, check=True, env=env)
