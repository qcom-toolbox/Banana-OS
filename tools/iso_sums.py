#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""iso_sums.py ISOROOT FILE... - BANANA.SUM in ISOROOT: "crc32 size path" per
file (CRC-32 as zlib's, the kernel's crc32_update; paths as the ISO's level-1
names, upper case). update / repair / checkmedium read the CD or USB stick's
files back against it before using it (kernel/fsdisk.c: fsdisk_check_medium)."""
import os, sys, zlib
root = sys.argv[1]
lines = []
for rel in sys.argv[2:]:
    crc, size = 0, 0
    with open(os.path.join(root, rel), "rb") as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            crc = zlib.crc32(b, crc)
            size += len(b)
    lines.append("%08x %u %s\n" % (crc, size, rel.upper()))
with open(os.path.join(root, "BANANA.SUM"), "w") as f:
    f.write("".join(lines))
