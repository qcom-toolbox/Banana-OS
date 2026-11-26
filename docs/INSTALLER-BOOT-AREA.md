# Installer boot area

Build 249's published ISO is 164,177,920 bytes. The previous installer
allowed only 134,184,960 bytes (128 MiB minus its checksum reservation).

New installs reserve at least **1 GiB** for the boot image. The installer
requires spare space equal to **50% of the image size or 256 MiB**, whichever
is larger, in addition to the 128 KiB checksum reservation. It rounds the
result up to a power of two (1 GiB or 2 GiB). The current roughly 157 MiB
ISO therefore has about 867 MiB of growth room.

The checksum format supports images up to 1 GiB; larger images are rejected
before any disk write, even when the reserved area is larger. The filesystem
starts after the boot area; MBR entry 2 records its start and size as a Linux
(0x83) partition. Entry 1 remains the EFI system partition, so BIOS and UEFI
boot are kept. The destination must also have room for the filesystem and
the selected files; installation does not reduce the growth margin to fit
a smaller disk.

The image stream includes the target's filesystem partition entry before
hashing, comparing or writing. Updates therefore preserve the partition and
the checksums cover the bytes actually written, including sector zero.

Existing 256 MiB, 512 MiB and 1 GiB partitions remain readable and are
not resized during updates. Legacy layouts without this partition retain their 128 MiB boundary and
their original checksum-table format. Updates check the target boundary
before writing. The hybrid image now shares its kernel data between the ISO9660 tree and
the FAT EFI partition. Previously both kernels were stored twice. The new
image is about 80 MiB instead of 157 MiB, leaving about 48 MiB free in a
legacy 128 MiB boot area. Thus older installations can update without moving
their filesystem or reinstalling. Both the Rock Ridge and Joliet trees point
to the same aligned, contiguous kernel bytes used by UEFI.

The image builder verifies alignment, contiguity and identical contents
before removing duplicate extents, updates both volume sizes and preserves
other files. Unsupported layouts fail the build. The checksum image limit
also applies during updates.

## Growing the boot area on update

An image larger than an existing boot area (a disk installed with the old
128 MiB one, before build 250; or a 256 / 512 MiB partitioned install) is
no longer rejected when `update` runs from the live CD or USB stick: the
boot area grows to the size a new install would reserve (1 GiB), or to
512 / 256 MiB when the files would not fit after it, and the ext4
filesystem's start moves there (`grow_boot_area` in kernel/fsdisk.c,
kernel/ext4move.c). The installed system updating itself (`update` from
the desktop, Settings > System update) cannot move the filesystem it runs
from: it says to update once from the live stick (`FSDISK_ERR_NEEDS_LIVE`).

How the filesystem moves, delta blocks further in:

1. `/swapfile` is deleted (the system makes it again at its next start).
2. Inside the old filesystem, every file's data (and every folder's and
   extent tree's blocks) lying before the new start, where the new
   superblock and group descriptors go, or past the new end is copied into
   what becomes the new filesystem (`ext4_move_into`, journaled
   transaction by transaction).
3. The old tree is read: names, inodes (hard links once), modes, owners,
   dates, symbolic link targets, each file's extents.
4. A new filesystem is made at the new start *around every block the old
   one uses* (`ext4_mkfs` with those runs kept), its superblock without
   the ext4 magic until it is complete. The folders, files and links are
   made again; each file is given the old file's extents where they are
   (`ext4_adopt_runs`), so its data is never copied a second time. The
   kept blocks no file has (the old folders, trees, journal) become free.
5. Sector 0 is written: the new image's MBR with the filesystem's new
   partition entry. That one write switches the disk over. The rest of
   the image follows as on every update, then the checksum table.

Until step 5 the old filesystem is the disk's and stays whole: the new one
is written only into blocks the old one has free. A power cut at any point
before it leaves the old system and its files as they were (some files'
data at new places); the next `update` starts again. After step 5 the
files are in the new filesystem; a cut while the image is written leaves a
disk that does not start until `update` is run again from the stick, with
the files kept. If the files would not fit after even a 256 MiB area, or the
old filesystem's own structures lie where the new one's go (a filesystem
not made by Banana OS), nothing is changed and `update` says so.

Tests: `sh tools/ext4test/run.sh` moves a filesystem holding files of every
kind (fragmented, sparse, preallocated, hard and symbolic links, owners,
permissions, dates, lost+found) by 128 MiB and by 896 MiB, checks every
file and e2fsck on both filesystems, and cuts the power at 19 points of
the move (120 in a longer sweep): each time the old filesystem replays its
journal clean with every file intact, and a second move succeeds. In QEMU:
build 212 installed on a 2 GiB disk, updated from the live CD to an image
over 128 MiB (the boot area grew to 1 GiB), restarted from the disk with
its files, hard links and old programs intact, `fsck` and e2fsck clean.

The same tests found an older crash-safety bug in the ext4 journal: after a
transaction's commit, its blocks were written in place with the
superblock's copy among them, which cleared the recovery flag on the disk
before the other blocks were there - a cut in between left a half-applied
transaction that Linux (and e2fsck) would not replay. The superblock now
keeps the flag set until the whole transaction is in place.

Run the host regression tests with: sh tools/tests/fsdisk_boot.sh

The tests exercise the real installer geometry and checksum code, including
the 156.6 MiB image, growth margins across all supported image sizes, threshold boundaries,
all old and new partition sizes, invalid partition entries, and
new and legacy checksum-table round trips and corruption detection.

Validation on the build machine (2026-10-23):
- Both i386 and x86_64 kernels compile; full hybrid ISO built.
- Host regression tests pass under AddressSanitizer and UBSan.
- Full installation onto a new 3 GiB QEMU disk succeeds.
- Ext4 starts at 256 MiB; BIOS boot without the CD reads a persisted file.
- Updating a deliberately changed boot image preserves the MBR filesystem
  partition; the persisted file remains after reboot.
- UEFI boot from the installed disk succeeds.
- Banana OS fsck and Linux e2fsck -fn report no errors.

The GNU/editor packages were reused from the official build-215 ISO for
the same source commit (c72e216); both kernels and the loaders were rebuilt.

Validation of shared kernel data:
- Run python3 tools/test_mkimage.py (mtools and xorriso required).
- Run BASE_ISO=/path/to/old-128MiB-installable.iso bash tools/ci-install-test.sh bios
  and repeat with uefi to test the legacy upgrade path and persisted files.
