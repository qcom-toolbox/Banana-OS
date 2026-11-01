/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Hosted run of kernel/gpu_i915.c, gpu_i915_lrc.c and gpu_gem.c against a model of
 * an Intel GPU's command streamer, gen 5, 6, 7 and 7.5. The model: the
 * register BAR (forcewake acks, masked registers, the GT reset), the global
 * GTT in its upper half, the aliasing PPGTT (its directory in GTT entries,
 * its tables in memory), the render ring (HEAD chases TAIL) and the
 * commands the kernel and these tests use - MI_STORE_DATA_IMM,
 * MI_STORE_DWORD_INDEX, PIPE_CONTROL's post-sync write, MI_BATCH_BUFFER_
 * START / END, everything else skipped by its length - through the address
 * space each batch uses (privileged: the GTT, else the PPGTT from gen 6).
 * "Memory" is one arena below 4 GiB, so the GTT's entries hold it. Then
 * the i915 interface as Mesa uses it: buffers, maps, relocations, execbuffer2
 * with a fence array, waits, syncobjs, contexts, a hung batch and the
 * reset, buffers closed while busy, the app's end; and `gpu3d test`.
 *   cc -O1 -no-pie -w -I kernel -I tools/tests tools/tests/test_i915.c kernel/gpu_i915.c kernel/gpu_i915_lrc.c kernel/gpu_gem.c -o /tmp/i915 && /tmp/i915 */
#include "i915/model.h"

/* ── the app side ──────────────────────────────────────────────────── */
static uspace_t* fake_us = (uspace_t*)(uintptr_t)0x1000;
int app_user_ok(const void* p, uint64_t n) { (void)n; return p != NULL; }
uspace_t* app_user_space(void) { return fake_us; }
static int maps_live;
uintptr_t uvm_map_frames(uspace_t* us, const uint64_t* f, uint32_t n, int prot, int wc) {
    (void)us; (void)prot; (void)wc;
    for (uint32_t i = 1; i < n; i++) if (f[i] != f[0] + i * 4096ull) { printf("    (a buffer's pages are not contiguous here: the model cannot map it)\n"); return 0; }
    maps_live++;
    return (uintptr_t)f[0];
}
int uvm_unmap_frames(uspace_t* us, uintptr_t a, uint32_t n, uint64_t f0) { (void)us; (void)n; if (a != f0) return -1; maps_live--; return 0; }

#define IOC(dir, nr, size) ((unsigned long)(dir) << 30 | (unsigned long)(size) << 16 | 'd' << 8 | (nr))
#define W 1
#define R 2
#define WR 3
static int io(int owner, int dir, unsigned nr, void* arg, unsigned size) { return gem_ioctl(owner, IOC(dir, nr, size), arg); }
#define I915(nr) (DRM_COMMAND_BASE + (nr))

static int failed, checks;
static void check(const char* what, int ok) { checks++; if (!ok) { failed++; printf("    FAIL: %s\n", what); } }

static uint32_t create(int owner, uint64_t size) {
    struct drm_i915_gem_create c = { size, 0, 0 };
    int r = io(owner, WR, I915(I915_NR_GEM_CREATE), &c, sizeof c);
    return r ? 0 : c.handle;
}
static uint32_t* map(int owner, uint32_t h, uint64_t size) {
    struct drm_i915_gem_mmap m = { h, 0, 0, size, 0, 0 };
    if (io(owner, WR, I915(I915_NR_GEM_MMAP), &m, sizeof m)) return NULL;
    return (uint32_t*)(uintptr_t)m.addr_ptr;
}
static int gem_wait(int owner, uint32_t h, int64_t ns) {
    struct drm_i915_gem_wait w = { h, 0, ns };
    return io(owner, WR, I915(I915_NR_GEM_WAIT), &w, sizeof w);
}

static void report(const char* name, int ok, const char* d) { printf("    gpu3d test: %s %s: %s\n", ok ? "ok  " : "FAIL", name, d); check(name, ok); }

/* a store of v at addr (its address dword: *rel), as each generation has it */
static int sdi(uint32_t* b, int k, uint32_t v, int* rel) {
    b[k++] = (0x20u << 23) | 2u;
    if (gen >= 8) { *rel = k; b[k++] = 0xDEAD0000u; b[k++] = 0; }
    else { b[k++] = 0; *rel = k; b[k++] = 0xDEAD0000u; }
    b[k++] = v;
    return k;
}
/* PIPE_CONTROL's post-sync write (gen 6+) */
static int pc_write(uint32_t* b, int k, uint32_t v, int* rel) {
    if (gen >= 8) {
        b[k++] = 3u << 29 | 3u << 27 | 2u << 24 | 4u; b[k++] = (1u << 14) | (1u << 20) | (1u << 12);
        *rel = k; b[k++] = 0xDEAD0000u; b[k++] = 0; b[k++] = v; b[k++] = 0;
    } else {
        b[k++] = 3u << 29 | 3u << 27 | 2u << 24 | 2u; b[k++] = (1u << 14) | (1u << 20) | (1u << 12);
        *rel = k; b[k++] = 0xDEAD0000u; b[k++] = v;
    }
    return k;
}

static void one(int g, int is_hsw, uint16_t devid) {
    gen = g; hsw = is_hsw;
    printf("gen %s (%04x)\n", hsw ? "7.5" : g == 7 ? "7" : g == 6 ? "6" : g == 5 ? "5" : g == 8 ? "8" : g == 9 ? "9" : "11", devid);
    bar = (uint8_t*)arena_take(bar_size(), 4096);
    memset(bar, 0, bar_size());
    g_ran = 0; bad_writes = 0; hung = 0; g_resets_seen = 0;
    /* (a fresh driver: the globals of the two files start over by running the program again
     *  - so each generation runs in its own process: see main) */
    i915_hw_t hw;
    memset(&hw, 0, sizeof hw);
    hw.mmio = bar; hw.mmio_size = bar_size(); hw.devid = devid; hw.revision = 9; hw.gen = g; hw.hsw = is_hsw;
    hw.gtt_entries = g == 5 ? 256u * 1024u : g >= 8 ? 1024u * 1024u : 512u * 1024u;
    hw.reserved_lo = 8192; hw.reserved_hi = 16;
    model_regs();
    i915_attach(&hw);
    check("present", i915_present());

    int A = 7;                                           /* an app */
    g_on = 0;
    struct drm_version v0; memset(&v0, 0, sizeof v0);
    check("off (gpu3d off): apps get no GPU", io(A, WR, DRM_NR_VERSION, &v0, sizeof v0) == -19 && !i915_running());
    gem_set_enabled(1);
    check("gpu3d on", gem_enabled());
    struct drm_version v; memset(&v, 0, sizeof v);
    char name[16] = {0};
    v.name_len = sizeof name; v.name = (uint64_t)(uintptr_t)name;
    check("DRM_IOCTL_VERSION (starts the ring)", io(A, WR, DRM_NR_VERSION, &v, sizeof v) == 0 && !memcmp(name, "i915", 5));
    check("the ring runs", i915_running());
    int chip = 0;
    struct drm_i915_getparam gp = { I915_PARAM_CHIPSET_ID, 0, (uint64_t)(uintptr_t)&chip };
    check("GETPARAM chipset", io(A, WR, I915(I915_NR_GETPARAM), &gp, sizeof gp) == 0 && chip == devid);
    int llc = -1;
    gp.param = I915_PARAM_HAS_LLC; gp.value = (uint64_t)(uintptr_t)&llc;
    check("GETPARAM has_llc", io(A, WR, I915(I915_NR_GETPARAM), &gp, sizeof gp) == 0 && llc == (g >= 6));
    gp.param = 9999;
    check("GETPARAM unknown: EINVAL", io(A, WR, I915(I915_NR_GETPARAM), &gp, sizeof gp) == -22);

    struct drm_i915_gem_context_create_ext cc = { 0, 0, 0 };
    check("CONTEXT_CREATE", io(A, WR, I915(I915_NR_CONTEXT_CREATE), &cc, 8) == 0 && cc.ctx_id);
    struct drm_i915_gem_context_param cp = { cc.ctx_id, 0, I915_CONTEXT_PARAM_RECOVERABLE, 0 };
    check("CONTEXT_SETPARAM recoverable=0", io(A, WR, I915(I915_NR_CONTEXT_SETPARAM), &cp, sizeof cp) == 0);
    cp.param = I915_CONTEXT_PARAM_GTT_SIZE;
    check("CONTEXT_GETPARAM gtt size", io(A, WR, I915(I915_NR_CONTEXT_GETPARAM), &cp, sizeof cp) == 0 &&
          cp.value == (g >= 8 ? 1ull << 48 : (uint64_t)hw.gtt_entries * 4096u));

    uint32_t hb = create(A, 8192), ht = create(A, 4096), hbig = create(A, 1u << 20);
    check("GEM_CREATE x3", hb && ht && hbig);
    uint32_t* bm = map(A, hb, 8192);
    uint32_t* tm = map(A, ht, 4096);
    check("GEM_MMAP", bm && tm);
    if (!bm || !tm) return;
    /* a batch: two stores and a PIPE_CONTROL write into the target, by relocation */
    /* (gen 5: unprivileged batches may not store to the GTT - Mesa writes with PIPE_CONTROL there) */
    int k = 0, nrel = 0;
    uint32_t rel_at[3], rel_delta[3];
    int ra;
    if (g != 5)
        for (int i = 0; i < 2; i++) {
            k = sdi(bm, k, 0x11110000u + (uint32_t)i, &ra);
            rel_at[nrel] = (uint32_t)ra; rel_delta[nrel++] = 64u + (uint32_t)i * 4u;
        }
    if (g == 5) { bm[k++] = (3u << 29 | 3u << 27 | 2u << 24 | 2u) | (1u << 14); rel_at[nrel] = (uint32_t)k; rel_delta[nrel++] = 72; bm[k++] = 0xDEAD0000u; bm[k++] = 0x22220000u; bm[k++] = 0; }
    else { k = pc_write(bm, k, 0x22220000u, &ra); rel_at[nrel] = (uint32_t)ra; rel_delta[nrel++] = 72; }
    bm[k++] = 0x0Au << 23; bm[k++] = 0;
    struct drm_i915_gem_relocation_entry rl[3];
    for (int i = 0; i < nrel; i++) rl[i] = (struct drm_i915_gem_relocation_entry){ 1 /* LUT: the target */, rel_delta[i], rel_at[i] * 4u, 0, 2, 2 };
    struct drm_i915_gem_exec_object2 ex[2];
    memset(ex, 0, sizeof ex);
    ex[0].handle = hb; ex[0].relocation_count = (uint32_t)nrel; ex[0].relocs_ptr = (uint64_t)(uintptr_t)rl;
    ex[1].handle = ht; ex[1].flags = EXEC_OBJECT_WRITE;
    struct drm_syncobj_create sc = { 0, 0 };
    check("SYNCOBJ_CREATE", io(A, WR, DRM_NR_SYNCOBJ_CREATE, &sc, sizeof sc) == 0 && sc.handle);
    struct drm_i915_gem_exec_fence fe = { sc.handle, I915_EXEC_FENCE_SIGNAL };
    struct drm_i915_gem_execbuffer2 eb;
    memset(&eb, 0, sizeof eb);
    eb.buffers_ptr = (uint64_t)(uintptr_t)ex; eb.buffer_count = 2; eb.batch_len = (uint32_t)k * 4;
    eb.flags = I915_EXEC_RENDER | I915_EXEC_NO_RELOC | I915_EXEC_HANDLE_LUT | I915_EXEC_BATCH_FIRST | I915_EXEC_FENCE_ARRAY;
    eb.cliprects_ptr = (uint64_t)(uintptr_t)&fe; eb.num_cliprects = 1; eb.rsvd1 = cc.ctx_id;
    int r = io(A, W, I915(I915_NR_GEM_EXECBUFFER2), &eb, sizeof eb);
    check("EXECBUFFER2", r == 0);
    check("the offsets written back", ex[1].offset && ex[0].offset && ex[1].offset != ex[0].offset);
    check("the relocations' presumed offsets", rl[0].presumed_offset == ex[1].offset);
    check("GEM_WAIT", gem_wait(A, ht, 1000000000ll) == 0);
    if (g != 5) check("the stores landed (PPGTT)", tm[16] == 0x11110000u && tm[17] == 0x11110001u);
    check("the PIPE_CONTROL write landed", tm[18] == 0x22220000u);
    struct drm_i915_gem_busy busy = { ht, 7 };
    check("GEM_BUSY: idle", io(A, WR, I915(I915_NR_GEM_BUSY), &busy, sizeof busy) == 0 && busy.busy == 0);
    uint32_t hs = sc.handle;
    struct drm_syncobj_wait sw = { (uint64_t)(uintptr_t)&hs, (int64_t)(now_ms + 1000) * 1000000, 1, 0, 0, 0 };
    check("SYNCOBJ_WAIT: signalled", io(A, WR, DRM_NR_SYNCOBJ_WAIT, &sw, sizeof sw) == 0);
    struct drm_syncobj_create sc2 = { 0, 0 };
    io(A, WR, DRM_NR_SYNCOBJ_CREATE, &sc2, sizeof sc2);
    hs = sc2.handle;
    sw.timeout_nsec = (int64_t)(now_ms + 50) * 1000000;
    check("SYNCOBJ_WAIT: an unsubmitted one -> EINVAL", io(A, WR, DRM_NR_SYNCOBJ_WAIT, &sw, sizeof sw) == -22);
    sw.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
    check("SYNCOBJ_WAIT for submit: times out", io(A, WR, DRM_NR_SYNCOBJ_WAIT, &sw, sizeof sw) == -62);

    /* again, offsets now right: the kernel leaves the batch alone */
    uint32_t mark = bm[rel_at[nrel - 1]];
    tm[18] = 0;
    check("EXECBUFFER2 again", io(A, W, I915(I915_NR_GEM_EXECBUFFER2), &eb, sizeof eb) == 0);
    check("no relocation rewritten", bm[rel_at[nrel - 1]] == mark);
    check("GEM_WAIT again", gem_wait(A, ht, -1) == 0 && tm[18] == 0x22220000u);

    /* gen 8+: the parameters iris needs, the topology, softpin */
    if (g >= 8) {
        int v = -1;
        gp.param = I915_PARAM_HAS_EXEC_SOFTPIN; gp.value = (uint64_t)(uintptr_t)&v;
        check("GETPARAM has_exec_softpin", io(A, WR, I915(I915_NR_GETPARAM), &gp, sizeof gp) == 0 && v == 1);
        gp.param = I915_PARAM_CS_TIMESTAMP_FREQUENCY;
        check("GETPARAM timestamp frequency", io(A, WR, I915(I915_NR_GETPARAM), &gp, sizeof gp) == 0 &&
              v == (g == 8 ? 12500000 : g == 9 ? 12000000 : 19200000));
        gp.param = I915_PARAM_EU_TOTAL;
        check("GETPARAM eu_total", io(A, WR, I915(I915_NR_GETPARAM), &gp, sizeof gp) == 0 && v == (g == 8 ? 24 : g == 9 ? 24 : 32));
        struct drm_i915_query_item qi = { DRM_I915_QUERY_TOPOLOGY_INFO, 0, 0, 0 };
        struct drm_i915_query q = { 1, 0, (uint64_t)(uintptr_t)&qi };
        check("QUERY topology: its length", io(A, WR, I915(I915_NR_QUERY), &q, sizeof q) == 0 && qi.length > 16);
        uint8_t topo[128];
        memset(topo, 0, sizeof topo);
        qi.data_ptr = (uint64_t)(uintptr_t)topo;
        struct drm_i915_query_topology_info* ti = (struct drm_i915_query_topology_info*)topo;
        check("QUERY topology", io(A, WR, I915(I915_NR_QUERY), &q, sizeof q) == 0 && qi.length > 16 && topo[16] == 1 &&
              topo[16 + ti->subslice_offset] == (g == 9 ? 0x7 : g == 8 ? 0x7 : 0xFF) && topo[16 + ti->eu_offset] == (g == 11 ? 0xF0 : 0xFF));
        struct drm_i915_gem_context_param vp = { 0, 0, I915_CONTEXT_PARAM_VM, 0 };
        check("CONTEXT_GETPARAM vm (the app's address space)", io(A, WR, I915(I915_NR_CONTEXT_GETPARAM), &vp, sizeof vp) == 0 && vp.value);
        /* a context made with extensions: recoverable off, that address space */
        struct drm_i915_gem_context_create_ext_setparam e2 = { { 0, I915_CONTEXT_CREATE_EXT_SETPARAM, 0, { 0 } }, { 0, 0, I915_CONTEXT_PARAM_VM, vp.value } };
        struct drm_i915_gem_context_create_ext_setparam e1 = { { (uint64_t)(uintptr_t)&e2, I915_CONTEXT_CREATE_EXT_SETPARAM, 0, { 0 } }, { 0, 0, I915_CONTEXT_PARAM_RECOVERABLE, 0 } };
        struct drm_i915_gem_context_create_ext ce = { 0, I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS, (uint64_t)(uintptr_t)&e1 };
        check("CONTEXT_CREATE_EXT with SETPARAMs", io(A, WR, I915(I915_NR_CONTEXT_CREATE), &ce, sizeof ce) == 0 && ce.ctx_id);
        /* softpin: buffers where the app says, high in the 48-bit space (as iris: canonical addresses) */
        uint32_t hp = create(A, 4096), hq = create(A, 4096);
        uint32_t* pm = map(A, hp, 4096);
        uint32_t* qm = map(A, hq, 4096);
        uint64_t qa = 0x0000700000010000ull, pa = 0xFFFF800000200000ull;     /* (the second: bit 47, sign-extended) */
        int kk = 0;
        pm[kk++] = (0x20u << 23) | 2u; pm[kk++] = (uint32_t)qa + 32; pm[kk++] = (uint32_t)(qa >> 32); pm[kk++] = 0x50F70001u;
        pm[kk++] = 0x0Au << 23; pm[kk++] = 0;
        struct drm_i915_gem_exec_object2 xs[2];
        memset(xs, 0, sizeof xs);
        xs[0].handle = hp; xs[0].offset = pa; xs[0].flags = EXEC_OBJECT_PINNED | EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
        xs[1].handle = hq; xs[1].offset = qa; xs[1].flags = EXEC_OBJECT_PINNED | EXEC_OBJECT_SUPPORTS_48B_ADDRESS | EXEC_OBJECT_WRITE;
        struct drm_i915_gem_execbuffer2 es;
        memset(&es, 0, sizeof es);
        es.buffers_ptr = (uint64_t)(uintptr_t)xs; es.buffer_count = 2; es.batch_len = (uint32_t)kk * 4;
        es.flags = I915_EXEC_RENDER | I915_EXEC_NO_RELOC | I915_EXEC_HANDLE_LUT | I915_EXEC_BATCH_FIRST; es.rsvd1 = ce.ctx_id;
        check("EXECBUFFER2 softpinned (48-bit addresses)", io(A, W, I915(I915_NR_GEM_EXECBUFFER2), &es, sizeof es) == 0);
        check("its offsets left as given", xs[0].offset == pa && xs[1].offset == qa);
        check("the store landed at the softpinned address", gem_wait(A, hq, -1) == 0 && qm[8] == 0x50F70001u);
        /* the address given to another buffer (the first one closed): it lands there now */
        struct drm_gem_close gq = { hq, 0 };
        io(A, W, DRM_NR_GEM_CLOSE, &gq, sizeof gq);
        uint32_t hr = create(A, 4096);
        uint32_t* rm = map(A, hr, 4096);
        xs[1].handle = hr;
        pm[3] = 0x50F70002u;
        check("an address reused by another buffer", io(A, W, I915(I915_NR_GEM_EXECBUFFER2), &es, sizeof es) == 0 &&
              gem_wait(A, hr, -1) == 0 && rm[8] == 0x50F70002u);
        xs[1].offset = 0x123;
        check("a softpinned offset not page-aligned: EINVAL", io(A, W, I915(I915_NR_GEM_EXECBUFFER2), &es, sizeof es) == -22);
    }

    /* An aligned offset near UINT64_MAX used to wrap the end check and
     * index far beyond the physical-page table. No batch may be submitted. */
    uint64_t saved_offset = rl[0].offset, saved_presumed = rl[0].presumed_offset;
    rl[0].offset = ~3ull;
    rl[0].presumed_offset = ~0ull;
    check("overflowing relocation rejected",
          io(A, W, I915(I915_NR_GEM_EXECBUFFER2), &eb, sizeof eb) == -22);
    rl[0].offset = 8192;
    check("relocation beyond buffer rejected",
          io(A, W, I915(I915_NR_GEM_EXECBUFFER2), &eb, sizeof eb) == -22);
    rl[0].offset = saved_offset;
    rl[0].presumed_offset = saved_presumed;
    check("valid submission works after rejected relocation",
          io(A, W, I915(I915_NR_GEM_EXECBUFFER2), &eb, sizeof eb) == 0 && gem_wait(A, ht, -1) == 0);

    /* hardware contexts (gen 6+): each keeps its own state between batches */
    int iso = -1;
    gp.param = I915_PARAM_HAS_CONTEXT_ISOLATION; gp.value = (uint64_t)(uintptr_t)&iso;
    check("GETPARAM has_context_isolation", io(A, WR, I915(I915_NR_GETPARAM), &gp, sizeof gp) == 0 && iso == (g >= 6));
    if (g >= 6) {
        uint32_t ids[2];
        for (int i = 0; i < 2; i++) {
            struct drm_i915_gem_context_create_ext c2 = { 0, 0, 0 };
            io(A, WR, I915(I915_NR_CONTEXT_CREATE), &c2, 8);
            ids[i] = c2.ctx_id;
        }
        uint32_t hc = create(A, 4096);
        uint32_t* cm = map(A, hc, 4096);
        struct drm_i915_gem_exec_object2 exc[2];
        struct drm_i915_gem_relocation_entry rc;
        struct drm_i915_gem_execbuffer2 ebc;
        /* a batch: "state" in a register (LRI), or that register stored into the target (SRM) */
        #define CTX_BATCH(ctx, store, val) do { \
            int q = 0; \
            if (store) { cm[q++] = (0x24u << 23) | (g >= 8 ? 2u : 1u); cm[q++] = 0x7300; cm[q++] = 0xDEAD0000u; if (g >= 8) cm[q++] = 0; } \
            else { cm[q++] = (0x22u << 23) | 1u; cm[q++] = 0x7300; cm[q++] = (val); } \
            cm[q++] = 0x0Au << 23; cm[q++] = 0; \
            rc = (struct drm_i915_gem_relocation_entry){ 1, 96, 8, 0, 2, 2 }; \
            memset(exc, 0, sizeof exc); \
            exc[0].handle = hc; exc[0].relocation_count = (store) ? 1 : 0; exc[0].relocs_ptr = (uint64_t)(uintptr_t)&rc; \
            exc[1].handle = ht; exc[1].flags = EXEC_OBJECT_WRITE; \
            memset(&ebc, 0, sizeof ebc); \
            ebc.buffers_ptr = (uint64_t)(uintptr_t)exc; ebc.buffer_count = 2; ebc.batch_len = (uint32_t)q * 4; \
            ebc.flags = I915_EXEC_RENDER | I915_EXEC_HANDLE_LUT | I915_EXEC_BATCH_FIRST; ebc.rsvd1 = (ctx); \
            r = io(A, W, I915(I915_NR_GEM_EXECBUFFER2), &ebc, sizeof ebc); \
            gem_wait(A, ht, -1); \
        } while (0)
        uint32_t inh0 = ctx_inhibits, sw0 = ctx_switches;
        CTX_BATCH(ids[0], 0, 0xAAAA0001u);
        CTX_BATCH(ids[1], 0, 0xBBBB0002u);
        check("two new contexts: switched to, from the golden image", r == 0 && ctx_inhibits == inh0 && ctx_restores > 0);
        tm[24] = 0;
        CTX_BATCH(ids[0], 1, 0);
        check("context A's state back after B's batch", r == 0 && tm[24] == 0xAAAA0001u);
        CTX_BATCH(ids[1], 1, 0);
        check("context B's state back after A's", r == 0 && tm[24] == 0xBBBB0002u);
        CTX_BATCH(ids[1], 1, 0);
        check("the same context twice: no switch", ctx_switches - sw0 == 4);
        CTX_BATCH(0, 1, 0);
        check("the app's default context: its own state (not B's)", r == 0 && tm[24] != 0xBBBB0002u);
        struct drm_i915_gem_context_destroy cd = { ids[1], 0 };
        check("CONTEXT_DESTROY of the one the GPU holds", io(A, W, I915(I915_NR_CONTEXT_DESTROY), &cd, sizeof cd) == 0);
        check("its handle is gone", io(A, W, I915(I915_NR_CONTEXT_DESTROY), &cd, sizeof cd) == -2);
        CTX_BATCH(ids[0], 1, 0);
        check("A still has its state", r == 0 && tm[24] == 0xAAAA0001u);
    }

    /* many: the ring wraps */
    uint32_t t = 0;
    for (int i = 0; i < 3000 && !r; i++) { r = io(A, W, I915(I915_NR_GEM_EXECBUFFER2), &eb, sizeof eb); t++; }
    check("3000 submissions", r == 0 && gem_wait(A, ht, -1) == 0);
    check("the model ran every batch", g_ran == 3003 + (g >= 6 ? 7 : 0) + (g >= 8 ? 2 : 0));

    /* a big buffer away from the 64 KiB-aligned place: alignment asked for */
    struct drm_i915_gem_exec_object2 ex2[2];
    memset(ex2, 0, sizeof ex2);
    ex2[0] = ex[0]; ex2[0].relocation_count = 0;
    ex2[1].handle = hbig; ex2[1].alignment = 1u << 20;
    eb.buffers_ptr = (uint64_t)(uintptr_t)ex2;
    uint32_t fl = (uint32_t)eb.flags;
    eb.flags &= ~(uint64_t)I915_EXEC_FENCE_ARRAY;
    check("EXECBUFFER2 with a 1 MiB alignment", io(A, W, I915(I915_NR_GEM_EXECBUFFER2), &eb, sizeof eb) == 0 && ex2[1].offset % (1u << 20) == 0);
    eb.flags = fl;
    eb.buffers_ptr = (uint64_t)(uintptr_t)ex;

    /* a hung batch: reset after 2 s; the context is lost (not recoverable) */
    uint32_t hh = create(A, 4096);
    uint32_t* hm = map(A, hh, 4096);
    hm[0] = 0x3Fu << 23; hm[1] = 0x0Au << 23;
    struct drm_i915_gem_exec_object2 exh = { hh, 0, 0, 0, 0, 0, 0, 0 };
    struct drm_i915_gem_execbuffer2 ebh = eb;
    ebh.buffers_ptr = (uint64_t)(uintptr_t)&exh; ebh.buffer_count = 1; ebh.flags = I915_EXEC_RENDER;
    quiet = 1;
    check("EXECBUFFER2 (a batch that hangs)", io(A, W, I915(I915_NR_GEM_EXECBUFFER2), &ebh, sizeof ebh) == 0);
    uint32_t t0 = now_ms;
    int wr = gem_wait(A, hh, -1);
    quiet = 0;
    check("GEM_WAIT: -EIO after the reset", wr == -5 && now_ms - t0 >= 2000);
    check("the GPU was reset", g_resets_seen == 1 && i915_resets() == 1 && i915_running());
    struct drm_i915_reset_stats st = { cc.ctx_id, 0, 0, 0, 0, 0 };
    check("GET_RESET_STATS: the batch was active", io(A, WR, I915(I915_NR_GET_RESET_STATS), &st, sizeof st) == 0 && st.batch_active == 1);
    check("the lost context: EXECBUFFER2 -> EIO", io(A, W, I915(I915_NR_GEM_EXECBUFFER2), &eb, sizeof eb) == -5);
    struct drm_i915_gem_context_create_ext cc2 = { 0, 0, 0 };
    io(A, WR, I915(I915_NR_CONTEXT_CREATE), &cc2, 8);
    eb.rsvd1 = cc2.ctx_id;
    tm[18] = 0;
    check("a new context works after the reset", io(A, W, I915(I915_NR_GEM_EXECBUFFER2), &eb, sizeof eb) == 0 && gem_wait(A, ht, -1) == 0 && tm[18] == 0x22220000u);

    /* closing a buffer the GPU still uses: freed when the GPU is done */
    uint32_t before = pages_out;
    check("EXECBUFFER2", io(A, W, I915(I915_NR_GEM_EXECBUFFER2), &eb, sizeof eb) == 0);
    struct drm_gem_close gc = { ht, 0 };
    check("GEM_CLOSE while busy", io(A, W, DRM_NR_GEM_CLOSE, &gc, sizeof gc) == 0);
    check("its handle is gone", gem_wait(A, ht, 0) == -2);
    task_sleep_ms(5);
    struct drm_i915_gem_get_aperture ap;
    io(A, R, I915(I915_NR_GEM_GET_APERTURE), &ap, sizeof ap);
    io(A, 0, I915(I915_NR_GEM_THROTTLE), NULL, 0);
    check("then freed", pages_out == before - 1);
    check("another app cannot use A's buffer", gem_wait(A + 1, hb, 0) == -2);
    gem_release(A);
    check("all of the app's pages back at its end", pages_out == 0);
    check("no stray GPU writes", bad_writes == 0);

    /* Retiring the last busy handle must not leave the next one outside
     * the live handle table. Repeat to catch progressive handle leaks. */
    for (int iteration = 0; iteration < 3; iteration++) {
        uint32_t old = create(A, 4096);
        uint32_t* batch = map(A, old, 4096);
        check("retirement fixture mapped", batch != NULL);
        if (!batch) break;
        batch[0] = 0x0Au << 23; batch[1] = 0;
        struct drm_i915_gem_exec_object2 object = {0};
        object.handle = old;
        struct drm_i915_gem_execbuffer2 submit = {0};
        submit.buffers_ptr = (uint64_t)(uintptr_t)&object;
        submit.buffer_count = 1;
        check("retirement fixture submitted", io(A, W, I915(I915_NR_GEM_EXECBUFFER2), &submit, sizeof submit) == 0);
        struct drm_gem_close close = {old, 0};
        check("retirement fixture closed", io(A, W, DRM_NR_GEM_CLOSE, &close, sizeof close) == 0);
        task_sleep_ms(5);
        uint32_t fresh = create(A, 4096);
        check("allocation after tail retirement returns a usable handle",
              fresh && gem_wait(A, fresh, 0) == 0);
        check("retired tail slot is reused", fresh == old);
        gem_release(A);
        check("retirement cycle releases every page", pages_out == 0);
    }

    /* Context extension chains are bounded and failure must roll back. */
    struct drm_i915_gem_context_create_ext_setparam extensions[17];
    memset(extensions, 0, sizeof extensions);
    for (int i = 0; i < 17; i++) {
        extensions[i].base.next_extension = i < 16 ? (uint64_t)(uintptr_t)&extensions[i + 1] : 0;
        extensions[i].base.name = I915_CONTEXT_CREATE_EXT_SETPARAM;
        extensions[i].param.param = I915_CONTEXT_PARAM_RECOVERABLE;
        extensions[i].param.value = 1;
    }
    struct drm_i915_gem_context_create_ext chain = {0, I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS, (uint64_t)(uintptr_t)extensions};
    check("overlong context chain rejected", io(A, WR, I915(I915_NR_CONTEXT_CREATE), &chain, sizeof chain) == -22);
    struct drm_i915_gem_context_destroy destroy = {chain.ctx_id, 0};
    check("rejected context is not live", io(A, W, I915(I915_NR_CONTEXT_DESTROY), &destroy, sizeof destroy) == -2);
    extensions[15].base.next_extension = 0;
    chain.ctx_id = 0;
    check("sixteen context extensions supported", io(A, WR, I915(I915_NR_CONTEXT_CREATE), &chain, sizeof chain) == 0);
    destroy.ctx_id = chain.ctx_id;
    check("bounded context can be destroyed", io(A, W, I915(I915_NR_CONTEXT_DESTROY), &destroy, sizeof destroy) == 0);
    extensions[0].base.next_extension = (uint64_t)(uintptr_t)extensions;
    chain.ctx_id = 0;
    check("cyclic context chain rejected", io(A, WR, I915(I915_NR_CONTEXT_CREATE), &chain, sizeof chain) == -22);
    gem_release(A);

    /* gpu3d test */
    i915_tests(report);
}

#define NGEN 9
int main(int argc, char** argv) {
    arena = mmap(0, arena_size, 3, 0x22 | 0x40 /* MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT */, -1, 0);
    if (!arena || arena == (void*)-1) { printf("no arena\n"); return 2; }
    int which = argc > 1 ? argv[1][0] - '0' : -1;
    if (which >= NGEN) return 2;
    if (which < 0) {
        /* each generation in a process of its own (the drivers' state is global) */
        extern int system(const char*);
        char cmd[512];
        int bad = 0;
        for (int i = 0; i < NGEN; i++) { ksnprintf(cmd, sizeof cmd, "%s %d", argv[0], i); bad += system(cmd) != 0; }
        printf(bad ? "%d generation(s) FAILED\n" : "all %d generations passed\n", bad ? bad : NGEN);
        return bad != 0;
    }
    static const struct { int gen, hsw; uint16_t id; } G[NGEN] = {
        { 5, 0, 0x0046 }, { 6, 0, 0x0116 }, { 7, 0, 0x0166 }, { 7, 1, 0x0416 },        /* Ironlake, Sandy / Ivy Bridge, Haswell */
        { 8, 0, 0x1616 }, { 9, 0, 0x1912 }, { 9, 0, 0x3E92 }, { 9, 0, 0x9BC5 },        /* Broadwell, Skylake, Coffee Lake, Comet Lake */
        { 11, 0, 0x8A52 },                                                             /* Ice Lake */
    };
    one(G[which].gen, G[which].hsw, G[which].id);
    printf("  %d of %d checks passed\n", checks - failed, checks);
    return failed != 0;
}
