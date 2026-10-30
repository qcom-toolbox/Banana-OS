/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IWX_HW_H
#define IWX_HW_H
#include "iwx_boot.h"
typedef struct {
    int (*read32)(void*,uint32_t,uint32_t*);
    int (*write32)(void*,uint32_t,uint32_t);
    void (*delay_us)(void*,unsigned);
} iwx_io_ops_t;
typedef struct {
    const iwx_io_ops_t* io;
    void* owner;
    int model,locked,started,kicked;
} iwx_hw_t;
/* Serialized caller only. No PCI binding and no DMA memory ownership here. */
int iwx_hw_start(iwx_hw_t*);
int iwx_hw_kick(iwx_hw_t*,const iwx_boot_t*,const iwx_firmware_t*);
/* After ALIVE and published AX210 PNVM scratch data. No memory ownership. */
int iwx_hw_pnvm(iwx_hw_t*);
/* Success proves bus-master stop acknowledged. Failure keeps DMA ownership. */
int iwx_hw_stop(iwx_hw_t*);
#endif
