/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IWX_MSG_H
#define IWX_MSG_H
#include "iwx_fw.h"
#define IWX_COMMAND_MAX 4088u
#define IWX_TFD_BYTES 256u
typedef struct {
    uint8_t opcode,group,index,queue;
    const uint8_t* data;uint32_t size;
} iwx_event_t;
typedef struct {
    uint32_t sku[3],lmac_major,umac_major,flags;
    uint64_t imr_base;uint32_t imr_size,imr_enabled;
} iwx_alive_t;
/* Builds a DQA command-queue slot; no MMIO/ownership changes.
 * Invalid arguments leave the slot untouched. Slot must not be device-owned. */
int iwx_command_build(uint8_t* descriptor,uint32_t descriptor_capacity,
                      uint8_t* command,uint32_t command_capacity,uint64_t physical,
                      uint8_t opcode,uint8_t group,uint8_t version,uint8_t index,
                      const void* payload,uint32_t length);
/* Borrowed event views, valid only during callback. callback: 0 continue,
 * positive stop, negative error. No allocations or hardware access. */
int iwx_rx_events(const uint8_t*,uint32_t,int model,int (*callback)(void*,const iwx_event_t*),void*);
int iwx_alive_read(iwx_alive_t*,const iwx_firmware_t*,const iwx_event_t*);
#endif
