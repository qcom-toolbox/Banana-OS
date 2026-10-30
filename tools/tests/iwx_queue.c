/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../net/iwx_queue.h"
#include "../../third_party/iwx/iwx_boot_regs.h"
static unsigned checks;
static uint8_t* pnvm_data;static unsigned pnvm_bytes;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"iwx queue FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void put16(void* out,unsigned n){uint8_t* p=out;p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static void put32(void* out,uint32_t n){uint8_t* p=out;for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i));}
static uint16_t get16(const void* in){const uint8_t* p=in;return (uint16_t)(p[0]|p[1]<<8);}
static uint64_t get64(const void* in){const uint8_t* p=in;uint64_t n=0;for(unsigned i=0;i<8;i++)n|=(uint64_t)p[i]<<(8*i);return n;}
typedef struct {
 unsigned calls,live,fail,bad;
 uint64_t next;
} memory_t;
static int allocate(void* owner,uint32_t n,uint32_t align,iwx_dma_t* d){
 memory_t* m=owner;CHECK(align==4096&&n);if(++m->calls==m->fail)return -1;
 if(!m->live)m->next=0x1000000;
 d->size=(n+4095u)&~4095u;d->data=malloc(d->size);CHECK(d->data);
 d->physical=m->next;m->next+=d->size;CHECK(m->next<0x100000000ull);m->live++;
 if(m->bad==m->calls)d->physical=0x100000000ull;
 memset(d->data,0xa5,d->size);return 0;
}
static void release(void* owner,iwx_dma_t* d){
 memory_t* m=owner;CHECK(d->data&&m->live);free(d->data);m->live--;
}
static const iwx_dma_ops_t dma={allocate,release};
typedef struct {
 uint32_t regs[2048];unsigned operations,fail,delay;
 int stopfail,unplug,rfkill;
 uint32_t badwrite;
} device_t;
static int read32(void* owner,uint32_t reg,uint32_t* value){
 device_t* d=owner;if(++d->operations==d->fail)return -1;
 CHECK(!(reg&3)&&reg<sizeof(d->regs));
 if(d->unplug){*value=0xffffffffu;return 0;}
 if(reg==IWX_CSR_GP_CNTRL){
  if(d->regs[reg/4]&IWX_CSR_GP_CNTRL_REG_FLAG_INIT_DONE)
   d->regs[reg/4]|=IWX_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY;
  if(d->rfkill)d->regs[reg/4]&=~IWX_CSR_GP_CNTRL_REG_FLAG_HW_RF_KILL_SW;
  else d->regs[reg/4]|=IWX_CSR_GP_CNTRL_REG_FLAG_HW_RF_KILL_SW;
 }
 *value=reg==IWX_HBUS_TARG_PRPH_RDAT?0:d->regs[reg/4];return 0;
}
static int write32(void* owner,uint32_t reg,uint32_t value){
 device_t* d=owner;if(++d->operations==d->fail||reg==d->badwrite)return -1;
 CHECK(!(reg&3)&&reg<sizeof(d->regs));
 if(reg==IWX_CSR_INT||reg==IWX_CSR_FH_INT_STATUS)d->regs[reg/4]&=~value;
 else d->regs[reg/4]=value;
 if(reg==IWX_CSR_RESET&&(value&IWX_CSR_RESET_REG_FLAG_STOP_MASTER)&&!d->stopfail)
  d->regs[reg/4]|=IWX_CSR_RESET_REG_FLAG_MASTER_DISABLED;
 return 0;
}
static void delay(void* owner,unsigned us){device_t* d=owner;d->delay+=us;CHECK(d->delay<1000000);}
static const iwx_io_ops_t io={read32,write32,delay};
static void init(device_t* d,int model){
 memset(d,0,sizeof(*d));d->badwrite=0xffffffffu;
 d->regs[IWX_CSR_HW_REV/4]=model==200?0x340u:0x420u;
 d->regs[IWX_CSR_HW_RF_ID/4]=0x10d000;
}
static void prepared(iwx_queue_t* q,const iwx_firmware_t* fw,int model,memory_t* m,device_t* d){
 CHECK(!m->live);memset(q,0,sizeof(*q));memset(m,0,sizeof(*m));init(d,model);
 CHECK(iwx_queue_prepare(q,fw,model,&dma,m,&io,d)==0);
 CHECK(q->state==IWX_QUEUE_PREPARED&&q->rx_allocated==512);
 CHECK(d->operations==1); /* Preparing never enables device DMA. */
 for(unsigned i=0;i<512;i++){
  uint8_t* p=(uint8_t*)q->rx_free.data+i*(model==210?16:8);
  CHECK((model==210?get16(p):(unsigned)(get64(p)&4095))==i+1);
  CHECK((model==210?get64(p+8):get64(p)&~4095ull)==q->rx[i].physical);
  CHECK(((uint8_t*)q->rx[i].data)[4095]==0&&q->available[i]);
 }
}
static void booted(iwx_queue_t* q,device_t* d){
 CHECK(iwx_queue_boot(q)==0&&q->state==IWX_QUEUE_BOOTING&&q->hardware.kicked);
 CHECK(iwx_queue_boot(q)<0);
 d->regs[IWX_CSR_INT/4]|=IWX_CSR_INT_BIT_ALIVE;
 CHECK(iwx_queue_poll(q)==0&&q->rx_enabled);
 CHECK(!d->regs[IWX_CSR_INT/4]);
 CHECK(d->regs[IWX_RFH_Q0_FRBDCB_WIDX_TRG/4]==8);
}
/* Device model consumes the current RX free descriptor and writes a completion.
 * It does not assert compatibility with an untested physical Intel card. */
static unsigned produce(iwx_queue_t* q,device_t* d,uint8_t opcode,uint8_t group,
                         uint8_t index,uint8_t queue,const void* payload,unsigned size,int fragment){
 unsigned slot=get16(q->rx_status.data)&511u;
 uint8_t* free_desc=(uint8_t*)q->rx_free.data+slot*(q->hardware.model==210?16:8);
 unsigned id=q->hardware.model==210?get16(free_desc):(unsigned)(get64(free_desc)&4095u);
 CHECK(id&&id<=512&&size<=4088);
 uint8_t* p=q->rx[id-1].data;memset(p,0,4096);put32(p,size+4);
 p[4]=opcode;p[5]=group;p[6]=index;p[7]=queue;
 if(size)memcpy(p+8,payload,size);
 uint8_t* c=(uint8_t*)q->rx_used.data+slot*(q->hardware.model==210?32:4);
 memset(c,0,q->hardware.model==210?32:4);
 if(q->hardware.model==210){put16(c+4,id);c[6]=(uint8_t)fragment;}else put32(c,id);
 put16(q->rx_status.data,(slot+1)&511u);
 d->regs[IWX_CSR_INT/4]|=IWX_CSR_INT_BIT_FH_RX;
 d->regs[IWX_CSR_FH_INT_STATUS/4]|=IWX_CSR_FH_INT_RX_MASK;
 return id;
}
static unsigned notify_calls;
static int notification(void* owner,const iwx_event_t* event){
 iwx_queue_t* q=owner;CHECK(event->data&&q->polling);
 CHECK(iwx_queue_close(q)<0);
 CHECK(iwx_queue_command(q,0x98,0,NULL,0)<0);
 CHECK(iwx_queue_poll(q)<0);
 notify_calls++;return 0;
}
static void alive_only(iwx_queue_t* q,device_t* d){
 uint8_t payload[144]={0};
 int version=iwx_firmware_command_version(q->firmware,0,1,1);
 unsigned n=version==4?116:version==5?128:144;
 put16(payload,0xcafe);put32(payload+4,77);put32(payload+100,12);put32(payload+116,0x610d0);
 produce(q,d,1,0,0,0x80,payload,n,0);
 CHECK(iwx_queue_poll(q)==1&&q->state==IWX_QUEUE_ALIVE);
 CHECK(q->alive.lmac_major==77&&q->alive.umac_major==12);
 CHECK(!d->regs[IWX_CSR_FH_INT_STATUS/4]);
}
static void pnvm_ack(iwx_queue_t* q,device_t* d){
 uint8_t status[4]={0};produce(q,d,0xfe,12,0,0x80,status,4,0);
 CHECK(iwx_queue_poll(q)==1&&q->pnvm_state==IWX_PNVM_READY);
}
static void alive(iwx_queue_t* q,device_t* d){
 alive_only(q,d);
 if(q->hardware.model==210){
  CHECK(q->pnvm_state==IWX_PNVM_NONE);
  CHECK(iwx_queue_command(q,0x98,0,NULL,0)<0);
  CHECK(iwx_queue_pnvm(q,pnvm_data,pnvm_bytes)==0&&q->pnvm_state==IWX_PNVM_WAITING);
  CHECK(iwx_queue_command(q,0x98,0,NULL,0)<0);
  CHECK(q->boot.pnvm.data&&q->pnvm_version==0xe1c4fa8eu);
  CHECK(d->regs[IWX_HBUS_TARG_PRPH_WADDR/4]==((3u<<24)|0xd05c04u));
  CHECK(d->regs[IWX_HBUS_TARG_PRPH_WDAT/4]==(1u<<20));
  pnvm_ack(q,d);
 }
}
static void closed(iwx_queue_t* q,memory_t* m){
 CHECK(iwx_queue_close(q)==0&&!m->live);
 iwx_queue_t* zero=calloc(1,sizeof(*zero));CHECK(zero);
 CHECK(!memcmp(q,zero,sizeof(*q)));free(zero);
 CHECK(iwx_queue_close(q)==0);
}
static void transport(iwx_queue_t* q,const iwx_firmware_t* fw,int model){
 memory_t m={0};device_t d;prepared(q,fw,model,&m,&d);
 unsigned allocations=m.calls;
 CHECK(iwx_queue_command(q,0x98,0,NULL,0)<0);
 booted(q,&d);q->notify=notification;q->notify_owner=q;alive(q,&d);
 unsigned baseline=m.live;
 CHECK(iwx_queue_command(q,0xff,63,NULL,0)==-2&&m.live==baseline);
 for(unsigned i=0;i<600;i++){
  uint8_t request[23],reply[7];memset(request,(int)i,sizeof(request));memset(reply,0x5a,sizeof(reply));
  CHECK(iwx_queue_command(q,0x98,0,request,sizeof(request))==0);
  CHECK(q->pending&&m.live==baseline+1&&!q->reply_ready);
  CHECK(iwx_queue_command(q,0x98,0,NULL,0)<0&&m.live==baseline+1);
  uint8_t* command=q->pending_dma.data;
  CHECK(command[0]==0x98&&command[1]==1&&command[2]==(uint8_t)i&&command[7]==1);
  CHECK(!memcmp(command+8,request,sizeof(request)));
  CHECK(d.regs[IWX_HBUS_TARG_WRPTR/4]==(model==210?i+1:(uint8_t)(i+1)));
  if(i==0){
   produce(q,&d,0x98,0,1,0,reply,sizeof(reply),0);CHECK(iwx_queue_poll(q)==1&&q->pending&&m.live==baseline+1);
   produce(q,&d,0x99,0,0,0,reply,sizeof(reply),0);CHECK(iwx_queue_poll(q)==1&&q->pending);
   produce(q,&d,0x98,2,0,0,reply,sizeof(reply),0);CHECK(iwx_queue_poll(q)==1&&q->pending);
  }
  produce(q,&d,0x98,(uint8_t)((i%2)?1:0),(uint8_t)i,0,reply,sizeof(reply),0);
  CHECK(iwx_queue_poll(q)==1&&!q->pending&&q->reply_ready&&q->reply_status==0);
  CHECK(m.live==baseline&&q->reply_size==sizeof(reply)&&!memcmp(q->reply,reply,sizeof(reply)));
 }
 CHECK(q->sequence==600&&notify_calls>=601);
 /* Explicit long group must not accept a legacy group-zero completion. */
 CHECK(iwx_queue_command(q,0x98,1,NULL,0)==0);
 produce(q,&d,0x98,0,(uint8_t)600,0,NULL,0,0);
 CHECK(iwx_queue_poll(q)==1&&q->pending);
 produce(q,&d,0x98,0x41,(uint8_t)600,0,NULL,0,0);
 CHECK(iwx_queue_poll(q)==1&&!q->pending&&q->reply_status==-1);
 closed(q,&m);
 /* Exhaustive allocation rollback before hardware gets any DMA addresses. */
 for(unsigned bad=0;bad<2;bad++)for(unsigned failure=1;failure<=allocations;failure++){
  memset(&m,0,sizeof(m));init(&d,model);
  if(bad)m.bad=failure;else m.fail=failure;
  CHECK(iwx_queue_prepare(q,fw,model,&dma,&m,&io,&d)<0);
  CHECK(!m.live&&q->state==IWX_QUEUE_EMPTY&&!q->rx_allocated&&d.operations<=1);
 }
}
static void boundaries(iwx_queue_t* q,const iwx_firmware_t* fw,int model){
 memory_t m={0};device_t d;prepared(q,fw,model,&m,&d);booted(q,&d);alive(q,&d);
 uint8_t full[IWX_COMMAND_MAX];memset(full,0x96,sizeof(full));
 q->sequence=65535;
 CHECK(iwx_queue_command(q,0x98,0,full,sizeof(full))==0&&q->sequence==0);
 CHECK(d.regs[IWX_HBUS_TARG_WRPTR/4]==0);
 produce(q,&d,0x98,0,255,0,full,sizeof(full),0);
 CHECK(iwx_queue_poll(q)==1&&!q->pending&&q->reply_size==sizeof(full));
 CHECK(!memcmp(q->reply,full,sizeof(full)));
 CHECK(iwx_queue_command(q,0x98,0,full,sizeof(full)+1)<0&&!q->pending);
 CHECK(iwx_queue_command(q,0x98,0,NULL,1)<0&&!q->pending);
 /* Several completions in one poll publish only after every buffer is safe. */
 for(unsigned i=0;i<17;i++)produce(q,&d,2,0,0,0x80,NULL,0,0);
 CHECK(iwx_queue_poll(q)==17);
 CHECK(d.regs[IWX_RFH_Q0_FRBDCB_WIDX_TRG/4]==((q->write-1u)&~7u));
 closed(q,&m);
 /* Every I/O failure in poll retains the ring until stop, including ALIVE. */
 for(unsigned alive_irq=0;alive_irq<2;alive_irq++){
  prepared(q,fw,model,&m,&d);booted(q,&d);alive(q,&d);
  produce(q,&d,2,0,0,0x80,NULL,0,0);
  if(alive_irq){q->rx_enabled=0;d.regs[IWX_CSR_INT/4]|=IWX_CSR_INT_BIT_ALIVE;}
  unsigned before=d.operations;CHECK(iwx_queue_poll(q)==1);unsigned operations=d.operations-before;
  closed(q,&m);
  for(unsigned step=1;step<=operations;step++){
   prepared(q,fw,model,&m,&d);booted(q,&d);alive(q,&d);
   produce(q,&d,2,0,0,0x80,NULL,0,0);
   if(alive_irq){q->rx_enabled=0;d.regs[IWX_CSR_INT/4]|=IWX_CSR_INT_BIT_ALIVE;}
   unsigned retained=m.live;d.fail=d.operations+step;
   CHECK(iwx_queue_poll(q)<0&&q->state==IWX_QUEUE_FAILED&&m.live==retained);
   d.fail=0;closed(q,&m);
  }
 }
}
static void faults(iwx_queue_t* q,const iwx_firmware_t* fw,int model){
 memory_t m={0};device_t d;
 for(unsigned which=0;which<11;which++){
  prepared(q,fw,model,&m,&d);booted(q,&d);
  if(which==0){
   /* Failed ALIVE status cannot unlock command submission. */
   uint8_t bad[144]={0};produce(q,&d,1,0,0,0x80,bad,sizeof(bad),0);
  }else if(which==1||which==2){
   alive(q,&d);produce(q,&d,2,0,0,0x80,NULL,0,0);
   unsigned slot=q->read;uint8_t* c=(uint8_t*)q->rx_used.data+slot*(model==210?32:4);
   if(model==210)put16(c+4,which==1?0:513);else put32(c,which==1?0:513);
  }else if(which==3){
   alive(q,&d);unsigned slot=q->read;
   unsigned id=produce(q,&d,2,0,0,0x80,NULL,0,0);
   produce(q,&d,2,0,0,0x80,NULL,0,0);
   uint8_t* second=(uint8_t*)q->rx_used.data+((slot+1)&511)*(model==210?32:4);
   if(model==210)put16(second+4,id);else put32(second,id);
  }else if(which==4){
   alive(q,&d);unsigned id=produce(q,&d,2,0,0,0x80,NULL,0,0);
   put32(q->rx[id-1].data,4096); /* Four bytes beyond available payload. */
  }else if(which==5){
   alive(q,&d);d.regs[IWX_CSR_INT/4]|=IWX_CSR_INT_BIT_HW_ERR;
  }else if(which==6){
   alive(q,&d);d.unplug=1;
  }else if(which==7){
   alive(q,&d);d.badwrite=IWX_HBUS_TARG_WRPTR;
   CHECK(iwx_queue_command(q,0x98,0,NULL,0)<0&&q->pending&&q->pending_dma.data);
  }else if(which==8){
   alive(q,&d);produce(q,&d,2,0,0,0x80,NULL,0,0);
   d.badwrite=IWX_RFH_Q0_FRBDCB_WIDX_TRG;
  }else if(which==9){
   alive(q,&d);d.rfkill=1;d.regs[IWX_CSR_INT/4]|=IWX_CSR_INT_BIT_RF_KILL;
  }else{
   alive(q,&d);d.regs[IWX_CSR_INT/4]=0xa5a5a5a0u;
  }
  CHECK(iwx_queue_poll(q)<0&&q->state==IWX_QUEUE_FAILED);
  unsigned retained=m.live;CHECK(retained>512);
  d.badwrite=0xffffffffu;d.unplug=0;d.stopfail=1;
  CHECK(iwx_queue_close(q)<0&&m.live==retained&&q->hardware.kicked);
  d.stopfail=0;closed(q,&m);
 }
 if(model==210){
  prepared(q,fw,model,&m,&d);booted(q,&d);alive(q,&d);
  produce(q,&d,2,0,0,0x80,NULL,0,1);
  CHECK(iwx_queue_poll(q)==1&&q->rx_dropped==1&&q->state==IWX_QUEUE_ALIVE);
  closed(q,&m);
 }
 /* Doorbell/interrupt I/O errors retain buffers until a proven stop. */
 prepared(q,fw,model,&m,&d);booted(q,&d);alive(q,&d);
 unsigned baseline=m.live;m.fail=m.calls+1;
 CHECK(iwx_queue_command(q,0x98,0,NULL,0)<0&&m.live==baseline&&!q->pending);
 m.fail=0;CHECK(iwx_queue_command(q,0x98,0,NULL,0)==0);
 produce(q,&d,0x98,0,0,0,NULL,0,0);d.fail=d.operations+1;
 CHECK(iwx_queue_poll(q)<0&&q->pending&&q->pending_dma.data);
 d.fail=0;closed(q,&m);
}
static void calibration(iwx_queue_t* q,const iwx_firmware_t* fw,int model){
 memory_t m={0};device_t d;
 if(model==200){
  prepared(q,fw,model,&m,&d);booted(q,&d);alive_only(q,&d);
  CHECK(iwx_queue_pnvm(q,pnvm_data,pnvm_bytes)<0&&q->pnvm_state==IWX_PNVM_READY);
  closed(q,&m);return;
 }
 /* No selected SKU means firmware does not require a PNVM handoff. */
 prepared(q,fw,model,&m,&d);booted(q,&d);alive_only(q,&d);
 memset(q->alive.sku,0,sizeof(q->alive.sku));unsigned before=d.operations,base=m.live;
 CHECK(iwx_queue_pnvm(q,NULL,0)==0&&q->pnvm_state==IWX_PNVM_READY);
 CHECK(before==d.operations&&base==m.live);closed(q,&m);
 /* Unknown board/radio and truncated data cannot publish any calibration. */
 prepared(q,fw,model,&m,&d);booted(q,&d);alive_only(q,&d);base=m.live;
 q->alive.sku[0]^=1;CHECK(iwx_queue_pnvm(q,pnvm_data,pnvm_bytes)==-2);
 CHECK(m.live==base&&!q->boot.pnvm.data&&q->state==IWX_QUEUE_ALIVE);
 q->alive.sku[0]^=1;d.regs[IWX_CSR_HW_RF_ID/4]^=0x1000;
 CHECK(iwx_queue_pnvm(q,pnvm_data,pnvm_bytes)==-2&&m.live==base);
 d.regs[IWX_CSR_HW_RF_ID/4]^=0x1000;
 CHECK(iwx_queue_pnvm(q,pnvm_data,7)<0&&m.live==base&&!q->boot.pnvm.data);
 CHECK(iwx_queue_pnvm(q,pnvm_data,pnvm_bytes)==0);
 unsigned pnvm_operations;
 pnvm_ack(q,&d);closed(q,&m);
 /* A notification cannot finish an unsolicited, duplicate, short or failed handoff. */
 for(unsigned which=0;which<4;which++){
  prepared(q,fw,model,&m,&d);booted(q,&d);alive_only(q,&d);
  if(which!=0){CHECK(!iwx_queue_pnvm(q,pnvm_data,pnvm_bytes));}
  if(which==1)pnvm_ack(q,&d);
  uint8_t status[4]={0};if(which==3)put32(status,1);
  produce(q,&d,0xfe,12,0,0x80,status,which==2?3:4,0);
  CHECK(iwx_queue_poll(q)<0&&q->state==IWX_QUEUE_FAILED);
  if(which==3)CHECK(q->pnvm_status==1);
  closed(q,&m);
 }
 /* Both supported PNVM allocation layouts, every failed allocation position. */
 for(unsigned fragmented=0;fragmented<2;fragmented++){
  iwx_firmware_t variant=*fw;
  if(fragmented)variant.capabilities[1]|=1;else variant.capabilities[1]&=~1u;
  prepared(q,&variant,model,&m,&d);booted(q,&d);alive_only(q,&d);
  unsigned calls=m.calls;CHECK(!iwx_queue_pnvm(q,pnvm_data,pnvm_bytes));
  unsigned allocations=m.calls-calls;CHECK(allocations==(fragmented?3u:1u));
  pnvm_ack(q,&d);closed(q,&m);
  for(unsigned bad=0;bad<2;bad++)for(unsigned at=1;at<=allocations;at++){
   prepared(q,&variant,model,&m,&d);booted(q,&d);alive_only(q,&d);base=m.live;
   unsigned blocks=q->boot.nblocks;
   if(bad)m.bad=m.calls+at;else m.fail=m.calls+at;
   CHECK(iwx_queue_pnvm(q,pnvm_data,pnvm_bytes)<0);
   CHECK(q->state==IWX_QUEUE_ALIVE&&q->pnvm_state==IWX_PNVM_NONE&&m.live==base);
   CHECK(q->boot.nblocks==blocks&&!q->boot.pnvm.data);
   m.bad=m.fail=0;CHECK(!iwx_queue_pnvm(q,pnvm_data,pnvm_bytes));
   pnvm_ack(q,&d);closed(q,&m);
  }
 }
 /* Inject each failed register operation; uncertain handoff keeps DMA pinned. */
 prepared(q,fw,model,&m,&d);booted(q,&d);alive_only(q,&d);
 before=d.operations;CHECK(!iwx_queue_pnvm(q,pnvm_data,pnvm_bytes));
 pnvm_operations=d.operations-before;pnvm_ack(q,&d);closed(q,&m);
 for(unsigned at=1;at<=pnvm_operations;at++){
  prepared(q,fw,model,&m,&d);booted(q,&d);alive_only(q,&d);
  d.fail=d.operations+at;
  CHECK(iwx_queue_pnvm(q,pnvm_data,pnvm_bytes)<0&&q->state==IWX_QUEUE_FAILED);
  unsigned retained=m.live;d.fail=0;d.stopfail=1;
  CHECK(iwx_queue_close(q)<0&&m.live==retained);
  d.stopfail=0;closed(q,&m);
 }
 /* Missing completion is not permission to send; closing safely retains until stop. */
 prepared(q,fw,model,&m,&d);booted(q,&d);alive_only(q,&d);
 CHECK(!iwx_queue_pnvm(q,pnvm_data,pnvm_bytes));
 CHECK(iwx_queue_pnvm(q,pnvm_data,pnvm_bytes)<0);
 CHECK(iwx_queue_poll(q)==0&&q->pnvm_state==IWX_PNVM_WAITING);
 CHECK(iwx_queue_command(q,0x98,0,NULL,0)<0);
 d.stopfail=1;base=m.live;CHECK(iwx_queue_close(q)<0&&base==m.live);
 d.stopfail=0;closed(q,&m);
}
int main(int argc,char** argv){
 CHECK(argc==4);int model=atoi(argv[2]);FILE* f=fopen(argv[1],"rb");CHECK(f);
 CHECK(!fseek(f,0,SEEK_END));long length=ftell(f);CHECK(length>0&&length<8000000);rewind(f);
 uint8_t* data=malloc((size_t)length);CHECK(data);CHECK(fread(data,1,(size_t)length,f)==(size_t)length);CHECK(!fclose(f));
 f=fopen(argv[3],"rb");CHECK(f);CHECK(!fseek(f,0,SEEK_END));long pn=ftell(f);CHECK(pn>0&&pn<1000000);rewind(f);
 pnvm_bytes=(unsigned)pn;pnvm_data=malloc(pnvm_bytes);CHECK(pnvm_data);
 CHECK(fread(pnvm_data,1,pnvm_bytes,f)==pnvm_bytes);CHECK(!fclose(f));
 iwx_firmware_t fw;CHECK(!iwx_firmware_parse(&fw,data,(uint32_t)length,model));
 iwx_queue_t* q=calloc(1,sizeof(*q));CHECK(q);
 transport(q,&fw,model);boundaries(q,&fw,model);faults(q,&fw,model);calibration(q,&fw,model);free(q);free(data);free(pnvm_data);
 printf("Intel AX%d queue model: %u checks passed\n",model,checks);return 0;
}
