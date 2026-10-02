#include "arena.h"
#include "kheap.h"
#include "kstring.h"

#define CHUNK_MIN (64u * 1024u)

/* handed out when the heap is exhausted, so callers never get NULL; the
 * oom flag tells the page/script to stop */
static uint8_t g_scratch[4096] __attribute__((aligned(16)));
/* ...or, for bigger requests, this one (grown as needed, never freed): the
 * contents are garbage shared by everyone, but it is valid memory */
static uint8_t* g_spill;
static uint32_t g_spill_size;

static void* fallback(uint32_t n) {
    if (n <= sizeof(g_scratch)) { memset(g_scratch, 0, n); return g_scratch; }
    if (n > g_spill_size) {
        uint8_t* b = (uint8_t*)kmalloc(n);
        if (b) { if (g_spill) kfree(g_spill); g_spill = b; g_spill_size = n; }
    }
    if (g_spill && n <= g_spill_size) { memset(g_spill, 0, n); return g_spill; }
    memset(g_scratch, 0, sizeof(g_scratch));
    return g_scratch;                    /* nothing else left: callers stop on oom */
}

void arena_init(arena_t* a, uint32_t limit) {
    a->head = NULL;
    a->total = 0;
    a->limit = limit;
    a->oom = 0;
}

void* arena_alloc(arena_t* a, uint32_t n) {
    n = (n + 15u) & ~15u;
    if (n == 0) n = 16;
    arena_chunk_t* c = a->head;
    if (!c || c->cap - c->used < n) {
        uint32_t cap = n > CHUNK_MIN ? n : CHUNK_MIN;
        /* the limit is soft: past it the owner is told (oom: scripts stop, loading
         * stops) but still gets real memory, up to half as much again */
        if (a->limit && a->total + cap > a->limit) a->oom = 1;
        int hard = a->limit && a->total + cap > a->limit + a->limit / 2;
        c = hard ? NULL : (arena_chunk_t*)kmalloc(sizeof(arena_chunk_t) + cap + 16);
        if (!c) {
            a->oom = 1;
            return fallback(n);
        }
        c->cap = cap;
        c->used = (uint32_t)((16u - ((uintptr_t)c->data & 15u)) & 15u);   /* align data */
        c->next = a->head;
        a->head = c;
        a->total += cap;
    }
    void* p = c->data + c->used;
    c->used += n;
    memset(p, 0, n);
    return p;
}

char* arena_strdup(arena_t* a, const char* s, uint32_t n) {
    char* d = (char*)arena_alloc(a, n + 1);
    /* the fallback buffer may be smaller than asked for: then keep it short */
    if (d == (char*)g_scratch && n >= sizeof(g_scratch)) n = sizeof(g_scratch) - 1;
    if (n) memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

void arena_free_all(arena_t* a) {
    arena_chunk_t* c = a->head;
    while (c) {
        arena_chunk_t* n = c->next;
        kfree(c);
        c = n;
    }
    a->head = NULL;
    a->total = 0;
    a->oom = 0;
}
