/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The radeon interface for apps - what Mesa's r600 driver (its radeon
 * winsys) asks Linux's radeon driver for, with Linux's request numbers and
 * structures (third_party/radeon/radeon_drm.h), on the Radeon HD 5000 -
 * 7000 whose 3D engine kernel/radeon_gfx.c runs. An app's SDK sends them
 * through the drm_ioctl system call of a /dev/dri/card0 it opened, as for
 * Intel GPUs (kernel/gpu_gem.c hands them over when a Radeon is driven).
 *
 *  - buffers (GEM objects) in video memory - the part the CPU reaches
 *    through the BAR, above what the display uses and below the engine's
 *    own area; when that is full, or for buffers the CPU never maps, the
 *    rest of it - or in the GTT: pages of RAM bound in the GART. Mapped
 *    into the app on GEM_MMAP (video memory write-combining); Banana OS's
 *    winsys takes the address GEM_MMAP returns (Linux returns an offset to
 *    mmap the device file at).
 *  - submissions (CS): the app's command stream copied in and checked by
 *    Linux's checker (third_party/radeon/radeon_cs_check.c) - what it may
 *    write, and its buffers' addresses patched in (relocations: TeraScale
 *    2 has no address space per process) - then run as an indirect buffer
 *    from a pool in the GTT, a fence after it; every buffer remembers the
 *    last fence that uses it (waits, "busy", closing one the GPU still
 *    uses: freed once the GPU is done).
 * Everything is the app's (owner): another app's handles do not exist for
 * it, and what it has is released when it ends. No async DMA ring, no UVD,
 * no virtual memory (Mesa's r600 asks for none of them on TeraScale 2 when
 * the kernel says so). */
#include "radeon.h"
#include "gpu.h"
#include "gpu_i915.h"
#include "kheap.h"
#include "kstring.h"
#include "serial.h"
#include "spinlock.h"
#include "task.h"
#include "timer.h"
#include "upage.h"
#include "paging.h"
#include "uvm.h"
#include "../third_party/radeon/radeon_cs_shim.h"

#ifndef RAD_PHYS
#define RAD_PHYS(p) ((uint64_t)(uintptr_t)(p))
#define RAD_VIRT(a) ((void*)(uintptr_t)(a))
#endif

#if defined(__x86_64__) || defined(RADEON_MODEL)

int app_user_ok(const void* p, uint64_t n);
uspace_t* app_user_space(void);

#define E_NOENT    2
#define E_IO       5
#define E_NOMEM   12
#define E_ACCES   13
#define E_FAULT   14
#define E_BUSY    16
#define E_NODEV   19
#define E_INVAL   22
#define E_NOSPC   28
#define E_NOTTY   25

/* (Linux's radeon_drm.h, the parts used - the kernel has no <drm/drm.h>) */
#define DRM_NR(req)    ((unsigned)(req) & 0xFF)
#define DRM_TYPE(req)  (((unsigned)(req) >> 8) & 0xFF)
#define DRM_SIZE(req)  (((unsigned)(req) >> 16) & 0x3FFF)
#define DRM_COMMAND_BASE 0x40
#define NR_VERSION 0x00
#define NR_GEM_CLOSE 0x09
#define NR_GET_CAP 0x0C
#define NR_SET_CLIENT_CAP 0x0D
#define RD_GEM_INFO 0x1c
#define RD_GEM_CREATE 0x1d
#define RD_GEM_MMAP 0x1e
#define RD_GEM_SET_DOMAIN 0x23
#define RD_GEM_WAIT_IDLE 0x24
#define RD_CS 0x26
#define RD_INFO 0x27
#define RD_GEM_SET_TILING 0x28
#define RD_GEM_GET_TILING 0x29
#define RD_GEM_BUSY 0x2a
#define RD_GEM_VA 0x2b
#define RD_GEM_OP 0x2c
#define RD_GEM_USERPTR 0x2d
#define DOMAIN_CPU  0x1
#define DOMAIN_GTT  0x2
#define DOMAIN_VRAM 0x4
#define GEM_NO_CPU_ACCESS (1u << 4)
#define CHUNK_RELOCS 0x01
#define CHUNK_IB     0x02
#define CHUNK_FLAGS  0x03
#define CHUNK_CONST_IB 0x04
#define CS_RING_GFX 0
#define OP_GET_INITIAL_DOMAIN 0
#define OP_SET_INITIAL_DOMAIN 1
enum {
    INFO_DEVICE_ID = 0x00, INFO_NUM_GB_PIPES = 0x01, INFO_NUM_Z_PIPES = 0x02, INFO_ACCEL_WORKING = 0x03,
    INFO_CRTC_FROM_ID = 0x04, INFO_ACCEL_WORKING2 = 0x05, INFO_TILING_CONFIG = 0x06, INFO_WANT_HYPERZ = 0x07,
    INFO_WANT_CMASK = 0x08, INFO_CLOCK_CRYSTAL_FREQ = 0x09, INFO_NUM_BACKENDS = 0x0a, INFO_NUM_TILE_PIPES = 0x0b,
    INFO_FUSION_GART_WORKING = 0x0c, INFO_BACKEND_MAP = 0x0d, INFO_VA_START = 0x0e, INFO_IB_VM_MAX_SIZE = 0x0f,
    INFO_MAX_PIPES = 0x10, INFO_TIMESTAMP = 0x11, INFO_MAX_SE = 0x12, INFO_MAX_SH_PER_SE = 0x13,
    INFO_FASTFB_WORKING = 0x14, INFO_RING_WORKING = 0x15, INFO_SI_TILE_MODE_ARRAY = 0x16,
    INFO_SI_CP_DMA_COMPUTE = 0x17, INFO_CIK_MACROTILE_MODE_ARRAY = 0x18, INFO_SI_BACKEND_ENABLED_MASK = 0x19,
    INFO_MAX_SCLK = 0x1a, INFO_VCE_FW_VERSION = 0x1b, INFO_VCE_FB_VERSION = 0x1c, INFO_NUM_BYTES_MOVED = 0x1d,
    INFO_VRAM_USAGE = 0x1e, INFO_GTT_USAGE = 0x1f, INFO_ACTIVE_CU_COUNT = 0x20, INFO_CURRENT_GPU_TEMP = 0x21,
    INFO_CURRENT_GPU_SCLK = 0x22, INFO_CURRENT_GPU_MCLK = 0x23, INFO_READ_REG = 0x24, INFO_VA_UNMAP_WORKING = 0x25,
    INFO_GPU_RESET_COUNTER = 0x26,
};
struct drm_version_k {
    int32_t  version_major, version_minor, version_patchlevel;
    uint64_t name_len, name, date_len, date, desc_len, desc;
};
struct gem_close_k { uint32_t handle, pad; };
struct get_cap_k { uint64_t capability, value; };
struct gem_info_k { uint64_t gart_size, vram_size, vram_visible; };
struct gem_create_k { uint64_t size, alignment; uint32_t handle, initial_domain, flags; };
struct gem_mmap_k { uint32_t handle, pad; uint64_t offset, size, addr_ptr; };
struct gem_set_domain_k { uint32_t handle, read_domains, write_domain; };
struct gem_wait_idle_k { uint32_t handle, pad; };
struct gem_busy_k { uint32_t handle, domain; };
struct gem_tiling_k { uint32_t handle, tiling_flags, pitch; };
struct gem_op_k { uint32_t handle, op; uint64_t value; };
struct cs_chunk_k { uint32_t chunk_id, length_dw; uint64_t chunk_data; };
struct cs_k { uint32_t num_chunks, cs_id; uint64_t chunks, gart_limit, vram_limit; };
struct cs_reloc_k { uint32_t handle, read_domains, write_domain, flags; };
struct info_k { uint32_t request, pad; uint64_t value; };

/* ── buffers ───────────────────────────────────────────────────────── */
#define OBJ_MAX   16384
#define MAPS      4
#define PAGE      4096u
#define IB_POOL_PAGES 1024u             /* 4 MiB of indirect buffers in the GTT */
#define IB_MAX_DW (64u * 1024u)         /* (Mesa's largest command stream) */
#define RELOC_MAX 4096u

typedef struct {
    int       owner;                    /* 0: a free slot; -1: closed, waiting for the GPU */
    uint32_t  domain;                   /* DOMAIN_VRAM or DOMAIN_GTT: where it is */
    uint32_t  initial_domain;           /* what the app asked for */
    uint32_t  pages;
    uint64_t  vram_off;                 /* VRAM: its offset in video memory */
    uint32_t  gtt_page;                 /* GTT: its first GART page */
    uint64_t* phys;                     /* GTT: its pages; VRAM: the frames it is mapped from */
    uint32_t  last_read, last_write;    /* fences */
    uint32_t  tiling, pitch;
    struct radeon_bo bo;                /* (its size, for the checker) */
    struct { uintptr_t addr; uint32_t pages; uint64_t frame0; } map[MAPS];
} obj_t;

static obj_t*    g_obj;
static uint32_t  g_nobj;
/* (one app's request at a time; a submission may wait for the GPU, so
 * waiting for the lock yields instead of spinning) */
static volatile int g_lock;
static void lock(void) { while (__sync_lock_test_and_set(&g_lock, 1)) task_yield(); }
static void unlock(void) { __sync_lock_release(&g_lock); }
static radeon_t* g_card;
static struct radeon_device* g_rd;
static uintptr_t g_vbar_cpu;            /* the video memory BAR (CPU address of offset 0) */
static uint64_t  g_vis_lo, g_vis_hi;    /* apps' buffers the CPU reaches: [lo, hi) */
static uint64_t  g_inv_lo, g_inv_hi;    /* ... and those it does not: the rest of video memory */
static uint8_t*  g_vram_map;            /* a bit per 4 KiB page of video memory: used */
static uint8_t*  g_gtt_map;             /* a bit per GART page: used */
static uint32_t  g_gtt_first, g_gtt_pages;
static uint64_t  g_vram_used, g_gtt_used;
static uint32_t  g_submits, g_rejected;

/* the IB pool: a ring of pages bound in the GART; each IB's fence frees its part */
static uint8_t*  g_ib_pool;
static uint64_t  g_ib_gpu;
static uint32_t  g_ib_head;             /* next free page */
static struct { uint32_t page, pages, seq; } g_ib_live[64];
static int g_ib_n;

static int bit(const uint8_t* m, uint32_t i) { return (m[i >> 3] >> (i & 7)) & 1; }
static void set_bits(uint8_t* m, uint32_t i, uint32_t n, int v) {
    for (uint32_t k = i; k < i + n; k++) { if (v) m[k >> 3] |= (uint8_t)(1u << (k & 7)); else m[k >> 3] &= (uint8_t)~(1u << (k & 7)); }
}
/* the first run of n free pages in [lo, hi), aligned to al pages: its first page, or -1 */
static int64_t find_run(const uint8_t* m, uint32_t lo, uint32_t hi, uint32_t n, uint32_t al) {
    if (!al) al = 1;
    for (uint32_t i = (lo + al - 1) / al * al; i + n <= hi; ) {
        uint32_t k = 0;
        while (k < n && !bit(m, i + k)) k++;
        if (k == n) return i;
        i = (i + k + 1 + al - 1) / al * al;
    }
    return -1;
}

static int u_ok(uint64_t p, uint64_t n) { return n == 0 || (p && app_user_ok((const void*)(uintptr_t)p, n)); }

static int ready(void) {
    if (g_rd) return 0;
    g_card = gpu_radeon_card();
    if (!g_card) return -E_NODEV;
    if (!rad_gfx_ready()) {
        char why[200];
        if (gpu_radeon_3d(why, sizeof(why)) != 0) {
            klog("radeon drm: the 3D engine does not start: %s\n", why);
            return -E_NODEV;
        }
    }
    struct radeon_device* rd = rad_gfx_rdev();
    if (!rd) return -E_NODEV;
    g_obj = (obj_t*)kzalloc(sizeof(obj_t) * OBJ_MAX);
    uint64_t vram = (uint64_t)rad_vram_mb() << 20;
    uint32_t vpages = (uint32_t)(vram / PAGE);
    g_vram_map = (uint8_t*)kzalloc(vpages / 8 + 1);
    g_gtt_pages = (uint32_t)(rd->mc.gtt_size / PAGE);
    g_gtt_map = (uint8_t*)kzalloc(g_gtt_pages / 8 + 1);
    g_ib_pool = (uint8_t*)kmalloc_aligned(IB_POOL_PAGES * PAGE, PAGE);
    if (!g_obj || !g_vram_map || !g_gtt_map || !g_ib_pool) {
        kfree(g_obj); kfree(g_vram_map); kfree(g_gtt_map); kfree(g_ib_pool);
        g_obj = NULL; g_vram_map = g_gtt_map = g_ib_pool = NULL;
        return -E_NOMEM;
    }
    /* video memory for apps: above the display, below the engine's area; the rest past the window */
    g_vbar_cpu = (uintptr_t)rad_gfx_vram_cpu();
    g_vis_lo = gpu_radeon_display_end();
    g_vis_hi = rad_gfx_area_off();
    if (g_vis_hi < g_vis_lo) g_vis_hi = g_vis_lo;
    g_inv_lo = rd->mc.visible_vram_size;
    g_inv_hi = vram > g_inv_lo ? vram : g_inv_lo;
    if (g_vis_hi > g_vis_lo && !mmio_map(g_vbar_cpu + g_vis_lo, g_vis_hi - g_vis_lo)) g_vis_hi = g_vis_lo;
    /* the GTT: the kernel's pages first, then the IB pool, then apps' buffers */
    uint32_t rsv = rad_gfx_reserved_gtt_pages();
    set_bits(g_gtt_map, 0, rsv, 1);
    uint64_t phys[64];
    for (uint32_t i = 0; i < IB_POOL_PAGES; i += 64) {
        for (uint32_t k = 0; k < 64; k++) phys[k] = RAD_PHYS(g_ib_pool + (i + k) * PAGE);
        if (i == 0) g_ib_gpu = rad_gart_bind(rsv, phys, 64);
        else rad_gart_bind(rsv + i, phys, 64);
    }
    set_bits(g_gtt_map, rsv, IB_POOL_PAGES, 1);
    g_gtt_first = rsv + IB_POOL_PAGES;
    g_rd = rd;
    klog("radeon drm: video memory for apps %llu MiB (CPU-visible) + %llu MiB, GTT %u MiB\n",
         (unsigned long long)((g_vis_hi - g_vis_lo) >> 20), (unsigned long long)((g_inv_hi - g_inv_lo) >> 20),
         (g_gtt_pages - g_gtt_first) >> 8);
    return 0;
}

static obj_t* lookup(int owner, uint32_t h) {
    if (!h || h > g_nobj) return NULL;
    obj_t* o = &g_obj[h - 1];
    return o->owner == owner ? o : NULL;
}

static uint64_t obj_gpu_addr(const obj_t* o) {
    return o->domain == DOMAIN_VRAM ? g_rd->mc.vram_start + o->vram_off : g_rd->mc.gtt_start + (uint64_t)o->gtt_page * PAGE;
}

static void obj_free(obj_t* o) {
    if (o->domain == DOMAIN_VRAM) {
        set_bits(g_vram_map, (uint32_t)(o->vram_off / PAGE), o->pages, 0);
        g_vram_used -= (uint64_t)o->pages * PAGE;
    } else if (o->domain == DOMAIN_GTT) {
        rad_gart_unbind(o->gtt_page, o->pages);
        set_bits(g_gtt_map, o->gtt_page, o->pages, 0);
        for (uint32_t i = 0; i < o->pages; i++) upage_free(RAD_VIRT(o->phys[i]));
        g_gtt_used -= (uint64_t)o->pages * PAGE;
    }
    kfree(o->phys);
    memset(o, 0, sizeof(*o));
}

static void retire(void) {
    for (uint32_t i = 0; i < g_nobj; i++)
        if (g_obj[i].owner == -1 && rad_gfx_seq_done(g_obj[i].last_read) && rad_gfx_seq_done(g_obj[i].last_write))
            obj_free(&g_obj[i]);
    while (g_nobj && !g_obj[g_nobj - 1].owner) g_nobj--;
}

/* in video memory (the part the CPU reaches; else past it, if the CPU never maps it): 0, or -1 */
static int place_vram(obj_t* o, uint32_t n, uint32_t al, int cpu) {
    int64_t at = find_run(g_vram_map, (uint32_t)(g_vis_lo / PAGE), (uint32_t)(g_vis_hi / PAGE), n, al);
    if (at < 0 && !cpu) at = find_run(g_vram_map, (uint32_t)(g_inv_lo / PAGE), (uint32_t)(g_inv_hi / PAGE), n, al);
    if (at < 0) return -1;
    set_bits(g_vram_map, (uint32_t)at, n, 1);
    o->domain = DOMAIN_VRAM;
    o->vram_off = (uint64_t)at * PAGE;
    o->phys = (uint64_t*)kmalloc(sizeof(uint64_t) * n);
    if (!o->phys) { set_bits(g_vram_map, (uint32_t)at, n, 0); return -1; }
    for (uint32_t i = 0; i < n; i++) o->phys[i] = (uint64_t)(g_vbar_cpu + o->vram_off) + (uint64_t)i * PAGE;
    /* (cleared, as Linux's: an app never sees another's leftovers) */
    if (o->vram_off + (uint64_t)n * PAGE <= g_vis_hi) memset((void*)(g_vbar_cpu + (uintptr_t)o->vram_off), 0, (size_t)n * PAGE);
    g_vram_used += (uint64_t)n * PAGE;
    return 0;
}

/* in the GTT: RAM pages bound in the GART: 0, or -1 */
static int place_gtt(obj_t* o, uint32_t n, uint32_t al) {
    int64_t at = find_run(g_gtt_map, g_gtt_first, g_gtt_pages, n, al);
    if (at < 0) return -1;
    o->phys = (uint64_t*)kmalloc(sizeof(uint64_t) * n);
    if (!o->phys) return -1;
    for (uint32_t i = 0; i < n; i++) {
        void* pg = upage_alloc();
        if (!pg) { for (uint32_t k = 0; k < i; k++) upage_free(RAD_VIRT(o->phys[k])); kfree(o->phys); o->phys = NULL; return -1; }
        memset(pg, 0, PAGE);
        o->phys[i] = RAD_PHYS(pg);
    }
    set_bits(g_gtt_map, (uint32_t)at, n, 1);
    o->domain = DOMAIN_GTT;
    o->gtt_page = (uint32_t)at;
    rad_gart_bind((uint32_t)at, o->phys, n);
    g_gtt_used += (uint64_t)n * PAGE;
    return 0;
}

static int obj_create(int owner, struct gem_create_k* c) {
    if (!c->size || c->size > (512ull << 20)) return -E_INVAL;
    uint32_t n = (uint32_t)((c->size + PAGE - 1) / PAGE);
    uint64_t al = c->alignment < PAGE ? PAGE : c->alignment;
    if (al & (al - 1) || al > (16u << 20)) return -E_INVAL;
    uint32_t slot = 0;
    while (slot < g_nobj && g_obj[slot].owner) slot++;
    if (slot == g_nobj) { retire(); slot = 0; while (slot < g_nobj && g_obj[slot].owner) slot++; }
    if (slot >= OBJ_MAX) return -E_NOSPC;
    obj_t* o = &g_obj[slot];
    memset(o, 0, sizeof(*o));
    uint32_t want = c->initial_domain & (DOMAIN_VRAM | DOMAIN_GTT | DOMAIN_CPU);
    int cpu = !(c->flags & GEM_NO_CPU_ACCESS);
    int ok = -1;
    if (want & DOMAIN_VRAM) ok = place_vram(o, n, (uint32_t)(al / PAGE), cpu);
    if (ok) { retire(); if (want & DOMAIN_VRAM) ok = place_vram(o, n, (uint32_t)(al / PAGE), cpu); }
    if (ok) ok = place_gtt(o, n, (uint32_t)(al / PAGE));        /* (GTT asked for, or video memory full: as Linux) */
    if (ok) return -E_NOMEM;
    o->owner = owner;
    o->pages = n;
    o->initial_domain = want ? want : DOMAIN_GTT;
    o->bo.size = (uint64_t)n * PAGE;
    if (slot == g_nobj) g_nobj++;
    c->handle = slot + 1;
    return 0;
}

static void obj_unmap(obj_t* o, uspace_t* us) {
    for (int k = 0; k < MAPS; k++) {
        if (!o->map[k].addr) continue;
        if (us) uvm_unmap_frames(us, o->map[k].addr, o->map[k].pages, o->map[k].frame0);
        o->map[k].addr = 0;
    }
}

static void obj_close(obj_t* o, uspace_t* us) {
    obj_unmap(o, us);
    if (rad_gfx_seq_done(o->last_read) && rad_gfx_seq_done(o->last_write)) obj_free(o);
    else o->owner = -1;
}

static int obj_busy(const obj_t* o) { return !rad_gfx_seq_done(o->last_read) || !rad_gfx_seq_done(o->last_write); }

static int obj_wait(const obj_t* o) {
    uint32_t a = o->last_read, b = o->last_write;
    uint32_t last = (int32_t)(a - b) > 0 ? a : b;
    return rad_gfx_wait(last, 10000) ? -E_BUSY : 0;
}

/* ── submissions ───────────────────────────────────────────────────── */

/* n pages of the IB pool, free once the GPU has run what was there: the first, or -1 */
static int64_t ib_alloc(uint32_t n) {
    for (int tries = 0; tries < 2; tries++) {
        /* (the live ones done: forgotten) */
        int k = 0;
        for (int i = 0; i < g_ib_n; i++) if (!rad_gfx_seq_done(g_ib_live[i].seq)) g_ib_live[k++] = g_ib_live[i];
        g_ib_n = k;
        uint32_t at = g_ib_head + n <= IB_POOL_PAGES ? g_ib_head : 0;
        int clash = 0;
        for (int i = 0; i < g_ib_n; i++) {
            uint32_t p = g_ib_live[i].page, q = p + g_ib_live[i].pages;
            if (at < q && p < at + n) clash = 1;
        }
        if (!clash && g_ib_n < 64) return at;
        /* the oldest finished first */
        if (g_ib_n && rad_gfx_wait(g_ib_live[0].seq, 5000)) return -1;
    }
    return -1;
}

static int cs_submit(int owner, struct cs_k* cs) {
    if (!cs->num_chunks || cs->num_chunks > 8 || !u_ok(cs->chunks, (uint64_t)cs->num_chunks * 8)) return -E_INVAL;
    uint64_t ptrs[8];
    memcpy(ptrs, (const void*)(uintptr_t)cs->chunks, (size_t)cs->num_chunks * 8);
    struct cs_chunk_k ch[8];
    struct cs_chunk_k *ib = NULL, *rel = NULL, *fl = NULL;
    for (uint32_t i = 0; i < cs->num_chunks; i++) {
        if (!u_ok(ptrs[i], sizeof(ch[i]))) return -E_FAULT;
        memcpy(&ch[i], (const void*)(uintptr_t)ptrs[i], sizeof(ch[i]));
        if (ch[i].chunk_id == CHUNK_IB) ib = &ch[i];
        else if (ch[i].chunk_id == CHUNK_RELOCS) rel = &ch[i];
        else if (ch[i].chunk_id == CHUNK_FLAGS) fl = &ch[i];
        else if (ch[i].chunk_id == CHUNK_CONST_IB) return -E_INVAL;     /* (SI+) */
    }
    if (!ib || !ib->length_dw || ib->length_dw > IB_MAX_DW || !u_ok(ib->chunk_data, (uint64_t)ib->length_dw * 4)) return -E_INVAL;
    uint32_t flags[3] = { 0, CS_RING_GFX, 0 };
    if (fl) {
        uint32_t n = fl->length_dw < 3 ? fl->length_dw : 3;
        if (!u_ok(fl->chunk_data, (uint64_t)n * 4)) return -E_FAULT;
        memcpy(flags, (const void*)(uintptr_t)fl->chunk_data, (size_t)n * 4);
    }
    if (flags[1] != CS_RING_GFX) return -E_INVAL;              /* (the 3D ring only: no DMA, no UVD) */
    if (flags[0] & RADEON_CS_USE_VM) return -E_INVAL;          /* (no virtual memory on TeraScale 2) */
    uint32_t nrel = rel ? rel->length_dw / 4 : 0;
    if (nrel > RELOC_MAX || (nrel && !u_ok(rel->chunk_data, (uint64_t)nrel * 16))) return -E_INVAL;

    uint32_t* kib = (uint32_t*)kmalloc((size_t)ib->length_dw * 4);
    uint32_t* krel = nrel ? (uint32_t*)kmalloc((size_t)nrel * 16) : NULL;
    struct radeon_bo_list* rl = nrel ? (struct radeon_bo_list*)kzalloc(sizeof(*rl) * nrel) : NULL;
    obj_t** ob = nrel ? (obj_t**)kzalloc(sizeof(obj_t*) * nrel) : NULL;
    int rc = 0;
    if (!kib || (nrel && (!krel || !rl || !ob))) { rc = -E_NOMEM; goto out; }
    memcpy(kib, (const void*)(uintptr_t)ib->chunk_data, (size_t)ib->length_dw * 4);
    if (nrel) memcpy(krel, (const void*)(uintptr_t)rel->chunk_data, (size_t)nrel * 16);

    lock();
    for (uint32_t i = 0; i < nrel && !rc; i++) {
        const struct cs_reloc_k* r = (const struct cs_reloc_k*)&krel[i * 4];
        obj_t* o = lookup(owner, r->handle);
        if (!o) { rc = -E_NOENT; break; }
        ob[i] = o;
        rl[i].robj = &o->bo;
        rl[i].gpu_offset = obj_gpu_addr(o);
        rl[i].tiling_flags = o->tiling;
        rl[i].handle = r->handle;
        rl[i].write = r->write_domain != 0;
        rl[i].allowed_domains = rl[i].preferred_domains = o->domain;
    }
    uint32_t npages = (ib->length_dw * 4 + PAGE - 1) / PAGE;
    int64_t at = rc ? -1 : ib_alloc(npages);
    if (!rc && at < 0) rc = -E_BUSY;
    if (!rc) {
        uint32_t* dst = (uint32_t*)(g_ib_pool + (uint64_t)at * PAGE);
        memcpy(dst, kib, (size_t)ib->length_dw * 4);
        struct radeon_cs_chunk cib = { ib->length_dw, kib, NULL };
        struct radeon_cs_chunk crel = { nrel * 4, krel, NULL };
        struct radeon_cs_parser p;
        memset(&p, 0, sizeof(p));
        p.rdev = g_rd;
        p.family = (unsigned)g_rd->family;
        p.chunk_ib = &cib;
        p.chunk_relocs = nrel ? &crel : NULL;
        p.relocs = rl;
        p.nrelocs = nrel;
        p.cs_flags = flags[0];
        p.ring = flags[1];
        p.ib.ptr = dst;                          /* (the checker patches the copy the GPU runs) */
        p.ib.length_dw = ib->length_dw;
        int pr = evergreen_cs_parse(&p);
        if (pr) { g_rejected++; rc = -E_INVAL; klog("radeon drm: a command stream refused (%d)\n", pr); }
        else {
            uint32_t seq = rad_gfx_submit_ib(g_ib_gpu + (uint64_t)at * PAGE, ib->length_dw);
            if (!seq) rc = -E_IO;
            else {
                g_ib_live[g_ib_n].page = (uint32_t)at;
                g_ib_live[g_ib_n].pages = npages;
                g_ib_live[g_ib_n].seq = seq;
                g_ib_n++;
                g_ib_head = (uint32_t)at + npages;
                if (g_ib_head >= IB_POOL_PAGES) g_ib_head = 0;
                for (uint32_t i = 0; i < nrel; i++) {
                    ob[i]->last_read = seq;
                    if (rl[i].write) ob[i]->last_write = seq;
                }
                g_submits++;
            }
        }
    }
    unlock();
out:
    kfree(kib); kfree(krel); kfree(rl); kfree(ob);
    return rc;
}

/* ── INFO ──────────────────────────────────────────────────────────── */
static int info(struct info_k* in) {
    struct radeon_device* rd = g_rd;
    uint64_t v = 0;
    int wide = 0;
    switch (in->request) {
    case INFO_DEVICE_ID: v = g_card->pci.device; break;
    case INFO_ACCEL_WORKING: case INFO_ACCEL_WORKING2: v = 1; break;
    case INFO_TILING_CONFIG: v = rd->config.evergreen.tile_config; break;
    case INFO_CLOCK_CRYSTAL_FREQ: v = g_card->clk.ref_freq ? g_card->clk.ref_freq * 10u : 27000; break;   /* (kHz) */
    case INFO_NUM_BACKENDS: v = rd->config.evergreen.max_backends; break;
    case INFO_NUM_TILE_PIPES: v = rd->config.evergreen.max_tile_pipes; break;
    case INFO_BACKEND_MAP: v = rd->config.evergreen.backend_map; break;
    case INFO_MAX_PIPES: v = rd->config.evergreen.max_pipes; break;
    case INFO_MAX_SE: v = rd->config.evergreen.num_ses; break;
    case INFO_MAX_SH_PER_SE: v = 1; break;
    case INFO_ACTIVE_CU_COUNT: v = rd->config.evergreen.active_simds; break;
    case INFO_MAX_SCLK: v = 625000; break;                    /* (kHz: the HD 6450 / 7450's engine clock) */
    case INFO_WANT_HYPERZ: case INFO_WANT_CMASK: v = 0; break; /* (r300's: not granted) */
    case INFO_FASTFB_WORKING: v = 0; break;
    case INFO_VRAM_USAGE: v = g_vram_used; wide = 1; break;
    case INFO_GTT_USAGE: v = g_gtt_used; wide = 1; break;
    case INFO_NUM_BYTES_MOVED: v = 0; wide = 1; break;
    case INFO_GPU_RESET_COUNTER: v = 0; break;
    case INFO_CURRENT_GPU_TEMP: case INFO_CURRENT_GPU_SCLK: case INFO_CURRENT_GPU_MCLK: v = 0; break;
    case INFO_TIMESTAMP: v = timer_counter(); wide = 1; break;
    case INFO_READ_REG: {
        if (!u_ok(in->value, 4)) return -E_FAULT;
        uint32_t reg = *(uint32_t*)(uintptr_t)in->value;
        /* (as Linux's evergreen_get_allowed_info_register: status registers only) */
        if (reg != 0x8010 && reg != 0x8014 && reg != 0x8018 && reg != 0x0E50 && reg != 0x0EC4 && reg != 0xD034 && reg != 0xD834)
            return -E_INVAL;
        v = rad_rd(g_card, reg);
        break;
    }
    /* no virtual memory, no rings but the 3D one, no SI / CIK tables */
    default: return -E_INVAL;
    }
    if (!u_ok(in->value, wide ? 8 : 4)) return -E_FAULT;
    if (wide) *(uint64_t*)(uintptr_t)in->value = v;
    else *(uint32_t*)(uintptr_t)in->value = (uint32_t)v;
    return 0;
}

static int copy_str(uint64_t len, uint64_t dst, const char* s) {
    uint64_t n = strlen(s);
    if (dst && len) {
        uint64_t c = n < len ? n : len;
        if (!u_ok(dst, c)) return -1;
        memcpy((void*)(uintptr_t)dst, s, (size_t)c);
    }
    return (int)n;
}

static int ioctl_radeon(int owner, unsigned nr, void* arg, uspace_t* us) {
    switch (nr) {
    case RD_GEM_INFO: {
        struct gem_info_k* g = (struct gem_info_k*)arg;
        g->vram_size = (uint64_t)rad_vram_mb() << 20;
        g->vram_visible = g_vis_hi - g_vis_lo;
        g->gart_size = (uint64_t)(g_gtt_pages - g_gtt_first) * PAGE;
        return 0;
    }
    case RD_GEM_CREATE: return obj_create(owner, (struct gem_create_k*)arg);
    case RD_GEM_MMAP: {
        struct gem_mmap_k* m = (struct gem_mmap_k*)arg;
        obj_t* o = lookup(owner, m->handle);
        if (!o) return -E_NOENT;
        if (o->domain == DOMAIN_VRAM && o->vram_off + (uint64_t)o->pages * PAGE > g_vis_hi) return -E_INVAL;   /* (past the window) */
        uint64_t off = m->offset, size = m->size ? m->size : (uint64_t)o->pages * PAGE - off;
        if ((off & (PAGE - 1)) || off + size > (uint64_t)o->pages * PAGE || !us) return -E_INVAL;
        uint32_t first = (uint32_t)(off / PAGE), pages = (uint32_t)((size + PAGE - 1) / PAGE);
        int slot = -1;
        for (int k = 0; k < MAPS && slot < 0; k++) if (!o->map[k].addr) slot = k;
        uintptr_t a = uvm_map_frames(us, o->phys + first, pages, UVM_READ | UVM_WRITE, o->domain == DOMAIN_VRAM);
        if (!a) return -E_NOMEM;
        if (slot >= 0) { o->map[slot].addr = a; o->map[slot].pages = pages; o->map[slot].frame0 = o->phys[first]; }
        m->addr_ptr = (uint64_t)a;
        return 0;
    }
    case RD_GEM_SET_DOMAIN: {
        obj_t* o = lookup(owner, ((struct gem_set_domain_k*)arg)->handle);
        if (!o) return -E_NOENT;
        return obj_wait(o);                                         /* (the CPU's turn: the GPU done with it) */
    }
    case RD_GEM_WAIT_IDLE: {
        obj_t* o = lookup(owner, ((struct gem_wait_idle_k*)arg)->handle);
        if (!o) return -E_NOENT;
        return obj_wait(o);
    }
    case RD_GEM_BUSY: {
        struct gem_busy_k* b = (struct gem_busy_k*)arg;
        obj_t* o = lookup(owner, b->handle);
        if (!o) return -E_NOENT;
        b->domain = o->domain;
        return obj_busy(o) ? -E_BUSY : 0;
    }
    case RD_GEM_SET_TILING: {
        struct gem_tiling_k* t = (struct gem_tiling_k*)arg;
        obj_t* o = lookup(owner, t->handle);
        if (!o) return -E_NOENT;
        o->tiling = t->tiling_flags;
        o->pitch = t->pitch;
        return 0;
    }
    case RD_GEM_GET_TILING: {
        struct gem_tiling_k* t = (struct gem_tiling_k*)arg;
        obj_t* o = lookup(owner, t->handle);
        if (!o) return -E_NOENT;
        t->tiling_flags = o->tiling;
        t->pitch = o->pitch;
        return 0;
    }
    case RD_GEM_OP: {
        struct gem_op_k* op = (struct gem_op_k*)arg;
        obj_t* o = lookup(owner, op->handle);
        if (!o) return -E_NOENT;
        if (op->op == OP_GET_INITIAL_DOMAIN) { op->value = o->initial_domain; return 0; }
        if (op->op == OP_SET_INITIAL_DOMAIN) { o->initial_domain = (uint32_t)op->value; return 0; }
        return -E_INVAL;
    }
    case RD_CS: return cs_submit(owner, (struct cs_k*)arg);
    case RD_INFO: return info((struct info_k*)arg);
    case RD_GEM_USERPTR: return -E_INVAL;          /* (not there: Mesa then copies) */
    case RD_GEM_VA: return -E_INVAL;
    default: return -E_INVAL;
    }
}

int rad_drm_ioctl(int owner, unsigned long req, void* arg) {
    if (DRM_TYPE(req) != 'd') return -E_NOTTY;
    unsigned nr = DRM_NR(req), size = DRM_SIZE(req);
    if (size > 256 || (size && !u_ok((uint64_t)(uintptr_t)arg, size))) return -E_FAULT;
    int r = ready();
    if (r) return r;
    uint64_t k[32];
    memset(k, 0, sizeof(k));
    if (size) memcpy(k, arg, size);
    uspace_t* us = app_user_space();
    if (nr >= DRM_COMMAND_BASE && nr < DRM_COMMAND_BASE + 0x40) {
        if (nr - DRM_COMMAND_BASE == RD_CS) r = ioctl_radeon(owner, nr - DRM_COMMAND_BASE, k, us);
        else { lock(); r = ioctl_radeon(owner, nr - DRM_COMMAND_BASE, k, us); unlock(); }
    } else switch (nr) {
    case NR_VERSION: {
        struct drm_version_k* v = (struct drm_version_k*)k;
        v->version_major = 2; v->version_minor = 50; v->version_patchlevel = 0;   /* (what Mesa's r600 asks for: Linux 4.12's) */
        int a = copy_str(v->name_len, v->name, "radeon");
        int b = copy_str(v->date_len, v->date, "20080528");
        int c = copy_str(v->desc_len, v->desc, "AMD Radeon (Banana OS)");
        if (a < 0 || b < 0 || c < 0) { r = -E_FAULT; break; }
        v->name_len = (uint64_t)a; v->date_len = (uint64_t)b; v->desc_len = (uint64_t)c;
        r = 0;
        break;
    }
    case NR_GEM_CLOSE: {
        lock();
        obj_t* o = lookup(owner, (uint32_t)k[0]);                 /* (struct drm_gem_close: its handle) */
        if (!o) r = -E_NOENT;
        else { obj_close(o, us); r = 0; }
        unlock();
        break;
    }
    case NR_GET_CAP: k[1] = 0; r = 0; break;             /* (its value: none of the capabilities) */
    case NR_SET_CLIENT_CAP: r = 0; break;
    default: r = -E_INVAL;
    }
    if (size && ((req >> 30) & 2)) memcpy(arg, k, size);
    return r;
}

int rad_drm_active(void) { return gpu_radeon_card() != NULL; }

void rad_drm_release(int owner) {
    if (!g_obj) return;
    lock();
    for (uint32_t i = 0; i < g_nobj; i++)
        if (g_obj[i].owner == owner) {
            memset(g_obj[i].map, 0, sizeof(g_obj[i].map));          /* (its address space is going away) */
            obj_close(&g_obj[i], NULL);
        }
    retire();
    unlock();
}

void rad_drm_stats(char* buf, int cap) {
    uint32_t live = 0;
    for (uint32_t i = 0; g_obj && i < g_nobj; i++) if (g_obj[i].owner > 0) live++;
    ksnprintf(buf, (size_t)cap, "%u buffers, video memory %llu KiB, GTT %llu KiB; %u submissions, %u refused by the checker",
              live, (unsigned long long)(g_vram_used >> 10), (unsigned long long)(g_gtt_used >> 10), g_submits, g_rejected);
}

#else
int  rad_drm_ioctl(int owner, unsigned long req, void* arg) { (void)owner; (void)req; (void)arg; return -19; }
int  rad_drm_active(void) { return 0; }
void rad_drm_release(int owner) { (void)owner; }
void rad_drm_stats(char* buf, int cap) { ksnprintf(buf, (size_t)cap, "64-bit Banana OS only"); }
#endif
