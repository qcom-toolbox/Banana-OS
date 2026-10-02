#ifndef AHCI_H
#define AHCI_H

#include "types.h"
#include "ata.h"

/*
 * AHCI (SATA) host controller driver: hard disks / SSDs (READ/WRITE DMA
 * EXT, 48-bit LBA) and SATA CD/DVD drives (ATAPI packets), the way modern
 * PCs attach their drives. Polled, one command at a time per port.
 */

/* Finds the AHCI controllers (first call) and fills out[] with the SATA
 * devices found (present = 1, ahci_port >= 0), at most max. Returns how many. */
int ahci_probe(ata_disk_t* out, int max);

/* 512-byte sectors on a SATA disk; 0 = ok, -1 = error/timeout */
int ahci_read(int port, uint32_t lba, uint32_t count, void* buf);
int ahci_write(int port, uint32_t lba, uint32_t count, const void* buf);
int ahci_flush(int port);

/* 2048-byte blocks on a SATA CD/DVD drive */
int ahci_atapi_read(int port, uint32_t lba, uint32_t count, void* buf);

/* one line per controller and port, for `lsblk`-style listings */
const char* ahci_status(void);

#endif
