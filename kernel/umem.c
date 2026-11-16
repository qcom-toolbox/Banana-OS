/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "umem.h"
#include "fs.h"
#include "kheap.h"
#include "kstring.h"
#include "serial.h"
#include "hmem.h"
#include "spinlock.h"
#include "smp.h"

/*
 * Three sizes of allocation:
 *  - small (up to 2 KiB): slabs - a page (or 4) cut into equal pieces of
 *    one size class, a bitmap of the free ones;
 *  - up to 2 MiB: a run of whole 4 KiB pages inside one block;
 *  - bigger: blocks of their own, in a row. Freed, they become ordinary
 *    blocks the app's other allocations can use.
 * Blocks come from RAM above 4 GiB first (kernel/hmem.c, 64-bit), then
 * from the kernel heap.
 * Every block, run and slab is described in kernel memory; g_dir maps a
 * 2 MiB frame of physical memory to the block that holds it (and to its
 * app: umem_owns checks a system call's pointers against it).
 */

#define BLOCK  0x200000u
#define PAGE   4096u
#define PAGES  (BLOCK / PAGE)           /* 512 */
#ifdef __x86_64__
#define FRAMES (512u * 512u)            /* 2 MiB frames: the first 512 GiB (hmem.c) */
#else
#define FRAMES 2048u                    /* ... the first 4 GiB */
#endif
#define SMALL  2048u
#define LIMIT  ((uint64_t)FRAMES * BLOCK)   /* (more than a 32-bit uintptr_t holds) */

static const uint16_t g_cls[] = { 16, 32, 48, 64, 80, 96, 128, 160, 192, 256, 320, 384, 512, 768, 1024, 1536, 2048 };
#define NCLS ((int)(sizeof(g_cls) / sizeof(g_cls[0])))

typedef struct uslab {
    uintptr_t     base;
    uint16_t      cls, nobj, nfree, pages;
    uint64_t      map[4];               /* 1 = free (up to 256 pieces) */
    int           listed;               /* in its class's list of slabs with room */
    struct uslab* next;                 /* ... that list */
    struct uslab* all_next;             /* every slab of the app */
} uslab_t;

typedef struct uchunk {
    umem_t*        m;
    uintptr_t      base;
    void*          alloc;               /* what kfree takes (the first block of a row from the heap) */
    uint32_t       high;                /* frames of RAM above 4 GiB to give back (hmem_free) */
    uint32_t       nframes;             /* > 1: one big allocation */
    int            big_used;
    int            broken;              /* its tables could not be made: never handed out */
    int            pooled;
    int            pinned;              /* (a big allocation) the system's: see umem_alloc_pages */
    uint64_t       pin[PAGES / 64];     /* runs starting at these pages are the system's */
    uint32_t       free_pages;
    uint64_t       used[PAGES / 64];    /* pages in use */
    uint16_t       run[PAGES];          /* pages of the run starting at this page (0: none) */
    uslab_t*       slab[PAGES];         /* the slab a page belongs to */
    struct uchunk* next;
} uchunk_t;

struct umem {
    spinlock_t lock;                    /* its threads allocate on any core (kernel/app.c) */
    uspace_t* us;
    uchunk_t* chunks;
    uslab_t*  partial[NCLS];
    uslab_t*  slabs;
    uint32_t  used;
    uint32_t  limit, reserved;           /* physical blocks; zero limit: unrestricted */
};

/* Restricted processes share a finite pool, leaving at least half of the
 * currently free kernel heap for the system and ordinary applications. File
 * data cached from the disk counts as free: the heap evicts it when an
 * allocation finds no room (fs.c's reclaim). (An installed system that had
 * just read 200 MiB of files left the second browser renderer no pool.) */
static spinlock_t g_pool_lock = SPINLOCK_INIT;
static uint32_t g_pool_limit, g_pool_used;
static int pool_take(uint32_t bytes) {
    uintptr_t flags=spin_lock_irq(&g_pool_lock);
    if(!g_pool_limit){
        uint64_t free=(uint64_t)(kheap_total_bytes()-kheap_used_bytes())+fs_evictable_bytes();
        g_pool_limit=(uint32_t)(free/2u);
        if(g_pool_limit>(1024u<<20))g_pool_limit=1024u<<20;
    }
    int ok=bytes<=g_pool_limit-g_pool_used;
    if(ok)g_pool_used+=bytes;
    spin_unlock_irq(&g_pool_lock,flags);return ok;
}
/* Snapshot once before the first worker image is loaded; its code is itself
 * charged to this pool. Initializing afterwards would count that image twice
 * when deriving the available budget and penalize larger optional engines. */
void umem_web_pool_init(void) { (void)pool_take(0); }
static void pool_give(uint32_t bytes) {
    uintptr_t flags=spin_lock_irq(&g_pool_lock);
    g_pool_used-=bytes;
    spin_unlock_irq(&g_pool_lock,flags);
}

/* frame -> its block: a directory of 4 KiB tables (512 frames, 1 GiB each)
 * made when a gigabyte first has a block */
static uchunk_t** g_dir[FRAMES / 512];
static spinlock_t  g_dir_lock = SPINLOCK_INIT;   /* (two apps may add a gigabyte's table at once) */

static uchunk_t* frame_get(uintptr_t f) {
    uchunk_t** t = g_dir[f / 512];
    return t ? t[f % 512] : NULL;
}

static int frame_set(uintptr_t f, uchunk_t* c) {
    uchunk_t** t = g_dir[f / 512];
    if (!t) {
        if (!c) return 0;
        uchunk_t** nt = (uchunk_t**)kzalloc(512 * sizeof(uchunk_t*));
        if (!nt) return -1;
        uintptr_t fl = spin_lock_irq(&g_dir_lock);
        if (!g_dir[f / 512]) { g_dir[f / 512] = nt; nt = NULL; }
        t = g_dir[f / 512];
        spin_unlock_irq(&g_dir_lock, fl);
        if (nt) kfree(nt);                  /* (another core made it meanwhile) */
    }
    t[f % 512] = c;
    return 0;
}

/* the memory of a block back to where it came from */
static void chunk_release(uchunk_t* c) {
    if(c->pooled)pool_give(c->nframes*BLOCK);
    if (c->alloc) kfree(c->alloc);
    if (c->high) hmem_free(c->base, c->high);
}

umem_t* umem_create(uspace_t* us) {
    umem_t* m = (umem_t*)kzalloc(sizeof(umem_t));
    if (m) m->us = us;
    return m;
}

static uchunk_t* chunk_of(umem_t* m, const void* p) {
    uintptr_t a = (uintptr_t)p;
    if (a / BLOCK >= FRAMES) return NULL;
    uchunk_t* c = frame_get(a / BLOCK);
    return c && c->m == m ? c : NULL;
}

static int link_chunk(umem_t* m, uchunk_t* c) {
    for (uint32_t k = 0; k < c->nframes; k++)
        if (frame_set(c->base / BLOCK + k, c) < 0) {
            while (k--) frame_set(c->base / BLOCK + k, NULL);
            return -1;
        }
    c->next = m->chunks;
    m->chunks = c;
    return 0;
}

/* n blocks in a row, zeroed and opened to the app */
static uchunk_t* new_chunk(umem_t* m, uint32_t n) {
    if (!n || (m->limit && ((uint64_t)m->reserved + (uint64_t)n * BLOCK > m->limit))) return NULL;
    if(m->limit&&!pool_take(n*BLOCK))return NULL;
    uchunk_t* c = (uchunk_t*)kzalloc(sizeof(uchunk_t));
    if (!c) { if(m->limit)pool_give(n*BLOCK);return NULL; }
    c->pooled=m->limit!=0;c->nframes=n;
    uint8_t* mem = NULL;
#ifdef __x86_64__
    mem = (uint8_t*)(uintptr_t)hmem_alloc(n);           /* above 4 GiB first */
    c->high = mem ? n : 0;
#endif
    if (!mem) {
        mem = (uint8_t*)kmalloc_aligned((size_t)n * BLOCK, BLOCK);
        if (!mem) { chunk_release(c);kfree(c);return NULL; }
        c->alloc = mem;
    }
    c->m = m;
    c->base = (uintptr_t)mem;
    c->nframes = n;
    c->free_pages = n == 1 ? PAGES : 0;
    if ((uint64_t)c->base + (uint64_t)n * BLOCK > LIMIT || link_chunk(m, c) < 0) {
        chunk_release(c);
        kfree(c);
        return NULL;
    }
    m->reserved += n * BLOCK;
    memset(mem, 0, (size_t)n * BLOCK);        /* nothing of the kernel's (or another app's) left in it */
    /* (if the tables cannot be made, the block is still the app's - given
     * back when it exits - but never handed out) */
    if (m->us && uspace_map(m->us, (uintptr_t)mem, n * BLOCK) < 0) {     /* (none: the kernel's own scratch memory) */
        c->free_pages = 0;
        c->broken = 1;
        return NULL;
    }
    return c;
}

static int page_used(const uchunk_t* c, uint32_t i) { return (int)((c->used[i >> 6] >> (i & 63)) & 1); }

static void mark(uchunk_t* c, uint32_t i, uint32_t n, int used) {
    for (uint32_t k = i; k < i + n; k++) {
        if (used) c->used[k >> 6] |= 1ull << (k & 63);
        else c->used[k >> 6] &= ~(1ull << (k & 63));
    }
}

static int find_run(const uchunk_t* c, uint32_t n) {
    uint32_t len = 0;
    for (uint32_t i = 0; i < PAGES; i++) {
        if (page_used(c, i)) { len = 0; continue; }
        if (++len == n) return (int)(i + 1 - n);
    }
    return -1;
}

static void* take_run(uchunk_t* c, uint32_t i, uint32_t n) {
    mark(c, i, n, 1);
    c->run[i] = (uint16_t)n;
    c->free_pages -= n;
    return (void*)(c->base + (uintptr_t)i * PAGE);
}

/* a run's pages back to its block (no accounting) */
static void run_free(uchunk_t* c, uint32_t pi) {
    uint32_t n = c->run[pi];
    mark(c, pi, n, 0);
    c->run[pi] = 0;
    c->free_pages += n;
}

/* n (<= 512) pages in a row inside one block */
static void* pages_alloc(umem_t* m, uint32_t n) {
    for (uchunk_t* c = m->chunks; c; c = c->next) {
        if (c->nframes != 1 || c->free_pages < n) continue;
        int i = find_run(c, n);
        if (i >= 0) return take_run(c, (uint32_t)i, n);
    }
    uchunk_t* c = new_chunk(m, 1);
    return c ? take_run(c, 0, n) : NULL;
}

/* a freed big allocation: its blocks become ordinary ones */
static void split_big(umem_t* m, uchunk_t* big) {
    for (uchunk_t** pp = &m->chunks; *pp; pp = &(*pp)->next)
        if (*pp == big) { *pp = big->next; break; }
    for (uint32_t k = 0; k < big->nframes; k++) {
        uchunk_t* c = (uchunk_t*)kzalloc(sizeof(uchunk_t));
        if (!c) {
            /* (no memory for the bookkeeping: the rest stays one used block) */
            big->base += (uintptr_t)k * BLOCK;
            big->alloc = k ? NULL : big->alloc;
            big->nframes -= k;
            big->high = big->high ? big->nframes : 0;
            big->big_used = 1;
            if (link_chunk(m, big) < 0) { chunk_release(big); kfree(big); }   /* (cannot fail: its tables exist) */
            return;
        }
        c->m = m;
        c->base = big->base + (uintptr_t)k * BLOCK;
        c->alloc = k ? NULL : big->alloc;
        c->high = big->high ? 1 : 0;        /* (above 4 GiB each frame goes back by itself) */
        c->nframes = 1;
        c->free_pages = PAGES;
        link_chunk(m, c);                   /* (cannot fail: the frames' tables exist) */
    }
    kfree(big);
}

static int cls_of(size_t size) {
    for (int i = 0; i < NCLS; i++) if (size <= g_cls[i]) return i;
    return -1;
}

static void slab_unlist(umem_t* m, uslab_t* s) {
    if (!s->listed) return;
    for (uslab_t** pp = &m->partial[s->cls]; *pp; pp = &(*pp)->next)
        if (*pp == s) { *pp = s->next; break; }
    s->listed = 0;
}

static void* small_alloc(umem_t* m, int ci) {
    uslab_t* s = m->partial[ci];
    if (!s) {
        uint16_t pages = g_cls[ci] <= 256 ? 1 : 4;
        uint8_t* base = (uint8_t*)pages_alloc(m, pages);
        if (!base) return NULL;
        s = (uslab_t*)kzalloc(sizeof(uslab_t));
        if (!s) { uchunk_t* c = chunk_of(m, base); run_free(c, (uint32_t)(((uintptr_t)base - c->base) / PAGE)); return NULL; }
        s->base = (uintptr_t)base;
        s->cls = (uint16_t)ci;
        s->pages = pages;
        s->nobj = (uint16_t)(pages * PAGE / g_cls[ci]);
        s->nfree = s->nobj;
        for (uint32_t k = 0; k < s->nobj; k++) s->map[k >> 6] |= 1ull << (k & 63);
        uchunk_t* c = chunk_of(m, base);
        uint32_t pi = (uint32_t)((s->base - c->base) / PAGE);
        for (uint32_t k = 0; k < pages; k++) c->slab[pi + k] = s;
        s->all_next = m->slabs;
        m->slabs = s;
        s->next = NULL;
        s->listed = 1;
        m->partial[ci] = s;
    }
    for (int w = 0; w < 4; w++) {
        if (!s->map[w]) continue;
        int b = __builtin_ctzll(s->map[w]);
        s->map[w] &= ~(1ull << b);
        if (--s->nfree == 0) slab_unlist(m, s);
        m->used += g_cls[ci];
        return (void*)(s->base + (uintptr_t)(w * 64 + b) * g_cls[ci]);
    }
    return NULL;                                /* (cannot happen: a listed slab has room) */
}

/* (a bad free is logged on the boot core only: the log is not for other cores) */
#define BAD_FREE(msg, p) do { if (cpu_id() == 0) klog(msg, p); } while (0)

static void* alloc_l(umem_t* m, size_t size) {
    if (size > (1ul << 30)) return NULL;
    if (!size) size = 1;
    if (size <= SMALL) return small_alloc(m, cls_of(size));
    if (size <= BLOCK) {
        uint32_t n = (uint32_t)((size + PAGE - 1) / PAGE);
        void* p = pages_alloc(m, n);
        if (p) m->used += n * PAGE;
        return p;
    }
    uint32_t n = (uint32_t)((size + BLOCK - 1) / BLOCK);
    uchunk_t* c = new_chunk(m, n);
    if (!c) return NULL;
    c->big_used = 1;
    m->used += n * BLOCK;
    return (void*)c->base;
}

static size_t size_l(umem_t* m, const void* p) {
    uchunk_t* c = chunk_of(m, p);
    if (!c) return 0;
    uintptr_t a = (uintptr_t)p;
    if (c->broken) return 0;
    if (c->nframes > 1) return a == c->base && c->big_used ? (size_t)c->nframes * BLOCK : 0;
    uint32_t pi = (uint32_t)((a - c->base) / PAGE);
    uslab_t* s = c->slab[pi];
    if (s) {
        uintptr_t off = a - s->base;
        if (off % g_cls[s->cls]) return 0;
        uint32_t k = (uint32_t)(off / g_cls[s->cls]);
        if (k >= s->nobj || ((s->map[k >> 6] >> (k & 63)) & 1)) return 0;   /* free */
        return g_cls[s->cls];
    }
    if ((a & (PAGE - 1)) || !c->run[pi]) return 0;
    return (size_t)c->run[pi] * PAGE;
}

static void free_l(umem_t* m, void* p) {
    uchunk_t* c = chunk_of(m, p);
    uintptr_t a = (uintptr_t)p;
    if (!c) { BAD_FREE("app: free() of a pointer that is not its memory (%p)\n", p); return; }
    if (c->broken) return;
    if (c->nframes > 1) {
        if (a != c->base || !c->big_used) { BAD_FREE("app: free() of a bad pointer (%p)\n", p); return; }
        m->used -= c->nframes * BLOCK;
        c->big_used = 0;
        split_big(m, c);
        return;
    }
    uint32_t pi = (uint32_t)((a - c->base) / PAGE);
    uslab_t* s = c->slab[pi];
    if (s) {
        uintptr_t off = a - s->base;
        uint32_t k = (uint32_t)(off / g_cls[s->cls]);
        if (off % g_cls[s->cls] || k >= s->nobj || ((s->map[k >> 6] >> (k & 63)) & 1)) {
            BAD_FREE("app: free() of a bad pointer (%p)\n", p);
            return;
        }
        s->map[k >> 6] |= 1ull << (k & 63);
        s->nfree++;
        m->used -= g_cls[s->cls];
        if (s->nfree == s->nobj && (m->partial[s->cls] != s || s->next)) {
            /* empty, and its class has another slab with room: the pages go back */
            slab_unlist(m, s);
            for (uslab_t** pp = &m->slabs; *pp; pp = &(*pp)->all_next)
                if (*pp == s) { *pp = s->all_next; break; }
            uint32_t first = (uint32_t)((s->base - c->base) / PAGE);
            for (uint32_t i = 0; i < s->pages; i++) c->slab[first + i] = NULL;
            run_free(c, first);
            kfree(s);
        } else if (!s->listed) {
            s->listed = 1;
            s->next = m->partial[s->cls];
            m->partial[s->cls] = s;
        }
        return;
    }
    if ((a & (PAGE - 1)) || !c->run[pi]) { BAD_FREE("app: free() of a bad pointer (%p)\n", p); return; }
    m->used -= c->run[pi] * PAGE;
    run_free(c, pi);
}

static void* realloc_l(umem_t* m, void* p, size_t size) {
    if (!p) return alloc_l(m, size);
    if (!size) { free_l(m, p); return NULL; }
    size_t old = size_l(m, p);
    if (!old) return NULL;
    if (size <= old && (old <= SMALL || size > old / 2)) return p;
    void* n = alloc_l(m, size);
    if (!n) return NULL;
    memcpy(n, p, old < size ? old : size);
    free_l(m, p);
    return n;
}

/* The system's blocks in the app's memory - its code, stacks (with guard
 * pages), call table - come from umem_alloc_pages and are pinned: free()
 * and realloc() from the app leave them alone. (Its own code and stack are
 * not its to free; and a copy over a guard page would fault while the lock
 * is held, leaving it held for good.) */
static int pinned(umem_t* m, const void* p) {
    uchunk_t* c = chunk_of(m, p);
    if (!c) return 0;
    if (c->nframes > 1) return c->pinned;
    uint32_t pi = (uint32_t)(((uintptr_t)p - c->base) / PAGE);
    return (int)((c->pin[pi >> 6] >> (pi & 63)) & 1);
}

static void set_pin(umem_t* m, const void* p, int on) {
    uchunk_t* c = chunk_of(m, p);
    if (!c) return;
    if (c->nframes > 1) { c->pinned = on; return; }
    uint32_t pi = (uint32_t)(((uintptr_t)p - c->base) / PAGE);
    if (on) c->pin[pi >> 6] |= 1ull << (pi & 63);
    else c->pin[pi >> 6] &= ~(1ull << (pi & 63));
}

/* ── what the rest of the kernel calls: each holds the app's lock ── */

void* umem_alloc(umem_t* m, size_t size) {
    if (!m) return NULL;
    spin_lock(&m->lock);
    void* p = alloc_l(m, size);
    spin_unlock(&m->lock);
    return p;
}

void umem_free(umem_t* m, void* p) {
    if (!m || !p) return;
    spin_lock(&m->lock);
    if (!pinned(m, p)) free_l(m, p);
    spin_unlock(&m->lock);
}

void* umem_realloc(umem_t* m, void* p, size_t size) {
    if (!m) return NULL;
    spin_lock(&m->lock);
    void* n = p && pinned(m, p) ? NULL : realloc_l(m, p, size);
    spin_unlock(&m->lock);
    return n;
}

size_t umem_size(umem_t* m, const void* p) {
    if (!m) return 0;
    spin_lock(&m->lock);
    size_t n = pinned(m, p) ? 0 : size_l(m, p);
    spin_unlock(&m->lock);
    return n;
}

size_t umem_pages_size(umem_t* m, const void* p) {
    if (!m) return 0;
    spin_lock(&m->lock);
    size_t n = pinned(m, p) ? size_l(m, p) : 0;
    spin_unlock(&m->lock);
    return n;
}

void* umem_alloc_pages(umem_t* m, size_t size, size_t guard) {
    if (!m || (guard & (PAGE - 1))) return NULL;
    size_t total = (size + guard + PAGE - 1) & ~(size_t)(PAGE - 1);
    spin_lock(&m->lock);
    uint8_t* p = (uint8_t*)alloc_l(m, total < SMALL + 1 ? SMALL + 1 : total);   /* (whole pages) */
    if (p) set_pin(m, p, 1);
    if (p && guard) uspace_guard(m->us, (uintptr_t)p, (uint32_t)guard, 1);
    spin_unlock(&m->lock);
    return p ? p + guard : NULL;
}

void umem_free_pages(umem_t* m, void* p, size_t guard) {
    if (!m || !p) return;
    uint8_t* base = (uint8_t*)p - guard;
    spin_lock(&m->lock);
    if (guard) uspace_guard(m->us, (uintptr_t)base, (uint32_t)guard, 0);
    set_pin(m, base, 0);
    free_l(m, base);
    spin_unlock(&m->lock);
}

/* Charge anonymous mmap pages to the same restricted budget as loader and
 * direct allocations. Swapping does not release this commitment. */
int umem_vm_charge(umem_t* m,uint32_t bytes) {
    if(!m||!m->limit)return 1;
    if(bytes>m->limit-m->reserved||!pool_take(bytes))return 0;
    m->reserved+=bytes;return 1;
}
void umem_vm_uncharge(umem_t* m,uint32_t bytes) {
    if(!m||!m->limit||!bytes)return;
    m->reserved-=bytes;pool_give(bytes);
}

uint32_t umem_committed(umem_t* m) { return m ? __atomic_load_n(&m->reserved,__ATOMIC_RELAXED) : 0; }

uint32_t umem_used(umem_t* m) { return m ? m->used : 0; }

int umem_owns(umem_t* m, const void* p, size_t n) {
    uintptr_t a = (uintptr_t)p;
    if (!m || !n) n = 1;
    if (!m || (uint64_t)a + n > LIMIT) return 0;
    for (uintptr_t f = a / BLOCK; f <= (uintptr_t)(((uint64_t)a + n - 1) / BLOCK); f++)
        if (!frame_get(f) || frame_get(f)->m != m) return 0;
    return 1;
}

int umem_owns_str(umem_t* m, const char* s) {
    uintptr_t a = (uintptr_t)s;
    for (;;) {
        if (!m || a / BLOCK >= FRAMES) return 0;
        uintptr_t f = a / BLOCK;
        if (!frame_get(f) || frame_get(f)->m != m) return 0;
        uint64_t end = (uint64_t)(f + 1) * BLOCK;
        for (; (uint64_t)a < end; a++) {
            if (!*(const volatile char*)a) return 1;
            if (a == (uintptr_t)-1) return 0;
        }
    }
}

void umem_destroy(umem_t* m) {
    if (!m) return;
    while (m->slabs) {
        uslab_t* s = m->slabs;
        m->slabs = s->all_next;
        kfree(s);
    }
    /* (the first block of a row holds the allocation of the whole row) */
    while (m->chunks) {
        uchunk_t* c = m->chunks;
        m->chunks = c->next;
        for (uint32_t k = 0; k < c->nframes; k++) frame_set(c->base / BLOCK + k, NULL);
        chunk_release(c);
        kfree(c);
    }
    kfree(m);
}

/* Set by the kernel before starting a restricted process. Existing loader
 * allocations are charged too; only process teardown releases physical blocks. */
int umem_limit(umem_t* m, uint32_t bytes) {
    if(!m||m->limit||!bytes||m->reserved>bytes||!pool_take(m->reserved))return -1;
    m->limit=bytes;
    for(uchunk_t* c=m->chunks;c;c=c->next)c->pooled=1;
    return 0;
}
