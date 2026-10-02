#include "ahci.h"
#include "pci.h"
#include "kheap.h"
#include "kstring.h"
#include "timer.h"
#include "serial.h"

/*
 * AHCI 1.3: the HBA's registers are memory-mapped at BAR5 (ABAR); each
 * port has a command list (32 command headers), a received-FIS area and,
 * per command, a command table holding the command FIS, the ATAPI packet
 * and the scatter/gather list (PRDT). Memory is identity-mapped and the
 * heap lies below 4 GiB, so buffers are handed to the HBA as they are.
 */

#define MAX_HBA      4
#define MAX_PORTS    32
#define MAX_DEV      16
#define CMD_TIMEOUT  10000u          /* ms */
#define MAX_SECTORS  128u            /* per command (64 KiB) */

/* HBA registers */
#define HBA_CAP   0x00
#define HBA_GHC   0x04
#define HBA_IS    0x08
#define HBA_PI    0x0C
#define HBA_VS    0x10
#define HBA_CAP2  0x24
#define HBA_BOHC  0x28
#define GHC_AE    (1u << 31)

/* port registers (at 0x100 + port * 0x80) */
#define PX_CLB    0x00
#define PX_CLBU   0x04
#define PX_FB     0x08
#define PX_FBU    0x0C
#define PX_IS     0x10
#define PX_IE     0x14
#define PX_CMD    0x18
#define PX_TFD    0x20
#define PX_SIG    0x24
#define PX_SSTS   0x28
#define PX_SCTL   0x2C
#define PX_SERR   0x30
#define PX_CI     0x38

#define CMD_ST    (1u << 0)
#define CMD_SUD   (1u << 1)
#define CMD_POD   (1u << 2)
#define CMD_FRE   (1u << 4)
#define CMD_FR    (1u << 14)
#define CMD_CR    (1u << 15)

#define TFD_ERR   0x01
#define TFD_DRQ   0x08
#define TFD_BSY   0x80

#define SIG_ATA   0x00000101u
#define SIG_ATAPI 0xEB140101u

typedef struct __attribute__((packed)) {
    uint16_t flags;       /* CFL (FIS length in dwords) | A (ATAPI) bit 5 | W (write) bit 6 */
    uint16_t prdtl;       /* PRDT entries */
    uint32_t prdbc;       /* bytes transferred (written by the HBA) */
    uint32_t ctba, ctbau; /* command table address */
    uint32_t rsv[4];
} cmd_header_t;

typedef struct __attribute__((packed)) {
    uint32_t dba, dbau, rsv;
    uint32_t dbc;         /* byte count - 1, bit 31 = interrupt on completion */
} prd_t;

#define PRD_MAX 8
typedef struct __attribute__((packed)) {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t rsv[48];
    prd_t   prdt[PRD_MAX];
} cmd_table_t;

typedef struct {
    volatile uint8_t* abar;
    int      ok;
    uint8_t  bus, dev, fn;
} hba_t;

typedef struct {
    hba_t*        hba;
    int           port;
    int           atapi;
    cmd_header_t* clist;
    cmd_table_t*  table;
    void*         fis;
} port_t;

static hba_t  g_hba[MAX_HBA];
static int    g_nhba;
static port_t g_ports[MAX_DEV];
static int    g_nports;
static int    g_probed;
static char   g_status[512];

static inline uint32_t rd(volatile uint8_t* base, uint32_t off) { return *(volatile uint32_t*)(base + off); }
static inline void wr(volatile uint8_t* base, uint32_t off, uint32_t v) { *(volatile uint32_t*)(base + off) = v; }

static volatile uint8_t* port_regs(const port_t* p) { return p->hba->abar + 0x100 + (uint32_t)p->port * 0x80; }

/* aligned allocation (never freed: one set per port for the OS's lifetime) */
static void* alloc_aligned(uint32_t size, uint32_t align) {
    uint8_t* m = (uint8_t*)kmalloc(size + align);
    if (!m) return NULL;
    uintptr_t a = ((uintptr_t)m + align - 1) & ~(uintptr_t)(align - 1);
    memset((void*)a, 0, size);
    return (void*)a;
}

static int wait_clear(volatile uint8_t* r, uint32_t off, uint32_t bits, uint32_t ms) {
    uint32_t t0 = timer_ms();
    while (rd(r, off) & bits) {
        if ((uint32_t)(timer_ms() - t0) > ms) return -1;
    }
    return 0;
}

static void stop_port(volatile uint8_t* r) {
    wr(r, PX_CMD, rd(r, PX_CMD) & ~CMD_ST);
    wait_clear(r, PX_CMD, CMD_CR, 500);
    wr(r, PX_CMD, rd(r, PX_CMD) & ~CMD_FRE);
    wait_clear(r, PX_CMD, CMD_FR, 500);
}

static void start_port(volatile uint8_t* r) {
    wait_clear(r, PX_CMD, CMD_CR, 500);
    wr(r, PX_CMD, rd(r, PX_CMD) | CMD_FRE | CMD_SUD | CMD_POD);
    wr(r, PX_CMD, rd(r, PX_CMD) | CMD_ST);
}

static int setup_port(port_t* p) {
    volatile uint8_t* r = port_regs(p);
    stop_port(r);
    p->clist = (cmd_header_t*)alloc_aligned(32 * sizeof(cmd_header_t), 1024);
    p->fis = alloc_aligned(256, 256);
    p->table = (cmd_table_t*)alloc_aligned(sizeof(cmd_table_t), 128);
    if (!p->clist || !p->fis || !p->table) return -1;
    wr(r, PX_CLB, (uint32_t)(uintptr_t)p->clist);
    wr(r, PX_CLBU, 0);
    wr(r, PX_FB, (uint32_t)(uintptr_t)p->fis);
    wr(r, PX_FBU, 0);
    p->clist[0].ctba = (uint32_t)(uintptr_t)p->table;
    p->clist[0].ctbau = 0;
    wr(r, PX_SERR, 0xFFFFFFFFu);
    wr(r, PX_IS, 0xFFFFFFFFu);
    wr(r, PX_IE, 0);                  /* polled */
    start_port(r);
    return 0;
}

/*
 * One command in slot 0. fis_cmd: the ATA command; packet: an ATAPI
 * command (12 bytes) or NULL; buf/bytes: the data (bytes may be 0).
 */
static int exec(port_t* p, uint8_t fis_cmd, uint64_t lba, uint16_t count, const uint8_t* packet,
                void* buf, uint32_t bytes, int write) {
    volatile uint8_t* r = port_regs(p);
    if (wait_clear(r, PX_TFD, TFD_BSY | TFD_DRQ, 1000) != 0) {
        /* a stuck device: restart the port once */
        stop_port(r);
        wr(r, PX_SERR, 0xFFFFFFFFu);
        start_port(r);
        if (wait_clear(r, PX_TFD, TFD_BSY | TFD_DRQ, 1000) != 0) return -1;
    }
    cmd_header_t* h = &p->clist[0];
    cmd_table_t* t = p->table;
    memset(t, 0, sizeof(*t));

    /* scatter/gather: the buffer is physically contiguous; 4 MiB per entry */
    uint32_t n = 0, off = 0;
    while (off < bytes && n < PRD_MAX) {
        uint32_t chunk = bytes - off;
        if (chunk > (4u << 20)) chunk = 4u << 20;
        t->prdt[n].dba = (uint32_t)((uintptr_t)buf + off);
        t->prdt[n].dbau = 0;
        t->prdt[n].dbc = chunk - 1;
        off += chunk;
        n++;
    }
    if (off < bytes) return -1;

    uint8_t* f = t->cfis;
    f[0] = 0x27;                      /* FIS: register host-to-device */
    f[1] = 0x80;                      /* command */
    f[2] = fis_cmd;
    f[4] = (uint8_t)lba;
    f[5] = (uint8_t)(lba >> 8);
    f[6] = (uint8_t)(lba >> 16);
    f[7] = 0x40;                      /* LBA mode */
    f[8] = (uint8_t)(lba >> 24);
    f[9] = (uint8_t)(lba >> 32);
    f[10] = (uint8_t)(lba >> 40);
    f[12] = (uint8_t)count;
    f[13] = (uint8_t)(count >> 8);
    if (packet) {
        f[3] = 1;                     /* features: DMA */
        f[5] = (uint8_t)bytes;        /* byte count limit (PIO fallback) */
        f[6] = (uint8_t)(bytes >> 8);
        memcpy(t->acmd, packet, 12);
    }
    h->flags = (uint16_t)(5 | (packet ? 1u << 5 : 0) | (write ? 1u << 6 : 0));
    h->prdtl = (uint16_t)n;
    h->prdbc = 0;

    wr(r, PX_IS, 0xFFFFFFFFu);
    wr(r, PX_CI, 1);
    uint32_t t0 = timer_ms();
    for (;;) {
        if (!(rd(r, PX_CI) & 1)) break;
        if (rd(r, PX_IS) & (1u << 30)) break;     /* task file error */
        if ((uint32_t)(timer_ms() - t0) > CMD_TIMEOUT) {
            klog("ahci: port %d: command %x timed out\n", p->port, fis_cmd);
            stop_port(r);
            start_port(r);
            return -1;
        }
    }
    if (rd(r, PX_TFD) & TFD_ERR || rd(r, PX_IS) & (1u << 30)) {
        klog("ahci: port %d: command %x failed (tfd %x)\n", p->port, fis_cmd, rd(r, PX_TFD));
        stop_port(r);
        wr(r, PX_SERR, 0xFFFFFFFFu);
        wr(r, PX_IS, 0xFFFFFFFFu);
        start_port(r);
        return -1;
    }
    return 0;
}

static void ident_string(const uint16_t* id, int first, int words, char* out) {
    for (int i = 0; i < words; i++) {
        out[i * 2] = (char)(id[first + i] >> 8);
        out[i * 2 + 1] = (char)id[first + i];
    }
    out[words * 2] = 0;
    for (int i = words * 2 - 1; i >= 0 && out[i] == ' '; i--) out[i] = 0;
    int s = 0;
    while (out[s] == ' ') s++;
    if (s) memmove(out, out + s, strlen(out + s) + 1);
}

/* BIOS -> OS ownership handoff (AHCI 1.2+), when the BIOS supports it */
static void handoff(volatile uint8_t* abar) {
    if (!(rd(abar, HBA_CAP2) & 1)) return;
    wr(abar, HBA_BOHC, rd(abar, HBA_BOHC) | (1u << 1));          /* OOS */
    uint32_t t0 = timer_ms();
    while ((rd(abar, HBA_BOHC) & 1) && (uint32_t)(timer_ms() - t0) < 1000) { }   /* BOS clears */
    t0 = timer_ms();
    while ((rd(abar, HBA_BOHC) & (1u << 4)) && (uint32_t)(timer_ms() - t0) < 2000) { }   /* BB */
}

static int find_hba(const pci_dev_t* d, void* ctx) {
    (void)ctx;
    if (d->class_code != 0x01 || d->subclass != 0x06 || d->prog_if != 0x01) return 0;
    if (g_nhba >= MAX_HBA) return 1;
    int io;
    uint32_t bar = pci_bar(d, 5, &io);
    if (!bar || io) return 0;
    pci_enable(d);
    hba_t* h = &g_hba[g_nhba++];
    h->abar = (volatile uint8_t*)(uintptr_t)bar;
    h->bus = d->bus;
    h->dev = d->dev;
    h->fn = d->fn;
    return 0;
}

static void init_all(void) {
    g_probed = 1;
    pci_scan(find_hba, NULL);
    char* st = g_status;
    st[0] = 0;
    for (int i = 0; i < g_nhba; i++) {
        hba_t* h = &g_hba[i];
        volatile uint8_t* abar = h->abar;
        handoff(abar);
        wr(abar, HBA_GHC, rd(abar, HBA_GHC) | GHC_AE);
        uint32_t pi = rd(abar, HBA_PI), vs = rd(abar, HBA_VS);
        h->ok = 1;
        char line[96];
        ksnprintf(line, sizeof(line), "AHCI %u.%u at %02x:%02x.%u, ports %x\n",
                  vs >> 16, (vs >> 8) & 0xFF, h->bus, h->dev, h->fn, pi);
        kstrlcat(g_status, line, sizeof(g_status));
        for (int pn = 0; pn < MAX_PORTS && g_nports < MAX_DEV; pn++) {
            if (!(pi & (1u << pn))) continue;
            volatile uint8_t* r = abar + 0x100 + (uint32_t)pn * 0x80;
            uint32_t ssts = rd(r, PX_SSTS);
            if ((ssts & 0xF) != 3) {
                /* not up yet: spin it up / bring the link up once */
                wr(r, PX_CMD, rd(r, PX_CMD) | CMD_SUD | CMD_POD);
                uint32_t t0 = timer_ms();
                while ((rd(r, PX_SSTS) & 0xF) != 3 && (uint32_t)(timer_ms() - t0) < 50) { }
                ssts = rd(r, PX_SSTS);
                if ((ssts & 0xF) != 3) continue;
            }
            /* the signature arrives with the device's first D2H FIS */
            uint32_t t0 = timer_ms();
            while ((rd(r, PX_TFD) & TFD_BSY) && (uint32_t)(timer_ms() - t0) < 3000) { }
            uint32_t sig = rd(r, PX_SIG);
            if (sig != SIG_ATA && sig != SIG_ATAPI) continue;
            port_t* p = &g_ports[g_nports];
            p->hba = h;
            p->port = pn;
            p->atapi = sig == SIG_ATAPI;
            if (setup_port(p) != 0) continue;
            g_nports++;
        }
    }
}

int ahci_probe(ata_disk_t* out, int max) {
    if (!g_probed) init_all();
    int n = 0;
    static uint16_t id[256] __attribute__((aligned(16)));
    for (int i = 0; i < g_nports && n < max; i++) {
        port_t* p = &g_ports[i];
        ata_disk_t* d = &out[n];
        memset(d, 0, sizeof(*d));
        d->ahci_port = i;
        d->bus = -1;
        memset(id, 0, sizeof(id));
        if (exec(p, p->atapi ? 0xA1 : 0xEC, 0, 0, NULL, id, 512, 0) != 0) continue;
        d->present = 1;
        d->is_atapi = p->atapi;
        ident_string(id, 27, 20, d->model);
        if (!p->atapi) {
            uint64_t s48 = (uint64_t)id[100] | (uint64_t)id[101] << 16 | (uint64_t)id[102] << 32 | (uint64_t)id[103] << 48;
            uint32_t s28 = (uint32_t)id[61] << 16 | id[60];
            uint64_t s = (id[83] & (1u << 10)) && s48 ? s48 : s28;
            d->sectors = s > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)s;   /* 2 TiB is plenty here */
        }
        n++;
    }
    return n;
}

static port_t* get(int port, int atapi) {
    if (!g_probed) init_all();
    if (port < 0 || port >= g_nports || g_ports[port].atapi != atapi) return NULL;
    return &g_ports[port];
}

static int rw(int port, uint32_t lba, uint32_t count, void* buf, int write) {
    port_t* p = get(port, 0);
    if (!p) return -1;
    uint8_t* b = (uint8_t*)buf;
    while (count) {
        uint32_t n = count > MAX_SECTORS ? MAX_SECTORS : count;
        if (exec(p, write ? 0x35 : 0x25, lba, (uint16_t)n, NULL, b, n * 512, write) != 0) return -1;
        lba += n;
        count -= n;
        b += n * 512;
    }
    return 0;
}

int ahci_read(int port, uint32_t lba, uint32_t count, void* buf) { return rw(port, lba, count, buf, 0); }
int ahci_write(int port, uint32_t lba, uint32_t count, const void* buf) { return rw(port, lba, count, (void*)buf, 1); }

int ahci_flush(int port) {
    port_t* p = get(port, 0);
    return p ? exec(p, 0xEA, 0, 0, NULL, NULL, 0, 0) : -1;
}

int ahci_atapi_read(int port, uint32_t lba, uint32_t count, void* buf) {
    port_t* p = get(port, 1);
    if (!p) return -1;
    uint8_t* b = (uint8_t*)buf;
    while (count) {
        uint32_t n = count > 32 ? 32 : count;
        uint8_t pkt[12] = { 0xA8, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
                            (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n, 0, 0 };   /* READ(12) */
        int ok = -1;
        for (int tries = 0; tries < 3 && ok != 0; tries++)          /* the first command may report a media change */
            ok = exec(p, 0xA0, 0, 0, pkt, b, n * 2048, 0);
        if (ok != 0) return -1;
        lba += n;
        count -= n;
        b += n * 2048;
    }
    return 0;
}

const char* ahci_status(void) {
    if (!g_probed) init_all();
    return g_nhba ? g_status : "no AHCI controller\n";
}
