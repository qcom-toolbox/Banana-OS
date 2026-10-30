/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "net.h"
#include "tcp.h"
#include "ipv6.h"
#include "firewall.h"
static unsigned checks,live,icmp_packets,ipv6_packets,physical_timers;
static uint32_t clock_ms,rng=0x12345678;
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c);exit(1);}}while(0)
void* kmalloc(size_t n) {void* p=malloc(n?n:1);if(p)live++;return p;}
void kfree(void* p) {if(p){CHECK(live>0);live--;free(p);}}
uint32_t timer_ms(void) {return clock_ms++;}
void task_sleep_ms(uint32_t n) {clock_ms+=n;}
int task_current_pid(void) {return 1;}
void task_wake(int pid) {(void)pid;}
void gui_poll(void) {}
int gui_focused_vt(void) {return -1;}
int terminal_vt_get_active(void) {return 0;}
void terminal_vt_set_active(int n) {(void)n;}
int keyboard_try_getchar(void) {return 0;}
void terminal_writeln(const char* s) {(void)s;}
uint32_t random_u32(void) {rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
void random_add_jitter(void) {}
void klog(const char* f,...) {(void)f;}
int ksnprintf(char* b,size_t n,const char* f,...) {va_list a;va_start(a,f);int v=vsnprintf(b,n,f,a);va_end(a);return v;}
int arp_lookup(ip4_t ip,uint8_t mac[6]) {(void)ip;(void)mac;return 0;}
void arp_queue_packet(ip4_t ip,const uint8_t* p,uint32_t n) {(void)ip;(void)p;(void)n;}
void arp_timer(void) {physical_timers++;}
void arp_rx(const uint8_t* p,uint32_t n) {(void)p;(void)n;}
void dhcp_timer(void) {physical_timers++;}
void ipv6_timer(void) {physical_timers++;}
void ipv6_loopback_drain(void) {}
void ipv6_rx(const uint8_t* p,uint32_t n) {(void)p;(void)n;ipv6_packets++;}
int ipv6_enabled(void) {return 0;}
int ipv6_source(const ip6_t* d,ip6_t* s) {(void)d;(void)s;return NET_ERR_NOTREADY;}
int ipv6_send(const ip6_t* s,const ip6_t* d,uint8_t p,const void* b,uint32_t n) {(void)s;(void)d;(void)p;(void)b;(void)n;return NET_ERR_NOTREADY;}
uint16_t ip6_checksum(const ip6_t* s,const ip6_t* d,uint8_t p,const void* b,uint32_t n) {(void)s;(void)d;(void)p;(void)b;(void)n;return 0;}
void ip6_format(const ip6_t* a,char* b,int n) {(void)a;if(n)b[0]=0;}
int ip6_is_loopback(const ip6_t* a) {for(int i=0;i<15;i++)if(a->b[i])return 0;return a->b[15]==1;}
void fw_out_udp(uint16_t port) {(void)port;}
int fw_enabled(void) {return 0;}
int fw_in_tcp(const char* s,uint16_t port,int syn) {(void)s;(void)port;(void)syn;return 1;}
int fw_in_udp(const char* s,uint16_t sport,uint16_t dport) {(void)s;(void)sport;(void)dport;return 1;}
int fw_in_ping(const char* s) {(void)s;return 1;}
void icmp_rx(ip4_t ip,const uint8_t* p,uint32_t n,uint8_t ttl) {(void)ip;(void)p;(void)n;(void)ttl;icmp_packets++;}
static unsigned udp_got;
static void udp_handler(ip4_t src,uint16_t sport,const uint8_t* p,uint32_t n,void* ctx) {
    CHECK(src==IP4(127,0,0,1) && sport==22100 && ctx==(void*)1 && n==5 && !memcmp(p,"hello",5));udp_got++;
}
static void local_tcp(ip4_t ip) {
    int err=0;
    tcp_listener_t* l=tcp_listen(22001,&err);CHECK(l && err==0);
    tcp_conn_t* a=tcp_connect(ip,22001,1000,&err);CHECK(a && err==0);
    tcp_conn_t* b=tcp_accept(l,1000);CHECK(b);
    uint8_t sent[100000],got[100000];
    for(unsigned i=0;i<sizeof(sent);i++)sent[i]=(uint8_t)(i*13);
    CHECK(tcp_send(a,sent,sizeof(sent),1000)==sizeof(sent));
    unsigned n=0;
    while(n<sizeof(got)){int rc=tcp_recv(b,got+n,sizeof(got)-n,1000);CHECK(rc>0);n+=(unsigned)rc;}
    CHECK(!memcmp(sent,got,sizeof(sent)));
    CHECK(tcp_send(b,"reply",5,1000)==5);
    CHECK(tcp_recv(a,got,5,1000)==5 && !memcmp(got,"reply",5));
    CHECK(tcp_recv(a,got,1,20)==NET_ERR_TIMEOUT);
    tcp_network_changed(); /* Physical link changes leave local streams intact. */
    CHECK(tcp_send(a,"ok",2,1000)==2 && tcp_recv(b,got,2,1000)==2);
    tcp_abort(a);tcp_abort(b);tcp_unlisten(l);
    for(int i=0;i<5;i++)net_wait(10);
}
static void physical_rx_isolation(void) {
    netdev_t dev={0};netif_t* nif=net_if();nif->dev=&dev;nif->configured=1;nif->ip=IP4(10,0,2,15);
    uint8_t f[80]={0};wr16(f+12,ETH_TYPE_IP);
    uint8_t* p=f+14;p[0]=0x45;wr16(p+2,20);p[8]=64;p[9]=IP_PROTO_ICMP;
    ip4_t addresses[][2]={{IP4(127,0,0,1),nif->ip},{IP4(10,0,2,2),IP4(127,0,0,1)},{IP4(10,0,2,2),nif->ip}};
    for(unsigned i=0;i<3;i++){
        wr32(p+12,addresses[i][0]);wr32(p+16,addresses[i][1]);wr16(p+10,0);wr16(p+10,net_checksum(p,20));
        net_rx(&dev,f,34);CHECK(icmp_packets==(i==2?1u:0u));
    }
    memset(f,0,sizeof(f));wr16(f+12,0x86dd);p=f+14;p[0]=0x60;
    p[8+15]=1;net_rx(&dev,f,54);CHECK(ipv6_packets==0);
    memset(p+8,0,32);p[24+15]=1;net_rx(&dev,f,54);CHECK(ipv6_packets==0);
    memset(p+8,0,32);p[8]=p[24]=0x20;net_rx(&dev,f,54);CHECK(ipv6_packets==1);
    nif->dev=NULL;nif->configured=0;
}
int main(void) {
    CHECK(!net_if()->dev && !net_if()->configured);
    local_tcp(IP4(127,0,0,1));local_tcp(IP4(127,0,0,2));
    int err=0;CHECK(!tcp_connect(IP4(127,0,0,1),22002,1000,&err) && err==NET_ERR_REFUSED);
    CHECK(!tcp_connect(IP4(192,0,2,1),22002,1000,&err) && err==NET_ERR_NODEV);
    CHECK(!udp_bind(22101,udp_handler,(void*)1));
    CHECK(udp_send(IP4(127,0,0,1),22100,22101,"hello",5)==0);
    net_poll();CHECK(udp_got==1);udp_unbind(22101);
    CHECK(physical_timers==0);
    physical_rx_isolation();
    CHECK(live==0);printf("Loopback without NIC: %u checks passed\n",checks);
}
