#ifndef NET_H
#define NET_H

#include "types.h"
#include "netdev.h"

/*
 * Banana OS TCP/IP stack.
 *
 *   net.c    interface config, Ethernet, the netd task, polling/waiting
 *   arp.c    ARP resolution + cache (with a small queue for packets
 *            waiting on a resolution)
 *   ip.c     IPv4 send/receive + routing (one interface, default gateway)
 *   icmp.c   echo request/reply (ping, and answering pings)
 *   udp.c    UDP ports
 *   dhcp.c   DHCP client (runs in the background from boot)
 *   dns.c    DNS resolver (A records)
 *   tcp.c    TCP (client side): see tcp.h
 *
 * IPv4 addresses are kept in host byte order as uint32_t (10.0.2.15 is
 * 0x0A00020F); only the wire format uses network order. Everything runs
 * in task context: NIC interrupts just wake the netd task.
 */

typedef uint32_t ip4_t;
#define IP4(a, b, c, d) (((ip4_t)(a) << 24) | ((ip4_t)(b) << 16) | ((ip4_t)(c) << 8) | (ip4_t)(d))
#define IP4_BROADCAST   0xFFFFFFFFu

/* error codes returned (negated) by the blocking APIs */
#define NET_OK            0
#define NET_ERR_TIMEOUT   (-1)
#define NET_ERR_RESET     (-2)
#define NET_ERR_REFUSED   (-3)
#define NET_ERR_NOROUTE   (-4)
#define NET_ERR_NOMEM     (-5)
#define NET_ERR_CLOSED    (-6)
#define NET_ERR_DNS       (-7)
#define NET_ERR_NOTREADY  (-8)
#define NET_ERR_NODEV     (-9)
#define NET_ERR_PROTO     (-10)
#define NET_ERR_INTR      (-11)

const char* net_strerror(int err);

typedef struct {
    netdev_t* dev;          /* NULL: no supported NIC found */
    ip4_t     ip, netmask, gateway, dns;
    int       configured;   /* ip is valid (DHCP bound or static) */
    int       dhcp;         /* 1 = managed by the DHCP client */
    ip4_t     dhcp_server;
    uint32_t  lease_s;
    uint32_t  bound_ms;     /* timer_ms() when configured */
} netif_t;

netif_t* net_if(void);

void net_init(void);          /* probe NICs, start netd + DHCP */
void net_poll(void);          /* drain RX, run protocol timers */
/* Blocking helpers call this in their wait loops: polls, then sleeps
 * until a packet arrives or `max_ms` passes. */
void net_wait(uint32_t max_ms);
/* Waits up to timeout_ms for an IP configuration. 1 = configured. */
int  net_wait_configured(uint32_t timeout_ms);
/* 1 if the user pressed Ctrl+C (consumed); blocking commands poll this */
int  net_interrupted(void);

void net_set_static(ip4_t ip, ip4_t mask, ip4_t gw, ip4_t dns);

/* interfaces (PCI cards + USB adapters) */
int       net_device_count(void);
netdev_t* net_device_at(int i);
void      net_select_device(netdev_t* nd);   /* switch the active interface */

/* byte order + checksum helpers */
static inline uint16_t htons(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24);
}
static inline uint32_t ntohl(uint32_t v) { return htonl(v); }
static inline uint16_t rd16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t rd32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

uint32_t net_csum_add(uint32_t sum, const void* data, uint32_t len);
uint16_t net_csum_fold(uint32_t sum);
uint16_t net_checksum(const void* data, uint32_t len);

void ip4_to_str(ip4_t ip, char out[16]);
int  str_to_ip4(const char* s, ip4_t* out);    /* 1 on success */

/* ── Ethernet ─────────────────────────────────────────────────── */
#define ETH_TYPE_IP  0x0800
#define ETH_TYPE_ARP 0x0806
extern const uint8_t ETH_BROADCAST[6];
int eth_send(const uint8_t dst[6], uint16_t type, const void* payload, uint32_t len);

/* ── ARP ──────────────────────────────────────────────────────── */
void arp_rx(const uint8_t* pkt, uint32_t len);
int  arp_lookup(ip4_t ip, uint8_t mac_out[6]);    /* 1 if cached */
void arp_request(ip4_t ip);
/* queue a finished IP packet until `next_hop` resolves (takes a copy) */
void arp_queue_packet(ip4_t next_hop, const uint8_t* ip_pkt, uint32_t len);
void arp_timer(void);
void arp_dump(void);
void arp_flush(void);

/* ── IPv4 ─────────────────────────────────────────────────────── */
#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17
#define IP_MAX_PAYLOAD 1480
int  ip_send(ip4_t dst, uint8_t proto, const void* payload, uint32_t len);
void ip_rx(const uint8_t* pkt, uint32_t len);

/* ── ICMP ─────────────────────────────────────────────────────── */
void icmp_rx(ip4_t src, const uint8_t* msg, uint32_t len, uint8_t ttl);
int  icmp_echo_send(ip4_t dst, uint16_t id, uint16_t seq, uint32_t data_len);
typedef struct {
    ip4_t    src;
    uint16_t id, seq;
    uint8_t  ttl;
    uint8_t  type;          /* 0 = echo reply, 3 = unreachable, 11 = TTL exceeded */
    uint32_t bytes;
    uint32_t recv_ms;
} icmp_event_t;
/* next ping-related event for `id`, 1 if one was waiting */
int  icmp_poll_event(uint16_t id, icmp_event_t* out);

/* ── UDP ──────────────────────────────────────────────────────── */
typedef void (*udp_handler_t)(ip4_t src, uint16_t sport, const uint8_t* data, uint32_t len, void* ctx);
int      udp_bind(uint16_t port, udp_handler_t h, void* ctx);  /* 0 ok */
void     udp_unbind(uint16_t port);
uint16_t udp_ephemeral_port(void);
int      udp_send(ip4_t dst, uint16_t sport, uint16_t dport, const void* data, uint32_t len);
void     udp_rx(ip4_t src, ip4_t dst, const uint8_t* dgram, uint32_t len);

/* ── DHCP ─────────────────────────────────────────────────────── */
void        dhcp_start(void);
void        dhcp_stop(void);       /* a static configuration takes over */
void        dhcp_timer(void);
const char* dhcp_state_str(void);

/* ── DNS ──────────────────────────────────────────────────────── */
/* Resolves a hostname (or dotted quad) to an IPv4 address. Blocking. */
int dns_resolve(const char* name, ip4_t* out, uint32_t timeout_ms);

/* ── TCP: see tcp.h ───────────────────────────────────────────── */
void tcp_rx(ip4_t src, ip4_t dst, const uint8_t* seg, uint32_t len);
void tcp_timer(void);
void tcp_flush(void);

/* ── loopback (ip.c) ──────────────────────────────────────────── */
void ip_loopback_drain(void);

#endif
