#include "net.h"
#include "kstring.h"
#include "random.h"

/* UDP (RFC 768): a small table of bound ports with receive callbacks. */

#define UDP_BINDINGS 16

typedef struct {
    uint16_t      port;
    udp_handler_t handler;
    void*         ctx;
} udp_binding_t;

static udp_binding_t g_bind[UDP_BINDINGS];

int udp_bind(uint16_t port, udp_handler_t h, void* ctx) {
    int slot = -1;
    for (int i = 0; i < UDP_BINDINGS; i++) {
        if (g_bind[i].handler && g_bind[i].port == port) return -1;
        if (!g_bind[i].handler && slot < 0) slot = i;
    }
    if (slot < 0) return -1;
    g_bind[slot].port = port;
    g_bind[slot].handler = h;
    g_bind[slot].ctx = ctx;
    return 0;
}

void udp_unbind(uint16_t port) {
    for (int i = 0; i < UDP_BINDINGS; i++)
        if (g_bind[i].handler && g_bind[i].port == port) g_bind[i].handler = NULL;
}

uint16_t udp_ephemeral_port(void) {
    for (;;) {
        uint16_t p = (uint16_t)(49152u + random_u32() % 16384u);
        int used = 0;
        for (int i = 0; i < UDP_BINDINGS; i++)
            if (g_bind[i].handler && g_bind[i].port == p) used = 1;
        if (!used) return p;
    }
}

int udp_send(ip4_t dst, uint16_t sport, uint16_t dport, const void* data, uint32_t len) {
    netif_t* nif = net_if();
    if (len > IP_MAX_PAYLOAD - 8) return NET_ERR_PROTO;
    uint8_t d[IP_MAX_PAYLOAD];
    wr16(d, sport);
    wr16(d + 2, dport);
    wr16(d + 4, (uint16_t)(8 + len));
    wr16(d + 6, 0);
    memcpy(d + 8, data, len);

    /* checksum over the pseudo header + datagram */
    uint8_t ph[12];
    wr32(ph, nif->configured ? nif->ip : 0);
    wr32(ph + 4, dst);
    ph[8] = 0;
    ph[9] = IP_PROTO_UDP;
    wr16(ph + 10, (uint16_t)(8 + len));
    uint16_t cs = net_csum_fold(net_csum_add(net_csum_add(0, ph, 12), d, 8 + len));
    wr16(d + 6, cs ? cs : 0xFFFF);
    return ip_send(dst, IP_PROTO_UDP, d, 8 + len);
}

void udp_rx(ip4_t src, ip4_t dst, const uint8_t* d, uint32_t len) {
    if (len < 8) return;
    uint16_t ulen = rd16(d + 4);
    if (ulen < 8 || ulen > len) return;
    if (rd16(d + 6) != 0) {
        uint8_t ph[12];
        wr32(ph, src);
        wr32(ph + 4, dst);
        ph[8] = 0;
        ph[9] = IP_PROTO_UDP;
        wr16(ph + 10, ulen);
        if (net_csum_fold(net_csum_add(net_csum_add(0, ph, 12), d, ulen)) != 0) return;
    }
    uint16_t sport = rd16(d), dport = rd16(d + 2);
    for (int i = 0; i < UDP_BINDINGS; i++) {
        if (g_bind[i].handler && g_bind[i].port == dport) {
            g_bind[i].handler(src, sport, d + 8, ulen - 8u, g_bind[i].ctx);
            return;
        }
    }
}
