#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The radeon command-stream checker's safe-register bitmaps, from Linux's
register lists (third_party/radeon/reg_srcs) - the same table Linux's
drivers/gpu/drm/radeon/mkregtable.c makes: one bit per register (offset / 4),
all set ("check it"), each listed register's bit flipped (safe to write).
    tools/radeon_regtable.py third_party/radeon/reg_srcs/evergreen > evergreen_reg_safe.h"""
import re, sys

def build(path):
    lines = open(path, encoding="ascii").read().splitlines()
    name, last = lines[0].split()[:2]
    offset_max, offsets = 0, []
    for l in lines[1:]:
        m = re.search(r"(0x[0-9a-fA-F]*) *([_a-zA-Z0-9]*)", l)
        if not m:
            continue
        o = int(m.group(1), 16)
        offsets.append(o)
        offset_max = max(offset_max, o)
    offset_max = max(offset_max, int(last, 16))
    n = ((offset_max >> 2) + 31) // 32
    table = [0xFFFFFFFF] * n
    for o in offsets:
        table[(o >> 2) // 32] ^= 1 << ((o >> 2) & 31)
    out = ["static const unsigned %s_reg_safe_bm[%d] = {" % (name, n)]
    for i in range(0, n, 4):
        out.append("\t" + " ".join("0x%08X," % v for v in table[i:i + 4]))
    out.append("};")
    return "\n".join(out) + "\n"

if __name__ == "__main__":
    sys.stdout.write(build(sys.argv[1]))
