#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Banana Boot: makes the ISO from xorriso a hybrid image - a CD and a disk.

usage: mkimage.py Banana_OS.iso loader/mbr.bin

xorriso has made the CD part: El Torito entries for the BIOS loader
(/boot/bios.bin) and the UEFI one (/efi.img, a FAT image holding
EFI/BOOT/BOOTX64.EFI and the kernels). This writes sector 0 for USB sticks
and hard disks (the installer copies the image onto the disk as it is):

  - the MBR code (loader/mbr.bin) with where /boot/bios.bin is in the image
    (512-byte sectors) - it loads that to 0x8000 and starts it;
  - one partition, active (some BIOSes boot only disks that have one), of
    type 0xEF: /efi.img itself - the EFI system partition UEFI firmware
    boots BOOTX64.EFI from.
"""
import struct, sys


def iso_has(img, path):
    """1 if the ISO9660 image has the file"""
    try:
        iso_find(img, path)
        return True
    except SystemExit:
        return False


def iso_find(img, path):
    """(first 2048-byte block, size) of an ISO9660 file"""
    pvd = img[16 * 2048:17 * 2048]
    if pvd[1:6] != b"CD001":
        sys.exit("mkimage: not an ISO9660 image")
    root = pvd[156:156 + 34]
    lba, size = struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0]
    for part in path.strip("/").split("/"):
        found = None
        for off in range(0, size, 2048):
            blk = img[(lba * 2048) + off:(lba * 2048) + off + 2048]
            p = 0
            while p < 2048 and blk[p]:
                n = blk[p + 32]
                name = blk[p + 33:p + 33 + n].decode("ascii", "replace").split(";")[0].rstrip(".")
                if name.upper() == part.upper():
                    found = (struct.unpack_from("<I", blk, p + 2)[0], struct.unpack_from("<I", blk, p + 10)[0])
                    break
                p += blk[p]
            if found:
                break
        if not found:
            sys.exit("mkimage: %s is not in the image" % path)
        lba, size = found
    return lba, size



def fat_file(img, start, size, path):
    """Return the absolute offset and size of a contiguous FAT16/32 file."""
    boot = img[start:start + 512]
    bps = struct.unpack_from("<H", boot, 11)[0]
    spc = boot[13]
    reserved = struct.unpack_from("<H", boot, 14)[0]
    roots = struct.unpack_from("<H", boot, 17)[0]
    fatlen = struct.unpack_from("<H", boot, 22)[0] or struct.unpack_from("<I", boot, 36)[0]
    total = struct.unpack_from("<H", boot, 19)[0] or struct.unpack_from("<I", boot, 32)[0]
    rootsecs = (roots * 32 + 511) // 512
    data = reserved + boot[16] * fatlen + rootsecs
    if bps != 512 or not spc or spc & (spc - 1) or not fatlen or total * bps > size:
        raise ValueError("invalid ESP geometry")
    count = (total - data) // spc
    bits = 16 if count < 65525 else 32
    if count < 4085:
        raise ValueError("FAT12 ESP is not supported")
    cluster_size = spc * bps
    fatbase = start + reserved * bps

    def chain(first):
        seen = set()
        while first < (0xfff8 if bits == 16 else 0xffffff8):
            if first < 2 or first >= count + 2 or first in seen:
                raise ValueError("invalid ESP cluster chain")
            seen.add(first)
            yield first
            pos = fatbase + first * (bits // 8)
            first = struct.unpack_from("<H" if bits == 16 else "<I", img, pos)[0]
            if bits == 32:
                first &= 0xfffffff

    def offset(cluster):
        return start + (data + (cluster - 2) * spc) * bps

    if bits == 16:
        directory = img[start + (reserved + boot[16] * fatlen) * bps:
                        start + data * bps]
    else:
        directory = b"".join(img[offset(c):offset(c) + cluster_size]
                             for c in chain(struct.unpack_from("<I", boot, 44)[0]))
    parts = path.strip("/").upper().split("/")
    for i, part in enumerate(parts):
        found = None
        for n in range(0, len(directory), 32):
            e = directory[n:n + 32]
            if not e[0]:
                break
            if e[0] == 0xe5 or e[11] & 0x08 or e[11] == 0x0f:
                continue
            name = e[:8].decode("ascii").rstrip()
            ext = e[8:11].decode("ascii").rstrip()
            if ext:
                name += "." + ext
            if name == part:
                first = struct.unpack_from("<H", e, 26)[0]
                if bits == 32:
                    first |= struct.unpack_from("<H", e, 20)[0] << 16
                found = (e, list(chain(first)))
                break
        if found is None:
            raise ValueError("missing ESP file " + path)
        entry, clusters = found
        if i + 1 < len(parts):
            if not entry[11] & 0x10:
                raise ValueError("ESP path is not a directory")
            directory = b"".join(img[offset(c):offset(c) + cluster_size] for c in clusters)
        else:
            length = struct.unpack_from("<I", entry, 28)[0]
            if entry[11] & 0x10 or not length or not clusters or len(clusters) != (length + cluster_size - 1) // cluster_size:
                raise ValueError("invalid ESP kernel file")
            if any(b != a + 1 for a, b in zip(clusters, clusters[1:])):
                raise ValueError("fragmented ESP kernel file")
            return offset(clusters[0]), length


def compact_kernels(img):
    """Share aligned kernel data between FAT and ISO9660 (including Joliet).

    Remove duplicate ISO extents, preserving every remaining file. Refuse
    layouts with metadata behind the removed extents rather than silently
    invalidating path tables, boot catalog pointers or Rock Ridge data.
    """
    esp_lba, esp_size = iso_find(img, "efi.img")
    aliases, removed = {}, []
    # (an image may have one kernel: Banana_OS-32.iso, or the 64-bit ones)
    names = [n for n in ("kernel.bin", "kernel64.bin") if iso_has(img, "boot/" + n)]
    if not names:
        raise ValueError("no kernel in the image")
    for name in names:
        lba, size = iso_find(img, "boot/" + name)
        off, fat_size = fat_file(img, esp_lba * 2048, esp_size, "boot/" + name)
        if off % 2048 or size != fat_size or img[lba * 2048:lba * 2048 + size] != img[off:off + size]:
            raise ValueError("ESP/ISO kernel mismatch or alignment error: " + name)
        aliases[lba] = off // 2048
        removed.append((lba, lba + (size + 2047) // 2048))
    removed.sort()
    first = removed[0][0]
    if any(removed[i][1] > removed[i + 1][0] for i in range(len(removed) - 1)) or esp_lba + (esp_size + 2047) // 2048 > first:
        raise ValueError("unexpected kernel/ESP extent overlap")

    def moved(lba):
        for a, b in removed:
            if a <= lba < b:
                raise ValueError("file overlaps a removed kernel extent")
        return lba - sum(b - a for a, b in removed if b <= lba)

    def both32(pos, value):
        struct.pack_into("<I", img, pos, value)
        struct.pack_into(">I", img, pos + 4, value)

    descriptors = []
    visited = set()

    def walk(lba, size):
        key = (lba, size)
        if key in visited:
            return
        visited.add(key)
        if lba + (size + 2047) // 2048 > first:
            raise ValueError("directory metadata follows kernel data")
        for block in range(0, size, 2048):
            pos, end = lba * 2048 + block, lba * 2048 + min(block + 2048, size)
            while pos < end and img[pos]:
                length = img[pos]
                if length < 34 or pos + length > end:
                    raise ValueError("invalid ISO directory record")
                extent = struct.unpack_from("<I", img, pos + 2)[0]
                length_bytes = struct.unpack_from("<I", img, pos + 10)[0]
                isdir = img[pos + 25] & 2
                # Rock Ridge continuation records refer to absolute blocks.
                namelen = img[pos + 32]
                su = pos + 33 + namelen + (not (namelen & 1))
                while su + 4 <= pos + length and img[su + 2] >= 4:
                    n = img[su + 2]
                    if su + n > pos + length:
                        raise ValueError("invalid Rock Ridge entry")
                    if img[su:su + 2] == b"CE" and struct.unpack_from("<I", img, su + 4)[0] >= first:
                        raise ValueError("Rock Ridge continuation follows kernel data")
                    su += n
                if isdir:
                    walk(extent, length_bytes)
                else:
                    if extent not in aliases:
                        for a, b in removed:
                            if extent < b and extent + (length_bytes + 2047) // 2048 > a:
                                raise ValueError("file overlaps kernel data")
                    both32(pos + 2, aliases[extent] if extent in aliases else moved(extent))
                pos += length

    block = 16
    while True:
        pos = block * 2048
        if img[pos + 1:pos + 6] != b"CD001":
            raise ValueError("invalid volume descriptor")
        kind = img[pos]
        if kind in (1, 2):
            descriptors.append(pos)
            for off, fmt in ((140, "<I"), (144, "<I"), (148, ">I"), (152, ">I")):
                if struct.unpack_from(fmt, img, pos + off)[0] >= first:
                    raise ValueError("path table follows kernel data")
            root = pos + 156
            walk(struct.unpack_from("<I", img, root + 2)[0],
                 struct.unpack_from("<I", img, root + 10)[0])
        elif kind == 0:
            catalog = struct.unpack_from("<I", img, pos + 71)[0]
            if catalog >= first:
                raise ValueError("boot catalog follows kernel data")
            # All boot images must precede the compacted extents.
            for entry in range(catalog * 2048 + 32, (catalog + 1) * 2048, 32):
                if img[entry] == 0x88 and struct.unpack_from("<I", img, entry + 8)[0] >= first:
                    raise ValueError("boot image follows kernel data")
        elif kind == 255:
            break
        block += 1
    blocks = len(img) // 2048 - sum(b - a for a, b in removed)
    for pos in descriptors:
        both32(pos + 80, blocks)
    result, previous = bytearray(), 0
    for a, b in removed:
        result.extend(img[previous:a * 2048])
        previous = b * 2048
    result.extend(img[previous:])
    print("mkimage: shared kernel data saves %d KiB" % (sum(b-a for a,b in removed)*2))
    return result


def chs(lba):
    """CHS for a partition entry (255 heads, 63 sectors; past 8 GB: the maximum)"""
    c, rest = divmod(lba, 255 * 63)
    h, s = divmod(rest, 63)
    if c > 1023:
        return bytes([0xFE, 0xFF, 0xFF])
    return bytes([h, ((c >> 2) & 0xC0) | (s + 1), c & 0xFF])


def main():
    iso, mbr_path = sys.argv[1], sys.argv[2]
    img = bytearray(open(iso, "rb").read())
    img = compact_kernels(img)
    mbr = bytearray(open(mbr_path, "rb").read())
    if len(mbr) != 440:
        sys.exit("mkimage: the MBR code must be 440 bytes")

    bios_lba, bios_size = iso_find(img, "boot/bios.bin")
    efi_lba, efi_size = iso_find(img, "efi.img")
    sectors = (bios_size + 511) // 512
    if sectors > 64:
        sys.exit("mkimage: the BIOS loader is over 32 KiB")
    struct.pack_into("<IH", mbr, 0x1B0, bios_lba * 4, sectors)

    sec0 = bytearray(512)
    sec0[0:440] = mbr
    sec0[440:444] = b"BnOS"                      # disk signature
    start, count = efi_lba * 4, (efi_size + 511) // 512
    entry = bytes([0x80]) + chs(start) + bytes([0xEF]) + chs(start + count - 1) + struct.pack("<II", start, count)
    sec0[446:462] = entry
    sec0[510:512] = b"\x55\xAA"
    img[0:512] = sec0
    open(iso, "wb").write(img)
    print("mkimage: BIOS loader at sector %d (%d sectors), EFI partition at %d (%d KiB)"
          % (bios_lba * 4, sectors, start, count // 2))


if __name__ == "__main__":
    main()
