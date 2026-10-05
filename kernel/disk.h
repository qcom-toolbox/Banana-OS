#ifndef DISK_H
#define DISK_H

#include "types.h"
#include "ata.h"

/* Disks of every kind - IDE (ata.c, atapi.c), SATA (ahci.c), NVMe (nvme.c) - behind
 * one interface, for install/sync and the disk listing. */

#define DISK_MAX 20

/* every present drive (hard disks and CD/DVD drives); returns the count */
int disk_probe(ata_disk_t* out, int max);

/* 512-byte sectors; 0 = ok */
int disk_read(const ata_disk_t* d, uint32_t lba, uint32_t count, void* buf);
int disk_write(const ata_disk_t* d, uint32_t lba, uint32_t count, const void* buf);
int disk_flush(const ata_disk_t* d);

/* CD/DVD: 2048-byte blocks, and the size of the ISO9660 image on it */
int disk_cd_read(const ata_disk_t* d, uint32_t lba, uint32_t count, void* buf);
int disk_cd_iso_size(const ata_disk_t* d, uint32_t* out_bytes);

/* "IDE primary master" / "SATA port 2" */
void disk_describe(const ata_disk_t* d, char* out, int cap);

#endif
