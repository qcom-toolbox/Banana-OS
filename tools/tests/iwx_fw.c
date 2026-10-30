/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../net/iwx_boot.h"
#include "../../net/iwx_hw.h"
#include "../../third_party/iwx/iwx_boot_regs.h"
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"iwx firmware FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void views(const iwx_firmware_t* fw,const uint8_t* b,uint32_t n){
 CHECK(fw->api==77&&fw->nsections<=IWX_FW_SECTIONS&&fw->ncommands<=IWX_FW_COMMANDS);
 for(uint32_t i=0;i<fw->nsections;i++){
  const iwx_fw_section_t* s=&fw->sections[i];
  CHECK(s->data>=b&&s->data<=b+n);CHECK(s->size<=n-(uint32_t)(s->data-b));
 }
 if(fw->iml_size){CHECK(fw->iml>=b&&fw->iml<=b+n);CHECK(fw->iml_size<=n-(uint32_t)(fw->iml-b));}
}
static unsigned dma_live,dma_calls,dma_serial,fail_call,bad_address;
static int dma_alloc(void* owner,uint32_t n,uint32_t align,iwx_dma_t* d){
 (void)owner;CHECK(align==4096);dma_calls++;if(dma_calls==fail_call)return -1;
 d->size=(n+4095u)&~4095u;d->data=malloc(d->size);CHECK(d->data);d->physical=0x1000000u+(uint64_t)(++dma_serial)*65536;dma_live++;
 if(bad_address==dma_calls)d->physical=0x100000000ull;
 return 0;
}
static void dma_release(void* owner,iwx_dma_t* d){(void)owner;CHECK(d->data&&dma_live);dma_live--;free(d->data);}
typedef struct {uint32_t regs[2048],paddr,raddr,pvalue;unsigned operations,fail,delay;int clockfail,stopfail,unplug,rfkill,kicks;} io_model_t;
static int io_read(void* owner,uint32_t reg,uint32_t* value){
 io_model_t* m=owner;if(++m->operations==m->fail)return -1;CHECK(!(reg&3)&&reg<sizeof(m->regs));
 if(m->unplug){*value=0xffffffffu;return 0;}
 if(reg==IWX_CSR_GP_CNTRL){
  if((m->regs[reg/4]&IWX_CSR_GP_CNTRL_REG_FLAG_INIT_DONE)&&!m->clockfail)m->regs[reg/4]|=IWX_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY;
  if(!m->rfkill)m->regs[reg/4]|=IWX_CSR_GP_CNTRL_REG_FLAG_HW_RF_KILL_SW;
  else m->regs[reg/4]&=~IWX_CSR_GP_CNTRL_REG_FLAG_HW_RF_KILL_SW;
 }
 *value=reg==IWX_HBUS_TARG_PRPH_RDAT?0:m->regs[reg/4];return 0;
}
static int io_write(void* owner,uint32_t reg,uint32_t value){
 io_model_t* m=owner;if(++m->operations==m->fail)return -1;CHECK(!(reg&3)&&reg<sizeof(m->regs));
 m->regs[reg/4]=value;
 if(reg==IWX_CSR_RESET&&(value&IWX_CSR_RESET_REG_FLAG_STOP_MASTER)&&!m->stopfail)m->regs[reg/4]|=IWX_CSR_RESET_REG_FLAG_MASTER_DISABLED;
 if(reg==IWX_HBUS_TARG_PRPH_WADDR)m->paddr=value&0xffffffu;
 if(reg==IWX_HBUS_TARG_PRPH_WDAT&&(m->paddr==(IWX_UREG_CPU_INIT_RUN&0xfffffu)||m->paddr==IWX_UREG_CPU_INIT_RUN+0x300000u))m->kicks++;
 return 0;
}
static void io_delay(void* owner,unsigned us){io_model_t* m=owner;m->delay+=us;CHECK(m->delay<1000000);}
static const iwx_io_ops_t io_ops={io_read,io_write,io_delay};
static void reset_io(io_model_t* io,int model){
 memset(io,0,sizeof(*io));
 io->regs[IWX_CSR_HW_REV/4]=model==200?0x340u:0x420u;
}
static void hardware(const iwx_boot_t* b,const iwx_firmware_t* fw,int model){
 io_model_t io;reset_io(&io,model);iwx_hw_t h={.io=&io_ops,.owner=&io,.model=model};
 CHECK(iwx_hw_start(&h)==0&&h.started&&!h.locked);
 CHECK(iwx_hw_kick(&h,b,fw)==0&&h.kicked&&!h.locked&&io.kicks==1);
 CHECK(io.regs[IWX_CSR_INT_MASK/4]==0);
 if(model==210)CHECK(io.regs[IWX_CSR_IML_SIZE_ADDR/4]==fw->iml_size&&b->iml.size>fw->iml_size);
 CHECK(iwx_hw_kick(&h,b,fw)<0&&io.kicks==1);
 unsigned operations=io.operations;
 io.stopfail=1;CHECK(iwx_hw_stop(&h)<0&&h.kicked);
 io.stopfail=0;CHECK(iwx_hw_stop(&h)==0&&!h.kicked&&!h.started);
 for(unsigned failure=1;failure<=operations;failure++){
  reset_io(&io,model);io.fail=failure;
  h=(iwx_hw_t){.io=&io_ops,.owner=&io,.model=model};
  int rc=iwx_hw_start(&h);if(!rc)rc=iwx_hw_kick(&h,b,fw);
  CHECK(rc<0||(h.kicked&&io.kicks==1)); /* Hardware-ready probing may recover from one transient read. */
  io.fail=0;CHECK(iwx_hw_stop(&h)==0);
 }
 reset_io(&io,model);io.clockfail=1;
 h=(iwx_hw_t){.io=&io_ops,.owner=&io,.model=model};
 CHECK(iwx_hw_start(&h)<0&&!h.kicked&&io.delay<1000000);
 reset_io(&io,model);io.rfkill=1;
 h=(iwx_hw_t){.io=&io_ops,.owner=&io,.model=model};
 CHECK(iwx_hw_start(&h)==-2&&!h.kicked&&io.kicks==0);
 reset_io(&io,model);io.unplug=1;
 h=(iwx_hw_t){.io=&io_ops,.owner=&io,.model=model};
 CHECK(iwx_hw_start(&h)<0&&!h.kicked);
}
static void bootstrap(const iwx_firmware_t* fw,int model,const iwx_pnvm_t* pnvm){
 iwx_dma_ops_t ops={dma_alloc,dma_release};
 iwx_boot_rings_t rings={.rx_free=0x100000,.rx_used=0x102000,.rx_status=0x104000,.command=0x200000,.rx_count=512,.command_count=256,.mac_id=0x1234};
 iwx_boot_t b={0};dma_calls=0;CHECK(iwx_boot_prepare(&b,fw,model,&rings,&ops,NULL)==0);
 CHECK(b.nblocks==(model==200?49u:58u)&&dma_live==b.nblocks);
 CHECK(b.lmac&&b.umac&&b.paging);
 if(model==200){
  struct iwx_context_info* c=b.context.data;
  CHECK(c->version.mac_id==0x1234&&c->version.size==sizeof(*c)/4);
  CHECK(c->rbd_cfg.free_rbd_addr==rings.rx_free&&c->hcmd_cfg.cmd_queue_addr==rings.command);
 }else{
  struct iwx_context_info_gen3* c=b.context.data;
  struct iwx_prph_scratch* s=b.scratch.data;
  CHECK(c->prph_scratch_base_addr==b.scratch.physical&&c->prph_info_base_addr==b.peripheral.physical);
  CHECK(s->ctrl_cfg.rbd_cfg.free_rbd_addr==rings.rx_free);
  CHECK(!memcmp(b.iml.data,fw->iml,fw->iml_size));
  CHECK(c->tr_tail_idx_arr_base_addr==b.peripheral.physical+2048);
 }
 const struct iwx_context_info_dram* dram;
 if(model==200)dram=&((struct iwx_context_info*)b.context.data)->dram;
 else dram=&((struct iwx_prph_scratch*)b.scratch.data)->dram;
 unsigned group=0,index=0;
 for(uint32_t i=0;i<fw->nsections;i++){
  const iwx_fw_section_t* sec=&fw->sections[i];
  if(!sec->size){group++;index=0;continue;}
  size_t offset=group==0?__builtin_offsetof(struct iwx_context_info_dram,lmac_img):
                group==1?__builtin_offsetof(struct iwx_context_info_dram,umac_img):
                         __builtin_offsetof(struct iwx_context_info_dram,virtual_img);
  uint64_t physical;memcpy(&physical,(const uint8_t*)dram+offset+8*index++,8);
  int found=0;
  for(uint32_t j=0;j<b.nblocks;j++)if(b.blocks[j].physical==physical){
   CHECK(b.blocks[j].size>=sec->size&&!memcmp(b.blocks[j].data,sec->data,sec->size));found=1;break;
  }
  CHECK(found);
 }
 hardware(&b,fw,model);
 if(pnvm){
  for(unsigned fragmented=0;fragmented<2;fragmented++){
   iwx_firmware_t variant=*fw;
   if(fragmented)variant.capabilities[1]|=1u;else variant.capabilities[1]&=~1u;
   unsigned base=b.nblocks;
   struct iwx_prph_scratch* scratch=b.scratch.data;
   for(unsigned i=1;i<=(fragmented?3u:1u);i++){
    dma_calls=0;fail_call=i;
    CHECK(iwx_boot_attach_pnvm(&b,&variant,pnvm,&ops,NULL)<0);
    CHECK(b.nblocks==base&&dma_live==base&&!b.pnvm.data&&!scratch->ctrl_cfg.pnvm_cfg.pnvm_base_addr);
   }
   fail_call=0;
   for(unsigned i=1;i<=(fragmented?3u:1u);i++){
    dma_calls=0;bad_address=i;
    CHECK(iwx_boot_attach_pnvm(&b,&variant,pnvm,&ops,NULL)<0);
    CHECK(b.nblocks==base&&dma_live==base&&!b.pnvm.data);
   }
   bad_address=0;dma_calls=0;
   CHECK(iwx_boot_attach_pnvm(&b,&variant,pnvm,&ops,NULL)==0);
   CHECK(b.nblocks==base+(fragmented?3u:1u)&&b.pnvm_bytes==pnvm->size);
   CHECK(scratch->ctrl_cfg.pnvm_cfg.pnvm_base_addr==b.pnvm.physical);
   CHECK(scratch->ctrl_cfg.pnvm_cfg.pnvm_size==pnvm->size);
   CHECK(iwx_boot_attach_pnvm(&b,&variant,pnvm,&ops,NULL)<0&&b.nblocks==base+(fragmented?3u:1u));
   uint32_t data_offset=0;
   for(uint32_t i=0;i<pnvm->nsegments;i++){
    const uint8_t* actual;
    if(fragmented){
     uint64_t physical;memcpy(&physical,(uint8_t*)b.pnvm.data+8*i,8);
     actual=NULL;
     for(uint32_t j=base;j<b.nblocks;j++)if(b.blocks[j].physical==physical)actual=b.blocks[j].data;
     CHECK(actual);
    }else{actual=(const uint8_t*)b.pnvm.data+data_offset;data_offset+=pnvm->segments[i].size;}
    CHECK(!memcmp(actual,pnvm->segments[i].data,pnvm->segments[i].size));
   }
   iwx_boot_release(&b,&ops,NULL);CHECK(dma_live==0);
   dma_calls=0;CHECK(iwx_boot_prepare(&b,fw,model,&rings,&ops,NULL)==0);
  }
 }
 unsigned count=dma_calls;
 if(pnvm)count=model==200?49u:58u; /* only bootstrap allocations in the following fault sweep */
 iwx_boot_release(&b,&ops,NULL);CHECK(!dma_live&&!b.nblocks);
 for(unsigned i=1;i<=count;i++){
  dma_calls=0;fail_call=i;CHECK(iwx_boot_prepare(&b,fw,model,&rings,&ops,NULL)<0);
  CHECK(!dma_live&&!b.nblocks);
 }
 fail_call=0;
 for(unsigned i=1;i<=count;i++){
  dma_calls=0;bad_address=i;CHECK(iwx_boot_prepare(&b,fw,model,&rings,&ops,NULL)<0);
  CHECK(!dma_live&&!b.nblocks);
 }
 bad_address=0;dma_calls=0;rings.rx_free=0x100000000ull;
 CHECK(iwx_boot_prepare(&b,fw,model,&rings,&ops,NULL)<0&&dma_calls==0);
 rings.rx_free=0x100001;CHECK(iwx_boot_prepare(&b,fw,model,&rings,&ops,NULL)<0&&dma_calls==0);
}
int main(int argc,char** argv){
 CHECK(argc==4);int model=atoi(argv[2]);FILE* f=fopen(argv[1],"rb");CHECK(f);
 CHECK(!fseek(f,0,SEEK_END));long z=ftell(f);CHECK(z>0&&z<8*1024*1024);
 rewind(f);uint32_t n=(uint32_t)z;uint8_t* b=malloc(n);CHECK(b&&fread(b,1,n,f)==n);fclose(f);
 iwx_firmware_t fw;int rc=iwx_firmware_parse(&fw,b,n,model);
 if(rc)fprintf(stderr,"model %d parse returned %d\n",model,rc);
 CHECK(rc==0);views(&fw,b,n);
 iwx_pnvm_t pnvm,*selected=NULL;
 uint8_t* platform=NULL;
 if(model==210){
  FILE* pf=fopen(argv[3],"rb");CHECK(pf);CHECK(!fseek(pf,0,SEEK_END));long pn=ftell(pf);CHECK(pn>0&&pn<1024*1024);rewind(pf);
  platform=malloc((size_t)pn);CHECK(platform&&fread(platform,1,(size_t)pn,pf)==(size_t)pn);fclose(pf);
  const uint32_t skus[4]={0x610d0,0x510d0,0x4510d0,0x4610d0};
  const uint32_t sizes[4]={13652,13704,13704,13656};
  for(unsigned i=0;i<4;i++){
   uint32_t sku[3]={skus[i],0,0};
   CHECK(iwx_pnvm_select(&pnvm,platform,(uint32_t)pn,sku,0x42,0x010d)==0);
   CHECK(pnvm.nsegments==2&&pnvm.size==sizes[i]&&pnvm.version==0xe1c4fa8e);
   CHECK(pnvm.segments[0].size==1656&&pnvm.segments[0].tag==0xcafedead);
   CHECK(pnvm.segments[1].tag==0xdeadcafe);
   for(unsigned j=0;j<2;j++)CHECK(pnvm.segments[j].data>=platform&&pnvm.segments[j].data+pnvm.segments[j].size<=platform+pn);
  }
  uint32_t sku[3]={skus[0],0,0};
  CHECK(iwx_pnvm_select(&pnvm,platform,(uint32_t)pn,sku,0x42,0x999)==-2);
  CHECK(iwx_pnvm_select(&pnvm,platform,(uint32_t)pn,sku,0x42,0x010d)==0);
  uint8_t* broken=malloc((size_t)pn);CHECK(broken);
  for(uint32_t pos=0;pos<(uint32_t)pn;){
   uint32_t len=(uint32_t)platform[pos+4]|(uint32_t)platform[pos+5]<<8|(uint32_t)platform[pos+6]<<16|(uint32_t)platform[pos+7]<<24;
   memcpy(broken,platform,(size_t)pn);memset(broken+pos+4,0xff,4);
   CHECK(iwx_pnvm_select(&pnvm,broken,(uint32_t)pn,sku,0x42,0x010d)<0);
   CHECK(iwx_pnvm_select(&pnvm,platform,pos+4,sku,0x42,0x010d)<0);
   pos+=8+((len+3u)&~3u);
  }
  free(broken);
  CHECK(iwx_pnvm_select(&pnvm,platform,(uint32_t)pn,sku,0x42,0x010d)==0);
  selected=&pnvm;
 }
 bootstrap(&fw,model,selected);free(platform);
 CHECK(fw.nsections==(model==200?50u:56u));
 CHECK(fw.ncommands==(model==200?192u:191u));
 CHECK((model==210)==(fw.iml_size!=0));
 CHECK(iwx_firmware_command_version(&fw,0xff,0xff,0)<0);
 for(uint32_t i=0;i<fw.ncommands;i++){
  iwx_fw_command_t* c=&fw.commands[i];
  int duplicate=0;for(uint32_t j=0;j<i;j++)if(fw.commands[j].group==c->group&&fw.commands[j].command==c->command)duplicate=1;
  if(duplicate)continue; /* Upstream tables contain command/notification aliases; first match wins. */
  CHECK(iwx_firmware_command_version(&fw,c->group,c->command,0)==c->command_version);
  CHECK(iwx_firmware_command_version(&fw,c->group,c->command,1)==c->notification_version);
 }
 uint8_t* changed=malloc(n);CHECK(changed);unsigned random=0x1234;
 for(int i=0;i<3000;i++){
  memcpy(changed,b,n);
  random=random*1664525u+1013904223u;
  uint32_t at=(i<88)?(uint32_t)i:random%n;
  changed[at]^=(uint8_t)(1u<<(random%8));
  rc=iwx_firmware_parse(&fw,changed,n,model);
  if(!rc)views(&fw,changed,n);
  else {iwx_firmware_t zero={0};CHECK(!memcmp(&fw,&zero,sizeof(fw)));}
 }
 for(uint32_t pos=88;pos<n;){
  uint32_t length=(uint32_t)b[pos+4]|(uint32_t)b[pos+5]<<8|(uint32_t)b[pos+6]<<16|(uint32_t)b[pos+7]<<24;
  memcpy(changed,b,n);memset(changed+pos+4,0xff,4);
  CHECK(iwx_firmware_parse(&fw,changed,n,model)<0);
  pos+=8+((length+3u)&~3u);
 }
 CHECK(iwx_firmware_parse(&fw,b,n,model==200?210:200)<0);
 for(uint32_t cut=0;cut<96;cut++){CHECK(iwx_firmware_parse(&fw,b,cut,model)<0);}
 CHECK(iwx_firmware_parse(&fw,NULL,n,model)<0);
 CHECK(iwx_firmware_parse(&fw,b,n,999)==-2);
 free(changed);free(b);
 printf("Intel AX%d firmware: %u checks passed\n",model,checks);return 0;
}
