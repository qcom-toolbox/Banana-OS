#!/usr/bin/env python3
"""Banana Boot: checks that a UEFI loader (the ELF before objcopy) needs no
relocation - the PE file has none, so the code must run wherever the
firmware puts it.

usage: efi_relocs.py loader/bootx64.so

The only one allowed is the GOT's first slot: the 32-bit linker always
stores the address of .dynamic there (for a dynamic loader - there is none
here, and the code never reads it). Any other GOT entry is a pointer the
code would read unrelocated: extern symbols must be hidden (efi.c)."""
import re, subprocess, sys

elf = sys.argv[1]
got = None
for line in subprocess.run(["readelf", "-SW", elf], capture_output=True, text=True).stdout.splitlines():
    m = re.search(r"\]\s+\.got\s+\S+\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)", line)
    if m:
        got = (int(m.group(1), 16), int(m.group(2), 16))
bad = []
for line in subprocess.run(["readelf", "-rW", elf], capture_output=True, text=True).stdout.splitlines():
    m = re.match(r"\s*([0-9a-f]{8,16})\s+[0-9a-f]+\s+(R_\S+)", line)
    if not m:
        continue
    off = int(m.group(1), 16)
    if got and off == got[0] and m.group(2) == "R_386_RELATIVE":
        continue
    bad.append(line.strip())
if bad:
    print("%s: relocations - it must be position-independent:" % elf)
    for b in bad:
        print("   ", b)
    sys.exit(1)
