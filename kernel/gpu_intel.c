/* Intel integrated graphics: GMA 950 (945G / 945GM / 945GME, "gen 3"),
 * HD Graphics 2000 / 3000 (Sandy Bridge, gen 6) and HD Graphics 2500 /
 * 4000 (Ivy Bridge, gen 7). After the Intel graphics PRMs (Programmer's
 * Reference Manuals) and how Linux's i915 uses these registers.
 *
 * The firmware (VBIOS / GOP) has lit the panel and the outputs up; this
 * driver takes the display engine over from there:
 *  - it reads what is shown: the pipe and plane in use, the mode, the
 *    stride, the video memory (the "stolen" memory) size;
 *  - the mouse pointer in hardware: the cursor plane (64x64 ARGB), its
 *    image in memory the display reads (gen 3: a physical address; gen
 *    6/7: a page of ours mapped into the graphics translation table);
 *  - the vertical blank (the pipe's frame counter), so frames are copied
 *    to the screen when it is not being scanned out;
 *  - the backlight of a laptop panel (the PWM duty cycle).
 * The mode itself stays the one the firmware set (the panel's native
 * resolution): reprogramming the clocks and timings of real panels is
 * not done. */
#include "gpu.h"
#include "fb.h"
#include "kheap.h"
#include "kstring.h"
#include "serial.h"
#include "paging.h"

/* display registers (offsets in the MMIO BAR, the same on gen 3 to 7) */
#define PIPEACONF     0x70008
#define PIPEBCONF     0x71008
#define PIPEASRC      0x6001C
#define PIPEBSRC      0x6101C
#define HTOTAL_A      0x60000
#define VTOTAL_A      0x6000C
#define DSPACNTR      0x70180
#define DSPBCNTR      0x71180
#define DSPASTRIDE    0x70188
#define DSPBSTRIDE    0x71188
#define DSPASURF      0x7019C      /* gen 4+: the surface's graphics address */
#define DSPAADDR      0x70184      /* gen 3: the base (linear offset) */
#define CURACNTR      0x70080
#define CURABASE      0x70084
#define CURAPOS       0x70088
#define CURBCNTR      0x71080
#define FRMCOUNT_A    0x70040      /* gen 4+: frame counter; gen 3: PIPEAFRAMEHIGH */
#define FRAMEPIXEL_A  0x70044      /* gen 3: frame counter's low 8 bits in 31:24 */
#define PIPE_B_OFF    0x1000

/* backlight */
#define BLC_PWM_CTL       0x61254   /* gen 3: 31:17 period, 15:1 duty, 16 "legacy" (combination) mode */
#define BLC_PWM_CPU_CTL2  0x48250   /* gen 6/7: bit 31 PWM enable */
#define BLC_PWM_CPU_CTL   0x48254   /* gen 6/7: 15:0 duty */
#define BLC_PWM_PCH_CTL1  0xC8250   /* bit 31 PCH PWM enable */
#define BLC_PWM_PCH_CTL2  0xC8254   /* 31:16 period */
#define GFX_FLSH_CNTL     0x101008  /* gen 6/7: flushes the GTT's cache after PTE writes */

#define CUR_MODE_ARGB64   0x27      /* 64x64, 32-bit ARGB */

typedef struct { uint16_t id; uint8_t gen; const char* name; } intel_id_t;
static const intel_id_t IDS[] = {
    { 0x2772, 3, "Intel GMA 950 (945G)" },
    { 0x27A2, 3, "Intel GMA 950 (945GM)" },
    { 0x27AE, 3, "Intel GMA 950 (945GME)" },
    { 0x0102, 6, "Intel HD Graphics 2000 (Sandy Bridge)" },
    { 0x0106, 6, "Intel HD Graphics 2000 (Sandy Bridge mobile)" },
    { 0x010A, 6, "Intel HD Graphics P3000 (Sandy Bridge server)" },
    { 0x0112, 6, "Intel HD Graphics 3000 (Sandy Bridge)" },
    { 0x0116, 6, "Intel HD Graphics 3000 (Sandy Bridge mobile)" },
    { 0x0122, 6, "Intel HD Graphics 3000 (Sandy Bridge)" },
    { 0x0126, 6, "Intel HD Graphics 3000 (Sandy Bridge mobile)" },
    { 0x0152, 7, "Intel HD Graphics 2500 (Ivy Bridge)" },
    { 0x0156, 7, "Intel HD Graphics 2500 (Ivy Bridge mobile)" },
    { 0x015A, 7, "Intel HD Graphics P2500 (Ivy Bridge server)" },
    { 0x0162, 7, "Intel HD Graphics 4000 (Ivy Bridge)" },
    { 0x0166, 7, "Intel HD Graphics 4000 (Ivy Bridge mobile)" },
    { 0x016A, 7, "Intel HD Graphics P4000 (Ivy Bridge server)" },
};

static gpu_t g_gpu;
static volatile uint8_t* g_mmio;
static uint32_t g_mmio_size;
static int g_gen, g_pipe;                  /* the pipe that shows the screen: 0 = A, 1 = B */
static int g_w, g_h;
static uint32_t g_stolen, g_gtt_entries;
static uint32_t* g_cur;                    /* the cursor image: 64x64 ARGB */
static uint32_t g_cur_addr;                /* as the display sees it */
static int g_cur_ok, g_cur_on;
static int g_hot_x, g_hot_y;                /* the pointer image's hot spot */
static int g_blc;                          /* 0 none, 3: gen 3 BLC_PWM_CTL, 6: CPU + PCH PWM */
static uint32_t g_blc_max;

static uint32_t rd(uint32_t r) { return r + 4 <= g_mmio_size ? *(volatile uint32_t*)(g_mmio + r) : 0; }
static void wr(uint32_t r, uint32_t v) { if (r + 4 <= g_mmio_size) *(volatile uint32_t*)(g_mmio + r) = v; }
static uint32_t pr(uint32_t a_reg) { return rd(a_reg + (uint32_t)g_pipe * PIPE_B_OFF); }

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

static void it_wait_vblank(gpu_t* g) {
    (void)g;
    uint32_t f = frame_count();
    for (uint32_t i = 0; i < 3000000u && frame_count() == f; i++) __asm__ volatile("pause");
}

/* ── the hardware pointer ─────────────────────────────────────────── */
static int setup_cursor_memory(void) {
    uint8_t* raw = kmalloc(64 * 64 * 4 + 16384);
    if (!raw) return -1;
    g_cur = (uint32_t*)(((uintptr_t)raw + 16383) & ~(uintptr_t)16383);
    memset(g_cur, 0, 64 * 64 * 4);
    uintptr_t phys = (uintptr_t)g_cur;            /* (identity-mapped heap: virtual = physical) */
    if (g_gen == 3) {                             /* gen 3 reads the cursor at a physical address */
        if ((uint64_t)phys >> 32) return -1;
        g_cur_addr = (uint32_t)phys;
        return 0;
    }
    /* gen 6/7: four pages at the top of the graphics address space, mapped
     * to our memory (uncached: the display does not snoop the CPU's caches) */
    if (g_gtt_entries < 1024 || g_mmio_size < (2u << 20) + g_gtt_entries * 4u) return -1;
    volatile uint32_t* gtt = (volatile uint32_t*)(g_mmio + (2u << 20));
    uint32_t first = g_gtt_entries - 4;
    for (uint32_t i = 0; i < 4; i++) {
        uint64_t pa = (uint64_t)phys + i * 4096u;
        gtt[first + i] = (uint32_t)(pa & 0xFFFFF000u) | (uint32_t)((pa >> 28) & 0xFF0u) | 0x2u /* uncached */ | 0x1u /* valid */;
    }
    (void)gtt[first + 3];                        /* (posts the writes) */
    wr(GFX_FLSH_CNTL, 1);
    g_cur_addr = first * 4096u;
    return 0;
}

static void cursor_arm(int x, int y) {
    uint32_t pos = (y < 0 ? 0x80000000u | (uint32_t)(-y) << 16 : (uint32_t)y << 16) |
                   (x < 0 ? 0x8000u | (uint32_t)(-x) : (uint32_t)x);
    uint32_t base = CURACNTR + (g_gen >= 5 ? (uint32_t)g_pipe * PIPE_B_OFF : 0);
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
    uint32_t base = CURACNTR + (g_gen >= 5 ? (uint32_t)g_pipe * PIPE_B_OFF : 0);
    if (visible != g_cur_on) {
        uint32_t cntl = visible ? CUR_MODE_ARGB64 : 0;
        if (g_gen < 5 && visible) cntl |= (uint32_t)g_pipe << 28;     /* gen 3: the pipe it is on */
        wr(base, cntl);
        g_cur_on = visible;
    }
    cursor_arm(x, y);
}

/* ── the backlight ────────────────────────────────────────────────── */
static int it_backlight(gpu_t* g, int percent) {
    (void)g;
    if (!g_blc || !g_blc_max) return -1;
    uint32_t level;
    if (g_blc == 3) level = (rd(BLC_PWM_CTL) & 0xFFFE) >> 1;
    else level = rd(BLC_PWM_CPU_CTL) & 0xFFFF;
    if (percent < 0) return (int)(level * 100u / g_blc_max);
    if (percent > 100) percent = 100;
    if (percent < 5) percent = 5;                        /* (never quite off: the screen must stay readable) */
    level = (uint32_t)percent * g_blc_max / 100u;
    if (g_blc == 3) wr(BLC_PWM_CTL, (rd(BLC_PWM_CTL) & ~0xFFFFu) | (level << 1 & 0xFFFE));
    else wr(BLC_PWM_CPU_CTL, (rd(BLC_PWM_CPU_CTL) & ~0xFFFFu) | (level & 0xFFFF));
    return percent;
}

static void setup_backlight(void) {
    if (g_gen == 3) {
        uint32_t ctl = rd(BLC_PWM_CTL);
        if (ctl & (1u << 16)) return;                    /* combination mode (with PCI LBPC): not handled */
        g_blc_max = ctl >> 17;
        if (g_blc_max) g_blc = 3;
    } else {
        if (!(rd(BLC_PWM_CPU_CTL2) & 0x80000000u) || !(rd(BLC_PWM_PCH_CTL1) & 0x80000000u)) return;
        g_blc_max = rd(BLC_PWM_PCH_CTL2) >> 16;
        if (g_blc_max) g_blc = 6;
    }
}

static void it_info(gpu_t* g, char* buf, int cap) {
    (void)g;
    uint32_t ht = pr(HTOTAL_A), vt = pr(VTOTAL_A);
    ksnprintf(buf, (size_t)cap, "generation %d, pipe %c, plane control 0x%08x, stride %u\n"
              "timings: %u x %u visible of %u x %u total\n"
              "stolen memory: %u MiB, graphics translation table: %u entries",
              g_gen, 'A' + g_pipe, rd(g_pipe ? DSPBCNTR : DSPACNTR), rd(g_pipe ? DSPBSTRIDE : DSPASTRIDE),
              (ht & 0xFFF) + 1, (vt & 0xFFF) + 1, (ht >> 16 & 0x1FFF) + 1, (vt >> 16 & 0x1FFF) + 1,
              g_stolen >> 20, g_gtt_entries);
}

int gpu_intel_probe(const pci_dev_t* d) {
    if (d->vendor != 0x8086) return -1;
    const intel_id_t* id = NULL;
    for (unsigned i = 0; i < sizeof(IDS) / sizeof(IDS[0]); i++) if (IDS[i].id == d->device) id = &IDS[i];
    if (!id) return -1;
    g_gen = id->gen;
    int io = 0;
    uintptr_t mm = pci_bar(d, 0, &io);
    if (!mm || io) return -1;
    g_mmio_size = g_gen == 3 ? 512u << 10 : 4u << 20;   /* gen 3: 512 KiB of registers; gen 6/7: 2 MiB + the GTT */
    if (!mmio_map(mm, g_mmio_size)) return -1;
    g_mmio = (volatile uint8_t*)mm;
    pci_enable(d);

    /* which pipe shows something: the one whose plane is on */
    uint32_t ca = rd(DSPACNTR), cb = rd(DSPBCNTR);
    if (ca & 0x80000000u) g_pipe = g_gen == 3 ? (int)(ca >> 24 & 1) : 0;
    else if (cb & 0x80000000u) g_pipe = g_gen == 3 ? (int)(cb >> 24 & 1) : 1;
    else { klog("intel-gpu: %s, but no display plane is on - left alone\n", id->name); return -1; }
    uint32_t src = pr(PIPEASRC);
    g_w = (int)(src >> 16 & 0xFFF) + 1;
    g_h = (int)(src & 0xFFF) + 1;

    /* the stolen memory and the GTT size: the host bridge's graphics control */
    uint16_t ggc = pci_read16(0, 0, 0, g_gen == 3 ? 0x52 : 0x50);
    if (g_gen == 3) {
        static const uint32_t MB[8] = { 0, 1, 4, 8, 16, 32, 48, 64 };
        g_stolen = MB[ggc >> 4 & 7] << 20;
        g_gtt_entries = 0;
    } else {
        g_stolen = (uint32_t)(ggc >> 3 & 0x1F) * (32u << 20);
        uint32_t gtt_mb = ggc >> 8 & 3;                  /* 1 or 2 MiB of entries */
        g_gtt_entries = gtt_mb ? gtt_mb * (1u << 20) / 4u : 0;
    }

    const fb_info_t* fi = fb_info();
    if (!fb_available()) { klog("intel-gpu: %s without a firmware framebuffer - left alone\n", id->name); return -1; }
    if ((int)fi->width != g_w || (int)fi->height != g_h)
        klog("intel-gpu: the pipe shows %dx%d, the framebuffer is %ux%u\n", g_w, g_h, fi->width, fi->height);
    g_w = (int)fi->width;
    g_h = (int)fi->height;

    g_cur_ok = setup_cursor_memory() == 0;
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
    return gpu_register(&g_gpu);
}
