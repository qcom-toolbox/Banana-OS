#!/usr/bin/env python3
"""Actual configuration reader/writer under cache eviction and disk faults."""
import argparse,os,shlex,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[1]
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--32',dest='bits32',action='store_true')
ap.add_argument('--sanitize',action='store_true')
ap.add_argument('--case',choices=['all','reclaim','noop','io'],default='all')
a=ap.parse_args()
out=root/'build/test/config';out.mkdir(parents=True,exist_ok=True)
flags=['-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Ikernel','-include','web/test/shim/types.h']
if a.bits32:flags+=['-m32']
if a.sanitize:flags+=['-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-fno-pie','-no-pie']
exe=out/'test'
subprocess.run([*shlex.split(os.environ.get('CC','cc')),*flags,'tools/tests/config.c','-o',str(exe)],cwd=root,check=True)
subprocess.run([str(exe),a.case],cwd=root,check=True)
