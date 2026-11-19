/* SPDX-License-Identifier: GPL-3.0-or-later
 * Register/PHY/DMA model derived from the Broadcom programming interface.
 * Runs the actual native driver, not a replacement networking algorithm.
 */
#include <stdarg.h>
#include "netdev.h"
#include "pci.h"
#include "kstring.h"
extern int printf(const char*,...);
extern int fflush(void*);
extern void abort(void);
extern int posix_memalign(void**,size_t,size_t);
extern void free(void*);
#define CHECK(c) do{checks++;if(!(c)){printf("FAIL %d: %s\n",__LINE__,#c);fflush(0);abort();}}while(0)
static unsigned checks,allocn,registered,received,enabled;
static void*allocs[16];
static uint32_t now;
static uint8_t pcfg[2][256];
static uint16_t commands[2];
#include "../../net/bcm5720.c"
static uint32_t regs[2][16384] __attribute__((aligned(4096)));
static uint32_t ape[2][16384] __attribute__((aligned(4096)));
static uint16_t phy[2][32];
static int nv_stuck,boot_stuck,mdio_stuck,mdio_fail,bm_stuck,ftq_stuck,no_mac,register_full,phy_stuck;
static unsigned resets,requests;
static unsigned card(bcm5720_t*c){CHECK(c->pci.dev<2);return c->pci.dev;}
static uint32_t bc_read(bcm5720_t*c,unsigned a){
 CHECK(a<=65532&&!(a&3)&&c->mmio==(void*)regs[card(c)]);
 return regs[card(c)][a/4];
}
static uint32_t bc_ape_read(bcm5720_t*c,unsigned a){
 CHECK(a<=65532&&!(a&3)&&c->ape==(void*)ape[card(c)]);return ape[card(c)][a/4];
}
static void bc_write(bcm5720_t*c,unsigned a,uint32_t v){
 unsigned k=card(c);CHECK(a<=65532&&!(a&3));regs[k][a/4]=v;
 if(a==0x7020){
  if(v&2){requests++;if(!nv_stuck)regs[k][a/4]|=1u<<9;}
  if(v&32)regs[k][a/4]=0;
 }
 if(a==0x6804&&(v&1)){
  resets++;CHECK(!(commands[k]&4));CHECK(regs[k][0x8b50/4]==0x4b657654);
  CHECK(regs[k][0x7020/4]&(1u<<9));
  if(!boot_stuck)regs[k][0x8b50/4]=0xb49a89ab;
  if(!no_mac){regs[k][0x8c14/4]=0x484b0200;regs[k][0x8c18/4]=k+1;}
 }
 if(a==0x44c&&!mdio_stuck){
  unsigned address=(v>>21)&31,r=(v>>16)&31,op=(v>>26)&3;
  CHECK(address==(unsigned)c->pci.fn+1);
  if(op==1){
   phy[k][r]=v&65535;
   if(r==0&&(v&0x8000)&&!phy_stuck)phy[k][0]=0;
   regs[k][a/4]=v&~(1u<<29);
  }else{CHECK(op==2);regs[k][a/4]=phy[k][r]|(mdio_fail?1u<<28:0);}
 }
 if(a==0x4400&&bm_stuck)regs[k][a/4]&=~2u;
 if(a==0x5c00&&ftq_stuck)regs[k][a/4]=1;
 if(a==0x3c00)regs[k][a/4]=v&~0x808u;
 /* Bus mastering remains off until every ring, MAC and PHY is configured. */
 if(!(commands[k]&4))CHECK(!enabled||bc_count>1||register_full);
}
static void bc_delay(unsigned us){now+=(us+999)/1000;}
uint32_t timer_ms(void){return now;}
/* Like the kernel heap the driver gets its rings from: a block never spans
 * a 4 GiB line (bc_dma_range refuses one). The host's heap can place one
 * there now and then (ASLR) - such a block is kept aside and another taken. */
void*kmalloc_aligned(size_t n,size_t align){
 void*held[8];unsigned nh=0;void*p=0;
 for(;;){
  if(posix_memalign(&p,align,n)){p=0;break;}
  uint64_t a=(uintptr_t)p;
  if((a>>32)==((a+n-1)>>32)||nh==8)break;
  held[nh++]=p;
 }
 while(nh)free(held[--nh]);
 if(!p)return 0;
 CHECK(allocn<16);allocs[allocn++]=p;return p;
}
void klog(const char*f,...){(void)f;}
void net_register_device(netdev_t*n){CHECK(commands[((bcm5720_t*)n->priv)->pci.dev&1]&0x400u);registered++;n->present=!register_full;}
void net_rx(netdev_t*n,const uint8_t*b,uint32_t len){
 CHECK(n&&b&&len>=14&&len<=ETH_FRAME_MAX);CHECK(b[0]==0x7a);received++;
}
uint16_t pci_read16(uint8_t b,uint8_t d,uint8_t f,uint8_t off){
 (void)b;(void)f;CHECK(d<2&&!(off&1));
 if(!off)return 0x14e4;
 if(off==4)return commands[d];
 return pcfg[d][off]|(uint16_t)pcfg[d][off+1]<<8;
}
uint32_t pci_read32(uint8_t b,uint8_t d,uint8_t f,uint8_t off){
 CHECK(!(off&3));return pci_read16(b,d,f,off)|(uint32_t)pci_read16(b,d,f,off+2)<<16;
}
void pci_write16(uint8_t b,uint8_t d,uint8_t f,uint8_t off,uint16_t v){
 (void)b;(void)f;CHECK(d<2&&!(off&1));
 if(off==4){commands[d]=v;return;}
 /* PCI status fields next to control registers must never be written. */
 CHECK(off!=6&&off!=0x7a&&off!=0x82);
 if(off==0x44)CHECK(!(v&0x8003u));
 pcfg[d][off]=v;pcfg[d][off+1]=v>>8;
}
void pci_write32(uint8_t b,uint8_t d,uint8_t f,uint8_t off,uint32_t v){
 CHECK(off==0x68||off==0x6c);
 pci_write16(b,d,f,off,v);pci_write16(b,d,f,off+2,v>>16);
}
uintptr_t pci_bar(const pci_dev_t*p,int n,int*io){
 CHECK(p->dev<2&&(n==0||n==2));*io=0;return (uintptr_t)(n==0?regs[p->dev]:ape[p->dev]);
}
void pci_enable_from(const pci_dev_t*p,const char*file){
 CHECK(p&&file);bcm5720_t*c=bc_cards[bc_count-1];
 CHECK(c->ok&&c->rxpost==BC_RX-1);
 CHECK((regs[p->dev][0x6800/4]&0x36034u)==0x36034u);
 CHECK(regs[p->dev][0x26c/4]==BC_RX-1);
 CHECK((regs[p->dev][0x468/4]&0x01700402)==0x01700402);
 CHECK(regs[p->dev][0x2458/4]==BC_RX<<16);
 CHECK(regs[p->dev][0x245c/4]==0);
 CHECK(regs[p->dev][0x810c/4]==0x4000);
 CHECK((regs[p->dev][0x4910/4]&0xf0000)==0xe0000);
 CHECK(phy[p->dev][4]==0x1e1&&phy[p->dev][9]==0x200);
 for(unsigned i=0;i<BC_RX-1;i++){
  CHECK(c->rx[i][2]==(i<<16|BC_BUF)&&c->rx[i][7]==i);
  CHECK((((uint64_t)c->rx[i][0]<<32)|c->rx[i][1])==(uintptr_t)c->rxbuf[i]);
 }
 commands[p->dev]=(commands[p->dev]|7)&~0x400u;enabled++;
}
static pci_dev_t pd={.vendor=0x14e4,.device=0x165f,.class_code=2};
int pci_scan(int(*cb)(const pci_dev_t*,void*),void*arg){
 for(unsigned i=0;i<2;i++){pci_dev_t p=pd;p.dev=p.fn=i;int r=cb(&p,arg);if(r)return r;}return 0;
}
static void init(void){
 while(allocn)free(allocs[--allocn]);
 bc_count=0;memset(bc_cards,0,sizeof(bc_cards));
 memset(regs,0,sizeof(regs));memset(ape,0,sizeof(ape));memset(pcfg,0,sizeof(pcfg));memset(phy,0,sizeof(phy));
 now=registered=received=enabled=resets=requests=0;
 nv_stuck=boot_stuck=mdio_stuck=mdio_fail=bm_stuck=ftq_stuck=no_mac=register_full=phy_stuck=0;
 for(unsigned i=0;i<2;i++){
  commands[i]=7;pcfg[i][0xf7]=5;pcfg[i][0xf6]=0x72;
  phy[i][2]=0x600d;phy[i][3]=0x8580;phy[i][1]=4;phy[i][25]=7<<8;
 }
}
static void inject(bcm5720_t*c,unsigned source,unsigned len,unsigned flags,uint32_t error,uint32_t cookie){
 unsigned i=c->rxcons;
 if(source<BC_RX)memset(c->rxbuf[source],0x7a,BC_BUF);
 memset((void*)c->returns[i],0,32);c->returns[i][2]=(source<<16)|len;
 c->returns[i][3]=flags;c->returns[i][5]=error<<16;c->returns[i][7]=cookie;
 c->status[4]=(c->status[4]&0xffff0000u)|((i+1)%BC_RX);
 bc_poll(&c->nd);
}
static void valid_rx(bcm5720_t*c,unsigned source,unsigned len,unsigned flags,unsigned errors){
 inject(c,source,len,flags,errors,c->cookies[source]);
}
int main(void){
 init();CHECK(bc_attach(&pd)==0&&enabled==1&&registered==1&&resets==1);
 bcm5720_t*c=bc_cards[0];CHECK(c->nd.mac[0]==2&&c->nd.mac[5]==1);
 now+=250;CHECK(bc_carrier(&c->nd));
 uint8_t frame[60]={0};CHECK(bc_send(&c->nd,frame,14)==0);
 CHECK(c->tx[0][2]==(60<<16|4));
 CHECK((((uint64_t)c->tx[0][0]<<32)|c->tx[0][1])==(uintptr_t)c->txbuf[0]);
 for(unsigned i=14;i<60;i++)CHECK(c->txbuf[0][i]==0);
 c->status[4]=1u<<16;bc_poll(&c->nd);CHECK(!c->txused);
 for(unsigned i=0;i<BC_TX-1;i++)CHECK(!bc_send(&c->nd,frame,60));
 CHECK(bc_send(&c->nd,frame,60)<0);
 c->status[4]=c->txprod<<16;bc_poll(&c->nd);CHECK(!c->txused);
 for(unsigned i=0;i<2000;i++)valid_rx(c,i%(BC_RX-1),64,4,0);
 CHECK(received==2000);
 valid_rx(c,0,64,0,0);valid_rx(c,1,64,0x404,0);valid_rx(c,2,64,4,1);
 valid_rx(c,3,17,4,0);valid_rx(c,4,1519,4,0);CHECK(received==2000);
 valid_rx(c,5,18,4,0);valid_rx(c,6,1518,4,0);CHECK(received==2002);
 /* Descriptor lengths, errors and flags are untrusted even after DMA. */
 uint32_t rng=0x5720abcd;
 for(unsigned i=0;i<20000;i++){
  rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;
  valid_rx(c,i%(BC_RX-1),rng&0xffff,(rng>>16)&0xffff,rng&0x8000?0:1);
  CHECK(c->ok);
 }
 unsigned before=received;
 inject(c,0,64,4,0,c->cookies[0]-BC_RX);CHECK(!c->ok&&!(commands[0]&4)&&received==before);
 init();CHECK(!bc_attach(&pd));c=bc_cards[0];
 inject(c,BC_RX+7,64,4,0,0);CHECK(!c->ok&&!(commands[0]&4));
 init();CHECK(!bc_attach(&pd));c=bc_cards[0];
 inject(c,BC_RX-1,64,4,0,BC_RX-1);CHECK(!c->ok&&!(commands[0]&4));
 init();CHECK(!bc_attach(&pd));c=bc_cards[0];c->status[4]=BC_RX;bc_poll(&c->nd);CHECK(!c->ok);
 init();CHECK(!bc_attach(&pd));c=bc_cards[0];c->status[4]=BC_TX<<16;bc_poll(&c->nd);CHECK(!c->ok);
 init();CHECK(!bc_attach(&pd));c=bc_cards[0];c->status[4]=1u<<16;bc_poll(&c->nd);CHECK(!c->ok);
 init();CHECK(!bc_attach(&pd));c=bc_cards[0];c->status[0]=4;bc_poll(&c->nd);CHECK(!c->ok&&!(commands[0]&4));
 init();CHECK(!bc_attach(&pd));c=bc_cards[0];c->cookies[0]=0xfffffe00u;
 valid_rx(c,0,64,4,0);CHECK(!c->ok&&!(commands[0]&4));
 #define BAD(flag) do{init();flag=1;CHECK(bc_attach(&pd)<0&&!enabled&&!(commands[0]&4));}while(0)
 BAD(nv_stuck);BAD(boot_stuck);BAD(mdio_stuck);BAD(mdio_fail);BAD(bm_stuck);BAD(ftq_stuck);BAD(no_mac);BAD(phy_stuck);
 init();register_full=1;CHECK(bc_attach(&pd)<0&&!(commands[0]&4)&&registered==1);
 init();ape[0][0x4000/4]=0x41504521;CHECK(bc_attach(&pd)<0&&!resets&&!requests&&!enabled);
 init();ape[0][0x4000/4]=0xffffffffu;CHECK(bc_attach(&pd)<0&&!resets&&!requests);
 init();ape[0][0x4000/4]=0x1234;CHECK(bc_attach(&pd)<0&&!resets&&!requests);
 init();regs[0][0x3664/4]=0x20;CHECK(bc_attach(&pd)<0&&!resets&&!requests);
 init();pcfg[0][0xf4]=1;CHECK(bc_attach(&pd)<0&&!allocn);
 init();pd.device=0x1682;CHECK(bc_attach(&pd)<0&&!allocn);pd.device=0x165f;
 init();pcfg[0][6]=16;pcfg[0][0x34]=0x40;
 pcfg[0][0x40]=1;pcfg[0][0x41]=0x50;pcfg[0][0x44]=3;pcfg[0][0x45]=0x80;
 pcfg[0][0x50]=5;pcfg[0][0x51]=0x60;pcfg[0][0x52]=1;
 pcfg[0][0x60]=0x11;pcfg[0][0x61]=0x70;pcfg[0][0x63]=0x80;
 pcfg[0][0x70]=0x10;pcfg[0][0x78]=0xf0;pcfg[0][0x79]=0x78;pcfg[0][0x80]=3;
 CHECK(!bc_attach(&pd)&&!(pcfg[0][0x44]&3)&&!pcfg[0][0x52]&&!pcfg[0][0x63]);
 CHECK(pcfg[0][0x78]==0&&pcfg[0][0x79]==0x40&&pcfg[0][0x80]==0);
 init();pcfg[0][6]=16;pcfg[0][0x34]=0x40;pcfg[0][0x40]=5;pcfg[0][0x41]=0x40;
 CHECK(bc_attach(&pd)<0&&!registered&&!(commands[0]&4));
 init();bcm5720_probe_all();CHECK(registered==2&&enabled==2);
 bcm5720_probe_all();CHECK(registered==2&&enabled==2);
 CHECK(bc_cards[0]->nd.mac[5]==1&&bc_cards[1]->nd.mac[5]==2);
 now+=250;CHECK(!bc_send(&bc_cards[0]->nd,frame,60));CHECK(!bc_send(&bc_cards[1]->nd,frame,60));
 bc_cards[0]->status[0]=4;bc_poll(&bc_cards[0]->nd);
 CHECK(!(commands[0]&4)&&(commands[1]&4)&&bc_cards[1]->ok);
 CHECK(!bc_send(&bc_cards[1]->nd,frame,60));
 init();printf("BCM5720 native: %u checks passed\n",checks);return 0;
}
