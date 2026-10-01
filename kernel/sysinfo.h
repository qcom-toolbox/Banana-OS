#ifndef SYSINFO_H
#define SYSINFO_H

#include "types.h"

typedef struct {
    char     cpu_vendor[13];    /* e.g. "GenuineIntel" */
    char     cpu_brand[49];     /* full CPU name string */
    uint32_t cpu_cores;         /* physical cores per package (best effort) */
    uint32_t cpu_threads;       /* logical threads per package */
    uint32_t cpu_family;
    uint32_t cpu_model;
    uint32_t cpu_stepping;
    uint32_t cpu_has_ht;        /* 1 if Hyper-Threading flag set */
    uint32_t mem_kb;            /* total memory in KiB from multiboot */
    int      uefi;              /* booted through UEFI firmware (else BIOS) */
} sysinfo_t;

void sysinfo_init(uint32_t mb_info_addr);
void sysinfo_init_mb2(uint32_t mb2_info_addr);
const sysinfo_t* sysinfo_get(void);

#endif
