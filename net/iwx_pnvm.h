/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IWX_PNVM_H
#define IWX_PNVM_H
#include "iwx_fw.h"
#define IWX_PNVM_SEGMENTS 64
typedef struct { const uint8_t* data;uint32_t size,tag; } iwx_pnvm_segment_t;
typedef struct {
    uint32_t sku[3],version,nsegments,size;
    uint16_t mac,rf;
    iwx_pnvm_segment_t segments[IWX_PNVM_SEGMENTS];
} iwx_pnvm_t;
/* Borrowed views; caller keeps the blob resident. Exact identity only:
 * no generic fallback to another board's calibration/regulatory data.
 * -1 malformed, -2 no match, 0 selected. Failure clears output. */
int iwx_pnvm_select(iwx_pnvm_t*,const uint8_t*,uint32_t,
                    const uint32_t sku[3],uint16_t mac,uint16_t rf);
#endif
