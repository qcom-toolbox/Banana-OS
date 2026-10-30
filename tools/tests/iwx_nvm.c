/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../net/iwx_nvm.h"
#include "../../third_party/iwx/iwx_nvm_regs.h"
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"iwx NVM FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static const uint8_t mac[6]={0x10,0x22,0x33,0x44,0x55,0x66};
static void put16(void* out,unsigned n){uint8_t* p=out;p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static void put32(void* out,uint32_t n){uint8_t* p=out;for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i));}
static void clear(const iwx_nvm_t* n){iwx_nvm_t zero={0};CHECK(!memcmp(n,&zero,sizeof(zero)));}
static void parse(int model,int version){
 iwx_firmware_t fw={.api=77,.hardware={model==200?0x34:0x42},.ncommands=1,
                   .commands={{.group=12,.command=2,.command_version=1,.notification_version=(uint8_t)version}}};
 uint8_t payload[469]={0};unsigned size=version==3?128:468;
 /* Fixtures use the independently imported ABI structs; parser uses byte reads. */
 if(version==3){
  struct iwx_nvm_get_info_rsp_v3* p=(void*)payload;
  p->general.n_hw_addrs=2;put16(&p->general.nvm_version,0x1234);
  put32(&p->mac_sku.mac_sku_flags,0x1f);put32(&p->phy_sku.tx_chains,3);put32(&p->phy_sku.rx_chains,1);
  put32(&p->regulatory.lar_enabled,1);
  for(unsigned i=0;i<51;i++)put16(&p->regulatory.channel_profile[i],0x109u+i*2);
 }else{
  struct iwx_nvm_get_info_rsp* p=(void*)payload;
  p->general.n_hw_addrs=2;put16(&p->general.nvm_version,0x1234);
  put32(&p->mac_sku.mac_sku_flags,0x1f);put32(&p->phy_sku.tx_chains,3);put32(&p->phy_sku.rx_chains,1);
  put32(&p->regulatory.lar_enabled,1);put32(&p->regulatory.n_channels,110);
  for(unsigned i=0;i<110;i++)put32(&p->regulatory.channel_profile[i],0x10000109u+i*2);
 }
 iwx_nvm_t out;
 CHECK(!iwx_nvm_read(&out,&fw,model,mac,payload,size));
 CHECK(!memcmp(out.mac,mac,6)&&out.version==0x1234&&out.addresses==2);
 CHECK(out.sku==0x1f&&out.lar==1&&out.tx_chains==3&&out.rx_chains==1);
 CHECK(out.nchannels==(model==210&&version==4?110u:51u));
 for(unsigned i=0;i<out.nchannels;i++)CHECK(out.channels[i].flags==(version==3?0x109u:0x10000109u)+2*i);
 CHECK(out.channels[0].band==2&&out.channels[0].number==1&&out.channels[0].mhz==2412);
 CHECK(out.channels[13].number==14&&out.channels[13].mhz==2484);
 CHECK(out.channels[14].band==5&&out.channels[14].number==36&&out.channels[14].mhz==5180);
 CHECK(out.channels[41].number==144&&out.channels[42].number==149);
 CHECK(out.channels[50].number==181&&out.channels[50].mhz==5905);
 if(out.nchannels>51){
  CHECK(out.channels[51].band==6&&out.channels[51].number==1&&out.channels[51].mhz==5955);
  CHECK(out.channels[109].number==233&&out.channels[109].mhz==7115);
 }
 for(unsigned n=0;n<size;n++){
  uint8_t* truncated=malloc(n?n:1);CHECK(truncated);if(n)memcpy(truncated,payload,n);
  CHECK(iwx_nvm_read(&out,&fw,model,mac,truncated,n)<0);clear(&out);free(truncated);
 }
 CHECK(iwx_nvm_read(&out,&fw,model,mac,payload,size+1)<0);clear(&out);
 uint8_t changed[469];unsigned random=42;
 for(unsigned i=0;i<5000;i++){
  memcpy(changed,payload,size);random=random*1664525u+1013904223u;
  changed[random%size]^=(uint8_t)(1u<<(random%8));
  int rc=iwx_nvm_read(&out,&fw,model,mac,changed,size);
  if(rc)clear(&out);
  else{
   CHECK(out.nchannels&&out.nchannels<=(model==200?51u:110u));
   CHECK(out.tx_chains&&out.rx_chains&&!((out.tx_chains|out.rx_chains)&~3u));
   CHECK(!memcmp(out.mac,mac,6));
  }
 }
 for(unsigned field=0;field<6;field++){
  memcpy(changed,payload,size);
  if(field==0)put32(changed,1);
  if(field==1)changed[7]=0;
  if(field==2)put32(changed+12,0);
  if(field==3)put32(changed+16,4);
  if(field==4)put32(changed+20,2);
  if(field==5){if(version==3)continue;put32(changed+24,111);}
  CHECK(iwx_nvm_read(&out,&fw,model,mac,changed,size)<0);clear(&out);
 }
 fw.commands[0].notification_version=5;
 CHECK(iwx_nvm_read(&out,&fw,model,mac,payload,size)<0);clear(&out);
 fw.commands[0].notification_version=(uint8_t)version;
 CHECK(iwx_nvm_read(&out,&fw,model==200?210:200,mac,payload,size)<0);clear(&out);
 CHECK(iwx_nvm_read(&out,&fw,999,mac,payload,size)<0);clear(&out);
 CHECK(iwx_nvm_read(&out,&fw,model,NULL,payload,size)<0);clear(&out);
}
int main(void){
 uint8_t out[6];CHECK(!iwx_mac_decode(out,0x10223344,0x5566)&&!memcmp(out,mac,6));
 CHECK(iwx_mac_decode(out,0,0)<0);CHECK(!memcmp(out,(uint8_t[6]){0},6));
 CHECK(iwx_mac_decode(out,0xffffffffu,0xffffffffu)<0);
 CHECK(iwx_mac_decode(out,0x01223344,0x5566)<0);
 CHECK(iwx_mac_decode(out,0x02ccaaff,0xee00)<0);
 CHECK(!iwx_mac_valid(NULL));
 parse(200,3);parse(200,4);parse(210,3);parse(210,4);
 printf("Intel NVM codec: %u checks passed\n",checks);return 0;
}
