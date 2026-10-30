/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../net/iwx_msg.h"
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"iwx message FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static unsigned get16(const uint8_t* p){return p[0]|p[1]<<8;}
static uint64_t get64(const uint8_t* p){uint64_t v=0;for(unsigned i=0;i<8;i++)v|=(uint64_t)p[i]<<(8*i);return v;}
static void put32(uint8_t* p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static void commands(void){
 uint8_t tfd[256],cmd[4096],payload[4088];
 for(unsigned i=0;i<sizeof(payload);i++)payload[i]=(uint8_t)(i*19u+7u);
 const unsigned lengths[]={0,1,12,13,4088};
 for(unsigned i=0;i<sizeof(lengths)/sizeof(lengths[0]);i++){
  unsigned n=lengths[i];memset(tfd,0xcc,sizeof(tfd));memset(cmd,0xcc,sizeof(cmd));
  CHECK(iwx_command_build(tfd,sizeof(tfd),cmd,sizeof(cmd),0x1234000,0x6a,0,7,255,payload,n)==0);
  CHECK(cmd[0]==0x6a&&cmd[1]==1&&cmd[2]==255&&cmd[3]==0&&get16(cmd+4)==n&&cmd[6]==0&&cmd[7]==7);
  CHECK(!memcmp(cmd+8,payload,n));
  CHECK(get16(tfd)==(n>12?2u:1u)&&get16(tfd+2)==(n+8<20?n+8:20));
  CHECK(get64(tfd+4)==0x1234000);
  if(n>12)CHECK(get16(tfd+12)==n+8-20&&get64(tfd+14)==0x1234014);
 }
 memset(cmd,0x5a,sizeof(cmd));
 CHECK(iwx_command_build(tfd,256,cmd,4096,0x100000,4,2,0,0,cmd,100)==0);
 for(unsigned i=0;i<100;i++)CHECK(cmd[8+i]==0x5a);
 uint8_t before[4096],old[256];memcpy(before,cmd,sizeof(cmd));memcpy(old,tfd,sizeof(tfd));
 CHECK(iwx_command_build(tfd,255,cmd,4096,0x100000,1,1,0,0,payload,1)<0);
 CHECK(iwx_command_build(tfd,256,cmd,7,0x100000,1,1,0,0,payload,1)<0);
 CHECK(iwx_command_build(tfd,256,cmd,4096,0xffffffffu,1,1,0,0,payload,1)<0);
 CHECK(iwx_command_build(tfd,256,cmd,4096,0x100000,1,64,0,0,payload,1)<0);
 CHECK(iwx_command_build(tfd,256,cmd,4096,0x100000,1,1,99,0,payload,1)<0);
 CHECK(iwx_command_build(tfd,256,cmd,4096,0x100000,1,1,0,0,payload,0xffffffffu)<0);
 CHECK(iwx_command_build(tfd,256,cmd,4096,0x100000,1,1,0,0,NULL,1)<0);
 CHECK(!memcmp(before,cmd,sizeof(cmd))&&!memcmp(old,tfd,sizeof(tfd)));
 CHECK(iwx_command_build(cmd,256,cmd+64,4032,0x100000,1,1,0,0,payload,1)<0);
}
typedef struct {const uint8_t* bytes;unsigned size,seen;int stop,error;} received_t;
static int receive(void* owner,const iwx_event_t* e){
 received_t* r=owner;
 CHECK(e->data>=r->bytes&&e->data<=r->bytes+r->size);
 CHECK(e->size<=r->size-(unsigned)(e->data-r->bytes));
 r->seen++;return r->error?-7:r->stop?1:0;
}
static void rx(void){
 uint8_t bytes[4096]={0},mutated[4096];
 put32(bytes,6);bytes[4]=0x81;bytes[5]=2;bytes[6]=4;bytes[7]=0x80;bytes[8]=1;bytes[9]=2;
 put32(bytes+64,5);bytes[68]=0x82;bytes[69]=3;bytes[70]=5;bytes[71]=0x80;bytes[72]=3;
 received_t r={.bytes=bytes,.size=4096};
 CHECK(iwx_rx_events(bytes,4096,IWX_FW_AX200,receive,&r)==2&&r.seen==2);
 r.seen=0;CHECK(iwx_rx_events(bytes,4096,IWX_FW_AX210,receive,&r)==1&&r.seen==1);
 r.seen=0;r.stop=1;CHECK(iwx_rx_events(bytes,4096,IWX_FW_AX200,receive,&r)==1&&r.seen==1);
 r.stop=0;r.error=1;CHECK(iwx_rx_events(bytes,4096,IWX_FW_AX200,receive,&r)==-7);r.error=0;
 put32(bytes,3);CHECK(iwx_rx_events(bytes,4096,IWX_FW_AX200,receive,&r)<0);
 put32(bytes,4096);CHECK(iwx_rx_events(bytes,4096,IWX_FW_AX200,receive,&r)<0);
 put32(bytes,0x55550000);CHECK(iwx_rx_events(bytes,4096,IWX_FW_AX200,receive,&r)==0);
 put32(bytes,6);
 unsigned random=12345;
 for(unsigned i=0;i<5000;i++){
  memcpy(mutated,bytes,sizeof(bytes));random=random*1664525u+1013904223u;
  mutated[random%sizeof(mutated)]^=(uint8_t)(1u<<(random%8));
  r=(received_t){.bytes=mutated,.size=sizeof(mutated)};
  int n=iwx_rx_events(mutated,sizeof(mutated),i&1?IWX_FW_AX200:IWX_FW_AX210,receive,&r);
  CHECK(n<0||(unsigned)n<=64);
 }
 CHECK(iwx_rx_events(bytes,7,IWX_FW_AX200,receive,&r)<0);
}
static void alive(void){
 iwx_firmware_t fw={.ncommands=1,.commands={{.command=1,.group=0,.notification_version=6}}};
 uint8_t payload[144]={0xfe,0xca};put32(payload+4,77);put32(payload+100,88);put32(payload+116,0x610d0);
 iwx_event_t event={.opcode=1,.group=0,.queue=0x80,.data=payload,.size=144};
 iwx_alive_t out;
 CHECK(iwx_alive_read(&out,&fw,&event)==0&&out.sku[0]==0x610d0&&out.lmac_major==77&&out.umac_major==88);
 for(unsigned version=4;version<=7;version++){
  fw.commands[0].notification_version=(uint8_t)version;event.size=version==4?116:version==5?128:144;
  CHECK(iwx_alive_read(&out,&fw,&event)==0);
  CHECK(out.sku[0]==(version==4?0u:0x610d0u));
 }
 fw.commands[0].notification_version=6;event.size=143;CHECK(iwx_alive_read(&out,&fw,&event)<0);
 event.size=144;event.group=1;CHECK(iwx_alive_read(&out,&fw,&event)<0);event.group=0;
 event.queue=0;CHECK(iwx_alive_read(&out,&fw,&event)<0);event.queue=0x80;
 payload[0]=0;CHECK(iwx_alive_read(&out,&fw,&event)<0);
 iwx_alive_t zero={0};CHECK(!memcmp(&out,&zero,sizeof(out)));
}
int main(void){commands();rx();alive();printf("Intel message codecs: %u checks passed\n",checks);return 0;}
