#ifndef TLS_H
#define TLS_H

#include "tcp.h"

/*
 * TLS 1.3 client (RFC 8446) over a connected TCP stream, for HTTPS.
 *
 * Key exchange: X25519. Ciphers: TLS_AES_128_GCM_SHA256 and
 * TLS_CHACHA20_POLY1305_SHA256. The handshake transcript, Finished MACs
 * and all record protection are fully verified - but the server's
 * certificate chain is NOT validated (Banana OS ships no CA store), so
 * this protects against eavesdropping, not against an active
 * man-in-the-middle. Equivalent to `curl --insecure`.
 */

typedef struct tls_conn tls_conn_t;

/* ciphers to offer (curl --tls13-ciphers) */
#define TLS_CIPHERS_ALL    0
#define TLS_CIPHERS_AES    1   /* TLS_AES_128_GCM_SHA256 only */
#define TLS_CIPHERS_CHACHA 2   /* TLS_CHACHA20_POLY1305_SHA256 only */

/* Performs the handshake. On failure returns NULL, sets *err (NET_ERR_*)
 * and writes a human readable reason into errmsg. `tcp` stays owned by
 * the caller either way. */
tls_conn_t* tls_connect(tcp_conn_t* tcp, const char* server_name, int ciphers, uint32_t timeout_ms,
                        int* err, char* errmsg, uint32_t errmsg_len);

/* >0 bytes, 0 = orderly end of stream (close_notify or TCP FIN), <0 NET_ERR_* */
int  tls_read(tls_conn_t* t, void* buf, uint32_t max, uint32_t timeout_ms);
/* returns len or NET_ERR_* */
int  tls_write(tls_conn_t* t, const void* data, uint32_t len, uint32_t timeout_ms);
/* sends close_notify and frees the TLS state (not the TCP connection) */
void tls_close(tls_conn_t* t);

const char* tls_cipher_name(const tls_conn_t* t);

#endif
