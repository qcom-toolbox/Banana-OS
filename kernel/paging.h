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

#endif
