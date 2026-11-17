/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pci.h"
#include "io.h"
#include "paging.h"
#include "serial.h"
#include "kstring.h"

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

/* The functions there are: every bus and slot looked at once (256 x 32
 * reads of the vendor ID, two port accesses each - in a virtual machine
 * each one a VM exit, ~150 ms a scan under KVM), then only those. A driver
 * probe scans the bus each time - a dozen at boot. */
#define CACHE_MAX 256
typedef struct { uint8_t bus, dev, fn; uint32_t id, cls; } pci_fn_t;
static pci_fn_t g_fns[CACHE_MAX];
static int      g_nfns = -1;                   /* -1: not scanned yet; -2: too many to keep */
static uint32_t g_scans;

static void enumerate(void) {
    int n = 0;
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            uint32_t id0 = pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0);
            if ((id0 & 0xFFFF) == 0xFFFF) continue;
            uint32_t hdr = pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0x0C);
            int nfn = ((hdr >> 16) & 0x80) ? 8 : 1;  /* multi-function? */
            for (int fn = 0; fn < nfn; fn++) {
                uint32_t id = fn ? pci_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0) : id0;
                if ((id & 0xFFFF) == 0xFFFF) continue;
                if (n == CACHE_MAX) { g_nfns = -2; return; }
                g_fns[n++] = (pci_fn_t){ (uint8_t)bus, (uint8_t)dev, (uint8_t)fn, id,
                                         pci_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x08) };
            }
        }
    }
    g_nfns = n;
}

int pci_scan(int (*cb)(const pci_dev_t* d, void* ctx), void* ctx) {
    g_scans++;
    if (g_nfns == -1) enumerate();
    if (g_nfns < 0) {                            /* (more functions than kept: every time) */
        for (int bus = 0; bus < 256; bus++)
            for (int dev = 0; dev < 32; dev++) {
                uint32_t id0 = pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0);
                if ((id0 & 0xFFFF) == 0xFFFF) continue;
                uint32_t hdr = pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0x0C);
                int nfn = ((hdr >> 16) & 0x80) ? 8 : 1;
                for (int fn = 0; fn < nfn; fn++) {
                    uint32_t id = pci_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0);
                    if ((id & 0xFFFF) == 0xFFFF) continue;
                    pci_dev_t d;
                    fill(&d, (uint8_t)bus, (uint8_t)dev, (uint8_t)fn, id);
                    int r = cb(&d, ctx);
                    if (r) return r;
                }
            }
        return 0;
    }
    for (int i = 0; i < g_nfns; i++) {
        const pci_fn_t* f = &g_fns[i];
        pci_dev_t d;
        d.bus = f->bus; d.dev = f->dev; d.fn = f->fn;
        d.vendor = (uint16_t)(f->id & 0xFFFF);
        d.device = (uint16_t)(f->id >> 16);
        d.class_code = (uint8_t)(f->cls >> 24);
        d.subclass   = (uint8_t)(f->cls >> 16);
        d.prog_if    = (uint8_t)(f->cls >> 8);
        d.irq_line   = (uint8_t)(pci_read32(f->bus, f->dev, f->fn, 0x3C) & 0xFF);   /* (set by firmware or a driver: read now) */
        int r = cb(&d, ctx);
        if (r) return r;
    }
    return 0;
}

int pci_function_count(void) { return g_nfns; }
uint32_t pci_scan_count(void) { return g_scans; }

/* (selftest pci) every function found by a full scan now, against the list */
int pci_check_list(char* msg, int cap) {
    if (g_nfns < 0) { ksnprintf(msg, (size_t)cap, "no list kept (%d)", g_nfns); return -1; }
    int found = 0;
    for (int bus = 0; bus < 256; bus++)
        for (int dev = 0; dev < 32; dev++) {
            uint32_t id0 = pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0);
            if ((id0 & 0xFFFF) == 0xFFFF) continue;
            uint32_t hdr = pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0x0C);
            int nfn = ((hdr >> 16) & 0x80) ? 8 : 1;
            for (int fn = 0; fn < nfn; fn++) {
                uint32_t id = pci_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0);
                if ((id & 0xFFFF) == 0xFFFF) continue;
                int in = 0;
                for (int i = 0; i < g_nfns && !in; i++)
                    in = g_fns[i].bus == bus && g_fns[i].dev == dev && g_fns[i].fn == fn && g_fns[i].id == id;
                if (!in) { ksnprintf(msg, (size_t)cap, "%02x:%02x.%d (%08x) is not in the list", bus, dev, fn, id); return -1; }
                found++;
            }
        }
    if (found != g_nfns) { ksnprintf(msg, (size_t)cap, "%d functions now, %d in the list", found, g_nfns); return -1; }
    return found;
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

uint64_t pci_bar_size(const pci_dev_t* d, int bar) {
    if (bar < 0 || bar > 5) return 0;
    uint8_t off = (uint8_t)(0x10 + bar * 4);
    uint32_t lo = pci_read32(d->bus, d->dev, d->fn, off);
    if (lo & 1) return 0;                                   /* (an I/O BAR) */
    int is64 = (lo & 6) == 4 && bar < 5;
    uint32_t hi = is64 ? pci_read32(d->bus, d->dev, d->fn, (uint8_t)(off + 4)) : 0;
    uintptr_t f;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory");
    uint16_t cmd = pci_read16(d->bus, d->dev, d->fn, 0x04);
    pci_write16(d->bus, d->dev, d->fn, 0x04, (uint16_t)(cmd & ~2u));     /* memory decoding off */
    pci_write32(d->bus, d->dev, d->fn, off, 0xFFFFFFFFu);
    uint32_t mlo = pci_read32(d->bus, d->dev, d->fn, off);
    uint32_t mhi = 0xFFFFFFFFu;
    if (is64) {
        pci_write32(d->bus, d->dev, d->fn, (uint8_t)(off + 4), 0xFFFFFFFFu);
        mhi = pci_read32(d->bus, d->dev, d->fn, (uint8_t)(off + 4));
        pci_write32(d->bus, d->dev, d->fn, (uint8_t)(off + 4), hi);
    }
    pci_write32(d->bus, d->dev, d->fn, off, lo);
    pci_write16(d->bus, d->dev, d->fn, 0x04, cmd);
    if (f & 0x200) __asm__ volatile("sti" ::: "memory");
    uint64_t mask = ((uint64_t)mhi << 32) | (mlo & ~0xFull);
    if (!is64) mask |= 0xFFFFFFFF00000000ull;
    return (mlo & ~0xFull) ? ~mask + 1 : 0;
}

uintptr_t pci_bar(const pci_dev_t* d, int bar, int* is_io) {
    uint32_t v = pci_read32(d->bus, d->dev, d->fn, (uint8_t)(0x10 + bar * 4));
    if (v & 1) {
        if (is_io) *is_io = 1;
        return v & ~0x3u;
    }
    if (is_io) *is_io = 0;
    uint64_t addr = v & ~0xFu;
    if ((v & 0x6) == 0x4 && bar < 5)                 /* a 64-bit BAR: the high half follows */
        addr |= (uint64_t)pci_read32(d->bus, d->dev, d->fn, (uint8_t)(0x14 + bar * 4)) << 32;
    if (addr >= (4ull << 30)) {
        /* UEFI firmware puts 64-bit BARs high: map 16 MiB of it */
        if (!mmio_map(addr, 16ull << 20)) {
            klog("pci: %02x:%02x.%u BAR%d at %llx is out of reach\n", d->bus, d->dev, d->fn, bar, addr);
            return 0;
        }
    }
    return (uintptr_t)addr;
}

/* which driver took which function (Device Manager) */
#define PCI_CLAIMS 64
static struct { uint8_t bus, dev, fn, used; char name[16]; } g_claims[PCI_CLAIMS];

void pci_claim(const pci_dev_t* d, const char* driver) {
    int slot = -1;
    for (int i = 0; i < PCI_CLAIMS; i++) {
        if (g_claims[i].used && g_claims[i].bus == d->bus && g_claims[i].dev == d->dev && g_claims[i].fn == d->fn) { slot = i; break; }
        if (!g_claims[i].used && slot < 0) slot = i;
    }
    if (slot < 0) return;
    g_claims[slot].bus = d->bus; g_claims[slot].dev = d->dev; g_claims[slot].fn = d->fn;
    int n = 0;
    while (driver[n] && n < (int)sizeof(g_claims[slot].name) - 1) { g_claims[slot].name[n] = driver[n]; n++; }
    g_claims[slot].name[n] = 0;
    g_claims[slot].used = 1;
}

const char* pci_driver_of(uint8_t bus, uint8_t dev, uint8_t fn) {
    for (int i = 0; i < PCI_CLAIMS; i++)
        if (g_claims[i].used && g_claims[i].bus == bus && g_claims[i].dev == dev && g_claims[i].fn == fn) return g_claims[i].name;
    return NULL;
}

void pci_enable_from(const pci_dev_t* d, const char* file) {
    /* "net/e1000.c" -> "e1000" */
    const char* b = file;
    for (const char* q = file; *q; q++) if (*q == '/') b = q + 1;
    char name[16];
    int n = 0;
    while (b[n] && b[n] != '.' && n < (int)sizeof(name) - 1) { name[n] = b[n]; n++; }
    name[n] = 0;
    pci_claim(d, name);
    uint16_t cmd = pci_read16(d->bus, d->dev, d->fn, 0x04);
    cmd |= 0x0007;          /* I/O space, memory space, bus master */
    cmd &= (uint16_t)~0x0400; /* make sure INTx isn't disabled */
    pci_write16(d->bus, d->dev, d->fn, 0x04, cmd);
}
