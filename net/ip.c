#include "net.h"
#include "kstring.h"
#include "kheap.h"

/* IPv4 (RFC 791): one interface, a default gateway, no fragmentation
 * (we send with Don't Fragment set and drop incoming fragments - TCP's
 * MSS keeps our traffic under the 1500-byte MTU). */

static uint16_t g_ip_id = 1;
uint32_t g_ip_rx_dropped;

/* Loopback: packets to 127.0.0.0/8 or to our own address are queued and
 * fed back through ip_rx() from net_poll() - never recursively, since
 * the receive path itself sends (e.g. an echo reply). */
#define LOOP_MAX 8
static struct { uint8_t* pkt; uint32_t len; } g_loop[LOOP_MAX];

static int is_local(ip4_t dst) {
    netif_t* nif = net_if();
    return (dst >> 24) == 127 || (nif->configured && dst == nif->ip);
}

static int loopback_send(ip4_t dst, uint8_t proto, const void* payload, uint32_t len) {
    for (int i = 0; i < LOOP_MAX; i++) {
        if (g_loop[i].pkt) continue;
        uint8_t* p = (uint8_t*)kmalloc(20 + len);
        if (!p) return NET_ERR_NOMEM;
        p[0] = 0x45; p[1] = 0;
        wr16(p + 2, (uint16_t)(20 + len));
        wr16(p + 4, g_ip_id++);
        wr16(p + 6, 0x4000);
        p[8] = 64; p[9] = proto;
        wr16(p + 10, 0);
        wr32(p + 12, dst);             /* loopback: from ourselves */
        wr32(p + 16, dst);
        wr16(p + 10, net_checksum(p, 20));
        memcpy(p + 20, payload, len);
        g_loop[i].pkt = p;
        g_loop[i].len = 20 + len;
        return NET_OK;
    }
    return NET_ERR_NOMEM;
}

void ip_loopback_drain(void) {
    for (int i = 0; i < LOOP_MAX; i++) {
        if (!g_loop[i].pkt) continue;
        uint8_t* p = g_loop[i].pkt;
        uint32_t len = g_loop[i].len;
        g_loop[i].pkt = NULL;       /* free the slot first: ip_rx may send again */
        ip_rx(p, len);
        kfree(p);
    }
}

int ip_send(ip4_t dst, uint8_t proto, const void* payload, uint32_t len) {
    netif_t* nif = net_if();
    if (!nif->dev) return NET_ERR_NODEV;
    if (len > IP_MAX_PAYLOAD) return NET_ERR_PROTO;
    if (is_local(dst)) return loopback_send(dst, proto, payload, len);
    /* before DHCP completes only broadcasts (DHCP itself) may go out */
    if (!nif->configured && dst != IP4_BROADCAST) return NET_ERR_NOTREADY;

    static uint8_t pkt[20 + IP_MAX_PAYLOAD];   /* not reentrant, saves task stack */
    pkt[0] = 0x45;                     /* IPv4, 20-byte header */
    pkt[1] = 0;
    wr16(pkt + 2, (uint16_t)(20 + len));
    wr16(pkt + 4, g_ip_id++);
    wr16(pkt + 6, 0x4000);             /* DF */
    pkt[8] = 64;                       /* TTL */
    pkt[9] = proto;
    wr16(pkt + 10, 0);
    wr32(pkt + 12, nif->configured ? nif->ip : 0);
    wr32(pkt + 16, dst);
    wr16(pkt + 10, net_checksum(pkt, 20));
    memcpy(pkt + 20, payload, len);

    if (dst == IP4_BROADCAST ||
        (nif->configured && nif->netmask && (dst | nif->netmask) == IP4_BROADCAST &&
         (dst & nif->netmask) == (nif->ip & nif->netmask))) {
        return eth_send(ETH_BROADCAST, ETH_TYPE_IP, pkt, 20 + len);
    }

    /* on-link destinations go direct, everything else via the gateway */
    ip4_t next_hop = dst;
    if ((dst & nif->netmask) != (nif->ip & nif->netmask)) {
        if (!nif->gateway) return NET_ERR_NOROUTE;
        next_hop = nif->gateway;
    }
    uint8_t mac[6];
    if (arp_lookup(next_hop, mac)) return eth_send(mac, ETH_TYPE_IP, pkt, 20 + len);
    arp_queue_packet(next_hop, pkt, 20 + len);
    return NET_OK;
}

void ip_rx(const uint8_t* p, uint32_t len) {
    netif_t* nif = net_if();
    if (len < 20 || (p[0] >> 4) != 4) return;
    uint32_t ihl = (uint32_t)(p[0] & 0x0F) * 4u;
    uint32_t total = rd16(p + 2);
    if (ihl < 20 || total < ihl || total > len) { g_ip_rx_dropped++; return; }
    if (net_checksum(p, ihl) != 0) { g_ip_rx_dropped++; return; }
    uint16_t frag = rd16(p + 6);
    if ((frag & 0x2000) || (frag & 0x1FFF)) { g_ip_rx_dropped++; return; }   /* fragment */

    ip4_t src = rd32(p + 12), dst = rd32(p + 16);
    int for_us = (dst == IP4_BROADCAST) || !nif->configured ||
                 dst == nif->ip || (dst >> 24) == 127 ||
                 (nif->netmask && dst == ((nif->ip & nif->netmask) | ~nif->netmask));
    if (!for_us) return;

    const uint8_t* payload = p + ihl;
    uint32_t plen = total - ihl;
    switch (p[9]) {
    case IP_PROTO_ICMP: icmp_rx(src, payload, plen, p[8]); break;
    case IP_PROTO_UDP:  udp_rx(src, dst, payload, plen); break;
    case IP_PROTO_TCP:  if (nif->configured) tcp_rx(src, dst, payload, plen); break;
    default: break;
    }
}
