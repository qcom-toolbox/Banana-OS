#ifndef TCP_H
#define TCP_H

#include "net.h"

/*
 * TCP (RFC 793 / 1122 / 6298): active and passive open, reliable
 * in-order byte stream with retransmission (RTT-estimated RTO with
 * backoff, fast retransmit on 3 duplicate ACKs), flow control, out-of-
 * order segment reassembly, delayed ACKs and orderly close.
 *
 * The API is blocking and meant for shell-command tasks: every wait
 * polls the stack itself (net_wait()), so it works whether or not netd
 * gets scheduled in between.
 */

typedef struct tcp_conn tcp_conn_t;

typedef enum {
    TCP_CLOSED = 0, TCP_SYN_SENT, TCP_ESTABLISHED, TCP_FIN_WAIT1, TCP_FIN_WAIT2,
    TCP_CLOSE_WAIT, TCP_CLOSING, TCP_LAST_ACK, TCP_TIME_WAIT, TCP_SYN_RCVD
} tcp_state_t;

/* Opens a connection. Returns NULL and sets *err (NET_ERR_*) on failure. */
tcp_conn_t* tcp_connect(ip4_t ip, uint16_t port, uint32_t timeout_ms, int* err);

/* Queues all `len` bytes (blocking while the send buffer is full).
 * Returns len, or a NET_ERR_* code. */
int tcp_send(tcp_conn_t* c, const void* data, uint32_t len, uint32_t timeout_ms);

/* Receives up to `max` bytes. Returns >0 bytes, 0 at end of stream
 * (peer closed), or a NET_ERR_* code (timeout, reset, interrupted). */
int tcp_recv(tcp_conn_t* c, void* buf, uint32_t max, uint32_t timeout_ms);

/* Graceful close (FIN after queued data). The connection object belongs
 * to the stack afterwards and is freed once the close completes. */
void tcp_close(tcp_conn_t* c);

/* Immediate close with RST. */
void tcp_abort(tcp_conn_t* c);

/* ── server side (passive open) ─────────────────────────────────
 * tcp_listen() starts answering SYNs on a local port; connections that
 * complete the handshake queue up (a few per listener) until
 * tcp_accept() hands them out. Accepted connections are used exactly
 * like tcp_connect() ones. */
typedef struct tcp_listener tcp_listener_t;
tcp_listener_t* tcp_listen(uint16_t port, int* err);
/* waits up to timeout_ms (0: just checks) for a connection; NULL if none */
tcp_conn_t* tcp_accept(tcp_listener_t* l, uint32_t timeout_ms);
void tcp_unlisten(tcp_listener_t* l);   /* also resets unaccepted connections */
void tcp_peer(const tcp_conn_t* c, ip4_t* ip, uint16_t* port);
/* bytes waiting to be read; -1 once nothing more can arrive (FIN/RST) */
int  tcp_readable(const tcp_conn_t* c);
/* bytes that can be queued with tcp_send() without blocking */
uint32_t tcp_send_space(const tcp_conn_t* c);

tcp_state_t tcp_state(const tcp_conn_t* c);
const char* tcp_state_str(tcp_state_t s);

void tcp_dump(void);     /* netstat */

#endif
