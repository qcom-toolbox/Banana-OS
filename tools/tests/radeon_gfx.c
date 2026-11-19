/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Radeon HD 7450's 3D engine bring-up (kernel/radeon_gfx.c, with
 * Linux's evergreen_gfx.c / r600_gfx.c / ni_gfx.c) on a model of the card:
 * its registers, its video memory, and a command processor that runs what
 * the ring holds - type 0 / 2 / 3 packets, SET_CONFIG_REG, MEM_WRITE,
 * EVENT_WRITE_EOP, INDIRECT_BUFFER fetched through the GART's page table
 * as the memory controller would walk it.
 *   radeon_gfx ok | dead | gddr5 | cedar | real2g | fake2g | juniper | wideap
 * fake2g: a BIOS that says 2 GiB on 1 GiB of memory, which repeats past its end
 * wideap: a firmware leaving the system aperture over everything (the GTT too) */
#define RADEON_MODEL 1

#include "../../kernel/types.h"
#ifndef MODEL_ARENA
#define MODEL_ARENA (8u << 20)                   /* (the kernel's pages: the IB pool, the GTT buffers) */
#endif
static unsigned char g_arena[MODEL_ARENA] __attribute__((aligned(4096)));
static size_t g_arena_used;
#define ARENA_PHYS 0x20000000ull
#define RAD_PHYS(p) ((uint64_t)((uintptr_t)(p) - (uintptr_t)g_arena) + ARENA_PHYS)
#include "../../kernel/radeon_gfx.c"
#define RAD_VIRT(a) ((void*)(g_arena + ((uint64_t)(a) - ARENA_PHYS)))
#include "../../kernel/radeon_drm.c"
extern int printf(const char*, ...);
extern int vprintf(const char*, __builtin_va_list);
extern void* calloc(size_t, size_t);
extern void free(void*);
extern void exit(int);
/* (the Linux files are their own units, as in the kernel: their headers disagree) */
#define MC_SEQ_SUP_CNTL           0x28c8
#define RUN_MASK                  (1 << 0)
#define MC_SEQ_SUP_PGM            0x28cc
#define MC_IO_PAD_CNTL_D0         0x29d0
#define MEM_FALL_OUT_CMD          (1 << 8)
#define MC_SEQ_MISC0              0x2a00
#define MC_SEQ_MISC0_GDDR5_SHIFT  28
#define MC_SEQ_MISC0_GDDR5_VALUE  5

/* the microcode, as the kernel has it */
__asm__(".section .rodata\n.balign 4\n"
        ".global rad_fw_caicos_pfp\nrad_fw_caicos_pfp:\n.incbin \"firmware/amd/CAICOS_pfp.bin\"\n.global rad_fw_caicos_pfp_end\nrad_fw_caicos_pfp_end:\n"
        ".balign 4\n.global rad_fw_turks_pfp\nrad_fw_turks_pfp:\n.incbin \"firmware/amd/TURKS_pfp.bin\"\n.global rad_fw_turks_pfp_end\nrad_fw_turks_pfp_end:\n"
        ".balign 4\n.global rad_fw_barts_pfp\nrad_fw_barts_pfp:\n.incbin \"firmware/amd/BARTS_pfp.bin\"\n.global rad_fw_barts_pfp_end\nrad_fw_barts_pfp_end:\n"
        ".balign 4\n.global rad_fw_caicos_me\nrad_fw_caicos_me:\n.incbin \"firmware/amd/CAICOS_me.bin\"\n.global rad_fw_caicos_me_end\nrad_fw_caicos_me_end:\n"
        ".balign 4\n.global rad_fw_btc_rlc\nrad_fw_btc_rlc:\n.incbin \"firmware/amd/BTC_rlc.bin\"\n.global rad_fw_btc_rlc_end\nrad_fw_btc_rlc_end:\n"
        ".balign 4\n.global rad_fw_caicos_mc\nrad_fw_caicos_mc:\n.incbin \"firmware/amd/CAICOS_mc.bin\"\n.global rad_fw_caicos_mc_end\nrad_fw_caicos_mc_end:\n"
        ".balign 4\n.global rad_fw_cedar_pfp\nrad_fw_cedar_pfp:\n.incbin \"firmware/amd/CEDAR_pfp.bin\"\n.global rad_fw_cedar_pfp_end\nrad_fw_cedar_pfp_end:\n"
        ".balign 4\n.global rad_fw_cedar_me\nrad_fw_cedar_me:\n.incbin \"firmware/amd/CEDAR_me.bin\"\n.global rad_fw_cedar_me_end\nrad_fw_cedar_me_end:\n"
        ".balign 4\n.global rad_fw_cedar_rlc\nrad_fw_cedar_rlc:\n.incbin \"firmware/amd/CEDAR_rlc.bin\"\n.global rad_fw_cedar_rlc_end\nrad_fw_cedar_rlc_end:\n"
        ".balign 4\n.global rad_fw_licence\nrad_fw_licence:\n.incbin \"firmware/amd/LICENSE.radeon\"\n.global rad_fw_licence_end\nrad_fw_licence_end:\n.byte 0\n"
        ".previous\n");

/* ---- the kernel's services ---- */
static uint64_t g_ns;
void* kmalloc(size_t n) { return calloc(1, n ? n : 1); }
void* kzalloc(size_t n) { return calloc(1, n ? n : 1); }
void* kmalloc_aligned(size_t n, size_t a) {
    size_t at = (g_arena_used + a - 1) & ~(a - 1);
    if (at + n > sizeof(g_arena)) return NULL;
    g_arena_used = at + n;
    return g_arena + at;
}
void kfree(void* p) { if ((uint8_t*)p < g_arena || (uint8_t*)p >= g_arena + sizeof(g_arena)) free(p); }
static int g_quiet;
void klog(const char* fmt, ...) {
    if (g_quiet) return;
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    printf("  | ");
    vprintf(fmt, ap);
    __builtin_va_end(ap);
}
uint64_t timer_counter(void) { return g_ns += 1000; }
uint32_t timer_ms(void) { return (uint32_t)(g_ns / 1000000u); }
void timer_sleep_ms(uint32_t ms) { g_ns += (uint64_t)ms * 1000000u; }
uint32_t pci_read32(uint8_t b, uint8_t d, uint8_t f, uint8_t o) { (void)b; (void)d; (void)f; (void)o; return 0; }
static uint16_t g_pcicmd = 0x0003;                 /* (memory and I/O decoding on, bus mastering off: as UEFI leaves a GPU) */
uint16_t pci_read16(uint8_t b, uint8_t d, uint8_t f, uint8_t o) { (void)b; (void)d; (void)f; return o == 4 ? g_pcicmd : 0; }
void pci_write16(uint8_t b, uint8_t d, uint8_t f, uint8_t o, uint16_t v) { (void)b; (void)d; (void)f; if (o == 4) g_pcicmd = v; }
int mmio_map(uint64_t p, uint64_t n) { (void)p; (void)n; return 1; }
/* (kernel/radeon_drm.c's: the app's memory is the test's, pages from the arena) */
int app_user_ok(const void* p, uint64_t n) { (void)p; (void)n; return 1; }
uspace_t* app_user_space(void) { return (uspace_t*)1; }
uintptr_t uvm_map_frames(uspace_t* us, const uint64_t* frames, uint32_t n, int prot, int wc) {
    (void)us; (void)n; (void)prot; (void)wc;
    if (frames[0] >= ARENA_PHYS && frames[0] < ARENA_PHYS + sizeof(g_arena)) return (uintptr_t)RAD_VIRT(frames[0]);
    return (uintptr_t)frames[0];                       /* (video memory: the model's, through its "BAR") */
}
int uvm_unmap_frames(uspace_t* us, uintptr_t a, uint32_t n, uint64_t f0) { (void)us; (void)a; (void)n; (void)f0; return 0; }
void* upage_alloc(void) { return kmalloc_aligned(4096, 4096); }
void upage_free(void* p) { (void)p; }
void task_yield(void) { g_ns += 100000; }
static radeon_t rt;
struct radeon* gpu_radeon_card(void) { return &rt; }
uint64_t gpu_radeon_display_end(void) { return 64ull << 20; }
int gpu_radeon_3d(char* msg, int cap) { kstrlcpy(msg, "model: not started", (size_t)cap); return -1; }
const char* rad_family_name(int f) { return f == CHIP_CAICOS ? "Caicos" : f == CHIP_CEDAR ? "Cedar" : f == CHIP_JUNIPER ? "Juniper" : f == CHIP_TURKS ? "Turks" : f == CHIP_BARTS ? "Barts" : "?"; }

/* ---- the card ---- */
static uint32_t VRAM_MB = 1024, REAL_MB = 1024;
static uint32_t g_sink[2];
#define VISIBLE (128ull << 20)                  /* (a card showing 128 MiB through its BAR: the area in its last MiB) */
#define VRAM_HOST  (130u << 20)                  /* (what the driver touches: the first 129 MiB) */
static uint32_t R[(128u << 10) / 4];
static uint8_t* VR;
static int g_cp_alive = 1, g_gddr5, g_cedar;
static const uint8_t *FW_PFP, *FW_ME, *FW_RLC;
static unsigned g_rptr;
static uint32_t g_pfp_tot, g_me_tot, g_pfp_n, g_me_n, g_rlc_n, g_mc_n, g_bad_pfp, g_bad_me, g_bad_rlc, g_bad_mc;
static uint32_t g_pkt_cfg, g_pkt_eop, g_pkt_ib, g_pkt_mem, g_pkt_init, g_bad_addr, g_bad_pkt;

static uint32_t fw_dw(const uint8_t* fw, uint32_t i) { const uint8_t* p = fw + 4 * i; return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* a memory controller address, as the card resolves it: video memory, or the GTT through the page table */
static void* mc(uint64_t a, uint32_t bytes) {
    uint64_t vend = (uint64_t)((R[MC_VM_FB_LOCATION / 4] >> 16) + 1) << 24;
    /* the system aperture: taken as video memory as it is (not through the GART) */
    uint32_t aplo = R[MC_VM_SYSTEM_APERTURE_LOW_ADDR / 4], aphi = R[MC_VM_SYSTEM_APERTURE_HIGH_ADDR / 4];
    if (aphi && a >= ((uint64_t)aplo << 12) && a <= (((uint64_t)aphi << 12) | 0xFFF) && a >= vend) {
        g_bad_addr++;                                            /* (raw "video memory" past its end: junk) */
        return NULL;
    }
    if (a < vend) {
        a %= (uint64_t)REAL_MB << 20;                            /* (the chips there: past them it repeats) */
        if (a >= VRAM_HOST && bytes <= sizeof(g_sink)) return g_sink;   /* (beyond what the model keeps) */
        if (a + bytes > VRAM_HOST) { printf("  model: VRAM 0x%llx out of reach\n", (unsigned long long)a); g_bad_addr++; return NULL; }
        return VR + a;
    }
    uint64_t gs = (uint64_t)R[VM_CONTEXT0_PAGE_TABLE_START_ADDR / 4] << 12, ge = (uint64_t)R[VM_CONTEXT0_PAGE_TABLE_END_ADDR / 4] << 12;
    if (!(R[VM_CONTEXT0_CNTL / 4] & 1) || a < gs || a > ge + 0xFFF) { g_bad_addr++; return NULL; }
    if (!(g_pcicmd & 4)) { g_bad_addr++; return NULL; }          /* (no bus mastering: the GPU cannot reach system memory) */
    uint64_t* pt = mc((uint64_t)R[VM_CONTEXT0_PAGE_TABLE_BASE_ADDR / 4] << 12, 8);
    if (!pt) return NULL;
    uint64_t pte = pt[(a - gs) >> 12];
    if ((pte & 3) != 3) { if (!g_quiet) printf("  model: GTT 0x%llx unmapped\n", (unsigned long long)a); g_bad_addr++; return NULL; }            /* (valid, system) */
    uint64_t pa = (pte & 0xFFFFFFFFFF000ull) | (a & 0xFFF);
    if (pa < ARENA_PHYS || pa + bytes > ARENA_PHYS + sizeof(g_arena)) { g_bad_addr++; return NULL; }
    return g_arena + (pa - ARENA_PHYS);
}

static void reg_write(uint32_t reg, uint32_t v);
static void run(const uint32_t* (*at)(uint32_t i, void* c), void* c, uint32_t start, uint32_t end, int depth);

static const uint32_t* ring_at(uint32_t i, void* c) {
    (void)c;
    uint32_t n = 2u << (R[CP_RB_CNTL / 4] & 0x3F);
    uint32_t* ring = mc((uint64_t)R[CP_RB_BASE / 4] << 8, n * 4);
    return ring ? &ring[i & (n - 1)] : NULL;
}
static const uint32_t* ib_at(uint32_t i, void* c) { return mc(*(uint64_t*)c + 4ull * i, 4); }

static void run(const uint32_t* (*at)(uint32_t i, void* c), void* c, uint32_t i, uint32_t end, int depth) {
    while (i != end) {
        const uint32_t* hp = at(i, c);
        if (!hp) return;
        uint32_t h = *hp, type = h >> 30, n = ((h >> 16) & 0x3FFF) + 1;
        uint32_t p[256];
        if (type == 2) { i++; continue; }
        if (type == 1 || n > 256) { printf("  model: packet %08x\n", h); g_bad_pkt++; return; }
        for (uint32_t k = 0; k < n; k++) { const uint32_t* q = at(i + 1 + k, c); if (!q) return; p[k] = *q; }
        i += 1 + n;
        if (type == 0) { for (uint32_t k = 0; k < n; k++) reg_write(((h & 0xFFFF) << 2) + 4 * k, p[k]); continue; }
        switch ((h >> 8) & 0xFF) {
        case PACKET3_SET_CONFIG_REG:
            g_pkt_cfg++;
            for (uint32_t k = 1; k < n; k++) reg_write(PACKET3_SET_CONFIG_REG_START + 4 * (p[0] + k - 1), p[k]);
            break;
        case PACKET3_MEM_WRITE: {
            g_pkt_mem++;
            uint32_t* d = mc(p[0] | (uint64_t)(p[1] & 0xFF) << 32, 4);
            if (d) *d = p[2];
            break;
        }
        case PACKET3_EVENT_WRITE_EOP: {
            g_pkt_eop++;
            uint32_t sel = (p[2] >> 29) & 7;
            uint32_t* d = mc(p[1] | (uint64_t)(p[2] & 0xFF) << 32, 8);
            if (d && sel >= 1) d[0] = p[3];
            if (d && sel == 2) d[1] = p[4];
            break;
        }
        case PACKET3_INDIRECT_BUFFER: {
            g_pkt_ib++;
            if (depth) { g_bad_pkt++; break; }
            uint64_t base = (p[0] & ~3u) | (uint64_t)(p[1] & 0xFF) << 32;
            run(ib_at, &base, 0, p[2], 1);
            break;
        }
        case PACKET3_ME_INITIALIZE: g_pkt_init++; break;
        default: break;
        }
    }
}

static void cp_kick(void) {
    if (!g_cp_alive || (R[CP_ME_CNTL / 4] & (CP_ME_HALT | CP_PFP_HALT)) || !R[CP_RB_BASE / 4]) return;
    uint32_t n = 2u << (R[CP_RB_CNTL / 4] & 0x3F);
    uint32_t w = R[CP_RB_WPTR / 4] & (n - 1);
    run(ring_at, NULL, g_rptr, w, 0);
    g_rptr = w;
}

static void reg_write(uint32_t reg, uint32_t v) {
    if (reg + 4 > sizeof(R)) return;
    switch (reg) {
    case CP_PFP_UCODE_ADDR: g_pfp_n = v; break;
    case CP_PFP_UCODE_DATA: if (g_pfp_n < EVERGREEN_PFP_UCODE_SIZE && v != fw_dw(FW_PFP, g_pfp_n)) g_bad_pfp++; g_pfp_n++; g_pfp_tot++; return;
    case CP_ME_RAM_WADDR: g_me_n = v; break;
    case CP_ME_RAM_DATA: if (g_me_n < EVERGREEN_PM4_UCODE_SIZE && v != fw_dw(FW_ME, g_me_n)) g_bad_me++; g_me_n++; g_me_tot++; return;
    case RLC_UCODE_DATA: if (v != fw_dw(FW_RLC, R[RLC_UCODE_ADDR / 4])) g_bad_rlc++; g_rlc_n++; return;
    case MC_SEQ_SUP_PGM: if (g_mc_n < BTC_MC_UCODE_SIZE && v != fw_dw(rad_fw_caicos_mc, g_mc_n)) g_bad_mc++; g_mc_n++; return;
    case CP_RB_RPTR_WR: if (R[CP_RB_CNTL / 4] & RB_RPTR_WR_ENA) g_rptr = v; break;
    }
    R[reg / 4] = v;
    if (reg == CP_RB_WPTR || reg == CP_ME_CNTL) cp_kick();
}

uint32_t rad_rd(radeon_t* r, uint32_t reg) {
    (void)r;
    if (reg + 4 > sizeof(R)) return 0;
    switch (reg) {
    case CP_RB_RPTR: return g_rptr;
    case VM_CONTEXT0_REQUEST_RESPONSE: return 1u << RESPONSE_TYPE_SHIFT;     /* (the TLB flushed) */
    case MC_SEQ_MISC0: return g_gddr5 ? (uint32_t)MC_SEQ_MISC0_GDDR5_VALUE << MC_SEQ_MISC0_GDDR5_SHIFT : 0;
    case MC_SEQ_SUP_CNTL: return R[reg / 4] == 1 ? RUN_MASK : 0;
    case MC_IO_PAD_CNTL_D0: return MEM_FALL_OUT_CMD;
    }
    return R[reg / 4];
}
void rad_wr(radeon_t* r, uint32_t reg, uint32_t v) { (void)r; reg_write(reg, v); }

static int g_fails;
#define CHECK(c, ...) do { if (c) printf("ok   " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); g_fails++; } printf("\n"); } while (0)

/* the card as the firmware leaves it (mode: as for main), and the kernel's bring-up */
static int model_boot(const char* mode, char* why, int cap) {
    if (!strcmp(mode, "fake2g")) VRAM_MB = 2048;
    if (!strcmp(mode, "real2g")) VRAM_MB = REAL_MB = 2048;
    g_cedar = !strcmp(mode, "cedar");
    int turks = !strcmp(mode, "turks"), barts = !strcmp(mode, "barts");
    FW_PFP = g_cedar ? rad_fw_cedar_pfp : turks ? rad_fw_turks_pfp : barts ? rad_fw_barts_pfp : rad_fw_caicos_pfp;
    FW_ME = g_cedar ? rad_fw_cedar_me : rad_fw_caicos_me;
    FW_RLC = g_cedar ? rad_fw_cedar_rlc : rad_fw_btc_rlc;
    VR = calloc(1, VRAM_HOST);
    R[MC_VM_FB_LOCATION / 4] = ((VRAM_MB / 16 - 1) << 16) | 0;      /* (0 .. 1 GiB) */
    R[CONFIG_MEMSIZE / 4] = VRAM_MB;
    R[CP_ME_CNTL / 4] = CP_ME_HALT | CP_PFP_HALT;
    R[MC_VM_SYSTEM_APERTURE_LOW_ADDR / 4] = 0;
    R[MC_VM_SYSTEM_APERTURE_HIGH_ADDR / 4] = !strcmp(mode, "wideap") ? 0x0FFFFFFFu : ((VRAM_MB << 20) - 1) >> 12;
    g_cp_alive = strcmp(mode, "dead") != 0;
    g_gddr5 = !strcmp(mode, "gddr5");
    rt.family = g_cedar ? CHIP_CEDAR : !strcmp(mode, "juniper") ? CHIP_JUNIPER : turks ? CHIP_TURKS : barts ? CHIP_BARTS : CHIP_CAICOS;
    rt.pci.device = turks ? 0x6758 : barts ? 0x6738 : 0x6779;    /* (HD 6670, HD 6870, HD 7450) */
    return rad_gfx_init(&rt, (uintptr_t)VR, 0, VISIBLE, why, cap);
}

#ifndef RADEON_PRELOAD                             /* (tools/tests/radeon_host: Mesa's r600 on the model) */
int main(int argc, char** argv) {
    const char* mode = argc > 1 && argv[1] ? argv[1] : "ok";
    char why[160] = "";
    int r = model_boot(mode, why, sizeof(why));
    printf("%s: rad_gfx_init = %d: %s\n", mode, r, why);

    if (!strcmp(mode, "juniper")) {
        CHECK(r == -1 && strstr(why, "Caicos"), "another family: refused, it has no microcode here");
        CHECK(R[CP_RB_BASE / 4] == 0 && g_pfp_n == 0, "and the card untouched");
    } else if (!strcmp(mode, "drm")) {
        CHECK(r == 0, "the engine up");
        /* Task Manager's load: the 3D engine's busy bit, read as it is */
        R[GRBM_STATUS / 4] = 0;
        int idle = rad_gfx_busy_now();
        R[GRBM_STATUS / 4] = GUI_ACTIVE;
        int busy = rad_gfx_busy_now();
        R[GRBM_STATUS / 4] = 0;
        CHECK(!idle && busy, "the GPU's load: GRBM_STATUS.GUI_ACTIVE read as idle %d, busy %d", idle, busy);
        /* the version Mesa's radeon winsys asks for */
        char name[16] = "";
        struct drm_version_k v = { 0, 0, 0, sizeof(name) - 1, (uint64_t)(uintptr_t)name, 0, 0, 0, 0 };
        #define IOC(dir, nr, sz) ((unsigned long)(dir) << 30 | (unsigned long)(sz) << 16 | 'd' << 8 | (nr))
        CHECK(rad_drm_ioctl(1, IOC(3, NR_VERSION, sizeof(v)), &v) == 0 && v.version_major == 2 && v.version_minor >= 50 && !strcmp(name, "radeon"),
              "DRM_IOCTL_VERSION: %s %d.%d", name, v.version_major, v.version_minor);
        uint32_t val = 0;
        struct info_k in = { INFO_DEVICE_ID, 0, (uint64_t)(uintptr_t)&val };
        CHECK(rad_drm_ioctl(1, IOC(3, 0x40 + RD_INFO, sizeof(in)), &in) == 0 && val == 0x6779, "INFO device id %04x", val);
        in.request = INFO_NUM_BACKENDS;
        CHECK(rad_drm_ioctl(1, IOC(3, 0x40 + RD_INFO, sizeof(in)), &in) == 0 && val == 1, "INFO backends %u", val);
        in.request = INFO_VA_START;
        CHECK(rad_drm_ioctl(1, IOC(3, 0x40 + RD_INFO, sizeof(in)), &in) == -E_INVAL, "INFO VA_START refused (no virtual memory: relocations)");
        struct gem_info_k gi = { 0, 0, 0 };
        CHECK(rad_drm_ioctl(1, IOC(3, 0x40 + RD_GEM_INFO, sizeof(gi)), &gi) == 0 && gi.vram_size == 1024ull << 20 && gi.vram_visible == 63ull << 20,
              "GEM_INFO: %llu MiB video memory, %llu MiB of it for apps' mapped buffers, GTT %llu MiB",
              (unsigned long long)(gi.vram_size >> 20), (unsigned long long)(gi.vram_visible >> 20), (unsigned long long)(gi.gart_size >> 20));
        /* a buffer in the GTT and one in video memory, mapped */
        struct gem_create_k cg = { 4096, 4096, 0, DOMAIN_GTT, 0 }, cv = { 65536, 4096, 0, DOMAIN_VRAM, 0 };
        CHECK(rad_drm_ioctl(1, IOC(3, 0x40 + RD_GEM_CREATE, sizeof(cg)), &cg) == 0 && cg.handle, "GEM_CREATE in the GTT: handle %u", cg.handle);
        CHECK(rad_drm_ioctl(1, IOC(3, 0x40 + RD_GEM_CREATE, sizeof(cv)), &cv) == 0 && cv.handle, "GEM_CREATE in video memory: handle %u", cv.handle);
        struct gem_mmap_k mg = { cg.handle, 0, 0, 4096, 0 }, mv = { cv.handle, 0, 0, 65536, 0 };
        CHECK(rad_drm_ioctl(1, IOC(3, 0x40 + RD_GEM_MMAP, sizeof(mg)), &mg) == 0 && mg.addr_ptr, "GEM_MMAP of the GTT buffer");
        CHECK(rad_drm_ioctl(1, IOC(3, 0x40 + RD_GEM_MMAP, sizeof(mv)), &mv) == 0 && mv.addr_ptr, "GEM_MMAP of the video memory buffer");
        struct gem_mmap_k mg2 = { cg.handle, 0, 0, 4096, 0 };
        CHECK(rad_drm_ioctl(1, IOC(3, 0x40 + RD_GEM_MMAP, sizeof(mg2)), &mg2) == 0 && mg2.addr_ptr == mg.addr_ptr, "GEM_MMAP again: the same mapping");
        volatile uint32_t* pg = (volatile uint32_t*)(uintptr_t)mg.addr_ptr;
        volatile uint32_t* pv = (volatile uint32_t*)(uintptr_t)mv.addr_ptr;
        CHECK(pv == (volatile uint32_t*)(VR + (64ull << 20)) && pg[0] == 0 && pv[0] == 0, "video memory buffers above the display (64 MiB), cleared");
        /* a command stream: MEM_WRITE into the GTT buffer, EVENT_WRITE_EOP into the video memory one,
         * each followed by its relocation (a NOP naming the buffer) - the checker patches the addresses */
        uint32_t ibd[] = {
            PACKET3(PACKET3_MEM_WRITE, 3), 0, 1u << 18, 0x12345678, 0, PACKET3(PACKET3_NOP, 0), 0 * 4,
            PACKET3(PACKET3_EVENT_WRITE_EOP, 4), 0x14 | 5u << 8, 8, 1u << 29, 0xCAFE0001, 0,
            PACKET3(PACKET3_NOP, 0), 1 * 4,
        };
        struct cs_reloc_k rel[2] = { { cg.handle, 0, DOMAIN_GTT, 0 }, { cv.handle, 0, DOMAIN_VRAM, 0 } };
        uint32_t fl[3] = { 0, CS_RING_GFX, 0 };
        struct cs_chunk_k chk[3] = { { CHUNK_IB, sizeof(ibd) / 4, (uint64_t)(uintptr_t)ibd },
                                     { CHUNK_RELOCS, 8, (uint64_t)(uintptr_t)rel }, { CHUNK_FLAGS, 3, (uint64_t)(uintptr_t)fl } };
        uint64_t cp[3] = { (uint64_t)(uintptr_t)&chk[0], (uint64_t)(uintptr_t)&chk[1], (uint64_t)(uintptr_t)&chk[2] };
        struct cs_k cs = { 3, 0, (uint64_t)(uintptr_t)cp, 0, 0 };
        uint32_t ib_before = g_pkt_ib;
        int cr = rad_drm_ioctl(1, IOC(3, 0x40 + RD_CS, sizeof(cs)), &cs);
        CHECK(cr == 0 && g_pkt_ib == ib_before + 1, "CS accepted by the checker and run as an indirect buffer (%d)", cr);
        struct gem_wait_idle_k wi = { cg.handle, 0 };
        CHECK(rad_drm_ioctl(1, IOC(1, 0x40 + RD_GEM_WAIT_IDLE, sizeof(wi)), &wi) == 0, "GEM_WAIT_IDLE");
        CHECK(pg[0] == 0x12345678, "MEM_WRITE landed in the GTT buffer, its address patched in (%08x)", pg[0]);
        CHECK(pv[2] == 0xCAFE0001, "EVENT_WRITE_EOP landed 8 bytes into the video memory buffer (%08x)", pv[2]);
        struct gem_busy_k gb = { cv.handle, 0 };
        CHECK(rad_drm_ioctl(1, IOC(3, 0x40 + RD_GEM_BUSY, sizeof(gb)), &gb) == 0 && gb.domain == DOMAIN_VRAM, "GEM_BUSY: idle, in video memory");
        /* refused: a write to the command processor's ring base (outside what apps may set) */
        uint32_t bad[] = { PACKET3(PACKET3_SET_CONFIG_REG, 1), (0xC100 - PACKET3_SET_CONFIG_REG_START) >> 2, 0 };
        chk[0].chunk_data = (uint64_t)(uintptr_t)bad; chk[0].length_dw = 3;
        uint32_t base = R[CP_RB_BASE / 4], ib_now = g_pkt_ib;
        g_quiet = 1;
        cr = rad_drm_ioctl(1, IOC(3, 0x40 + RD_CS, sizeof(cs)), &cs);
        g_quiet = 0;
        CHECK(cr == -E_INVAL && g_pkt_ib == ib_now && R[CP_RB_BASE / 4] == base, "a CS writing CP_RB_BASE refused, nothing run");
        /* a buffer past its end */
        uint32_t past[] = { PACKET3(PACKET3_MEM_WRITE, 3), 4096, 1u << 18, 1, 0, PACKET3(PACKET3_NOP, 0), 0 };
        chk[0].chunk_data = (uint64_t)(uintptr_t)past; chk[0].length_dw = 7;
        g_quiet = 1;
        cr = rad_drm_ioctl(1, IOC(3, 0x40 + RD_CS, sizeof(cs)), &cs);
        g_quiet = 0;
        CHECK(cr == -E_INVAL, "a CS writing past its buffer's end refused");
        /* another app's handle does not exist for it */
        struct gem_busy_k other = { cg.handle, 0 };
        CHECK(rad_drm_ioctl(2, IOC(3, 0x40 + RD_GEM_BUSY, sizeof(other)), &other) == -E_NOENT, "another app's buffer: not there for it");
        struct gem_close_k gc = { cv.handle, 0 };
        uint64_t used = g_vram_used;
        CHECK(rad_drm_ioctl(1, IOC(1, NR_GEM_CLOSE, sizeof(gc)), &gc) == 0 && g_vram_used == used - 65536, "GEM_CLOSE gives its video memory back");
        rad_drm_release(1);
        CHECK(g_gtt_used == 0 && g_vram_used == 0, "the app's end: everything given back");
        char st[160];
        rad_drm_stats(st, sizeof(st));
        printf("  %s\n", st);
    } else if (!strcmp(mode, "dead")) {
        CHECK(r == -1 && strstr(why, "ring test"), "a command processor that does not run: the ring test fails");
        CHECK(R[CP_ME_CNTL / 4] == (CP_ME_HALT | CP_PFP_HALT), "the command processor halted again");
        CHECK(!(R[VM_CONTEXT0_CNTL / 4] & 1), "the GART off again");
        CHECK(!rad_gfx_ready(), "not ready");
    } else {
        CHECK(r == 0 && rad_gfx_ready(), "brought up");
        CHECK(g_bad_pfp == 0 && g_bad_me == 0 && g_pfp_tot == EVERGREEN_PFP_UCODE_SIZE && g_me_tot == EVERGREEN_PM4_UCODE_SIZE,
              "the PFP (%u) and ME (%u) microcode loaded, as the files have it", g_pfp_tot, g_me_tot);
        CHECK(g_bad_rlc == 0 && g_rlc_n == EVERGREEN_RLC_UCODE_SIZE, "the RLC microcode (%u dwords)", g_rlc_n);
        if (g_cedar) CHECK(g_mc_n == 0 && strstr(why, "Cedar"), "Cedar (Evergreen): its own microcode, none for the memory controller");
        else if (g_gddr5) CHECK(g_bad_mc == 0 && g_mc_n == BTC_MC_UCODE_SIZE, "GDDR5, the sequencer not running: its microcode loaded (%u dwords)", g_mc_n);
        else CHECK(g_mc_n == 0, "DDR3: the memory controller's microcode left as the BIOS has it");
        CHECK(R[VM_CONTEXT0_CNTL / 4] & 1, "the GART on");
        CHECK(((uint64_t)R[VM_CONTEXT0_PAGE_TABLE_BASE_ADDR / 4] << 12) == VISIBLE - (1ull << 20), "its page table in the last MiB the CPU reaches (127 MiB of a 128 MiB window)");
        CHECK(((uint64_t)R[VM_CONTEXT0_PAGE_TABLE_START_ADDR / 4] << 12) == (uint64_t)VRAM_MB << 20, "the GTT right after video memory");
        CHECK(((uint64_t)R[CP_RB_BASE / 4] << 8) == (uint64_t)VRAM_MB << 20 && (2u << (R[CP_RB_CNTL / 4] & 0x3F)) == RING_BYTES / 4,
              "the ring: 64 KiB of system memory at the start of the GTT");
        CHECK(g_pkt_init == 1, "ME_INITIALIZE run");
        uint32_t probes = VRAM_MB == 2048 ? 3 : 2;             /* (256, 512 [, 1024] MiB past the probe dword) */
        CHECK(g_pkt_eop == 2 + probes && g_pkt_ib == 1 && g_pkt_mem == 1 + probes,
              "two fences, one indirect buffer (and its next-rptr write), %u memory probes", probes);
        if (VRAM_MB != REAL_MB) CHECK(rad_vram_mb() == REAL_MB && strstr(why, "BIOS says 2048"), "a BIOS saying 2 GiB on 1 GiB: %u MiB found and said", rad_vram_mb());
        else CHECK(rad_vram_mb() == VRAM_MB && !strstr(why, "BIOS says"), "all %u MiB there", rad_vram_mb());
        CHECK(g_bad_addr == 0 && g_bad_pkt == 0, "every address resolved, every packet understood (%u, %u)", g_bad_addr, g_bad_pkt);
        CHECK(g_dev.wb.wb[R600_WB_EVENT_OFFSET / 4] == g_seq, "the last fence written back to system memory: %u", g_seq);
        int cleared = 1;
        for (uint32_t i = SYS_PAGES; i < 65536; i++) if ((((uint64_t*)(VR + GFX_OFF))[i] & 1)) cleared = 0;
        CHECK(cleared, "the test page unbound again");
        CHECK(R[SCRATCH_REG0 / 4] == 0xDEADBEEF && g_pkt_cfg == 2, "the ring test and the indirect buffer set the scratch register (%u SET_CONFIG_REGs)", g_pkt_cfg);
        uint64_t pa = ARENA_PHYS;
        CHECK(rad_gart_bind(65535, &pa, 2) == 0 && rad_gart_bind(70000, &pa, 1) == 0, "binding past the GTT refused");
        uint64_t ga = rad_gart_bind(100, &pa, 1);
        CHECK(ga == ((uint64_t)VRAM_MB << 20) + 100 * 4096 && mc(ga, 4) == (void*)g_arena, "a page bound: the card reads system memory there");
        rad_gart_unbind(100, 1);
        g_quiet = 1;
        CHECK(mc(ga, 4) == NULL, "and unbound");
        g_quiet = 0;
        char again[64];
        CHECK(rad_gfx_init(&rt, (uintptr_t)VR, 0, VISIBLE, again, sizeof(again)) == 0 && g_pkt_init == 1, "a second start: left as it is");
        CHECK(strstr(rad_gfx_status(), "up:") != NULL, "status: %s", rad_gfx_status());
        CHECK(R[MC_VM_SYSTEM_APERTURE_HIGH_ADDR / 4] == ((VRAM_MB << 20) - 1) >> 12, "the system aperture: exactly the video memory");
        CHECK((g_pcicmd & 6) == 6, "bus mastering on (the firmware had it off)");
    }
    free(VR);
    printf(g_fails ? "%d FAILED\n" : "all passed\n", g_fails);
    return g_fails != 0;
}
#endif
