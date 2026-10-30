/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "iwx_init.h"
#include "../kernel/kstring.h"
static int failed(iwx_init_t* init,int error){
    init->error=error;init->state=IWX_INIT_FAILED;
    init->queue->state=IWX_QUEUE_FAILED;return error;
}
int iwx_init_begin(iwx_init_t* init,iwx_queue_t* q,uint32_t now){
    if(!init||init->state!=IWX_INIT_EMPTY||!q||q->state!=IWX_QUEUE_ALIVE||
       q->pnvm_state!=IWX_PNVM_READY||q->polling||q->pending||q->init_armed||q->init_complete)return -1;
    int config_version=iwx_firmware_command_version(q->firmware,2,3,0);
    if((config_version!=-1&&config_version!=0)||
       iwx_firmware_command_version(q->firmware,12,0,0)!=1||
       iwx_firmware_command_version(q->firmware,12,2,0)!=1)return -2;
    memset(init,0,sizeof(*init));init->queue=q;
    if(iwx_hw_mac(&q->hardware,init->mac))return failed(init,-1);
    uint8_t flags[4]={2,0,0,0}; /* INIT_NVM, API-1 init extended config */
    if(iwx_queue_command(q,3,2,flags,4))return failed(init,-1);
    init->state=IWX_INIT_CONFIG;init->since=now;return 0;
}
int iwx_init_poll(iwx_init_t* init,uint32_t now){
    if(!init||!init->queue||init->state==IWX_INIT_EMPTY)return -1;
    if(init->state==IWX_INIT_FAILED)return init->error;
    if(init->state==IWX_INIT_READY)return 1;
    iwx_queue_t* q=init->queue;
    if(iwx_queue_poll(q)<0)return failed(init,-1);
    if((uint32_t)(now-init->since)>=2000u)return failed(init,-3);
    if(init->state==IWX_INIT_NOTIFICATION){
        if(!q->init_complete)return 0;
        uint8_t reserved[4]={0};
        if(iwx_queue_command(q,2,12,reserved,4))return failed(init,-1);
        init->state=IWX_INIT_INFO;init->since=now;return 0;
    }
    if(q->pending)return 0;
    if(!q->reply_ready||q->reply_status)return failed(init,-2);
    q->reply_ready=0;
    if(init->state==IWX_INIT_CONFIG){
        uint8_t reserved[4]={0};
        q->init_armed=1;
        if(iwx_queue_command(q,0,12,reserved,4))return failed(init,-1);
        init->state=IWX_INIT_COMPLETE_ACK;init->since=now;return 0;
    }
    if(init->state==IWX_INIT_COMPLETE_ACK){
        init->state=IWX_INIT_NOTIFICATION;init->since=now;return 0;
    }
    if(init->state==IWX_INIT_INFO){
        if(iwx_nvm_read(&init->nvm,q->firmware,q->hardware.model,init->mac,q->reply,q->reply_size))
            return failed(init,-2);
        init->state=IWX_INIT_READY;return 1;
    }
    return failed(init,-1);
}
