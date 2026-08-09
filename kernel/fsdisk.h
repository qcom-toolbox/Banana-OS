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
#define FSDISK_ERR_TOO_SMALL   -4 /* target disk too small for the boot region + filesystem */
#define FSDISK_ERR_ISO_TOO_BIG -5 /* boot image bigger than the reserved region (build issue) */
#define FSDISK_ERR_IO          -6 /* a read or write failed */

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
int fsdisk_try_load(void);

/* Whether this session has a remembered installed/loaded target disk
 * (used to decide whether shutdown/reboot/halt should auto-sync). */
int fsdisk_is_installed(void);

#endif
