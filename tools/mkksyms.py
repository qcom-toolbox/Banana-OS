#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The kernel's symbol table for prof (kernel/prof.c: g_ksyms).

  mkksyms.py KERNEL.ELF OUT.bin   the code symbols of the first link pass:
                                  u32 count, u32 addresses (sorted), u32 name
                                  offsets, the names (NUL-terminated)
  mkksyms.py --same A.ELF B.ELF   the code symbols are where they were: the
                                  table, linked after the code in the second
                                  pass, moved nothing it describes
"""
import subprocess
import struct
import sys


def code_symbols(elf):
    out = subprocess.run(['nm', '-n', '--defined-only', elf], check=True,
                         capture_output=True, text=True).stdout
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) != 3 or parts[1] not in 'tTwW':
            continue
        addr = int(parts[0], 16)
        if addr >> 32:
            continue
        syms.setdefault(addr, parts[2])
    return sorted(syms.items())


def main():
    if len(sys.argv) == 4 and sys.argv[1] == '--same':
        a, b = code_symbols(sys.argv[2]), code_symbols(sys.argv[3])
        if a != b:
            moved = [(x, y) for x, y in zip(a, b) if x != y][:5]
            sys.exit(f'mkksyms: code symbols moved between the link passes: {moved}')
        return
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    syms = code_symbols(sys.argv[1])
    names = bytearray()
    offs = []
    for _, name in syms:
        offs.append(len(names))
        names += name.encode()[:80] + b'\0'
    blob = struct.pack('<I', len(syms))
    blob += struct.pack(f'<{len(syms)}I', *(a for a, _ in syms))
    blob += struct.pack(f'<{len(syms)}I', *offs)
    blob += bytes(names)
    with open(sys.argv[2], 'wb') as f:
        f.write(blob)


if __name__ == '__main__':
    main()
