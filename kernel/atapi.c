#include "atapi.h"

/* ── I/O helpers (each kernel .c file keeps its own copy - see usb.c) ── */
static inline uint8_t  inb(uint16_t p) { uint8_t  v; __asm__ volatile("inb  %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void     outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline uint16_t inw(uint16_t p) { uint16_t v; __asm__ volatile("inw  %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void     outw(uint16_t p, uint16_t v) { __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p)); }

#define ATA_REG_DATA      0
#define ATA_REG_FEATURES  1
#define ATA_REG_LBA1      4 /* "byte count limit" (low) once PACKET is issued */
#define ATA_REG_LBA2      5 /* "byte count limit" (high) once PACKET is issued */
#define ATA_REG_HDDEVSEL  6
#define ATA_REG_STATUS    7
#define ATA_REG_COMMAND   7

#define ATA_SR_ERR  0x01
#define ATA_SR_DRQ  0x08
#define ATA_SR_BSY  0x80

#define ATA_CMD_PACKET 0xA0

static uint16_t bus_io(int bus)   { return (bus == 0) ? 0x1F0 : 0x170; }
static uint16_t bus_ctrl(int bus) { return (bus == 0) ? 0x3F6 : 0x376; }

static void io_wait(uint16_t ctrl) { inb(ctrl); inb(ctrl); inb(ctrl); inb(ctrl); }

static int wait_not_busy(uint16_t io, uint32_t spins) {
    while (spins--) { if (!(inb(io + ATA_REG_STATUS) & ATA_SR_BSY)) return 0; }
    return -1;
}

/* Waits for either DRQ (more data/command ready to transfer) or a clean
 * "finished" state (BSY and DRQ both clear). Returns -1 on error/timeout,
 * 0 otherwise with *done set to indicate which case happened. */
static int wait_drq_or_done(uint16_t io, uint32_t spins, int* done) {
    *done = 0;
    while (spins--) {
        uint8_t st = inb(io + ATA_REG_STATUS);
        if (st & ATA_SR_ERR) return -1;
        if (st & ATA_SR_BSY) continue;
        if (st & ATA_SR_DRQ) return 0;
        *done = 1;
        return 0;
    }
    return -1;
}

static void select_drive(int bus, int is_slave) {
    uint16_t io = bus_io(bus), ctrl = bus_ctrl(bus);
    outb(io + ATA_REG_HDDEVSEL, (uint8_t)(0xA0 | (is_slave ? 0x10 : 0x00)));
    io_wait(ctrl);
}

int atapi_read_blocks(int bus, int is_slave, uint32_t lba, uint32_t count, void* buf) {
    if (count == 0 || !buf) return -1;
    uint16_t io = bus_io(bus);

    select_drive(bus, is_slave);
    if (wait_not_busy(io, 100000) < 0) return -1;

    outb(io + ATA_REG_FEATURES, 0);   /* PIO, no DMA/overlap */
    outb(io + ATA_REG_LBA1, 0xFE);    /* byte count limit ~64KB - the drive */
    outb(io + ATA_REG_LBA2, 0xFF);    /* may still transfer smaller chunks, handled below */
    outb(io + ATA_REG_COMMAND, ATA_CMD_PACKET);

    if (wait_not_busy(io, 100000) < 0) return -1;
    int done = 0;
    if (wait_drq_or_done(io, 100000, &done) < 0 || done) return -1;

    uint8_t packet[12] = {0};
    packet[0] = 0xA8; /* READ(12) */
    packet[2] = (uint8_t)((lba >> 24) & 0xFF);
    packet[3] = (uint8_t)((lba >> 16) & 0xFF);
    packet[4] = (uint8_t)((lba >> 8) & 0xFF);
    packet[5] = (uint8_t)(lba & 0xFF);
    packet[6] = (uint8_t)((count >> 24) & 0xFF);
    packet[7] = (uint8_t)((count >> 16) & 0xFF);
    packet[8] = (uint8_t)((count >> 8) & 0xFF);
    packet[9] = (uint8_t)(count & 0xFF);

    const uint16_t* pw = (const uint16_t*)(const void*)packet;
    for (int i = 0; i < 6; i++) outw(io + ATA_REG_DATA, pw[i]);

    uint8_t* out = (uint8_t*)buf;
    uint32_t total = count * 2048u;
    uint32_t got = 0;
    while (got < total) {
        if (wait_not_busy(io, 1000000) < 0) return -1;
        int d = 0;
        if (wait_drq_or_done(io, 1000000, &d) < 0) return -1;
        if (d) break; /* device signalled finished */

        uint32_t chunk = ((uint32_t)inb(io + ATA_REG_LBA2) << 8) | inb(io + ATA_REG_LBA1);
        if (chunk == 0) break;
        if (chunk > total - got) chunk = total - got;

        uint32_t words = chunk / 2;
        for (uint32_t w = 0; w < words; w++) {
            uint16_t v = inw(io + ATA_REG_DATA);
            out[got++] = (uint8_t)(v & 0xFF);
            out[got++] = (uint8_t)(v >> 8);
        }
    }
    return (got >= total) ? 0 : -1;
}

int atapi_iso_size(int bus, int is_slave, uint32_t* out_bytes) {
    uint8_t pvd[2048];
    if (atapi_read_blocks(bus, is_slave, 16, 1, pvd) != 0) return -1;
    if (pvd[0] != 1) return -1; /* not a Primary Volume Descriptor */
    if (pvd[1] != 'C' || pvd[2] != 'D' || pvd[3] != '0' || pvd[4] != '0' || pvd[5] != '1') return -1;

    /* "Volume Space Size" (both-byte-order field, offset 80): total number
     * of logical blocks (2048 bytes each) in the volume - little-endian
     * copy comes first. */
    uint32_t blocks = (uint32_t)pvd[80] | ((uint32_t)pvd[81] << 8) |
                       ((uint32_t)pvd[82] << 16) | ((uint32_t)pvd[83] << 24);
    *out_bytes = blocks * 2048u;
    return 0;
}
