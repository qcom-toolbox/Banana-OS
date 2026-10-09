#!/usr/bin/env python3
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
