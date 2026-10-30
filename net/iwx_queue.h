/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IWX_QUEUE_H
#define IWX_QUEUE_H
#include "iwx_hw.h"
#include "iwx_msg.h"
#define IWX_RX_COUNT 512u
#define IWX_TX_COUNT 256u
enum { IWX_PNVM_NONE,IWX_PNVM_WAITING,IWX_PNVM_READY };
enum { IWX_QUEUE_EMPTY,IWX_QUEUE_PREPARED,IWX_QUEUE_BOOTING,IWX_QUEUE_ALIVE,IWX_QUEUE_FAILED };
/* Per-card object: zero initialize on the heap. Serialized owner only. Firmware
 * views and both ops tables must remain resident until close succeeds. */
typedef struct {
    iwx_hw_t hardware;
    iwx_boot_t boot;
    const iwx_firmware_t* firmware;
    const iwx_dma_ops_t* dma;
    void* dma_owner;
    iwx_dma_t rx_free,rx_used,rx_status,commands,rx[IWX_RX_COUNT],pending_dma;
    uint16_t returned[IWX_RX_COUNT],read,write,sequence;
    uint8_t available[IWX_RX_COUNT];
    uint32_t rx_allocated,state,rx_dropped,pnvm_state,pnvm_status,pnvm_version;
    int polling,pending,reply_ready,reply_status,rx_enabled,pending_legacy;
    uint8_t pending_index,pending_opcode,pending_group;
    uint8_t reply[IWX_COMMAND_MAX];
    uint32_t reply_size;
    iwx_alive_t alive;
    int (*notify)(void*,const iwx_event_t*);
    void* notify_owner;
} iwx_queue_t;
int iwx_queue_prepare(iwx_queue_t*,const iwx_firmware_t*,int model,
                       const iwx_dma_ops_t*,void* dma_owner,const iwx_io_ops_t*,void* io_owner);
int iwx_queue_boot(iwx_queue_t*);
int iwx_queue_poll(iwx_queue_t*);
/* Nonblocking AX210 calibration handoff after ALIVE. Exact hardware/SKU match.
 * poll must receive a successful PNVM notification before commands are allowed.
 * Blob views are copied; no blob lifetime extends beyond this call. No fallback.
 * Owner must impose a deadline and close on timeout. */
int iwx_queue_pnvm(iwx_queue_t*,const uint8_t* blob,uint32_t bytes);
/* One outstanding command. Reuse requires an acknowledged completion.
 * A timeout must fail/close the queue, never free or recycle pending DMA. */
int iwx_queue_command(iwx_queue_t*,uint8_t opcode,uint8_t group,const void*,uint32_t);
/* On failure all allocations remain owned: caller must keep this object. */
int iwx_queue_close(iwx_queue_t*);
#endif
