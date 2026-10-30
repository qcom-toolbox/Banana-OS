/* SPDX-License-Identifier: GPL-3.0-or-later */
/* API-77 NVM_GET_INFO layouts: pinned OpenBSD if_iwxreg.h, sections
 * iwx_nvm_get_info_rsp_v3 / iwx_nvm_get_info_rsp. See third_party/iwx/. */
#include "iwx_nvm.h"
#include "../third_party/iwx/iwx_nvm_regs.h"
_Static_assert(sizeof(struct iwx_nvm_get_info_rsp_v3)==128,"NVM v3 ABI");
_Static_assert(sizeof(struct iwx_nvm_get_info_rsp)==468,"NVM v4 ABI");
_Static_assert(__builtin_offsetof(struct iwx_nvm_get_info_rsp,regulatory.channel_profile)==28,"NVM v4 channels");
#include "../kernel/kstring.h"
static uint16_t le16(const uint8_t* p){return (uint16_t)(p[0]|p[1]<<8);}
static uint32_t le32(const uint8_t* p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
int iwx_mac_valid(const uint8_t mac[6]){
    static const uint8_t reserved[6]={2,0xcc,0xaa,0xff,0xee,0};
    if(!mac||(mac[0]&1)||!memcmp(mac,reserved,6))return 0;
    unsigned any=0;for(unsigned i=0;i<6;i++)any|=mac[i];
    return any!=0;
}
int iwx_mac_decode(uint8_t out[6],uint32_t first,uint32_t last){
    if(!out)return -1;
    uint8_t mac[6]={(uint8_t)(first>>24),(uint8_t)(first>>16),(uint8_t)(first>>8),
                   (uint8_t)first,(uint8_t)(last>>8),(uint8_t)last};
    if(!iwx_mac_valid(mac)){memset(out,0,6);return -1;}
    memcpy(out,mac,6);return 0;
}
static void channel(iwx_nvm_channel_t* c,unsigned index,uint32_t flags){
    c->flags=flags;
    if(index<14){
        c->band=2;c->number=(uint16_t)(index+1);
        c->mhz=index==13?2484:(uint16_t)(2412+5*index);
    }else if(index<51){
        unsigned i=index-14;
        c->band=5;c->number=(uint16_t)(i<28?36+4*i:149+4*(i-28));
        c->mhz=(uint16_t)(5000+5*c->number);
    }else{
        c->band=6;c->number=(uint16_t)(1+4*(index-51));
        c->mhz=(uint16_t)(5950+5*c->number);
    }
}
int iwx_nvm_read(iwx_nvm_t* out,const iwx_firmware_t* fw,int model,const uint8_t mac[6],
                  const uint8_t* p,uint32_t size){
    if(!out)return -1;
    memset(out,0,sizeof(*out));
    if(!fw||!p||!iwx_mac_valid(mac)||fw->api!=77||
       (model!=IWX_FW_AX200&&model!=IWX_FW_AX210)||
       (fw->hardware[0]&0xffffu)!=(model==IWX_FW_AX200?0x34u:0x42u))return -1;
    int version=iwx_firmware_command_version(fw,12,2,1);
    uint32_t expected=version==3?128u:version==4?468u:0;
    if(!expected||size!=expected)return -1;
    uint32_t tx=le32(p+12),rx=le32(p+16),lar=le32(p+20);
    unsigned count=version==3?51u:le32(p+24);
    /* Empty/unknown antenna configurations cannot become a usable radio.
     * AX200/AX210 are two-chain devices; preserve only the supported A/B ABI. */
    if(le32(p)&1u||!p[7]||!tx||!rx||((tx|rx)&~3u)||lar>1||
       !count||count>IWX_NVM_CHANNELS)return -1;
    memcpy(out->mac,mac,6);out->addresses=p[7];out->version=le16(p+4);
    out->tx_chains=(uint8_t)tx;out->rx_chains=(uint8_t)rx;
    out->sku=le32(p+8);out->lar=lar;
    /* AX200 has no 6 GHz radio. Do not alias a 6 GHz channel number onto 2.4 GHz. */
    out->nchannels=model==IWX_FW_AX200&&count>51?51:count;
    for(unsigned i=0;i<out->nchannels;i++){
        uint32_t flags=version==3?le16(p+24+2*i):le32(p+28+4*i);
        channel(&out->channels[i],i,flags);
    }
    return 0;
}
