#include "tcp.h"
#include "kstring.h"
#include "kheap.h"
#include "timer.h"
#include "random.h"
#include "terminal.h"
#include "serial.h"

#define TCP_MAX_CONN  16
#define TCP_MAX_LISTEN 4
#define TCP_BACKLOG   4                 /* handshaking + not yet accepted, per listener */
#define TCP_MSS       1460              /* what we accept: 1500 - IP - TCP headers */
#define RBUF_SIZE     65536u            /* receive ring (window capped at 65535) */
#define SBUF_SIZE     32768u
#define OOO_MAX       32                /* out-of-order segments held per conn */
#define RTO_INIT      1000u
#define RTO_MIN       200u
#define RTO_MAX       60000u
#define MAX_RETRIES   10
#define SYN_RETRIES   6
#define TIME_WAIT_MS  2000u

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

#define SEQ_LT(a, b)  ((int32_t)((a) - (b)) < 0)
#define SEQ_LEQ(a, b) ((int32_t)((a) - (b)) <= 0)
#define SEQ_GT(a, b)  ((int32_t)((a) - (b)) > 0)
#define SEQ_GEQ(a, b) ((int32_t)((a) - (b)) >= 0)

typedef struct ooo_seg {
    struct ooo_seg* next;
    uint32_t seq, len;
    int      fin;
    uint8_t  data[];
} ooo_seg_t;

struct tcp_conn {
    int         used;
    tcp_state_t state;
    ip4_t       rip;
    uint16_t    lport, rport;

    /* send side. sbuf[0] is the byte at sequence snd_una (SYN/FIN are
     * sequence numbers only, never in the buffer) */
    uint32_t iss, snd_una, snd_nxt, snd_max, snd_wnd, snd_wl1, snd_wl2;
    uint32_t peer_mss, cwnd, ssthresh;
    uint8_t* sbuf;
    uint32_t sb_len;
    int      fin_pending, fin_sent;
    uint32_t fin_seq;

    /* receive side: ring buffer + out-of-order list sorted by seq */
    uint32_t irs, rcv_nxt;
    uint8_t* rbuf;
    uint32_t rb_head, rb_len;
    ooo_seg_t* ooo;
    int      ooo_count;
    int      fin_rcvd;

    /* timers: one retransmission timer, RTT estimation per RFC 6298 */
    int      timer_on;
    uint32_t rto, rto_deadline;
    int      retries;
    uint32_t srtt, rttvar;
    int      have_rtt, rtt_timing;
    uint32_t rtt_seq, rtt_start;
    uint32_t dupacks;
    uint32_t tw_deadline;

    int      ack_pending;
    uint32_t unacked_segs;
    uint32_t last_adv_wnd;

    int      user_closed;
    int      err;

    struct tcp_listener* listener;   /* passive open: not yet tcp_accept()ed */
};

struct tcp_listener {
    int      used;
    uint16_t port;
};

static tcp_conn_t g_conns[TCP_MAX_CONN];
static tcp_listener_t g_listeners[TCP_MAX_LISTEN];

const char* tcp_state_str(tcp_state_t s) {
    static const char* names[] = {
        "CLOSED", "SYN_SENT", "ESTABLISHED", "FIN_WAIT1", "FIN_WAIT2",
        "CLOSE_WAIT", "CLOSING", "LAST_ACK", "TIME_WAIT", "SYN_RCVD",
    };
    return ((unsigned)s < sizeof(names) / sizeof(names[0])) ? names[s] : "?";
}

tcp_state_t tcp_state(const tcp_conn_t* c) { return c->state; }

static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

static void conn_free(tcp_conn_t* c) {
    kfree(c->sbuf);
    kfree(c->rbuf);
    while (c->ooo) {
        ooo_seg_t* n = c->ooo->next;
        kfree(c->ooo);
        c->ooo = n;
    }
    memset(c, 0, sizeof(*c));
}

static uint32_t adv_window(const tcp_conn_t* c) {
    return min_u32(RBUF_SIZE - c->rb_len, 65535u);
}

/* ── segment output ─────────────────────────────────────────────── */

static void send_raw(ip4_t dst, uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack,
                     uint8_t flags, uint16_t wnd, const uint8_t* data, uint32_t len, int mss_opt) {
    static uint8_t seg[24 + TCP_MSS];   /* not reentrant; see net.h */
    netif_t* nif = net_if();
    uint32_t hl = mss_opt ? 24 : 20;
    wr16(seg, sport);
    wr16(seg + 2, dport);
    wr32(seg + 4, seq);
    wr32(seg + 8, ack);
    seg[12] = (uint8_t)((hl / 4) << 4);
    seg[13] = flags;
    wr16(seg + 14, wnd);
    wr16(seg + 16, 0);
    wr16(seg + 18, 0);
    if (mss_opt) {
        seg[20] = 2; seg[21] = 4;
        wr16(seg + 22, TCP_MSS);
    }
    if (len) memcpy(seg + hl, data, len);

    uint8_t ph[12];
    wr32(ph, nif->ip);
    wr32(ph + 4, dst);
    ph[8] = 0;
    ph[9] = IP_PROTO_TCP;
    wr16(ph + 10, (uint16_t)(hl + len));
    wr16(seg + 16, net_csum_fold(net_csum_add(net_csum_add(0, ph, 12), seg, hl + len)));
    ip_send(dst, IP_PROTO_TCP, seg, hl + len);
}

static void send_seg(tcp_conn_t* c, uint32_t seq, uint8_t flags, const uint8_t* data, uint32_t len) {
    uint32_t wnd = adv_window(c);
    send_raw(c->rip, c->lport, c->rport, seq, (flags & F_ACK) ? c->rcv_nxt : 0,
             flags, (uint16_t)wnd, data, len, (flags & F_SYN) != 0);
    if (flags & F_ACK) {
        c->ack_pending = 0;
        c->unacked_segs = 0;
        c->last_adv_wnd = wnd;
    }
}

static void arm_timer(tcp_conn_t* c) {
    c->timer_on = 1;
    c->rto_deadline = timer_ms() + c->rto;
}

/* Sends whatever the peer's window and our congestion window allow,
 * then the FIN once all data has gone out. */
static void tcp_output(tcp_conn_t* c) {
    if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT &&
        c->state != TCP_FIN_WAIT1 && c->state != TCP_CLOSING && c->state != TCP_LAST_ACK)
        return;
    for (;;) {
        uint32_t off = c->snd_nxt - c->snd_una;       /* bytes in flight */
        if (off >= c->sb_len) break;
        uint32_t wnd = min_u32(c->snd_wnd, c->cwnd);
        uint32_t usable = wnd > off ? wnd - off : 0;
        uint32_t n = min_u32(min_u32(c->sb_len - off, usable), c->peer_mss);
        if (n == 0) {
            /* zero window: the retransmission timer doubles as the
             * persist timer and will probe with one byte */
            if (!c->timer_on) arm_timer(c);
            break;
        }
        uint8_t flags = F_ACK;
        if (off + n == c->sb_len) flags |= F_PSH;
        send_seg(c, c->snd_nxt, flags, c->sbuf + off, n);
        if (!c->rtt_timing) {
            c->rtt_timing = 1;
            c->rtt_seq = c->snd_nxt;
            c->rtt_start = timer_ms();
        }
        c->snd_nxt += n;
        if (SEQ_GT(c->snd_nxt, c->snd_max)) c->snd_max = c->snd_nxt;
        if (!c->timer_on) arm_timer(c);
    }
    if (c->fin_pending && !c->fin_sent && c->snd_nxt - c->snd_una == c->sb_len) {
        c->fin_seq = c->snd_nxt;
        send_seg(c, c->snd_nxt, F_FIN | F_ACK, NULL, 0);
        c->fin_sent = 1;
        c->snd_nxt++;
        if (SEQ_GT(c->snd_nxt, c->snd_max)) c->snd_max = c->snd_nxt;
        if (c->state == TCP_ESTABLISHED) c->state = TCP_FIN_WAIT1;
        else if (c->state == TCP_CLOSE_WAIT) c->state = TCP_LAST_ACK;
        if (!c->timer_on) arm_timer(c);
    }
}

/* ── input ──────────────────────────────────────────────────────── */

static uint32_t rbuf_put(tcp_conn_t* c, const uint8_t* data, uint32_t len) {
    uint32_t space = RBUF_SIZE - c->rb_len;
    if (len > space) len = space;
    uint32_t tail = (c->rb_head + c->rb_len) % RBUF_SIZE;
    uint32_t first = min_u32(len, RBUF_SIZE - tail);
    memcpy(c->rbuf + tail, data, first);
    if (len > first) memcpy(c->rbuf, data + first, len - first);
    c->rb_len += len;
    return len;
}

static void fin_received(tcp_conn_t* c) {
    c->rcv_nxt++;
    c->fin_rcvd = 1;
    c->ack_pending = 1;
    c->unacked_segs = 2;     /* ack a FIN right away */
    switch (c->state) {
    case TCP_ESTABLISHED: c->state = TCP_CLOSE_WAIT; break;
    case TCP_FIN_WAIT1:
        /* our FIN not acked yet (otherwise we'd be in FIN_WAIT2) */
        c->state = TCP_CLOSING;
        break;
    case TCP_FIN_WAIT2:
        c->state = TCP_TIME_WAIT;
        c->tw_deadline = timer_ms() + TIME_WAIT_MS;
        c->timer_on = 0;
        break;
    default: break;
    }
}

/* in-order data at rcv_nxt; returns bytes accepted */
static uint32_t deliver(tcp_conn_t* c, const uint8_t* data, uint32_t len) {
    uint32_t n = c->user_closed ? len : rbuf_put(c, data, len);   /* closed: discard, still ack */
    c->rcv_nxt += n;
    return n;
}

static void ooo_store(tcp_conn_t* c, uint32_t seq, const uint8_t* data, uint32_t len, int fin) {
    if (c->ooo_count >= OOO_MAX) return;
    /* beyond what we advertised? drop */
    if (SEQ_GT(seq + len, c->rcv_nxt + adv_window(c))) return;
    ooo_seg_t** pp = &c->ooo;
    while (*pp && SEQ_LT((*pp)->seq, seq)) pp = &(*pp)->next;
    if (*pp && (*pp)->seq == seq && (*pp)->len >= len) return;   /* duplicate */
    ooo_seg_t* s = (ooo_seg_t*)kmalloc(sizeof(ooo_seg_t) + len);
    if (!s) return;
    s->seq = seq;
    s->len = len;
    s->fin = fin;
    memcpy(s->data, data, len);
    s->next = *pp;
    *pp = s;
    c->ooo_count++;
}

static void ooo_drain(tcp_conn_t* c) {
    while (c->ooo && SEQ_LEQ(c->ooo->seq, c->rcv_nxt)) {
        ooo_seg_t* s = c->ooo;
        c->ooo = s->next;
        c->ooo_count--;
        uint32_t end = s->seq + s->len;
        if (SEQ_GT(end, c->rcv_nxt)) {
            uint32_t skip = c->rcv_nxt - s->seq;
            deliver(c, s->data + skip, s->len - skip);
        }
        if (s->fin && c->rcv_nxt == end && !c->fin_rcvd) fin_received(c);
        kfree(s);
    }
}

static void update_rtt(tcp_conn_t* c, uint32_t sample) {
    if (!c->have_rtt) {
        c->srtt = sample;
        c->rttvar = sample / 2;
        c->have_rtt = 1;
    } else {
        uint32_t diff = c->srtt > sample ? c->srtt - sample : sample - c->srtt;
        c->rttvar = (3 * c->rttvar + diff) / 4;
        c->srtt = (7 * c->srtt + sample) / 8;
    }
    uint32_t rto = c->srtt + (4 * c->rttvar > 10 ? 4 * c->rttvar : 10);
    if (rto < RTO_MIN) rto = RTO_MIN;
    if (rto > RTO_MAX) rto = RTO_MAX;
    c->rto = rto;
}

static void parse_mss(tcp_conn_t* c, const uint8_t* opt, uint32_t len) {
    uint32_t i = 0;
    while (i < len) {
        if (opt[i] == 0) break;
        if (opt[i] == 1) { i++; continue; }
        if (i + 1 >= len || opt[i + 1] < 2) break;
        if (opt[i] == 2 && opt[i + 1] == 4 && i + 4 <= len) {
            uint32_t mss = rd16(opt + i + 2);
            if (mss >= 64) c->peer_mss = min_u32(mss, TCP_MSS);
        }
        i += opt[i + 1];
    }
}

static void send_reset_for(ip4_t src, uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack,
                           uint8_t flags, uint32_t seg_len) {
    if (flags & F_RST) return;
    if (flags & F_ACK) {
        send_raw(src, dport, sport, ack, 0, F_RST, 0, NULL, 0, 0);
    } else {
        uint32_t n = seg_len + ((flags & F_SYN) ? 1 : 0) + ((flags & F_FIN) ? 1 : 0);
        send_raw(src, dport, sport, 0, seq + n, F_RST | F_ACK, 0, NULL, 0, 0);
    }
}

static void ack_processing(tcp_conn_t* c, uint32_t seq, uint32_t ack, uint32_t wnd,
                           uint32_t seg_len, uint8_t flags) {
    if (SEQ_GT(ack, c->snd_una) && SEQ_LEQ(ack, c->snd_max)) {
        uint32_t acked = ack - c->snd_una;
        uint32_t data_acked = min_u32(acked, c->sb_len);
        if (data_acked) {
            memmove(c->sbuf, c->sbuf + data_acked, c->sb_len - data_acked);
            c->sb_len -= data_acked;
        }
        c->snd_una = ack;
        if (SEQ_LT(c->snd_nxt, c->snd_una)) c->snd_nxt = c->snd_una;

        if (c->rtt_timing && SEQ_GT(ack, c->rtt_seq)) {
            update_rtt(c, timer_ms() - c->rtt_start);
            c->rtt_timing = 0;
        }
        /* congestion window: slow start, then additive increase */
        if (c->cwnd < c->ssthresh) c->cwnd += min_u32(acked, c->peer_mss);
        else c->cwnd += (c->peer_mss * c->peer_mss) / (c->cwnd ? c->cwnd : 1);
        if (c->cwnd > 4u * 65535u) c->cwnd = 4u * 65535u;

        c->retries = 0;
        c->dupacks = 0;
        if (c->snd_una == c->snd_max) c->timer_on = 0;
        else arm_timer(c);

        if (c->fin_sent && SEQ_GT(ack, c->fin_seq)) {
            if (c->state == TCP_FIN_WAIT1) c->state = TCP_FIN_WAIT2;
            else if (c->state == TCP_CLOSING) {
                c->state = TCP_TIME_WAIT;
                c->tw_deadline = timer_ms() + TIME_WAIT_MS;
            } else if (c->state == TCP_LAST_ACK) {
                c->state = TCP_CLOSED;
            }
        }
    } else if (ack == c->snd_una && seg_len == 0 && !(flags & (F_SYN | F_FIN)) &&
               c->snd_max != c->snd_una && wnd == c->snd_wnd) {
        /* duplicate ACK: three in a row means a lost segment - resend it
         * now instead of waiting for the timeout (fast retransmit) */
        if (++c->dupacks == 3) {
            uint32_t flight = c->snd_max - c->snd_una;
            c->ssthresh = flight / 2 > 2 * c->peer_mss ? flight / 2 : 2 * c->peer_mss;
            c->cwnd = c->ssthresh;
            uint32_t n = min_u32(c->sb_len, c->peer_mss);
            if (n) send_seg(c, c->snd_una, F_ACK, c->sbuf, n);
            c->rtt_timing = 0;
        }
    }
    /* window update, taking only the newest segment's view (RFC 793) */
    if (SEQ_LT(c->snd_wl1, seq) || (c->snd_wl1 == seq && SEQ_LEQ(c->snd_wl2, ack))) {
        c->snd_wnd = wnd;
        c->snd_wl1 = seq;
        c->snd_wl2 = ack;
    }
}

static tcp_conn_t* conn_alloc(void) {
    tcp_conn_t* c = NULL;
    for (int i = 0; i < TCP_MAX_CONN; i++)
        if (!g_conns[i].used) { c = &g_conns[i]; break; }
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));
    c->sbuf = (uint8_t*)kmalloc(SBUF_SIZE);
    c->rbuf = (uint8_t*)kmalloc(RBUF_SIZE);
    if (!c->sbuf || !c->rbuf) {
        kfree(c->sbuf);
        kfree(c->rbuf);
        c->sbuf = c->rbuf = NULL;
        return NULL;
    }
    c->used = 1;
    c->iss = random_u32();
    c->snd_una = c->iss;
    c->snd_nxt = c->iss + 1;
    c->snd_max = c->snd_nxt;
    c->peer_mss = 536;                   /* RFC 1122 default until told */
    c->cwnd = 4 * TCP_MSS;
    c->ssthresh = 65535;
    c->rto = RTO_INIT;
    return c;
}

/* A SYN for a port somebody listens on: answer with SYN-ACK. The
 * connection waits in SYN_RCVD for the final ACK, then in the listener's
 * queue for tcp_accept(). Returns 0 if nobody listens on that port. */
static int passive_open(ip4_t src, uint16_t sport, uint16_t dport, uint32_t seq, uint32_t wnd,
                        const uint8_t* seg, uint32_t hl) {
    tcp_listener_t* l = NULL;
    for (int i = 0; i < TCP_MAX_LISTEN; i++)
        if (g_listeners[i].used && g_listeners[i].port == dport) { l = &g_listeners[i]; break; }
    if (!l) return 0;
    int pending = 0;
    for (int i = 0; i < TCP_MAX_CONN; i++)
        if (g_conns[i].used && g_conns[i].listener == l && g_conns[i].state != TCP_CLOSED) pending++;
    if (pending >= TCP_BACKLOG) return 1;       /* busy: drop, the client retries */

    tcp_conn_t* c = conn_alloc();
    if (!c) return 1;
    c->listener = l;
    c->rip = src;
    c->rport = sport;
    c->lport = dport;
    c->irs = seq;
    c->rcv_nxt = seq + 1;
    c->snd_wnd = wnd;
    c->snd_wl1 = seq;
    parse_mss(c, seg + 20, hl - 20);
    c->state = TCP_SYN_RCVD;
    send_seg(c, c->iss, F_SYN | F_ACK, NULL, 0);
    c->rtt_timing = 1;
    c->rtt_seq = c->iss;
    c->rtt_start = timer_ms();
    arm_timer(c);
    return 1;
}

void tcp_rx(ip4_t src, ip4_t dst, const uint8_t* seg, uint32_t len) {
    if (len < 20) return;
    uint8_t ph[12];
    wr32(ph, src);
    wr32(ph + 4, dst);
    ph[8] = 0;
    ph[9] = IP_PROTO_TCP;
    wr16(ph + 10, (uint16_t)len);
    if (net_csum_fold(net_csum_add(net_csum_add(0, ph, 12), seg, len)) != 0) return;

    uint16_t sport = rd16(seg), dport = rd16(seg + 2);
    uint32_t seq = rd32(seg + 4), ack = rd32(seg + 8);
    uint32_t hl = (uint32_t)(seg[12] >> 4) * 4u;
    uint8_t  flags = seg[13];
    uint32_t wnd = rd16(seg + 14);
    if (hl < 20 || hl > len) return;
    const uint8_t* data = seg + hl;
    uint32_t dlen = len - hl;

    tcp_conn_t* c = NULL;
    for (int i = 0; i < TCP_MAX_CONN; i++) {
        tcp_conn_t* k = &g_conns[i];
        if (k->used && k->state != TCP_CLOSED && k->lport == dport &&
            k->rport == sport && k->rip == src) { c = k; break; }
    }
    if (!c) {
        if ((flags & (F_SYN | F_ACK | F_RST)) == F_SYN && passive_open(src, sport, dport, seq, wnd, seg, hl))
            return;
        send_reset_for(src, sport, dport, seq, ack, flags, dlen);
        return;
    }

    if (c->state == TCP_SYN_RCVD) {
        if (flags & F_RST) { c->state = TCP_CLOSED; c->timer_on = 0; return; }
        if (flags & F_SYN) {                     /* our SYN-ACK got lost */
            send_seg(c, c->iss, F_SYN | F_ACK, NULL, 0);
            return;
        }
        if (!(flags & F_ACK)) return;
        if (ack != c->iss + 1) {
            send_reset_for(src, sport, dport, seq, ack, flags, dlen);
            return;
        }
        c->snd_una = ack;
        c->snd_wnd = wnd;
        c->snd_wl1 = seq;
        c->snd_wl2 = ack;
        if (c->rtt_timing) {
            update_rtt(c, timer_ms() - c->rtt_start);
            c->rtt_timing = 0;
        }
        c->retries = 0;
        c->timer_on = 0;
        c->cwnd = 4 * c->peer_mss;
        c->state = TCP_ESTABLISHED;
        /* the ACK may already carry data (or a FIN): handled below */
    }

    if (c->state == TCP_SYN_SENT) {
        if ((flags & F_ACK) && ack != c->iss + 1) {
            send_reset_for(src, sport, dport, seq, ack, flags, dlen);
            return;
        }
        if (flags & F_RST) {
            if (flags & F_ACK) {
                c->err = NET_ERR_REFUSED;
                c->state = TCP_CLOSED;
            }
            return;
        }
        if ((flags & F_SYN) && (flags & F_ACK)) {
            c->irs = seq;
            c->rcv_nxt = seq + 1;
            c->snd_una = ack;
            c->snd_wnd = wnd;
            c->snd_wl1 = seq;
            c->snd_wl2 = ack;
            parse_mss(c, seg + 20, hl - 20);
            if (c->rtt_timing) {
                update_rtt(c, timer_ms() - c->rtt_start);
                c->rtt_timing = 0;
            }
            c->retries = 0;
            c->timer_on = 0;
            c->state = TCP_ESTABLISHED;
            send_seg(c, c->snd_nxt, F_ACK, NULL, 0);
            tcp_output(c);
        }
        return;
    }

    if (flags & F_RST) {
        /* only honour resets that land inside our receive window */
        if (SEQ_GEQ(seq, c->rcv_nxt) && SEQ_LT(seq, c->rcv_nxt + adv_window(c) + 1)) {
            c->err = NET_ERR_RESET;
            c->state = TCP_CLOSED;
            c->timer_on = 0;
        }
        return;
    }
    if (flags & F_SYN) {
        /* a retransmitted SYN-ACK: our ACK got lost, repeat it */
        send_seg(c, c->snd_nxt, F_ACK, NULL, 0);
        return;
    }

    if (flags & F_ACK) ack_processing(c, seq, ack, wnd, dlen, flags);
    if (c->state == TCP_CLOSED) return;

    if (c->state == TCP_TIME_WAIT) {
        if (flags & F_FIN) {
            send_seg(c, c->snd_nxt, F_ACK, NULL, 0);
            c->tw_deadline = timer_ms() + TIME_WAIT_MS;
        }
        return;
    }

    int can_receive = (c->state == TCP_ESTABLISHED || c->state == TCP_FIN_WAIT1 ||
                       c->state == TCP_FIN_WAIT2);
    int fin = (flags & F_FIN) != 0;
    if (can_receive && (dlen || fin)) {
        if (seq == c->rcv_nxt) {
            uint32_t got = deliver(c, data, dlen);
            if (fin && got == dlen) fin_received(c);
            ooo_drain(c);
            c->ack_pending = 1;
            c->unacked_segs++;
        } else if (SEQ_LT(seq, c->rcv_nxt)) {
            uint32_t skip = c->rcv_nxt - seq;
            if (skip < dlen) {
                uint32_t got = deliver(c, data + skip, dlen - skip);
                if (fin && got == dlen - skip) fin_received(c);
                ooo_drain(c);
            } else if (fin && skip == dlen && !c->fin_rcvd) {
                fin_received(c);
            }
            /* (partial) duplicate: tell the sender where we are, now */
            c->ack_pending = 1;
            c->unacked_segs = 2;
        } else {
            ooo_store(c, seq, data, dlen, fin);
            c->ack_pending = 1;
            c->unacked_segs = 2;     /* immediate duplicate ACK */
        }
        /* ACK every second full segment right away (RFC 1122 4.2.3.2);
         * the rest wait for the end of the receive batch (tcp_flush) */
        if (c->unacked_segs >= 2) send_seg(c, c->snd_nxt, F_ACK, NULL, 0);
    }

    tcp_output(c);
}

/* ── timers / flush ─────────────────────────────────────────────── */

void tcp_flush(void) {
    for (int i = 0; i < TCP_MAX_CONN; i++) {
        tcp_conn_t* c = &g_conns[i];
        if (c->used && c->ack_pending && c->state != TCP_CLOSED && c->state != TCP_SYN_SENT &&
            c->state != TCP_SYN_RCVD)
            send_seg(c, c->snd_nxt, F_ACK, NULL, 0);
    }
}

static void on_timeout(tcp_conn_t* c) {
    int handshake = (c->state == TCP_SYN_SENT || c->state == TCP_SYN_RCVD);
    int limit = handshake ? SYN_RETRIES : MAX_RETRIES;
    if (++c->retries > limit) {
        c->err = NET_ERR_TIMEOUT;
        if (!handshake) send_seg(c, c->snd_nxt, F_RST | F_ACK, NULL, 0);
        c->state = TCP_CLOSED;
        c->timer_on = 0;
        return;
    }
    c->rtt_timing = 0;                  /* Karn: never time a retransmission */
    c->rto = min_u32(c->rto * 2, RTO_MAX);

    if (c->state == TCP_SYN_RCVD) {
        send_seg(c, c->iss, F_SYN | F_ACK, NULL, 0);
        arm_timer(c);
        return;
    }
    if (c->state == TCP_SYN_SENT) {
        send_seg(c, c->iss, F_SYN, NULL, 0);
        arm_timer(c);
        return;
    }
    if (c->snd_wnd == 0 && c->sb_len > c->snd_nxt - c->snd_una) {
        /* persist: probe the zero window with one byte */
        uint32_t off = c->snd_nxt - c->snd_una;
        send_seg(c, c->snd_nxt, F_ACK, c->sbuf + off, 1);
        arm_timer(c);
        return;
    }
    /* retransmission timeout: back to slow start from the first
     * unacknowledged byte (go-back-N, paced by cwnd) */
    uint32_t flight = c->snd_max - c->snd_una;
    c->ssthresh = flight / 2 > 2 * c->peer_mss ? flight / 2 : 2 * c->peer_mss;
    c->cwnd = c->peer_mss;
    c->snd_nxt = c->snd_una;
    if (c->fin_sent && SEQ_LEQ(c->snd_una, c->fin_seq)) c->fin_sent = 0;
    c->timer_on = 0;
    tcp_output(c);
    if (!c->timer_on && c->snd_una != c->snd_max) arm_timer(c);
}

void tcp_timer(void) {
    uint32_t now = timer_ms();
    for (int i = 0; i < TCP_MAX_CONN; i++) {
        tcp_conn_t* c = &g_conns[i];
        if (!c->used) continue;
        if (c->state == TCP_TIME_WAIT && (int32_t)(now - c->tw_deadline) >= 0)
            c->state = TCP_CLOSED;
        if (c->timer_on && c->state != TCP_CLOSED && (int32_t)(now - c->rto_deadline) >= 0)
            on_timeout(c);
        /* closed and nobody will look at it again: the user closed it, or
         * it died before tcp_accept() handed it out */
        if (c->state == TCP_CLOSED && (c->user_closed || c->listener)) conn_free(c);
    }
}

/* ── user API ───────────────────────────────────────────────────── */

static uint16_t pick_port(void) {
    for (;;) {
        uint16_t p = (uint16_t)(49152u + random_u32() % 16384u);
        int used = 0;
        for (int i = 0; i < TCP_MAX_CONN; i++)
            if (g_conns[i].used && g_conns[i].lport == p) used = 1;
        if (!used) return p;
    }
}

tcp_conn_t* tcp_connect(ip4_t ip, uint16_t port, uint32_t timeout_ms, int* err) {
    int e = NET_OK;
    if (!err) err = &e;
    netif_t* nif = net_if();
    if (!nif->dev) { *err = NET_ERR_NODEV; return NULL; }
    if (!net_wait_configured(timeout_ms)) { *err = NET_ERR_NOTREADY; return NULL; }

    tcp_conn_t* c = conn_alloc();
    if (!c) { *err = NET_ERR_NOMEM; return NULL; }
    c->rip = ip;
    c->rport = port;
    c->lport = pick_port();
    c->state = TCP_SYN_SENT;

    send_seg(c, c->iss, F_SYN, NULL, 0);
    c->rtt_timing = 1;
    c->rtt_seq = c->iss;
    c->rtt_start = timer_ms();
    arm_timer(c);

    uint32_t start = timer_ms();
    while (c->state == TCP_SYN_SENT) {
        if (timer_ms() - start >= timeout_ms) { *err = NET_ERR_TIMEOUT; break; }
        if (net_interrupted()) { *err = NET_ERR_INTR; break; }
        net_wait(20);
    }
    if (c->state == TCP_ESTABLISHED) {
        c->cwnd = 4 * c->peer_mss;
        return c;
    }
    if (*err == NET_OK) *err = c->err ? c->err : NET_ERR_REFUSED;
    conn_free(c);
    return NULL;
}

int tcp_send(tcp_conn_t* c, const void* data, uint32_t len, uint32_t timeout_ms) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t done = 0, start = timer_ms();
    while (done < len) {
        if (c->err) return c->err;
        if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT) return NET_ERR_CLOSED;
        uint32_t space = SBUF_SIZE - c->sb_len;
        if (space) {
            uint32_t n = min_u32(space, len - done);
            memcpy(c->sbuf + c->sb_len, p + done, n);
            c->sb_len += n;
            done += n;
            tcp_output(c);
            continue;
        }
        if (timer_ms() - start >= timeout_ms) return NET_ERR_TIMEOUT;
        if (net_interrupted()) return NET_ERR_INTR;
        net_wait(20);
    }
    return (int)len;
}

int tcp_recv(tcp_conn_t* c, void* buf, uint32_t max, uint32_t timeout_ms) {
    uint32_t start = timer_ms();
    for (;;) {
        if (c->rb_len) {
            uint32_t n = min_u32(max, c->rb_len);
            uint32_t first = min_u32(n, RBUF_SIZE - c->rb_head);
            memcpy(buf, c->rbuf + c->rb_head, first);
            if (n > first) memcpy((uint8_t*)buf + first, c->rbuf, n - first);
            c->rb_head = (c->rb_head + n) % RBUF_SIZE;
            c->rb_len -= n;
            /* the window reopened noticeably: tell the sender now rather
             * than letting it sit idle until its persist timer fires */
            uint32_t wnd = adv_window(c);
            if (c->state != TCP_CLOSED && wnd > c->last_adv_wnd &&
                (wnd - c->last_adv_wnd >= 2 * TCP_MSS || (c->last_adv_wnd < TCP_MSS && wnd >= TCP_MSS)))
                send_seg(c, c->snd_nxt, F_ACK, NULL, 0);
            return (int)n;
        }
        if (c->fin_rcvd) return 0;
        if (c->err) return c->err;
        if (c->state == TCP_CLOSED) return NET_ERR_CLOSED;
        if (timer_ms() - start >= timeout_ms) return NET_ERR_TIMEOUT;
        if (net_interrupted()) return NET_ERR_INTR;
        net_wait(20);
    }
}

void tcp_close(tcp_conn_t* c) {
    if (!c || !c->used) return;
    c->user_closed = 1;
    if (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT) {
        c->fin_pending = 1;
        tcp_output(c);
    } else if (c->state == TCP_SYN_SENT) {
        c->state = TCP_CLOSED;
    }
    if (c->state == TCP_CLOSED) conn_free(c);
}

void tcp_abort(tcp_conn_t* c) {
    if (!c || !c->used) return;
    if (c->state != TCP_CLOSED && c->state != TCP_SYN_SENT && c->state != TCP_TIME_WAIT)
        send_seg(c, c->snd_nxt, F_RST | F_ACK, NULL, 0);
    conn_free(c);
}

void tcp_dump(void) {
    terminal_writeln("Proto Local                 Foreign               State        Recv-Q Send-Q");
    int any = 0;
    netif_t* nif = net_if();
    for (int i = 0; i < TCP_MAX_CONN; i++) {
        tcp_conn_t* c = &g_conns[i];
        if (!c->used) continue;
        char lip[16], rip[16], l[24], r[24], line[100];
        ip4_to_str(nif->ip, lip);
        ip4_to_str(c->rip, rip);
        ksnprintf(l, sizeof(l), "%s:%u", lip, c->lport);
        ksnprintf(r, sizeof(r), "%s:%u", rip, c->rport);
        ksnprintf(line, sizeof(line), "tcp   %-21s %-21s %-12s %6u %6u",
                  l, r, tcp_state_str(c->state), c->rb_len, c->sb_len);
        terminal_writeln(line);
        any = 1;
    }
    for (int i = 0; i < TCP_MAX_LISTEN; i++) {
        if (!g_listeners[i].used) continue;
        char l[24], line[100];
        ksnprintf(l, sizeof(l), "0.0.0.0:%u", g_listeners[i].port);
        ksnprintf(line, sizeof(line), "tcp   %-21s %-21s %-12s", l, "*:*", "LISTEN");
        terminal_writeln(line);
        any = 1;
    }
    if (!any) terminal_writeln("(no connections)");
}

/* ── server side ────────────────────────────────────────────────── */

tcp_listener_t* tcp_listen(uint16_t port, int* err) {
    int e;
    if (!err) err = &e;
    for (int i = 0; i < TCP_MAX_LISTEN; i++)
        if (g_listeners[i].used && g_listeners[i].port == port) { *err = NET_ERR_PROTO; return NULL; }
    for (int i = 0; i < TCP_MAX_LISTEN; i++) {
        if (g_listeners[i].used) continue;
        g_listeners[i].used = 1;
        g_listeners[i].port = port;
        *err = NET_OK;
        return &g_listeners[i];
    }
    *err = NET_ERR_NOMEM;
    return NULL;
}

tcp_conn_t* tcp_accept(tcp_listener_t* l, uint32_t timeout_ms) {
    if (!l || !l->used) return NULL;
    uint32_t start = timer_ms();
    for (;;) {
        for (int i = 0; i < TCP_MAX_CONN; i++) {
            tcp_conn_t* c = &g_conns[i];
            if (c->used && c->listener == l &&
                (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT)) {
                c->listener = NULL;             /* the caller owns it now */
                return c;
            }
        }
        if (timer_ms() - start >= timeout_ms) return NULL;
        if (net_interrupted()) return NULL;
        net_wait(20);
    }
}

void tcp_unlisten(tcp_listener_t* l) {
    if (!l || !l->used) return;
    for (int i = 0; i < TCP_MAX_CONN; i++) {
        tcp_conn_t* c = &g_conns[i];
        if (c->used && c->listener == l) {
            if (c->state != TCP_CLOSED) send_seg(c, c->snd_nxt, F_RST | F_ACK, NULL, 0);
            conn_free(c);
        }
    }
    l->used = 0;
}

void tcp_peer(const tcp_conn_t* c, ip4_t* ip, uint16_t* port) {
    if (ip) *ip = c->rip;
    if (port) *port = c->rport;
}

int tcp_readable(const tcp_conn_t* c) {
    if (c->rb_len) return (int)c->rb_len;
    if (c->fin_rcvd || c->err || c->state == TCP_CLOSED) return -1;
    return 0;
}

uint32_t tcp_send_space(const tcp_conn_t* c) {
    if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT) return 0;
    return SBUF_SIZE - c->sb_len;
}
