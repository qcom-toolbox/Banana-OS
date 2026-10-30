/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IWX_BOOT_H
#define IWX_BOOT_H
#include "iwx_fw.h"
#include "iwx_pnvm.h"
typedef struct { void* data; uint64_t physical; uint32_t size; } iwx_dma_t;
typedef struct {
    int (*alloc)(void* owner,uint32_t bytes,uint32_t align,iwx_dma_t* out);
    void (*release)(void* owner,iwx_dma_t*);
} iwx_dma_ops_t;
typedef struct {
    uint64_t rx_free,rx_used,rx_status,command;
    uint32_t rx_count,command_count;
    uint16_t mac_id;
} iwx_boot_rings_t;
#define IWX_BOOT_BLOCKS (IWX_FW_SECTIONS+4+IWX_PNVM_SEGMENTS+1)
typedef struct {
    iwx_dma_t blocks[IWX_BOOT_BLOCKS];
    uint32_t nblocks;
    iwx_dma_t context,scratch,peripheral,iml;
    unsigned lmac,umac,paging;
    int model;
    uint32_t iml_bytes;
    iwx_dma_t pnvm;
    uint32_t pnvm_bytes;
} iwx_boot_t;
/* out must be zero initialized and unused. Ring memory must remain owned
 * until the device is quiesced. Failure rolls back every allocation.
 * Prepare never performs MMIO or starts DMA. */
int iwx_boot_prepare(iwx_boot_t*,const iwx_firmware_t*,int model,
                     const iwx_boot_rings_t*,const iwx_dma_ops_t*,void* owner);
/* Only while firmware awaits PNVM handoff. Publishes the scratch pointer
 * after every allocation succeeds. Failure keeps earlier DMA blocks owned. */
int iwx_boot_attach_pnvm(iwx_boot_t*,const iwx_firmware_t*,const iwx_pnvm_t*,
                          const iwx_dma_ops_t*,void* owner);
/* Only before DMA starts, or after bus mastering is disabled and stopped. */
void iwx_boot_release(iwx_boot_t*,const iwx_dma_ops_t*,void* owner);
#endif
