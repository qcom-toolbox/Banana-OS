#include "kheap.h"
#include "kstring.h"

/* Every block (free or used) starts with this header; blocks tile the
 * whole heap in address order, so neighbours are found through prev/next
 * and merging on free is O(1). The header is 16 bytes, keeping payloads
 * 16-byte aligned. */
typedef struct block {
    uint32_t      size;   /* payload bytes (excludes header) */
    uint32_t      used;   /* 0 = free, HEAP_MAGIC = allocated */
    struct block* prev;
    struct block* next;
} block_t;

#define HEAP_MAGIC 0xB16B00B5u
#define HDR        ((uint32_t)sizeof(block_t))
#define ALIGN      16u
#define MIN_SPLIT  64u   /* don't leave slivers smaller than this */

extern char _kernel_end[];  /* boot/linker.ld */

static block_t* g_head = NULL;
static uint32_t g_total = 0;
static uint32_t g_used = 0;

typedef struct { uint32_t type, size; } __attribute__((packed)) mb2_tag_t;
typedef struct {
    uint32_t type, size, entry_size, entry_version;
} __attribute__((packed)) mb2_tag_mmap_t;
typedef struct {
    uint64_t base, length;
    uint32_t type, reserved;
} __attribute__((packed)) mb2_mmap_entry_t;

static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1u) & ~(a - 1u); }

/* End of the available RAM region containing `addr`, or 0 if none found. */
static uint32_t region_end_for(uint32_t mb2, uint32_t addr) {
    if (!mb2) return 0;
    uint32_t total = *(uint32_t*)(uintptr_t)mb2;
    uint32_t off = 8;
    while (off + 8 <= total) {
        mb2_tag_t* tag = (mb2_tag_t*)(uintptr_t)(mb2 + off);
        if (tag->type == 0) break;
        if (tag->type == 6) {
            mb2_tag_mmap_t* mm = (mb2_tag_mmap_t*)tag;
            for (uint32_t pos = sizeof(*mm); pos + mm->entry_size <= mm->size; pos += mm->entry_size) {
                mb2_mmap_entry_t* e = (mb2_mmap_entry_t*)((uintptr_t)mm + pos);
                if (e->type != 1) continue;
                uint64_t lo = e->base, hi = e->base + e->length;
                if ((uint64_t)addr >= lo && (uint64_t)addr < hi) {
                    /* cap at 3.5 GiB: stay clear of the 32-bit MMIO hole */
                    if (hi > 0xE0000000ull) hi = 0xE0000000ull;
                    return (uint32_t)hi;
                }
            }
        }
        uint32_t adv = align_up(tag->size, 8);
        off += adv < 8 ? 8 : adv;
    }
    return 0;
}

void kheap_init(uint32_t mb2_info_addr) {
    uint32_t start = align_up((uint32_t)(uintptr_t)_kernel_end, 4096u);

    /* The Multiboot2 info block is usually right after the kernel. Every
     * consumer (fb.c, sysinfo.c) has already copied what it needs by now,
     * but skip past it anyway so nothing reads freshly-clobbered tags. */
    if (mb2_info_addr >= start && mb2_info_addr < start + (1u << 20)) {
        uint32_t mb_end = mb2_info_addr + *(uint32_t*)(uintptr_t)mb2_info_addr;
        start = align_up(mb_end, 4096u);
    }

    uint32_t end = region_end_for(mb2_info_addr, start);
    if (end == 0 || end <= start + (1u << 20)) {
        /* No usable map: assume a conservative 16 MiB machine. */
        end = 16u << 20;
    }
    end &= ~(ALIGN - 1u);

    g_head = (block_t*)(uintptr_t)start;
    g_head->size = end - start - HDR;
    g_head->used = 0;
    g_head->prev = NULL;
    g_head->next = NULL;
    g_total = g_head->size;
    g_used = 0;
}

static void split(block_t* b, uint32_t size) {
    if (b->size < size + HDR + MIN_SPLIT) return;
    block_t* n = (block_t*)((uint8_t*)b + HDR + size);
    n->size = b->size - size - HDR;
    n->used = 0;
    n->prev = b;
    n->next = b->next;
    if (b->next) b->next->prev = n;
    b->next = n;
    b->size = size;
}

/* merges b with its following block if that one is free */
static void merge_next(block_t* b) {
    block_t* n = b->next;
    if (!n || n->used) return;
    b->size += HDR + n->size;
    b->next = n->next;
    if (n->next) n->next->prev = b;
}

void* kmalloc(size_t size) {
    if (!g_head || size == 0 || size > 0xF0000000u) return NULL;
    uint32_t need = align_up((uint32_t)size, ALIGN);
    for (block_t* b = g_head; b; b = b->next) {
        if (b->used || b->size < need) continue;
        split(b, need);
        b->used = HEAP_MAGIC;
        g_used += b->size;
        return (uint8_t*)b + HDR;
    }
    return NULL;
}

void* kzalloc(size_t size) {
    void* p = kmalloc(size);
    if (p) memset(p, 0, size);
    return p;
}

static block_t* hdr_of(void* p) {
    block_t* b = (block_t*)((uint8_t*)p - HDR);
    return (b->used == HEAP_MAGIC) ? b : NULL;
}

void kfree(void* ptr) {
    if (!ptr) return;
    block_t* b = hdr_of(ptr);
    if (!b) return; /* double free or garbage pointer: ignore */
    g_used -= b->size;
    b->used = 0;
    merge_next(b);
    if (b->prev && !b->prev->used) merge_next(b->prev);
}

void* krealloc(void* ptr, size_t size) {
    if (!ptr) return kmalloc(size);
    if (size == 0) { kfree(ptr); return NULL; }
    block_t* b = hdr_of(ptr);
    if (!b) return NULL;
    uint32_t need = align_up((uint32_t)size, ALIGN);
    if (need <= b->size) return ptr;

    /* grow in place by swallowing a free neighbour when possible: keeps
     * repeated appends (downloads, file writes) from copying every time */
    block_t* n = b->next;
    if (n && !n->used && b->size + HDR + n->size >= need) {
        g_used -= b->size;
        merge_next(b);
        split(b, need);
        g_used += b->size;
        return ptr;
    }

    void* np = kmalloc(size);
    if (!np) return NULL;
    memcpy(np, ptr, b->size);
    kfree(ptr);
    return np;
}

uint32_t kheap_total_bytes(void) { return g_total; }
uint32_t kheap_used_bytes(void)  { return g_used; }

uint32_t kheap_largest_free(void) {
    uint32_t best = 0;
    for (block_t* b = g_head; b; b = b->next)
        if (!b->used && b->size > best) best = b->size;
    return best;
}
