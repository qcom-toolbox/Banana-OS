#ifndef FSDISK_H
#define FSDISK_H

#include "types.h"
#include "ata.h"

/* Persists the in-memory filesystem (kernel/fs.c) to a dedicated ATA hard
 * disk, so its contents survive a reboot. This only makes the filesystem
 * *contents* persistent across boots of the same GRUB-loaded kernel - it
 * does not make the disk itself bootable (Banana OS still boots from the
 * GRUB CD/ISO every time). */

/* Looks for exactly one non-ATAPI ATA disk attached (any bus/position).
 * Returns 1 and fills *out if exactly one was found, 0 if none, -1 if
 * more than one (ambiguous - caller should ask the user to detach extras). */
int fsdisk_find_target(ata_disk_t* out);

/* Formats the target disk found by fsdisk_find_target() and writes the
 * current in-memory filesystem to it. Destroys any prior disk contents.
 * Returns 0 on success, -1 on failure (no/ambiguous target, or I/O error). */
int fsdisk_install(void);

/* Re-writes the current in-memory filesystem to the disk this session
 * installed to or booted from. Returns -1 if this session isn't
 * associated with an installed disk yet (call fsdisk_install() first). */
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
