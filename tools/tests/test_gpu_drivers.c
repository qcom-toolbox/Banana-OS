/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Hosted run of the Intel (gen 5 - 9) and the experimental Radeon (DCE 4/5,
 * DCE 11) display drivers against fake register files laid out as the
 * hardware's: the pipe / CRTC they pick, the cursor registers and image,
 * the graphics translation table entries, the backlight. Each case runs in
 * a process of its own (the drivers keep their state in statics).
 *   cc -O1 -I kernel tools/tests/test_gpu_drivers.c kernel/gpu_intel.c kernel/gpu_radeon.c -o /tmp/gpudrv && /tmp/gpudrv */
#include "../../kernel/gpu.h"
#include "../../kernel/gpu_i915.h"
#include "../../kernel/fb.h"
#include "../../kernel/kstring.h"

extern int printf(const char*, ...);
extern int vsnprintf(char*, size_t, const char*, __builtin_va_list);
extern void* mmap(void*, size_t, int, int, int, long);
extern int fork(void);
extern int waitpid(int, int*, int);
extern void exit(int);
extern int fflush(void*);

/* ── the kernel's side, stubbed ── */
static uint8_t* g_arena; static size_t g_arena_used;
void* kmalloc(size_t n) { void* p = g_arena + g_arena_used; g_arena_used += (n + 4095) & ~(size_t)4095; return p; }
void* kzalloc(size_t n) { void* p = kmalloc(n); memset(p, 0, n); return p; }
void  kfree(void* p) { (void)p; }
int ksnprintf(char* b, size_t n, const char* f, ...) { __builtin_va_list a; __builtin_va_start(a, f); int r = vsnprintf(b, n, f, a); __builtin_va_end(a); return r; }
size_t kstrlcpy(char* d, const char* s, size_t n) { size_t l = strlen(s); if (n) { size_t k = l < n - 1 ? l : n - 1; memcpy(d, s, k); d[k] = 0; } return l; }
void klog(const char* f, ...) { char b[256]; __builtin_va_list a; __builtin_va_start(a, f); vsnprintf(b, sizeof(b), f, a); __builtin_va_end(a); printf("    klog: %s", b); }
static uint64_t g_ns;
uint64_t timer_counter(void) { return g_ns += 100000; }
int mmio_map(uint64_t phys, uint64_t size) { (void)phys; (void)size; return 1; }
int paging_set_wc(uint64_t phys, uint64_t size) { (void)phys; (void)size; return 0; }
static fb_info_t g_fb;
const fb_info_t* fb_info(void) { return &g_fb; }
int fb_available(void) { return 1; }
static uintptr_t g_bar[6];
uintptr_t pci_bar(const pci_dev_t* d, int bar, int* is_io) { (void)d; if (is_io) *is_io = 0; return bar >= 0 && bar < 6 ? g_bar[bar] : 0; }
static uint16_t g_ggc, g_pch;
uint16_t pci_read16(uint8_t b, uint8_t dv, uint8_t fn, uint8_t off) {
    (void)fn;
    if (b == 0 && dv == 0 && (off == 0x50 || off == 0x52)) return g_ggc;
    if (b == 0 && dv == 0x1F && off == 2) return g_pch;
    return 0;
}
/* Record the display-to-render handoff without starting a render engine. */
static i915_hw_t render_hw;
static int render_attached;
void i915_attach(const i915_hw_t* hw) { render_hw = *hw; render_attached++; }
uint32_t pci_read32(uint8_t b, uint8_t dv, uint8_t fn, uint8_t off) {
    (void)b; (void)dv; (void)fn;
    return off == 8 ? 9 : 0;
}
void pci_enable_from(const pci_dev_t* d, const char* f) { (void)d; (void)f; }
void pci_claim(const pci_dev_t* d, const char* drv) { (void)d; (void)drv; }
static pci_dev_t g_dev;
int pci_scan(int (*cb)(const pci_dev_t*, void*), void* ctx) { return cb(&g_dev, ctx); }
static gpu_t* g_reg_gpu;
int gpu_register(gpu_t* g) { g_reg_gpu = g; return 0; }
void gpu_note_vblank(int t) { (void)t; }
int settings_get(const char* k, char* v, int cap) { (void)k; kstrlcpy(v, "on", (size_t)cap); return 1; }
void settings_set(const char* k, const char* v) { (void)k; (void)v; }

/* ── checks ── */
static int g_fail;
static void check(const char* what, int ok, uint64_t got, uint64_t want) {
    if (ok) return;
    printf("    FAIL %s: 0x%llx, wanted 0x%llx\n", what, (unsigned long long)got, (unsigned long long)want);
    g_fail = 1;
}
#define EQ(what, got, want) do { \
    uint64_t actual_ = (uint64_t)(got), expected_ = (uint64_t)(want); \
    check(what, actual_ == expected_, actual_, expected_); \
} while (0)

static uint8_t* g_mm;                    /* the fake register BAR */
static uint32_t R(uint32_t r) { return *(volatile uint32_t*)(g_mm + r); }
static void W(uint32_t r, uint32_t v) { *(volatile uint32_t*)(g_mm + r) = v; }

static uint32_t g_img[32 * 32];
static void make_img(void) { for (int i = 0; i < 32 * 32; i++) g_img[i] = 0xFF000000u | (uint32_t)i * 7919u; }

/* ── Intel ── */
typedef struct {
    const char* name; uint16_t id; int gen, hsw, pipe; uint16_t ggc, pch;
    uint32_t gtt_entries; int ddb; int blc; uint32_t blc_max;   /* blc: 1 CPU, 2 LPT, 3 CNP */
} icase_t;

static void intel_case(const icase_t* c) {
    memset(g_mm, 0, 16u << 20);
    g_ggc = c->ggc; g_pch = c->pch;
    g_dev = (pci_dev_t){ 0, 2, 0, 0x8086, c->id, 3, 0, 0, 16 };
    g_bar[0] = (uintptr_t)g_mm;
    uint32_t po = (uint32_t)c->pipe * 0x1000;
    W(0x70180 + po, 0x80000000u);                         /* the plane on that pipe is on */
    W(0x6001C + po, 1919u << 16 | 1079u);
    W(0x70040 + po, 1234);
    if (c->ddb) { W(0x7017C + po, 900u << 16 | 892u); W(0x70140 + po, 0x80000000u | 8); }
    if (c->blc) W(0xC8250, 0x80000000u);
    if (c->blc == 1) { W(0x48250, 0x80000000u); W(0xC8254, c->blc_max << 16); W(0x48254, c->blc_max / 4); }
    if (c->blc == 2) W(0xC8254, c->blc_max << 16 | c->blc_max / 4);
    if (c->blc == 3) { W(0xC8254, c->blc_max); W(0x48254, c->blc_max / 4); }
    g_fb = (fb_info_t){ 0 };
    g_fb.addr = 0xE0000000u; g_fb.width = 1920; g_fb.height = 1080; g_fb.pitch = 7680; g_fb.bpp = 32;
    g_reg_gpu = NULL;
    EQ("probe", gpu_intel_probe(&g_dev), 0);
    if (!g_reg_gpu) { g_fail = 1; return; }
    gpu_t* g = g_reg_gpu;
    if (c->gen >= 5) {
        EQ("render handoff", render_attached, 1);
        EQ("render generation", render_hw.gen, c->gen);
        EQ("render device ID", render_hw.devid, c->id);
        EQ("render PCI revision", render_hw.revision, 9);
        EQ("render GTT size", render_hw.gtt_entries, c->gtt_entries);
        EQ("cursor pages reserved", render_hw.reserved_hi >= 4, 1);
    }
    char info[512];
    g->info(g, info, sizeof(info));
    printf("    %s\n", info);
    if (!c->ddb && c->gen == 9) {                       /* the firmware gave the cursor nothing: none */
        EQ("no cursor without a DDB share", g->cursor_image == NULL, 1);
    } else {
        EQ("cursor", g->cursor_image != NULL, 1);
        if (!g->cursor_image) return;
        EQ("cursor image", g->cursor_image(g, g_img, 32, 32, 3, 2), 0);
        g->cursor_move(g, 103, 52, 1);
        uint32_t cb = 0x70080 + po;
        EQ("CURCNTR", R(cb), 0x27);
        EQ("CURPOS", R(cb + 8), 50u << 16 | 100u);
        uint32_t first = c->gtt_entries - 4;
        EQ("CURBASE", R(cb + 4), first * 4096u);
        /* the translation table entry for the cursor's first page: back to an address */
        uint64_t pa;
        if (c->gen >= 8) {
            uint32_t lo = R((8u << 20) + first * 8), hi = R((8u << 20) + first * 8 + 4);
            EQ("PTE present", lo & 1, 1);
            pa = (uint64_t)hi << 32 | (lo & 0xFFFFF000u);
        } else {
            uint32_t e = R((2u << 20) + first * 4);
            EQ("PTE valid", e & 1, 1);
            if (c->gen == 5) pa = (e & 0xFFFFF000u) | (uint64_t)(e & 0xF0) << 28;
            else if (c->hsw) { pa = (e & 0xFFFFF000u) | (uint64_t)(e & 0x7F0) << 28; EQ("HSW uncached", e & 0x80E, 0); }
            else { pa = (e & 0xFFFFF000u) | (uint64_t)(e & 0xFF0) << 28; EQ("SNB uncached", e & 0x6, 2); }
        }
        EQ("the image behind the PTE", *(uint32_t*)(uintptr_t)pa, g_img[0]);
        EQ("its second row", ((uint32_t*)(uintptr_t)pa)[64], g_img[32]);
        g->cursor_move(g, 1, 2, 1);
        EQ("CURPOS off the left edge", R(cb + 8), 0x8000u | 2u);
        g->cursor_move(g, 0, 0, 0);
        EQ("CURCNTR hidden", R(cb), 0);
    }
    if (c->blc) {
        EQ("backlight", g->backlight != NULL, 1);
        if (!g->backlight) return;
        EQ("backlight read", g->backlight(g, -1), 25);
        EQ("backlight set", g->backlight(g, 50), 50);
        uint32_t duty = c->blc == 1 ? R(0x48254) & 0xFFFF : c->blc == 2 ? R(0xC8254) & 0xFFFF : R(0x48254);
        EQ("duty", duty, c->blc_max / 2);
        if (c->blc == 2) EQ("period kept", R(0xC8254) >> 16, c->blc_max);
    } else EQ("no backlight", g->backlight == NULL, 1);
    g->wait_vblank(g);                                   /* (a frozen counter: must come back) */
}

static const icase_t ICASES[] = {
    { "Ironlake (1st gen), pipe B", 0x0046, 5, 0, 1, 0x0350 /* GMS 5 = 32 MB, 2 MiB GTT */, 0x3B00, 2048u * 1024 / 4, 0, 1, 0x1000 },
    { "Sandy Bridge (2nd gen)", 0x0116, 6, 0, 0, 0x0208 /* 32 MB, 2 MiB */, 0x1C00, 2u * 1024 * 1024 / 4, 0, 1, 0x2000 },
    { "Ivy Bridge (3rd gen), pipe C", 0x0166, 7, 0, 2, 0x0208, 0x1E00, 2u * 1024 * 1024 / 4, 0, 0, 0 },
    { "Haswell (4th gen)", 0x0416, 7, 1, 0, 0x0208, 0x8C00, 2u * 1024 * 1024 / 4, 0, 2, 0x4000 },
    { "Broadwell (5th gen), pipe B", 0x1616, 8, 0, 1, 0x01C0 /* GMS 1, GGMS 3 = 8 MiB */, 0x9C00, 8u * 1024 * 1024 / 8, 0, 2, 0x8000 },
    { "Skylake (6th gen)", 0x1916, 9, 0, 0, 0x0280 /* GGMS 2 = 4 MiB */, 0x9D40, 4u * 1024 * 1024 / 8, 1, 2, 0x5000 },
    { "Kaby Lake R (8th gen)", 0x5917, 9, 0, 0, 0x01C0, 0x9D4E, 8u * 1024 * 1024 / 8, 1, 2, 0x6000 },
    { "Coffee Lake (8th gen), Cannon Point PCH", 0x3E92, 9, 0, 0, 0x01C0, 0xA305, 8u * 1024 * 1024 / 8, 1, 3, 0x30000 },
    { "Coffee Lake, cursor without a DDB share", 0x3E92, 9, 0, 0, 0x01C0, 0xA305, 8u * 1024 * 1024 / 8, 0, 0, 0 },
};

/* ── Radeon ── */
typedef struct { const char* name; uint16_t id; int reg_bar; uint32_t crtc_off; int crtc; int match; } rcase_t;
static uint8_t* g_vram;

static void radeon_case(const rcase_t* c) {
    memset(g_mm, 0, 1u << 20);
    memset(g_vram, 0xA5, 32u << 20);                 /* (old pixels: the cursor must cover them all) */
    g_dev = (pci_dev_t){ 1, 0, 0, 0x1002, c->id, 3, 0, 0, 11 };
    for (int i = 0; i < 6; i++) g_bar[i] = 0;
    g_bar[0] = (uintptr_t)g_vram;
    g_bar[c->reg_bar] = (uintptr_t)g_mm;
    uint32_t fboff = 0x100000;
    g_fb = (fb_info_t){ 0 };
    g_fb.addr = (uintptr_t)g_vram + fboff; g_fb.width = 1280; g_fb.height = 1024; g_fb.pitch = 5120; g_fb.bpp = 32;
    W(0x2024, 0x00F4u << 16 | 0x00F4);                   /* the video memory at MC 0xF4000000 */
    uint64_t mc = 0xF4000000ull;
    /* CRTC 0 on, but showing something else; the chosen one shows the framebuffer */
    W(0x6E70, 1); W(0x6800, 1); W(0x6810, (uint32_t)(mc + 0x2000000));
    W(0x6E70 + c->crtc_off, 1); W(0x6800 + c->crtc_off, 1);
    W(0x6810 + c->crtc_off, c->match ? (uint32_t)(mc + fboff) : (uint32_t)(mc + 0x1000));
    g_reg_gpu = NULL;
    int r = gpu_radeon_try();
    printf("    %s\n", gpu_radeon_status());
    if (!c->match) {
        EQ("left alone", r, -1);
        EQ("no cursor written", R(0x699C + c->crtc_off) | R(0x6998 + c->crtc_off), 0);
        return;
    }
    EQ("probe", r, 0);
    if (!g_reg_gpu) { g_fail = 1; return; }
    gpu_t* g = g_reg_gpu;
    uint32_t o = c->crtc_off;
    uint64_t off = ((uint64_t)fboff + 5120u * 1024u + 0x10000 + 0xFFF) & ~0xFFFull;
    EQ("CUR_SURFACE_ADDRESS", R(0x699C + o), (uint32_t)(mc + off));
    EQ("CUR_SURFACE_ADDRESS_HIGH", R(0x69A4 + o), 0);
    EQ("CUR_SIZE", R(0x69A0 + o), 63u << 16 | 63u);
    EQ("cursor image", g->cursor_image(g, g_img, 32, 32, 1, 1), 0);
    EQ("the image in video memory", *(uint32_t*)(g_vram + off), g_img[0]);
    uint32_t row = c->crtc_off == 0x9800 && c->reg_bar == 5 ? 512 : 256;    /* (Polaris: 128-pixel rows) */
    EQ("its second row", *(uint32_t*)(g_vram + off + row), g_img[32]);
    EQ("transparent right of the image", *(uint32_t*)(g_vram + off + 32 * 4), 0);
    EQ("transparent under the image", *(uint32_t*)(g_vram + off + 40 * row), 0);
    g->cursor_move(g, 11, 21, 1);
    EQ("CUR_CONTROL", R(0x6998 + o), 1u | 3u << 8 | 4u << 24);
    EQ("CUR_POSITION", R(0x69A8 + o), 10u << 16 | 20u);
    EQ("CUR_HOT_SPOT", R(0x69AC + o), 0);
    EQ("unlocked", R(0x69B8 + o) & (1u << 16), 0);
    g->cursor_move(g, -4, -2, 1);
    EQ("CUR_POSITION at the edge", R(0x69A8 + o), 0);
    EQ("CUR_HOT_SPOT at the edge", R(0x69AC + o), 5u << 16 | 3u);
    EQ("CRTC 0 untouched", R(0x6998), 0);
    g->wait_vblank(g);
}

static const rcase_t RCASES[] = {
    { "Radeon HD 7450 (Caicos, DCE 5), CRTC 1", 0x677B, 2, 0xC00, 1, 1 },
    { "Radeon HD 7450, the framebuffer on no CRTC", 0x677B, 2, 0xC00, 1, 0 },
    { "Radeon RX 580 (Polaris 10, DCE 11.2), CRTC 3", 0x67DF, 5, 0x9800, 3, 1 },
};

static void run(const char* name, void (*fn)(const void*), const void* c, int* failed) {
    printf("%s\n", name);
    fflush(0);
    int pid = fork();
    if (pid == 0) { fn(c); exit(g_fail); }
    int st = 0;
    waitpid(pid, &st, 0);
    int ok = st == 0;
    printf("  %s\n", ok ? "ok" : "FAIL");
    if (!ok) (*failed)++;
}

int main(void) {
    make_img();
    /* (below 4 GiB, as the kernel's heap: the drivers hand its addresses to the hardware) */
    g_arena = mmap((void*)0x10000000, 64u << 20, 3, 0x22 | 0x100000, -1, 0);
    g_mm = mmap((void*)0x20000000, 16u << 20, 3, 0x22 | 0x100000, -1, 0);
    g_vram = mmap((void*)0x30000000, 32u << 20, 3, 0x22 | 0x100000, -1, 0);
    if ((long)g_arena == -1 || (long)g_mm == -1 || (long)g_vram == -1) { printf("mmap failed\n"); return 2; }
    int failed = 0;
    for (unsigned i = 0; i < sizeof(ICASES) / sizeof(ICASES[0]); i++) run(ICASES[i].name, (void (*)(const void*))intel_case, &ICASES[i], &failed);
    for (unsigned i = 0; i < sizeof(RCASES) / sizeof(RCASES[0]); i++) run(RCASES[i].name, (void (*)(const void*))radeon_case, &RCASES[i], &failed);
    printf("%s: %d failed\n", failed ? "FAIL" : "ok", failed);
    return failed ? 1 : 0;
}
