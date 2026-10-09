#ifndef MEMINFO_H
#define MEMINFO_H

#include "types.h"

/* Where the RAM goes - one set of numbers for `free`, `top`, `htop`,
 * `ram_info` and the Task Manager.
 *
 * Banana OS has no paging allocator: it runs from one stretch of RAM (the
 * largest usable region below 3.5 GB, from the firmware's memory map) - its
 * kernel image at 1 MB, then the heap that everything else comes from (file
 * data, windows, network buffers, apps). "total" is that stretch, "free" is
 * what the heap has left. RAM in other regions is "installed" but unused. */
typedef struct {
    uint32_t installed_kb;   /* usable RAM in the machine, from the memory map */
    uint32_t total_kb;       /* what Banana OS runs in: kernel image + heap */
    uint32_t kernel_kb;      /* the kernel image with its static buffers */
    uint32_t used_kb;        /* kernel image + what the heap handed out */
    uint32_t files_kb;       /* of which: file data read into RAM, and the file tables */
    uint32_t free_kb;        /* what the heap has left */
    uint32_t avail_kb;       /* free + file data the cache would give back (saved, not in use) */
    uint32_t largest_kb;     /* its largest free block (the biggest single allocation possible) */
} meminfo_t;

void meminfo_get(meminfo_t* m);

#endif
