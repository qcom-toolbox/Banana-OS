/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The GPU model and kernel stubs of tools/tests/test_i915.c, shared with
 * tools/tests/crocus_host (Mesa's crocus on the model). */
#include "gpu_i915.h"
#include "i915_uapi.h"
#include "uvm.h"
#include "kstring.h"

extern int printf(const char*, ...);
extern int vsnprintf(char*, size_t, const char*, __builtin_va_list);
extern void* mmap(void*, size_t, int, int, int, long);
extern void exit(int);

/* ── memory: an arena below 4 GiB (physical = virtual) ─────────────── */
#ifndef MODEL_ARENA_MB
#define MODEL_ARENA_MB 512
#endif
static uint8_t* arena;
static uint64_t arena_used, arena_size = (uint64_t)MODEL_ARENA_MB << 20;
static void* arena_take(uint64_t n, uint64_t align) {
    arena_used = (arena_used + align - 1) & ~(align - 1);
    if (arena_used + n > arena_size) { printf("arena full\n"); exit(2); }
    void* p = arena + arena_used;
    arena_used += n;
    return p;
}
void* kmalloc(size_t n) { return arena_take(n ? n : 1, 16); }
void* kzalloc(size_t n) { void* p = kmalloc(n); memset(p, 0, n); return p; }
static struct { void* ptr; size_t bytes; } aligned_blocks[4096];
static unsigned aligned_count;
void* kmalloc_aligned(size_t n, size_t a) {
    void* p = arena_take(n, a);
    if (aligned_count >= 4096) { printf("too many aligned blocks\n"); exit(2); }
    aligned_blocks[aligned_count].ptr = p;
    aligned_blocks[aligned_count++].bytes = n;
    return p;
}
void  kfree(void* p) { (void)p; }
static void* freelist[65536];
static int nfree;
static uint32_t pages_out;
void* upage_alloc(void) {
    pages_out++;
#ifdef MODEL_NO_REUSE                       /* (a buffer's pages stay contiguous: they can be "mapped") */
    return arena_take(4096, 4096);
#else
    return nfree ? freelist[--nfree] : arena_take(4096, 4096);
#endif
}
void  upage_free(void* p) { pages_out--; if (nfree < 65536) freelist[nfree++] = p; }

void* memcpy(void* d, const void* s, size_t n) { uint8_t* a = d; const uint8_t* b = s; while (n--) *a++ = *b++; return d; }
void* memset(void* d, int c, size_t n) { uint8_t* a = d; while (n--) *a++ = (uint8_t)c; return d; }
int memcmp(const void* x, const void* y, size_t n) { const uint8_t* a = x; const uint8_t* b = y; for (; n; n--, a++, b++) if (*a != *b) return *a - *b; return 0; }
size_t strlen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
size_t kstrlcpy(char* d, const char* s, size_t n) { size_t l = strlen(s); if (n) { size_t c = l < n - 1 ? l : n - 1; memcpy(d, s, c); d[c] = 0; } return l; }
int ksnprintf(char* b, size_t n, const char* f, ...) { __builtin_va_list ap; __builtin_va_start(ap, f); int r = vsnprintf(b, n, f, ap); __builtin_va_end(ap); return r; }
static int quiet;
void klog(const char* f, ...) { if (quiet) return; char b[512]; __builtin_va_list ap; __builtin_va_start(ap, f); vsnprintf(b, sizeof b, f, ap); __builtin_va_end(ap); printf("    klog: %s", b); }

/* ── the GPU model ─────────────────────────────────────────────────── */
static uint8_t* bar;                       /* 4 MiB: registers, then the GTT */
static int gen, hsw;
static int hung;                            /* a batch is stuck */
static uint32_t now_ms = 1000;
static int pumping;
#define REG(r) (*(volatile uint32_t*)(bar + (r)))
static uint32_t g_ran, g_resets_seen;

static uint32_t* phys_ptr(uint64_t pa) { return (uint32_t*)(uintptr_t)pa; }
static uint32_t bar_size(void) { return gen >= 8 ? 16u << 20 : 4u << 20; }
/* gen 8+: 64-bit GTT entries in the upper half; the PPGTT a context's 4 levels */
static uint64_t cur_pml4;                    /* the running context's (0: none) */
static uint32_t* xlate8(uint64_t a, int ppgtt) {
    uint64_t e;
    if (ppgtt) {
        if (!cur_pml4) return NULL;
        uint64_t* t = (uint64_t*)(uintptr_t)cur_pml4;
        for (int level = 4; level > 1; level--) {
            uint64_t d = t[(a >> (12 + 9 * (level - 1))) & 511];
            if (!(d & 1)) return NULL;
            t = (uint64_t*)(uintptr_t)(d & 0x7FFFFFF000ull);
        }
        e = t[(a >> 12) & 511];
    } else {
        if (a >= 4ull << 30) return NULL;
        e = ((volatile uint64_t*)(bar + bar_size() / 2))[a >> 12];
    }
    if (!(e & 1)) return NULL;
    return phys_ptr((e & 0x7FFFFFF000ull) + (a & 4095));
}
static uint64_t pte_phys(uint32_t e) {
    uint64_t hi = gen == 5 ? (e & 0xF0u) : hsw ? (e & 0x7F0u) : (e & 0xFF0u);
    return (uint64_t)(e & 0xFFFFF000u) | hi << 28;
}
/* an address of the GPU's: through the GTT, or the PPGTT (its directory: PP_DIR_BASE) */
static uint32_t* xlate(uint32_t a, int ppgtt) {
    if (gen >= 8) return xlate8(a, ppgtt);
    volatile uint32_t* gsm = (volatile uint32_t*)(bar + (2u << 20));
    uint32_t e;
    if (ppgtt && gen >= 6) {
        uint32_t base = REG(0x2228) >> 10;                 /* the directory's byte offset in the GTT's entries */
        uint32_t pde = gsm[base / 4 + (a >> 22)];
        if (!(pde & 1)) return NULL;
        uint32_t* pt = phys_ptr(pte_phys(pde));
        e = pt[(a >> 12) & 1023];
    } else e = gsm[a >> 12];
    if (!(e & 1)) return NULL;
    return phys_ptr(pte_phys(e) + (a & 4095));
}
static int bad_writes;
static void gpu_write64(uint64_t a, uint32_t v, int ppgtt) {
    uint32_t* p = gen >= 8 ? xlate8(a & ~3ull, ppgtt) : xlate((uint32_t)a & ~3u, ppgtt);
    if (!p) { bad_writes++; printf("    model: a write to unmapped %llx (%s)\n", (unsigned long long)a, ppgtt ? "PPGTT" : "GTT"); return; }
    *p = v;
}
static void gpu_write(uint32_t a, uint32_t v, int ppgtt) { gpu_write64(a, v, ppgtt); }

/* hardware contexts (gen 6+): the registers batches set with
 * MI_LOAD_REGISTER_IMM stand for the 3D state; MI_SET_CONTEXT saves them
 * into the image the GPU holds and restores the next one's (unless
 * MI_RESTORE_INHIBIT) - the image: a count, then (register, value) pairs */
#define CTXREG_MAX 64
static uint32_t ctxreg[CTXREG_MAX], nctxreg;
static uint32_t ctx_cur;                    /* the image's GTT address (0: none) */
static uint32_t ctx_switches, ctx_inhibits, ctx_restores;
static void ctxreg_note(uint32_t r) {
    if (r < 0x2000 || r >= 0x8000 || (r >= 0x2000 && r < 0x2400)) return;   /* (3D state, not the ring's own) */
    for (uint32_t i = 0; i < nctxreg; i++) if (ctxreg[i] == r) return;
    if (nctxreg < CTXREG_MAX) ctxreg[nctxreg++] = r;
}
static uint32_t* xlate(uint32_t a, int ppgtt);
static void ctx_save(void) {
    if (!ctx_cur) return;
    /* The real engine saves the whole hardware context, not just this
     * model's small register sample. Check its destination allocation. */
    uint32_t* base = xlate(ctx_cur, 0);
    size_t need = gen == 6 ? 8192 : hsw ? 17u * 4096u : 8192;
    int fits = 0;
    for (unsigned k = 0; k < aligned_count; k++)
        if (aligned_blocks[k].ptr == base && aligned_blocks[k].bytes >= need) fits = 1;
    if (!fits) { printf("model: hardware context save exceeds its allocation\n"); bad_writes++; }
    uint32_t* img = xlate(ctx_cur + 0x400, 0);
    if (!img) return;
    img[0] = nctxreg;
    for (uint32_t i = 0; i < nctxreg; i++) { img[1 + 2 * i] = ctxreg[i]; img[2 + 2 * i] = REG(ctxreg[i]); }
}
static void ctx_load(uint32_t a, int inhibit) {
    if (inhibit) { ctx_inhibits++; return; }
    uint32_t* img = xlate(a + 0x400, 0);
    if (!img || img[0] > CTXREG_MAX) return;
    ctx_restores++;
    for (uint32_t i = 0; i < nctxreg; i++) REG(ctxreg[i]) = 0;     /* (what the image does not have: as at start) */
    for (uint32_t i = 0; i < img[0]; i++) { ctxreg_note(img[1 + 2 * i]); REG(img[1 + 2 * i]) = img[2 + 2 * i]; }
}
/* what the GPU's registers say before the driver touches them */
static void model_regs(void) {
    if (gen == 6) REG(0x21A0) = 1u << 24 | 63u << 18 | 60u << 6 | 2u; /* ring + extended + pipeline = 125 cache lines; POWER is not RING */
    if (gen == 7) REG(0x21A8) = 100u << 9 | 20u;                /* GEN7_CXT_SIZE: 120 x 64 bytes */
    if (gen == 8 || gen == 9) REG(0x9120) = 1u << 25 | (gen == 8 ? 0u : 1u << 23);   /* FUSE2: slice 0; (gen 9: subslice 3 off) */
    if (gen == 9) REG(0xA26C) = 2u << 1;                        /* CTC_MODE: 24 MHz / 2 */
    if (gen == 11) { REG(0x9138) = 1; REG(0x913C) = 0; REG(0x9134) = 0x0F; REG(0x0D00) = 1u << 3 | 3u << 1; }   /* (EUs 0-3 off; 19.2 MHz) */
    ctx_cur = 0; nctxreg = 0; cur_pml4 = 0;
}

/* runs commands at a (through the GTT or the PPGTT) until a batch end; 0 when done, -1 stuck */
static int run(uint32_t a, int ppgtt, int depth, uint32_t end, int is_ring);
static int run8(uint64_t a, int ppgtt, int depth);
static uint32_t hws_base(void) { return gen == 5 ? REG(0x2080) : REG(0x4080); }

static int exec_cmd(uint32_t* c, uint32_t* len, int ppgtt, int depth) {
    uint32_t h = c[0], client = h >> 29;
    *len = 1;
    if (client == 0) {                                   /* MI */
        uint32_t op = (h >> 23) & 0x3F;
        if (op >= 0x10) *len = (h & 0x3F) + 2;
        if (op == 0x22 || op == 0x1A) *len = (h & 0xFF) + 2;              /* LOAD_REGISTER_IMM, MATH */
        if (op == 0x31) *len = (gen == 5 ? (h & 0x3F) : (h & 0xFF)) + 2;   /* (gen 5: bits 7:6 are MI_BATCH_GTT) */
        switch (op) {
        case 0x0A: return 1;                             /* MI_BATCH_BUFFER_END */
        case 0x21: gpu_write(hws_base() + c[1], c[2], 0); break;     /* STORE_DWORD_INDEX */
        case 0x20: if (gen >= 8) {                       /* STORE_DATA_IMM, 64-bit address */
            int global = (h >> 22) & 1;
            gpu_write64(c[1] | (uint64_t)c[2] << 32, c[3], ppgtt && !global);
            break;
        } else {
            int global = (h >> 22) & 1;
            if (gen == 5 && ppgtt && global) { printf("    model: an unprivileged batch stored to the GTT\n"); bad_writes++; break; }
            gpu_write(c[2], c[3], gen >= 6 && ppgtt && !global);
            break;
        }
        case 0x24:                                       /* STORE_REGISTER_MEM */
            if (gen >= 8) gpu_write64(c[2] | (uint64_t)c[3] << 32, REG(c[1] & 0x3FFFFC), ppgtt);
            else gpu_write(c[2], REG(c[1] & 0x3FFFFC), gen >= 6 && ppgtt);
            break;
        case 0x22:                                       /* LOAD_REGISTER_IMM: (register, value) pairs */
            for (uint32_t k = 1; k + 1 < *len; k += 2) { uint32_t r = c[k] & 0x3FFFFC; REG(r) = c[k + 1]; ctxreg_note(r); }
            break;
        case 0x18: {                                     /* SET_CONTEXT */
            uint32_t a = c[1] & ~0xFFFu, fl = c[1] & 0xFFFu;
            if (gen < 6 || gen >= 8 || depth > 0) { printf("    model: MI_SET_CONTEXT %s\n", gen < 6 ? "on gen 5" : gen >= 8 ? "with execlists" : "in a batch"); bad_writes++; break; }
            if (!(fl & (1u << 8)) || (gen == 6 && (a & 0xFFFF))) { printf("    model: MI_SET_CONTEXT %08x: not GTT / misaligned\n", c[1]); bad_writes++; }
            ctx_save();
            ctx_load(a, fl & 1);
            ctx_cur = a;
            ctx_switches++;
            REG(0x2180) = a | 1;                         /* CCID */
            break;
        }
        case 0x3F: hung = 1; return -1;                  /* (the tests' "hang here") */
        case 0x31: if (gen >= 8) {                       /* BATCH_BUFFER_START, 64-bit */
            if (depth > 2) return -1;
            g_ran++;
            int r = run8(c[1] | (uint64_t)c[2] << 32, (h >> 8) & 1, depth + 1);
            if (r < 0) return -1;
            if (depth > 0) return 1;
            break;
        } else {
            int unpriv = gen == 5 ? 0 : (h >> 8) & 1;
            if (gen == 5 && !((h >> 7) & 1)) { printf("    model: a gen 5 batch without MI_BATCH_GTT\n"); bad_writes++; }
            if (depth > 2) return -1;
            g_ran++;
            int r = run(c[1], unpriv, depth + 1, 0, 0);
            if (r < 0) return -1;
            if (depth > 0) return 1;                     /* (from a batch: a chain, it does not come back) */
            break;
        }
        }
        return 0;
    }
    if (client == 3) {                                   /* render: PIPE_CONTROL and the rest */
        *len = (h & 0xFF) + 2;
        /* single dwords: 3DSTATE_VF_STATISTICS (3,1,0,0Bh), PIPELINE_SELECT (3,1,1,04h) */
        if ((h >> 16) == 0x680B || (h >> 16) == 0x6904) *len = 1;
        if (((h >> 24) & 0x1F) == 0x1A) {                /* 3,3,2: PIPE_CONTROL */
            if (gen >= 8) {
                if (*len != 6) { printf("    model: a gen 8 PIPE_CONTROL of %u dwords\n", *len); bad_writes++; }
                else if (((c[1] >> 14) & 3) == 1) gpu_write64(c[2] | (uint64_t)c[3] << 32, c[4], ppgtt && !((c[1] >> 24) & 1));
            } else if (gen == 5) {
                if (h & (1u << 14)) gpu_write(c[1] & ~7u, c[2], 0);
            } else {
                uint32_t flags = c[1];
                if (((flags >> 14) & 3) == 1) {
                    int global = gen == 6 ? (c[2] >> 2) & 1 : (flags >> 24) & 1;
                    gpu_write(c[2] & ~7u, c[3], ppgtt && !global);
                }
            }
        }
        return 0;
    }
    if (client == 2) { *len = (h & 0xFF) + 2; return 0; }
    printf("    model: an unknown command %08x\n", h);
    bad_writes++;
    return 0;
}

static int run(uint32_t a, int ppgtt, int depth, uint32_t end, int is_ring) {
    for (int guard = 0; guard < 1000000; guard++) {
        if (is_ring && a == end) return 0;
        uint32_t* c = xlate(a, ppgtt);
        if (!c) { printf("    model: fetch from unmapped %08x\n", a); bad_writes++; return -1; }
        uint32_t len;
        int r = exec_cmd(c, &len, ppgtt, depth);
        if (r) return r > 0 ? 0 : -1;
        a += len * 4;
        if (is_ring) {
            uint32_t size = ((REG(0x203C) & 0x1FF000u) + 4096u);
            uint32_t start = REG(0x2038);
            if (a >= start + size) a = start;
            REG(0x2034) = a - start;
            REG(0x2074) = a;                             /* ACTHD */
        }
    }
    return -1;
}

/* gen 8+: a batch (64-bit addresses) until its end */
static int run8(uint64_t a, int ppgtt, int depth) {
    for (int guard = 0; guard < 1000000; guard++) {
        uint32_t* c = xlate8(a, ppgtt);
        if (!c) { printf("    model: fetch from unmapped %llx\n", (unsigned long long)a); bad_writes++; return -1; }
        uint32_t len;
        int r = exec_cmd(c, &len, ppgtt, depth);
        if (r) return r > 0 ? 0 : -1;
        a += len * 4;
    }
    return -1;
}

/* gen 8+: execlists - the context in an image (its ring, its PML4), run from
 * its HEAD to its TAIL, its state saved back; the CSB in the status page */
static uint32_t el_lrca, el_last_lrca, el_busy, csb_wp = 5;
static uint32_t* el_regs(uint32_t lrca) { return xlate8(lrca + 4096, 0); }
static void csb_write(uint32_t status) {
    uint32_t n = gen >= 11 ? 12 : 6, wi = gen >= 11 ? 0x2F : 0x1F;
    uint32_t* hws = xlate8(REG(0x2080), 0);
    if (!hws) { printf("    model: no status page for the CSB\n"); bad_writes++; return; }
    csb_wp = (csb_wp + 1) % n;
    hws[0x10 + csb_wp * 2] = status;
    hws[0x11 + csb_wp * 2] = el_lrca >> 12;
    hws[wi] = csb_wp;
}
static void el_save(uint32_t* r) {
    uint32_t* img = xlate8(el_lrca + 2 * 4096, 0);
    if (!img) return;
    img[0] = nctxreg;
    for (uint32_t i = 0; i < nctxreg; i++) { img[1 + 2 * i] = ctxreg[i]; img[2 + 2 * i] = REG(ctxreg[i]); }
    (void)r;
}
static void el_load(uint32_t lrca, int inhibit) {
    if (inhibit) { ctx_inhibits++; for (uint32_t i = 0; i < nctxreg; i++) REG(ctxreg[i]) = 0; return; }
    uint32_t* img = xlate8(lrca + 2 * 4096, 0);
    if (!img || img[0] > CTXREG_MAX) return;
    ctx_restores++;
    for (uint32_t i = 0; i < nctxreg; i++) REG(ctxreg[i]) = 0;
    for (uint32_t i = 0; i < img[0]; i++) { ctxreg_note(img[1 + 2 * i]); REG(img[1 + 2 * i]) = img[2 + 2 * i]; }
}
static void el_run(void) {
    uint32_t* r = el_regs(el_lrca);
    if (!r) { printf("    model: a context image not in the GTT (%08x)\n", el_lrca); bad_writes++; el_busy = 0; return; }
    /* the ring context: its registers as the image's LRI lists name them */
    if (r[1] >> 23 != 0x22 || r[2] != 0x2244 || r[4] != 0x2034 || r[8] != 0x2038 || r[0x32] != 0x2270) {
        printf("    model: a context image without its register lists\n"); bad_writes++; el_busy = 0; return;
    }
    uint32_t ctl = r[3];
    int inhibit = ((ctl >> 16) & 1) && (ctl & 1);
    if (!(r[0x0B] & 1)) { printf("    model: the context's ring is not valid\n"); bad_writes++; }
    if (!(r[0x11] & (1u << 5))) { printf("    model: the context's batches are not in its PPGTT\n"); bad_writes++; }
    if (el_lrca != el_last_lrca) { ctx_switches++; el_load(el_lrca, inhibit); }
    else if (inhibit) el_load(el_lrca, 1);
    el_last_lrca = el_lrca;
    cur_pml4 = (uint64_t)r[0x31] << 32 | r[0x33];
    uint32_t start = r[9], head = r[5] & 0x1FFFFC, tail = r[7] & 0x1FFFF8, size = (r[0x0B] & 0x1FF000u) + 4096u;
    if (head != tail) {
        uint32_t a = start + head;
        for (int guard = 0; guard < 100000 && a != start + tail; guard++) {
            uint32_t* c = xlate8(a, 0);
            if (!c) { printf("    model: ring fetch from unmapped %08x\n", a); bad_writes++; break; }
            uint32_t len;
            int rr = exec_cmd(c, &len, 0, 0);
            if (rr < 0) { REG(0x2074) = a; return; }        /* (stuck: the engine stays busy) */
            a += len * 4;
            if (a >= start + size) a = start;
            REG(0x2074) = a;
        }
        head = a - start;
    }
    r[5] = head;
    el_save(r);
    el_busy = 0;
    csb_write((1u << 4) | (1u << 3));                    /* COMPLETE | ACTIVE_IDLE */
}

static void pump(void) {
    if (pumping) return;
    pumping = 1;
    if (gen >= 8) {
        /* forcewake: the render and GT domains' acks follow */
        static uint32_t fw_r, fw_g;
        uint32_t v = REG(0xA278); if (v >> 16) { fw_r = (fw_r & ~(v >> 16)) | (v & (v >> 16)); REG(0xA278) = 0; }
        v = REG(0xA188); if (v >> 16) { fw_g = (fw_g & ~(v >> 16)) | (v & (v >> 16)); REG(0xA188) = 0; }
        REG(0x0D84) = fw_r & 1; REG(0x130044) = fw_g & 1;
        /* masked registers settle */
        uint32_t mi = REG(0x209C);
        if (mi >> 16) { static uint32_t mode; mode = (mode & ~(mi >> 16)) | (mi & (mi >> 16)); REG(0x209C) = (mode & 0xFFFF) | ((mode & (1u << 8)) ? 1u << 9 : 0); }
        static uint32_t ring_mode;
        uint32_t rm = REG(0x229C); if (rm >> 16) { ring_mode = (ring_mode & ~(rm >> 16)) | (rm & (rm >> 16)); REG(0x229C) = ring_mode; }
        uint32_t cp = REG(0x23A0);
        if (cp >> 16) { if ((cp >> 16) & 0xF) csb_wp = cp & 0xF; REG(0x23A0) = csb_wp; }
        uint32_t rc = REG(0x20D0);
        if (rc >> 16) { static uint32_t rst; rst = (rst & ~(rc >> 16)) | (rc & (rc >> 16)); REG(0x20D0) = (rst & 1) ? 3u : 0u; }
        if (REG(0x941C) & 1) {                           /* the GT reset */
            REG(0x941C) = 0; hung = 0; g_resets_seen++; el_busy = 0; el_last_lrca = 0; ring_mode = 0; REG(0x229C) = 0;
            for (uint32_t i = 0; i < nctxreg; i++) REG(ctxreg[i]) = 0;
        }
        REG(0x2358) = now_ms * 12500u;
        int el_on = gen >= 11 ? (ring_mode & (1u << 3)) : (ring_mode & (1u << 15));
        /* a submission: the ELSP's last write (element 0's low dword: the image), or the ELSQ loaded */
        uint32_t d = 0;
        if (gen >= 11) { if (REG(0x2550) & 1) { d = REG(0x2510); REG(0x2550) = 0; } }
        else if (REG(0x2230)) { d = REG(0x2230); REG(0x2230) = 0; }
        if (d) {
            if (!el_on) { printf("    model: a submission with execlists off\n"); bad_writes++; }
            else if (el_busy) { printf("    model: a submission while the engine is busy\n"); bad_writes++; }
            else if ((d & 0x19) != 0x19) { printf("    model: a descriptor %08x not valid / 48-bit\n", d); bad_writes++; }
            else { el_lrca = d & ~0xFFFu; el_busy = 1; csb_write(1u << 0); }   /* IDLE_ACTIVE */
        }
        if (el_busy && !hung && el_on && !(REG(0x209C) & (1u << 8))) el_run();
        pumping = 0;
        return;
    }
    /* forcewake: the acks follow */
    if (gen >= 6) {
        REG(0x130090) = REG(0xA18C) & 1;
        uint32_t mt = REG(0xA188);
        static uint32_t mt_state;
        if (mt >> 16) { mt_state = (mt_state & ~(mt >> 16)) | (mt & (mt >> 16)); REG(0xA188) = 0; }
        REG(hsw ? 0x130044 : 0x130040) = mt_state & 1;
        REG(0xA180) = gen == 7 && !hsw && (mt_state & 1) ? 1u << 5 : 0;  /* ECOBUS: MT forcewake set up */
        REG(0x13805C) = 0;
    }
    /* masked registers settle: STOP_RING -> idle; sync flush done */
    uint32_t mi = REG(0x209C);
    if (mi >> 16) { static uint32_t mode; mode = (mode & ~(mi >> 16)) | (mi & (mi >> 16)); REG(0x209C) = (mode & 0xFFFF) | ((mode & (1u << 8)) ? 1u << 9 : 0); }
    uint32_t ip = REG(0x20C0);
    if (ip >> 16) REG(0x20C0) = ip & 0xFFFF & ~(1u << 5);
    /* the GT reset */
    if (gen >= 6 && (REG(0x941C) & 1)) {
        REG(0x941C) = 0; hung = 0; g_resets_seen++; ctx_cur = 0;
        REG(0x2034) = 0; REG(0x2030) = 0; REG(0x203C) = 0; REG(0x2038) = 0;
    }
    if (gen == 5 && (REG(0x12CA4) & 1)) {
        REG(0x12CA4) &= ~1u;
        if (((REG(0x12CA4) >> 1) & 3) == 1) { hung = 0; g_resets_seen++; REG(0x2034) = 0; REG(0x2030) = 0; REG(0x203C) = 0; }
    }
    REG(0x2358) = now_ms * 12500u;                       /* the engine's clock: 12.5 MHz */
    /* the ring: HEAD runs to TAIL */
    if ((REG(0x203C) & 1) && !hung && !(REG(0x209C) & (1u << 8))) {
        uint32_t start = REG(0x2038), head = REG(0x2034) & 0x1FFFFC, tail = REG(0x2030) & 0x1FFFF8;
        if (head != tail) run(start + head, 0, 0, start + tail, 1);
    }
    pumping = 0;
}

uint32_t timer_ms(void) { pump(); return now_ms++; }
uint64_t timer_counter(void) { return (uint64_t)now_ms * 1000000ull; }
void task_sleep_ms(uint32_t ms) { now_ms += ms ? ms : 1; pump(); }
void task_yield(void) { pump(); }

/* the settings: gpu3d on */
static int g_on = 1;                    /* 1 on, 0 off, -1 never set */
int  settings_get(const char* k, char* out, int cap) { (void)k; if (cap < 4 || g_on < 0) return 0; memcpy(out, g_on ? "on" : "off", g_on ? 3 : 4); return 1; }
void settings_set(const char* k, const char* v) { (void)k; g_on = v[1] == 110; }
int  strcmp(const char* a, const char* b) { while (*a && *a == *b) a++, b++; return (unsigned char)*a - (unsigned char)*b; }

