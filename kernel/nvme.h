#ifndef NVME_H
#define NVME_H

#include "types.h"
#include "ata.h"

/*
 * NVMe SSDs (PCIe, QEMU `-device nvme`): namespace 1 of each controller
 * is a disk with 512-byte sectors, polled (no interrupts). kernel/disk.c
 * puts it next to the IDE and SATA disks (install, sync, booting from
 * it), and its FAT32 partitions are mounted at /mnt/nvme.
 */

#define NVME_MAX 4

void nvme_init(void);                  /* finds and starts the controllers */
int  nvme_probe(ata_disk_t* out, int max);
int  nvme_read(int idx, uint32_t lba, uint32_t count, void* buf);
int  nvme_write(int idx, uint32_t lba, uint32_t count, const void* buf);
int  nvme_flush(int idx);

#endif
