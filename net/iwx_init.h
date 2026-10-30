/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IWX_INIT_H
#define IWX_INIT_H
#include "iwx_queue.h"
#include "iwx_nvm.h"
enum { IWX_INIT_EMPTY,IWX_INIT_CONFIG,IWX_INIT_COMPLETE_ACK,IWX_INIT_NOTIFICATION,
       IWX_INIT_INFO,IWX_INIT_READY,IWX_INIT_FAILED };
typedef struct {
    iwx_queue_t* queue;
    iwx_nvm_t nvm;
    uint32_t state,since;
    int error;
    uint8_t mac[6];
} iwx_init_t;
/* Zero initialize. Exclusive command owner through READY/FAILED.
 * READY means initialization metadata only, not an associated radio.
 * Queue storage must stay resident until the initializer is discarded.
 * Milliseconds use wrapping uint32 monotonic time. Each phase has a 2s bound.
 * Failures retain DMA and require an acknowledged queue_close by the owner. */
int iwx_init_begin(iwx_init_t*,iwx_queue_t*,uint32_t now_ms);
/* Poll transport and advance: 0 waiting, 1 metadata ready, negative failure. */
int iwx_init_poll(iwx_init_t*,uint32_t now_ms);
#endif
