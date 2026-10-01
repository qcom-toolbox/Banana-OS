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
} __attribute__((aligned(16))) block_t;   /* 16 or 32 bytes: payloads stay 16-aligned */

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

#define HEAP_MIN_ADDR 0x100000ull       /* below 1 MiB: BIOS/firmware leftovers */
#define HEAP_MAX_ADDR 0xE0000000ull     /* 3.5 GiB: stay clear of the 32-bit MMIO hole
                                         * (and inside the 64-bit kernel's 1:1 map) */

/* Largest piece of [lo, hi) that avoids [xlo, xhi): updates *best_lo/hi */
static void consider(uint64_t lo, uint64_t hi, uint64_t* best_lo, uint64_t* best_hi,
                     const uint64_t* xlo, const uint64_t* xhi, int nx) {
    if (lo < HEAP_MIN_ADDR) lo = HEAP_MIN_ADDR;
    if (hi > HEAP_MAX_ADDR) hi = HEAP_MAX_ADDR;
    if (hi <= lo) return;
    /* cut out the first excluded range that overlaps, recurse on both sides */
    for (int i = 0; i < nx; i++) {
        if (xhi[i] <= lo || xlo[i] >= hi) continue;
        consider(lo, xlo[i], best_lo, best_hi, xlo, xhi, nx);
        consider(xhi[i], hi, best_lo, best_hi, xlo, xhi, nx);
        return;
    }
    if (hi - lo > *best_hi - *best_lo) { *best_lo = lo; *best_hi = hi; }
}

/* The largest available RAM range from the Multiboot2 memory map that
 * does not overlap the kernel image or the Multiboot2 information.
 * (Firmware - UEFI in particular - splits memory into many regions; the
 * one the kernel was loaded into can be just a few MiB.) */
static int pick_region(uint32_t mb2, uint64_t* out_lo, uint64_t* out_hi) {
    if (!mb2) return 0;
    uint32_t total = *(uint32_t*)(uintptr_t)mb2;
    uint64_t xlo[2] = { 0x100000ull, mb2 };
    uint64_t xhi[2] = { align_up((uint32_t)(uintptr_t)_kernel_end, 4096u), (uint64_t)mb2 + total };
    uint64_t best_lo = 0, best_hi = 0;
    uint32_t off = 8;
    while (off + 8 <= total) {
        mb2_tag_t* tag = (mb2_tag_t*)(uintptr_t)(mb2 + off);
        if (tag->type == 0) break;
        if (tag->type == 6) {
            mb2_tag_mmap_t* mm = (mb2_tag_mmap_t*)tag;
            for (uint32_t pos = sizeof(*mm); pos + mm->entry_size <= mm->size; pos += mm->entry_size) {
                mb2_mmap_entry_t* e = (mb2_mmap_entry_t*)((uintptr_t)mm + pos);
                if (e->type != 1) continue;
                consider(e->base, e->base + e->length, &best_lo, &best_hi, xlo, xhi, 2);
            }
        }
        uint32_t adv = align_up(tag->size, 8);
        off += adv < 8 ? 8 : adv;
    }
    if (best_hi <= best_lo) return 0;
    *out_lo = best_lo;
    *out_hi = best_hi;
    return 1;
}

void kheap_init(uint32_t mb2_info_addr) {
    /* Every Multiboot2 consumer (fb.c, sysinfo.c) has copied what it needs
     * by now, but the info block is kept out of the heap anyway. */
    uint64_t lo, hi;
    uint32_t start, end;
    if (pick_region(mb2_info_addr, &lo, &hi) && hi - lo >= (1u << 20)) {
        start = align_up((uint32_t)lo, 4096u);
        end = (uint32_t)hi;
    } else {
        /* No usable map: right after the kernel, on a conservative 16 MiB machine. */
        start = align_up((uint32_t)(uintptr_t)_kernel_end, 4096u);
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
