/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Intel integrated graphics, from the GMA 950 to the 8th generation Core
 * processors:
 *   gen 3    GMA 950 (945G / 945GM / 945GME)
 *   gen 5    HD Graphics (Ironlake: 1st generation Core, Clarkdale / Arrandale)
 *   gen 6    HD Graphics 2000 / 3000 (Sandy Bridge, 2nd generation)
 *   gen 7    HD Graphics 2500 / 4000 (Ivy Bridge, 3rd generation)
 *   gen 7.5  HD Graphics 4200 - 5200 (Haswell, 4th generation)
 *   gen 8    HD Graphics 5300 - 6300 (Broadwell, 5th generation)
 *   gen 9    HD / UHD / Iris Graphics 500 - 600 (Skylake, Kaby Lake, Coffee
 *            Lake, Whiskey Lake: 6th to 8th generation)
 * After the Intel graphics PRMs (Programmer's Reference Manuals) and how
 * Linux's i915 uses these registers.
 *
 * The firmware (VBIOS / GOP) has lit the panel and the outputs up; this
 * driver takes the display engine over from there:
 *  - it reads what is shown: the pipe and plane in use, the mode, the
 *    stride, the video memory (the "stolen" memory) size;
 *  - the mouse pointer in hardware: the cursor plane (64x64 ARGB), its
 *    image in memory the display reads (gen 3: a physical address; gen 5
 *    on: four pages of ours mapped at the top of the graphics translation
 *    table - 32-bit entries up to gen 7.5, 64-bit from gen 8). Gen 9
 *    shares the display's buffer (DDB) between the planes: the cursor is
 *    used only when the firmware gave it a share, else the pointer stays
 *    drawn by the CPU;
 *  - the vertical blank (the pipe's frame counter), so frames are copied
 *    to the screen when it is not being scanned out;
 *  - the backlight of a laptop panel (the PWM duty cycle: in the CPU on
 *    gen 5 - 7, in the PCH from Haswell, the Cannon Point PCH's way on
 *    300-series boards).
 * The mode itself stays the one the firmware set (the panel's native
 * resolution): reprogramming the clocks and timings of real panels is
 * not done. */
#include "gpu.h"
#include "timer.h"
#include "fb.h"
#include "kheap.h"
#include "kstring.h"
#include "serial.h"
#include "paging.h"
#include "gpu_i915.h"

/* display registers (offsets in the MMIO BAR, the same from gen 3 to gen 9) */
#define PIPEASRC      0x6001C
#define HTOTAL_A      0x60000
#define VTOTAL_A      0x6000C
#define DSPACNTR      0x70180      /* gen 9: PLANE_CTL_1_A (bit 31: on, the same) */
#define DSPASTRIDE    0x70188
#define DSPASURF      0x7019C      /* gen 4+: the plane's surface in the GTT */
#define CURACNTR      0x70080
#define FRMCOUNT_A    0x70040      /* gen 4+: frame counter; gen 3: PIPEAFRAMEHIGH */
#define FRAMEPIXEL_A  0x70044      /* gen 3: frame counter's low 8 bits in 31:24 */
#define CUR_WM_0_A    0x70140      /* gen 9: the cursor's level-0 watermark (bit 31: on) */
#define CUR_BUF_CFG_A 0x7017C      /* gen 9: the cursor's share of the DDB (25:16 end, 9:0 start) */
#define PIPE_OFF      0x1000       /* pipe B, C: + 0x1000, + 0x2000 */

/* backlight */
#define BLC_PWM_CTL       0x61254   /* gen 3: 31:17 period, 15:1 duty, 16 "legacy" (combination) mode */
#define BLC_PWM_CPU_CTL2  0x48250   /* gen 5-7: bit 31 PWM enable */
#define BLC_PWM_CPU_CTL   0x48254   /* gen 5-7: 15:0 duty; Cannon Point: the duty (32 bits) */
#define BLC_PWM_PCH_CTL1  0xC8250   /* bit 31 PCH PWM enable */
#define BLC_PWM_PCH_CTL2  0xC8254   /* 31:16 period (Haswell+: 15:0 duty); Cannon Point: the period (32 bits) */
#define GFX_FLSH_CNTL     0x101008  /* gen 6+: flushes the GTT's cache after PTE writes */

#define CUR_MODE_ARGB64   0x27      /* 64x64, 32-bit ARGB */

enum { BLC_NONE = 0, BLC_GEN3, BLC_CPU, BLC_LPT, BLC_CNP };

typedef struct { uint16_t id; uint8_t gen; const char* name; } intel_id_t;
#define HSW 75                      /* (gen 7.5) */
static const intel_id_t IDS[] = {
    { 0x2772, 3, "Intel GMA 950 (945G)" },
    { 0x27A2, 3, "Intel GMA 950 (945GM)" },
    { 0x27AE, 3, "Intel GMA 950 (945GME)" },
    /* 1st generation Core */
    { 0x0042, 5, "Intel HD Graphics (Ironlake, Clarkdale)" },
    { 0x0046, 5, "Intel HD Graphics (Ironlake, Arrandale)" },
    /* 2nd */
    { 0x0102, 6, "Intel HD Graphics 2000 (Sandy Bridge)" },
    { 0x0106, 6, "Intel HD Graphics 2000 (Sandy Bridge mobile)" },
    { 0x010A, 6, "Intel HD Graphics P3000 (Sandy Bridge server)" },
    { 0x0112, 6, "Intel HD Graphics 3000 (Sandy Bridge)" },
    { 0x0116, 6, "Intel HD Graphics 3000 (Sandy Bridge mobile)" },
    { 0x0122, 6, "Intel HD Graphics 3000 (Sandy Bridge)" },
    { 0x0126, 6, "Intel HD Graphics 3000 (Sandy Bridge mobile)" },
    /* 3rd */
    { 0x0152, 7, "Intel HD Graphics 2500 (Ivy Bridge)" },
    { 0x0156, 7, "Intel HD Graphics 2500 (Ivy Bridge mobile)" },
    { 0x015A, 7, "Intel HD Graphics P2500 (Ivy Bridge server)" },
    { 0x0162, 7, "Intel HD Graphics 4000 (Ivy Bridge)" },
    { 0x0166, 7, "Intel HD Graphics 4000 (Ivy Bridge mobile)" },
    { 0x016A, 7, "Intel HD Graphics P4000 (Ivy Bridge server)" },
    /* 4th */
    { 0x0402, HSW, "Intel HD Graphics (Haswell)" },
    { 0x0406, HSW, "Intel HD Graphics (Haswell mobile)" },
    { 0x0412, HSW, "Intel HD Graphics 4600 (Haswell)" },
    { 0x0416, HSW, "Intel HD Graphics 4600 (Haswell mobile)" },
    { 0x041A, HSW, "Intel HD Graphics P4600/P4700 (Haswell)" },
    { 0x041E, HSW, "Intel HD Graphics 4400 (Haswell)" },
    { 0x0422, HSW, "Intel HD Graphics 5000 (Haswell)" },
    { 0x0A06, HSW, "Intel HD Graphics (Haswell ULT)" },
    { 0x0A16, HSW, "Intel HD Graphics 4400 (Haswell ULT)" },
    { 0x0A1E, HSW, "Intel HD Graphics 4200 (Haswell ULT)" },
    { 0x0A26, HSW, "Intel HD Graphics 5000 (Haswell ULT)" },
    { 0x0A2E, HSW, "Intel Iris Graphics 5100 (Haswell)" },
    { 0x0D12, HSW, "Intel HD Graphics 4600 (Haswell)" },
    { 0x0D22, HSW, "Intel Iris Pro Graphics 5200 (Haswell)" },
    { 0x0D26, HSW, "Intel Iris Pro Graphics 5200 (Haswell mobile)" },
    /* 5th */
    { 0x1602, 8, "Intel HD Graphics (Broadwell)" },
    { 0x1606, 8, "Intel HD Graphics (Broadwell mobile)" },
    { 0x160E, 8, "Intel HD Graphics (Broadwell)" },
    { 0x1612, 8, "Intel HD Graphics 5600 (Broadwell)" },
    { 0x1616, 8, "Intel HD Graphics 5500 (Broadwell)" },
    { 0x161E, 8, "Intel HD Graphics 5300 (Broadwell)" },
    { 0x1622, 8, "Intel Iris Pro Graphics 6200 (Broadwell)" },
    { 0x1626, 8, "Intel HD Graphics 6000 (Broadwell)" },
    { 0x162A, 8, "Intel Iris Pro Graphics P6300 (Broadwell)" },
    { 0x162B, 8, "Intel Iris Graphics 6100 (Broadwell)" },
    { 0x162D, 8, "Intel Iris Pro Graphics P6300 (Broadwell)" },
    /* 6th: Skylake */
    { 0x1902, 9, "Intel HD Graphics 510 (Skylake)" },
    { 0x1906, 9, "Intel HD Graphics 510 (Skylake mobile)" },
    { 0x190B, 9, "Intel HD Graphics 510 (Skylake)" },
    { 0x1912, 9, "Intel HD Graphics 530 (Skylake)" },
    { 0x1916, 9, "Intel HD Graphics 520 (Skylake)" },
    { 0x191B, 9, "Intel HD Graphics 530 (Skylake mobile)" },
    { 0x191D, 9, "Intel HD Graphics P530 (Skylake)" },
    { 0x191E, 9, "Intel HD Graphics 515 (Skylake)" },
    { 0x1921, 9, "Intel HD Graphics 520 (Skylake)" },
    { 0x1926, 9, "Intel Iris Graphics 540 (Skylake)" },
    { 0x1927, 9, "Intel Iris Graphics 550 (Skylake)" },
    { 0x193B, 9, "Intel Iris Pro Graphics 580 (Skylake)" },
    /* 7th: Kaby Lake (and 8th: Kaby Lake R, Amber Lake, Whiskey Lake) */
    { 0x5902, 9, "Intel HD Graphics 610 (Kaby Lake)" },
    { 0x5906, 9, "Intel HD Graphics 610 (Kaby Lake mobile)" },
    { 0x5912, 9, "Intel HD Graphics 630 (Kaby Lake)" },
    { 0x5916, 9, "Intel HD Graphics 620 (Kaby Lake)" },
    { 0x5917, 9, "Intel UHD Graphics 620 (Kaby Lake R)" },
    { 0x591B, 9, "Intel HD Graphics 630 (Kaby Lake mobile)" },
    { 0x591C, 9, "Intel UHD Graphics 615 (Amber Lake)" },
    { 0x591D, 9, "Intel HD Graphics P630 (Kaby Lake)" },
    { 0x591E, 9, "Intel HD Graphics 615 (Kaby Lake)" },
    { 0x5926, 9, "Intel Iris Plus Graphics 640 (Kaby Lake)" },
    { 0x5927, 9, "Intel Iris Plus Graphics 650 (Kaby Lake)" },
    { 0x87C0, 9, "Intel UHD Graphics 617 (Amber Lake)" },
    { 0x3EA0, 9, "Intel UHD Graphics 620 (Whiskey Lake)" },
    /* 8th: Coffee Lake */
    { 0x3E90, 9, "Intel UHD Graphics 610 (Coffee Lake)" },
    { 0x3E91, 9, "Intel UHD Graphics 630 (Coffee Lake)" },
    { 0x3E92, 9, "Intel UHD Graphics 630 (Coffee Lake)" },
    { 0x3E93, 9, "Intel UHD Graphics 610 (Coffee Lake)" },
    { 0x3E94, 9, "Intel UHD Graphics P630 (Coffee Lake)" },
    { 0x3E96, 9, "Intel UHD Graphics P630 (Coffee Lake)" },
    { 0x3E98, 9, "Intel UHD Graphics 630 (Coffee Lake)" },
    { 0x3E9A, 9, "Intel UHD Graphics P630 (Coffee Lake)" },
    { 0x3E9B, 9, "Intel UHD Graphics 630 (Coffee Lake mobile)" },
    { 0x3EA5, 9, "Intel Iris Plus Graphics 655 (Coffee Lake)" },
    { 0x3EA6, 9, "Intel Iris Plus Graphics 645 (Coffee Lake)" },
    { 0x3EA8, 9, "Intel Iris Plus Graphics 655 (Coffee Lake)" },
    { 0x3EA9, 9, "Intel UHD Graphics 620 (Coffee Lake)" },
    /* 10th: Comet Lake (gen 9.5, as Coffee Lake) */
    { 0x9B21, 9, "Intel UHD Graphics (Comet Lake ULT)" },
    { 0x9B41, 9, "Intel UHD Graphics (Comet Lake ULT)" },
    { 0x9BA2, 9, "Intel UHD Graphics 610 (Comet Lake)" },
    { 0x9BA4, 9, "Intel UHD Graphics 610 (Comet Lake)" },
    { 0x9BA5, 9, "Intel UHD Graphics 610 (Comet Lake)" },
    { 0x9BA8, 9, "Intel UHD Graphics 610 (Comet Lake)" },
    { 0x9BAA, 9, "Intel UHD Graphics (Comet Lake ULT)" },
    { 0x9BAC, 9, "Intel UHD Graphics (Comet Lake ULT)" },
    { 0x9BC2, 9, "Intel UHD Graphics 630 (Comet Lake)" },
    { 0x9BC4, 9, "Intel UHD Graphics (Comet Lake mobile)" },
    { 0x9BC5, 9, "Intel UHD Graphics 630 (Comet Lake)" },
    { 0x9BC6, 9, "Intel UHD Graphics P630 (Comet Lake)" },
    { 0x9BC8, 9, "Intel UHD Graphics 630 (Comet Lake)" },
    { 0x9BCA, 9, "Intel UHD Graphics (Comet Lake ULT)" },
    { 0x9BCC, 9, "Intel UHD Graphics (Comet Lake ULT)" },
    { 0x9BE6, 9, "Intel UHD Graphics P630 (Comet Lake)" },
    { 0x9BF6, 9, "Intel UHD Graphics P630 (Comet Lake)" },
    /* 10th: Ice Lake (gen 11) */
    { 0x8A50, 11, "Intel Iris Plus Graphics (Ice Lake)" },
    { 0x8A51, 11, "Intel Iris Plus Graphics G7 (Ice Lake)" },
    { 0x8A52, 11, "Intel Iris Plus Graphics G7 (Ice Lake)" },
    { 0x8A53, 11, "Intel Iris Plus Graphics G7 (Ice Lake)" },
    { 0x8A54, 11, "Intel Iris Plus Graphics (Ice Lake)" },
    { 0x8A56, 11, "Intel UHD Graphics G1 (Ice Lake)" },
    { 0x8A57, 11, "Intel Iris Plus Graphics (Ice Lake)" },
    { 0x8A58, 11, "Intel UHD Graphics G1 (Ice Lake)" },
    { 0x8A59, 11, "Intel Iris Plus Graphics (Ice Lake)" },
    { 0x8A5A, 11, "Intel Iris Plus Graphics G4 (Ice Lake)" },
    { 0x8A5B, 11, "Intel Iris Plus Graphics (Ice Lake)" },
    { 0x8A5C, 11, "Intel Iris Plus Graphics G4 (Ice Lake)" },
    { 0x8A5D, 11, "Intel Iris Plus Graphics (Ice Lake)" },
    { 0x8A71, 11, "Intel Iris Plus Graphics (Ice Lake)" },
};

static gpu_t g_gpu;
static volatile uint8_t* g_mmio;
static uint32_t g_mmio_size;
static int g_gen, g_hsw, g_pipe, g_npipes;      /* the pipe that shows the screen: 0 = A, 1 = B, 2 = C */
static int g_w, g_h;
static uint32_t g_stolen, g_gtt_entries;
static uint32_t* g_cur;                    /* the cursor image: 64x64 ARGB */
static uint32_t g_cur_addr;                /* as the display sees it */
static int g_cur_ok, g_cur_on;
static const char* g_cur_why = "";         /* why there is none */
static int g_hot_x, g_hot_y;                /* the pointer image's hot spot */
static int g_blc;                          /* BLC_* */
static uint32_t g_blc_max;

static uint32_t rd(uint32_t r) { return r + 4 <= g_mmio_size ? *(volatile uint32_t*)(g_mmio + r) : 0; }
static void wr(uint32_t r, uint32_t v) { if (r + 4 <= g_mmio_size) *(volatile uint32_t*)(g_mmio + r) = v; }
static uint32_t pr(uint32_t a_reg) { return rd(a_reg + (uint32_t)g_pipe * PIPE_OFF); }

static void clflush_range(const void* p, uint32_t n) {
    for (uint32_t o = 0; o < n; o += 64) __asm__ volatile("clflush (%0)" :: "r"((const uint8_t*)p + o) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}

static int it_modes(gpu_t* g, display_mode_t* out, int max) {
    (void)g;
    if (max < 1) return 0;
    out[0] = (display_mode_t){ g_w, g_h };     /* the panel's own mode */
    return 1;
}

static int it_set_mode(gpu_t* g, int w, int h) {
    (void)g;
    return w == g_w && h == g_h ? 0 : -1;
}

/* the frame counter of the pipe in use */
static uint32_t frame_count(void) {
    if (g_gen >= 4) return pr(FRMCOUNT_A);
    return (pr(FRMCOUNT_A) & 0xFFFF) << 8 | pr(FRAMEPIXEL_A) >> 24;
}

/* vsync: asleep for most of the frame (kernel/gpu.c), not spinning through it */
static gpu_vblank_clock_t g_vclock;
static void it_wait_vblank(gpu_t* g) {
    (void)g;
    gpu_vblank_wait(&g_vclock, frame_count);
}

/* ── the hardware pointer ─────────────────────────────────────────── */

/* one page of ours in the graphics translation table, at entry i */
static void gtt_set(uint32_t i, uint64_t pa) {
    if (g_gen >= 8) {
        /* 64-bit entries at 8 MiB: the address, present (the caching: PAT index 0, as i915's) */
        volatile uint32_t* gtt = (volatile uint32_t*)(g_mmio + (8u << 20));
        gtt[i * 2] = (uint32_t)(pa & 0xFFFFF000u) | 1u;
        gtt[i * 2 + 1] = (uint32_t)(pa >> 32);
        (void)gtt[i * 2];
        return;
    }
    volatile uint32_t* gtt = (volatile uint32_t*)(g_mmio + (2u << 20));
    uint32_t e;
    if (g_gen == 5) e = (uint32_t)(pa & 0xFFFFF000u) | (uint32_t)((pa >> 28) & 0xF0u) | 0x1u;               /* address 35:32 in 7:4 */
    else if (g_hsw) e = (uint32_t)(pa & 0xFFFFF000u) | (uint32_t)((pa >> 28) & 0x7F0u) | 0x1u;           /* 38:32 in 10:4, uncached */
    else e = (uint32_t)(pa & 0xFFFFF000u) | (uint32_t)((pa >> 28) & 0xFF0u) | 0x2u /* uncached */ | 0x1u;  /* 39:32 in 11:4 */
    gtt[i] = e;
    (void)gtt[i];
}

static int setup_cursor_memory(void) {
    uint8_t* raw = kmalloc(64 * 64 * 4 + 16384);
    if (!raw) { g_cur_why = "no memory for its image"; return -1; }
    g_cur = (uint32_t*)(((uintptr_t)raw + 16383) & ~(uintptr_t)16383);
    memset(g_cur, 0, 64 * 64 * 4);
    uintptr_t phys = (uintptr_t)g_cur;            /* (identity-mapped heap: virtual = physical) */
    if (g_gen == 3) {                             /* gen 3 reads the cursor at a physical address */
        if ((uint64_t)phys >> 32) { g_cur_why = "its image above 4 GiB"; return -1; }
        g_cur_addr = (uint32_t)phys;
        return 0;
    }
    if (g_gen >= 9) {
        /* the display buffer: the cursor's share and its watermark, as the firmware set them */
        uint32_t ddb = pr(CUR_BUF_CFG_A), wm = pr(CUR_WM_0_A);
        if ((ddb >> 16 & 0x3FF) <= (ddb & 0x3FF) || !(wm & 0x80000000u)) {
            g_cur_why = "the firmware gave the cursor no display buffer";
            return -1;
        }
    }
    /* four pages at the top of the graphics address space, mapped to our
     * memory (the display does not snoop the CPU's caches: clflush) */
    uint32_t gtt_off = g_gen >= 8 ? 8u << 20 : 2u << 20, esz = g_gen >= 8 ? 8 : 4;
    if (g_gtt_entries < 1024 || g_mmio_size < gtt_off + g_gtt_entries * esz) { g_cur_why = "the translation table is out of reach"; return -1; }
    uint32_t first = g_gtt_entries - 4;
    for (uint32_t i = 0; i < 4; i++) gtt_set(first + i, (uint64_t)phys + i * 4096u);
    if (g_gen >= 6) wr(GFX_FLSH_CNTL, 1);
    g_cur_addr = first * 4096u;
    return 0;
}

static uint32_t cur_base(void) { return CURACNTR + (g_gen >= 5 ? (uint32_t)g_pipe * PIPE_OFF : 0); }

static void cursor_arm(int x, int y) {
    uint32_t pos = (y < 0 ? 0x80000000u | (uint32_t)(-y) << 16 : (uint32_t)y << 16) |
                   (x < 0 ? 0x8000u | (uint32_t)(-x) : (uint32_t)x);
    uint32_t base = cur_base();
    wr(base + 8, pos);                                   /* CURxPOS */
    wr(base + 4, g_cur_addr);                            /* CURxBASE: the update takes effect */
}

static int it_cursor_image(gpu_t* g, const uint32_t* argb, int w, int h, int hx, int hy) {
    (void)g;
    if (!g_cur_ok || w > 64 || h > 64) return -1;
    memset(g_cur, 0, 64 * 64 * 4);
    for (int y = 0; y < h; y++) memcpy(g_cur + y * 64, argb + y * w, (size_t)w * 4);
    clflush_range(g_cur, 64 * 64 * 4);
    g_hot_x = hx;
    g_hot_y = hy;
    return 0;
}

static void it_cursor_move(gpu_t* g, int x, int y, int visible) {
    (void)g;
    if (!g_cur_ok) return;
    x -= g_hot_x;
    y -= g_hot_y;
    if (visible != g_cur_on) {
        uint32_t cntl = visible ? CUR_MODE_ARGB64 : 0;
        if (g_gen < 5 && visible) cntl |= (uint32_t)g_pipe << 28;     /* gen 3: the pipe it is on */
        wr(cur_base(), cntl);
        g_cur_on = visible;
    }
    cursor_arm(x, y);
}

/* ── the backlight ────────────────────────────────────────────────── */
static uint32_t blc_level(void) {
    switch (g_blc) {
    case BLC_GEN3: return (rd(BLC_PWM_CTL) & 0xFFFE) >> 1;
    case BLC_CPU:  return rd(BLC_PWM_CPU_CTL) & 0xFFFF;
    case BLC_LPT:  return rd(BLC_PWM_PCH_CTL2) & 0xFFFF;
    case BLC_CNP:  return rd(BLC_PWM_CPU_CTL);
    }
    return 0;
}

static int it_backlight(gpu_t* g, int percent) {
    (void)g;
    if (!g_blc || !g_blc_max) return -1;
    if (percent < 0) return (int)((uint64_t)blc_level() * 100u / g_blc_max);
    if (percent > 100) percent = 100;
    if (percent < 5) percent = 5;                        /* (never quite off: the screen must stay readable) */
    uint32_t level = (uint32_t)((uint64_t)percent * g_blc_max / 100u);
    switch (g_blc) {
    case BLC_GEN3: wr(BLC_PWM_CTL, (rd(BLC_PWM_CTL) & ~0xFFFFu) | (level << 1 & 0xFFFE)); break;
    case BLC_CPU:  wr(BLC_PWM_CPU_CTL, (rd(BLC_PWM_CPU_CTL) & ~0xFFFFu) | (level & 0xFFFF)); break;
    case BLC_LPT:  wr(BLC_PWM_PCH_CTL2, (rd(BLC_PWM_PCH_CTL2) & ~0xFFFFu) | (level & 0xFFFF)); break;
    case BLC_CNP:  wr(BLC_PWM_CPU_CTL, level); break;
    }
    return percent;
}

/* the south bridge (PCH): Cannon Point (300 series) keeps the backlight its own way */
/* Cannon Point and the PCHs after it that set the backlight its way: Comet
 * Point (Comet Lake), Ice Point (Ice Lake) */
static int pch_is_cnp(void) {
    uint16_t id = pci_read16(0, 0x1F, 0, 2) & 0xFF80;
    return id == 0xA300 || id == 0x9D80 || id == 0x0280 || id == 0x0680 || id == 0xA380 || id == 0x3480 || id == 0x3880;
}

static void setup_backlight(void) {
    if (g_gen == 3) {
        uint32_t ctl = rd(BLC_PWM_CTL);
        if (ctl & (1u << 16)) return;                    /* combination mode (with PCI LBPC): not handled */
        g_blc_max = ctl >> 17;
        if (g_blc_max) g_blc = BLC_GEN3;
        return;
    }
    if (!(rd(BLC_PWM_PCH_CTL1) & 0x80000000u)) return;  /* the PCH's PWM is off: an external controller (eDP AUX...) */
    if (g_gen <= 7 && !g_hsw) {
        if (!(rd(BLC_PWM_CPU_CTL2) & 0x80000000u)) return;
        g_blc_max = rd(BLC_PWM_PCH_CTL2) >> 16;
        if (g_blc_max) g_blc = BLC_CPU;
    } else if (g_gen >= 9 && pch_is_cnp()) {
        g_blc_max = rd(BLC_PWM_PCH_CTL2);
        if (g_blc_max && rd(BLC_PWM_CPU_CTL) <= g_blc_max) g_blc = BLC_CNP;
    } else {
        g_blc_max = rd(BLC_PWM_PCH_CTL2) >> 16;
        if (g_blc_max) g_blc = BLC_LPT;
    }
}

static void it_info(gpu_t* g, char* buf, int cap) {
    (void)g;
    uint32_t ht = pr(HTOTAL_A), vt = pr(VTOTAL_A);
    static const char* const BLC[] = { "none", "PWM (gen 3)", "CPU PWM", "PCH PWM", "PCH PWM (Cannon Point)" };
    ksnprintf(buf, (size_t)cap, "generation %s, pipe %c of %d, plane control 0x%08x, stride %u\n"
              "timings: %u x %u visible of %u x %u total\n"
              "stolen memory: %u MiB, graphics translation table: %u entries\n"
              "hardware cursor: %s%s, backlight: %s",
              g_hsw ? "7.5" : g_gen == 3 ? "3" : g_gen == 5 ? "5" : g_gen == 6 ? "6" : g_gen == 7 ? "7" : g_gen == 8 ? "8" : g_gen == 9 ? "9" : "11",
              'A' + g_pipe, g_npipes, pr(DSPACNTR), pr(DSPASTRIDE),
              (ht & 0xFFF) + 1, (vt & 0xFFF) + 1, (ht >> 16 & 0x1FFF) + 1, (vt >> 16 & 0x1FFF) + 1,
              g_stolen >> 20, g_gtt_entries, g_cur_ok ? "yes" : "no - ", g_cur_ok ? "" : g_cur_why, BLC[g_blc]);
}

/* the stolen memory and the GTT size: the host bridge's graphics control */
static void read_ggc(void) {
    if (g_gen == 3) {
        static const uint32_t MB[8] = { 0, 1, 4, 8, 16, 32, 48, 64 };
        uint16_t ggc = pci_read16(0, 0, 0, 0x52);
        g_stolen = MB[ggc >> 4 & 7] << 20;
        g_gtt_entries = 0;
    } else if (g_gen == 5) {
        static const uint16_t MB[16] = { 0, 1, 4, 8, 16, 32, 48, 64, 128, 256, 96, 160, 224, 352, 0, 0 };
        uint16_t ggc = pci_read16(0, 0, 0, 0x52);
        g_stolen = (uint32_t)MB[ggc >> 4 & 0xF] << 20;
        uint32_t sz = (ggc >> 8 & 7) == 1 ? 1024 : (ggc >> 8 & 7) == 2 ? 1536 : (ggc >> 8 & 7) == 3 ? 2048 : 0;  /* KiB of entries */
        g_gtt_entries = sz * 1024u / 4u;
    } else if (g_gen <= 7) {
        uint16_t ggc = pci_read16(0, 0, 0, 0x50);
        g_stolen = (uint32_t)(ggc >> 3 & 0x1F) * (32u << 20);
        uint32_t gtt_mb = ggc >> 8 & 3;                  /* 1 or 2 MiB of entries */
        g_gtt_entries = gtt_mb ? gtt_mb * (1u << 20) / 4u : 0;
    } else {
        uint16_t ggc = pci_read16(0, 0, 0, 0x50);
        uint32_t gms = ggc >> 8 & 0xFF, ggms = ggc >> 6 & 3;
        if (g_gen >= 9 && gms >= 0xF0) g_stolen = (gms - 0xF0 + 1) * (4u << 20);
        else g_stolen = gms > 0x7F ? 0 : gms * (32u << 20);
        g_gtt_entries = ggms ? (1u << ggms) * (1u << 20) / 8u : 0;    /* 2, 4 or 8 MiB of 8-byte entries */
    }
}

int gpu_intel_probe(const pci_dev_t* d) {
    if (d->vendor != 0x8086) return -1;
    const intel_id_t* id = NULL;
    for (unsigned i = 0; i < sizeof(IDS) / sizeof(IDS[0]); i++) if (IDS[i].id == d->device) id = &IDS[i];
    if (!id) return -1;
    g_hsw = id->gen == HSW;
    g_gen = g_hsw ? 7 : id->gen;
    g_npipes = g_gen == 3 || g_gen == 5 || g_gen == 6 ? 2 : 3;
    int io = 0;
    uintptr_t mm = pci_bar(d, 0, &io);
    if (!mm || io) return -1;
    /* gen 3: 512 KiB of registers; gen 5 - 7.5: 2 MiB + the GTT (4 MiB); gen 8 - 11: 8 MiB + the GTT (16 MiB) */
    g_mmio_size = g_gen == 3 ? 512u << 10 : g_gen >= 8 ? 16u << 20 : 4u << 20;
    if (!mmio_map(mm, g_mmio_size)) return -1;
    g_mmio = (volatile uint8_t*)mm;
    pci_enable(d);

    /* which pipe shows something: the one whose plane is on */
    g_pipe = -1;
    for (int p = 0; p < g_npipes && g_pipe < 0; p++) {
        uint32_t c = rd(DSPACNTR + (uint32_t)p * PIPE_OFF);
        if (!(c & 0x80000000u)) continue;
        g_pipe = g_gen == 3 ? (int)(c >> 24 & 1) : p;    /* (gen 3: a plane says which pipe it feeds) */
    }
    if (g_pipe < 0) { klog("intel-gpu: %s, but no display plane is on - left alone\n", id->name); return -1; }
    uint32_t src = pr(PIPEASRC);
    g_w = (int)(src >> 16 & 0x1FFF) + 1;
    g_h = (int)(src & 0x1FFF) + 1;
    read_ggc();

    const fb_info_t* fi = fb_info();
    if (!fb_available()) { klog("intel-gpu: %s without a firmware framebuffer - left alone\n", id->name); return -1; }
    if ((int)fi->width != g_w || (int)fi->height != g_h)
        klog("intel-gpu: the pipe shows %dx%d, the framebuffer is %ux%u\n", g_w, g_h, fi->width, fi->height);
    g_w = (int)fi->width;
    g_h = (int)fi->height;
    /* the firmware's framebuffer: write-combining, as for the cards without a driver */
    if (fi->addr) paging_set_wc(fi->addr, (uint64_t)fi->pitch * fi->height);

    g_cur_ok = setup_cursor_memory() == 0;
    if (!g_cur_ok) klog("intel-gpu: no hardware cursor: %s\n", g_cur_why);
    setup_backlight();

    memset(&g_gpu, 0, sizeof(g_gpu));
    kstrlcpy(g_gpu.name, id->name, sizeof(g_gpu.name));
    g_gpu.driver = "intel";
    g_gpu.pci = *d;
    g_gpu.vram = g_stolen;
    g_gpu.modes = it_modes;
    g_gpu.set_mode = it_set_mode;
    g_gpu.wait_vblank = it_wait_vblank;
    if (g_cur_ok) { g_gpu.cursor_image = it_cursor_image; g_gpu.cursor_move = it_cursor_move; g_gpu.cursor_irq_safe = 1; }
    if (g_blc) g_gpu.backlight = it_backlight;
    g_gpu.info = it_info;
    if (g_gen >= 5) {
        /* 3D (kernel/gpu_i915.c): its own part of the GTT - not the firmware's
         * framebuffer (the stolen memory, and the surface shown), not the
         * cursor's pages at the top */
        i915_hw_t hw;
        memset(&hw, 0, sizeof(hw));
        hw.mmio = g_mmio;
        hw.mmio_size = g_mmio_size;
        hw.devid = d->device;
        hw.revision = (uint8_t)pci_read32(d->bus, d->dev, d->fn, 8);
        hw.gen = g_gen;
        hw.hsw = g_hsw;
        hw.gtt_entries = g_gtt_entries;
        uint64_t shown = (uint64_t)(pr(DSPASURF) & ~0xFFFu) + (uint64_t)pr(DSPASTRIDE) * (uint32_t)g_h;
        uint64_t lo = g_stolen;
        if (shown > lo) lo = shown;
        hw.reserved_lo = (uint32_t)((lo + 0xFFFFFu) >> 12) & ~0xFFu;      /* (a whole MiB) */
        hw.reserved_hi = 16;
        if (g_gtt_entries > hw.reserved_lo + hw.reserved_hi + 65536) i915_attach(&hw);
    }
    return gpu_register(&g_gpu);
}
