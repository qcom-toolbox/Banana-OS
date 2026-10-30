/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Native construction of Intel firmware self-load contexts; layout and
 * field assignments follow the pinned OpenBSD iwx reference. */
#include "iwx_boot.h"
#include "../third_party/iwx/iwx_boot_regs.h"
#include "../kernel/kstring.h"
_Static_assert(sizeof(struct iwx_context_info)==1792,"AX200 context ABI");
_Static_assert(sizeof(struct iwx_prph_scratch)==1660,"AX210 scratch ABI");
_Static_assert(sizeof(struct iwx_context_info_gen3)==104,"AX210 context ABI");
_Static_assert(__builtin_offsetof(struct iwx_context_info,dram)==192,"AX200 DRAM ABI");
_Static_assert(__builtin_offsetof(struct iwx_prph_scratch,dram)==124,"AX210 DRAM ABI");
static int address(uint64_t p,uint32_t n,uint32_t alignment) {
    return p && !(p&(alignment-1u)) && n && p<=0xffffffffu &&
           n-1u<=0xffffffffu-p;
}
static int block(iwx_boot_t* b,const iwx_dma_ops_t* ops,void* owner,
                 uint32_t n,iwx_dma_t* out) {
    if (!n || b->nblocks>=IWX_BOOT_BLOCKS) return -1;
    iwx_dma_t d={0};
    if (ops->alloc(owner,n,4096,&d)!=0) return -1;
    if (!d.data || d.size<n || d.size>8u*1024u*1024u || !address(d.physical,d.size,4096)) {
        ops->release(owner,&d);
        return -1;
    }
    b->blocks[b->nblocks++]=d;
    memset(d.data,0,d.size);
    *out=d;
    return 0;
}
void iwx_boot_release(iwx_boot_t* b,const iwx_dma_ops_t* ops,void* owner) {
    if (!b || !ops || !ops->release || b->nblocks>IWX_BOOT_BLOCKS) return;
    while (b->nblocks) ops->release(owner,&b->blocks[--b->nblocks]);
    memset(b,0,sizeof(*b));
}
static int power_of_two(uint32_t n) { return n && !(n&(n-1u)); }
static unsigned exponent(uint32_t n) { unsigned v=0;while(n>1){v++;n>>=1;}return v; }
int iwx_boot_prepare(iwx_boot_t* b,const iwx_firmware_t* fw,int model,
                     const iwx_boot_rings_t* r,const iwx_dma_ops_t* ops,void* owner) {
    if (!b || b->nblocks || !fw || !r || !ops || !ops->alloc || !ops->release) return -1;
    if ((model!=IWX_FW_AX200 && model!=IWX_FW_AX210) || fw->api!=77 ||
        !fw->nsections || fw->nsections>IWX_FW_SECTIONS ||
        (fw->hardware[0]&0xffffu)!=(model==IWX_FW_AX200?0x34u:0x42u)) return -1;
    if (!power_of_two(r->rx_count) || r->rx_count<8 || r->rx_count>512 ||
        !power_of_two(r->command_count) || r->command_count<8 || r->command_count>256 ||
        !address(r->rx_free,r->rx_count*(model==IWX_FW_AX210?16u:8u),4096) ||
        !address(r->rx_used,r->rx_count*(model==IWX_FW_AX210?32u:4u),4096) ||
        !address(r->rx_status,64,4096) ||
        !address(r->command,r->command_count*256,4096)) return -1;
    if (model==IWX_FW_AX210 && (!fw->iml || !fw->iml_size || fw->iml_size>256u*1024u)) return -1;
    struct iwx_context_info_dram* dram;
    if (model==IWX_FW_AX200) {
        if (block(b,ops,owner,sizeof(struct iwx_context_info),&b->context)) goto failed;
        struct iwx_context_info* c=b->context.data;
        c->version.mac_id=r->mac_id;c->version.size=sizeof(*c)/4;
        c->control.control_flags=IWX_CTXT_INFO_TFD_FORMAT_LONG |
            exponent(r->rx_count)<<IWX_CTXT_INFO_RB_CB_SIZE_POS |
            IWX_CTXT_INFO_RB_SIZE_4K<<IWX_CTXT_INFO_RB_SIZE_POS;
        c->rbd_cfg.free_rbd_addr=r->rx_free;c->rbd_cfg.used_rbd_addr=r->rx_used;
        c->rbd_cfg.status_wr_ptr=r->rx_status;
        c->hcmd_cfg.cmd_queue_addr=r->command;c->hcmd_cfg.cmd_queue_size=exponent(r->command_count)-3;
        dram=&c->dram;
    } else {
        if (block(b,ops,owner,sizeof(struct iwx_context_info_gen3),&b->context) ||
            block(b,ops,owner,sizeof(struct iwx_prph_scratch),&b->scratch) ||
            block(b,ops,owner,4096,&b->peripheral) ||
            block(b,ops,owner,fw->iml_size,&b->iml)) goto failed;
        memcpy(b->iml.data,fw->iml,fw->iml_size);
        b->iml_bytes=fw->iml_size;
        struct iwx_prph_scratch* s=b->scratch.data;
        s->ctrl_cfg.version.mac_id=r->mac_id;s->ctrl_cfg.version.size=sizeof(*s)/4;
        s->ctrl_cfg.control.control_flags=IWX_PRPH_SCRATCH_RB_SIZE_4K |
            IWX_PRPH_SCRATCH_MTR_MODE | IWX_PRPH_MTR_FORMAT_256B;
        s->ctrl_cfg.rbd_cfg.free_rbd_addr=r->rx_free;
        struct iwx_context_info_gen3* c=b->context.data;
        c->prph_info_base_addr=b->peripheral.physical;
        c->prph_scratch_base_addr=b->scratch.physical;c->prph_scratch_size=sizeof(*s);
        c->cr_head_idx_arr_base_addr=r->rx_status;
        c->tr_tail_idx_arr_base_addr=b->peripheral.physical+2048;
        c->cr_tail_idx_arr_base_addr=b->peripheral.physical+3072;
        c->mtr_base_addr=r->command;c->mcr_base_addr=r->rx_used;
        c->mtr_size=exponent(r->command_count)-3;c->mcr_size=exponent(r->rx_count);
        dram=&s->dram;
    }
    unsigned group=0,count[3]={0};
    for (uint32_t i=0;i<fw->nsections;i++) {
        const iwx_fw_section_t* s=&fw->sections[i];
        if (s->offset==IWX_FW_CPU_SEPARATOR || s->offset==IWX_FW_PAGING_SEPARATOR) {
            if (s->size || !count[group] ||
                (group==0 && s->offset!=IWX_FW_CPU_SEPARATOR) ||
                (group==1 && s->offset!=IWX_FW_PAGING_SEPARATOR) || group>=2) goto failed;
            group++;continue;
        }
        if (!s->data || !s->size || s->size>32768 || count[group]>=IWX_MAX_DRAM_ENTRY) goto failed;
        iwx_dma_t d;
        if (block(b,ops,owner,s->size,&d)) goto failed;
        memcpy(d.data,s->data,s->size);
        uint32_t offset=group==0?__builtin_offsetof(struct iwx_context_info_dram,lmac_img):
                        group==1?__builtin_offsetof(struct iwx_context_info_dram,umac_img):
                                 __builtin_offsetof(struct iwx_context_info_dram,virtual_img);
        uint8_t* image=(uint8_t*)dram+offset;
        memcpy(image+count[group]*8,&d.physical,8);
        count[group]++;
    }
    if (group!=2 || !count[0] || !count[1] || !count[2]) goto failed;
    b->lmac=count[0];b->umac=count[1];b->paging=count[2];b->model=model;
    return 0;
failed:
    iwx_boot_release(b,ops,owner);
    return -1;
}

int iwx_boot_attach_pnvm(iwx_boot_t* b,const iwx_firmware_t* fw,const iwx_pnvm_t* pnvm,
                          const iwx_dma_ops_t* ops,void* owner){
    if(!b||!fw||!pnvm||!ops||!ops->alloc||!ops->release||b->model!=IWX_FW_AX210||
       !b->scratch.data||!b->nblocks||b->nblocks>IWX_BOOT_BLOCKS||b->pnvm.data||
       !pnvm->nsegments||pnvm->nsegments>IWX_PNVM_SEGMENTS||!pnvm->size||
       pnvm->size>512u*1024u||pnvm->mac!=(fw->hardware[0]&0xffffu)||
       pnvm->rf!=(fw->hardware[0]>>16))return -1;
    uint32_t total=0;
    for(uint32_t i=0;i<pnvm->nsegments;i++){
        const iwx_pnvm_segment_t* s=&pnvm->segments[i];
        if(!s->data||!s->size||s->size>pnvm->size-total)return -1;
        total+=s->size;
    }
    if(total!=pnvm->size)return -1;
    uint32_t mark=b->nblocks;
    int fragmented=!!(fw->capabilities[1]&1u); /* API-77 capability 32 */
    iwx_dma_t image;
    if(block(b,ops,owner,fragmented?IWX_MAX_DRAM_ENTRY*8:pnvm->size,&image))return -1;
    uint32_t offset=0;
    for(uint32_t i=0;i<pnvm->nsegments;i++){
        const iwx_pnvm_segment_t* s=&pnvm->segments[i];
        if(fragmented){
            iwx_dma_t segment;
            if(block(b,ops,owner,s->size,&segment))goto failed_pnvm;
            memcpy(segment.data,s->data,s->size);
            memcpy((uint8_t*)image.data+8*i,&segment.physical,8);
        }else{
            memcpy((uint8_t*)image.data+offset,s->data,s->size);
            offset+=s->size;
        }
    }
    __sync_synchronize();
    struct iwx_prph_scratch* scratch=b->scratch.data;
    scratch->ctrl_cfg.pnvm_cfg.pnvm_size=pnvm->size;
    scratch->ctrl_cfg.pnvm_cfg.pnvm_base_addr=image.physical;
    __sync_synchronize();
    b->pnvm=image;b->pnvm_bytes=pnvm->size;
    return 0;
failed_pnvm:
    /* These blocks were never published. The running firmware's previous
     * context/paging/ring allocations must not be released on this error. */
    while(b->nblocks>mark){
        ops->release(owner,&b->blocks[--b->nblocks]);
        memset(&b->blocks[b->nblocks],0,sizeof(b->blocks[0]));
    }
    return -1;
}
