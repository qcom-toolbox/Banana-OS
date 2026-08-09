#ifndef ATA_H
#define ATA_H

#include "types.h"

/* Minimal 28-bit LBA ATA PIO driver (primary + secondary bus, master +
 * slave). Only handles plain ATA hard disks - ATAPI (CD/DVD) devices are
 * detected and skipped, so this never touches the GRUB boot CD. */

#define ATA_BUS_PRIMARY   0
#define ATA_BUS_SECONDARY 1

typedef struct {
    int      present;   /* drive responded to IDENTIFY/IDENTIFY PACKET */
    int      is_atapi;  /* CD/DVD (ATAPI) - never a valid install target */
    int      bus;        /* ATA_BUS_PRIMARY / ATA_BUS_SECONDARY */
    int      is_slave;
    uint32_t sectors;    /* 28-bit LBA sector count (0 if unknown/ATAPI) */
    char     model[41];  /* IDENTIFY model string, NUL-terminated */
} ata_disk_t;

/* Probes all 4 possible drives (primary/secondary x master/slave) into
 * out[0..3] (out must have room for 4). Returns the number of entries with
 * present == 1 (both ATA and ATAPI). */
int ata_probe_disks(ata_disk_t* out);

/* Reads/writes `count` consecutive 512-byte sectors starting at `lba`
 * to/from buf (buf must be count*512 bytes). Returns 0 on success, -1 on
 * error or timeout. count must be 1-255 (0 is reserved by the ATA spec
 * for "256 sectors", not supported here). */
int ata_read_sectors(int bus, int is_slave, uint32_t lba, uint8_t count, void* buf);
int ata_write_sectors(int bus, int is_slave, uint32_t lba, uint8_t count, const void* buf);

#endif
