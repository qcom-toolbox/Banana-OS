/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Firmware container layout follows OpenBSD if_iwxreg.h / if_iwx.c:
 * https://github.com/openbsd/src/tree/65596250e06438442ce07bad87e5ba129229607d/sys/dev/pci
 * No firmware instructions are decoded or modified here. */
#include "iwx_fw.h"
#include "../kernel/kstring.h"
static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
int iwx_firmware_parse(iwx_firmware_t* fw,const uint8_t* bytes,uint32_t size,int model) {
    if (!fw) return -1;
    memset(fw,0,sizeof(*fw));
    int result=-1;
    if (model!=IWX_FW_AX200 && model!=IWX_FW_AX210) return -2;
    if (!bytes || size<88 || size>8u*1024u*1024u) return -1;
    if (le32(bytes)!=0 || le32(bytes+4)!=0x0a4c5749u) return -1;
    fw->api=le32(bytes+72); fw->build=le32(bytes+76); fw->cpus=1;
    if (fw->api!=77) { result=-2; goto failed; }
    uint32_t pos=88,seen=0;
    while (pos<size) {
        if (size-pos<8) goto failed;
        uint32_t kind=le32(bytes+pos),n=le32(bytes+pos+4);pos+=8;
        if (n>size-pos) goto failed;
        const uint8_t* p=bytes+pos;
        /* Check padding by subtraction too; n+3 must not wrap. */
        uint32_t padding=(4u-(n&3u))&3u;
        if (padding>size-pos-n) goto failed;
        switch (kind) {
        case 18:
            if (n<4 || (n&3u) || (seen&1u)) goto failed;
            fw->flags=le32(p);seen|=1u;break;
        case 19: {
            if (n<4 || fw->nsections==IWX_FW_SECTIONS) goto failed;
            iwx_fw_section_t* s=&fw->sections[fw->nsections++];
            s->offset=le32(p);s->data=p+4;s->size=n-4;
            if (s->offset!=IWX_FW_CPU_SEPARATOR && s->offset!=IWX_FW_PAGING_SEPARATOR) {
                if (!s->size || s->size>0xffffffffu-s->offset) goto failed;
            } else {
                /* API 77 uses a zero word as the separator payload. It is
                 * metadata, not a device-memory write. */
                if (s->size!=4 || le32(s->data)!=0) goto failed;
                s->size=0;
            }
            break;
        }
        case 23:
            if (n!=4 || (seen&2u)) goto failed;
            fw->phy_config=le32(p);seen|=2u;break;
        case 27:
            if (n!=4 || (seen&4u)) goto failed;
            fw->cpus=le32(p);seen|=4u;
            if (fw->cpus<1 || fw->cpus>2) goto failed;
            break;
        case 29:
        case 30: {
            if (n!=8) goto failed;
            uint32_t index=le32(p);
            if (index>=IWX_FW_BITMAP_WORDS) { result=-2;goto failed; }
            uint32_t* bitmap=kind==29?fw->api_flags:fw->capabilities;
            bitmap[index]|=le32(p+4);break;
        }
        case 32:
            if (n!=4 || (seen&8u)) goto failed;
            fw->paging_size=le32(p);seen|=8u;
            if (fw->paging_size>1024u*1024u || (fw->paging_size&4095u)) goto failed;
            break;
        case 36:
            if (n!=12 || (seen&16u)) goto failed;
            for (int i=0;i<3;i++) fw->version[i]=le32(p+4*i);
            if (fw->version[0]!=77) { result=-2;goto failed; }
            seen|=16u;break;
        case 48:
            if (!n || (n&3u) || n/4>IWX_FW_COMMANDS || (seen&32u)) goto failed;
            fw->ncommands=n/4;
            memcpy(fw->commands,p,n);seen|=32u;
            break;
        case 52:
            if (!n || n>256u*1024u || (seen&64u)) goto failed;
            fw->iml=p;fw->iml_size=n;seen|=64u;break;
        case 58:
            if (n!=12 || (seen&128u)) goto failed;
            for (int i=0;i<3;i++) fw->hardware[i]=le32(p+4*i);
            seen|=128u;break;
        default:
            /* Other records include optional debug metadata. Bootstrap uses
             * only the validated runtime sections and advertised command ABI. */
            break;
        }
        pos+=n+padding;
    }
    if (!fw->nsections || !(seen&2u) || !(seen&16u) || !(seen&32u)) goto failed;
    if (!(seen&128u) || (fw->hardware[0]&0xffffu)!=(model==IWX_FW_AX200?0x34u:0x42u)) {
        result=-2;goto failed;
    }
    if (model==IWX_FW_AX210 && !fw->iml_size) goto failed;
    return 0;
failed:
    memset(fw,0,sizeof(*fw));
    return result;
}
int iwx_firmware_command_version(const iwx_firmware_t* fw,uint8_t group,uint8_t command,int notification) {
    if (!fw || fw->ncommands>IWX_FW_COMMANDS) return -1;
    for (uint32_t i=0;i<fw->ncommands;i++)
        if (fw->commands[i].group==group && fw->commands[i].command==command)
            return notification?fw->commands[i].notification_version:fw->commands[i].command_version;
    return -1;
}
