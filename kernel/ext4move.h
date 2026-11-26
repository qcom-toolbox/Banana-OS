/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef EXT4MOVE_H
#define EXT4MOVE_H

#include "ext4.h"

/*
 * Moving an ext4 filesystem's start further into the disk (kernel/fsdisk.c:
 * a boot area that grows, `update` on a disk installed with the old 128 MiB
 * one). old_dev is the filesystem as it is; new_dev starts delta blocks
 * (4 KiB) after it and ends where it ends (or before). The files' data does
 * not move twice: what lies before the new start (or where the new
 * filesystem's superblock and descriptors go) is copied further in, inside
 * the old filesystem, in its own transactions; then a new filesystem is
 * made at the new start AROUND every block the old one uses, and its files
 * are given the old files' data where it is (ext4_adopt_runs) - folders,
 * links, owners, permissions and dates made again.
 *
 * Until the caller points the disk at the new start (the MBR), the old
 * filesystem is whole and in use: the new one is written only into blocks
 * the old one has free, and its superblock carries no magic until it is
 * complete. A cut at any moment leaves the old filesystem as it was (with
 * some of its files' data at new places) - the new one is simply made
 * again next time. /swapfile goes (the system makes it again at boot).
 *
 * The old filesystem's own structures (bitmaps, inode tables, journal) must
 * lie before the new start - as this driver's mkfs puts them - or past the
 * new superblock's blocks: EXT4MOVE_LAYOUT otherwise, nothing changed.
 */

#define EXT4MOVE_OK       0
#define EXT4MOVE_IO      -1      /* a read or write failed (or no memory) */
#define EXT4MOVE_FULL    -2      /* the files do not fit in the smaller filesystem */
#define EXT4MOVE_LAYOUT  -3      /* the old filesystem's structures are where the new one's go */
#define EXT4MOVE_MOUNT   -4      /* the old filesystem does not mount */

typedef struct {
    uint32_t files, dirs, links, symlinks, skipped;
    uint32_t moved_blocks;       /* data copied further in */
    uint32_t kept_blocks;        /* the old filesystem's blocks the new one keeps */
    uint32_t freed_blocks;       /* of those, the old one's own (not a file's): free again */
} ext4move_info_t;

/* the room the new filesystem would have: 0 if it fits (the old one's used
 * blocks, the new one's own structures and some to spare), EXT4MOVE_FULL,
 * or another error. Mounts and unmounts old_dev, changes nothing */
int ext4move_check(const ext4_dev_t* old_dev, const ext4_dev_t* new_dev, uint32_t delta);

/* the move (above): EXT4MOVE_OK with the new filesystem complete (and not
 * mounted), or an error with the old one as it was */
int ext4move_run(const ext4_dev_t* old_dev, const ext4_dev_t* new_dev, uint32_t delta, ext4move_info_t* info);

#endif
