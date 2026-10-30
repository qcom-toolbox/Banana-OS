/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "iwx_queue.h"
#include "../third_party/iwx/iwx_boot_regs.h"
#include "../kernel/kstring.h"
static uint16_t le16(const uint8_t* p){return (uint16_t)(p[0]|p[1]<<8);}
static uint32_t le32(const uint8_t* p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put16(uint8_t* p,uint16_t n){p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static void put64(uint8_t* p,uint64_t n){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(n>>(8*i));}
static int allocation(iwx_queue_t* q,iwx_dma_t* d,uint32_t n){
    iwx_dma_t block={0};
    if(q->dma->alloc(q->dma_owner,n,4096,&block))return -1;
    if(!block.data||block.size<n||block.size>8u*1024u*1024u||
       !block.physical||block.physical>0xffffffffu||(block.physical&4095u)||
       block.size-1u>0xffffffffu-block.physical){
        q->dma->release(q->dma_owner,&block);return -1;
    }
    memset(block.data,0,block.size);*d=block;return 0;
}
static void release(iwx_queue_t* q,iwx_dma_t* d){
    if(d->data){q->dma->release(q->dma_owner,d);memset(d,0,sizeof(*d));}
}
static void descriptor(iwx_queue_t* q,unsigned slot,unsigned buffer){
    uint8_t* p=(uint8_t*)q->rx_free.data+slot*(q->hardware.model==IWX_FW_AX210?16u:8u);
    /* Nonzero IDs make a stale/unwritten completion distinguishable. */
    if(q->hardware.model==IWX_FW_AX210){
        memset(p,0,16);put16(p,(uint16_t)(buffer+1));put64(p+8,q->rx[buffer].physical);
    }else put64(p,q->rx[buffer].physical|(buffer+1u));
}
int iwx_queue_close(iwx_queue_t* q){
    if(!q||q->polling)return -1;
    if(q->hardware.kicked||q->hardware.started||q->hardware.locked){
        if(iwx_hw_stop(&q->hardware)){q->state=IWX_QUEUE_FAILED;return -1;}
    }
    if(q->dma){
        release(q,&q->pending_dma);
        iwx_boot_release(&q->boot,q->dma,q->dma_owner);
        while(q->rx_allocated)release(q,&q->rx[--q->rx_allocated]);
        release(q,&q->commands);release(q,&q->rx_status);
        release(q,&q->rx_used);release(q,&q->rx_free);
    }
    memset(q,0,sizeof(*q));return 0;
}
int iwx_queue_prepare(iwx_queue_t* q,const iwx_firmware_t* fw,int model,
                       const iwx_dma_ops_t* dma,void* dma_owner,const iwx_io_ops_t* io,void* io_owner){
    if(!q||q->state!=IWX_QUEUE_EMPTY||q->rx_allocated||q->rx_free.data||
       !fw||!dma||!dma->alloc||!dma->release||!io||!io->read32||!io->write32||!io->delay_us||
       (model!=IWX_FW_AX200&&model!=IWX_FW_AX210))return -1;
    q->firmware=fw;q->dma=dma;q->dma_owner=dma_owner;
    q->hardware=(iwx_hw_t){.io=io,.owner=io_owner,.model=model};
    if(allocation(q,&q->rx_free,IWX_RX_COUNT*(model==IWX_FW_AX210?16u:8u))||
       allocation(q,&q->rx_used,IWX_RX_COUNT*(model==IWX_FW_AX210?32u:4u))||
       allocation(q,&q->rx_status,64)||
       allocation(q,&q->commands,IWX_TX_COUNT*IWX_TFD_BYTES))goto failed;
    for(unsigned i=0;i<IWX_RX_COUNT;i++){
        if(allocation(q,&q->rx[i],4096))goto failed;
        q->rx_allocated++;descriptor(q,i,i);q->available[i]=1;
    }
    uint32_t revision;
    if(io->read32(io_owner,IWX_CSR_HW_REV,&revision)||revision==0xffffffffu)goto failed;
    iwx_boot_rings_t rings={.rx_free=q->rx_free.physical,.rx_used=q->rx_used.physical,
        .rx_status=q->rx_status.physical,.command=q->commands.physical,
        .rx_count=IWX_RX_COUNT,.command_count=IWX_TX_COUNT,.mac_id=(uint16_t)revision};
    if(iwx_boot_prepare(&q->boot,fw,model,&rings,dma,dma_owner))goto failed;
    q->state=IWX_QUEUE_PREPARED;return 0;
failed:
    iwx_queue_close(q);return -1;
}
int iwx_queue_boot(iwx_queue_t* q){
    if(!q||q->state!=IWX_QUEUE_PREPARED)return -1;
    if(iwx_hw_start(&q->hardware)||iwx_hw_kick(&q->hardware,&q->boot,q->firmware)){
        q->state=IWX_QUEUE_FAILED;return -1;
    }
    q->state=IWX_QUEUE_BOOTING;return 0;
}
static int event(void* owner,const iwx_event_t* e){
    iwx_queue_t* q=owner;
    if(e->opcode==1&&e->group==0&&(e->queue&0x80u)){
        if(q->state!=IWX_QUEUE_BOOTING||iwx_alive_read(&q->alive,q->firmware,e))return -1;
        q->state=IWX_QUEUE_ALIVE;
        if(q->hardware.model==IWX_FW_AX200)q->pnvm_state=IWX_PNVM_READY;
    }else if(e->opcode==0xfe&&e->group==12&&(e->queue&0x80u)){
        if(q->hardware.model!=IWX_FW_AX210||q->state!=IWX_QUEUE_ALIVE||
           q->pnvm_state!=IWX_PNVM_WAITING||e->size!=4||
           iwx_firmware_command_version(q->firmware,12,0xfe,1)!=1)return -1;
        q->pnvm_status=le32(e->data);
        /* Fail closed on firmware-reported errors; keep calibration DMA owned. */
        if(q->pnvm_status)return -1;
        q->pnvm_state=IWX_PNVM_READY;
    }else if(!(e->queue&0x80u)){
        uint8_t group=e->group&~0x40u;
        if(!q->pending||e->queue!=0||e->index!=q->pending_index||
           e->opcode!=q->pending_opcode||
           (group!=q->pending_group&&!(q->pending_legacy&&group==0))){
            q->rx_dropped++;return 0;
        }
        if(e->size>sizeof(q->reply))return -1;
        memcpy(q->reply,e->data,e->size);q->reply_size=e->size;
        q->reply_status=(e->group&0x40u)?-1:0;
        q->reply_ready=1;q->pending=0;
        release(q,&q->pending_dma);
    }
    if(q->notify&&q->notify(q->notify_owner,e)<0)return -1;
    return 0;
}
int iwx_queue_poll(iwx_queue_t* q){
    if(!q||q->polling||(q->state!=IWX_QUEUE_BOOTING&&q->state!=IWX_QUEUE_ALIVE))return -1;
    q->polling=1;
    const iwx_io_ops_t* io=q->hardware.io;void* owner=q->hardware.owner;
    uint32_t interrupt;
    if(io->read32(owner,IWX_CSR_INT,&interrupt)||interrupt==0xffffffffu||
       (interrupt&0xfffffff0u)==0xa5a5a5a0u||
       (interrupt&(IWX_CSR_INT_BIT_HW_ERR|IWX_CSR_INT_BIT_SW_ERR)))goto failed;
    if(interrupt&IWX_CSR_INT_BIT_RF_KILL){
        uint32_t gp;
        if(io->read32(owner,IWX_CSR_GP_CNTRL,&gp)||gp==0xffffffffu||
           !(gp&IWX_CSR_GP_CNTRL_REG_FLAG_HW_RF_KILL_SW))goto failed;
    }
    if(interrupt&IWX_CSR_INT_BIT_ALIVE){
        if(q->rx_enabled)goto failed;
        for(unsigned i=0;i<IWX_RX_COUNT;i++)descriptor(q,i,i);
        __sync_synchronize();
        if(io->write32(owner,IWX_RFH_Q0_FRBDCB_WIDX_TRG,8))goto failed;
        q->rx_enabled=1;
    }
    if((interrupt&(IWX_CSR_INT_BIT_FH_RX|IWX_CSR_INT_BIT_SW_RX))&&
       io->write32(owner,IWX_CSR_FH_INT_STATUS,IWX_CSR_FH_INT_RX_MASK))goto failed;
    if((interrupt&IWX_CSR_INT_BIT_FH_TX)&&
       io->write32(owner,IWX_CSR_FH_INT_STATUS,IWX_CSR_FH_INT_TX_MASK))goto failed;
    if(interrupt&&io->write32(owner,IWX_CSR_INT,interrupt))goto failed;
    if(!q->rx_enabled){q->polling=0;return 0;}
    __sync_synchronize();
    unsigned hardware=le16((const uint8_t*)q->rx_status.data)&(IWX_RX_COUNT-1u),returned=0;
    while(q->read!=hardware){
        const uint8_t* completion=(const uint8_t*)q->rx_used.data+
            q->read*(q->hardware.model==IWX_FW_AX210?32u:4u);
        unsigned id=q->hardware.model==IWX_FW_AX210?le16(completion+4):le32(completion)&0xfffu;
        if(!id||id>IWX_RX_COUNT||!q->available[id-1])goto failed;
        unsigned buffer=id-1;q->available[buffer]=0;
        q->read=(uint16_t)((q->read+1u)&(IWX_RX_COUNT-1u));
        /* Unsupported split frames are discarded; command replies fit 4 KiB. */
        if(q->hardware.model==IWX_FW_AX210&&(completion[6]&1u))q->rx_dropped++;
        else if(iwx_rx_events(q->rx[buffer].data,4096,q->hardware.model,event,q)<0)goto failed;
        memset(q->rx[buffer].data,0,4096);
        descriptor(q,q->write,buffer);
        q->write=(uint16_t)((q->write+1u)&(IWX_RX_COUNT-1u));
        q->returned[returned++]=(uint16_t)buffer;
    }
    if(returned){
        for(unsigned i=0;i<returned;i++)q->available[q->returned[i]]=1;
        __sync_synchronize();
        unsigned last=q->write?q->write-1u:IWX_RX_COUNT-1u;
        if(io->write32(owner,IWX_RFH_Q0_FRBDCB_WIDX_TRG,last&~7u))goto failed;
    }
    q->polling=0;return (int)returned;
failed:
    q->polling=0;q->state=IWX_QUEUE_FAILED;return -1;
}
int iwx_queue_pnvm(iwx_queue_t* q,const uint8_t* blob,uint32_t bytes){
    if(!q||q->state!=IWX_QUEUE_ALIVE||q->hardware.model!=IWX_FW_AX210||
       q->polling||q->pending||q->pnvm_state!=IWX_PNVM_NONE)return -1;
    if(!(q->alive.sku[0]|q->alive.sku[1]|q->alive.sku[2])){
        q->pnvm_state=IWX_PNVM_READY;return 0;
    }
    if(iwx_firmware_command_version(q->firmware,12,0xfe,1)!=1)return -2;
    uint32_t revision,radio;
    const iwx_io_ops_t* io=q->hardware.io;void* owner=q->hardware.owner;
    if(io->read32(owner,IWX_CSR_HW_REV,&revision)||revision==0xffffffffu||
       io->read32(owner,IWX_CSR_HW_RF_ID,&radio)||radio==0xffffffffu){
        q->state=IWX_QUEUE_FAILED;return -1;
    }
    iwx_pnvm_t selected;
    int rc=iwx_pnvm_select(&selected,blob,bytes,q->alive.sku,
                          IWX_CSR_HW_REV_TYPE(revision),IWX_CSR_HW_RFID_TYPE(radio));
    if(rc)return rc;
    if(iwx_boot_attach_pnvm(&q->boot,q->firmware,&selected,q->dma,q->dma_owner))return -1;
    q->pnvm_version=selected.version;
    /* Publish expectation before the uncertain doorbell. Never discard PNVM DMA
     * after handoff failure; firmware may already have observed the scratch. */
    q->pnvm_state=IWX_PNVM_WAITING;
    if(iwx_hw_pnvm(&q->hardware)){q->state=IWX_QUEUE_FAILED;return -1;}
    return 0;
}
int iwx_queue_command(iwx_queue_t* q,uint8_t opcode,uint8_t group,const void* data,uint32_t length){
    if(!q||q->state!=IWX_QUEUE_ALIVE||q->pnvm_state!=IWX_PNVM_READY||q->pending||q->polling||
       length>IWX_COMMAND_MAX||(!data&&length))return -1;
    int version=iwx_firmware_command_version(q->firmware,group?group:1,opcode,0);
    if(version<0||version==99||version>255)return -2;
    iwx_dma_t command={0};
    if(allocation(q,&command,8+length))return -1;
    uint8_t index=(uint8_t)q->sequence;
    uint8_t* desc=(uint8_t*)q->commands.data+index*IWX_TFD_BYTES;
    if(iwx_command_build(desc,IWX_TFD_BYTES,command.data,command.size,command.physical,
                         opcode,group,(uint8_t)version,index,data,length)){
        q->dma->release(q->dma_owner,&command);return -1;
    }
    q->pending_dma=command;q->pending=1;q->reply_ready=0;q->reply_size=0;
    q->pending_index=index;q->pending_opcode=opcode;q->pending_group=group?group:1;q->pending_legacy=group==0;
    q->sequence++;
    __sync_synchronize();
    unsigned write=q->hardware.model==IWX_FW_AX210?q->sequence:(uint8_t)q->sequence;
    /* Any failed doorbell has an uncertain DMA outcome. Retain the command. */
    if(q->hardware.io->write32(q->hardware.owner,IWX_HBUS_TARG_WRPTR,write)){
        q->state=IWX_QUEUE_FAILED;return -1;
    }
    return 0;
}
