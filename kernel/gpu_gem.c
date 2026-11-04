/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The i915 interface for apps (kernel/gpu_i915.h): what Mesa's crocus
 * driver asks Linux for, with Linux's request numbers and structures
 * (kernel/i915_uapi.h) - an app's SDK sends them through the drm_ioctl
 * system call of a /dev/dri/card0 it opened.
 *
 *  - buffers (GEM objects): pages of RAM, each bound for its life at a
 *    place in the GPU's address space (the global GTT, and the PPGTT that
 *    mirrors it) - so the offsets Mesa presumes stay right and relocations
 *    are written only the first time; mapped into the app (write-back, or
 *    write-combining on Ironlake, which has no shared cache). Gen 8+: each
 *    app has an address space of its own (48 bits), a buffer bound in it
 *    where the app's submissions say ("softpin": Mesa's iris places them
 *    itself), else at the place the kernel gave it;
 *  - submissions (execbuffer2): the relocations written, the batch run on
 *    the render ring; every buffer remembers the last seqno that reads or
 *    writes it, for waits, "busy" and closing a buffer the GPU still uses
 *    (it is freed once the GPU is done with it);
 *  - contexts: each a hardware context of its own from gen 6 (its state
 *    saved and restored by the GPU; an app's batches without one go in a
 *    default context of the app's), and their reset statistics; syncobjs
 *    (a seqno each), the GPU's clock.
 * Everything is the app's (owner): another app's handles do not exist for
 * it, and what it has is released when it ends. */
#include "gpu_i915.h"
#include "i915_uapi.h"
#include "kheap.h"
#include "kstring.h"
#include "serial.h"
#include "timer.h"
#include "task.h"
#include "upage.h"
#include "uvm.h"
#include "settings.h"

#ifdef __x86_64__

/* kernel/app.c: the calling app's memory */
int app_user_ok(const void* p, uint64_t n);
uspace_t* app_user_space(void);

#define E_PERM     1
#define E_NOENT    2
#define E_IO       5
#define E_NOMEM   12
#define E_FAULT   14
#define E_BUSY    16
#define E_NODEV   19
#define E_INVAL   22
#define E_NOSPC   28
#define E_NOTTY   25
#define E_TIME    62
#define E_OPNOTSUPP 95

#define OBJ_MAX   32768
#define CTX_MAX   512
#define SYNC_MAX  8192
#define MAPS      4
#define EXEC_MAX  8192

typedef struct {
    int       owner;                /* 0: a free slot; -1: closed, waiting for the GPU to finish */
    uint32_t  pages, gtt;           /* gtt: its first page in the GPU's address space */
    int       vm;                   /* gen 8+: the address space it is bound in (0: none yet) */
    uint64_t  va;                   /*   and where */
    uint64_t* phys;
    uint32_t  last_read, last_write;
    uint32_t  tiling, stride;
    struct { uintptr_t addr; uint32_t pages; uint64_t frame0; } map[MAPS];   /* its mappings in the app */
} obj_t;

typedef struct {
    int      owner;
    int      hw;                    /* its hardware context (kernel/gpu_i915.c), 0: none */
    int      dflt;                  /* the app's default context (ctx_id 0) */
    uint32_t last;                  /* its last seqno */
    uint32_t active, pending;       /* batches lost in GPU resets: guilty, innocent */
    int      banned;                /* lost work and not "recoverable": submissions fail (-EIO) */
    int      recoverable;
} ctx_t;

typedef struct { int owner; uint32_t seqno; int signaled; } sync_t;

/* gen 8+: each app's address space (one: iris asks for the default one's id
 * and puts every context in it) */
#define VMS_MAX 128
static struct { int owner, vm; } g_vms[VMS_MAX];
static int owner_vm(int owner) {
    for (int i = 0; i < VMS_MAX; i++) if (g_vms[i].owner == owner) return g_vms[i].vm;
    for (int i = 0; i < VMS_MAX; i++)
        if (!g_vms[i].owner) {
            int vm = i915_vm_create();
            if (vm <= 0) return -1;
            g_vms[i].owner = owner;
            g_vms[i].vm = vm;
            return vm;
        }
    return -1;
}

static obj_t*  g_obj;
static uint32_t g_nobj;             /* slots used up to here */
static ctx_t   g_ctx[CTX_MAX];
static sync_t* g_sync;
static uint64_t g_bytes;            /* buffers' memory */
static uint32_t g_submits;

static void on_reset(uint32_t done, uint32_t submitted);

/* apps get a supported Intel GPU unless it was switched off (gpu3d off,
 * kept in the settings); a GPU that hangs is reset after 2 seconds */
int gem_enabled(void) {
    char v[8];
    return !(settings_get("gpu3d", v, sizeof(v)) && !strcmp(v, "off"));
}
void gem_set_enabled(int on) { settings_set("gpu3d", on ? "on" : "off"); }

static int ready(void) {
    if (!i915_present() || !gem_enabled()) return -E_NODEV;
    i915_on_reset(on_reset);
    if (!i915_running() && i915_start()) return -E_IO;
    if (!g_obj) {
        g_obj = (obj_t*)kzalloc(sizeof(obj_t) * OBJ_MAX);
        g_sync = (sync_t*)kzalloc(sizeof(sync_t) * SYNC_MAX);
        if (!g_obj || !g_sync) { kfree(g_obj); kfree(g_sync); g_obj = NULL; g_sync = NULL; return -E_NOMEM; }
    }
    return 0;
}

/* ── buffers ───────────────────────────────────────────────────────── */
static void clflush(const void* p, uint32_t n) {
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)63, e = (uintptr_t)p + n;
    for (; a < e; a += 64) __asm__ volatile("clflush (%0)" :: "r"(a) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}

static obj_t* lookup(int owner, uint32_t h) {
    if (!h || h > g_nobj) return NULL;
    obj_t* o = &g_obj[h - 1];
    return o->owner == owner ? o : NULL;
}

static void obj_free(obj_t* o) {
    if (o->vm) i915_vm_unbind(o->vm, o->va, o->phys, o->pages);   /* (what is still its own there) */
    if (!i915_full_ppgtt()) i915_gtt_unbind(o->gtt, o->pages);
    i915_gtt_free(o->gtt, o->pages);
    for (uint32_t i = 0; i < o->pages; i++) upage_free((void*)(uintptr_t)o->phys[i]);
    kfree(o->phys);
    g_bytes -= (uint64_t)o->pages * 4096u;
    memset(o, 0, sizeof(*o));
}

/* closed buffers the GPU is done with: freed */
static void retire(void) {
    for (uint32_t i = 0; i < g_nobj; i++)
        if (g_obj[i].owner == -1 && i915_done(g_obj[i].last_read)) obj_free(&g_obj[i]);
    while (g_nobj && !g_obj[g_nobj - 1].owner) g_nobj--;
}

static int obj_create(int owner, uint64_t size, uint32_t* handle) {
    if (!size || size > (1ull << 30)) return -E_INVAL;
    uint32_t n = (uint32_t)((size + 4095) / 4096);
    uint32_t slot = 0;
    while (slot < g_nobj && g_obj[slot].owner) slot++;
    if (slot == g_nobj) {
        retire();
        /* Retirement can both open interior holes and shrink the tail.
         * Search again: the old cursor may now lie beyond g_nobj. */
        slot = 0;
    }
    while (slot < g_nobj && g_obj[slot].owner) slot++;
    if (slot >= OBJ_MAX) return -E_NOSPC;
    obj_t* o = &g_obj[slot];
    o->phys = (uint64_t*)kmalloc(sizeof(uint64_t) * n);
    if (!o->phys) return -E_NOMEM;
    uint32_t got = 0;
    for (; got < n; got++) {
        void* pg = upage_alloc();
        if (!pg) break;
        memset(pg, 0, 4096);
        if (!i915_llc()) clflush(pg, 4096);
        o->phys[got] = (uint64_t)(uintptr_t)pg;
    }
    /* big buffers aligned to 64 KiB (surfaces, the instruction pool), the rest to a page */
    int64_t at = got == n ? i915_gtt_alloc(n, n >= 16 ? 16 : 1) : -1;
    if (at < 0) {
        for (uint32_t i = 0; i < got; i++) upage_free((void*)(uintptr_t)o->phys[i]);
        kfree(o->phys);
        o->phys = NULL;
        return got == n ? -E_NOSPC : -E_NOMEM;
    }
    o->owner = owner;
    o->pages = n;
    o->gtt = (uint32_t)at;
    /* (gen 8+: only a place - it is bound in the app's own address space when submitted) */
    if (!i915_full_ppgtt()) i915_gtt_bind(o->gtt, o->phys, n);
    g_bytes += (uint64_t)n * 4096u;
    if (slot == g_nobj) g_nobj++;
    *handle = slot + 1;
    return 0;
}

/* its mappings in the app gone (only those still showing it: the app may have unmapped one) */
static void obj_unmap(obj_t* o, uspace_t* us) {
    for (int k = 0; k < MAPS; k++) {
        if (!o->map[k].addr) continue;
        if (us) uvm_unmap_frames(us, o->map[k].addr, o->map[k].pages, o->map[k].frame0);
        o->map[k].addr = 0;
    }
}

static void obj_close(obj_t* o, uspace_t* us) {
    obj_unmap(o, us);
    if (i915_done(o->last_read)) obj_free(o);
    else o->owner = -1;
}

/* the GPU done with it (for reading: done writing; for writing: done with it altogether) */
static int obj_wait(obj_t* o, int write, int timeout_ms) {
    uint32_t s = write ? o->last_read : o->last_write;
    int r = i915_wait(s, timeout_ms);
    return r == -1 ? -E_TIME : r == -2 ? -E_IO : 0;
}

/* ── user memory ───────────────────────────────────────────────────── */
static int u_ok(uint64_t p, uint64_t n) { return n == 0 || (p && app_user_ok((const void*)(uintptr_t)p, n)); }

/* ── contexts and resets ───────────────────────────────────────────── */
static ctx_t* ctx_get(int owner, uint32_t id) {
    if (!id || id > CTX_MAX) return NULL;
    ctx_t* c = &g_ctx[id - 1];
    return c->owner == owner ? c : NULL;
}

static ctx_t* ctx_new(int owner, int dflt, uint32_t* id) {
    for (int i = 0; i < CTX_MAX; i++)
        if (!g_ctx[i].owner) {
            int hw = 0;
            if (i915_full_ppgtt()) {
                int vm = owner_vm(owner);
                if (vm <= 0 || (hw = i915_ctx_create_vm(vm)) < 0) return NULL;
            } else if (i915_has_contexts() && (hw = i915_ctx_create()) < 0) return NULL;
            g_ctx[i] = (ctx_t){ owner, hw, dflt, 0, 0, 0, 0, 1 };
            if (id) *id = (uint32_t)i + 1;
            return &g_ctx[i];
        }
    return NULL;
}

static void ctx_free(ctx_t* c) {
    if (c->hw > 0) i915_ctx_destroy(c->hw);
    memset(c, 0, sizeof(*c));
}

/* batches without a context: the app's default one (its state its own too) */
static ctx_t* ctx_default(int owner) {
    for (int i = 0; i < CTX_MAX; i++) if (g_ctx[i].owner == owner && g_ctx[i].dflt) return &g_ctx[i];
    return ctx_new(owner, 1, NULL);
}

/* the GPU hung: the context whose batch was running is guilty, the others with work queued innocent */
static void on_reset(uint32_t done, uint32_t submitted) {
    (void)submitted;
    uint32_t first = 0;
    for (int i = 0; i < CTX_MAX; i++)
        if (g_ctx[i].owner > 0 && (int32_t)(g_ctx[i].last - done) > 0 && (!first || (int32_t)(g_ctx[i].last - first) < 0))
            first = g_ctx[i].last;
    for (int i = 0; i < CTX_MAX; i++) {
        ctx_t* c = &g_ctx[i];
        if (c->owner <= 0 || (int32_t)(c->last - done) <= 0) continue;
        if (c->last == first) c->active++; else c->pending++;
        if (!c->recoverable) c->banned = 1;
    }
}

/* ── execbuffer2 ───────────────────────────────────────────────────── */
static int execbuffer(int owner, struct drm_i915_gem_execbuffer2* eb) {
    uint64_t ring = eb->flags & I915_EXEC_RING_MASK;
    if (ring != I915_EXEC_DEFAULT && ring != I915_EXEC_RENDER) return -E_INVAL;
    if (eb->flags & (I915_EXEC_FENCE_IN | I915_EXEC_FENCE_OUT | I915_EXEC_FENCE_SUBMIT | I915_EXEC_USE_EXTENSIONS | I915_EXEC_SECURE))
        return -E_INVAL;
    uint32_t n = eb->buffer_count;
    if (!n || n > EXEC_MAX) return -E_INVAL;
    uint32_t ctx_id = (uint32_t)eb->rsvd1;
    ctx_t* ctx = ctx_id ? ctx_get(owner, ctx_id) : ctx_default(owner);
    if (!ctx) return ctx_id ? -E_NOENT : -E_NOMEM;
    if (ctx->banned) return -E_IO;
    if (!u_ok(eb->buffers_ptr, (uint64_t)n * sizeof(struct drm_i915_gem_exec_object2))) return -E_FAULT;
    struct drm_i915_gem_exec_object2* uex = (struct drm_i915_gem_exec_object2*)(uintptr_t)eb->buffers_ptr;
    struct drm_i915_gem_exec_object2* ex = (struct drm_i915_gem_exec_object2*)kmalloc(sizeof(*ex) * n);
    obj_t** ob = (obj_t**)kmalloc(sizeof(obj_t*) * n);
    if (!ex || !ob) { kfree(ex); kfree(ob); return -E_NOMEM; }
    memcpy(ex, uex, sizeof(*ex) * n);
    int rc = 0;
    int full = i915_full_ppgtt();
    for (uint32_t i = 0; i < n && !rc; i++) {
        ob[i] = lookup(owner, ex[i].handle);
        if (!ob[i]) rc = -E_NOENT;
        else if ((ex[i].flags & EXEC_OBJECT_PINNED) && !full) rc = -E_INVAL;   /* (gen 5 - 7.5: offsets are the kernel's) */
    }
    /* an alignment its place does not have: moved (once it is idle) */
    for (uint32_t i = 0; i < n && !rc; i++) {
        uint64_t al = ex[i].alignment;
        if (full && (ex[i].flags & EXEC_OBJECT_PINNED)) continue;           /* (the app's place: its business) */
        if (al <= 4096 || !((uint64_t)ob[i]->gtt * 4096u % al)) continue;
        if (al & (al - 1) || al > (1u << 24)) { rc = -E_INVAL; break; }
        if (obj_wait(ob[i], 1, -1)) { rc = -E_IO; break; }
        int64_t at = i915_gtt_alloc(ob[i]->pages, (uint32_t)(al / 4096));
        if (at < 0) { rc = -E_NOSPC; break; }
        if (!full) i915_gtt_unbind(ob[i]->gtt, ob[i]->pages);
        i915_gtt_free(ob[i]->gtt, ob[i]->pages);
        ob[i]->gtt = (uint32_t)at;
        if (!full) i915_gtt_bind(ob[i]->gtt, ob[i]->phys, ob[i]->pages);   /* (gen 8+: rebound in its address space below) */
    }
    /* gen 8+: each buffer bound in the app's address space - where it says
     * (softpin), else at its place - moved there once the GPU is done with it */
    int vm = full ? owner_vm(owner) : 0;
    if (full && vm <= 0 && !rc) rc = -E_NOMEM;
    for (uint32_t i = 0; i < n && full && !rc; i++) {
        obj_t* o = ob[i];
        uint64_t want = (ex[i].flags & EXEC_OBJECT_PINNED) ? ex[i].offset & ((1ull << 48) - 1) : (uint64_t)o->gtt * 4096u;
        if ((want & 4095) || want + (uint64_t)o->pages * 4096u > i915_vm_size()) { rc = -E_INVAL; break; }
        if (o->vm == vm && o->va == want) continue;
        if (o->vm && obj_wait(o, 1, -1)) { rc = -E_IO; break; }
        if (o->vm) i915_vm_unbind(o->vm, o->va, o->phys, o->pages);
        if (i915_vm_bind(vm, want, o->phys, o->pages)) { o->vm = 0; rc = -E_NOMEM; break; }
        o->vm = vm;
        o->va = want;
    }
    /* relocations: written where the presumed offset is not the real one */
    for (uint32_t i = 0; i < n && !rc; i++) {
        uint32_t nr = ex[i].relocation_count;
        if (!nr) continue;
        if (nr > (1u << 20) || !u_ok(ex[i].relocs_ptr, (uint64_t)nr * sizeof(struct drm_i915_gem_relocation_entry))) { rc = -E_FAULT; break; }
        struct drm_i915_gem_relocation_entry* rl = (struct drm_i915_gem_relocation_entry*)(uintptr_t)ex[i].relocs_ptr;
        obj_t* o = ob[i];
        int waited = 0;
        for (uint32_t k = 0; k < nr; k++) {
            struct drm_i915_gem_relocation_entry r = rl[k];
            obj_t* t = NULL;
            if (eb->flags & I915_EXEC_HANDLE_LUT) { if (r.target_handle < n) t = ob[r.target_handle]; }
            else for (uint32_t j = 0; j < n && !t; j++) if (ex[j].handle == r.target_handle) t = ob[j];
            if (!t) { rc = -E_NOENT; break; }
            uint64_t off = full ? t->va : (uint64_t)t->gtt * 4096u;
            if (r.presumed_offset == off) continue;
            uint32_t wide = full ? 8 : 4;                            /* (gen 8+: 64-bit addresses in the batch) */
            uint64_t bytes = (uint64_t)o->pages * 4096u;
            if ((r.offset & 3) || r.offset > bytes - wide || (wide == 8 && (r.offset & 4095) > 4088)) { rc = -E_INVAL; break; }
            /* No submission occurs during relocation. Once this buffer is
             * idle, all its patches can be written without polling again. */
            if (!waited && obj_wait(o, 1, -1)) { rc = -E_IO; break; }
            waited = 1;
            uint32_t* w = (uint32_t*)(uintptr_t)(o->phys[r.offset >> 12] + (r.offset & 4095));
            uint64_t v = off + (uint64_t)(int64_t)(int32_t)r.delta;
            w[0] = (uint32_t)v;
            if (wide == 8) w[1] = (uint32_t)(v >> 32);
            if (!i915_llc()) clflush(w, wide);
            rl[k].presumed_offset = off;
        }
    }
    /* fences */
    struct drm_i915_gem_exec_fence* fe = NULL;
    uint32_t nf = 0;
    if (!rc && (eb->flags & I915_EXEC_FENCE_ARRAY)) {
        nf = eb->num_cliprects;
        if (nf > 4096 || !u_ok(eb->cliprects_ptr, (uint64_t)nf * sizeof(*fe))) rc = -E_FAULT;
        else {
            fe = (struct drm_i915_gem_exec_fence*)(uintptr_t)eb->cliprects_ptr;
            for (uint32_t k = 0; k < nf && !rc; k++) {
                uint32_t h = fe[k].handle;
                if (!h || h > SYNC_MAX || g_sync[h - 1].owner != owner) rc = -E_NOENT;
            }
        }
    }
    uint32_t bi = (eb->flags & I915_EXEC_BATCH_FIRST) ? 0 : n - 1;
    if (!rc && ((eb->batch_start_offset & 7) || eb->batch_start_offset >= (uint64_t)ob[bi]->pages * 4096u)) rc = -E_INVAL;
    if (!rc) {
        __asm__ volatile("sfence; mfence" ::: "memory");     /* (the app's write-combined stores out) */
        uint64_t at = (full ? ob[bi]->va : (uint64_t)ob[bi]->gtt * 4096u) + eb->batch_start_offset;
        uint32_t s = i915_submit_ctx(ctx->hw, at);
        if (!s) rc = -E_IO;
        else {
            g_submits++;
            for (uint32_t i = 0; i < n; i++) {
                ob[i]->last_read = s;
                if (ex[i].flags & EXEC_OBJECT_WRITE) ob[i]->last_write = s;
            }
            ctx->last = s;
            for (uint32_t k = 0; k < nf; k++)
                if (fe[k].flags & I915_EXEC_FENCE_SIGNAL) { sync_t* y = &g_sync[fe[k].handle - 1]; y->seqno = s; y->signaled = 0; }
        }
    }
    /* the offsets, for the next time */
    if (!rc)
        for (uint32_t i = 0; i < n; i++) {
            if (full && (ex[i].flags & EXEC_OBJECT_PINNED)) continue;    /* (the app's own: left as it gave it) */
            uint64_t at = full ? ob[i]->va : (uint64_t)ob[i]->gtt * 4096u;
            if (uex[i].offset != at) uex[i].offset = at;
        }
    kfree(ex);
    kfree(ob);
    return rc;
}

/* a context's parameter (c NULL: the app's defaults) */
#define APP_VM_ID 1                         /* (the id the app's address space has for it) */
static int ctx_param(int owner, ctx_t* c, int set, struct drm_i915_gem_context_param* p) {
    switch (p->param) {
    case I915_CONTEXT_PARAM_GTT_SIZE:
        if (set) return -E_INVAL;
        p->value = i915_full_ppgtt() ? i915_vm_size() : i915_gtt_size();
        return 0;
    case I915_CONTEXT_PARAM_VM:
        /* (gen 8+: one address space an app - every context is in it already) */
        if (!i915_full_ppgtt()) return -E_INVAL;
        if (set) return p->value == APP_VM_ID ? 0 : -E_NOENT;
        if (owner_vm(owner) <= 0) return -E_NOMEM;
        p->value = APP_VM_ID;
        return 0;
    case I915_CONTEXT_PARAM_PRIORITY: if (!set) p->value = 0; return 0;
    case I915_CONTEXT_PARAM_RECOVERABLE:
        if (set) { if (c) c->recoverable = p->value != 0; } else p->value = c ? (uint64_t)c->recoverable : 1;
        return 0;
    case I915_CONTEXT_PARAM_BANNABLE: case I915_CONTEXT_PARAM_NO_ERROR_CAPTURE: case I915_CONTEXT_PARAM_BAN_PERIOD:
    case I915_CONTEXT_PARAM_PERSISTENCE: case I915_CONTEXT_PARAM_LOW_LATENCY:
        if (!set) p->value = 0;
        return 0;
    case I915_CONTEXT_PARAM_PROTECTED_CONTENT:
        if (set) return p->value ? -E_NODEV : 0;
        p->value = 0;
        return 0;
    }
    return -E_INVAL;
}

/* ── the requests ──────────────────────────────────────────────────── */
static int getparam(int32_t param, int* v) {
    const i915_hw_t* h = i915_hw();
    if (h->gen >= 8) {
        i915_topology_t t;
        switch (param) {
        case I915_PARAM_HAS_EXEC_SOFTPIN: *v = 1; return 0;
        case I915_PARAM_HAS_ALIASING_PPGTT: *v = 2; return 0;                /* (full: an address space per app) */
        case I915_PARAM_CS_TIMESTAMP_FREQUENCY: *v = (int)i915_timestamp_hz(); return 0;
        case I915_PARAM_SLICE_MASK: i915_topology(&t); *v = (int)t.slice_mask; return 0;
        case I915_PARAM_SUBSLICE_MASK: i915_topology(&t); *v = t.subslice_mask[0]; return 0;
        case I915_PARAM_EU_TOTAL: i915_topology(&t); *v = (int)t.eu_total; return 0;
        case I915_PARAM_HAS_USERPTR_PROBE: case I915_PARAM_HAS_EXEC_TIMELINE_FENCES: *v = 0; return 0;
        }
    }
    switch (param) {
    case I915_PARAM_CHIPSET_ID: *v = h->devid; return 0;
    case I915_PARAM_REVISION: *v = h->revision; return 0;
    case I915_PARAM_HAS_GEM: case I915_PARAM_HAS_EXECBUF2: case I915_PARAM_HAS_RELAXED_FENCING:
    case I915_PARAM_HAS_RELAXED_DELTA: case I915_PARAM_HAS_WAIT_TIMEOUT: case I915_PARAM_HAS_EXEC_NO_RELOC:
    case I915_PARAM_HAS_EXEC_HANDLE_LUT: case I915_PARAM_HAS_EXEC_BATCH_FIRST: case I915_PARAM_HAS_EXEC_FENCE_ARRAY:
    case I915_PARAM_HAS_GPU_RESET: case I915_PARAM_HAS_COHERENT_RINGS: case I915_PARAM_MMAP_VERSION:
    case I915_PARAM_HAS_EXEC_ASYNC: case I915_PARAM_HAS_EXEC_CAPTURE:
        *v = 1; return 0;
    case I915_PARAM_HAS_LLC: *v = i915_llc(); return 0;
    case I915_PARAM_HAS_ALIASING_PPGTT: *v = h->gen >= 6; return 0;
    case I915_PARAM_CS_TIMESTAMP_FREQUENCY: *v = 12500000; return 0;
    case I915_PARAM_NUM_FENCES_AVAIL: case I915_PARAM_HAS_BSD: case I915_PARAM_HAS_BLT: case I915_PARAM_HAS_VEBOX:
    case I915_PARAM_HAS_BSD2: case I915_PARAM_HAS_SEMAPHORES: case I915_PARAM_HAS_SECURE_BATCHES:
    case I915_PARAM_HAS_PINNED_BATCHES: case I915_PARAM_HAS_EXEC_SOFTPIN: case I915_PARAM_MMAP_GTT_VERSION:
    case I915_PARAM_HAS_CONTEXT_ISOLATION: *v = i915_has_contexts(); return 0;   /* (the render engine) */
    case I915_PARAM_HAS_SCHEDULER: case I915_PARAM_HAS_EXEC_FENCE:
    case I915_PARAM_HAS_GEN7_SOL_RESET: case I915_PARAM_HAS_WT: case I915_PARAM_CMD_PARSER_VERSION:
        *v = 0; return 0;
    }
    return -E_INVAL;
}

static int copy_str(uint64_t len, uint64_t ptr, const char* s) {
    uint64_t n = strlen(s);
    if (len && ptr) {
        if (!u_ok(ptr, len)) return -E_FAULT;
        memcpy((void*)(uintptr_t)ptr, s, n < len ? n : len);
    }
    return (int)n;
}

static int ioctl_core(int owner, unsigned nr, void* arg, uspace_t* us) {
    switch (nr) {
    case DRM_NR_VERSION: {
        struct drm_version* v = (struct drm_version*)arg;
        v->version_major = 1; v->version_minor = 6; v->version_patchlevel = 0;
        int a = copy_str(v->name_len, v->name, "i915");
        int b = copy_str(v->date_len, v->date, "20201103");
        int c = copy_str(v->desc_len, v->desc, "Intel Graphics (Banana OS)");
        if (a < 0 || b < 0 || c < 0) return -E_FAULT;
        v->name_len = (uint64_t)a; v->date_len = (uint64_t)b; v->desc_len = (uint64_t)c;
        return 0;
    }
    case DRM_NR_GEM_CLOSE: {
        obj_t* o = lookup(owner, ((struct drm_gem_close*)arg)->handle);
        if (!o) return -E_NOENT;
        obj_close(o, us);
        return 0;
    }
    case DRM_NR_GET_CAP: {
        struct drm_get_cap* c = (struct drm_get_cap*)arg;
        c->value = c->capability == DRM_CAP_SYNCOBJ ? 1 : 0;
        return 0;
    }
    case DRM_NR_SET_CLIENT_CAP: return 0;
    case DRM_NR_SYNCOBJ_CREATE: {
        struct drm_syncobj_create* c = (struct drm_syncobj_create*)arg;
        for (int i = 0; i < SYNC_MAX; i++)
            if (!g_sync[i].owner) {
                g_sync[i] = (sync_t){ owner, 0, (c->flags & DRM_SYNCOBJ_CREATE_SIGNALED) != 0 };
                c->handle = (uint32_t)i + 1;
                return 0;
            }
        return -E_NOMEM;
    }
    case DRM_NR_SYNCOBJ_DESTROY: {
        uint32_t h = ((struct drm_syncobj_destroy*)arg)->handle;
        if (!h || h > SYNC_MAX || g_sync[h - 1].owner != owner) return -E_INVAL;
        memset(&g_sync[h - 1], 0, sizeof(sync_t));
        return 0;
    }
    case DRM_NR_SYNCOBJ_RESET: case DRM_NR_SYNCOBJ_SIGNAL: {
        struct drm_syncobj_array* a = (struct drm_syncobj_array*)arg;
        if (a->count_handles > 4096 || !u_ok(a->handles, (uint64_t)a->count_handles * 4)) return -E_FAULT;
        const uint32_t* hs = (const uint32_t*)(uintptr_t)a->handles;
        for (uint32_t k = 0; k < a->count_handles; k++) {
            uint32_t h = hs[k];
            if (!h || h > SYNC_MAX || g_sync[h - 1].owner != owner) return -E_INVAL;
            g_sync[h - 1].seqno = 0;
            g_sync[h - 1].signaled = nr == DRM_NR_SYNCOBJ_SIGNAL;
        }
        return 0;
    }
    case DRM_NR_SYNCOBJ_WAIT: {
        struct drm_syncobj_wait* w = (struct drm_syncobj_wait*)arg;
        if (!w->count_handles || w->count_handles > 4096 || !u_ok(w->handles, (uint64_t)w->count_handles * 4)) return -E_FAULT;
        const uint32_t* hs = (const uint32_t*)(uintptr_t)w->handles;
        for (uint32_t k = 0; k < w->count_handles; k++)
            if (!hs[k] || hs[k] > SYNC_MAX || g_sync[hs[k] - 1].owner != owner) return -E_INVAL;
        int all = (w->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) != 0;
        for (;;) {
            int ready_n = 0, first = -1, unsubmitted = 0;
            for (uint32_t k = 0; k < w->count_handles; k++) {
                sync_t* y = &g_sync[hs[k] - 1];
                int ok = y->signaled || (y->seqno && i915_done(y->seqno));
                if (!y->signaled && !y->seqno) unsubmitted = 1;
                if (ok) { ready_n++; if (first < 0) first = (int)k; }
            }
            if (all ? ready_n == (int)w->count_handles : ready_n > 0) { w->first_signaled = first < 0 ? 0 : (uint32_t)first; return 0; }
            if (unsubmitted && !(w->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT) && (all || ready_n == 0)) return -E_INVAL;
            int64_t now = (int64_t)timer_ms() * 1000000;
            if (w->timeout_nsec <= now) return -E_TIME;
            for (uint32_t k = 0; k < w->count_handles; k++) {         /* (the hang check runs in the wait) */
                sync_t* y = &g_sync[hs[k] - 1];
                if (y->seqno && !i915_done(y->seqno)) { i915_wait(y->seqno, 1); break; }
            }
            task_sleep_ms(1);
        }
    }
    case DRM_NR_GET_MAGIC: case DRM_NR_GEM_FLINK: case DRM_NR_GEM_OPEN:
    case DRM_NR_PRIME_TO_FD: case DRM_NR_PRIME_TO_HANDLE: case DRM_NR_SYNCOBJ_TO_FD: case DRM_NR_SYNCOBJ_FROM_FD:
        return -E_OPNOTSUPP;
    }
    return -E_NOTTY;
}

static int ioctl_i915(int owner, unsigned nr, unsigned size, void* arg, uspace_t* us) {
    switch (nr) {
    case I915_NR_GETPARAM: {
        struct drm_i915_getparam* g = (struct drm_i915_getparam*)arg;
        int v;
        int r = getparam(g->param, &v);
        if (r) return r;
        if (!u_ok(g->value, 4)) return -E_FAULT;
        *(int*)(uintptr_t)g->value = v;
        return 0;
    }
    case I915_NR_SETPARAM: return -E_INVAL;
    case I915_NR_GEM_THROTTLE: retire(); return 0;
    case I915_NR_GEM_CREATE: {
        struct drm_i915_gem_create* c = (struct drm_i915_gem_create*)arg;
        uint32_t h;
        int r = obj_create(owner, c->size, &h);
        if (r) return r;
        c->handle = h;
        c->size = (c->size + 4095) & ~4095ull;
        return 0;
    }
    case I915_NR_GEM_PREAD: case I915_NR_GEM_PWRITE: {
        struct drm_i915_gem_pread* p = (struct drm_i915_gem_pread*)arg;
        obj_t* o = lookup(owner, p->handle);
        if (!o) return -E_NOENT;
        if (p->offset > (uint64_t)o->pages * 4096u || p->size > (uint64_t)o->pages * 4096u - p->offset) return -E_INVAL;
        if (!u_ok(p->data_ptr, p->size)) return -E_FAULT;
        int r = obj_wait(o, nr == I915_NR_GEM_PWRITE, -1);
        if (r) return r;
        uint8_t* u = (uint8_t*)(uintptr_t)p->data_ptr;
        for (uint64_t done = 0; done < p->size; ) {
            uint64_t at = p->offset + done;
            uint32_t in = (uint32_t)(at & 4095), chunk = 4096 - in;
            if (chunk > p->size - done) chunk = (uint32_t)(p->size - done);
            uint8_t* k = (uint8_t*)(uintptr_t)o->phys[at >> 12] + in;
            if (!i915_llc()) clflush(k, chunk);
            if (nr == I915_NR_GEM_PWRITE) memcpy(k, u + done, chunk); else memcpy(u + done, k, chunk);
            if (!i915_llc() && nr == I915_NR_GEM_PWRITE) clflush(k, chunk);
            done += chunk;
        }
        return 0;
    }
    case I915_NR_GEM_MMAP: {
        if (size != sizeof(struct drm_i915_gem_mmap)) return -E_INVAL;
        struct drm_i915_gem_mmap* m = (struct drm_i915_gem_mmap*)arg;
        obj_t* o = lookup(owner, m->handle);
        if (!o) return -E_NOENT;
        if ((m->offset & 4095) || !m->size || m->offset > (uint64_t)o->pages * 4096u ||
            m->size > (uint64_t)o->pages * 4096u - m->offset) return -E_INVAL;
        if (!us) return -E_NODEV;
        uint32_t first = (uint32_t)(m->offset / 4096), pages = (uint32_t)((m->size + 4095) / 4096);
        /* write-combining only where the GPU does not share the CPU's cache (Ironlake):
         * elsewhere write-back is coherent, and WC beside the kernel's WB view would not be */
        int wc = (m->flags & I915_MMAP_WC) && !i915_llc();
        int slot = -1;
        for (int k = 0; k < MAPS && slot < 0; k++) if (!o->map[k].addr) slot = k;
        if (slot < 0) return -E_NOMEM;                    /* (each must be undone when it is closed) */
        uintptr_t a = uvm_map_frames(us, o->phys + first, pages, UVM_READ | UVM_WRITE, wc);
        if (!a) return -E_NOMEM;
        o->map[slot].addr = a;
        o->map[slot].pages = pages;
        o->map[slot].frame0 = o->phys[first];
        m->addr_ptr = a;
        return 0;
    }
    case I915_NR_GEM_MMAP_GTT: return -E_NODEV;          /* (no aperture maps: crocus uses CPU maps) */
    case I915_NR_GEM_SET_DOMAIN: {
        struct drm_i915_gem_set_domain* d = (struct drm_i915_gem_set_domain*)arg;
        obj_t* o = lookup(owner, d->handle);
        if (!o) return -E_NOENT;
        return obj_wait(o, d->write_domain != 0, -1);
    }
    case I915_NR_GEM_SW_FINISH: return lookup(owner, *(uint32_t*)arg) ? 0 : -E_NOENT;
    case I915_NR_GEM_SET_TILING: case I915_NR_GEM_GET_TILING: {
        struct drm_i915_gem_set_tiling* t = (struct drm_i915_gem_set_tiling*)arg;
        obj_t* o = lookup(owner, t->handle);
        if (!o) return -E_NOENT;
        if (nr == I915_NR_GEM_SET_TILING) {
            if (t->tiling_mode > I915_TILING_Y) return -E_INVAL;
            o->tiling = t->tiling_mode;
            o->stride = t->tiling_mode ? t->stride : 0;
            t->stride = o->stride;
            uint32_t sw = i915_swizzle_x();
            t->swizzle_mode = o->tiling == I915_TILING_X ? sw : o->tiling == I915_TILING_Y && sw ? I915_BIT_6_SWIZZLE_9 : 0;
        } else {
            struct drm_i915_gem_get_tiling* g = (struct drm_i915_gem_get_tiling*)arg;
            uint32_t sw = i915_swizzle_x();
            g->tiling_mode = o->tiling;
            g->swizzle_mode = o->tiling == I915_TILING_X ? sw : o->tiling == I915_TILING_Y && sw ? I915_BIT_6_SWIZZLE_9 : 0;
            g->phys_swizzle_mode = g->swizzle_mode;
        }
        return 0;
    }
    case I915_NR_GEM_GET_APERTURE: {
        struct drm_i915_gem_get_aperture* a = (struct drm_i915_gem_get_aperture*)arg;
        a->aper_size = i915_gtt_size();
        a->aper_available_size = i915_gtt_free_bytes();
        return 0;
    }
    case I915_NR_GEM_MADVISE: {
        struct drm_i915_gem_madvise* m = (struct drm_i915_gem_madvise*)arg;
        if (!lookup(owner, m->handle)) return -E_NOENT;
        m->retained = 1;                                  /* (never purged) */
        return 0;
    }
    case I915_NR_GEM_EXECBUFFER2:
        if (size != sizeof(struct drm_i915_gem_execbuffer2)) return -E_INVAL;
        return execbuffer(owner, (struct drm_i915_gem_execbuffer2*)arg);
    case I915_NR_GEM_BUSY: {
        struct drm_i915_gem_busy* b = (struct drm_i915_gem_busy*)arg;
        obj_t* o = lookup(owner, b->handle);
        if (!o) return -E_NOENT;
        i915_wait(o->last_read, 0);                        /* (a hung GPU noticed here too) */
        b->busy = !i915_done(o->last_read) ? (1u << 16) | (!i915_done(o->last_write) ? 1u : 0u) : 0;
        return 0;
    }
    case I915_NR_GEM_WAIT: {
        struct drm_i915_gem_wait* w = (struct drm_i915_gem_wait*)arg;
        obj_t* o = lookup(owner, w->bo_handle);
        if (!o) return -E_NOENT;
        int ms = w->timeout_ns < 0 ? -1 : (int)(w->timeout_ns >= 2000000000000ll ? 2000000000 : (w->timeout_ns + 999999) / 1000000);
        uint32_t t0 = timer_ms();
        int r = obj_wait(o, 1, ms);
        if (w->timeout_ns > 0) {
            int64_t left = w->timeout_ns - (int64_t)(timer_ms() - t0) * 1000000;
            w->timeout_ns = left > 0 ? left : 0;
        }
        return r;
    }
    case I915_NR_CONTEXT_CREATE: {
        struct drm_i915_gem_context_create_ext* c = (struct drm_i915_gem_context_create_ext*)arg;
        if (size >= sizeof(*c) && (c->flags & ~I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS)) return -E_INVAL;
        ctx_t* x = ctx_new(owner, 0, &c->ctx_id);
        if (!x) return -E_NOMEM;
        /* its parameters as SETPARAM extensions (iris: recoverable, the address space) */
        uint64_t e = size >= sizeof(*c) && (c->flags & I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS) ? c->extensions : 0;
        for (int k = 0; e && k < 16; k++) {
            if (!u_ok(e, sizeof(struct drm_i915_gem_context_create_ext_setparam))) { ctx_free(x); return -E_FAULT; }
            struct drm_i915_gem_context_create_ext_setparam sp = *(struct drm_i915_gem_context_create_ext_setparam*)(uintptr_t)e;
            int r = sp.base.name != I915_CONTEXT_CREATE_EXT_SETPARAM ? -E_INVAL : ctx_param(owner, x, 1, &sp.param);
            if (r) { ctx_free(x); return r; }
            e = sp.base.next_extension;
        }
        if (e) { ctx_free(x); c->ctx_id = 0; return -E_INVAL; }
        return 0;
    }
    case I915_NR_CONTEXT_DESTROY: {
        ctx_t* c = ctx_get(owner, ((struct drm_i915_gem_context_destroy*)arg)->ctx_id);
        if (!c || c->dflt) return -E_NOENT;
        ctx_free(c);
        return 0;
    }
    case I915_NR_CONTEXT_GETPARAM: case I915_NR_CONTEXT_SETPARAM: {
        struct drm_i915_gem_context_param* p = (struct drm_i915_gem_context_param*)arg;
        ctx_t* c = p->ctx_id ? ctx_get(owner, p->ctx_id) : NULL;
        if (p->ctx_id && !c) return -E_NOENT;
        return ctx_param(owner, c, nr == I915_NR_CONTEXT_SETPARAM, p);
    }
    case I915_NR_REG_READ: {
        struct drm_i915_reg_read* r = (struct drm_i915_reg_read*)arg;
        if ((r->offset & ~(uint64_t)I915_REG_READ_8B_WA) != 0x2358) return -E_INVAL;
        r->val = i915_timestamp();
        return 0;
    }
    case I915_NR_GET_RESET_STATS: {
        struct drm_i915_reset_stats* s = (struct drm_i915_reset_stats*)arg;
        if (s->flags) return -E_INVAL;
        ctx_t* c = s->ctx_id ? ctx_get(owner, s->ctx_id) : NULL;
        if (s->ctx_id && !c) return -E_NOENT;
        s->reset_count = i915_resets();
        s->batch_active = c ? c->active : 0;
        s->batch_pending = c ? c->pending : 0;
        return 0;
    }
    case I915_NR_QUERY: {
        struct drm_i915_query* q = (struct drm_i915_query*)arg;
        if (q->flags || q->num_items > 64 || !u_ok(q->items_ptr, (uint64_t)q->num_items * sizeof(struct drm_i915_query_item))) return -E_INVAL;
        struct drm_i915_query_item* it = (struct drm_i915_query_item*)(uintptr_t)q->items_ptr;
        for (uint32_t k = 0; k < q->num_items; k++) {
            if (it[k].query_id != DRM_I915_QUERY_TOPOLOGY_INFO || i915_hw()->gen < 8) { it[k].length = -E_INVAL; continue; }
            /* the slices, subslices and EUs (from gen 8: iris needs it on gen 11) */
            i915_topology_t t;
            i915_topology(&t);
            struct drm_i915_query_topology_info h;
            memset(&h, 0, sizeof(h));
            h.max_slices = (uint16_t)t.max_slices;
            h.max_subslices = (uint16_t)t.max_subslices;
            h.max_eus_per_subslice = (uint16_t)t.max_eus;
            h.subslice_offset = (uint16_t)((t.max_slices + 7) / 8);
            h.subslice_stride = (uint16_t)((t.max_subslices + 7) / 8);
            h.eu_offset = (uint16_t)(h.subslice_offset + t.max_slices * h.subslice_stride);
            h.eu_stride = (uint16_t)((t.max_eus + 7) / 8);
            uint32_t len = (uint32_t)sizeof(h) + h.eu_offset + (uint32_t)(t.max_slices * t.max_subslices) * h.eu_stride;
            if (it[k].length == 0) { it[k].length = (int32_t)len; continue; }
            if ((uint32_t)it[k].length < len || !u_ok(it[k].data_ptr, len)) { it[k].length = -E_INVAL; continue; }
            uint8_t buf[sizeof(h) + 64];
            memset(buf, 0, sizeof(buf));
            memcpy(buf, &h, sizeof(h));
            uint8_t* d = buf + sizeof(h);
            d[0] = (uint8_t)t.slice_mask;
            for (int sl = 0; sl < t.max_slices; sl++) {
                d[h.subslice_offset + sl * h.subslice_stride] = t.subslice_mask[sl];
                for (int ss = 0; ss < t.max_subslices; ss++)
                    d[h.eu_offset + (sl * t.max_subslices + ss) * h.eu_stride] = t.eu_mask[sl][ss];
            }
            memcpy((void*)(uintptr_t)it[k].data_ptr, buf, len);
            it[k].length = (int32_t)len;
        }
        return 0;
    }
    case I915_NR_GEM_SET_CACHING: case I915_NR_GEM_GET_CACHING: {
        struct drm_i915_gem_caching* c = (struct drm_i915_gem_caching*)arg;
        if (!lookup(owner, c->handle)) return -E_NOENT;
        if (nr == I915_NR_GEM_GET_CACHING) c->caching = i915_llc() ? 1 : 0;
        return 0;                                         /* (every buffer stays as it is) */
    }
    case I915_NR_GEM_USERPTR: case I915_NR_GEM_VM_CREATE: case I915_NR_GEM_CREATE_EXT:
        return -E_NODEV;
    }
    return -E_NOTTY;
}

int gem_ioctl(int owner, unsigned long req, void* arg) {
    if (DRM_TYPE(req) != 'd') return -E_NOTTY;
    unsigned nr = DRM_NR(req), size = DRM_SIZE(req);
    if (size > 256 || (size && !u_ok((uint64_t)(uintptr_t)arg, size))) return -E_FAULT;
    int r = ready();
    if (r) return r;
    /* (the request is copied: the app's other threads may change their copy meanwhile) */
    uint64_t k[32];
    memset(k, 0, sizeof(k));
    if (size) memcpy(k, arg, size);
    uspace_t* us = app_user_space();
    if (nr >= DRM_COMMAND_BASE && nr < DRM_COMMAND_BASE + 0x40) r = ioctl_i915(owner, nr - DRM_COMMAND_BASE, size, k, us);
    else r = ioctl_core(owner, nr, k, us);
    if (size && ((req >> 30) & 2)) memcpy(arg, k, size);           /* _IOC_READ: the answer back */
    return r;
}

void gem_release(int owner) {
    if (!g_obj) return;
    for (uint32_t i = 0; i < g_nobj; i++)
        if (g_obj[i].owner == owner) {
            memset(g_obj[i].map, 0, sizeof(g_obj[i].map));      /* (its address space is going away) */
            obj_close(&g_obj[i], NULL);
        }
    for (int i = 0; i < CTX_MAX; i++) if (g_ctx[i].owner == owner) ctx_free(&g_ctx[i]);
    for (int i = 0; i < SYNC_MAX; i++) if (g_sync[i].owner == owner) memset(&g_sync[i], 0, sizeof(sync_t));
    retire();
    /* its address space: its buffers' entries (the busy ones are freed later:
     * they no longer point at it) */
    for (int i = 0; i < VMS_MAX; i++)
        if (g_vms[i].owner == owner) {
            for (uint32_t k = 0; k < g_nobj; k++) if (g_obj[k].vm == g_vms[i].vm) g_obj[k].vm = 0;
            i915_vm_destroy(g_vms[i].vm);
            g_vms[i].owner = 0;
        }
}

void gem_stats(char* buf, int cap) {
    uint32_t live = 0, zombies = 0;
    for (uint32_t i = 0; i < g_nobj; i++) { if (g_obj[i].owner > 0) live++; else if (g_obj[i].owner < 0) zombies++; }
    ksnprintf(buf, (size_t)cap, "buffers: %u (%u MiB), %u waiting for the GPU; %u submissions",
              live, (uint32_t)(g_bytes >> 20), zombies, g_submits);
}

/* ── `gpu3d test` ──────────────────────────────────────────────────── */
void i915_tests(void (*report)(const char*, int, const char*)) {
    char d[160];
    i915_on_reset(on_reset);
    if (!i915_present()) { report("an Intel GPU of gen 5 - 11", 0, i915_why()); return; }
    int up = i915_running() || i915_start() == 0;
    report("the render ring starts", up, i915_why());
    if (!up) return;
    /* a target page and a batch page, both ours */
    uint8_t* tgt = (uint8_t*)upage_alloc();
    uint8_t* bat = (uint8_t*)upage_alloc();
    if (!tgt || !bat) { report("memory for the test", 0, ""); if (tgt) upage_free(tgt); if (bat) upage_free(bat); return; }
    memset(tgt, 0, 4096); memset(bat, 0, 4096);
    uint64_t pt = (uint64_t)(uintptr_t)tgt, pb = (uint64_t)(uintptr_t)bat;
    int64_t gt = i915_gtt_alloc(1, 1), gb = i915_gtt_alloc(1, 1);
    if (gt < 0 || gb < 0) { report("GPU address space for the test", 0, ""); return; }
    i915_gtt_bind((uint32_t)gt, &pt, 1);
    i915_gtt_bind((uint32_t)gb, &pb, 1);
    uint32_t ta = (uint32_t)gt * 4096u, ba = (uint32_t)gb * 4096u;
    const i915_hw_t* h = i915_hw();
    /* 1: MI_STORE_DATA_IMM from a batch, through the address space apps use */
    uint32_t* b = (uint32_t*)bat;
    int k = 0;
    int g8 = h->gen >= 8;                                         /* (64-bit addresses in commands) */
    for (int i = 0; i < 16; i++) {
        b[k++] = (0x20u << 23) | 2u | (h->gen == 5 ? 1u << 22 : 0);    /* MI_STORE_DATA_IMM (gen 5: GGTT) */
        if (g8) { b[k++] = ta + (uint32_t)i * 4u; b[k++] = 0; }
        else { b[k++] = 0; b[k++] = ta + (uint32_t)i * 4u; }
        b[k++] = 0xB0A00000u + (uint32_t)i;
    }
    b[k++] = 0x0Au << 23;                                         /* MI_BATCH_BUFFER_END */
    b[k++] = 0;
    if (!i915_llc()) clflush(bat, 4096);
    /* (Ironlake runs it privileged: unprivileged batches may not store there) */
    uint32_t s = h->gen == 5 ? i915_submit_privileged(ba) : i915_submit(ba);
    int r = s ? i915_wait(s, 1000) : -2;
    if (!i915_llc()) clflush(tgt, 4096);
    int good = 0;
    for (int i = 0; i < 16; i++) good += ((uint32_t*)tgt)[i] == 0xB0A00000u + (uint32_t)i;
    ksnprintf(d, sizeof(d), "seqno %u: %s, %d of 16 dwords written", s, r == 0 ? "done" : r == -1 ? "timed out" : "the GPU hung", good);
    report("a batch writes memory (MI_STORE_DATA_IMM)", r == 0 && good == 16, d);
    /* 2: the 3D pipe's post-sync write (PIPE_CONTROL), as Mesa's fences do */
    k = 0;
    memset(tgt, 0, 4096);
    if (!i915_llc()) clflush(tgt, 4096);
    if (h->gen == 5) { b[k++] = (3u << 29 | 3u << 27 | 2u << 24 | 2u) | (1u << 14); b[k++] = (ta + 64) | (1u << 2); b[k++] = 0x5EED0001u; b[k++] = 0; }
    else if (h->gen == 6) {
        b[k++] = 3u << 29 | 3u << 27 | 2u << 24 | 2u; b[k++] = (1u << 20) | (1u << 1); b[k++] = 0; b[k++] = 0;
        b[k++] = 3u << 29 | 3u << 27 | 2u << 24 | 2u; b[k++] = (1u << 14) | (1u << 20) | (1u << 12); b[k++] = ta + 64; b[k++] = 0x5EED0001u;
    } else if (g8) {
        b[k++] = 3u << 29 | 3u << 27 | 2u << 24 | 4u; b[k++] = (1u << 14) | (1u << 20) | (1u << 12);
        b[k++] = ta + 64; b[k++] = 0; b[k++] = 0x5EED0001u; b[k++] = 0;
    } else { b[k++] = 3u << 29 | 3u << 27 | 2u << 24 | 2u; b[k++] = (1u << 14) | (1u << 20) | (1u << 12); b[k++] = ta + 64; b[k++] = 0x5EED0001u; }
    b[k++] = 0x0Au << 23; b[k++] = 0;
    if (!i915_llc()) clflush(bat, 4096);
    s = h->gen == 5 ? i915_submit_privileged(ba) : i915_submit(ba);
    r = s ? i915_wait(s, 1000) : -2;
    if (!i915_llc()) clflush(tgt, 4096);
    uint32_t got = ((uint32_t*)tgt)[16];
    ksnprintf(d, sizeof(d), "%s, 0x%08x read back", r == 0 ? "done" : r == -1 ? "timed out" : "the GPU hung", got);
    report("the 3D pipeline's post-sync write (PIPE_CONTROL)", r == 0 && got == 0x5EED0001u, d);
    /* 3: many small submissions: the ring wraps several times */
    b[0] = (0x20u << 23) | 2u | (h->gen == 5 ? 1u << 22 : 0); b[1] = g8 ? ta + 128 : 0; b[2] = g8 ? 0 : ta + 128; b[3] = 0;
    b[4] = 0x0Au << 23; b[5] = 0;
    uint32_t t0 = timer_ms(), last = 0;
    int n = 0;
    for (; n < 4000; n++) {
        last = h->gen == 5 ? i915_submit_privileged(ba) : i915_submit(ba);
        if (!last) break;
    }
    r = last ? i915_wait(last, 3000) : -2;
    uint32_t ms = timer_ms() - t0;
    ksnprintf(d, sizeof(d), "%d batches in %u ms (%s)", n, ms, r == 0 ? "all done" : "not finished");
    report("4000 submissions (the ring wraps)", r == 0 && n == 4000, d);
    /* 4: the GPU's clock runs */
    uint64_t c0 = i915_timestamp();
    task_sleep_ms(20);
    uint64_t c1 = i915_timestamp();
    ksnprintf(d, sizeof(d), "%llu ticks in 20 ms (%u Hz: ~%u)", (unsigned long long)(c1 - c0), i915_timestamp_hz(), i915_timestamp_hz() / 50u);
    report("the render engine's clock", c1 > c0, d);
    i915_gtt_unbind((uint32_t)gt, 1); i915_gtt_free((uint32_t)gt, 1);
    i915_gtt_unbind((uint32_t)gb, 1); i915_gtt_free((uint32_t)gb, 1);
    upage_free(tgt);
    upage_free(bat);
}

#else

int  gem_ioctl(int owner, unsigned long req, void* arg) { (void)owner; (void)req; (void)arg; return -19; }
int  gem_enabled(void) { return 0; }
void gem_set_enabled(int on) { (void)on; }
void gem_release(int owner) { (void)owner; }
void gem_stats(char* buf, int cap) { ksnprintf(buf, (size_t)cap, "64-bit Banana OS only"); }
void i915_tests(void (*report)(const char*, int, const char*)) { report("3D on Intel GPUs", 0, "64-bit Banana OS only"); }

#endif
