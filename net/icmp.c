#include "net.h"
#include "kstring.h"
#include "timer.h"

/* ICMP (RFC 792): answers echo requests, and records echo replies (plus
 * unreachable / time-exceeded errors about our own echoes) for `ping`. */

#define EV_MAX 16
static icmp_event_t g_ev[EV_MAX];
static int g_ev_head, g_ev_count;

static void push_event(const icmp_event_t* e) {
    int idx = (g_ev_head + g_ev_count) % EV_MAX;
    if (g_ev_count == EV_MAX) {           /* full: overwrite the oldest */
        g_ev_head = (g_ev_head + 1) % EV_MAX;
        g_ev_count--;
    }
    g_ev[idx] = *e;
    g_ev_count++;
}

int icmp_poll_event(uint16_t id, icmp_event_t* out) {
    for (int i = 0; i < g_ev_count; i++) {
        int idx = (g_ev_head + i) % EV_MAX;
        if (g_ev[idx].id != id) continue;
        *out = g_ev[idx];
        /* remove by shifting the later ones down */
        for (int j = i; j < g_ev_count - 1; j++)
            g_ev[(g_ev_head + j) % EV_MAX] = g_ev[(g_ev_head + j + 1) % EV_MAX];
        g_ev_count--;
        return 1;
    }
    return 0;
}

int icmp_echo_send(ip4_t dst, uint16_t id, uint16_t seq, uint32_t data_len) {
    uint8_t msg[8 + 1472];
    if (data_len > 1472) data_len = 1472;
    msg[0] = 8;                            /* echo request */
    msg[1] = 0;
    wr16(msg + 2, 0);
    wr16(msg + 4, id);
    wr16(msg + 6, seq);
    /* payload: send timestamp, then a recognizable pattern */
    for (uint32_t i = 0; i < data_len; i++) msg[8 + i] = (uint8_t)(0x10 + i);
    if (data_len >= 4) wr32(msg + 8, timer_ms());
    wr16(msg + 2, net_checksum(msg, 8 + data_len));
    return ip_send(dst, IP_PROTO_ICMP, msg, 8 + data_len);
}

void icmp_rx(ip4_t src, const uint8_t* msg, uint32_t len, uint8_t ttl) {
    if (len < 8 || net_checksum(msg, len) != 0) return;
    uint8_t type = msg[0];

    if (type == 8) {                       /* echo request -> reply in kind */
        uint8_t reply[8 + 1472];
        if (len > sizeof(reply)) return;
        memcpy(reply, msg, len);
        reply[0] = 0;
        wr16(reply + 2, 0);
        wr16(reply + 2, net_checksum(reply, len));
        ip_send(src, IP_PROTO_ICMP, reply, len);
        return;
    }

    icmp_event_t e;
    memset(&e, 0, sizeof(e));
    e.src = src;
    e.ttl = ttl;
    e.type = type;
    e.recv_ms = timer_ms();
    if (type == 0) {                       /* echo reply */
        e.id = rd16(msg + 4);
        e.seq = rd16(msg + 6);
        e.bytes = len;
        push_event(&e);
    } else if ((type == 3 || type == 11) && len >= 8 + 20 + 8) {
        /* error: the offending datagram's header + 8 bytes follow; only
         * report it if it was one of our echo requests */
        const uint8_t* inner = msg + 8;
        uint32_t ihl = (uint32_t)(inner[0] & 0x0F) * 4u;
        if (inner[9] != IP_PROTO_ICMP || len < 8 + ihl + 8) return;
        const uint8_t* echo = inner + ihl;
        if (echo[0] != 8) return;
        e.id = rd16(echo + 4);
        e.seq = rd16(echo + 6);
        push_event(&e);
    }
}
