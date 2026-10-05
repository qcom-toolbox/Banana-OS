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

#else

int mmio_map(uint64_t phys, uint64_t size) {
    return phys + size <= (4ull << 30);   /* no paging: only the low 4 GiB exist */
}

#endif
