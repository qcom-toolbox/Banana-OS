/* SPDX-License-Identifier: BSD-2-Clause */
/* Banana OS SDK: malloc, free, realloc, calloc and aligned allocation.
 *
 * 64-bit: the app's own heap. It used to be a system call for every
 * malloc and every free (the kernel's allocator, behind its lock); now the
 * memory comes from the system in big pieces (vm_map: addresses reserved,
 * pages given on first touch, compressed away when idle) and blocks are
 * handed out here:
 *  - small blocks (up to 32 KiB): 40 size classes, cut from 64 KiB spans in
 *    1 GiB regions. Each span's bookkeeping sits in its region's header, out
 *    of the way of the blocks. Every thread keeps a few free blocks of each
 *    class (its cache, reached through the thread pointer: pthread.c), so
 *    most malloc / free pairs take no lock at all; caches are filled and
 *    emptied in batches, and given back when their thread ends.
 *  - large blocks: a mapping each; the last few freed are kept for reuse.
 *  - empty spans beyond a few are given back to the system (their pages).
 * Memory the kernel handed the app (http bodies, decoded pictures...) is
 * not the heap's: free / realloc pass it back to the kernel, told apart by
 * address (the heap is above BANANA_VM_BASE, the kernel's below).
 *
 * 32-bit, and systems without vm_map: the kernel's allocator, as before.
 */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <banana.h>
#include "banana_api.h"

extern const banana_api_t* __banana;

int __aligned_block(void* p, void** raw, size_t* size, int remove);   /* posix.c (the kernel's path) */

/* pthread.c: where the calling thread keeps its cache (NULL: threads have
 * no thread pointer - then every call takes the lock) */
void** (*__malloc_slot_fn)(void);
void __malloc_thread_end(void* cache);

#ifdef __x86_64__

#define SPAN       65536ul
#define REGION     (1ul << 30)
#define NSPANS     (REGION / SPAN)
#define MAX_REGION 64                   /* 64 GiB of small blocks */
#define SMALL_MAX  32768ul
#define NCLS       40
#define FREE_KEEP  16                   /* empty spans kept ready (1 MiB); more go back */
#define LARGE_KEEP 4                    /* freed large mappings kept for reuse */
#define LARGE_HDR  64
#define LARGE_MAGIC 0x6C61726765426Eull  /* "nBegral" */
#define REGION_MAGIC 0x6E6F696765526Eull /* "nRegion" */
#define PROT_ALL   (BANANA_PROT_READ | BANANA_PROT_WRITE | BANANA_PROT_EXEC)

typedef struct span {
    struct span* next;
    struct span* prev;
    void*        free;                  /* freed blocks, a list through them */
    uint8_t*     bump;                  /* blocks never handed out begin here */
    uint32_t     used;                  /* handed out (thread caches count as out) */
    uint32_t     cap;
    uint16_t     cls;
    uint16_t     state;                 /* S_UNUSED, S_CLASS (in use), S_FREE (empty, kept) */
    uint32_t     released;              /* (S_FREE) its pages went back to the system */
} span_t;
enum { S_UNUSED = 0, S_CLASS, S_FREE };

typedef struct {
    uint64_t magic;
    uint8_t* base;                      /* the region: base .. base + REGION */
    uint32_t next;                      /* spans never used begin here */
    span_t   spans[NSPANS];
} region_t;
#define FIRST_SPAN ((uint32_t)((sizeof(region_t) + SPAN - 1) / SPAN))

typedef struct {
    uint64_t magic;
    void*    map;                       /* the mapping it is in */
    size_t   maplen;
    size_t   cap;                       /* bytes usable from the block on */
    int      fresh;                     /* never used: still zeroed */
} large_t;

typedef struct { void* head; uint32_t n; uint32_t pad; } tbin_t;
typedef struct { tbin_t bin[NCLS]; } tcache_t;

static uint32_t       g_size[NCLS];
static span_t*        g_partial[NCLS];  /* spans of the class with room */
static span_t*        g_free;           /* empty spans */
static uint32_t       g_nfree;
static region_t*      g_region[MAX_REGION];
static int            g_nregion;
static large_t*       g_large[LARGE_KEEP];
static banana_mutex_t g_lock = BANANA_MUTEX_INIT;
static int            g_state;          /* 0 not yet, 1 the heap, -1 the kernel's */
static size_t         g_mapped, g_inuse;

static int heap(void) {
    if (g_state) return g_state > 0;
    int ok = __banana->version >= 16 && BANANA_HAS(__banana, vm_unmap);
    for (int i = 0, s = 16; ok && i < NCLS; i++) {
        if (i < 8) s = 16 * (i + 1);
        else {
            int b = 7 + (i - 8) / 4;                    /* sizes in (2^b, 2^(b+1)] in 4 steps */
            s = (1 << b) + ((i - 8) % 4 + 1) * (1 << (b - 2));
        }
        g_size[i] = (uint32_t)s;
    }
    g_state = ok ? 1 : -1;
    return ok;
}

static unsigned log2_floor(size_t v) {
    uint64_t r;
    __asm__("bsrq %1, %0" : "=r"(r) : "rm"((uint64_t)v));
    return (unsigned)r;
}

static int cls_of(size_t n) {
    if (n <= 128) return n ? (int)((n + 15) / 16) - 1 : 0;
    unsigned b = log2_floor(n - 1);                     /* n - 1 in [2^b, 2^(b+1)) */
    return 8 + (int)(b - 7) * 4 + (int)((n - 1 - ((size_t)1 << b)) >> (b - 2));
}

/* how many free blocks of a class a thread keeps */
static uint32_t tcache_max(int ci) {
    uint32_t n = 16384u / g_size[ci];
    return n < 4 ? 4 : n > 64 ? 64 : n;
}

/* ── regions and spans (under g_lock) ─────────────────────────────── */

static region_t* region_of(const void* p) {
    uintptr_t base = (uintptr_t)p & ~(REGION - 1);
    for (int i = 0; i < g_nregion; i++) if ((uintptr_t)g_region[i] == base) return g_region[i];
    return NULL;
}

static span_t* span_of(region_t* r, const void* p) {
    return &r->spans[((uintptr_t)p - (uintptr_t)r) / SPAN];
}

static uint8_t* span_mem(region_t* r, span_t* s) {
    return r->base + (size_t)(s - r->spans) * SPAN;
}

/* a new region: 2 GiB of addresses reserved, the 1 GiB-aligned half used */
static region_t* region_new(void) {
    if (g_nregion == MAX_REGION) return NULL;
    uint8_t* m = __banana->vm_map(NULL, 2 * REGION, PROT_ALL, 0);
    if (!m) return NULL;
    region_t* r = (region_t*)(((uintptr_t)m + REGION - 1) & ~(REGION - 1));
    r->magic = REGION_MAGIC;
    r->base = (uint8_t*)r;
    r->next = FIRST_SPAN;
    /* (free() looks regions up without the lock: the entry before the count) */
    g_region[g_nregion] = r;
    __asm__ volatile("" ::: "memory");
    g_nregion++;
    g_mapped += REGION;
    return r;
}

static void list_del(span_t** head, span_t* s) {
    if (s->prev) s->prev->next = s->next;
    else *head = s->next;
    if (s->next) s->next->prev = s->prev;
    s->next = s->prev = NULL;
}

static void list_add(span_t** head, span_t* s) {
    s->prev = NULL;
    s->next = *head;
    if (*head) (*head)->prev = s;
    *head = s;
}

/* a span for class ci, with room: an empty one kept, else a new one */
static span_t* span_new(int ci) {
    span_t* s = g_free;
    region_t* r = NULL;
    if (s) {
        list_del(&g_free, s);
        g_nfree--;
        r = region_of(s);
    } else {
        for (int i = 0; i < g_nregion && !s; i++)
            if (g_region[i]->next < NSPANS) { r = g_region[i]; s = &r->spans[r->next++]; }
        if (!s) {
            r = region_new();
            if (!r) return NULL;
            s = &r->spans[r->next++];
        }
    }
    s->cls = (uint16_t)ci;
    s->state = S_CLASS;
    s->free = NULL;
    s->bump = span_mem(r, s);
    s->used = 0;
    s->cap = (uint32_t)(SPAN / g_size[ci]);
    s->released = 0;
    list_add(&g_partial[ci], s);
    return s;
}

/* one block of class ci (NULL: no memory) */
static void* take(int ci) {
    span_t* s = g_partial[ci];
    if (!s && !(s = span_new(ci))) return NULL;
    void* p;
    if (s->free) {
        p = s->free;
        s->free = *(void**)p;
    } else {
        p = s->bump;
        s->bump += g_size[ci];
    }
    if (++s->used == s->cap) list_del(&g_partial[ci], s);
    g_inuse += g_size[ci];
    return p;
}

/* block p (class ci, in span s of region r) back to its span */
static void give(region_t* r, span_t* s, void* p) {
    int ci = s->cls;
    *(void**)p = s->free;
    s->free = p;
    if (s->used-- == s->cap) list_add(&g_partial[ci], s);
    g_inuse -= g_size[ci];
    if (s->used) return;
    /* empty: kept ready for any class, or its pages go back */
    list_del(&g_partial[ci], s);
    s->state = S_FREE;
    if (g_nfree >= FREE_KEEP) {
        /* (mapped again in place: the addresses stay reserved, the pages go) */
        s->released = __banana->vm_map(span_mem(r, s), SPAN, PROT_ALL, BANANA_MAP_FIXED) == span_mem(r, s);
    }
    list_add(&g_free, s);
    g_nfree++;
}

/* ── large blocks ─────────────────────────────────────────────────── */

static void* large_alloc(size_t n, size_t align) {
    if (align < 16) align = 16;
    if (n > ((size_t)1 << 46)) return NULL;
    size_t need = n + align + LARGE_HDR;
    /* a kept one that fits without wasting more than half of it */
    banana_lock(&g_lock);
    large_t* h = NULL;
    for (int i = 0; i < LARGE_KEEP && !h; i++) {
        large_t* k = g_large[i];
        if (!k || k->maplen < need || k->maplen / 2 > need) continue;
        g_large[i] = NULL;
        h = k;
    }
    banana_unlock(&g_lock);
    void* map;
    size_t maplen;
    int fresh = 0;
    if (h) {
        map = h->map;
        maplen = h->maplen;
    } else {
        maplen = (need + 4095) & ~(size_t)4095;
        map = __banana->vm_map(NULL, maplen, PROT_ALL, 0);
        if (!map) return NULL;
        fresh = 1;
    }
    uintptr_t u = ((uintptr_t)map + LARGE_HDR + align - 1) & ~(uintptr_t)(align - 1);
    h = (large_t*)(u - LARGE_HDR);
    h->magic = LARGE_MAGIC;
    h->map = map;
    h->maplen = maplen;
    h->cap = (uintptr_t)map + maplen - u;
    h->fresh = fresh;
    banana_lock(&g_lock);
    g_inuse += h->cap;
    if (fresh) g_mapped += maplen;
    banana_unlock(&g_lock);
    return (void*)u;
}

static large_t* large_of(void* p) {
    large_t* h = (large_t*)((uintptr_t)p - LARGE_HDR);
    return h->magic == LARGE_MAGIC ? h : NULL;
}

static void large_free(large_t* h) {
    banana_lock(&g_lock);
    g_inuse -= h->cap;
    h->magic = 0;
    /* kept for the next one that fits (the smallest kept one goes instead) */
    int slot = -1;
    for (int i = 0; i < LARGE_KEEP && slot < 0; i++) if (!g_large[i]) slot = i;
    large_t* out = NULL;
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < LARGE_KEEP; i++) if (g_large[i]->maplen < g_large[slot]->maplen) slot = i;
        if (g_large[slot]->maplen >= h->maplen) { out = h; slot = -1; }
        else out = g_large[slot];
    }
    if (slot >= 0) {
        large_t* k = (large_t*)h->map;          /* (kept: its record at the mapping's start) */
        void* map = h->map;
        size_t maplen = h->maplen;
        k->map = map;
        k->maplen = maplen;
        g_large[slot] = k;
    }
    if (out) g_mapped -= out->maplen;
    banana_unlock(&g_lock);
    if (out) __banana->vm_unmap(out->map, out->maplen);
}

/* ── thread caches ────────────────────────────────────────────────── */

static tcache_t* tcache(void) {
    void** slot = __malloc_slot_fn ? __malloc_slot_fn() : NULL;
    if (!slot) return NULL;
    if (!*slot) {
        banana_lock(&g_lock);
        tcache_t* t = take(cls_of(sizeof(tcache_t)));
        banana_unlock(&g_lock);
        if (!t) return NULL;
        memset(t, 0, sizeof(*t));
        *slot = t;
    }
    return (tcache_t*)*slot;
}

/* half of a full bin back to the spans */
static void bin_drain(tbin_t* b, uint32_t keep) {
    banana_lock(&g_lock);
    while (b->n > keep) {
        void* p = b->head;
        b->head = *(void**)p;
        b->n--;
        region_t* r = region_of(p);
        give(r, span_of(r, p), p);
    }
    banana_unlock(&g_lock);
}

void __malloc_thread_end(void* cache) {
    tcache_t* t = (tcache_t*)cache;
    if (!t || g_state <= 0) return;
    for (int ci = 0; ci < NCLS; ci++) if (t->bin[ci].n) bin_drain(&t->bin[ci], 0);
    banana_lock(&g_lock);
    region_t* r = region_of(t);
    if (r) give(r, span_of(r, t), t);
    banana_unlock(&g_lock);
}

static void* small_alloc(int ci) {
    tcache_t* t = tcache();
    if (t) {
        tbin_t* b = &t->bin[ci];
        if (!b->head) {                             /* refilled in one go */
            uint32_t want = tcache_max(ci) / 2;
            banana_lock(&g_lock);
            while (b->n < want) {
                void* p = take(ci);
                if (!p) break;
                *(void**)p = b->head;
                b->head = p;
                b->n++;
            }
            banana_unlock(&g_lock);
            if (!b->head) return NULL;
        }
        void* p = b->head;
        b->head = *(void**)p;
        b->n--;
        return p;
    }
    banana_lock(&g_lock);
    void* p = take(ci);
    banana_unlock(&g_lock);
    return p;
}

static void small_free(region_t* r, void* p) {
    span_t* s = span_of(r, p);
    tcache_t* t = tcache();
    if (t) {
        tbin_t* b = &t->bin[s->cls];
        *(void**)p = b->head;
        b->head = p;
        if (++b->n > tcache_max(s->cls)) bin_drain(b, tcache_max(s->cls) / 2);
        return;
    }
    banana_lock(&g_lock);
    give(r, s, p);
    banana_unlock(&g_lock);
}

/* ── what the program calls ───────────────────────────────────────── */

/* is p the kernel's (below the addresses vm_map hands out)? */
static int kernel_block(const void* p) { return (uintptr_t)p < BANANA_VM_BASE; }

static size_t usable(void* p) {
    region_t* r = region_of(p);
    if (r) return g_size[span_of(r, p)->cls];
    large_t* h = large_of(p);
    return h ? h->cap : 0;
}

void* __malloc_aligned(size_t align, size_t n) {
    if (!heap()) return NULL;                           /* (posix.c asks __malloc_heap first) */
    if (!n) n = 1;
    if (align < 16) align = 16;
    if (n <= SMALL_MAX && align <= SMALL_MAX) {
        /* a class whose size is a multiple of align: its blocks all are aligned */
        size_t want = n > align ? n : align;
        for (int ci = cls_of(want); ci < NCLS; ci++)
            if (g_size[ci] % align == 0) {
                void* p = small_alloc(ci);
                if (!p) errno = ENOMEM;
                return p;
            }
    }
    void* p = large_alloc(n, align);
    if (!p) errno = ENOMEM;
    return p;
}

void* malloc(size_t n) {
    if (heap()) {
        void* p = n <= SMALL_MAX ? small_alloc(cls_of(n)) : large_alloc(n, 16);
        if (!p) errno = ENOMEM;
        return p;
    }
    void* p = __banana->malloc(n ? n : 1);
    if (!p) errno = ENOMEM;
    return p;
}

void free(void* p) {
    if (!p) return;
    if (g_state > 0 && !kernel_block(p)) {
        region_t* r = region_of(p);
        if (r) {
            if (span_of(r, p)->state == S_CLASS) small_free(r, p);   /* (else: freed twice - left alone) */
            return;
        }
        large_t* h = large_of(p);
        if (h) large_free(h);
        return;                                         /* (not a block: left alone) */
    }
    void* raw;
    __banana->free(__aligned_block(p, &raw, NULL, 1) ? raw : p);
}

void* realloc(void* p, size_t n) {
    if (!p) return malloc(n);
    if (!n) { free(p); return NULL; }
    if (g_state > 0 && !kernel_block(p)) {
        size_t have = usable(p);
        if (!have) { errno = EINVAL; return NULL; }
        /* it fits, and is not much too big: stays */
        if (n <= have && (n > have / 2 || have <= 64)) return p;
        void* q = malloc(n);
        if (!q) return NULL;
        memcpy(q, p, have < n ? have : n);
        free(p);
        return q;
    }
    void* raw;
    size_t old;
    if (__aligned_block(p, &raw, &old, 0)) {           /* (from posix_memalign) */
        void* q = malloc(n);
        if (q) {
            memcpy(q, p, old < n ? old : n);
            __aligned_block(p, &raw, NULL, 1);
            __banana->free(raw);
        }
        return q;
    }
    return __banana->realloc(p, n);
}

void* calloc(size_t n, size_t s) {
    if (s && n > (size_t)-1 / s) { errno = ENOMEM; return NULL; }
    size_t len = n * s;
    void* p = malloc(len);
    if (!p) return NULL;
    large_t* h = g_state > 0 && len > SMALL_MAX ? large_of(p) : NULL;
    if (!h || !h->fresh) memset(p, 0, len);            /* (a new mapping is zeroed already) */
    return p;
}

size_t malloc_usable_size(void* p) {
    if (!p) return 0;
    if (g_state > 0 && !kernel_block(p)) return usable(p);
    size_t old;
    void* raw;
    return __aligned_block(p, &raw, &old, 0) ? old : 0;
}

int __malloc_heap(void) { return heap(); }

void __malloc_stats(size_t* mapped, size_t* inuse) {
    if (mapped) *mapped = g_mapped;
    if (inuse) *inuse = g_inuse;
}

#else   /* ── 32-bit: the kernel's allocator ───────────────────────── */

void* __malloc_aligned(size_t align, size_t n) { (void)align; (void)n; return NULL; }
int   __malloc_heap(void) { return 0; }
void  __malloc_thread_end(void* cache) { (void)cache; }
void  __malloc_stats(size_t* mapped, size_t* inuse) { if (mapped) *mapped = 0; if (inuse) *inuse = 0; }

void* malloc(size_t n) { void* p = __banana->malloc(n ? n : 1); if (!p) errno = ENOMEM; return p; }

void free(void* p) {
    void* raw;
    if (!p) return;
    __banana->free(__aligned_block(p, &raw, NULL, 1) ? raw : p);
}

void* realloc(void* p, size_t n) {
    void* raw;
    size_t old;
    if (p && !n) { free(p); return NULL; }
    if (p && __aligned_block(p, &raw, &old, 0)) {      /* (from posix_memalign) */
        void* q = malloc(n);
        if (q) {
            memcpy(q, p, old < n ? old : n);
            __aligned_block(p, &raw, NULL, 1);
            __banana->free(raw);
        }
        return q;
    }
    return __banana->realloc(p, n);
}

void* calloc(size_t n, size_t s) {
    if (s && n > (size_t)-1 / s) return NULL;
    void* p = malloc(n * s);
    if (p) memset(p, 0, n * s);
    return p;
}

size_t malloc_usable_size(void* p) {
    size_t old;
    void* raw;
    return p && __aligned_block(p, &raw, &old, 0) ? old : 0;
}

#endif
