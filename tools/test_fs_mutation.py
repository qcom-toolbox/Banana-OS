#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the actual filesystem and cache under copy/write faults."""
from pathlib import Path
import argparse,os,shlex,subprocess
r=Path(__file__).resolve().parents[1];p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--32',dest='bits32',action='store_true');p.add_argument('--sanitize',action='store_true')
p.add_argument('--case',choices=['all','copy','move','overflow','gap','alias','write','links','offset','stream','failures'],default='all')
a=p.parse_args();out=r/'build/test/fs-mutation';out.mkdir(parents=True,exist_ok=True)
flags=['-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-Ikernel','-include','web/test/shim/types.h']
if a.bits32:flags+=['-m32']
if a.sanitize:flags+=['-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-pie','-no-pie']
exe=out/'test'
# Keep host libc memory interceptors: the kernel's assembly copies cannot be instrumented.
obj=out/'kstring.o'
subprocess.run([*shlex.split(os.environ.get('CC','cc')),*flags,
 '-Dmemcpy=kernel_memcpy','-Dmemmove=kernel_memmove','-Dmemset=kernel_memset','-Dmemcmp=kernel_memcmp',
 '-c','kernel/kstring.c','-o',str(obj)],cwd=r,check=True)
subprocess.run([*shlex.split(os.environ.get('CC','cc')),*flags,'tools/tests/fs_mutation.c',str(obj),'-Wl,--gc-sections','-o',str(exe)],cwd=r,check=True)
for case in (['copy','move','overflow','gap','alias','write','links','offset','stream','failures'] if a.case=='all' else [a.case]):
 subprocess.run([str(exe),case],cwd=r,check=True)
