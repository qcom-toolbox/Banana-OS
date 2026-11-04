/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef EXT4_H
#define EXT4_H

#include "types.h"

/*
 * ext4 (kernel/ext4.c): the installed disk's filesystem, in ext4's own
 * on-disk format - Linux mounts it, its e2fsck checks it. 4 KiB blocks in
 * groups of 32768 (flex_bg: the bitmaps and inode tables together at the
 * start), 256-byte inodes, files as extent trees, directories as lists of
 * entries, symbolic links up to 59 bytes in the inode itself. Every block
 * goes through a cache: a read or a write touches only the blocks it
 * needs, and changed blocks go to the disk within a second (ext4_flush).
 *
 * Features: filetype, extents, flex_bg (incompat); sparse_super, large_file,
 * dir_nlink, extra_isize, uninit_bg (ro_compat); sparse_super2 with no
 * backup superblock (compat); an internal JBD2 journal in ordered mode.
 * See docs/ext4-format.md for geometry and mount acceptance. Not (yet): htree directories,
 * metadata checksums, inline data, extended attributes.
 */

#define EXT4_ROOT_INO   2u
#define EXT4_LPF_INO    11u                 /* lost+found, the first ordinary inode */

#define EXT4_FT_REG     1u
#define EXT4_FT_DIR     2u
#define EXT4_FT_LNK     7u

#define EXT4_S_IFMT     0170000u
#define EXT4_S_IFREG    0100000u
#define EXT4_S_IFDIR    0040000u
#define EXT4_S_IFLNK    0120000u

/* the device: whole 512-byte sectors from the start of the filesystem */
typedef struct {
    int  (*read)(void* ctx, uint32_t sector, uint32_t n, void* buf);
    int  (*write)(void* ctx, uint32_t sector, uint32_t n, const void* buf);
    int  (*flush)(void* ctx);
    void* ctx;
    uint32_t sectors;                       /* the filesystem's size */
} ext4_dev_t;

typedef struct {
    uint32_t ino;
    uint32_t mode;                          /* EXT4_S_IF* | permissions */
    uint32_t uid, gid;
    uint64_t size;
    uint32_t atime, mtime, ctime, crtime;   /* UTC, seconds since 1970 */
    uint32_t links;
    uint64_t blocks;                        /* 512-byte sectors in use (data and tree) */
} ext4_stat_t;

/* the filesystem on dev, if there is one: 0, or -1 (not ext4, or features
 * this driver does not have) */
int  ext4_mount(const ext4_dev_t* dev);
int  ext4_mounted(void);
int  ext4_unmount(void);            /* flushed, then let go */
/* makes an empty ext4 filesystem over all of dev, named label, and mounts
 * it. keep: nkeep runs of 4 KiB blocks {first, count} holding data already
 * (moving an older Banana OS disk over: ext4_adopt gives them to files) -
 * marked in use, its own structures put around them. 0, or -1 (a kept run
 * lies where ext4 must have its superblock or group descriptors) */
int  ext4_mkfs(const ext4_dev_t* dev, const char* label, const uint32_t (*keep)[2], int nkeep);
/* file ino (empty) gets the nblocks blocks from pblk - a kept run - as its
 * data, size bytes of it: 0, or -1 */
int  ext4_adopt(uint32_t ino, uint64_t size, uint32_t pblk, uint32_t nblocks);
/* making a filesystem in place of an older one: until ext4_activate, its
 * superblock goes to the disk without the ext4 magic (a cut meanwhile:
 * the disk is still what it was); and blocks given back (the old one's) */
void ext4_set_hidden(int on);
int  ext4_activate(void);
void ext4_free_run(uint32_t blk, uint32_t n);
/* the blocks ext4 must have at fixed places on a disk of `sectors`: block 0
 * up to this one (the superblock and the group descriptors) */
uint32_t ext4_fixed_blocks(uint32_t sectors);
/* the volume's name, "" if none */
const char* ext4_label(void);

/* name in folder dir: its inode (and *type, EXT4_FT_*), or 0 */
uint32_t ext4_lookup(uint32_t dir, const char* name, uint32_t* type);
/* every entry of folder dir but . and ..: fn(arg, name, ino, type); fn
 * returning non-zero stops it. 0, or -1 */
int  ext4_readdir(uint32_t dir, int (*fn)(void* arg, const char* name, uint32_t ino, uint32_t type), void* arg);
int  ext4_stat(uint32_t ino, ext4_stat_t* st);

/* a new file / folder / link in dir: its inode, or 0 (*err: why) */
uint32_t ext4_create(uint32_t dir, const char* name, uint32_t mode, uint32_t uid, uint32_t gid);
uint32_t ext4_mkdir(uint32_t dir, const char* name, uint32_t mode, uint32_t uid, uint32_t gid);
uint32_t ext4_symlink(uint32_t dir, const char* name, const char* target, uint32_t uid, uint32_t gid);
/* removes name from dir (a folder: only an empty one); the inode goes with
 * its last link. 0, or -1 */
int  ext4_unlink(uint32_t dir, const char* name);
int  ext4_rmdir(uint32_t dir, const char* name);
/* name in dir: another (hard) link to file ino. 0, or -1 */
int  ext4_link(uint32_t dir, const char* name, uint32_t ino);
/* moves / renames; a file of the new name is replaced. 0, or -1 */
int  ext4_rename(uint32_t odir, const char* oname, uint32_t ndir, const char* nname);

/* len bytes at off: the bytes read (0 past the end), or -1 */
long ext4_read(uint32_t ino, uint64_t off, void* buf, uint32_t len);
/* writes len bytes at off (the file grows as needed): 0, or -1 (full) */
int  ext4_write(uint32_t ino, uint64_t off, const void* buf, uint32_t len);
int  ext4_truncate(uint32_t ino, uint64_t size);
/* sets what is not ~0u: mode's permission bits, uid, gid, the dates */
int  ext4_setattr(uint32_t ino, uint32_t perms, uint32_t uid, uint32_t gid,
                  uint32_t atime, uint32_t mtime, uint32_t ctime);
/* blocks for a file of size bytes, not written (read as zeros), in ONE run:
 * the longest free one if none is that long (the file is then that size).
 * For the swap file; ext4_first_run: where it is, in sectors */
int  ext4_fallocate(uint32_t ino, uint64_t size);
int  ext4_first_run(uint32_t ino, uint32_t* sector, uint32_t* nsectors);

/* the changed blocks to the disk, the disk's cache flushed: 0, or -1 */
int  ext4_flush(void);
/* blocks waiting to be written */
uint32_t ext4_dirty_blocks(void);
/* gives cached blocks back (RAM low): bytes freed */
uint32_t ext4_cache_trim(uint32_t want);
uint32_t ext4_cache_bytes(void);

/* blocks / inodes in all and free, the block size */
void ext4_statfs(uint64_t* blocks, uint64_t* free_blocks, uint32_t* inodes, uint32_t* free_inodes);

/* fsck: the groups' counts against the bitmaps, the folders' links; lines
 * through out(): the number of problems */
int  ext4_check(void (*out)(const char* line));

#endif
