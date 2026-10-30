/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tools/test_tcp.py: net/tcp.c on Linux, both ends of a connection in one
 * process (as the OS's loopback: 127.0.0.1 to itself), over a simulated
 * link - a one-way delay, a bandwidth, a bottleneck queue and random loss,
 * on a virtual clock. Downloads are checked byte for byte and timed in
 * virtual seconds. Built twice: against the new tcp.c (-DNEW_TCP: window
 * scaling, SACK) and the one before it from git. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tcp.h"

/* ── the simulated world ── */
static uint64_t g_us;                           /* the virtual clock */
static uint32_t g_rand = 2463534242u;
static uint32_t xs(void) { g_rand ^= g_rand << 13; g_rand ^= g_rand >> 17; g_rand ^= g_rand << 5; return g_rand; }

uint32_t timer_ms(void) { return (uint32_t)(g_us / 1000); }
uint32_t random_u32(void) { return xs(); }
void terminal_writeln(const char* s) { (void)s; }

static netif_t g_if = { (void*)1, IP4(10, 0, 2, 15), 1 };
netif_t* net_if(void) { return &g_if; }
int  net_wait_configured(uint32_t t) { (void)t; return 1; }
int  net_interrupted(void) { return 0; }
uint32_t net_csum_add(uint32_t sum, const void* data, uint32_t len) {
    const uint8_t* p = (const uint8_t*)data;
    while (len > 1) { sum += (uint32_t)((p[0] << 8) | p[1]); p += 2; len -= 2; }
    if (len) sum += (uint32_t)(p[0] << 8);
    return sum;
}
uint16_t net_csum_fold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}
void ip4_to_str(ip4_t ip, char out[16]) { snprintf(out, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255); }
int ip6_is_loopback(const ip6_t* ip) {
    for (int i=0;i<15;i++) if (ip->b[i]) return 0;
    return ip->b[15]==1;
}
int  ipv6_enabled(void) { return 0; }
int  ipv6_source(const ip6_t* d, ip6_t* s) { (void)d; (void)s; return NET_ERR_NOROUTE; }
int  ipv6_send(const ip6_t* s, const ip6_t* d, uint8_t p, const void* x, uint32_t n) { (void)s; (void)d; (void)p; (void)x; (void)n; return 0; }
uint16_t ip6_checksum(const ip6_t* s, const ip6_t* d, uint8_t p, const void* x, uint32_t n) { (void)s; (void)d; (void)p; (void)x; (void)n; return 0; }
void ip6_format(const ip6_t* a, char* out, int cap) { (void)a; snprintf(out, (size_t)cap, "::"); }

/* the link: one per direction (by the segment's source port) */
typedef struct {
    uint64_t free_us;                           /* when the wire is free again */
    uint32_t sent, lost, queue_drops;
} dir_t;
static struct { uint32_t delay_us, bytes_per_ms, loss_per_10k, queue_us; } L;
static dir_t g_dir[2];
static uint16_t g_server_port;

typedef struct pkt { struct pkt* next; uint64_t at; uint32_t len; uint8_t d[]; } pkt_t;
static pkt_t* g_q;                              /* sorted by arrival */

int ip_send(ip4_t dst, uint8_t proto, const void* payload, uint32_t len) {
    (void)dst; (void)proto;
    dir_t* d = &g_dir[rd16((const uint8_t*)payload) == g_server_port];
    d->sent++;
    uint64_t start = d->free_us > g_us ? d->free_us : g_us;
    if (start - g_us > L.queue_us) { d->queue_drops++; return 0; }      /* the bottleneck's queue is full */
    d->free_us = start + (uint64_t)(len + 40) * 1000 / L.bytes_per_ms;
    if (xs() % 10000 < L.loss_per_10k) { d->lost++; return 0; }
    pkt_t* p = malloc(sizeof(pkt_t) + len);
    p->at = d->free_us + L.delay_us;
    p->len = len;
    memcpy(p->d, payload, len);
    pkt_t** pp = &g_q;
    while (*pp && (*pp)->at <= p->at) pp = &(*pp)->next;
    p->next = *pp;
    *pp = p;
    return 0;
}

static uint64_t g_last_timer;

/* as the OS's: deliver what arrived, the timers every 10 ms, then the
 * delayed ACKs - waiting (the clock jumps) only if nothing is due */
void net_wait(uint32_t max_ms) {
    if (!(g_q && g_q->at <= g_us)) {
        uint64_t next = g_us + (uint64_t)max_ms * 1000, tick = g_last_timer + 10000;
        if (g_q && g_q->at < next) next = g_q->at;
        if (tick < next) next = tick;
        if (next > g_us) g_us = next;
    }
    while (g_q && g_q->at <= g_us) {
        pkt_t* p = g_q;
        g_q = p->next;
        tcp_rx(IP4(127, 0, 0, 1), IP4(127, 0, 0, 1), p->d, p->len);
        free(p);
    }
    if (g_us - g_last_timer >= 10000) { g_last_timer = g_us; tcp_timer(); }
    tcp_flush();
}

/* ── the test ── */
static int g_fail;
static uint8_t byte_at(uint32_t i) { return (uint8_t)(i * 7u + (i >> 11)); }

typedef struct { double mbps; int ok; uint32_t rcv_window; int ws, sack; } result_t;

static result_t download(uint16_t port, uint32_t total, uint32_t delay_ms, uint32_t mbit, uint32_t loss_per_10k) {
    result_t r = { 0, 0, 0, 0, 0 };
    L.delay_us = delay_ms * 1000;
    L.bytes_per_ms = mbit * 125;
    L.loss_per_10k = loss_per_10k;
    L.queue_us = 2 * L.delay_us + 20000;        /* (a bottleneck queue of about a round trip) */
    memset(g_dir, 0, sizeof(g_dir));
    g_server_port = port;
    int err;
    tcp_listener_t* l = tcp_listen(port, &err);
    uint32_t saved_loss = L.loss_per_10k;
    L.loss_per_10k = 0;                         /* (the handshake gets through) */
    tcp_conn_t* c = tcp_connect(IP4(127, 0, 0, 1), port, 5000, &err);
    tcp_conn_t* s = l ? tcp_accept(l, 1000) : NULL;
    L.loss_per_10k = saved_loss;
    if (!c || !s) { printf("FAIL  connect (err %d)\n", err); g_fail++; return r; }
#ifdef NEW_TCP
    tcp_info_t ci;
    tcp_info(c, &ci);
    r.ws = ci.wscale; r.sack = ci.sack; r.rcv_window = ci.rcv_window;
#endif
    static uint8_t buf[65536];
    uint32_t sent = 0, got = 0;
    uint64_t t0 = g_us, limit = g_us + 900ull * 1000000;
    int bad = 0;
    while (got < total && g_us < limit && !bad) {
        uint32_t space = tcp_send_space(s);
        if (sent < total && space) {
            uint32_t n = total - sent < space ? total - sent : space;
            if (n > sizeof(buf)) n = sizeof(buf);
            for (uint32_t k = 0; k < n; k++) buf[k] = byte_at(sent + k);
            if (tcp_send(s, buf, n, 0) != (int)n) { printf("FAIL  tcp_send\n"); bad = 1; break; }
            sent += n;
        }
        int n = tcp_recv(c, buf, sizeof(buf), 0);
        if (n > 0) {
            for (int k = 0; k < n; k++) if (buf[k] != byte_at(got + (uint32_t)k)) { printf("FAIL  byte %u wrong\n", got + k); bad = 1; break; }
            got += (uint32_t)n;
            continue;
        }
        if (n != NET_ERR_TIMEOUT) { printf("FAIL  tcp_recv: %d after %u bytes\n", n, got); bad = 1; break; }
        net_wait(1);
    }
    double secs = (double)(g_us - t0) / 1e6;
    r.ok = !bad && got == total;
    r.mbps = secs > 0 ? got / secs / 1e6 : 0;
    if (!r.ok && !bad) printf("FAIL  only %u of %u bytes in %.0f s\n", got, total, secs);
    if (!r.ok) g_fail++;
    tcp_abort(c);
    tcp_abort(s);
    tcp_unlisten(l);
    while (g_q) { pkt_t* p = g_q; g_q = p->next; free(p); }
    return r;
}

int main(void) {
    static const struct { uint32_t delay, mbit, loss, mib; } sc[] = {
        { 1, 100, 0, 16 }, { 25, 100, 0, 16 }, { 50, 100, 0, 16 }, { 50, 100, 50, 16 }, { 50, 100, 200, 8 },
        { 150, 50, 10, 8 },
    };
    uint16_t port = 8000;
    for (unsigned i = 0; i < sizeof(sc) / sizeof(sc[0]); i++) {
        result_t r = download(port++, sc[i].mib << 20, sc[i].delay, sc[i].mbit, sc[i].loss);
        printf("RESULT %u %u %u %u %.3f %d %u %d %d\n", sc[i].delay * 2, sc[i].mbit, sc[i].loss, sc[i].mib,
               r.mbps, r.ok, r.rcv_window, r.ws, r.sack);
    }
#ifdef NEW_TCP
    tcp_stats_t st;
    tcp_stats(&st);
    printf("STATS fast %u sack %u rto %u sack_acks %u\n", st.fast_rexmit, st.sack_rexmit, st.rto_rexmit, st.sack_acks);
#endif
    return g_fail ? 1 : 0;
}
