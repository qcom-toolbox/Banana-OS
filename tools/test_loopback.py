#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Real IPv4/TCP/UDP/net polling without a NIC, plus physical RX isolation."""
import argparse,os,shlex,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--32',dest='bits32',action='store_true')
p.add_argument('--sanitize',action='store_true')
a=p.parse_args()
out=root/'build/test/loopback';out.mkdir(parents=True,exist_ok=True)
flags=['-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-Ikernel','-Inet','-include','web/test/shim/types.h']
if a.bits32:flags+=['-m32']
if a.sanitize:flags+=['-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-fno-pie','-no-pie']
exe=out/'test'
subprocess.run([*shlex.split(os.environ.get('CC','cc')),*flags,'tools/tests/loopback.c','net/net.c','net/ip.c','net/tcp.c','net/udp.c','-Wl,--gc-sections','-o',str(exe)],cwd=root,check=True)
subprocess.run([str(exe)],cwd=root,check=True)
