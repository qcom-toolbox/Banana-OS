#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the actual desktop layout parser/placement with malformed coordinates."""
import argparse,os,shlex,subprocess
from pathlib import Path
r=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--32',dest='bits32',action='store_true');p.add_argument('--sanitize',action='store_true')
a=p.parse_args();out=r/'build/test/desktop-layout';out.mkdir(parents=True,exist_ok=True)
s=(r/'kernel/gui.c').read_text()
part=s[s.index('#define LAYOUT_FILE'):s.index('static void layout_save(')]
def fn(sig):
 start=s.index(sig);end=s.index('{',start)+1;level=1
 while level:
  level+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[start:end]+'\n'
part+=fn('static int desk_cols(')+fn('static int desk_rows(const fb_info_t* fi) {')+fn('static void desk_place(')
(out/'layout.inc').write_text(part)
flags=['-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-I'+str(out)]
if a.bits32:flags+=['-m32']
if a.sanitize:flags+=['-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-pie','-no-pie']
exe=out/'test'
subprocess.run([*shlex.split(os.environ.get('CC','cc')),*flags,'tools/tests/desktop_layout.c','-o',str(exe)],cwd=r,check=True)
subprocess.run([str(exe)],cwd=r,check=True)
