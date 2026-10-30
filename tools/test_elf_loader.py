#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the actual kernel ELF loader with malformed ELF and real SDK executables."""
import argparse,os,shlex,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[1]
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--32',dest='bits32',action='store_true')
ap.add_argument('--sanitize',action='store_true')
ap.add_argument('--case',choices=['all','valid','headers','segments','dynamic','relocations','fuzz'],default='all')
a=ap.parse_args()
out=root/'build/test/elf-loader';out.mkdir(parents=True,exist_ok=True)
s=(root/'kernel/app.c').read_text()
start=s.index('#define PT_LOAD');end=s.index('/* the same loader for driver modules',start)
part=s[start:end]
start=s.index('static uint64_t elf_span(',s.index('/* the bytes an ELF'))
end=s.index('\n/* 64-bit:',start)
part+=s[start:end]
(out/'elf_loader.inc').write_text(part)
flags=['-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-I'+str(out)]
if a.bits32:flags+=['-m32']
if a.sanitize:flags+=['-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-fno-pie','-no-pie']
exe=out/'test'
subprocess.run([*shlex.split(os.environ.get('CC','cc')),*flags,'tools/tests/elf_loader.c','-o',str(exe)],cwd=root,check=True)
subprocess.run([str(exe),a.case],cwd=root,check=True)
if a.case=='all':
    arch='i686' if a.bits32 else 'x86_64'
    files=[*root.glob('sdk/examples/*/build/app-'+arch),*root.glob('apps/*/build/app-'+arch)]
    if not a.bits32:files+=list(root.glob('ports/*/build/x86_64/app-x86_64'))
    for p in files:subprocess.run([str(exe),'file',str(p)],cwd=root,check=True)
    print('Real SDK executable fixtures:',len(files))
