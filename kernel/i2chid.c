/*
 * HID over I2C touchpads (Microsoft's "HID over I2C protocol" 1.0) on the
 * Intel LPSS DesignWare I2C controllers (Skylake and later laptops), by
 * polling.
 *
 *  - the ACPI tables say whether there is an I2C HID device at all
 *    (PNP0C50 / ACPI0C50) and at which I2C addresses devices sit
 *    (I2cSerialBus resource descriptors) - nothing is probed otherwise;
 *  - the controllers are the PCI functions of class 0C80 whose DesignWare
 *    component type reads "DW" 0x0140 (taken out of reset, powered on);
 *  - at each address the HID descriptor is looked for at register 0x20
 *    (Synaptics, Cirque...) and 0x01 (ELAN, ALPS...), and checked;
 *  - a precision touchpad is switched to touchpad mode (feature Input
 *    Mode = 3) and its fingers go to the gesture engine (touchpad.c);
 *    otherwise its mouse reports are used.
 *
 * References: Microsoft "HID over I2C Protocol Specification" 1.0,
 * Microsoft "Precision Touchpad Required HID Top-Level Collections",
 * Synopsys DesignWare DW_apb_i2c databook register map (as also used by
 * Linux drivers/i2c/busses/i2c-designware-*), Intel LPSS private registers
 * (Linux drivers/mfd/intel-lpss.c). Independent implementation.
 */
#include "i2chid.h"
#include "hidparse.h"
#include "touchpad.h"
#include "usb.h"
#include "pci.h"
#include "smp.h"
#include "kstring.h"
#include "kheap.h"
#include "timer.h"
#include "serial.h"

/* ── ACPI: is there an I2C HID device, and at which addresses ───────── */

static int      g_acpi_hid;          /* PNP0C50 / ACPI0C50 named somewhere */
static uint8_t  g_addrs[24];
static int      g_naddr;
static uint32_t g_speed;             /* the fastest bus speed asked for */

static uint32_t rd32le(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void add_addr(uint8_t a) {
    for (int i = 0; i < g_naddr; i++) if (g_addrs[i] == a) return;
    if (g_naddr < (int)sizeof(g_addrs)) g_addrs[g_naddr++] = a;
}

static void scan_aml(const uint8_t* a, uint32_t n) {
    static const uint8_t EISA_PNP0C50[5] = { 0x0C, 0x41, 0xD0, 0x0C, 0x50 };   /* DWordConst EisaId("PNP0C50") */
    for (uint32_t i = 0; i + 20 < n; i++) {
        if ((a[i] == 'P' && memcmp(a + i, "PNP0C50", 7) == 0) || (a[i] == 'A' && memcmp(a + i, "ACPI0C50", 8) == 0) ||
            (a[i] == 0x0C && memcmp(a + i, EISA_PNP0C50, 5) == 0))
            g_acpi_hid = 1;
        /* I2cSerialBus(V2): large item 0x8E, serial bus type 1 */
        if (a[i] != 0x8E) continue;
        uint32_t len = a[i + 1] | (uint32_t)a[i + 2] << 8;
        if (len < 15 || len > 255 || i + 3 + len > n || a[i + 5] != 1) continue;
        uint32_t tdlen = a[i + 10] | (uint32_t)a[i + 11] << 8;
        if (tdlen < 6 || tdlen > len) continue;
        uint32_t speed = rd32le(a + i + 12);
        uint32_t addr = a[i + 16] | (uint32_t)a[i + 17] << 8;
        if ((a[i + 7] & 1) || speed < 10000 || speed > 3400000) continue;     /* (10-bit addresses: no) */
        if (addr >= 0x08 && addr < 0x78) add_addr((uint8_t)addr);
        if (speed > g_speed) g_speed = speed;
    }
}

static void scan_acpi(void) {
    const uint8_t* f = (const uint8_t*)acpi_find_table("FACP");
    if (f) {
        uint32_t flen = rd32le(f + 4);
        uint64_t dsdt_a = rd32le(f + 40);
        if (flen >= 148) {
            uint64_t x = (uint64_t)rd32le(f + 140) | (uint64_t)rd32le(f + 144) << 32;
            if (x) dsdt_a = x;
        }
        const uint8_t* d = (const uint8_t*)acpi_table_at(dsdt_a, "DSDT");
        if (d) scan_aml(d + 36, rd32le(d + 4) - 36);
    }
    for (int k = 0; k < 64; k++) {
        const uint8_t* t = (const uint8_t*)acpi_find_table_n("SSDT", k);
        if (!t) break;
        scan_aml(t + 36, rd32le(t + 4) - 36);
    }
}

/* ── DesignWare I2C controllers ──────────────────────────────────────── */

#define IC_CON           0x00
#define IC_TAR           0x04
#define IC_DATA_CMD      0x10
#define IC_SS_SCL_HCNT   0x14
#define IC_SS_SCL_LCNT   0x18
#define IC_FS_SCL_HCNT   0x1C
#define IC_FS_SCL_LCNT   0x20
#define IC_INTR_MASK     0x30
#define IC_RAW_INTR_STAT 0x34
#define IC_RX_TL         0x38
#define IC_TX_TL         0x3C
#define IC_CLR_INTR      0x40
#define IC_CLR_TX_ABRT   0x54
#define IC_CLR_STOP_DET  0x60
#define IC_ENABLE        0x6C
#define IC_STATUS        0x70
#define IC_TXFLR         0x74
#define IC_RXFLR         0x78
#define IC_SDA_HOLD      0x7C
#define IC_ENABLE_STATUS 0x9C
#define IC_COMP_PARAM_1  0xF4
#define IC_COMP_TYPE     0xFC
#define DW_COMP_TYPE     0x44570140u

#define INTR_TX_ABRT     (1u << 6)
#define INTR_STOP_DET    (1u << 9)
#define ST_MST_ACTIVITY  (1u << 5)
#define CMD_READ         (1u << 8)
#define CMD_STOP         (1u << 9)
#define CMD_RESTART      (1u << 10)

#define LPSS_PRIV        0x200       /* Intel LPSS private registers */

typedef struct {
    volatile uint8_t* base;
    int clk_mhz;
    int txd, rxd;                    /* FIFO depths */
    uint16_t pci_dev;
} dw_t;

static dw_t g_dw[8];
static int  g_ndw;

static uint32_t dr(dw_t* c, uint32_t r) { return *(volatile uint32_t*)(c->base + r); }
static void     dwr(dw_t* c, uint32_t r, uint32_t v) { *(volatile uint32_t*)(c->base + r) = v; }

static void mdelay(uint32_t ms) {
    uint32_t t = timer_ms();
    while (timer_ms() - t < ms) __asm__ volatile("pause");
}

static int dw_disable(dw_t* c) {
    dwr(c, IC_ENABLE, 0);
    uint32_t t = timer_ms();
    while (dr(c, IC_ENABLE_STATUS) & 1) {
        if (timer_ms() - t > 20) return -1;
        __asm__ volatile("pause");
    }
    return 0;
}

/* master, 7-bit addresses, restarts allowed; fast mode (400 kHz) or standard (100 kHz) */
static void dw_setup(dw_t* c, int fast) {
    dw_disable(c);
    int m = c->clk_mhz;
    /* SCL high / low times with room for the rise and fall: <= 400 / 100 kHz */
    dwr(c, IC_SS_SCL_HCNT, (uint32_t)(m * 45 / 10));
    dwr(c, IC_SS_SCL_LCNT, (uint32_t)(m * 52 / 10));
    dwr(c, IC_FS_SCL_HCNT, (uint32_t)(m * 9 / 10));
    dwr(c, IC_FS_SCL_LCNT, (uint32_t)(m * 16 / 10));
    uint32_t hold = dr(c, IC_SDA_HOLD);
    if (!(hold & 0xFFFF)) dwr(c, IC_SDA_HOLD, (hold & 0xFFFF0000u) | (uint32_t)(m * 3 / 10));   /* 300 ns */
    dwr(c, IC_CON, 0x01 | (fast ? 2u : 1u) << 1 | 0x20 | 0x40);
    dwr(c, IC_TX_TL, 0);
    dwr(c, IC_RX_TL, 0);
    dwr(c, IC_INTR_MASK, 0);
}

/* writes w (wl bytes), then (repeated start) reads rl bytes; 0, or -1
 * (no answer: nobody at that address, or it refused) */
static int dw_xfer(dw_t* c, uint8_t addr, const uint8_t* w, int wl, uint8_t* r, int rl) {
    if (dw_disable(c) < 0) return -1;
    dwr(c, IC_TAR, addr);
    (void)dr(c, IC_CLR_INTR);
    dwr(c, IC_ENABLE, 1);
    int wi = 0, rq = 0, rg = 0, rc = 0;
    uint32_t t0 = timer_ms(), limit = 30 + (uint32_t)(wl + rl) / 8;
    while (wi < wl || rq < rl || rg < rl) {
        uint32_t raw = dr(c, IC_RAW_INTR_STAT);
        if (raw & INTR_TX_ABRT) { rc = -1; break; }
        while ((int)dr(c, IC_TXFLR) < c->txd && (wi < wl || rq < rl) && rq - rg < c->rxd - 1) {
            uint32_t cmd;
            if (wi < wl) {
                cmd = w[wi];
                if (wi == wl - 1 && rl == 0) cmd |= CMD_STOP;
                wi++;
            } else {
                cmd = CMD_READ;
                if (rq == 0 && wl > 0) cmd |= CMD_RESTART;
                if (rq == rl - 1) cmd |= CMD_STOP;
                rq++;
            }
            dwr(c, IC_DATA_CMD, cmd);
        }
        while (rg < rl && dr(c, IC_RXFLR) > 0) r[rg++] = (uint8_t)dr(c, IC_DATA_CMD);
        if (timer_ms() - t0 > limit) { rc = -1; break; }
    }
    /* the end: STOP sent (a NACK on the last write byte shows only now) */
    while (rc == 0) {
        uint32_t raw = dr(c, IC_RAW_INTR_STAT);
        if (raw & INTR_TX_ABRT) { rc = -1; break; }
        if ((raw & INTR_STOP_DET) && !(dr(c, IC_STATUS) & ST_MST_ACTIVITY)) break;
        if (timer_ms() - t0 > limit + 10) { rc = -1; break; }
    }
    (void)dr(c, IC_CLR_TX_ABRT);
    (void)dr(c, IC_CLR_STOP_DET);
    dw_disable(c);
    return rc;
}

/* the LPSS I2C functions and their input clock (MHz); 0 = not an I2C one we know */
static int lpss_i2c_clock(uint16_t id) {
    static const struct { uint16_t lo, hi; int mhz; } T[] = {
        { 0x9d60, 0x9d65, 120 }, { 0xa160, 0xa163, 120 }, { 0xa2e0, 0xa2e3, 120 },      /* Skylake, Kaby Lake */
        { 0x5aac, 0x5aba, 133 }, { 0x31ac, 0x31ba, 133 }, { 0x0aac, 0x0aba, 133 },      /* Apollo / Gemini Lake, Broxton */
        { 0x1aac, 0x1aba, 133 },
        { 0x4b78, 0x4b7b, 100 }, { 0x4b4b, 0x4b4c, 100 },                               /* Elkhart Lake */
        { 0x9de8, 0x9deb, 216 }, { 0x9dc5, 0x9dc6, 216 }, { 0xa368, 0xa36b, 216 },      /* Cannon Lake */
        { 0x02e8, 0x02eb, 216 }, { 0x02c5, 0x02c6, 216 }, { 0x06e8, 0x06eb, 216 },      /* Comet Lake */
        { 0xa3e0, 0xa3e3, 216 },
        { 0x34e8, 0x34eb, 216 }, { 0x34c5, 0x34c6, 216 },                               /* Ice Lake */
        { 0xa0e8, 0xa0eb, 216 }, { 0xa0c5, 0xa0c6, 216 }, { 0xa0d8, 0xa0d9, 216 },      /* Tiger Lake */
        { 0x43e8, 0x43eb, 216 }, { 0x43d8, 0x43d8, 216 }, { 0x43ad, 0x43ae, 216 },
        { 0x4de8, 0x4deb, 216 }, { 0x4dc5, 0x4dc6, 216 },                               /* Jasper Lake */
        { 0x51e8, 0x51eb, 216 }, { 0x51c5, 0x51c6, 216 }, { 0x51d8, 0x51d9, 216 },      /* Alder Lake */
        { 0x7acc, 0x7acf, 216 }, { 0x7afc, 0x7afd, 216 }, { 0x54e8, 0x54eb, 216 },
        { 0x54c5, 0x54c6, 216 },
        { 0x7a4c, 0x7a4f, 216 }, { 0x7a7c, 0x7a7d, 216 },                               /* Raptor Lake */
        { 0x7e50, 0x7e51, 216 }, { 0x7e78, 0x7e7b, 216 },                               /* Meteor Lake */
        { 0xa878, 0xa87b, 216 }, { 0xa850, 0xa851, 216 },                               /* Lunar Lake */
    };
    for (unsigned i = 0; i < sizeof(T) / sizeof(T[0]); i++)
        if (id >= T[i].lo && id <= T[i].hi) return T[i].mhz;
    return 0;
}

static void pci_power_on(const pci_dev_t* d) {
    if (!(pci_read16(d->bus, d->dev, d->fn, 0x06) & 0x10)) return;      /* no capability list */
    uint8_t p = (uint8_t)(pci_read32(d->bus, d->dev, d->fn, 0x34) & 0xFC);
    for (int guard = 0; p && guard < 48; guard++) {
        uint32_t v = pci_read32(d->bus, d->dev, d->fn, p);
        if ((v & 0xFF) == 0x01) {                                       /* power management */
            uint16_t pmcsr = pci_read16(d->bus, d->dev, d->fn, (uint8_t)(p + 4));
            if (pmcsr & 3) {
                pci_write16(d->bus, d->dev, d->fn, (uint8_t)(p + 4), (uint16_t)(pmcsr & ~3u));
                mdelay(10);
            }
            return;
        }
        p = (uint8_t)((v >> 8) & 0xFC);
    }
}

static int find_dw(const pci_dev_t* d, void* ctx) {
    (void)ctx;
    if (d->vendor != 0x8086 || d->class_code != 0x0C || d->subclass != 0x80 || g_ndw == 8) return 0;
    int mhz = lpss_i2c_clock(d->device);
    int is_io = 0;
    uintptr_t bar = pci_bar(d, 0, &is_io);
    if (!bar || is_io) return 0;
    volatile uint8_t* b = (volatile uint8_t*)bar;
    if (mhz) {
        /* a known I2C function: power, take it out of reset, clock on */
        pci_enable(d);
        pci_power_on(d);
        *(volatile uint32_t*)(b + LPSS_PRIV + 0x04) = 0x7;
        *(volatile uint32_t*)(b + LPSS_PRIV) |= 1u;
        mdelay(1);
    } else {
        mhz = 216;                       /* unknown: only if it already answers; the fastest clock = the slowest bus */
    }
    if (*(volatile uint32_t*)(b + IC_COMP_TYPE) != DW_COMP_TYPE) return 0;
    if (!(pci_read16(d->bus, d->dev, d->fn, 0x04) & 2)) pci_enable(d);
    dw_t* c = &g_dw[g_ndw++];
    c->base = b;
    c->clk_mhz = mhz;
    c->pci_dev = d->device;
    uint32_t p1 = *(volatile uint32_t*)(b + IC_COMP_PARAM_1);
    c->txd = (int)((p1 >> 16) & 0xFF) + 1;
    c->rxd = (int)((p1 >> 8) & 0xFF) + 1;
    if (p1 == 0 || p1 == 0xFFFFFFFFu || c->txd < 2 || c->rxd < 2) c->txd = c->rxd = 16;
    klog("i2c: DesignWare controller %04x at %p, %d MHz, FIFO %d/%d\n", d->device, (void*)bar, mhz, c->txd, c->rxd);
    return 0;
}

#ifdef I2CHID_HOST_TEST              /* (a simulated touchpad instead of the bus: host-side tests) */
int i2chid_test_xfer(uint8_t addr, const uint8_t* w, int wl, uint8_t* r, int rl);
#define bus_xfer(c, a, w, wl, r, rl) i2chid_test_xfer(a, w, wl, r, rl)
#else
#define bus_xfer dw_xfer
#endif

/* ── HID over I2C devices ────────────────────────────────────────────── */

#define MAX_FINGERS 10
#define NSLOTS 16

typedef struct {
    dw_t*    bus;
    uint8_t  addr;
    uint16_t reg_input, max_input, reg_cmd, reg_data, vid, pid;
    hid_field_t* f;
    int      nf, ids;
    uint8_t* buf;
    uint32_t last_poll;
    int      ptp;                     /* in touchpad mode */
    /* touchpad mode: the report and its fields (indexes into f, -1: none) */
    int      tp_rid, nfing;
    int      tip[MAX_FINGERS], conf[MAX_FINGERS], cid[MAX_FINGERS], fx[MAX_FINGERS], fy[MAX_FINGERS];
    int      count, btn[3];
    struct { int down, x, y; } slot[NSLOTS];
    int      primary;
    touchpad_t tp;
    /* mouse mode */
    int      m_rid, mb[3], mx, my, mw;
} ihid_t;

static ihid_t* g_dev[4];
static int     g_ndev;

static int find_field(ihid_t* h, uint32_t app, uint32_t usage, int finger, uint8_t type, int rid) {
    for (int i = 0; i < h->nf; i++) {
        hid_field_t* f = &h->f[i];
        if (f->type == type && f->usage == usage && (!app || f->app == app) && (finger == -2 || f->finger == finger) &&
            (rid < 0 || f->rid == rid) && !(f->flags & 1))
            return i;
    }
    return -1;
}

static int read_reg(ihid_t* h, uint16_t reg, uint8_t* out, int n) {
    uint8_t w[2] = { (uint8_t)reg, (uint8_t)(reg >> 8) };
    return bus_xfer(h->bus, h->addr, w, 2, out, n);
}

static int command(ihid_t* h, uint8_t lo, uint8_t opcode) {
    uint8_t w[4] = { (uint8_t)h->reg_cmd, (uint8_t)(h->reg_cmd >> 8), lo, opcode };
    return bus_xfer(h->bus, h->addr, w, 4, NULL, 0);
}

/* SET_REPORT(feature, rid) with the report's bytes (without the ID) */
static int set_feature(ihid_t* h, uint8_t rid, const uint8_t* data, int n) {
    uint8_t w[80];
    int k = 0;
    if (n > 64) return -1;
    w[k++] = (uint8_t)h->reg_cmd; w[k++] = (uint8_t)(h->reg_cmd >> 8);
    w[k++] = (uint8_t)(0x30 | (rid < 15 ? rid : 15));
    w[k++] = 0x03;                                    /* SET_REPORT */
    if (rid >= 15) w[k++] = rid;
    w[k++] = (uint8_t)h->reg_data; w[k++] = (uint8_t)(h->reg_data >> 8);
    int len = 2 + (rid ? 1 : 0) + n;
    w[k++] = (uint8_t)len; w[k++] = (uint8_t)(len >> 8);
    if (rid) w[k++] = rid;
    for (int i = 0; i < n; i++) w[k++] = data[i];
    return bus_xfer(h->bus, h->addr, w, k, NULL, 0);
}

/* precision touchpad: its fields, and touchpad mode on */
static int setup_ptp(ihid_t* h) {
    const uint32_t TP = HID_U(0x0D, 0x05);
    int x0 = find_field(h, TP, HID_U(0x01, 0x30), 0, HID_INPUT, -1);
    if (x0 < 0) return 0;
    int rid = h->f[x0].rid;
    h->tp_rid = rid;
    h->nfing = 0;
    for (int k = 0; k < MAX_FINGERS; k++) {
        h->fx[k] = find_field(h, TP, HID_U(0x01, 0x30), k, HID_INPUT, rid);
        h->fy[k] = find_field(h, TP, HID_U(0x01, 0x31), k, HID_INPUT, rid);
        h->tip[k] = find_field(h, TP, HID_U(0x0D, 0x42), k, HID_INPUT, rid);
        h->conf[k] = find_field(h, TP, HID_U(0x0D, 0x47), k, HID_INPUT, rid);
        h->cid[k] = find_field(h, TP, HID_U(0x0D, 0x51), k, HID_INPUT, rid);
        if (h->fx[k] < 0 || h->fy[k] < 0 || h->tip[k] < 0) break;
        h->nfing = k + 1;
    }
    if (!h->nfing) return 0;
    h->count = find_field(h, TP, HID_U(0x0D, 0x54), -1, HID_INPUT, rid);
    for (int b = 0; b < 3; b++) h->btn[b] = find_field(h, TP, HID_U(0x09, b + 1), -1, HID_INPUT, rid);

    /* Input Mode (feature, Device Configuration collection) = 3: touchpad */
    int im = find_field(h, HID_U(0x0D, 0x0E), HID_U(0x0D, 0x52), -2, HID_FEATURE, -1);
    if (im < 0) im = find_field(h, 0, HID_U(0x0D, 0x52), -2, HID_FEATURE, -1);
    if (im < 0) return 0;
    uint8_t rep[64];
    int n = hid_report_len(h->f, h->nf, h->f[im].rid, HID_FEATURE);
    if (n <= 0 || n > 64) return 0;
    memset(rep, 0, sizeof(rep));
    hid_set_value(rep, n, &h->f[im], 3);
    if (set_feature(h, h->f[im].rid, rep, n) < 0) return 0;
    /* surface and button reporting on (when the device has the switches) */
    int sw = find_field(h, 0, HID_U(0x0D, 0x57), -2, HID_FEATURE, -1);
    int bw = find_field(h, 0, HID_U(0x0D, 0x58), -2, HID_FEATURE, -1);
    if (sw >= 0) {
        n = hid_report_len(h->f, h->nf, h->f[sw].rid, HID_FEATURE);
        if (n > 0 && n <= 64) {
            memset(rep, 0, sizeof(rep));
            hid_set_value(rep, n, &h->f[sw], 1);
            if (bw >= 0 && h->f[bw].rid == h->f[sw].rid) hid_set_value(rep, n, &h->f[bw], 1);
            set_feature(h, h->f[sw].rid, rep, n);
        }
    }

    /* the pad's size */
    hid_field_t* fx = &h->f[h->fx[0]];
    hid_field_t* fy = &h->f[h->fy[0]];
    touchpad_t* t = &h->tp;
    t->xmin = fx->lmin; t->xmax = fx->lmax;
    t->ymin = fy->lmin; t->ymax = fy->lmax;
    int span = fx->lmax - fx->lmin;
    t->upm = fx->mm10 > 0 ? span * 10 / fx->mm10 : span / 100;
    if (t->upm < 1) t->upm = 1;
    t->clickpad = h->btn[1] < 0;
    h->primary = -1;
    return 1;
}

static void setup_mouse(ihid_t* h) {
    const uint32_t M = HID_U(0x01, 0x02);
    h->mx = find_field(h, M, HID_U(0x01, 0x30), -1, HID_INPUT, -1);
    h->m_rid = h->mx >= 0 ? h->f[h->mx].rid : 0;
    h->my = find_field(h, M, HID_U(0x01, 0x31), -1, HID_INPUT, h->m_rid);
    h->mw = find_field(h, M, HID_U(0x01, 0x38), -1, HID_INPUT, h->m_rid);
    for (int b = 0; b < 3; b++) h->mb[b] = find_field(h, M, HID_U(0x09, b + 1), -1, HID_INPUT, h->m_rid);
}

static int probe(dw_t* bus, uint8_t addr, uint16_t desc_reg) {
    ihid_t tmp, *h = &tmp;
    memset(h, 0, sizeof(*h));
    h->bus = bus;
    h->addr = addr;
    uint8_t d[30];
    if (read_reg(h, desc_reg, d, 30) < 0) return -1;              /* nobody there */
    uint16_t dlen = d[0] | d[1] << 8, ver = d[2] | d[3] << 8;
    uint16_t rlen = d[4] | d[5] << 8, rreg = d[6] | d[7] << 8;
    h->reg_input = d[8] | d[9] << 8;
    h->max_input = d[10] | d[11] << 8;
    h->reg_cmd = d[16] | d[17] << 8;
    h->reg_data = d[18] | d[19] << 8;
    h->vid = d[20] | d[21] << 8;
    h->pid = d[22] | d[23] << 8;
    if (dlen != 30 || ver != 0x0100 || !rlen || rlen > 4096 || !h->reg_cmd || !h->reg_data ||
        h->max_input < 3 || h->max_input > 1024)
        return 0;                                                  /* something, but not HID */

    command(h, 0x00, 0x08);                                        /* SET_POWER: on */
    mdelay(2);
    command(h, 0x00, 0x01);                                        /* RESET */
    mdelay(100);
    uint8_t* buf = (uint8_t*)kmalloc((uint32_t)h->max_input + (uint32_t)rlen);
    if (!buf) return 0;
    bus_xfer(bus, addr, NULL, 0, buf, h->max_input);                /* the reset's answer */
    uint8_t* rd = buf + h->max_input;
    if (read_reg(h, rreg, rd, rlen) < 0) { kfree(buf); return 0; }
    hid_field_t* f = (hid_field_t*)kmalloc(sizeof(hid_field_t) * 400);
    int nf = f ? hid_parse(rd, rlen, f, 400) : -1;
    if (nf <= 0) { kfree(buf); kfree(f); return 0; }
    h->f = f;
    h->nf = nf;
    for (int i = 0; i < nf; i++) if (f[i].rid) h->ids = 1;
    h->buf = buf;

    int is_tp = 0, is_mouse = 0;
    for (int i = 0; i < nf; i++) {
        if (f[i].app == HID_U(0x0D, 0x05)) is_tp = 1;
        if (f[i].app == HID_U(0x01, 0x02)) is_mouse = 1;
    }
    klog("i2c-hid: %04x:%04x at %02x (descriptor at %04x): %d fields%s%s\n", h->vid, h->pid, addr, desc_reg, nf,
         is_tp ? ", touchpad" : "", is_mouse ? ", mouse" : "");
    if (!is_tp && !is_mouse) {                                     /* a touchscreen, a keyboard...: not ours */
        command(h, 0x01, 0x08);                                    /* (back to sleep) */
        kfree(buf); kfree(f);
        return 0;
    }
    if (is_tp) h->ptp = setup_ptp(h);
    if (!h->ptp) setup_mouse(h);
    klog("i2c-hid: %04x:%04x in %s mode\n", h->vid, h->pid, h->ptp ? "touchpad" : "mouse");
    ihid_t* keep = (ihid_t*)kmalloc(sizeof(ihid_t));
    if (!keep || g_ndev == 4) { kfree(buf); kfree(f); kfree(keep); return 0; }
    *keep = *h;
    g_dev[g_ndev++] = keep;
    return 1;
}

static int probe_all(void) {
    static const uint16_t REGS[] = { 0x0020, 0x0001 };
    static const uint8_t USUAL[] = { 0x2C, 0x15, 0x2A, 0x38 };     /* when ACPI fills the address in at run time */
    int found = 0;
    for (int b = 0; b < g_ndw; b++) {
        for (int pass = 0; pass < 2 && !found; pass++) {
            int n = pass == 0 ? g_naddr : (int)sizeof(USUAL);
            for (int i = 0; i < n; i++) {
                uint8_t a = pass == 0 ? g_addrs[i] : USUAL[i];
                for (unsigned r = 0; r < sizeof(REGS) / sizeof(REGS[0]); r++) {
                    int rc = probe(&g_dw[b], a, REGS[r]);
                    if (rc < 0) break;                             /* no answer at all: next address */
                    if (rc > 0) { found++; break; }
                }
            }
        }
    }
    return found;
}

void i2chid_init(void) {
    scan_acpi();
    if (!g_acpi_hid) return;                                       /* no I2C HID device here */
    klog("i2c-hid: ACPI names an I2C HID device; %d I2C addresses, up to %u Hz\n", g_naddr, g_speed);
    pci_scan(find_dw, NULL);
    if (!g_ndw) { klog("i2c-hid: no I2C controller we can drive\n"); return; }
    int fast = g_speed >= 400000;
    for (int b = 0; b < g_ndw; b++) dw_setup(&g_dw[b], fast);
    if (!probe_all() && fast) {
        /* nothing at 400 kHz: once more at 100 kHz */
        for (int b = 0; b < g_ndw; b++) dw_setup(&g_dw[b], 0);
        probe_all();
    }
    if (!g_ndev) klog("i2c-hid: no touchpad found\n");
}

/* ── reports ─────────────────────────────────────────────────────────── */

static int fv(ihid_t* h, int idx, const uint8_t* r, int n) { return idx < 0 ? 0 : hid_value(r, n, &h->f[idx]); }

static void ptp_report(ihid_t* h, const uint8_t* r, int n) {
    uint32_t seen = 0;
    int any = 0, palm = 0;
    for (int k = 0; k < h->nfing; k++) {
        int id = h->cid[k] >= 0 ? fv(h, h->cid[k], r, n) & (NSLOTS - 1) : k;
        int tip = fv(h, h->tip[k], r, n);
        if (tip && h->conf[k] >= 0 && !fv(h, h->conf[k], r, n)) { tip = 0; palm = 1; }   /* a palm */
        if (!tip) continue;
        h->slot[id].down = 1;
        h->slot[id].x = fv(h, h->fx[k], r, n);
        h->slot[id].y = fv(h, h->fy[k], r, n);
        seen |= 1u << id;
        any = 1;
    }
    for (int k = 0; k < h->nfing; k++) {
        int id = h->cid[k] >= 0 ? fv(h, h->cid[k], r, n) & (NSLOTS - 1) : k;
        if (!fv(h, h->tip[k], r, n) && !(seen & (1u << id)) && (fv(h, h->fx[k], r, n) || fv(h, h->fy[k], r, n)))
            h->slot[id].down = 0;                                   /* lifted */
    }
    int cc = h->count >= 0 ? fv(h, h->count, r, n) : -1;
    if ((cc > 0 && cc <= h->nfing) || (cc == 0 && !any))
        for (int i = 0; i < NSLOTS; i++) if (!(seen & (1u << i))) h->slot[i].down = 0;

    int fingers = 0, flags = palm ? TPF_PALM : 0;
    for (int i = 0; i < NSLOTS; i++) fingers += h->slot[i].down;
    if (h->primary < 0 || !h->slot[h->primary].down) {
        int old = h->primary;
        h->primary = -1;
        for (int i = 0; i < NSLOTS; i++) if (h->slot[i].down) { h->primary = i; break; }
        if (old >= 0 && h->primary >= 0) flags |= TPF_JUMP;
    }
    int buttons = 0;
    for (int b = 0; b < 3; b++) if (fv(h, h->btn[b], r, n)) buttons |= 1 << b;
    int p = h->primary >= 0 ? h->primary : 0;
    if (palm && fingers) flags &= ~TPF_PALM;                      /* a palm beside real fingers: ignore it only */
    tp_frame(&h->tp, fingers, h->slot[p].x, h->slot[p].y, buttons, flags);
}

static void mouse_report(ihid_t* h, const uint8_t* r, int n) {
    int buttons = 0;
    for (int b = 0; b < 3; b++) if (fv(h, h->mb[b], r, n)) buttons |= 1 << b;
    int dx = fv(h, h->mx, r, n), dy = fv(h, h->my, r, n);
    mouse_inject(dx, -dy, buttons);
    int w = fv(h, h->mw, r, n);
    if (w) mouse_inject_wheel(-w);
}

void i2chid_poll(void) {
    for (int i = 0; i < g_ndev; i++) {
        ihid_t* h = g_dev[i];
        if (h->ptp) tp_tick(&h->tp);
        uint32_t now = timer_ms();
        if (now - h->last_poll < 7) continue;
        h->last_poll = now;
        /* every waiting report (a few at most) */
        for (int k = 0; k < 4; k++) {
            if (bus_xfer(h->bus, h->addr, NULL, 0, h->buf, h->max_input) < 0) break;
            int len = h->buf[0] | h->buf[1] << 8;
            if (len <= 2 || len > h->max_input) break;              /* nothing new */
            const uint8_t* r = h->buf + 2;
            int n = len - 2;
            int rid = 0;
            if (h->ids) { rid = r[0]; r++; n--; }
            if (h->ptp && rid == h->tp_rid) ptp_report(h, r, n);
            else if (!h->ptp && rid == h->m_rid) mouse_report(h, r, n);
        }
    }
}

int i2chid_describe(char* out, int cap) {
    if (!g_ndev) return 0;
    ihid_t* h = g_dev[0];
    const char* v = h->vid == 0x06CB ? "Synaptics" : h->vid == 0x04F3 ? "ELAN" : h->vid == 0x044E ? "ALPS" :
                    h->vid == 0x0488 ? "Cirque" : h->vid == 0x2808 ? "FocalTech" : h->vid == 0x093A ? "PixArt" :
                    h->vid == 0x27C6 ? "Goodix" : "I2C";
    ksnprintf(out, (size_t)cap, "%s %s (I2C HID %04x:%04x)", v, h->ptp ? "precision touchpad" : "touchpad", h->vid, h->pid);
    return 1;
}
