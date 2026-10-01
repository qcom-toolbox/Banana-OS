#ifndef ARENA_H
#define ARENA_H

#include "types.h"

/*
 * Region allocator: everything a web page (DOM, styles, layout, script
 * values) or a PHP request needs is carved out of one arena and released
 * at once when the page is left. Allocations are zeroed and 16-aligned.
 */

typedef struct arena_chunk {
    struct arena_chunk* next;
    uint32_t used, cap;
    uint8_t  data[];
} arena_chunk_t;

typedef struct arena {
    arena_chunk_t* head;
    uint32_t total;          /* bytes in all chunks */
    uint32_t limit;          /* 0 = none */
    int      oom;            /* an allocation failed (callers check it) */
} arena_t;

void  arena_init(arena_t* a, uint32_t limit);
void* arena_alloc(arena_t* a, uint32_t n);
char* arena_strdup(arena_t* a, const char* s, uint32_t n);
void  arena_free_all(arena_t* a);

#endif
