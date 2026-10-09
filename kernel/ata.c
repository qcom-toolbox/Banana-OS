#include "ata.h"

/* ── I/O helpers (each kernel .c file keeps its own copy - see usb.c) ── */
static inline uint8_t  inb(uint16_t p) { uint8_t  v; __asm__ volatile("inb  %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void     outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline uint16_t inw(uint16_t p) { uint16_t v; __asm__ volatile("inw  %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void     outw(uint16_t p, uint16_t v) { __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p)); }

#define ATA_REG_DATA       0
#define ATA_REG_FEATURES   1
#define ATA_REG_SECCOUNT0  2
#define ATA_REG_LBA0       3
#define ATA_REG_LBA1       4
#define ATA_REG_LBA2       5
#define ATA_REG_HDDEVSEL   6
#define ATA_REG_STATUS     7
#define ATA_REG_COMMAND    7

#define ATA_SR_ERR  0x01
#define ATA_SR_DF   0x20
#define ATA_SR_DRQ  0x08
#define ATA_SR_BSY  0x80

#define ATA_CMD_READ_SECTORS  0x20
#define ATA_CMD_READ_EXT      0x24
#define ATA_CMD_WRITE_SECTORS 0x30
#define ATA_CMD_WRITE_EXT     0x34
#define ATA_CMD_CACHE_FLUSH   0xE7
#define ATA_CMD_FLUSH_EXT     0xEA
#define ATA_CMD_IDENTIFY      0xEC

static uint8_t g_lba48[2][2];      /* [bus][slave]: the drive takes 48-bit LBAs */

static uint16_t bus_io(int bus)   { return (bus == ATA_BUS_PRIMARY) ? 0x1F0 : 0x170; }
static uint16_t bus_ctrl(int bus) { return (bus == ATA_BUS_PRIMARY) ? 0x3F6 : 0x376; }

/* Reading the alternate status register costs ~100ns on real hardware;
 * four reads is the ATA spec's standard "400ns" settle delay after a
 * drive select or reset, before status bits can be trusted. */
static void io_wait(uint16_t ctrl) {
    inb(ctrl); inb(ctrl); inb(ctrl); inb(ctrl);
}

/* Polls STATUS until BSY clears (bounded), returns the last status byte
 * read, or -1 if it never cleared within `spins` iterations. */
static int wait_not_busy(uint16_t io, uint32_t spins) {
    uint8_t st = 0;
    while (spins--) {
        st = inb(io + ATA_REG_STATUS);
        if (!(st & ATA_SR_BSY)) return st;
    }
    return -1;
}

/* Polls until BSY clears and DRQ sets (ready to transfer a sector), or
 * until ERR/DF sets. Returns 0 if ready, -1 on error/timeout. */
static int wait_drq(uint16_t io, uint32_t spins) {
    while (spins--) {
        uint8_t st = inb(io + ATA_REG_STATUS);
        if (st & (ATA_SR_ERR | ATA_SR_DF)) return -1;
        if (!(st & ATA_SR_BSY) && (st & ATA_SR_DRQ)) return 0;
    }
    return -1;
}

static void select_drive(int bus, int is_slave) {
    uint16_t io = bus_io(bus), ctrl = bus_ctrl(bus);
    outb(io + ATA_REG_HDDEVSEL, (uint8_t)(0xA0 | (is_slave ? 0x10 : 0x00)));
    io_wait(ctrl);
}

/* Sends IDENTIFY DEVICE and fills *out. Returns 1 if a drive answered at
 * all (ATA or ATAPI - caller must check out->is_atapi), 0 if nothing is
 * wired up at this bus/position. ATAPI (CD/DVD) drives answer the initial
 * select with a "0x14,0xEB" signature in LBA1/LBA2 instead of returning
 * IDENTIFY data - we detect that and bail out without reading further, so
 * the GRUB boot CD is never touched by anything below. */
static int identify_one(int bus, int is_slave, ata_disk_t* out) {
    out->present  = 0;
    out->is_atapi = 0;
    out->bus      = bus;
    out->is_slave = is_slave;
    out->sectors  = 0;
    out->model[0] = '\0';
    out->ahci_port = -1;
    out->nvme = 0;

    uint16_t io = bus_io(bus);

    select_drive(bus, is_slave);
    outb(io + ATA_REG_SECCOUNT0, 0);
    outb(io + ATA_REG_LBA0, 0);
    outb(io + ATA_REG_LBA1, 0);
    outb(io + ATA_REG_LBA2, 0);
    outb(io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    uint8_t status = inb(io + ATA_REG_STATUS);
    if (status == 0x00 || status == 0xFF) return 0; /* floating bus - nothing here */

    int st = wait_not_busy(io, 100000);
    if (st < 0) return 0; /* timed out - treat as absent rather than hang forever */

    uint8_t lba1 = inb(io + ATA_REG_LBA1);
    uint8_t lba2 = inb(io + ATA_REG_LBA2);
    if (lba1 == 0x14 && lba2 == 0xEB) {
        out->present  = 1;
        out->is_atapi = 1;
        return 1; /* ATAPI (CD/DVD) - report present, but never a write target */
    }
    if (lba1 != 0 || lba2 != 0) {
        return 0; /* neither plain ATA nor ATAPI signature - don't guess */
    }
    if ((uint8_t)st & ATA_SR_ERR) return 0; /* IDENTIFY aborted */

    if (!((uint8_t)st & ATA_SR_DRQ) && wait_drq(io, 100000) < 0) return 0;

    uint16_t ident[256];
    for (int w = 0; w < 256; w++) ident[w] = inw(io + ATA_REG_DATA);

    out->present  = 1;
    out->is_atapi = 0;
    out->sectors  = ((uint32_t)ident[61] << 16) | ident[60]; /* LBA28 total sectors */
    /* 48-bit addressing (word 83 bit 10): disks past 128 GB, whole (up to 2 TB here) */
    g_lba48[bus & 1][is_slave & 1] = 0;
    if ((ident[83] & (1u << 10)) && (ident[83] & 0xC000) == 0x4000) {
        uint64_t s48 = (uint64_t)ident[100] | (uint64_t)ident[101] << 16 | (uint64_t)ident[102] << 32 | (uint64_t)ident[103] << 48;
        if (s48 > out->sectors) {
            out->sectors = s48 > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)s48;
            g_lba48[bus & 1][is_slave & 1] = 1;
        }
    }

    for (int i = 0; i < 20; i++) {
        uint16_t w = ident[27 + i];
        out->model[i * 2]     = (char)(w >> 8);
        out->model[i * 2 + 1] = (char)(w & 0xFF);
    }
    out->model[40] = '\0';
    for (int i = 39; i >= 0 && out->model[i] == ' '; i--) out->model[i] = '\0';

    return 1;
}

int ata_probe_disks(ata_disk_t* out) {
    static const int buses[2] = { ATA_BUS_PRIMARY, ATA_BUS_SECONDARY };
    int n = 0;
    int found = 0;
    for (int b = 0; b < 2; b++) {
        for (int slave = 0; slave < 2; slave++) {
            ata_disk_t* d = &out[n++];
            if (identify_one(buses[b], slave, d)) found++;
        }
    }
    return found;
}

static void setup_lba28(int bus, int is_slave, uint32_t lba, uint8_t count) {
    uint16_t io = bus_io(bus);
    select_drive(bus, is_slave);
    outb(io + ATA_REG_HDDEVSEL, (uint8_t)(0xE0 | (is_slave ? 0x10 : 0x00) | ((lba >> 24) & 0x0F)));
    outb(io + ATA_REG_FEATURES, 0);
    outb(io + ATA_REG_SECCOUNT0, count);
    outb(io + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
    outb(io + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(io + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
}

/* LBA48: the high bytes go first into the same registers (they are FIFOs) */
static void setup_lba48(int bus, int is_slave, uint32_t lba, uint8_t count) {
    uint16_t io = bus_io(bus);
    select_drive(bus, is_slave);
    outb(io + ATA_REG_HDDEVSEL, (uint8_t)(0x40 | (is_slave ? 0x10 : 0x00)));
    outb(io + ATA_REG_SECCOUNT0, 0);                         /* count bits 8-15 */
    outb(io + ATA_REG_LBA0, (uint8_t)(lba >> 24));
    outb(io + ATA_REG_LBA1, 0);                              /* LBA bits 32-47 */
    outb(io + ATA_REG_LBA2, 0);
    outb(io + ATA_REG_SECCOUNT0, count);
    outb(io + ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
    outb(io + ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(io + ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
}

/* past the 28-bit range (128 GB) with a drive that has 48-bit commands */
static int use48(int bus, int is_slave, uint32_t lba, uint8_t count) {
    return g_lba48[bus & 1][is_slave & 1] && (uint64_t)lba + count > 0x0FFFFFFFu;
}

int ata_read_sectors(int bus, int is_slave, uint32_t lba, uint8_t count, void* buf) {
    if (count == 0 || !buf) return -1;
    uint16_t io = bus_io(bus);
    if (use48(bus, is_slave, lba, count)) {
        setup_lba48(bus, is_slave, lba, count);
        outb(io + ATA_REG_COMMAND, ATA_CMD_READ_EXT);
    } else {
        setup_lba28(bus, is_slave, lba, count);
        outb(io + ATA_REG_COMMAND, ATA_CMD_READ_SECTORS);
    }

    uint16_t* dst = (uint16_t*)buf;
    for (int s = 0; s < count; s++) {
        if (wait_drq(io, 1000000) < 0) return -1;
        for (int w = 0; w < 256; w++) dst[s * 256 + w] = inw(io + ATA_REG_DATA);
    }
    return 0;
}

int ata_write_sectors(int bus, int is_slave, uint32_t lba, uint8_t count, const void* buf) {
    if (count == 0 || !buf) return -1;
    uint16_t io = bus_io(bus);
    int ext = use48(bus, is_slave, lba, count);
    if (ext) {
        setup_lba48(bus, is_slave, lba, count);
        outb(io + ATA_REG_COMMAND, ATA_CMD_WRITE_EXT);
    } else {
        setup_lba28(bus, is_slave, lba, count);
        outb(io + ATA_REG_COMMAND, ATA_CMD_WRITE_SECTORS);
    }

    const uint16_t* src = (const uint16_t*)buf;
    for (int s = 0; s < count; s++) {
        if (wait_drq(io, 1000000) < 0) return -1;
        for (int w = 0; w < 256; w++) outw(io + ATA_REG_DATA, src[s * 256 + w]);
    }

    outb(io + ATA_REG_COMMAND, ext ? ATA_CMD_FLUSH_EXT : ATA_CMD_CACHE_FLUSH);
    if (wait_not_busy(io, 1000000) < 0) return -1;
    return 0;
}
