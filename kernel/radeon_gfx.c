/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The 3D engine of a Radeon HD 6450 / 7450 / 8450 (Caicos, Northern
 * Islands): brought up as Linux's radeon driver does in evergreen_startup()
 * - the memory controller's microcode (when the video BIOS has not started
 * it), the GART, the engine's set-up, the run-list controller, the command
 * processor's microcode and its ring - then proven to work: a ring test, a
 * fence written back by the engine, an indirect buffer fetched from system
 * memory through the GART. The bring-up code itself is Linux's (MIT,
 * third_party/radeon/evergreen_gfx.c, r600_gfx.c, ni_gfx.c); this is what
 * it runs on: the card's memory laid out, the ring, the scratch registers.
 *
 * Video memory (offsets into it; the CPU reaches it through the BAR):
 *   0 .. 128 MiB     the display's (the firmware framebuffer, the cursor,
 *                    the one mode setting scans out at 64 MiB)
 *   128 MiB          the GART's page table (8 bytes a page)
 *   + 512 KiB        the ring (64 KiB)
 *   + 64 KiB         the writeback page (read pointer, scratch, fences)
 * The GTT (system memory as the GPU sees it, through the GART) comes after
 * the video memory in the memory controller's space. */
#include "radeon.h"
#include "../third_party/radeon/radeon_device.h"
#include "../third_party/radeon/evergreend.h"
#include "../third_party/radeon/radeon_ucode.h"
#include "kheap.h"
#include "kstring.h"
#include "paging.h"
#include "pci.h"
#include "serial.h"

#ifndef RAD_PHYS                       /* (the host test moves system memory elsewhere) */
#define RAD_PHYS(p) ((uint64_t)(uintptr_t)(p))
#endif

/* where the 3D engine's area starts: the last MiB of the video memory the
 * CPU can reach through the BAR (set in rad_gfx_init) */
static uint64_t g_gfx_off = 128ull << 20;
#define GFX_OFF        g_gfx_off
#define GART_BYTES     (512u << 10)
#define GTT_SIZE       (256ull << 20)            /* GART_BYTES / 8 pages of 4 KiB */
/* The ring and the writeback page are in system memory, bound at the start
 * of the GTT, as Linux has them: written by the CPU through its caches,
 * read by the GPU through the GART (snooped). In video memory, written
 * through the BAR, the command processor read something else than the CPU
 * had written there on a real HD 7450 ("ring[0] 0030cf95"). */
#define RING_BYTES     (64u << 10)
#define RING_PAGE      0u                        /* (GTT pages 0-15: the ring, 16: writeback) */
#define WB_PAGE        (RING_BYTES / 4096u)
#define SYS_PAGES      (WB_PAGE + 1u)
#define IB_TEST_PAGE   (SYS_PAGES)               /* (the indirect buffer test's page) */
/* in video memory: the GART's page table, then a page for the memory probe */
#define GFX_BYTES      (GART_BYTES + 4096u)

#define PTE_VALID      1ull
#define PTE_SYSTEM     2ull
#define PTE_SNOOPED    4ull
#define PTE_READABLE   0x20ull
#define PTE_WRITEABLE  0x40ull

#define NSCRATCH       7

/* the firmware (firmware/amd, AMD's licence: firmware/amd/LICENSE.radeon) */
#define FW(name, file)                                                        \
    __asm__(".section .rodata\n.balign 4\n"                                   \
            ".global " #name "\n" #name ":\n"                                 \
            ".incbin \"firmware/amd/" file "\"\n"                             \
            ".global " #name "_end\n" #name "_end:\n.byte 0\n.previous\n");   \
    extern const uint8_t name[], name##_end[];
#ifndef RADEON_MODEL
FW(rad_fw_caicos_pfp, "CAICOS_pfp.bin")
FW(rad_fw_caicos_me,  "CAICOS_me.bin")
FW(rad_fw_btc_rlc,    "BTC_rlc.bin")
FW(rad_fw_caicos_mc,  "CAICOS_mc.bin")
FW(rad_fw_cedar_pfp,  "CEDAR_pfp.bin")
FW(rad_fw_cedar_me,   "CEDAR_me.bin")
FW(rad_fw_cedar_rlc,  "CEDAR_rlc.bin")
FW(rad_fw_licence,    "LICENSE.radeon")
#else
extern const uint8_t rad_fw_caicos_pfp[], rad_fw_caicos_pfp_end[], rad_fw_caicos_me[], rad_fw_caicos_me_end[];
extern const uint8_t rad_fw_btc_rlc[], rad_fw_btc_rlc_end[], rad_fw_caicos_mc[], rad_fw_caicos_mc_end[];
extern const uint8_t rad_fw_cedar_pfp[], rad_fw_cedar_pfp_end[], rad_fw_cedar_me[], rad_fw_cedar_me_end[];
extern const uint8_t rad_fw_cedar_rlc[], rad_fw_cedar_rlc_end[];
extern const uint8_t rad_fw_licence[], rad_fw_licence_end[];
#endif
#define FWOF(n) (struct firmware){ n, (size_t)(n##_end - n) }

static struct radeon_device g_dev;
static struct firmware g_pfp, g_me, g_rlc, g_mc;
static uint8_t* g_vram;                /* the video memory, as the CPU writes it */
static uint64_t* g_gart;               /* the page table (in video memory) */
static uint8_t* g_dummy;               /* where pages not bound read and write */
static int g_state;                    /* 0 not tried, 1 up, -1 failed */
static char g_status[200] = "not started (gpu radeon 3d)";
static uint32_t g_ring_us, g_fence_us, g_ib_us;
static uint32_t g_seq;                 /* the last fence's sequence number */
static uint32_t g_bios_mb, g_real_mb;  /* the video memory the BIOS says, and what is there */
#define PROBE_OFF      (GFX_OFF + GART_BYTES)    /* (a video memory dword the CPU sees and nothing else uses) */
static uint8_t* g_sys;                 /* the ring and the writeback page (system memory) */

const char* rad_fw_notice(uint32_t* len) {
    *len = (uint32_t)(rad_fw_licence_end - rad_fw_licence);
    return (const char*)rad_fw_licence;
}

/* ---- what the Linux code calls ---- */

u32 radeon_rcu_rreg(struct radeon_device* rdev, u32 reg) {
    WREG32(RCU_IND_INDEX, reg);
    return RREG32(RCU_IND_DATA);
}

static uint8_t pcie_cap(const pci_dev_t* d) {
    if (!(pci_read16(d->bus, d->dev, d->fn, 6) & 0x10)) return 0;
    uint8_t p = (uint8_t)(pci_read32(d->bus, d->dev, d->fn, 0x34) & 0xFC);
    for (int guard = 0; p && guard < 48; guard++) {
        uint32_t h = pci_read32(d->bus, d->dev, d->fn, p);
        if ((h & 0xFF) == 0x10) return p;
        p = (uint8_t)((h >> 8) & 0xFC);
    }
    return 0;
}
/* the largest read request the card makes: 128 << (device control bits 14:12) */
int pcie_get_readrq(void* pdev) {
    (void)pdev;
    const pci_dev_t* d = &g_dev.rt->pci;
    uint8_t c = pcie_cap(d);
    if (!c) return 128;
    return 128 << ((pci_read16(d->bus, d->dev, d->fn, (uint8_t)(c + 8)) >> 12) & 7);
}
int pcie_set_readrq(void* pdev, int rq) {
    (void)pdev;
    const pci_dev_t* d = &g_dev.rt->pci;
    uint8_t c = pcie_cap(d);
    if (!c || rq < 128 || rq > 4096) return -1;
    uint16_t v = pci_read16(d->bus, d->dev, d->fn, (uint8_t)(c + 8));
    v = (uint16_t)((v & ~0x7000u) | ((uint16_t)(order_base_2((unsigned)rq) - 7) << 12));
    pci_write16(d->bus, d->dev, d->fn, (uint8_t)(c + 8), v);
    return 0;
}

int radeon_scratch_get(struct radeon_device* rdev, u32* reg) {
    for (int i = 0; i < NSCRATCH; i++)
        if (rdev->scratch_free & (1u << i)) {
            rdev->scratch_free &= ~(1u << i);
            *reg = SCRATCH_REG0 + 4u * (u32)i;
            return 0;
        }
    return -EINVAL;
}
void radeon_scratch_free(struct radeon_device* rdev, u32 reg) {
    if (reg >= SCRATCH_REG0 && reg < SCRATCH_REG0 + 4 * NSCRATCH) rdev->scratch_free |= 1u << ((reg - SCRATCH_REG0) / 4);
}

static unsigned ring_rptr(struct radeon_device* rdev) { return RREG32(CP_RB_RPTR) & rdev->ring[0].ptr_mask; }

int radeon_ring_lock(struct radeon_device* rdev, struct radeon_ring* ring, unsigned ndw) {
    unsigned size = ring->ptr_mask + 1;
    ndw = (ndw + ring->align_mask) & ~ring->align_mask;
    if (ndw >= size) return -ENOMEM;
    for (int t = 0;; t++) {
        unsigned free = (ring_rptr(rdev) + size - ring->wptr) & ring->ptr_mask;
        if (!free) free = size;                 /* (empty) */
        if (ndw < free) break;
        if (t >= rdev->usec_timeout) return -EBUSY;
        udelay(1);
    }
    ring->count_dw = ndw;
    ring->wptr_old = ring->wptr;
    return 0;
}

void radeon_ring_unlock_commit(struct radeon_device* rdev, struct radeon_ring* ring, bool hdp_flush) {
    (void)hdp_flush;
    while (ring->wptr & ring->align_mask) {    /* (the CP fetches 16 dwords at a time) */
        ring->ring[ring->wptr++] = ring->nop;
        ring->wptr &= ring->ptr_mask;
    }
    ring->count_dw = 0;
    /* the ring is in video memory: what the CPU wrote, out of the host data path first */
    mb();
    WREG32(HDP_MEM_COHERENCY_FLUSH_CNTL, 1);
    (void)RREG32(HDP_MEM_COHERENCY_FLUSH_CNTL);
    WREG32(CP_RB_WPTR, ring->wptr);
    (void)RREG32(CP_RB_WPTR);
}

int radeon_ring_test(struct radeon_device* rdev, int idx, struct radeon_ring* ring) {
    (void)idx;
    uint64_t t0 = timer_counter();
    int r = r600_ring_test(rdev, ring);
    g_ring_us = (uint32_t)((timer_counter() - t0) / 1000u);
    return r;
}

/* ---- the GART ---- */

static void gart_flush(void) {
    struct radeon_device* rdev = &g_dev;
    mb();
    WREG32(HDP_MEM_COHERENCY_FLUSH_CNTL, 1);
    (void)RREG32(HDP_MEM_COHERENCY_FLUSH_CNTL);
    if (rdev->gart.ready) evergreen_pcie_gart_tlb_flush(rdev);
}

uint64_t rad_gtt_base(void) { return g_state > 0 || g_dev.gart.ready ? g_dev.mc.gtt_start : 0; }
uint32_t rad_gtt_pages(void) { return g_dev.gart.num_gpu_pages; }

uint64_t rad_gart_bind(uint32_t first, const uint64_t* phys, uint32_t n) {
    if (!g_gart || first > g_dev.gart.num_gpu_pages || n > g_dev.gart.num_gpu_pages - first) return 0;
    for (uint32_t i = 0; i < n; i++)
        g_gart[first + i] = (phys[i] & 0xFFFFFFFFFF000ull) | PTE_VALID | PTE_SYSTEM | PTE_SNOOPED | PTE_READABLE | PTE_WRITEABLE;
    gart_flush();
    return g_dev.mc.gtt_start + ((uint64_t)first << 12);
}

void rad_gart_unbind(uint32_t first, uint32_t n) {
    if (!g_gart || first > g_dev.gart.num_gpu_pages || n > g_dev.gart.num_gpu_pages - first) return;
    for (uint32_t i = 0; i < n; i++) g_gart[first + i] = RAD_PHYS(g_dummy) & 0xFFFFFFFFFF000ull;
    gart_flush();
}

/* ---- bring-up ---- */

static int fail(const char* what, int r) {
    ksnprintf(g_status, sizeof(g_status), "failed: %s (%d)", what, r);
    klog("radeon: 3D engine %s\n", g_status);
    struct radeon_device* rdev = &g_dev;
    r700_cp_stop(rdev);
    rdev->ring[0].ready = false;
    if (rdev->gart.ready) { evergreen_pcie_gart_disable(rdev); rdev->gart.ready = false; }
    g_state = -1;
    return -1;
}

#define FAIL(what, r) do { fail(what, r); kstrlcpy(why, g_status, (size_t)cap); return -1; } while (0)

static uint32_t wait_fence(volatile uint32_t* at, uint32_t seq) {
    for (int i = 0; i < g_dev.usec_timeout; i++) {
        if (*at == seq) return (uint32_t)i + 1;
        udelay(1);
    }
    return 0;
}

/* the engine writes v at video memory offset off; a fence after it (0: no fence) */
static int gpu_write_vram(struct radeon_device* rdev, uint64_t off, uint32_t v) {
    struct radeon_ring* ring = &rdev->ring[0];
    uint64_t a = rdev->mc.vram_start + off;
    if (radeon_ring_lock(rdev, ring, 32) != 0) return 0;
    radeon_ring_write(ring, PACKET3(PACKET3_MEM_WRITE, 3));
    radeon_ring_write(ring, (u32)a & 0xFFFFFFFCu);
    radeon_ring_write(ring, (upper_32_bits(a) & 0xFF) | (1u << 18));    /* (32 bits) */
    radeon_ring_write(ring, v);
    radeon_ring_write(ring, 0);
    struct radeon_fence f = { 0, ++g_seq };
    r600_fence_ring_emit(rdev, &f);
    radeon_ring_unlock_commit(rdev, ring, false);
    return wait_fence(rdev->fence_drv[0].cpu_addr, g_seq) != 0;
}

/* the size the memory repeats at (MiB), the BIOS's if it does not; 0 if the engine stops */
static uint32_t probe_vram(struct radeon_device* rdev, uint32_t bios_mb) {
    volatile uint32_t* seen = (volatile uint32_t*)(g_vram + PROBE_OFF);
    uint64_t top = rdev->mc.vram_end - rdev->mc.vram_start + 1;
    for (uint64_t size = 256ull << 20; size < ((uint64_t)bios_mb << 20) && PROBE_OFF + size < top; size <<= 1) {
        uint32_t mark = 0x5EED0000u | (uint32_t)(size >> 20);
        *seen = 0;
        mb();
        if (!gpu_write_vram(rdev, PROBE_OFF + size, mark)) return 0;
        WREG32(HDP_MEM_COHERENCY_FLUSH_CNTL, 1);
        (void)RREG32(HDP_MEM_COHERENCY_FLUSH_CNTL);
        if (*seen == mark) return (uint32_t)(size >> 20);
    }
    return bios_mb;
}
uint32_t rad_vram_mb(void) { return g_real_mb; }

/* the CPU reaches the area through the BAR: a value written there reads back */
static int vram_reads_back(void) {
    volatile uint32_t* t = (volatile uint32_t*)(g_vram + PROBE_OFF);
    uint32_t keep = t[0];
    t[0] = 0x5A17C0DEu;
    mb();
    uint32_t got = t[0];
    t[0] = ~0x5A17C0DEu;
    mb();
    uint32_t got2 = t[0];
    t[0] = keep;
    return got == 0x5A17C0DEu && got2 == ~0x5A17C0DEu;
}

int rad_gfx_ready(void) { return g_state > 0; }
const char* rad_gfx_status(void) { return g_status; }

int rad_gfx_init(radeon_t* rt, uintptr_t vram_cpu, uint64_t vram_mc, uint64_t visible, char* why, int cap) {
    if (g_state > 0) { kstrlcpy(why, "already up", (size_t)cap); return 0; }
    struct radeon_device* rdev = &g_dev;
    if (rt->family != CHIP_CAICOS && rt->family != CHIP_CEDAR) {
        ksnprintf(g_status, sizeof(g_status), "no microcode for %s (only Caicos: HD 6450 / 7450 / 8450; Cedar: HD 5450 / 6350 / 7350)",
                  rad_family_name(rt->family));
        kstrlcpy(why, g_status, (size_t)cap);
        return -1;
    }
    memset(rdev, 0, sizeof(*rdev));
    rdev->rt = rt;
    rdev->family = rt->family;
    rdev->flags = RADEON_IS_PCIE;
    rdev->usec_timeout = 100000;
    rdev->pdev = &rdev->pdev_store;
    rdev->pdev->device = rt->pci.device;
    rdev->asic = &rdev->asic_store;
    rdev->scratch_free = (1u << NSCRATCH) - 1;

    /* where the video memory is, how much of it there is */
    uint32_t loc = RREG32(MC_VM_FB_LOCATION);
    rdev->mc.vram_start = (uint64_t)(loc & 0xFFFF) << 24;
    rdev->mc.vram_end = ((uint64_t)(loc >> 16) << 24) | 0xFFFFFF;
    uint64_t mb = RREG32(CONFIG_MEMSIZE);
    rdev->mc.real_vram_size = rdev->mc.mc_vram_size = mb << 20;
    /* The area goes in the last MiB the CPU reaches through the BAR (the
     * window into video memory). It was put 128 MiB in, on the assumption
     * that every card shows at least 256 MiB: a card showing less got its
     * ring written past the end of the window - the command processor
     * read video memory the CPU never wrote ("ring[0] 0030cf95"). */
    if (!visible) visible = 256ull << 20;
    rdev->mc.visible_vram_size = visible;
    g_gfx_off = ((visible > (1ull << 30) ? (1ull << 30) : visible) - (1ull << 20)) & ~0xFFFFFull;
    if (vram_mc != rdev->mc.vram_start || rdev->mc.vram_end < rdev->mc.vram_start) {
        ksnprintf(why, (size_t)cap, "the memory controller's map moved (FB_LOCATION %08x)", loc);
        kstrlcpy(g_status, why, sizeof(g_status));
        g_state = -1;
        return -1;
    }
    if (g_gfx_off < (96ull << 20) || mb < (GFX_OFF + GFX_BYTES) >> 20 ||
        rdev->mc.vram_end - rdev->mc.vram_start + 1 < GFX_OFF + GFX_BYTES) {
        ksnprintf(why, (size_t)cap, "%u MiB of video memory: too little", (unsigned)mb);
        kstrlcpy(g_status, why, sizeof(g_status));
        g_state = -1;
        return -1;
    }
    rdev->mc.gtt_size = GTT_SIZE;
    rdev->mc.gtt_start = (rdev->mc.vram_end + 1 + (4ull << 20) - 1) & ~((4ull << 20) - 1);
    rdev->mc.gtt_end = rdev->mc.gtt_start + GTT_SIZE - 1;

    if (!mmio_map(vram_cpu + GFX_OFF, GFX_BYTES)) { kstrlcpy(why, "its video memory could not be mapped", (size_t)cap); g_state = -1; return -1; }
    g_vram = (uint8_t*)vram_cpu;
    if (!vram_reads_back()) {
        ksnprintf(why, (size_t)cap, "video memory at %u MiB does not read back through the BAR (window %u MiB)",
                  (unsigned)(GFX_OFF >> 20), (unsigned)(visible >> 20));
        kstrlcpy(g_status, why, sizeof(g_status));
        g_state = -1;
        return -1;
    }
    g_dummy = kmalloc_aligned(4096, 4096);
    if (!g_dummy) { kstrlcpy(why, "out of memory", (size_t)cap); g_state = -1; return -1; }
    memset(g_dummy, 0, 4096);
    rdev->dummy_page.addr = RAD_PHYS(g_dummy);

    /* (Cedar is Evergreen: its memory controller needs no microcode) */
    int ni = rt->family == CHIP_CAICOS;
    g_pfp = ni ? FWOF(rad_fw_caicos_pfp) : FWOF(rad_fw_cedar_pfp);
    g_me  = ni ? FWOF(rad_fw_caicos_me) : FWOF(rad_fw_cedar_me);
    g_rlc = ni ? FWOF(rad_fw_btc_rlc) : FWOF(rad_fw_cedar_rlc);
    g_mc  = FWOF(rad_fw_caicos_mc);
    if (g_pfp.size != EVERGREEN_PFP_UCODE_SIZE * 4 || g_me.size != EVERGREEN_PM4_UCODE_SIZE * 4 ||
        g_rlc.size != EVERGREEN_RLC_UCODE_SIZE * 4 || (ni && g_mc.size != BTC_MC_UCODE_SIZE * 4)) {
        kstrlcpy(why, "the microcode is not the size it should be", (size_t)cap);
        kstrlcpy(g_status, why, sizeof(g_status));
        g_state = -1;
        return -1;
    }
    rdev->pfp_fw = &g_pfp;
    rdev->me_fw = &g_me;
    rdev->rlc_fw = &g_rlc;
    rdev->mc_fw = ni ? &g_mc : NULL;

    /* the GART: every page the dummy one to begin with */
    g_gart = (uint64_t*)(g_vram + GFX_OFF);
    rdev->gart.num_gpu_pages = (unsigned)(GTT_SIZE >> 12);
    for (unsigned i = 0; i < rdev->gart.num_gpu_pages; i++) g_gart[i] = rdev->dummy_page.addr & 0xFFFFFFFFFF000ull;
    rdev->gart.ptr = (u32*)g_gart;
    rdev->gart.robj = g_gart;
    rdev->gart.table_addr = rdev->mc.vram_start + GFX_OFF;

    /* the ring and the writeback page */
    struct radeon_ring* ring = &rdev->ring[RADEON_RING_TYPE_GFX_INDEX];
    if (!g_sys) g_sys = kmalloc_aligned(SYS_PAGES * 4096u, 4096);
    if (!g_sys) { kstrlcpy(why, "out of memory", (size_t)cap); g_state = -1; return -1; }
    memset(g_sys, 0, SYS_PAGES * 4096u);
    for (uint32_t i = 0; i < SYS_PAGES; i++) {
        uint64_t pa = RAD_PHYS(g_sys + i * 4096u);
        g_gart[RING_PAGE + i] = (pa & 0xFFFFFFFFFF000ull) | PTE_VALID | PTE_SYSTEM | PTE_SNOOPED | PTE_READABLE | PTE_WRITEABLE;
    }
    ring->ring = (u32*)g_sys;
    ring->gpu_addr = rdev->mc.gtt_start + (uint64_t)RING_PAGE * 4096u;
    ring->ring_size = RING_BYTES;
    ring->ptr_mask = RING_BYTES / 4 - 1;
    ring->align_mask = 15;
    ring->nop = RADEON_CP_PACKET2;
    ring->idx = RADEON_RING_TYPE_GFX_INDEX;
    rdev->wb.wb = (u32*)(g_sys + WB_PAGE * 4096u);
    rdev->wb.gpu_addr = rdev->mc.gtt_start + (uint64_t)WB_PAGE * 4096u;
    memset(rdev->wb.wb, 0, 4096);
    rdev->wb.enabled = true;
    rdev->wb.use_event = true;
    ring->next_rptr_gpu_addr = rdev->wb.gpu_addr + RADEON_WB_RING0_NEXT_RPTR;
    ring->next_rptr_cpu_addr = rdev->wb.wb + RADEON_WB_RING0_NEXT_RPTR / 4;
    rdev->fence_drv[0].gpu_addr = rdev->wb.gpu_addr + R600_WB_EVENT_OFFSET;
    rdev->fence_drv[0].cpu_addr = rdev->wb.wb + R600_WB_EVENT_OFFSET / 4;

    klog("radeon: 3D engine: video memory %u MiB at MC 0x%llx, GTT 0x%llx-0x%llx\n", (unsigned)mb,
         (unsigned long long)rdev->mc.vram_start, (unsigned long long)rdev->mc.gtt_start, (unsigned long long)rdev->mc.gtt_end);

    int r;
    /* Bus mastering on (as Linux's pci_set_master): the GPU reads and writes
     * system memory - the ring, the writeback page, the GART's pages - only
     * with it. The display driver had only claimed the card, and UEFI leaves
     * it off for a graphics card (its screen needs no DMA): the command
     * processor's first fetch from system memory then hung in the bus
     * interface (SRBM BIF_BUSY, GRBM SRBM_RQ_PENDING, RPTR stuck at b8). */
    {
        const pci_dev_t* d = &rt->pci;
        uint16_t cmd = pci_read16(d->bus, d->dev, d->fn, 0x04);
        if (!(cmd & 0x4)) klog("radeon: bus mastering was off - on now\n");
        pci_write16(d->bus, d->dev, d->fn, 0x04, (uint16_t)(cmd | 0x6));   /* (memory space and bus master) */
    }
    /* (evergreen_init: the golden registers first; then evergreen_startup's order) */
    evergreen_init_golden_registers(rdev);
    if (ni && (r = ni_mc_load_microcode(rdev)) != 0) FAIL("memory controller microcode", r);
    /* The memory controller's map, as Linux's evergreen_mc_program leaves
     * it (the frame buffer's place itself is kept: the screen uses it): the
     * system aperture - the addresses taken as video memory as they are,
     * not through the GART - exactly the video memory (a firmware leaving
     * it wider sends the GTT, where the ring is, to raw video memory); its
     * default page ours; the CPU's path (HDP) covering all video memory. */
    {
        uint32_t lo = RREG32(MC_VM_SYSTEM_APERTURE_LOW_ADDR), hi = RREG32(MC_VM_SYSTEM_APERTURE_HIGH_ADDR);
        uint32_t def = RREG32(MC_VM_SYSTEM_APERTURE_DEFAULT_ADDR);
        uint32_t hb = RREG32(HDP_NONSURFACE_BASE), hinfo = RREG32(HDP_NONSURFACE_INFO), hs = RREG32(HDP_NONSURFACE_SIZE);
        klog("radeon: firmware's map: system aperture %08x-%08x (default %08x), HDP base %08x info %08x size %08x; "
             "video memory %08llx-%08llx\n", lo, hi, def, hb, hinfo, hs,
             (unsigned long long)rdev->mc.vram_start, (unsigned long long)rdev->mc.vram_end);
        WREG32(MC_VM_SYSTEM_APERTURE_LOW_ADDR, (u32)(rdev->mc.vram_start >> 12));
        WREG32(MC_VM_SYSTEM_APERTURE_HIGH_ADDR, (u32)(rdev->mc.vram_end >> 12));
        WREG32(MC_VM_SYSTEM_APERTURE_DEFAULT_ADDR, (u32)((rdev->mc.vram_start + PROBE_OFF) >> 12));
        WREG32(HDP_NONSURFACE_BASE, (u32)(rdev->mc.vram_start >> 8));
        WREG32(HDP_NONSURFACE_INFO, (2u << 7) | (1u << 30));
        WREG32(HDP_NONSURFACE_SIZE, 0x3FFFFFFFu);
        if ((lo << 12) > rdev->mc.vram_start || ((uint64_t)hi << 12) < (rdev->mc.vram_end & ~0xFFFull) ||
            ((uint64_t)hi << 12) > rdev->mc.vram_end)
            klog("radeon: the system aperture was not the video memory - set to it\n");
        if (!vram_reads_back()) FAIL("video memory does not read back after the memory controller's map was set", -EIO);
    }
    /* the AGP aperture off: nothing of the GTT goes there */
    WREG32(MC_VM_AGP_BASE, 0);
    WREG32(MC_VM_AGP_TOP, 0x0FFFFFFF);
    WREG32(MC_VM_AGP_BOT, 0x0FFFFFFF);
    if ((r = evergreen_pcie_gart_enable(rdev)) != 0) FAIL("GART", r);
    evergreen_gpu_init(rdev);
    if (!vram_reads_back()) klog("radeon: after the engine's setup, video memory no longer reads back through the BAR\n");
    if ((r = evergreen_rlc_resume(rdev)) != 0) FAIL("run-list controller", r);
    if ((r = evergreen_cp_load_microcode(rdev)) != 0) FAIL("command processor microcode", r);
    if ((r = evergreen_cp_resume(rdev)) != 0) {
        /* what the command processor did: whether it read the ring at all
         * (RPTR), whether it is halted or busy, what the CPU wrote is there */
        char d[96];
        ksnprintf(d, sizeof(d), "ring test failed: RPTR %x WPTR %x ME_CNTL %08x GRBM %08x SRBM %08x PTE0 %08x%s",
                  RREG32(CP_RB_RPTR), RREG32(CP_RB_WPTR), RREG32(CP_ME_CNTL), RREG32(GRBM_STATUS),
                  RREG32(SRBM_STATUS), (uint32_t)g_gart[RING_PAGE], vram_reads_back() ? "" : " (BAR read-back bad)");
        klog("radeon: %s\n", d);
        FAIL(d, r);
    }

    /* a fence: the engine writes the sequence number back when it gets there */
    volatile uint32_t* fence = rdev->fence_drv[0].cpu_addr;
    *fence = 0;
    struct radeon_fence f = { 0, 1 };
    if ((r = radeon_ring_lock(rdev, ring, 16)) != 0) FAIL("ring lock", r);
    r600_fence_ring_emit(rdev, &f);
    radeon_ring_unlock_commit(rdev, ring, false);
    if (!(g_fence_us = wait_fence(fence, 1))) FAIL("fence not written back", -ETIMEDOUT);

    /* an indirect buffer in system memory, through the GART: it sets a scratch register */
    u32 scratch;
    if ((r = radeon_scratch_get(rdev, &scratch)) != 0) FAIL("scratch register", r);
    WREG32(scratch, 0xCAFEDEAD);
    u32* ib = kmalloc_aligned(4096, 4096);
    if (!ib) FAIL("out of memory", -ENOMEM);
    ib[0] = PACKET3(PACKET3_SET_CONFIG_REG, 1);
    ib[1] = (scratch - PACKET3_SET_CONFIG_REG_START) >> 2;
    ib[2] = 0xDEADBEEF;
    for (int i = 3; i < 16; i++) ib[i] = RADEON_CP_PACKET2;
    uint64_t pa = RAD_PHYS(ib);
    uint32_t page = IB_TEST_PAGE;
    struct radeon_ib rib = { 0, rad_gart_bind(page, &pa, 1), ib, 3, NULL, false, 0 };
    f.seq = 2;
    uint64_t t0 = timer_counter();
    if ((r = radeon_ring_lock(rdev, ring, 64)) != 0) { kfree(ib); FAIL("ring lock", r); }
    evergreen_ring_ib_execute(rdev, &rib);
    r600_fence_ring_emit(rdev, &f);
    radeon_ring_unlock_commit(rdev, ring, false);
    uint32_t got = wait_fence(fence, 2), v = RREG32(scratch);
    g_ib_us = (uint32_t)((timer_counter() - t0) / 1000u);
    rad_gart_unbind(page, 1);
    kfree(ib);
    radeon_scratch_free(rdev, scratch);
    if (!got) FAIL("the indirect buffer's fence not written back", -ETIMEDOUT);
    if (v != 0xDEADBEEF) {
        ksnprintf(why, (size_t)cap, "scratch %08x after the indirect buffer", v);
        FAIL(why, -EINVAL);
    }

    g_seq = 2;
    /* how much video memory is really there: cards sold with more than they carry
     * (a BIOS saying 2 GiB on 1 GiB of chips) repeat the memory past its end */
    g_bios_mb = (uint32_t)mb;
    g_real_mb = probe_vram(rdev, g_bios_mb);
    if (!g_real_mb) FAIL("the video memory probe's fence not written back", -ETIMEDOUT);
    rdev->mc.real_vram_size = (uint64_t)g_real_mb << 20;
    uint32_t mtype = (RREG32(0x2a00) >> 28) & 0xF;           /* (MC_SEQ_MISC0) */

    g_state = 1;
    char mem[64];
    if (g_real_mb == g_bios_mb) ksnprintf(mem, sizeof(mem), "%u MiB %s", g_real_mb, mtype == 5 ? "GDDR5" : "DDR3");
    else ksnprintf(mem, sizeof(mem), "%u MiB %s (BIOS says %u!)", g_real_mb, mtype == 5 ? "GDDR5" : "DDR3", g_bios_mb);
    ksnprintf(g_status, sizeof(g_status), "up: %s, %u SIMDs, %u RB; %s; ring %u us, fence %u us, GART IB %u us",
              rad_family_name(rt->family), rdev->config.evergreen.max_simds, rdev->config.evergreen.max_backends, mem,
              g_ring_us, g_fence_us, g_ib_us);
    if (g_real_mb != g_bios_mb)
        klog("radeon: WARNING: the video BIOS says %u MiB, but the memory repeats after %u MiB: only that is used\n",
             g_bios_mb, g_real_mb);
    klog("radeon: 3D engine %s\n", g_status);
    kstrlcpy(why, g_status, (size_t)cap);
    return 0;
}
