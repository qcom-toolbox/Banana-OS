#include "pci.h"
#include "io.h"

#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC

static uint32_t cfg_addr(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    return 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
           ((uint32_t)fn << 8) | (off & 0xFC);
}

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    outl(PCI_ADDR, cfg_addr(bus, dev, fn, off));
    return inl(PCI_DATA);
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v) {
    outl(PCI_ADDR, cfg_addr(bus, dev, fn, off));
    outl(PCI_DATA, v);
}

uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t v = pci_read32(bus, dev, fn, off);
    return (uint16_t)(v >> ((off & 2) * 8));
}

void pci_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint16_t v) {
    uint32_t cur = pci_read32(bus, dev, fn, off);
    int shift = (off & 2) * 8;
    cur &= ~(0xFFFFu << shift);
    cur |= (uint32_t)v << shift;
    pci_write32(bus, dev, fn, off, cur);
}

static void fill(pci_dev_t* d, uint8_t bus, uint8_t dev, uint8_t fn, uint32_t id) {
    d->bus = bus; d->dev = dev; d->fn = fn;
    d->vendor = (uint16_t)(id & 0xFFFF);
    d->device = (uint16_t)(id >> 16);
    uint32_t cls = pci_read32(bus, dev, fn, 0x08);
    d->class_code = (uint8_t)(cls >> 24);
    d->subclass   = (uint8_t)(cls >> 16);
    d->prog_if    = (uint8_t)(cls >> 8);
    d->irq_line   = (uint8_t)(pci_read32(bus, dev, fn, 0x3C) & 0xFF);
}

int pci_scan(int (*cb)(const pci_dev_t* d, void* ctx), void* ctx) {
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            uint32_t id0 = pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0);
            if ((id0 & 0xFFFF) == 0xFFFF) continue;
            uint32_t hdr = pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0x0C);
            int nfn = ((hdr >> 16) & 0x80) ? 8 : 1;  /* multi-function? */
            for (int fn = 0; fn < nfn; fn++) {
                uint32_t id = pci_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0);
                if ((id & 0xFFFF) == 0xFFFF) continue;
                pci_dev_t d;
                fill(&d, (uint8_t)bus, (uint8_t)dev, (uint8_t)fn, id);
                int r = cb(&d, ctx);
                if (r) return r;
            }
        }
    }
    return 0;
}

typedef struct { uint16_t vendor, device; pci_dev_t* out; } find_ctx_t;

static int find_cb(const pci_dev_t* d, void* ctx) {
    find_ctx_t* f = (find_ctx_t*)ctx;
    if (d->vendor == f->vendor && d->device == f->device) {
        *f->out = *d;
        return 1;
    }
    return 0;
}

int pci_find(uint16_t vendor, uint16_t device, pci_dev_t* out) {
    find_ctx_t f = { vendor, device, out };
    return pci_scan(find_cb, &f) ? 1 : 0;
}

uint32_t pci_bar(const pci_dev_t* d, int bar, int* is_io) {
    uint32_t v = pci_read32(d->bus, d->dev, d->fn, (uint8_t)(0x10 + bar * 4));
    if (v & 1) {
        if (is_io) *is_io = 1;
        return v & ~0x3u;
    }
    if (is_io) *is_io = 0;
    return v & ~0xFu;
}

void pci_enable(const pci_dev_t* d) {
    uint16_t cmd = pci_read16(d->bus, d->dev, d->fn, 0x04);
    cmd |= 0x0007;          /* I/O space, memory space, bus master */
    cmd &= (uint16_t)~0x0400; /* make sure INTx isn't disabled */
    pci_write16(d->bus, d->dev, d->fn, 0x04, cmd);
}
