/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Radeon's video BIOS (kernel/radeon.h): found, checked and handed to
 * the AtomBIOS interpreter (third_party/radeon/atom.c, Linux's), with the
 * register access its tables use; the display paths, PLL limits and DDC
 * (EDID) reads it offers. After Linux's radeon_bios.c, radeon_device.c
 * (the card_info callbacks), radeon_atombios.c and atombios_i2c.c (MIT). */
#include "radeon.h"
#include "../third_party/radeon/atom.h"
#include "io.h"
#include "kheap.h"
#include "kstring.h"
#include "paging.h"
#include "serial.h"
#include "smp.h"
#include "spinlock.h"

#ifndef RADEON_MODEL
/* the notices of the code this follows (installed as /usr/share/licenses/radeon/NOTICES) */
__asm__(".pushsection .rodata\n"
        ".global radeon_notice_start\nradeon_notice_start:\n"
        ".incbin \"third_party/radeon/NOTICES\"\n"
        ".global radeon_notice_end\nradeon_notice_end:\n.popsection\n");
#endif

/* ── registers ──────────────────────────────────────────────────────── */

uint32_t rad_rd(radeon_t* r, uint32_t reg) {
    return reg + 4 <= r->mmio_size ? *(volatile uint32_t*)(r->mmio + reg) : 0;
}
void rad_wr(radeon_t* r, uint32_t reg, uint32_t v) {
    if (reg + 4 <= r->mmio_size) *(volatile uint32_t*)(r->mmio + reg) = v;
}

/* (registers past the BAR: through MM_INDEX / MM_DATA) */
#define MM_INDEX 0x0000
#define MM_DATA  0x0004
static uint32_t rd_any(radeon_t* r, uint32_t reg) {
    if (reg + 4 <= r->mmio_size) return rad_rd(r, reg);
    rad_wr(r, MM_INDEX, reg);
    return rad_rd(r, MM_DATA);
}
static void wr_any(radeon_t* r, uint32_t reg, uint32_t v) {
    if (reg + 4 <= r->mmio_size) { rad_wr(r, reg, v); return; }
    rad_wr(r, MM_INDEX, reg);
    rad_wr(r, MM_DATA, v);
}

/* the interpreter's view: register numbers in dwords */
static radeon_t* card_of(struct card_info* c) { return (radeon_t*)c->dev; }
static uint32_t cail_reg_read(struct card_info* c, uint32_t reg) { return rd_any(card_of(c), reg * 4); }
static void cail_reg_write(struct card_info* c, uint32_t reg, uint32_t v) { wr_any(card_of(c), reg * 4, v); }
/* the I/O BAR, where there is one (the same registers, as MM_INDEX / MM_DATA past its 256 bytes) */
static uint32_t cail_ioreg_read(struct card_info* c, uint32_t reg) {
    radeon_t* r = card_of(c);
    uint32_t b = reg * 4;
    if (!r->io_base) return rd_any(r, b);
    if (b < 0x100) return inl((uint16_t)(r->io_base + b));
    outl((uint16_t)(r->io_base + MM_INDEX), b);
    return inl((uint16_t)(r->io_base + MM_DATA));
}
static void cail_ioreg_write(struct card_info* c, uint32_t reg, uint32_t v) {
    radeon_t* r = card_of(c);
    uint32_t b = reg * 4;
    if (!r->io_base) { wr_any(r, b, v); return; }
    if (b < 0x100) { outl((uint16_t)(r->io_base + b), v); return; }
    outl((uint16_t)(r->io_base + MM_INDEX), b);
    outl((uint16_t)(r->io_base + MM_DATA), v);
}
/* the memory controller's indirect registers (rv515_mc_rreg: every AVIVO part since) */
#define MC_IND_INDEX 0x0070
#define MC_IND_DATA  0x0074
static spinlock_t g_mc_lock = SPINLOCK_INIT;
static uint32_t cail_mc_read(struct card_info* c, uint32_t reg) {
    radeon_t* r = card_of(c);
    uintptr_t f = spin_lock_irq(&g_mc_lock);
    rad_wr(r, MC_IND_INDEX, 0x7f0000u | (reg & 0xffffu));
    uint32_t v = rad_rd(r, MC_IND_DATA);
    rad_wr(r, MC_IND_INDEX, 0);
    spin_unlock_irq(&g_mc_lock, f);
    return v;
}
static void cail_mc_write(struct card_info* c, uint32_t reg, uint32_t v) {
    radeon_t* r = card_of(c);
    uintptr_t f = spin_lock_irq(&g_mc_lock);
    rad_wr(r, MC_IND_INDEX, 0xff0000u | (reg & 0xffffu));
    rad_wr(r, MC_IND_DATA, v);
    rad_wr(r, MC_IND_INDEX, 0);
    spin_unlock_irq(&g_mc_lock, f);
}
/* (no PLL register space on these parts: as Linux's radeon_invalid_rreg) */
static uint32_t cail_pll_read(struct card_info* c, uint32_t reg) { (void)c; klog("radeon: the BIOS read PLL register %x (none on this card)\n", reg); return 0; }
static void cail_pll_write(struct card_info* c, uint32_t reg, uint32_t v) { (void)c; (void)v; klog("radeon: the BIOS wrote PLL register %x (none on this card)\n", reg); }

/* ── the families ───────────────────────────────────────────────────── */

/* (from Linux's drm_pciids.h: every TeraScale 2 part's device id) */
static const struct { uint16_t lo, hi; int family; } FAMILIES[] = {
    { 0x6700, 0x671F, CHIP_CAYMAN }, { 0x6720, 0x673F, CHIP_BARTS }, { 0x6740, 0x675F, CHIP_TURKS },
    { 0x6760, 0x677F, CHIP_CAICOS }, { 0x6880, 0x689B, CHIP_CYPRESS }, { 0x689C, 0x689D, CHIP_HEMLOCK },
    { 0x689E, 0x689F, CHIP_CYPRESS }, { 0x68A0, 0x68BF, CHIP_JUNIPER }, { 0x68C0, 0x68DF, CHIP_REDWOOD },
    { 0x68E0, 0x68FF, CHIP_CEDAR },
};

int rad_family(uint16_t device) {
    for (unsigned i = 0; i < sizeof(FAMILIES) / sizeof(FAMILIES[0]); i++)
        if (device >= FAMILIES[i].lo && device <= FAMILIES[i].hi) return FAMILIES[i].family;
    return -1;
}

const char* rad_family_name(int f) {
    switch (f) {
    case CHIP_CEDAR: return "Cedar"; case CHIP_REDWOOD: return "Redwood"; case CHIP_JUNIPER: return "Juniper";
    case CHIP_CYPRESS: return "Cypress"; case CHIP_HEMLOCK: return "Hemlock"; case CHIP_BARTS: return "Barts";
    case CHIP_TURKS: return "Turks"; case CHIP_CAICOS: return "Caicos"; case CHIP_CAYMAN: return "Cayman";
    default: return "?";
    }
}

/* ── finding the video BIOS ─────────────────────────────────────────── */

#define BIOS_MAX (256u << 10)

/* a plausible AtomBIOS image: its signature, x86 code type, the ATOM table */
static int bios_ok(const uint8_t* b, uint32_t n) {
    if (n < 0x100 || b[0] != 0x55 || b[1] != 0xAA) return 0;
    uint32_t pcir = (uint32_t)b[0x18] | (uint32_t)b[0x19] << 8;
    if (pcir + 0x18 > n || memcmp(b + pcir, "PCIR", 4) || b[pcir + 0x14] != 0) return 0;   /* (code type 0: x86) */
    uint32_t hdr = (uint32_t)b[0x48] | (uint32_t)b[0x49] << 8;
    if (!hdr || hdr + 8 > n) return 0;
    return !memcmp(b + hdr + 4, "ATOM", 4) || !memcmp(b + hdr + 4, "MOTA", 4);
}

static int keep(radeon_t* r, const uint8_t* src, uint32_t n, const char* from) {
    if (n > BIOS_MAX) n = BIOS_MAX;
    if (!bios_ok(src, n)) return 0;
    uint8_t* b = kmalloc(n);
    if (!b) return 0;
    memcpy(b, src, n);
    r->bios = b;
    r->bios_size = n;
    r->bios_from = from;
    return 1;
}

/* UEFI: the GOP driver's copy of the image, in the ACPI VFCT table */
static int from_vfct(radeon_t* r) {
    const uint8_t* t = acpi_find_table("VFCT");
    if (!t) return 0;
    uint32_t len = (uint32_t)t[4] | (uint32_t)t[5] << 8 | (uint32_t)t[6] << 16 | (uint32_t)t[7] << 24;
    if (len < 0x4C || len > (16u << 20)) return 0;
    uint32_t off = (uint32_t)t[0x34] | (uint32_t)t[0x35] << 8 | (uint32_t)t[0x36] << 16 | (uint32_t)t[0x37] << 24;
    while (off + 0x1C <= len) {
        const uint8_t* h = t + off;                         /* VFCT_IMAGE_HEADER */
        uint32_t bus = h[0], dev = h[4], fn = h[8];
        uint16_t ven = (uint16_t)(h[12] | h[13] << 8), did = (uint16_t)(h[14] | h[15] << 8);
        uint32_t ilen = (uint32_t)h[24] | (uint32_t)h[25] << 8 | (uint32_t)h[26] << 16 | (uint32_t)h[27] << 24;
        off += 0x1C;
        if (ilen > len - off) return 0;
        if (ilen && bus == r->pci.bus && dev == r->pci.dev && fn == r->pci.fn && ven == r->pci.vendor && did == r->pci.device)
            return keep(r, t + off, ilen, "ACPI VFCT");
        off += ilen;
    }
    return 0;
}

/* the option ROM, through the expansion ROM BAR (when the firmware gave it an address) */
static int from_rom_bar(radeon_t* r) {
    uint32_t old = pci_read32(r->pci.bus, r->pci.dev, r->pci.fn, 0x30);
    uint32_t base = old & 0xFFFFF800u;
    if (!base) return 0;
    pci_write32(r->pci.bus, r->pci.dev, r->pci.fn, 0x30, 0xFFFFF800u);
    uint32_t mask = pci_read32(r->pci.bus, r->pci.dev, r->pci.fn, 0x30) & 0xFFFFF800u;
    uint32_t size = mask ? ~mask + 1u : 0;
    pci_write32(r->pci.bus, r->pci.dev, r->pci.fn, 0x30, base | 1u);
    int ok = 0;
    if (size >= 0x800 && size <= (16u << 20) && mmio_map(base, size)) {
        const volatile uint8_t* rom = (const volatile uint8_t*)(uintptr_t)base;
        uint32_t n = size < BIOS_MAX ? size : BIOS_MAX;
        if (rom[0] == 0x55 && rom[1] == 0xAA) {
            uint32_t img = (uint32_t)rom[2] * 512u;          /* (the image's own length) */
            if (img && img < n) n = img;
            uint8_t* tmp = kmalloc(n);
            if (tmp) {
                for (uint32_t i = 0; i < n; i++) tmp[i] = rom[i];
                ok = keep(r, tmp, n, "PCI ROM");
                kfree(tmp);
            }
        }
    }
    pci_write32(r->pci.bus, r->pci.dev, r->pci.fn, 0x30, old);
    return ok;
}

/* Northern Islands with the ROM turned off by the firmware: on for a moment
 * (ni_read_disabled_bios) */
#define R600_BUS_CNTL           0x5420
#define R600_BIOS_ROM_DIS       (1u << 1)
#define R600_ROM_CNTL           0x1600
#define R600_SCK_OVERWRITE      (1u << 1)
#define AVIVO_D1VGA_CONTROL     0x0330
#define AVIVO_D2VGA_CONTROL     0x0338
#define AVIVO_VGA_RENDER_CONTROL 0x0300
#define DVGA_MODE_ENABLE        (1u << 0)
#define DVGA_TIMING_SELECT      (1u << 8)
#define VGA_VSTATUS_CNTL_MASK   (3u << 16)
static int from_disabled_rom(radeon_t* r) {
    uint32_t bus = rad_rd(r, R600_BUS_CNTL), d1 = rad_rd(r, AVIVO_D1VGA_CONTROL), d2 = rad_rd(r, AVIVO_D2VGA_CONTROL);
    uint32_t vga = rad_rd(r, AVIVO_VGA_RENDER_CONTROL), rom = rad_rd(r, R600_ROM_CNTL);
    rad_wr(r, R600_BUS_CNTL, bus & ~R600_BIOS_ROM_DIS);
    rad_wr(r, AVIVO_D1VGA_CONTROL, d1 & ~(DVGA_MODE_ENABLE | DVGA_TIMING_SELECT));
    rad_wr(r, AVIVO_D2VGA_CONTROL, d2 & ~(DVGA_MODE_ENABLE | DVGA_TIMING_SELECT));
    rad_wr(r, AVIVO_VGA_RENDER_CONTROL, vga & ~VGA_VSTATUS_CNTL_MASK);
    rad_wr(r, R600_ROM_CNTL, rom | R600_SCK_OVERWRITE);
    int ok = from_rom_bar(r);
    rad_wr(r, R600_BUS_CNTL, bus);
    rad_wr(r, AVIVO_D1VGA_CONTROL, d1);
    rad_wr(r, AVIVO_D2VGA_CONTROL, d2);
    rad_wr(r, AVIVO_VGA_RENDER_CONTROL, vga);
    rad_wr(r, R600_ROM_CNTL, rom);
    if (ok) r->bios_from = "PCI ROM (enabled for it)";
    return ok;
}

/* BIOS boot with the card as the primary display: the copy at 0xC0000 */
static int from_shadow(radeon_t* r) {
    const uint8_t* s = (const uint8_t*)(uintptr_t)0xC0000;
    if (s[0] != 0x55 || s[1] != 0xAA) return 0;
    uint32_t n = (uint32_t)s[2] * 512u;
    if (!n || n > (128u << 10)) n = 128u << 10;
    /* (only if it is this card's: its PCI data structure names it) */
    uint32_t pcir = (uint32_t)s[0x18] | (uint32_t)s[0x19] << 8;
    if (pcir + 8 > n || memcmp(s + pcir, "PCIR", 4)) return 0;
    uint16_t ven = (uint16_t)(s[pcir + 4] | s[pcir + 5] << 8), dev = (uint16_t)(s[pcir + 6] | s[pcir + 7] << 8);
    if (ven != r->pci.vendor || dev != r->pci.device) return 0;
    return keep(r, s, n, "legacy shadow (0xC0000)");
}

/* ── the interpreter ────────────────────────────────────────────────── */

static spinlock_t g_atom_lock = SPINLOCK_INIT;   /* (one table at a time, as Linux's mutex) */

int rad_atom_cmd_rev(radeon_t* r, int index, uint8_t* frev, uint8_t* crev) {
    return r->atom && atom_parse_cmd_header(r->atom, index, frev, crev) ? 0 : -1;
}

int rad_atom_exec(radeon_t* r, int index, void* params, int size) {
    if (!r->atom || !atom_parse_cmd_header(r->atom, index, NULL, NULL)) return -1;
    /* (tables wait in loops and may sleep briefly: a lock taken without
     *  interrupts off, from task context only) */
    spin_lock(&g_atom_lock);
    int rc = atom_execute_table(r->atom, index, (uint32_t*)params, size);
    spin_unlock(&g_atom_lock);
    return rc ? -1 : 0;
}

/* ── what the BIOS describes ────────────────────────────────────────── */

static const uint8_t* bios_at(radeon_t* r, uint32_t off, uint32_t n) {
    return off + n <= r->bios_size ? r->bios + off : NULL;
}
static uint16_t b16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

/* FirmwareInfo: the pixel PLLs' limits (radeon_atom_get_clock_info) */
static void read_clocks(radeon_t* r) {
    uint16_t off;
    uint8_t frev, crev;
    rad_clocks_t* c = &r->clk;
    memset(c, 0, sizeof(*c));
    if (!atom_parse_data_header(r->atom, GetIndexIntoMasterTable(DATA, FirmwareInfo), NULL, &frev, &crev, &off)) return;
    const uint8_t* fi = bios_at(r, off, sizeof(ATOM_FIRMWARE_INFO_V2_1));
    if (!fi) return;
    /* (as Linux's union firmware_info: the fields every version has through
     *  v1.0's layout, the DCE 4 ones through v2.1's) */
    const ATOM_FIRMWARE_INFO* f = (const ATOM_FIRMWARE_INFO*)fi;
    const ATOM_FIRMWARE_INFO_V2_1* f21 = (const ATOM_FIRMWARE_INFO_V2_1*)fi;
    c->ref_freq = f->usReferenceClock;
    c->pll_out_min = (frev < 2 && crev < 2) ? f->usMinPixelClockPLL_Output
                                            : ((const ATOM_FIRMWARE_INFO_V1_2*)fi)->ulMinPixelClockPLL_Output;
    c->pll_out_max = f->ulMaxPixelClockPLL_Output;
    if (!c->pll_out_min) c->pll_out_min = 64800;
    c->pll_in_min = f->usMinPixelClockPLL_Input;
    c->pll_in_max = f->usMaxPixelClockPLL_Input;
    c->max_pixel_clock = f->usMaxPixelClock ? f->usMaxPixelClock : 40000;
    if (frev >= 2) {
        c->default_dispclk = f21->ulDefaultDispEngineClkFreq;
        c->dp_extclk = f21->usUniphyDPModeExtClkFreq;
    }
    if (!c->default_dispclk) c->default_dispclk = 54000;            /* (540 MHz, DCE 5's default) */
}

/* Object_Header: the display paths, each with its connector's DDC line
 * (radeon_get_atom_connector_info_from_object_table, without routers,
 * TV and the IGP's lane tables) */
static void read_paths(radeon_t* r) {
    uint16_t off, size;
    uint8_t frev, crev;
    r->npaths = 0;
    if (!atom_parse_data_header(r->atom, GetIndexIntoMasterTable(DATA, Object_Header), &size, &frev, &crev, &off) || crev < 2) return;
    const uint8_t* h = bios_at(r, off, sizeof(ATOM_OBJECT_HEADER));
    if (!h) return;
    const ATOM_OBJECT_HEADER* oh = (const ATOM_OBJECT_HEADER*)h;
    uint16_t support = oh->usDeviceSupport;
    const uint8_t* pt = bios_at(r, off + oh->usDisplayPathTableOffset, 2);
    const uint8_t* ct = bios_at(r, off + oh->usConnectorObjectTableOffset, 1);
    if (!pt || !ct) return;
    uint32_t pos = off + oh->usDisplayPathTableOffset + offsetof(ATOM_DISPLAY_OBJECT_PATH_TABLE, asDispPath);
    for (int i = 0; i < pt[0] && r->npaths < RAD_PATHS; i++) {
        const uint8_t* p = bios_at(r, pos, sizeof(ATOM_DISPLAY_OBJECT_PATH));
        if (!p) break;
        const ATOM_DISPLAY_OBJECT_PATH* dp = (const ATOM_DISPLAY_OBJECT_PATH*)p;
        uint16_t psize = dp->usSize, tag = dp->usDeviceTag, conn = dp->usConnObjectId;
        if (psize < 8 || !bios_at(r, pos, psize)) break;
        pos += psize;
        if (!(support & tag) || (tag & (ATOM_DEVICE_TV_SUPPORT | ATOM_DEVICE_CV_SUPPORT))) continue;
        rad_path_t* rp = &r->paths[r->npaths];
        memset(rp, 0, sizeof(*rp));
        rp->device = tag;
        rp->conn_obj = conn;
        rp->conn_type = (uint8_t)((conn & OBJECT_ID_MASK) >> OBJECT_ID_SHIFT);
        for (int j = 0; j < (psize - 8) / 2; j++) {
            uint16_t g = b16(p + offsetof(ATOM_DISPLAY_OBJECT_PATH, usGraphicObjIds) + 2u * (uint32_t)j);
            if (((g & OBJECT_TYPE_MASK) >> OBJECT_TYPE_SHIFT) == GRAPH_OBJECT_TYPE_ENCODER && rp->nenc < 2)
                rp->enc_obj[rp->nenc++] = g;
        }
        /* the connector's records: its I2C one */
        for (int j = 0; j < ct[0]; j++) {
            /* (ATOM_OBJECT_TABLE: the count, 3 padding, then 8-byte objects) */
            const uint8_t* o = bios_at(r, off + oh->usConnectorObjectTableOffset + offsetof(ATOM_OBJECT_TABLE, asObjects) +
                                          sizeof(ATOM_OBJECT) * (uint32_t)j, sizeof(ATOM_OBJECT));
            if (!o || ((const ATOM_OBJECT*)o)->usObjectID != conn) continue;
            uint32_t rec = off + ((const ATOM_OBJECT*)o)->usRecordOffset;
            for (int guard = 0; guard < 32; guard++) {
                const uint8_t* rh = bios_at(r, rec, 2);
                if (!rh || !rh[0] || !rh[1] || rh[0] > ATOM_MAX_OBJECT_RECORD_NUMBER) break;
                if (rh[0] == ATOM_I2C_RECORD_TYPE && bios_at(r, rec, sizeof(ATOM_I2C_RECORD)))
                    rp->i2c_id = ((const ATOM_I2C_ID_CONFIG_ACCESS*)&((const ATOM_I2C_RECORD*)rh)->sucI2cId)->ucAccess;
                rec += rh[1];
            }
            break;
        }
        r->npaths++;
    }
}

int rad_atom_init(radeon_t* r, char* why, int cap) {
    if (!from_vfct(r) && !from_rom_bar(r) && !from_disabled_rom(r) && !from_shadow(r)) {
        kstrlcpy(why, "no video BIOS found (ACPI VFCT, PCI ROM, 0xC0000)", (size_t)cap);
        return -1;
    }
    static struct card_info card;                    /* (one card) */
    memset(&card, 0, sizeof(card));
    card.dev = r;
    card.family = r->family;
    card.reg_read = cail_reg_read;
    card.reg_write = cail_reg_write;
    card.ioreg_read = cail_ioreg_read;
    card.ioreg_write = cail_ioreg_write;
    card.mc_read = cail_mc_read;
    card.mc_write = cail_mc_write;
    card.pll_read = cail_pll_read;
    card.pll_write = cail_pll_write;
    r->atom = atom_parse(&card, r->bios, r->bios_size);
    if (!r->atom) { ksnprintf(why, (size_t)cap, "the video BIOS (%s) is not an AtomBIOS one", r->bios_from); return -1; }
    if (atom_allocate_fb_scratch(r->atom) != 0) { kstrlcpy(why, "no memory for the BIOS's scratch", (size_t)cap); return -1; }
    read_clocks(r);
    read_paths(r);
    return 0;
}

/* ── DDC: EDID through ProcessI2cChannelTransaction ─────────────────── */

int rad_atom_read_edid(radeon_t* r, uint8_t i2c_id, uint8_t* edid) {
    if (!r->atom || !i2c_id) return -1;
    PROCESS_I2C_CHANNEL_TRANSACTION_PS_ALLOCATION args;
    int index = GetIndexIntoMasterTable(COMMAND, ProcessI2cChannelTransaction);
    /* (one read of 128 bytes from offset 0 of the monitor's 0x50: the table
     *  writes the offset, then reads; the data lands in the BIOS's scratch) */
    memset(&args, 0, sizeof(args));
    args.ucFlag = HW_I2C_READ;
    args.ucI2CSpeed = 50;                             /* (kHz, as Linux's) */
    args.ucTransBytes = 128;
    args.ucSlaveAddr = 0x50 << 1;
    args.ucLineNumber = i2c_id;
    args.ucRegIndex = 0;
    if (!r->atom->scratch || r->atom->scratch_size_bytes < 128) return -1;
    if (rad_atom_exec(r, index, &args, sizeof(args)) != 0 || args.ucStatus != HW_ASSISTED_I2C_STATUS_SUCCESS) return -1;
    memcpy(edid, r->atom->scratch, 128);
    static const uint8_t HDR[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    if (memcmp(edid, HDR, 8)) return -1;
    uint8_t sum = 0;
    for (int i = 0; i < 128; i++) sum = (uint8_t)(sum + edid[i]);
    return sum ? -1 : 0;
}

static const char* conn_name(uint8_t t) {
    switch (t) {
    case CONNECTOR_OBJECT_ID_SINGLE_LINK_DVI_I: case CONNECTOR_OBJECT_ID_DUAL_LINK_DVI_I: return "DVI-I";
    case CONNECTOR_OBJECT_ID_SINGLE_LINK_DVI_D: case CONNECTOR_OBJECT_ID_DUAL_LINK_DVI_D: return "DVI-D";
    case CONNECTOR_OBJECT_ID_VGA: return "VGA";
    case CONNECTOR_OBJECT_ID_HDMI_TYPE_A: case CONNECTOR_OBJECT_ID_HDMI_TYPE_B: return "HDMI";
    case CONNECTOR_OBJECT_ID_DISPLAYPORT: return "DisplayPort";
    case CONNECTOR_OBJECT_ID_eDP: return "eDP";
    case CONNECTOR_OBJECT_ID_LVDS: return "LVDS";
    default: return "other";
    }
}

void rad_atom_info(radeon_t* r, char* buf, int cap) {
    int n = ksnprintf(buf, (size_t)cap, "video BIOS: %u KiB from %s; PLL ref %u.%02u MHz, max pixel clock %u MHz\n",
                      r->bios_size / 1024, r->bios_from ? r->bios_from : "-", r->clk.ref_freq / 100, r->clk.ref_freq % 100,
                      r->clk.max_pixel_clock / 100);
    for (int i = 0; i < r->npaths && n < cap; i++) {
        const rad_path_t* p = &r->paths[i];
        n += ksnprintf(buf + n, (size_t)(cap - n), "  output %d: %s, devices %04x, DDC line %u, encoders %04x %04x\n",
                       i, conn_name(p->conn_type), p->device, p->i2c_id, p->enc_obj[0], p->nenc > 1 ? p->enc_obj[1] : 0);
    }
}
