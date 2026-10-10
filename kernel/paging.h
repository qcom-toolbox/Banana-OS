#ifndef PAGING_H
#define PAGING_H

#include "types.h"

/*
 * Device memory above 4 GiB. The 64-bit kernel identity-maps the first
 * 4 GiB at boot (boot/boot64.asm); UEFI firmware likes to put 64-bit
 * PCI BARs (NVMe, graphics, sometimes xHCI / HDA) higher than that.
 * mmio_map() identity-maps such a range too (2 MiB pages, uncached), so
 * the physical address can be used as a pointer.
 *
 * Returns 1 if [phys, phys + size) is usable now; the 32-bit kernel runs
 * without paging and cannot reach above 4 GiB (0 then).
 */
int mmio_map(uint64_t phys, uint64_t size);

/* 64-bit kernel: unmaps page 0, so NULL pointer accesses fault */
void paging_guard_null(void);

/* 64-bit kernel: makes [addr, addr + size) (4 KiB pages, below 4 GiB)
 * fault on any access (guard = 1) or usable again (guard = 0) - guard
 * pages under app stacks. Returns 0 where that is not possible. */
int  paging_guard(uintptr_t addr, uint32_t size, int guard);

/* Write-combining for framebuffers (64-bit kernel): paging_pat_init() sets
 * the page attribute table on this CPU (every CPU must, before using such
 * pages: the boot one first, each other one when it starts) - PWT alone
 * then means write-combining; paging_set_wc() maps a range so. 0, or -1
 * (32-bit kernel, no PAT). */
void paging_pat_init(void);
int  paging_set_wc(uint64_t phys, uint64_t size);

#endif
