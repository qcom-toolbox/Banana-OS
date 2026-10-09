#ifndef FSDISK_H
#define FSDISK_H

#include "types.h"
#include "ata.h"

/* Installs Banana OS onto a dedicated ATA hard disk: raw-copies the
 * already directly-BIOS-bootable boot image (the same GRUB "hybrid" MBR
 * that lets Banana_OS.iso be dd'd straight to a USB stick or disk) from
 * the ATAPI CD Banana OS itself booted from, then writes the current
 * in-memory filesystem into a reserved region right after it. The result
 * is a disk that boots Banana OS on its own, no CD required, with your
 * files persisted. */

/* fsdisk_install() failure reasons. */
#define FSDISK_OK              0
#define FSDISK_ERR_NO_TARGET   -1 /* no ATA hard disk found */
#define FSDISK_ERR_AMBIGUOUS   -2 /* more than one ATA hard disk found */
#define FSDISK_ERR_NO_SOURCE   -3 /* no ATAPI boot CD found to copy from */
#define FSDISK_ERR_TOO_SMALL   -4 /* target disk too small for the 128 MB boot area + filesystem */
#define FSDISK_ERR_ISO_TOO_BIG -5 /* boot image bigger than the reserved region (build issue) */
#define FSDISK_ERR_IO          -6 /* a read or write failed */
#define FSDISK_ERR_FULL        -7 /* the files are past what a slot on this disk holds */
#define FSDISK_ERR_NO_INSTALL  -8 /* no disk holds an installed Banana OS */

/* Looks for exactly one non-ATAPI ATA disk attached (any bus/position).
 * Returns 1 and fills *out if exactly one was found, 0 if none, -1 if
 * more than one (ambiguous - caller should ask the user to detach extras). */
int fsdisk_find_target(ata_disk_t* out);

/* Copies the boot image from the ATAPI CD Banana OS booted from onto the
 * target ATA disk (found via fsdisk_find_target()), then writes the
 * current in-memory filesystem into the reserved region after it.
 * Destroys any prior disk contents. Returns FSDISK_OK on success, or one
 * of the FSDISK_ERR_* codes above on failure. */
int fsdisk_install(void);

/* Re-writes the current in-memory filesystem to the disk this session
 * installed to or booted from (leaves the boot image alone). Returns
 * FSDISK_ERR_NO_TARGET if this session isn't associated with an
 * installed disk yet (call fsdisk_install() first). */
int fsdisk_sync(void);

/* Called once at boot, before fs_init() would otherwise seed the default
 * hierarchy: if exactly one ATA disk is present and holds a valid Banana
 * OS filesystem image, loads it and returns 1 (skip fs_init()). Returns 0
 * if there's nothing to load (caller should fall back to fs_init()). */
int fsdisk_try_load(void);   /* (-1: a Banana OS disk, but no copy of it could be read) */

/* Whether this session has a remembered installed/loaded target disk
 * (used to decide whether shutdown/reboot/halt should auto-sync). */
int fsdisk_is_installed(void);

/* Saves by itself: a background task writes the filesystem to the
 * installed disk ~2 s after the last change (files, folders, settings). */
void fsdisk_start_autosave(void);
/* 1 while there are changes not on disk yet */
int  fsdisk_pending(void);
/* the last sync's result (FSDISK_OK or an FSDISK_ERR_*) */
int  fsdisk_last_error(void);

/* Updating from a newer CD: the one hard disk holding Banana OS (1, *out),
 * whether its system matches this CD's (1 same, 0 differs, <0 error), and
 * rewriting its system from the CD - the files and settings stay. */
int  fsdisk_find_install(ata_disk_t* out);
int  fsdisk_compare_boot(void);
int  fsdisk_update(void);
/* the installed disk's layout: 4 (128 MB boot area, slots over the whole
 * disk), 3 (before: 32 MB, fixed slots - `update` moves it), 0 none */
int  fsdisk_install_layout(void);
/* the space the saved files take on the disk, and the space for them */
void fsdisk_space(uint64_t* used_bytes, uint64_t* capacity_bytes);
/* the installed disk's name ("SATA port 0") and model, for df ("" if none) */
void fsdisk_describe(char* out, int cap);
/* how far an install / update / comparison is (done of total) */
void fsdisk_progress(uint32_t* done, uint32_t* total);

#endif
