/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PNVM TLV selection follows OpenBSD iwx_pnvm_parse/handle_section.
 * Container metadata only; see third_party/iwx/NOTICES. */
#include "iwx_pnvm.h"
#include "../kernel/kstring.h"
static uint32_t le32(const uint8_t* p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint16_t le16(const uint8_t* p){return (uint16_t)(p[0]|p[1]<<8);}
int iwx_pnvm_select(iwx_pnvm_t* out,const uint8_t* data,uint32_t n,
                    const uint32_t sku[3],uint16_t mac,uint16_t rf){
    if(!out)return -1;
    memset(out,0,sizeof(*out));
    if(!data||!sku||n>1024u*1024u||!n)return -1;
    if(!(sku[0]|sku[1]|sku[2])||mac!=0x42u)return -2;
    iwx_pnvm_t candidate={0};
    uint32_t pos=0;int active=0,match=0,version=0,chosen=0;
    while(pos<n){
        if(n-pos<8)goto malformed;
        uint32_t type=le32(data+pos),len=le32(data+pos+4);pos+=8;
        if(len>n-pos)goto malformed;
        uint32_t padding=(4u-(len&3u))&3u;
        if(padding>n-pos-len)goto malformed;
        const uint8_t* p=data+pos;
        if(type==64){
            if(len!=12)goto malformed;
            if(active&&match&&version&&candidate.nsegments&&!chosen){*out=candidate;chosen=1;}
            memset(&candidate,0,sizeof(candidate));match=version=0;
            active=le32(p)==sku[0]&&le32(p+4)==sku[1]&&le32(p+8)==sku[2];
            if(active){memcpy(candidate.sku,sku,sizeof(candidate.sku));candidate.mac=mac;candidate.rf=rf;}
        }else if(type==58){
            if(len!=12)goto malformed;
            if(active&&le16(p)==mac&&le16(p+2)==rf)match=1;
        }else if(type==62){
            if(len!=4)goto malformed;
            if(active){if(version)goto malformed;candidate.version=le32(p);version=1;}
        }else if(type==19){
            if(len<4)goto malformed;
            uint32_t tag=le32(p);
            if(active&&tag!=0xddddeeeeu){
                if(len==4||candidate.nsegments==IWX_PNVM_SEGMENTS||len-4>512u*1024u-candidate.size)goto malformed;
                iwx_pnvm_segment_t* s=&candidate.segments[candidate.nsegments++];
                s->data=p+4;s->size=len-4;s->tag=tag;candidate.size+=len-4;
            }
        }
        pos+=len+padding;
    }
    if(active&&match&&version&&candidate.nsegments&&!chosen){*out=candidate;chosen=1;}
    if(chosen)return 0;
    memset(out,0,sizeof(*out));return -2;
malformed:
    memset(out,0,sizeof(*out));return -1;
}
