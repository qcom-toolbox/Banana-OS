#ifndef ATAPI_H
#define ATAPI_H

#include "types.h"

/* Minimal read-only ATAPI PIO packet driver - just enough to read the CD
 * Banana OS itself booted from (2048-byte logical blocks), so `install`
 * can raw-copy the already directly-BIOS-bootable (GRUB "hybrid" MBR)
 * ISO image onto a target ATA hard disk without needing any of our own
 * bootloader/protected-mode/VBE code - GRUB's own boot record does the
 * same job on the hard disk it already does on the CD. */

/* Reads `count` consecutive 2048-byte logical blocks starting at `lba`
 * (ISO9660 block units, NOT ATA 512-byte sectors) into buf (buf must be
 * count*2048 bytes). Returns 0 on success, -1 on error/timeout. */
int atapi_read_blocks(int bus, int is_slave, uint32_t lba, uint32_t count, void* buf);

/* Reads the ISO9660 Primary Volume Descriptor (fixed at block 16) and
 * returns the total image size in bytes via *out_bytes. Returns 0 on
 * success (valid "CD001" PVD found), -1 otherwise. */
int atapi_iso_size(int bus, int is_slave, uint32_t* out_bytes);

#endif
