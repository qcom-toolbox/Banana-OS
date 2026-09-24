#ifndef KHEAP_H
#define KHEAP_H

#include "types.h"

/* Kernel heap: a first-fit, address-ordered free list with splitting and
 * coalescing, laid over the RAM between the end of the kernel image and
 * the end of the usable memory region it sits in (from the Multiboot2
 * memory map). No paging here, so heap pointers are physical addresses,
 * which is also what makes them directly usable as DMA buffers. */

void  kheap_init(uint32_t mb2_info_addr);
void* kmalloc(size_t size);                 /* 16-byte aligned, NULL on OOM */
void* kzalloc(size_t size);                 /* zeroed */
void* krealloc(void* ptr, size_t size);     /* grows in place when it can */
void  kfree(void* ptr);

uint32_t kheap_total_bytes(void);
uint32_t kheap_used_bytes(void);
uint32_t kheap_largest_free(void);

#endif
