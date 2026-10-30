#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the actual wallpaper manager with disk faults and cache eviction."""
from pathlib import Path
import argparse,os,shlex,subprocess
r=Path(__file__).resolve().parents[1];p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--32',dest='bits32',action='store_true');p.add_argument('--sanitize',action='store_true')
p.add_argument('--case',choices=['all','io','config','dimensions','preset'],default='all')
p.add_argument('--source',type=Path)
a=p.parse_args();out=r/'build/test/wallpaper';out.mkdir(parents=True,exist_ok=True)
(out/'wallpaper_impl.inc').write_text((a.source or r/'kernel/wallpaper.c').read_text())
flags=['-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Ikernel','-I'+str(out),'-include','web/test/shim/types.h']
if a.bits32:flags+=['-m32']
if a.sanitize:flags+=['-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-pie','-no-pie']
exe=out/'test'
obj=out/'landscapes.o'
subprocess.run(['nasm','-f','elf32' if a.bits32 else 'elf64','kernel/landscapebin.asm','-o',str(obj)],cwd=r,check=True)
subprocess.run([*shlex.split(os.environ.get('CC','cc')),*flags,'tools/tests/wallpaper.c','kernel/kstring.c','kernel/landscape_data.c',str(obj),'-o',str(exe)],cwd=r,check=True)
for case in (['io','config','dimensions','preset'] if a.case=='all' else [a.case]):
 subprocess.run([str(exe),case],cwd=r,check=True)
