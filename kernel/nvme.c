#include "nvme.h"
#include "pci.h"
#include "kheap.h"
#include "kstring.h"
#include "timer.h"
#include "serial.h"
#include "blockdev.h"

/*
 * NVMe: one admin queue pair and one I/O queue pair per controller, both
 * polled. Transfers go through a 64 KiB bounce buffer (16 pages: PRP1 +
 * a PRP list), so any caller buffer works.
 */

#define QDEPTH   32
#define CHUNK    65536u
#define PAGE     4096u

typedef struct {
    volatile uint8_t* regs;
    uint32_t dstrd;               /* doorbell stride: 4 << CAP.DSTRD */
    uint8_t* asq; uint8_t* acq;   /* admin queues */
    uint8_t* iosq; uint8_t* iocq; /* I/O queues */
    uint16_t asq_tail, acq_head, iosq_tail, iocq_head;
    int      aphase, iophase;
    uint16_t cid;
    uint8_t* bounce;              /* CHUNK bytes */
    uint64_t* prps;               /* the PRP list for the bounce buffer */
    uint32_t sectors;             /* namespace 1, 512-byte sectors (28-bit API) */
    char     model[41];
    blockdev_t* bd;
} nvme_t;

static nvme_t g_nv[NVME_MAX];
static int    g_count;

static uint32_t r32(nvme_t* n, uint32_t o) { return *(volatile uint32_t*)(n->regs + o); }
static void w32(nvme_t* n, uint32_t o, uint32_t v) { *(volatile uint32_t*)(n->regs + o) = v; }
static uint64_t r64(nvme_t* n, uint32_t o) { return (uint64_t)r32(n, o) | ((uint64_t)r32(n, o + 4) << 32); }
static void w64(nvme_t* n, uint32_t o, uint64_t v) { w32(n, o, (uint32_t)v); w32(n, o + 4, (uint32_t)(v >> 32)); }

static void* page_alloc(uint32_t size) {
    uint8_t* raw = (uint8_t*)kmalloc(size + PAGE);
    if (!raw) return NULL;
    uint8_t* p = (uint8_t*)(((uintptr_t)raw + PAGE - 1) & ~(uintptr_t)(PAGE - 1));
    memset(p, 0, size);
    return p;
}

/* submits a 64-byte command on queue qid (0 admin, 1 I/O) and waits for
 * its completion; returns the status field (0 = success), -1 on a timeout */
static int submit(nvme_t* n, int qid, uint32_t* cmd) {
    uint8_t* sq = qid ? n->iosq : n->asq;
    uint8_t* cq = qid ? n->iocq : n->acq;
    uint16_t* tail = qid ? &n->iosq_tail : &n->asq_tail;
    uint16_t* head = qid ? &n->iocq_head : &n->acq_head;
    int* phase = qid ? &n->iophase : &n->aphase;
    uint16_t cid = ++n->cid;
    cmd[0] = (cmd[0] & 0xFFFF) | ((uint32_t)cid << 16);
    memcpy(sq + (uint32_t)*tail * 64, cmd, 64);
    *tail = (uint16_t)((*tail + 1) % QDEPTH);
    w32(n, 0x1000 + (uint32_t)(2 * qid) * n->dstrd, *tail);          /* SQ tail doorbell */
    uint32_t start = timer_ms();
    for (;;) {
        volatile uint32_t* e = (volatile uint32_t*)(cq + (uint32_t)*head * 16);
        uint32_t dw3 = e[3];
        if ((int)((dw3 >> 16) & 1) == *phase) {
            *head = (uint16_t)((*head + 1) % QDEPTH);
            if (*head == 0) *phase ^= 1;
            w32(n, 0x1000 + (uint32_t)(2 * qid + 1) * n->dstrd, *head);   /* CQ head doorbell */
            if ((dw3 & 0xFFFF) == cid) return (int)((dw3 >> 17) & 0x7FFF);
            klog("nvme: stray completion %x (want cid %x)\n", dw3, cid);
        }
        if (timer_ms() - start > 5000) return -1;
        timer_idle();
    }
}

static int admin(nvme_t* n, uint8_t opc, uint32_t nsid, void* prp1, uint32_t c10, uint32_t c11) {
    uint32_t cmd[16];
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = opc;
    cmd[1] = nsid;
    uint64_t p = (uint64_t)(uintptr_t)prp1;
    cmd[6] = (uint32_t)p;
    cmd[7] = (uint32_t)(p >> 32);
    cmd[10] = c10;
    cmd[11] = c11;
    return submit(n, 0, cmd);
}

static int start_controller(nvme_t* n, const pci_dev_t* pd) {
    int io;
    uintptr_t bar = pci_bar(pd, 0, &io);
    if (io || !bar) return -1;
    pci_enable(pd);
    n->regs = (volatile uint8_t*)(uintptr_t)bar;
    uint64_t cap = r64(n, 0x00);
    n->dstrd = 4u << ((cap >> 32) & 0xF);
    uint32_t timeout = (uint32_t)((cap >> 24) & 0xFF) * 500 + 500;
    if (((cap >> 37) & 1) == 0) { klog("nvme: no NVM command set\n"); return -1; }
    if ((cap & 0xFFFF) + 1 < QDEPTH) { klog("nvme: queues too small\n"); return -1; }

    /* disable, set up the admin queues, enable */
    w32(n, 0x14, r32(n, 0x14) & ~1u);
    uint32_t t0 = timer_ms();
    while (r32(n, 0x1C) & 1) { if (timer_ms() - t0 > timeout) return -1; timer_sleep_ms(1); }
    n->asq = (uint8_t*)page_alloc(PAGE);
    n->acq = (uint8_t*)page_alloc(PAGE);
    n->iosq = (uint8_t*)page_alloc(PAGE);
    n->iocq = (uint8_t*)page_alloc(PAGE);
    n->bounce = (uint8_t*)page_alloc(CHUNK);
    n->prps = (uint64_t*)page_alloc(PAGE);
    if (!n->asq || !n->acq || !n->iosq || !n->iocq || !n->bounce || !n->prps) return -1;
    for (uint32_t i = 1; i < CHUNK / PAGE; i++) n->prps[i - 1] = (uint64_t)(uintptr_t)(n->bounce + i * PAGE);
    w32(n, 0x24, ((uint32_t)(QDEPTH - 1) << 16) | (QDEPTH - 1));
    w64(n, 0x28, (uint64_t)(uintptr_t)n->asq);
    w64(n, 0x30, (uint64_t)(uintptr_t)n->acq);
    n->aphase = n->iophase = 1;
    /* polled: no interrupts at all (a level-triggered INTx nobody acks
     * would keep the CPU in the interrupt handler of whoever shares it) */
    w32(n, 0x0C, 0xFFFFFFFFu);                         /* INTMS: mask every vector */
    pci_write16(pd->bus, pd->dev, pd->fn, 0x04, (uint16_t)(pci_read16(pd->bus, pd->dev, pd->fn, 0x04) | (1u << 10)));
    w32(n, 0x14, (4u << 20) | (6u << 16) | 1u);        /* CQ entry 16 B, SQ entry 64 B, 4 KiB pages, enable */
    t0 = timer_ms();
    while (!(r32(n, 0x1C) & 1)) {
        if (r32(n, 0x1C) & 2) { klog("nvme: controller fatal status\n"); return -1; }
        if (timer_ms() - t0 > timeout) { klog("nvme: controller does not become ready\n"); return -1; }
        timer_sleep_ms(1);
    }

    uint8_t* id = (uint8_t*)page_alloc(PAGE);
    if (!id) return -1;
    if (admin(n, 0x06, 0, id, 1, 0) != 0) { klog("nvme: identify controller failed\n"); return -1; }
    memcpy(n->model, id + 24, 40);
    n->model[40] = 0;
    for (int i = 39; i >= 0 && n->model[i] == ' '; i--) n->model[i] = 0;
    memset(id, 0, PAGE);
    if (admin(n, 0x06, 1, id, 0, 0) != 0) { klog("nvme: identify namespace failed\n"); return -1; }
    uint64_t nsze;
    memcpy(&nsze, id, 8);
    uint8_t flbas = id[26] & 0xF;
    uint32_t lbaf;
    memcpy(&lbaf, id + 128 + flbas * 4, 4);
    uint32_t lbads = (lbaf >> 16) & 0xFF;
    if (lbads != 9) { klog("nvme: %u-byte sectors are not supported\n", 1u << lbads); return -1; }
    n->sectors = nsze > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)nsze;

    /* the I/O queue pair (id 1), polled */
    if (admin(n, 0x05, 0, n->iocq, ((uint32_t)(QDEPTH - 1) << 16) | 1, 1) != 0) { klog("nvme: cannot create the I/O CQ\n"); return -1; }
    if (admin(n, 0x01, 0, n->iosq, ((uint32_t)(QDEPTH - 1) << 16) | 1, (1u << 16) | 1) != 0) { klog("nvme: cannot create the I/O SQ\n"); return -1; }
    klog("nvme: %s, %u MiB\n", n->model, n->sectors / 2048);
    return 0;
}

static int rw(nvme_t* n, int write, uint32_t lba, uint32_t count) {
    uint32_t cmd[16];
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = write ? 0x01 : 0x02;
    cmd[1] = 1;                                   /* namespace 1 */
    uint64_t p1 = (uint64_t)(uintptr_t)n->bounce;
    cmd[6] = (uint32_t)p1;
    cmd[7] = (uint32_t)(p1 >> 32);
    uint32_t bytes = count * 512;
    if (bytes > PAGE) {
        /* two pages: the second one's address; more: the PRP list */
        uint64_t p2 = bytes > 2 * PAGE ? (uint64_t)(uintptr_t)n->prps : (uint64_t)(uintptr_t)(n->bounce + PAGE);
        cmd[8] = (uint32_t)p2;
        cmd[9] = (uint32_t)(p2 >> 32);
    }
    cmd[10] = lba;
    cmd[11] = 0;
    cmd[12] = count - 1;
    return submit(n, 1, cmd) == 0 ? 0 : -1;
}

int nvme_read(int idx, uint32_t lba, uint32_t count, void* buf) {
    if (idx < 0 || idx >= g_count) return -1;
    nvme_t* n = &g_nv[idx];
    uint8_t* out = (uint8_t*)buf;
    while (count) {
        uint32_t c = count > CHUNK / 512 ? CHUNK / 512 : count;
        if (rw(n, 0, lba, c) != 0) { klog("nvme: read error at %u\n", lba); return -1; }
        memcpy(out, n->bounce, c * 512);
        out += c * 512;
        lba += c;
        count -= c;
    }
    return 0;
}

int nvme_write(int idx, uint32_t lba, uint32_t count, const void* buf) {
    if (idx < 0 || idx >= g_count) return -1;
    nvme_t* n = &g_nv[idx];
    const uint8_t* in = (const uint8_t*)buf;
    while (count) {
        uint32_t c = count > CHUNK / 512 ? CHUNK / 512 : count;
        memcpy(n->bounce, in, c * 512);
        if (rw(n, 1, lba, c) != 0) { klog("nvme: write error at %u\n", lba); return -1; }
        in += c * 512;
        lba += c;
        count -= c;
    }
    return 0;
}

int nvme_flush(int idx) {
    if (idx < 0 || idx >= g_count) return -1;
    uint32_t cmd[16];
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = 0x00;
    cmd[1] = 1;
    return submit(&g_nv[idx], 1, cmd) == 0 ? 0 : -1;
}

int nvme_probe(ata_disk_t* out, int max) {
    int n = 0;
    for (int i = 0; i < g_count && n < max; i++) {
        ata_disk_t* d = &out[n++];
        memset(d, 0, sizeof(*d));
        d->present = 1;
        d->ahci_port = -1;
        d->nvme = i + 1;
        d->sectors = g_nv[i].sectors;
        kstrlcpy(d->model, g_nv[i].model, sizeof(d->model));
    }
    return n;
}

/* blockdev glue: FAT32 partitions on the SSD get mounted at /mnt/nvme */
static int bd_read(blockdev_t* bd, uint32_t lba, uint32_t count, void* buf) {
    return nvme_read((int)(uintptr_t)bd->priv, lba, count, buf);
}
static int bd_write(blockdev_t* bd, uint32_t lba, uint32_t count, const void* buf) {
    int i = (int)(uintptr_t)bd->priv;
    return nvme_write(i, lba, count, buf) == 0 ? nvme_flush(i) : -1;
}

static int scan_cb(const pci_dev_t* d, void* ctx) {
    (void)ctx;
    if (d->class_code != 0x01 || d->subclass != 0x08 || d->prog_if != 0x02 || g_count >= NVME_MAX) return 0;
    nvme_t* n = &g_nv[g_count];
    memset(n, 0, sizeof(*n));
    if (start_controller(n, d) == 0) {
        n->bd = blockdev_register_kind("nvme", n->model, n->sectors, 512, bd_read, bd_write, (void*)(uintptr_t)g_count);
        g_count++;
    }
    return 0;
}

void nvme_init(void) {
    pci_scan(scan_cb, NULL);
}
