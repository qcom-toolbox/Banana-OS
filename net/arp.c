#include "net.h"
#include "kstring.h"
#include "kheap.h"
#include "timer.h"
#include "terminal.h"

/* ARP (RFC 826) for IPv4 over Ethernet. */

#define ARP_CACHE      16
#define ARP_TTL_MS     (10u * 60u * 1000u)
#define ARP_RETRY_MS   1000u
#define ARP_PENDING    16
#define ARP_PENDING_MS 3000u

typedef struct {
    ip4_t    ip;
    uint8_t  mac[6];
    uint32_t updated_ms;
    int      valid;
} arp_entry_t;

typedef struct {
    ip4_t    next_hop;
    uint8_t* pkt;
    uint32_t len;
    uint32_t queued_ms;
    uint32_t last_req_ms;
} arp_pending_t;

static arp_entry_t   g_cache[ARP_CACHE];
static arp_pending_t g_pending[ARP_PENDING];

static void arp_send(uint16_t op, const uint8_t dst_mac[6], const uint8_t target_mac[6], ip4_t target_ip) {
    netif_t* nif = net_if();
    uint8_t p[28];
    wr16(p, 1);              /* hardware: Ethernet */
    wr16(p + 2, ETH_TYPE_IP);
    p[4] = 6;
    p[5] = 4;
    wr16(p + 6, op);
    memcpy(p + 8, nif->dev->mac, 6);
    wr32(p + 14, nif->ip);
    memcpy(p + 18, target_mac, 6);
    wr32(p + 24, target_ip);
    eth_send(dst_mac, ETH_TYPE_ARP, p, sizeof(p));
}

void arp_request(ip4_t ip) {
    static const uint8_t zero[6] = { 0 };
    if (!net_if()->dev) return;
    arp_send(1, ETH_BROADCAST, zero, ip);
}

static void cache_put(ip4_t ip, const uint8_t mac[6]) {
    int slot = -1;
    uint32_t oldest = 0xFFFFFFFFu;
    for (int i = 0; i < ARP_CACHE; i++) {
        if (g_cache[i].valid && g_cache[i].ip == ip) { slot = i; break; }
    }
    if (slot < 0) {
        for (int i = 0; i < ARP_CACHE; i++) {
            if (!g_cache[i].valid) { slot = i; break; }
            if (g_cache[i].updated_ms < oldest) { oldest = g_cache[i].updated_ms; slot = i; }
        }
    }
    g_cache[slot].ip = ip;
    memcpy(g_cache[slot].mac, mac, 6);
    g_cache[slot].updated_ms = timer_ms();
    g_cache[slot].valid = 1;
}

int arp_lookup(ip4_t ip, uint8_t mac_out[6]) {
    for (int i = 0; i < ARP_CACHE; i++) {
        if (g_cache[i].valid && g_cache[i].ip == ip) {
            memcpy(mac_out, g_cache[i].mac, 6);
            return 1;
        }
    }
    return 0;
}

static void flush_pending(ip4_t ip, const uint8_t mac[6]) {
    for (int i = 0; i < ARP_PENDING; i++) {
        arp_pending_t* p = &g_pending[i];
        if (p->pkt && p->next_hop == ip) {
            eth_send(mac, ETH_TYPE_IP, p->pkt, p->len);
            kfree(p->pkt);
            p->pkt = NULL;
        }
    }
}

void arp_queue_packet(ip4_t next_hop, const uint8_t* ip_pkt, uint32_t len) {
    int slot = -1;
    int already_asking = 0;
    for (int i = 0; i < ARP_PENDING; i++) {
        if (g_pending[i].pkt && g_pending[i].next_hop == next_hop) already_asking = 1;
        if (!g_pending[i].pkt && slot < 0) slot = i;
    }
    if (slot < 0) return;   /* queue full: drop, upper layers retransmit */
    uint8_t* copy = (uint8_t*)kmalloc(len);
    if (!copy) return;
    memcpy(copy, ip_pkt, len);
    g_pending[slot].next_hop = next_hop;
    g_pending[slot].pkt = copy;
    g_pending[slot].len = len;
    g_pending[slot].queued_ms = timer_ms();
    g_pending[slot].last_req_ms = timer_ms();
    if (!already_asking) arp_request(next_hop);
}

void arp_rx(const uint8_t* p, uint32_t len) {
    netif_t* nif = net_if();
    if (len < 28) return;
    if (rd16(p) != 1 || rd16(p + 2) != ETH_TYPE_IP || p[4] != 6 || p[5] != 4) return;
    uint16_t op = rd16(p + 6);
    const uint8_t* sender_mac = p + 8;
    ip4_t sender_ip = rd32(p + 14);
    ip4_t target_ip = rd32(p + 24);

    if (sender_ip != 0) {
        /* learn from requests aimed at us and from every reply; refresh
         * entries we already have from anything we overhear */
        uint8_t dummy[6];
        if (op == 2 || target_ip == nif->ip || arp_lookup(sender_ip, dummy))
            cache_put(sender_ip, sender_mac);
        flush_pending(sender_ip, sender_mac);
    }
    if (op == 1 && nif->configured && target_ip == nif->ip) {
        arp_send(2, sender_mac, sender_mac, sender_ip);
    }
}

void arp_timer(void) {
    uint32_t now = timer_ms();
    for (int i = 0; i < ARP_CACHE; i++) {
        if (g_cache[i].valid && now - g_cache[i].updated_ms > ARP_TTL_MS) g_cache[i].valid = 0;
    }
    for (int i = 0; i < ARP_PENDING; i++) {
        arp_pending_t* p = &g_pending[i];
        if (!p->pkt) continue;
        if (now - p->queued_ms > ARP_PENDING_MS) {
            kfree(p->pkt);
            p->pkt = NULL;
        } else if (now - p->last_req_ms > ARP_RETRY_MS) {
            p->last_req_ms = now;
            arp_request(p->next_hop);
        }
    }
}

void arp_dump(void) {
    int any = 0;
    terminal_writeln("Address          HWaddress           Age(s)");
    for (int i = 0; i < ARP_CACHE; i++) {
        if (!g_cache[i].valid) continue;
        char ip[16], line[64];
        ip4_to_str(g_cache[i].ip, ip);
        const uint8_t* m = g_cache[i].mac;
        ksnprintf(line, sizeof(line), "%-16s %02x:%02x:%02x:%02x:%02x:%02x   %u",
                  ip, m[0], m[1], m[2], m[3], m[4], m[5],
                  (timer_ms() - g_cache[i].updated_ms) / 1000u);
        terminal_writeln(line);
        any = 1;
    }
    if (!any) terminal_writeln("(empty)");
}

void arp_flush(void) {
    for (int i = 0; i < ARP_CACHE; i++) g_cache[i].valid = 0;
    for (int i = 0; i < ARP_PENDING; i++) {
        kfree(g_pending[i].pkt);
        g_pending[i].pkt = NULL;
    }
}
