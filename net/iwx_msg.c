/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Intel API-77 message ABI, based on the pinned OpenBSD iwx definitions. */
#include "iwx_msg.h"
#include "../third_party/iwx/iwx_msg_regs.h"
#include "../kernel/kstring.h"
_Static_assert(sizeof(struct iwx_tfh_tfd)==256,"TFD ABI");
_Static_assert(sizeof(struct iwx_cmd_header_wide)==8,"wide command ABI");
_Static_assert(sizeof(struct iwx_alive_resp_v4)==116,"ALIVE v4 ABI");
_Static_assert(sizeof(struct iwx_alive_resp_v5)==128,"ALIVE v5 ABI");
_Static_assert(sizeof(struct iwx_alive_resp_v6)==144,"ALIVE v6 ABI");
static uint16_t le16(const uint8_t* p){return (uint16_t)(p[0]|p[1]<<8);}
static uint32_t le32(const uint8_t* p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t le64(const uint8_t* p){return le32(p)|((uint64_t)le32(p+4)<<32);}
static void put16(uint8_t* p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void put64(uint8_t* p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(8*i));}
int iwx_command_build(uint8_t* tfd,uint32_t tc,uint8_t* cmd,uint32_t cc,uint64_t physical,
                      uint8_t opcode,uint8_t group,uint8_t version,uint8_t index,
                      const void* payload,uint32_t length){
    if(!tfd||!cmd||tc<IWX_TFD_BYTES||length>IWX_COMMAND_MAX||
       cc<8||length>cc-8||(!payload&&length)||group>=64||version==99||
       !physical||physical>0xffffffffu||length+7u>0xffffffffu-physical)return -1;
    uintptr_t a=(uintptr_t)tfd,b=(uintptr_t)cmd;
    if(a>=b?a-b<cc:b-a<IWX_TFD_BYTES)return -1;
    if(length)memmove(cmd+8,payload,length);
    memset(cmd,0,8);
    cmd[0]=opcode;cmd[1]=group?group:1;cmd[2]=index;
    put16(cmd+4,(uint16_t)length);cmd[7]=version;
    memset(tfd,0,IWX_TFD_BYTES);
    uint32_t total=8+length,first=total<20?total:20;
    put16(tfd,total>20?2:1);put16(tfd+2,(uint16_t)first);put64(tfd+4,physical);
    if(total>20){put16(tfd+12,(uint16_t)(total-20));put64(tfd+14,physical+20);}
    return 0;
}
int iwx_rx_events(const uint8_t* bytes,uint32_t size,int model,
                  int (*callback)(void*,const iwx_event_t*),void* owner){
    if(!bytes||size<8||size>4096||!callback||
       (model!=IWX_FW_AX200&&model!=IWX_FW_AX210))return -1;
    uint32_t pos=0;int count=0;
    while(size-pos>=8){
        const uint8_t* p=bytes+pos;uint32_t word=le32(p),length=word&0x3fffu;
        if(word==0x55550000u||word==0)return count;
        if(length<4||length>size-pos-4)return -1;
        iwx_event_t event={.opcode=p[4],.group=p[5],.index=p[6],.queue=p[7],.data=p+8,.size=length-4};
        int rc=callback(owner,&event);if(rc<0)return rc;
        count++;if(rc>0||model==IWX_FW_AX210)return count;
        uint32_t step=(length+4+63u)&~63u;
        if(step>size-pos)return count;
        pos+=step;
    }
    return count;
}
int iwx_alive_read(iwx_alive_t* out,const iwx_firmware_t* fw,const iwx_event_t* event){
    if(!out)return -1;
    memset(out,0,sizeof(*out));
    if(!fw||!event||!event->data||event->opcode!=1||event->group!=0||
       !(event->queue&0x80u))return -1;
    int version=iwx_firmware_command_version(fw,0,1,1);
    uint32_t size=version==4?116:version==5?128:(version==6||version==7)?144:0;
    if(!size||event->size!=size||le16(event->data)!=0xcafe)return -1;
    const uint8_t* p=event->data;
    out->flags=le16(p+2);out->lmac_major=le32(p+4);out->umac_major=le32(p+100);
    if(version>=5)for(unsigned i=0;i<3;i++)out->sku[i]=le32(p+116+4*i);
    if(version>=6){out->imr_base=le64(p+128);out->imr_size=le32(p+136);out->imr_enabled=le32(p+140);}
    return 0;
}
