#ifndef FAT32_H
#define FAT32_H

#include "types.h"
#include "blockdev.h"

/*
 * FAT32 volumes (USB sticks): mounted into the filesystem tree (fs.h's
 * mount API) - folders and files appear under the mount point, contents
 * are read on first use, and every change (new files, writes, appends,
 * deletes, renames, new folders) is written straight to the stick, long
 * file names included.
 *
 * The volume is found on the whole device (a "superfloppy") or in the
 * first FAT32 partition of an MBR or GPT partition table.
 */

/* mounts bd's FAT32 volume at path (NULL: the first free /mnt/usb,
 * /mnt/usb2, ...); returns the fs mount id, or -1 with err set */
int fat32_mount(blockdev_t* bd, const char* path, char* err, int errcap);

/* for `mount`: one line about mount id mnt into out (0 if not FAT32) */
int fat32_describe(int mnt, char* out, int cap);

#endif
