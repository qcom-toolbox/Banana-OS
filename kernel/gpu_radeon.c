/* SPDX-License-Identifier: GPL-3.0-or-later */
/* AMD Radeon display engines, EXPERIMENTAL and off unless asked for
 * (`gpu experimental on`, kept in the settings):
 *   DCE 4 / 5   Radeon HD 5000 / 6000 / 7000 TeraScale 2 (Evergreen and
 *               Northern Islands: Cedar, Redwood, Juniper, Cypress, Caicos -
 *               the HD 7450 -, Turks, Barts, Cayman)
 *   DCE 11.2    Radeon RX 400 / 500 (Polaris 10 / 11 / 12: the RX 580...)
 * After Linux's radeon (evergreen) and amdgpu (dce_v11_0) drivers. Written
 * without the hardware to try it on: hence opt-in.
 *
 * The firmware (VBIOS / GOP) has set the mode and keeps scanning its
 * framebuffer out; this driver adds, on the CRTC that shows it:
 *  - the mouse pointer in hardware: the cursor (64x64 ARGB), its image in
 *    the card's memory just past the framebuffer;
 *  - the vertical blank (the CRTC's frame counter).
 * Before writing anything it checks that one CRTC is on and scans out
 * exactly the framebuffer the firmware handed over (its memory-controller
 * address against the video memory BAR); if not, it leaves the card alone
 * and the screen stays the firmware's (write-combining, kernel/gpu_fw.c).
 *
 * On TeraScale 2 (DCE 4 / 5) it also sets modes: the video BIOS is read and
 * run (kernel/radeon_atom.c), the monitor's modes come from its EDID over
 * the BIOS's DDC, and a new mode is set on that CRTC (kernel/radeon_kms.c)
 * with a framebuffer of its own 64 MiB into the video memory. `gpu radeon
 * dump` saves the BIOS and the display registers (a report to send). */
#include "gpu.h"
#include "timer.h"
#include "fb.h"
#include "kstring.h"
#include "serial.h"
#include "paging.h"
#include "settings.h"
#include "radeon.h"
#include "edid.h"
#include "fs.h"
#include "kheap.h"

/* byte offsets in the register BAR; the same for CRTC 0 on DCE 4, 5 and 11 */
#define MC_VM_FB_LOCATION          0x2024   /* 15:0 the video memory's start in the MC's space, in 16 MiB */
#define GRPH_ENABLE                0x6800
#define GRPH_PRIMARY_SURFACE_ADDR  0x6810
#define GRPH_PRIMARY_SURFACE_HIGH  0x681C
#define CUR_CONTROL                0x6998
#define CUR_SURFACE_ADDRESS        0x699C
#define CUR_SIZE                   0x69A0
#define CUR_SURF_HIGH              0x69A4
#define CUR_POSITION               0x69A8
#define CUR_HOT_SPOT               0x69AC
#define CUR_UPDATE                 0x69B8
#define CRTC_CONTROL               0x6E70
#define CRTC_STATUS_FRAME_COUNT    0x6E98

#define CUR_EN            (1u << 0)
#define CUR_MODE_ARGB     (3u << 8)         /* 24.8, not premultiplied */
#define CUR_URGENT_1_2    (4u << 24)
#define CUR_UPDATE_LOCK   (1u << 16)

/* the CRTCs' register blocks (bytes from CRTC 0's) */
static const uint32_t DCE4_CRTC[6]  = { 0, 0xC00, 0x9800, 0xA400, 0xB000, 0xBC00 };
static const uint32_t DCE11_CRTC[6] = { 0, 0x800, 0x1000, 0x9800, 0xA000, 0xA800 };

typedef struct { uint16_t lo, hi; int dce; const char* name; } rad_family_t;
static const rad_family_t FAM[] = {
    { 0x6880, 0x68FF, 4,  "AMD Radeon HD 5000 / 6000 series (Evergreen)" },
    { 0x6700, 0x671F, 5,  "AMD Radeon HD 6900 series (Cayman)" },
    { 0x6720, 0x673F, 5,  "AMD Radeon HD 6800 series (Barts)" },
    { 0x6740, 0x675F, 5,  "AMD Radeon HD 6500 / 6600 / 7500 / 7600 series (Turks)" },
    { 0x6760, 0x677F, 5,  "AMD Radeon HD 6400 / 7400 series (Caicos)" },
    { 0x67C0, 0x67DF, 11, "AMD Radeon RX 470 / 480 / 570 / 580 (Polaris 10)" },
    { 0x67E0, 0x67FF, 11, "AMD Radeon RX 460 / 560 (Polaris 11)" },
    { 0x6980, 0x699F, 11, "AMD Radeon RX 540 / 550 (Polaris 12)" },
    { 0x6FDF, 0x6FDF, 11, "AMD Radeon RX 580 2048SP (Polaris 20)" },
};

static gpu_t g_gpu;
static volatile uint8_t* g_reg;
static uint32_t g_reg_size;
static volatile uint32_t* g_cur;           /* the cursor image, in video memory (through the BAR) */
static uint64_t g_cur_mc;                  /* ... as the display engine sees it */
static uint32_t g_crtc;                    /* the CRTC's register offset */
static int g_crtc_n, g_dce, g_on, g_hot_x, g_hot_y, g_active;
/* the cursor surface's rows: 64 pixels on DCE 4/5; DCE 8 and later (Polaris) read
 * 128-pixel rows whatever the size shown (as amdgpu's 128x128 cursors) */
static int g_pitch = 64;
#define CUR_BYTES ((uint32_t)g_pitch * 64u * 4u)
static char g_why[96] = "not tried (gpu experimental on)";

/* mode setting (DCE 4 / 5) */
static radeon_t g_rad;
static int g_kms;                          /* 1: the BIOS runs and an output has a monitor */
static char g_kms_why[96] = "";
static int g_path = -1, g_digital;         /* the output the monitor is on; its EDID says digital */
static edid_mode_t g_modes[32];
static int g_nmodes;
static char g_monitor[24];
static uintptr_t g_vb;                     /* the video memory BAR */
static uint64_t g_vram_mc;                 /* ... where the memory controller puts it */
static int g_cur_w, g_cur_h;
#define NEW_FB_OFF (64ull << 20)

static uint32_t rd(uint32_t r) { return r + 4 <= g_reg_size ? *(volatile uint32_t*)(g_reg + r) : 0; }
static void wr(uint32_t r, uint32_t v) { if (r + 4 <= g_reg_size) *(volatile uint32_t*)(g_reg + r) = v; }

/* the sizes the monitor takes (one entry a size: the refresh nearest 60 Hz is the one set) */
static int rad_modes(gpu_t* g, display_mode_t* out, int max) {
    (void)g;
    int n = 0;
    if (max > 0 && g_cur_w) out[n++] = (display_mode_t){ g_cur_w, g_cur_h };
    for (int i = 0; i < g_nmodes && n < max; i++) {
        int dup = 0;
        for (int k = 0; k < n; k++) if (out[k].w == g_modes[i].hdisplay && out[k].h == g_modes[i].vdisplay) dup = 1;
        if (!dup) out[n++] = (display_mode_t){ g_modes[i].hdisplay, g_modes[i].vdisplay };
    }
    return n;
}

static int rad_set_mode(gpu_t* g, int w, int h) {
    (void)g;
    if (w == g_cur_w && h == g_cur_h) return 0;
    if (!g_kms) return -1;
    const edid_mode_t* best = NULL;
    for (int i = 0; i < g_nmodes; i++) {
        const edid_mode_t* m = &g_modes[i];
        if (m->hdisplay != w || m->vdisplay != h) continue;
        int d = m->refresh > 60 ? m->refresh - 60 : 60 - m->refresh;
        int bd = best ? (best->refresh > 60 ? best->refresh - 60 : 60 - best->refresh) : 1000;
        if (d < bd) best = m;
    }
    if (!best) return -1;
    /* its framebuffer: rows of a multiple of 64 pixels, 64 MiB into the video memory */
    int pitch = (w + 63) & ~63;
    uint64_t bytes = (uint64_t)pitch * (uint64_t)h * 4u;
    uint32_t vram_mb = rd(0x5428);                    /* (CONFIG_MEMSIZE: MiB on Evergreen) */
    if (bytes > (64ull << 20) || (vram_mb && NEW_FB_OFF + bytes > (uint64_t)vram_mb << 20)) return -1;
    uintptr_t va = g_vb + (uintptr_t)NEW_FB_OFF;
    if (!mmio_map(va, bytes)) return -1;
    paging_set_wc(va, bytes);
    for (uint64_t i = 0; i < bytes / 4; i++) ((volatile uint32_t*)va)[i] = 0;
    char why[96];
    if (rad_kms_set(&g_rad, g_crtc_n, g_path, g_digital, best, g_vram_mc + NEW_FB_OFF, w, h, pitch, why, sizeof(why)) != 0) {
        klog("radeon: %dx%d not set: %s\n", w, h, why);
        kstrlcpy(g_kms_why, why, sizeof(g_kms_why));
        return -1;
    }
    g_cur_w = w;
    g_cur_h = h;
    gpu_set_framebuffer(va, w, h, pitch * 4);
    return 0;
}

/* the video BIOS, the outputs, the monitor's modes: mode setting possible (DCE 4 / 5) */
static void kms_init(const pci_dev_t* d) {
    memset(&g_rad, 0, sizeof(g_rad));
    g_rad.pci = *d;
    g_rad.family = rad_family(d->device);
    g_rad.mmio = g_reg;
    g_rad.mmio_size = g_reg_size;
    for (int b = 0; b < 6; b++) {
        int io = 0;
        uintptr_t a = pci_bar(d, b, &io);
        if (io && a) { g_rad.io_base = (uint16_t)a; break; }
    }
    if (g_rad.family < 0) { kstrlcpy(g_kms_why, "not a TeraScale 2 part", sizeof(g_kms_why)); return; }
    if (rad_atom_init(&g_rad, g_kms_why, sizeof(g_kms_why)) != 0) return;
    /* the monitor: the first output whose DDC answers (a digital one first) */
    uint8_t edid[128];
    for (int pass = 0; pass < 2 && g_path < 0; pass++)
        for (int i = 0; i < g_rad.npaths && g_path < 0; i++) {
            const rad_path_t* p = &g_rad.paths[i];
            if (!p->i2c_id || rad_atom_read_edid(&g_rad, p->i2c_id, edid) != 0) continue;
            int digital = (edid[20] & 0x80) != 0;
            if (pass == 0 && !digital) continue;
            g_path = i;
            g_digital = digital;
        }
    if (g_path < 0) { kstrlcpy(g_kms_why, "no monitor answers on the outputs' DDC lines", sizeof(g_kms_why)); return; }
    edid_name(edid, g_monitor, sizeof(g_monitor));
    int kind = rad_kms_path_kind(&g_rad.paths[g_path], g_digital);
    if (kind == RAD_OUT_DP) { kstrlcpy(g_kms_why, "the monitor is on DisplayPort: the firmware's mode is kept", sizeof(g_kms_why)); return; }
    uint32_t max = rad_kms_max_clock(&g_rad, &g_rad.paths[g_path], kind);
    edid_mode_t all[32];
    int n = edid_modes(edid, all, 32);
    g_nmodes = 0;
    for (int i = 0; i < n; i++) if (all[i].clock <= max && all[i].hdisplay <= 4096 && all[i].vdisplay <= 4096) g_modes[g_nmodes++] = all[i];
    g_kms = g_nmodes > 0;
    ksnprintf(g_kms_why, sizeof(g_kms_why), "%s, %d modes", g_monitor, g_nmodes);
}

static void rad_wait_vblank(gpu_t* g) {
    (void)g;
    uint32_t f = rd(CRTC_STATUS_FRAME_COUNT + g_crtc);
    uint64_t start = timer_counter();
    for (uint32_t i = 0; i < 3000000u && rd(CRTC_STATUS_FRAME_COUNT + g_crtc) == f; i++) {
        if (!(i & 255) && timer_counter() - start >= 20000000) break;
        __asm__ volatile("pause");
    }
    gpu_note_vblank(rd(CRTC_STATUS_FRAME_COUNT + g_crtc) == f);
}

static int rad_cursor_image(gpu_t* g, const uint32_t* argb, int w, int h, int hx, int hy) {
    (void)g;
    if (w > 64 || h > 64) return -1;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < g_pitch; x++) g_cur[y * g_pitch + x] = x < w && y < h ? argb[y * w + x] : 0;
    g_hot_x = hx;
    g_hot_y = hy;
    return 0;
}

static void rad_cursor_move(gpu_t* g, int x, int y, int visible) {
    (void)g;
    x -= g_hot_x;
    y -= g_hot_y;
    int xo = 0, yo = 0;                          /* off the top / left edge: the hot spot takes it */
    if (x < 0) { xo = -x > 63 ? 63 : -x; x = 0; }
    if (y < 0) { yo = -y > 63 ? 63 : -y; y = 0; }
    wr(CUR_UPDATE + g_crtc, rd(CUR_UPDATE + g_crtc) | CUR_UPDATE_LOCK);
    if (visible != g_on) {
        wr(CUR_CONTROL + g_crtc, visible ? CUR_EN | CUR_MODE_ARGB | CUR_URGENT_1_2 : 0);
        g_on = visible;
    }
    wr(CUR_POSITION + g_crtc, (uint32_t)x << 16 | (uint32_t)y);
    wr(CUR_HOT_SPOT + g_crtc, (uint32_t)xo << 16 | (uint32_t)yo);
    wr(CUR_UPDATE + g_crtc, rd(CUR_UPDATE + g_crtc) & ~CUR_UPDATE_LOCK);
}

static void rad_info(gpu_t* g, char* buf, int cap) {
    (void)g;
    const fb_info_t* fi = fb_info();
    int n = ksnprintf(buf, (size_t)cap, "EXPERIMENTAL: DCE %d, CRTC %d, frame %u\n"
                      "framebuffer %ux%u, write-combining; cursor at 0x%llx\n",
                      g_dce, g_crtc_n, rd(CRTC_STATUS_FRAME_COUNT + g_crtc), fi ? fi->width : 0, fi ? fi->height : 0,
                      (unsigned long long)g_cur_mc);
    if (g_dce <= 5 && n < cap) {
        n += ksnprintf(buf + n, (size_t)(cap - n), "mode setting: %s\n", g_kms ? "yes" : "no");
        if (n < cap) n += ksnprintf(buf + n, (size_t)(cap - n), "  %s\n", g_kms_why);
        if (g_rad.atom && n < cap) { rad_atom_info(&g_rad, buf + n, cap - n); n += (int)strlen(buf + n); }
    }
    if (n < cap) ksnprintf(buf + n, (size_t)(cap - n), "a report: gpu radeon dump; turn off: gpu experimental off (and restart)");
}

/* `gpu radeon dump`: the video BIOS and the display registers, saved (a report) */
int gpu_radeon_dump(const char* dir, char* msg, int cap) {
    if (!g_reg) { kstrlcpy(msg, "no Radeon driven (gpu experimental on, lsgpu)", (size_t)cap); return -1; }
    char path[FS_PATH_LEN];
    fs_mkdir_p(dir);
    int saved = 0;
    if (g_rad.bios) {
        ksnprintf(path, sizeof(path), "%s/radeon-vbios.rom", dir);
        if (fs_write_path(path, g_rad.bios, g_rad.bios_size) >= 0) saved++;
    }
    /* the display engine's registers (0x6000 - 0x10000) and a few of the memory controller's */
    uint32_t n = 0x10000 / 4;
    uint32_t* regs = kmalloc(n * 4);
    if (regs) {
        for (uint32_t i = 0; i < n; i++) regs[i] = i * 4 >= 0x6000 || i * 4 < 0x3000 ? rd(i * 4) : 0;
        ksnprintf(path, sizeof(path), "%s/radeon-regs.bin", dir);
        if (fs_write_path(path, regs, n * 4) >= 0) saved++;
        kfree(regs);
    }
    char* info = kmalloc(4096);
    if (info) {
        int k = ksnprintf(info, 4096, "device %04x:%04x family %s, DCE %d, CRTC %d\nmode setting: %s (%s)\n",
                          g_gpu.pci.vendor, g_gpu.pci.device, rad_family_name(g_rad.family), g_dce, g_crtc_n,
                          g_kms ? "yes" : "no", g_kms_why);
        if (g_rad.atom) { rad_atom_info(&g_rad, info + k, 4096 - k); k += (int)strlen(info + k); }
        for (int i = 0; i < g_nmodes && k < 4000; i++)
            k += ksnprintf(info + k, (size_t)(4096 - k), "mode %dx%d@%u %u kHz\n", g_modes[i].hdisplay, g_modes[i].vdisplay,
                           g_modes[i].refresh, g_modes[i].clock);
        ksnprintf(path, sizeof(path), "%s/radeon-info.txt", dir);
        if (fs_write_path(path, info, (uint32_t)strlen(info)) >= 0) saved++;
        kfree(info);
    }
    ksnprintf(msg, (size_t)cap, "%d files in %s (radeon-vbios.rom, radeon-regs.bin, radeon-info.txt)", saved, dir);
    return saved ? 0 : -1;
}

/* the memory BAR the framebuffer is in: its base, or 0 */
static uintptr_t fb_bar(const pci_dev_t* d, uintptr_t fb) {
    uintptr_t best = 0;
    for (int b = 0; b < 6; b++) {
        int io = 0;
        uintptr_t base = pci_bar(d, b, &io);
        if (io || !base || base > fb) continue;
        if (!best || base > best) best = base;
    }
    return best;
}

static int try_card(const pci_dev_t* d) {
    const rad_family_t* f = NULL;
    for (unsigned i = 0; i < sizeof(FAM) / sizeof(FAM[0]); i++) if (d->device >= FAM[i].lo && d->device <= FAM[i].hi) f = &FAM[i];
    if (!f) { ksnprintf(g_why, sizeof(g_why), "%04x: not a Radeon HD 5000-7000 (TeraScale 2) or RX 400/500", d->device); return -1; }
    const fb_info_t* fi = fb_info();
    if (!fb_available() || !fi->addr) { kstrlcpy(g_why, "no firmware framebuffer", sizeof(g_why)); return -1; }
    /* the registers: BAR 2 (DCE 4/5), BAR 5 (DCE 11) */
    int io = 0;
    uintptr_t rb = pci_bar(d, f->dce == 11 ? 5 : 2, &io);
    if (!rb || io) { kstrlcpy(g_why, "no register BAR", sizeof(g_why)); return -1; }
    g_reg_size = 128u << 10;                       /* (what every CRTC's registers need) */
    if (!mmio_map(rb, g_reg_size)) { kstrlcpy(g_why, "the registers could not be mapped", sizeof(g_why)); return -1; }
    g_reg = (volatile uint8_t*)rb;
    g_dce = f->dce;
    /* the framebuffer as the display engine addresses it */
    uintptr_t vb = fb_bar(d, fi->addr);
    if (!vb) { kstrlcpy(g_why, "the framebuffer is in none of the card's BARs", sizeof(g_why)); return -1; }
    uint32_t loc = rd(MC_VM_FB_LOCATION);
    uint64_t vram_mc = (uint64_t)(loc & 0xFFFF) << 24;
    uint64_t want = vram_mc + (fi->addr - vb);
    const uint32_t* offs = f->dce == 11 ? DCE11_CRTC : DCE4_CRTC;
    int found = -1;
    for (int c = 0; c < 6 && found < 0; c++) {
        uint32_t o = offs[c];
        if (!(rd(CRTC_CONTROL + o) & 1) || !(rd(GRPH_ENABLE + o) & 1)) continue;
        uint64_t a = (uint64_t)rd(GRPH_PRIMARY_SURFACE_HIGH + o) << 32 | (rd(GRPH_PRIMARY_SURFACE_ADDR + o) & ~0xFFu);
        if (a == want) found = c;
    }
    if (found < 0) {
        ksnprintf(g_why, sizeof(g_why), "no CRTC scans out the framebuffer (MC 0x%llx) - left alone", (unsigned long long)want);
        return -1;
    }
    g_crtc_n = found;
    g_crtc = offs[found];
    /* the cursor's 16 KiB (64 KiB on DCE 8+), 64 KiB past the end of the framebuffer, in the same BAR */
    uint64_t off = ((uint64_t)(fi->addr - vb) + (uint64_t)fi->pitch * fi->height + 0x10000 + 0xFFF) & ~0xFFFull;
    g_pitch = f->dce >= 8 ? 128 : 64;
    if (off + CUR_BYTES > (256ull << 20)) { kstrlcpy(g_why, "no room for the cursor in the visible video memory", sizeof(g_why)); return -1; }
    if (!mmio_map(vb + (uintptr_t)off, CUR_BYTES)) { kstrlcpy(g_why, "the cursor's memory could not be mapped", sizeof(g_why)); return -1; }
    g_cur = (volatile uint32_t*)(vb + (uintptr_t)off);
    g_cur_mc = vram_mc + off;
    for (uint32_t i = 0; i < CUR_BYTES / 4; i++) g_cur[i] = 0;
    /* set it up, hidden */
    wr(CUR_UPDATE + g_crtc, rd(CUR_UPDATE + g_crtc) | CUR_UPDATE_LOCK);
    wr(CUR_CONTROL + g_crtc, 0);
    wr(CUR_SURF_HIGH + g_crtc, (uint32_t)(g_cur_mc >> 32));
    wr(CUR_SURFACE_ADDRESS + g_crtc, (uint32_t)g_cur_mc);
    wr(CUR_SIZE + g_crtc, 63u << 16 | 63u);
    wr(CUR_UPDATE + g_crtc, rd(CUR_UPDATE + g_crtc) & ~CUR_UPDATE_LOCK);
    pci_claim(d, "radeon");
    g_vb = vb;
    g_vram_mc = vram_mc;
    g_cur_w = (int)fi->width;
    g_cur_h = (int)fi->height;
    if (f->dce <= 5) kms_init(d);

    memset(&g_gpu, 0, sizeof(g_gpu));
    kstrlcpy(g_gpu.name, f->name, sizeof(g_gpu.name));
    g_gpu.driver = "radeon (experimental)";
    g_gpu.pci = *d;
    g_gpu.set_mode = rad_set_mode;
    g_gpu.modes = rad_modes;
    g_gpu.wait_vblank = rad_wait_vblank;
    g_gpu.cursor_image = rad_cursor_image;
    g_gpu.cursor_move = rad_cursor_move;
    g_gpu.cursor_irq_safe = 1;
    g_gpu.info = rad_info;
    if (gpu_register(&g_gpu) < 0) { kstrlcpy(g_why, "no room for another display driver", sizeof(g_why)); return -1; }
    ksnprintf(g_why, sizeof(g_why), "on: CRTC %d, DCE %d", found, f->dce);
    klog("radeon: %s - EXPERIMENTAL hardware cursor and vsync on CRTC %d; mode setting: %s\n", f->name, found,
         f->dce <= 5 ? g_kms_why : "not on this generation");
    return 0;
}

static int find_cb(const pci_dev_t* d, void* ctx) {
    if (d->vendor == 0x1002 && d->class_code == 0x03 && !g_active) {
        *(int*)ctx = 1;
        if (try_card(d) == 0) g_active = 1;
    }
    return 0;
}

int gpu_radeon_try(void) {
    if (g_active) return 0;
    int any = 0;
    pci_scan(find_cb, &any);
    if (!any) kstrlcpy(g_why, "no AMD Radeon card", sizeof(g_why));
    else if (!g_active) klog("radeon: %s\n", g_why);
    return g_active ? 0 : -1;
}

const char* gpu_radeon_status(void) { return g_why; }

int gpu_experimental(void) {
    char v[8];
    return settings_get("gpu_experimental", v, sizeof(v)) && !strcmp(v, "on");
}

void gpu_set_experimental(int on) { settings_set("gpu_experimental", on ? "on" : "off"); }

/* settings_boot: when it was asked for */
void gpu_radeon_boot(void) {
    if (gpu_experimental()) gpu_radeon_try();
}
