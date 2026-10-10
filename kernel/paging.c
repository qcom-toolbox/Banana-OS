#include "paging.h"
#include "kheap.h"
#include "kstring.h"
#include "serial.h"

#ifdef __x86_64__

#define P_PRESENT 0x001ull
#define P_WRITE   0x002ull
#define P_PWT     0x008ull
#define P_PCD     0x010ull
#define P_LARGE   0x080ull
#define ADDR_MASK 0x000FFFFFFFFFF000ull

/* a zeroed, 4 KiB-aligned page for a new table (heap memory is below
 * 4 GiB and identity-mapped, so its address is also its physical one) */
static uint64_t* new_table(void) {
    uint8_t* raw = (uint8_t*)kmalloc(8192);
    if (!raw) return NULL;
    uint64_t* t = (uint64_t*)(((uintptr_t)raw + 4095) & ~(uintptr_t)4095);
    memset(t, 0, 4096);
    return t;
}

int mmio_map(uint64_t phys, uint64_t size) {
    if (phys + size <= (4ull << 30)) return 1;          /* mapped since boot */
    if (phys + size > (1ull << 46)) return 0;           /* beyond what we support */
    uintptr_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t* pml4 = (uint64_t*)(cr3 & ADDR_MASK);
    for (uint64_t a = phys & ~0x1FFFFFull; a < phys + size; a += 0x200000ull) {
        if (a < (4ull << 30)) continue;
        uint64_t* e4 = &pml4[(a >> 39) & 511];
        if (!(*e4 & P_PRESENT)) {
            uint64_t* t = new_table();
            if (!t) return 0;
            *e4 = (uint64_t)(uintptr_t)t | P_PRESENT | P_WRITE;
        }
        uint64_t* pdpt = (uint64_t*)(uintptr_t)(*e4 & ADDR_MASK);
        uint64_t* e3 = &pdpt[(a >> 30) & 511];
        if (!(*e3 & P_PRESENT)) {
            uint64_t* t = new_table();
            if (!t) return 0;
            *e3 = (uint64_t)(uintptr_t)t | P_PRESENT | P_WRITE;
        }
        if (*e3 & P_LARGE) continue;                    /* a 1 GiB page covers it already */
        uint64_t* pd = (uint64_t*)(uintptr_t)(*e3 & ADDR_MASK);
        uint64_t* e2 = &pd[(a >> 21) & 511];
        *e2 = a | P_PRESENT | P_WRITE | P_LARGE | P_PCD | P_PWT;   /* device memory: uncached */
        __asm__ volatile("invlpg (%0)" :: "r"((uintptr_t)a) : "memory");
    }
    klog("paging: mapped device memory at %llx (%llu KiB)\n", phys, size >> 10);
    return 1;
}

/* ── write-combining ── */
#define P_PAT_LARGE (1ull << 12)              /* the PAT bit of a 2 MiB page */
#define P_PAT_SMALL (1ull << 7)               /* ... of a 4 KiB one */
static int g_pat;

void paging_pat_init(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (!(d & (1u << 16))) return;                 /* no PAT */
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x277));
    lo = (lo & ~0x0000FF00u) | (0x01u << 8);       /* entry 1 (PWT): write-combining (was write-through) */
    __asm__ volatile("wbinvd" ::: "memory");
    __asm__ volatile("wrmsr" :: "a"(lo), "d"(hi), "c"(0x277));
    uintptr_t cr3;
    __asm__ volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) :: "memory");
    __asm__ volatile("wbinvd" ::: "memory");
    g_pat = 1;
}

int paging_set_wc(uint64_t phys, uint64_t size) {
    if (!g_pat || !size || !mmio_map(phys, size)) return -1;
    uintptr_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t* pml4 = (uint64_t*)(cr3 & ADDR_MASK);
    for (uint64_t a = phys & ~0xFFFull; a < phys + size; ) {
        uint64_t e4 = pml4[(a >> 39) & 511];
        if (!(e4 & P_PRESENT)) return -1;
        uint64_t* pdpt = (uint64_t*)(uintptr_t)(e4 & ADDR_MASK);
        uint64_t e3 = pdpt[(a >> 30) & 511];
        if (!(e3 & P_PRESENT) || (e3 & P_LARGE)) return -1;   /* (1 GiB pages: not ours to change) */
        uint64_t* pd = (uint64_t*)(uintptr_t)(e3 & ADDR_MASK);
        uint64_t* e2 = &pd[(a >> 21) & 511];
        if (!(*e2 & P_PRESENT)) return -1;
        if (*e2 & P_LARGE) {
            *e2 = (*e2 & ~(P_PCD | P_PAT_LARGE)) | P_PWT;
            a = (a & ~0x1FFFFFull) + 0x200000ull;
        } else {
            uint64_t* pt = (uint64_t*)(uintptr_t)(*e2 & ADDR_MASK);
            uint64_t* e1 = &pt[(a >> 12) & 511];
            *e1 = (*e1 & ~(P_PCD | P_PAT_SMALL)) | P_PWT;
            a += 0x1000ull;
        }
    }
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");    /* every TLB entry of it gone */
    __asm__ volatile("wbinvd" ::: "memory");
    klog("paging: write-combining at %llx (%llu KiB)\n", phys, size >> 10);
    return 0;
}

/* the 4 KiB page table entry for addr (below 4 GiB), splitting the 2 MiB
 * page that covers it into 512 small ones (same mapping) the first time */
static uint64_t* small_pte(uintptr_t addr) {
    if (addr >= (4ull << 30)) return NULL;
    uintptr_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t* pml4 = (uint64_t*)(cr3 & ADDR_MASK);
    uint64_t* pdpt = (uint64_t*)(uintptr_t)(pml4[0] & ADDR_MASK);
    uint64_t* pd = (uint64_t*)(uintptr_t)(pdpt[(addr >> 30) & 3] & ADDR_MASK);
    uint64_t* e2 = &pd[(addr >> 21) & 511];
    if (*e2 & P_LARGE) {
        uint64_t* pt = new_table();
        if (!pt) return NULL;
        uint64_t big = *e2 & ~0x1FFFFFull & ADDR_MASK;
        uint64_t flags = *e2 & (P_WRITE | P_PWT | P_PCD);
        for (int i = 0; i < 512; i++) pt[i] = (big + ((uint64_t)i << 12)) | P_PRESENT | flags;
        *e2 = (uint64_t)(uintptr_t)pt | P_PRESENT | P_WRITE;
        __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");   /* flush */
    }
    uint64_t* pt = (uint64_t*)(uintptr_t)(*e2 & ADDR_MASK);
    return &pt[(addr >> 12) & 511];
}

int paging_guard(uintptr_t addr, uint32_t size, int guard) {
    for (uintptr_t a = addr & ~(uintptr_t)4095; a < addr + size; a += 4096) {
        uint64_t* pte = small_pte(a);
        if (!pte) return 0;
        if (guard) *pte &= ~P_PRESENT;
        else *pte |= P_PRESENT;
        __asm__ volatile("invlpg (%0)" :: "r"(a) : "memory");
    }
    return 1;
}

/* The first 2 MiB are one big page at boot; split into 4 KiB pages with
 * page 0 left out, a NULL pointer dereference (an app's most common bug)
 * faults instead of quietly reading or scribbling on low memory. */
static uint64_t g_low_pt[512] __attribute__((aligned(4096)));

void paging_guard_null(void) {
    uintptr_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t* pml4 = (uint64_t*)(cr3 & ADDR_MASK);
    uint64_t* pdpt = (uint64_t*)(uintptr_t)(pml4[0] & ADDR_MASK);
    uint64_t* pd = (uint64_t*)(uintptr_t)(pdpt[0] & ADDR_MASK);
    if (!(pd[0] & P_LARGE)) return;                     /* already split */
    for (int i = 0; i < 512; i++) g_low_pt[i] = ((uint64_t)i << 12) | P_PRESENT | P_WRITE;
    g_low_pt[0] = 0;                                    /* 0..4095: not present */
    pd[0] = (uint64_t)(uintptr_t)g_low_pt | P_PRESENT | P_WRITE;
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");   /* flush the TLB */
}

#else

void paging_guard_null(void) {}     /* no paging on the 32-bit kernel */

int paging_guard(uintptr_t addr, uint32_t size, int guard) {
    (void)addr; (void)size; (void)guard;
    return 0;
}


void paging_pat_init(void) {}
int  paging_set_wc(uint64_t phys, uint64_t size) { (void)phys; (void)size; return -1; }

int mmio_map(uint64_t phys, uint64_t size) {
    return phys + size <= (4ull << 30);   /* no paging: only the low 4 GiB exist */
}

#endif
