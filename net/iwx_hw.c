/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Standalone AX200/AX210 bootstrap register sequence, following OpenBSD
 * if_iwx.c. See third_party/iwx/NOTICES and its pinned source revision. */
#include "iwx_hw.h"
#include "../third_party/iwx/iwx_boot_regs.h"
static int rd(iwx_hw_t* h,uint32_t r,uint32_t* v){return h->io->read32(h->owner,r,v);}
static int wr(iwx_hw_t* h,uint32_t r,uint32_t v){return h->io->write32(h->owner,r,v);}
static int bits(iwx_hw_t* h,uint32_t r,uint32_t clear,uint32_t set){
    uint32_t v;if(rd(h,r,&v)||v==0xffffffffu)return -1;
    return wr(h,r,(v&~clear)|set);
}
static int poll(iwx_hw_t* h,uint32_t r,uint32_t mask,uint32_t want,unsigned us){
    for(unsigned elapsed=0;;elapsed+=10){
        uint32_t v;if(rd(h,r,&v)||v==0xffffffffu)return -1;
        if((v&mask)==want)return 0;
        if(elapsed>=us)return -1;
        h->io->delay_us(h->owner,10);
    }
}
static uint32_t prph_mask(iwx_hw_t* h){return h->model==IWX_FW_AX210?0xffffffu:0xfffffu;}
static int prd(iwx_hw_t* h,uint32_t addr,uint32_t* v){
    if(wr(h,IWX_HBUS_TARG_PRPH_RADDR,(addr&prph_mask(h))|(3u<<24)))return -1;
    __sync_synchronize();return rd(h,IWX_HBUS_TARG_PRPH_RDAT,v);
}
static int pwr(iwx_hw_t* h,uint32_t addr,uint32_t v){
    if(wr(h,IWX_HBUS_TARG_PRPH_WADDR,(addr&prph_mask(h))|(3u<<24)))return -1;
    __sync_synchronize();return wr(h,IWX_HBUS_TARG_PRPH_WDAT,v);
}
static int lock(iwx_hw_t* h){
    if(h->locked)return -1;
    if(bits(h,IWX_CSR_GP_CNTRL,0,IWX_CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ))return -1;
    h->io->delay_us(h->owner,2);
    if(poll(h,IWX_CSR_GP_CNTRL,
       IWX_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY|IWX_CSR_GP_CNTRL_REG_FLAG_GOING_TO_SLEEP,
       IWX_CSR_GP_CNTRL_REG_VAL_MAC_ACCESS_EN,150000)){
        bits(h,IWX_CSR_GP_CNTRL,IWX_CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ,0);return -1;
    }
    h->locked=1;return 0;
}
static int unlock(iwx_hw_t* h){
    int rc=bits(h,IWX_CSR_GP_CNTRL,IWX_CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ,0);
    if(!rc)h->locked=0;
    return rc;
}
static int ready(iwx_hw_t* h){
    if(bits(h,IWX_CSR_HW_IF_CONFIG_REG,0,IWX_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY) ||
       poll(h,IWX_CSR_HW_IF_CONFIG_REG,IWX_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY,
            IWX_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY,50))return -1;
    return bits(h,IWX_CSR_MBOX_SET_REG,0,IWX_CSR_MBOX_SET_REG_OS_ALIVE);
}
int iwx_hw_start(iwx_hw_t* h){
    if(!h||!h->io||!h->io->read32||!h->io->write32||!h->io->delay_us||
       h->locked||h->started||h->kicked||
       (h->model!=IWX_FW_AX200&&h->model!=IWX_FW_AX210))return -1;
    uint32_t revision;
    if(rd(h,IWX_CSR_HW_REV,&revision)||revision==0xffffffffu||
       IWX_CSR_HW_REV_TYPE(revision)!=(h->model==IWX_FW_AX200?0x34u:0x42u))return -1;
    if(ready(h)){
        if(bits(h,IWX_CSR_DBG_LINK_PWR_MGMT_REG,0,IWX_CSR_RESET_LINK_PWR_MGMT_DISABLED))return -1;
        h->io->delay_us(h->owner,1000);
        if(bits(h,IWX_CSR_HW_IF_CONFIG_REG,0,IWX_CSR_HW_IF_CONFIG_REG_PREPARE)||
           poll(h,IWX_CSR_HW_IF_CONFIG_REG,IWX_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY,
                IWX_CSR_HW_IF_CONFIG_REG_BIT_NIC_READY,150000))return -1;
        if(ready(h))return -1;
    }
    if(h->model==IWX_FW_AX200){
        uint32_t v,protect;
        if(prd(h,IWX_HPM_DEBUG,&v))return -1;
        if(v!=0xa5a5a5a0u && (v&IWX_PERSISTENCE_BIT)){
            if(prd(h,IWX_PREG_PRPH_WPROT_22000,&protect)||(protect&IWX_PREG_WFPM_ACCESS)||
               pwr(h,IWX_HPM_DEBUG,v&~IWX_PERSISTENCE_BIT))return -1;
        }
    }
    if(bits(h,IWX_CSR_RESET,0,IWX_CSR_RESET_REG_FLAG_SW_RESET))return -1;
    h->io->delay_us(h->owner,5000);
    /* Polling transport: keep interrupts masked until an IRQ owner exists. */
    if(wr(h,IWX_CSR_INT_MASK,0)||
       bits(h,IWX_CSR_GIO_CHICKEN_BITS,0,IWX_CSR_GIO_CHICKEN_BITS_REG_BIT_L1A_NO_L0S_RX)||
       bits(h,IWX_CSR_DBG_HPET_MEM_REG,0,IWX_CSR_DBG_HPET_MEM_REG_VAL)||
       bits(h,IWX_CSR_HW_IF_CONFIG_REG,0,IWX_CSR_HW_IF_CONFIG_REG_BIT_HAP_WAKE_L1A)||
       bits(h,IWX_CSR_GIO_REG,0,IWX_CSR_GIO_REG_VAL_L0S_DISABLED)||
       bits(h,IWX_CSR_GP_CNTRL,0,IWX_CSR_GP_CNTRL_REG_FLAG_INIT_DONE)||
       poll(h,IWX_CSR_GP_CNTRL,IWX_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY,
            IWX_CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY,25000))return -1;
    if(lock(h))return -1;
    uint32_t umac=h->model==IWX_FW_AX210?0x300000u:0;
    int rc=pwr(h,IWX_UREG_CHICK+umac,IWX_UREG_CHICK_MSI_ENABLE);
    if(unlock(h)||rc)return -1;
    uint32_t gp;if(rd(h,IWX_CSR_GP_CNTRL,&gp))return -1;
    if(!(gp&IWX_CSR_GP_CNTRL_REG_FLAG_HW_RF_KILL_SW))return -2;
    h->started=1;return 0;
}
static int write_address(iwx_hw_t* h,uint32_t reg,uint64_t address){
    if(!address||address>0xffffffffu||(address&4095u))return -1;
    return wr(h,reg,(uint32_t)address)||wr(h,reg+4,(uint32_t)(address>>32))?-1:0;
}
int iwx_hw_kick(iwx_hw_t* h,const iwx_boot_t* b,const iwx_firmware_t* fw){
    if(!h||!h->io||!h->io->read32||!h->io->write32||!h->io->delay_us||
       !b||!fw||!h->started||h->kicked||h->locked||!b->nblocks||
       b->model!=h->model||fw->api!=77||
       (fw->hardware[0]&0xffffu)!=(h->model==IWX_FW_AX200?0x34u:0x42u))return -1;
    uint32_t revision;if(rd(h,IWX_CSR_HW_REV,&revision)||revision==0xffffffffu)return -1;
    uint32_t gp;if(rd(h,IWX_CSR_GP_CNTRL,&gp)||
        !(gp&IWX_CSR_GP_CNTRL_REG_FLAG_HW_RF_KILL_SW))return -2;
    if(h->model==IWX_FW_AX200){
        uint32_t phy=fw->phy_config;
        uint32_t mask=IWX_CSR_HW_IF_CONFIG_REG_MSK_MAC_DASH|IWX_CSR_HW_IF_CONFIG_REG_MSK_MAC_STEP|
            IWX_CSR_HW_IF_CONFIG_REG_MSK_PHY_STEP|IWX_CSR_HW_IF_CONFIG_REG_MSK_PHY_DASH|
            IWX_CSR_HW_IF_CONFIG_REG_MSK_PHY_TYPE|IWX_CSR_HW_IF_CONFIG_REG_BIT_RADIO_SI|
            IWX_CSR_HW_IF_CONFIG_REG_BIT_MAC_SI;
        uint32_t value=IWX_CSR_HW_REV_STEP(revision)<<IWX_CSR_HW_IF_CONFIG_REG_POS_MAC_STEP |
            IWX_CSR_HW_REV_DASH(revision)<<IWX_CSR_HW_IF_CONFIG_REG_POS_MAC_DASH |
            ((phy&IWX_FW_PHY_CFG_RADIO_TYPE)>>IWX_FW_PHY_CFG_RADIO_TYPE_POS)<<IWX_CSR_HW_IF_CONFIG_REG_POS_PHY_TYPE |
            ((phy&IWX_FW_PHY_CFG_RADIO_STEP)>>IWX_FW_PHY_CFG_RADIO_STEP_POS)<<IWX_CSR_HW_IF_CONFIG_REG_POS_PHY_STEP |
            ((phy&IWX_FW_PHY_CFG_RADIO_DASH)>>IWX_FW_PHY_CFG_RADIO_DASH_POS)<<IWX_CSR_HW_IF_CONFIG_REG_POS_PHY_DASH;
        if(bits(h,IWX_CSR_HW_IF_CONFIG_REG,mask,value))return -1;
    }
    if(bits(h,IWX_CSR_MAC_SHADOW_REG_CTRL,0,0x800fffffu))return -1;
    __sync_synchronize();
    if(h->model==IWX_FW_AX200){
        if(write_address(h,IWX_CSR_CTXT_INFO_BA,b->context.physical))return -1;
    }else{
        if(!b->scratch.data||!b->iml.data||!b->iml_bytes||b->iml_bytes!=fw->iml_size||b->iml.size<b->iml_bytes||
           write_address(h,IWX_CSR_CTXT_INFO_ADDR,b->context.physical)||
           write_address(h,IWX_CSR_IML_DATA_ADDR,b->iml.physical)||
           wr(h,IWX_CSR_IML_SIZE_ADDR,b->iml_bytes)||
           bits(h,IWX_CSR_CTXT_INFO_BOOT_CTRL,0,IWX_CSR_AUTO_FUNC_BOOT_ENA))return -1;
    }
    if(lock(h))return -1;
    uint32_t ltr=IWX_CSR_LTR_LONG_VAL_AD_NO_SNOOP_REQ|
        (2u<<IWX_CSR_LTR_LONG_VAL_AD_NO_SNOOP_SCALE_SHIFT)|
        (250u<<IWX_CSR_LTR_LONG_VAL_AD_NO_SNOOP_VAL_SHIFT)|
        IWX_CSR_LTR_LONG_VAL_AD_SNOOP_REQ|(2u<<IWX_CSR_LTR_LONG_VAL_AD_SNOOP_SCALE_SHIFT)|250u;
    int rc=wr(h,IWX_CSR_LTR_LONG_VAL_AD,ltr);
    if(!rc){
        /* A failed doorbell write has an uncertain hardware outcome. Retain
         * all DMA ownership until stop is acknowledged, even on this error. */
        h->kicked=1;
        rc=pwr(h,IWX_UREG_CPU_INIT_RUN+(h->model==IWX_FW_AX210?0x300000u:0),1);
    }
    int release=unlock(h);
    return rc||release?-1:0;
}
int iwx_hw_pnvm(iwx_hw_t* h){
    if(!h||h->model!=IWX_FW_AX210||!h->io||!h->io->read32||
       !h->io->write32||!h->io->delay_us||!h->started||!h->kicked||h->locked)return -1;
    uint32_t gp;
    if(rd(h,IWX_CSR_GP_CNTRL,&gp)||gp==0xffffffffu||
       !(gp&IWX_CSR_GP_CNTRL_REG_FLAG_HW_RF_KILL_SW))return -1;
    if(lock(h))return -1;
    __sync_synchronize();
    int rc=pwr(h,IWX_UREG_DOORBELL_TO_ISR6+0x300000u,IWX_UREG_DOORBELL_TO_ISR6_PNVM);
    int release=unlock(h);
    return rc||release?-1:0;
}
int iwx_hw_stop(iwx_hw_t* h){
    if(!h||!h->io||!h->io->read32||!h->io->write32||!h->io->delay_us)return -1;
    if(wr(h,IWX_CSR_INT_MASK,0)||
       bits(h,IWX_CSR_RESET,0,IWX_CSR_RESET_REG_FLAG_STOP_MASTER)||
       poll(h,IWX_CSR_RESET,IWX_CSR_RESET_REG_FLAG_MASTER_DISABLED,
            IWX_CSR_RESET_REG_FLAG_MASTER_DISABLED,100))return -1;
    if(bits(h,IWX_CSR_GP_CNTRL,IWX_CSR_GP_CNTRL_REG_FLAG_INIT_DONE|
            IWX_CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ,0))return -1;
    h->locked=h->started=h->kicked=0;
    return 0;
}
