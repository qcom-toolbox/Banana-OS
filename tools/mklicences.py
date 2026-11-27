#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""mklicences.py OUT.asm OUT.h FILE... - Banana OS's licence and the notices
and licence texts of what it includes, built into the kernel
(kernel/licences.c writes them to /usr/share/licenses/banana-os at the
start): OUT.asm has each file (incbin), OUT.h the table of their names."""
import os, sys

asm, hdr, files = sys.argv[1], sys.argv[2], sys.argv[3:]
a = ['; made by tools/mklicences.py', 'section .note.GNU-stack noalloc noexec nowrite progbits', 'section .rodata', '']
h = ['/* made by tools/mklicences.py: kernel/licences.c */']
for i, f in enumerate(files):
    name = 'COPYING' if f == 'LICENSE' else os.path.basename(f)
    a += ['global lic_%d_start, lic_%d_end' % (i, i), 'lic_%d_start: incbin "%s"' % (i, f), 'lic_%d_end:' % i, '']
    h.append('LICENCE(%d, "%s")' % (i, name))
for path, text in ((asm, a), (hdr, h)):
    new = '\n'.join(text) + '\n'
    if not os.path.exists(path) or open(path).read() != new:
        with open(path, 'w') as out:
            out.write(new)
