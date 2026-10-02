#include "disk.h"
#include "atapi.h"
#include "ahci.h"
#include "kstring.h"

int disk_probe(ata_disk_t* out, int max) {
    ata_disk_t ide[4];
    ata_probe_disks(ide);
    int n = 0;
    for (int i = 0; i < 4 && n < max; i++)
        if (ide[i].present) out[n++] = ide[i];
    n += ahci_probe(out + n, max - n);
    return n;
}

int disk_read(const ata_disk_t* d, uint32_t lba, uint32_t count, void* buf) {
    if (d->ahci_port >= 0) return ahci_read(d->ahci_port, lba, count, buf);
    uint8_t* b = (uint8_t*)buf;
    while (count) {
        uint8_t n = (uint8_t)(count > 255 ? 255 : count);
        if (ata_read_sectors(d->bus, d->is_slave, lba, n, b) != 0) return -1;
        lba += n;
        count -= n;
        b += (uint32_t)n * 512;
    }
    return 0;
}

int disk_write(const ata_disk_t* d, uint32_t lba, uint32_t count, const void* buf) {
    if (d->ahci_port >= 0) return ahci_write(d->ahci_port, lba, count, buf);
    const uint8_t* b = (const uint8_t*)buf;
    while (count) {
        uint8_t n = (uint8_t)(count > 255 ? 255 : count);
        if (ata_write_sectors(d->bus, d->is_slave, lba, n, b) != 0) return -1;
        lba += n;
        count -= n;
        b += (uint32_t)n * 512;
    }
    return 0;
}

int disk_flush(const ata_disk_t* d) {
    return d->ahci_port >= 0 ? ahci_flush(d->ahci_port) : 0;   /* ata.c flushes after each write */
}

int disk_cd_read(const ata_disk_t* d, uint32_t lba, uint32_t count, void* buf) {
    if (d->ahci_port >= 0) return ahci_atapi_read(d->ahci_port, lba, count, buf);
    return atapi_read_blocks(d->bus, d->is_slave, lba, count, buf);
}

int disk_cd_iso_size(const ata_disk_t* d, uint32_t* out_bytes) {
    if (d->ahci_port < 0) return atapi_iso_size(d->bus, d->is_slave, out_bytes);
    static uint8_t pvd[2048] __attribute__((aligned(16)));
    if (ahci_atapi_read(d->ahci_port, 16, 1, pvd) != 0) return -1;
    if (pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) return -1;
    uint32_t blocks = (uint32_t)pvd[80] | (uint32_t)pvd[81] << 8 | (uint32_t)pvd[82] << 16 | (uint32_t)pvd[83] << 24;
    *out_bytes = blocks * 2048u;
    return 0;
}

void disk_describe(const ata_disk_t* d, char* out, int cap) {
    if (d->ahci_port >= 0) ksnprintf(out, (size_t)cap, "SATA #%d", d->ahci_port);
    else ksnprintf(out, (size_t)cap, "IDE %s %s", d->bus == ATA_BUS_PRIMARY ? "primary" : "secondary",
                   d->is_slave ? "slave" : "master");
}
