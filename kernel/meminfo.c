#include "meminfo.h"
#include "kheap.h"
#include "fs.h"
#include "sysinfo.h"

extern char _kernel_end[];   /* boot/linker.ld: the image starts at 1 MB */

void meminfo_get(meminfo_t* m) {
    uint32_t kernel = (uint32_t)(uintptr_t)_kernel_end - 0x100000u;
    uint32_t heap_total = kheap_total_bytes(), heap_used = kheap_used_bytes();
    m->kernel_kb = (kernel + 1023u) >> 10;
    m->total_kb = m->kernel_kb + (heap_total >> 10);
    m->used_kb = m->kernel_kb + ((heap_used + 1023u) >> 10);
    m->free_kb = (heap_total - heap_used) >> 10;
    m->files_kb = (fs_ram_used_bytes() + 1023u) >> 10;
    m->largest_kb = kheap_largest_free() >> 10;
    m->installed_kb = sysinfo_get()->mem_kb;
    if (m->installed_kb < m->total_kb) m->installed_kb = m->total_kb;
}
