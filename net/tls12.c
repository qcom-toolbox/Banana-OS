/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tls12.h"
#include "sha256.h"
#include "x25519.h"
#include "p256.h"
#include "aead.h"
#include "kstring.h"
#include "kheap.h"
#include "random.h"
#include "serial.h"
#include "task.h"
#include "timer.h"

/* TLS 1.2 client and server - see tls12.h. Section numbers: RFC 5246. */

#define SUITE_ECDHE_RSA_AES128_GCM   0xC02F
#define SUITE_ECDHE_ECDSA_AES128_GCM 0xC02B
#define SUITE_EMPTY_RENEGOTIATION    0x00FF

#define CT_CCS       20
#define CT_ALERT     21
#define CT_HANDSHAKE 22
#define CT_APPDATA   23

#define HS_CLIENT_HELLO   1
#define HS_SERVER_HELLO   2
#define HS_CERTIFICATE    11
#define HS_SERVER_KX      12
#define HS_CERT_REQUEST   13
#define HS_SERVER_DONE    14
#define HS_CLIENT_KX      16
#define HS_FINISHED       20

#define EXT_SNI           0x0000
#define EXT_GROUPS        0x000a
#define EXT_POINT_FORMATS 0x000b
#define EXT_SIGALGS       0x000d
#define EXT_EMS           0x0017
#define EXT_RENEGOTIATION 0xff01

#define GROUP_X25519      0x001d
#define GROUP_SECP256R1   0x0017
#define SIG_RSA_PKCS1_SHA256 0x0401

#define MAX_PLAINTEXT  16384u
#define MAX_RECORD     (MAX_PLAINTEXT + 2048u)
#define IN_CAP         (5u + MAX_RECORD)

typedef struct {
    int       on;
    aes_gcm_t gcm;
    uint8_t   salt[4];
    uint64_t  seq;
} dir_t;

struct tls12_conn {
    tcp_conn_t* tcp;
    int         server;
    uint32_t    timeout_ms;
    uint8_t     crand[32], srand[32];
    uint8_t     master[48];
    int         ems;                    /* extended master secret */
    dir_t       rx, tx;
    sha256_ctx_t th;                    /* handshake transcript */

    uint8_t*    in;                     /* raw bytes from TCP */
    uint32_t    in_len;
    uint8_t*    pt;                     /* the current record's plaintext */
    uint32_t    pt_pos, pt_len;
    int         eof;

    uint8_t*    hs;                     /* handshake reassembly */
    uint32_t    hs_len, hs_cap;

    uint8_t*    cert;                   /* the peer's (client side) */
    uint32_t    cert_len;
    const uint8_t* pubkey;
    uint32_t    pubkey_len;

    char*       errmsg;
    uint32_t    errmsg_len;
};

static void fail(tls12_conn_t* t, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void fail(tls12_conn_t* t, const char* fmt, ...) {
    if (!t->errmsg || !t->errmsg_len || t->errmsg[0]) return;   /* the first reason wins */
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvsnprintf(t->errmsg, t->errmsg_len, fmt, ap);
    __builtin_va_end(ap);
}

/* ── PRF (section 5) ──────────────────────────────────────────────── */

void tls12_prf(const uint8_t* secret, uint32_t slen, const char* label, const uint8_t* seed, uint32_t seed_len,
               uint8_t* out, uint32_t out_len) {
    /* P_SHA256(secret, label || seed): A(i) = HMAC(secret, A(i-1)),
     * output HMAC(secret, A(i) || label || seed) */
    uint32_t ll = (uint32_t)strlen(label);
    uint8_t a[32], blk[32];
    hmac_sha256_ctx_t h;
    hmac_sha256_init(&h, secret, slen);
    hmac_sha256_update(&h, label, ll);
    hmac_sha256_update(&h, seed, seed_len);
    hmac_sha256_final(&h, a);
    uint32_t done = 0;
    while (done < out_len) {
        hmac_sha256_init(&h, secret, slen);
        hmac_sha256_update(&h, a, 32);
        hmac_sha256_update(&h, label, ll);
        hmac_sha256_update(&h, seed, seed_len);
        hmac_sha256_final(&h, blk);
        uint32_t n = out_len - done < 32 ? out_len - done : 32;
        memcpy(out + done, blk, n);
        done += n;
        hmac_sha256(secret, slen, a, 32, a);
    }
    memset(blk, 0, sizeof(blk));
}

static void th_hash(const tls12_conn_t* t, uint8_t out[32]) {
    sha256_ctx_t c = t->th;
    sha256_final(&c, out);
}

/* master secret from the X25519 shared secret (section 8.1, RFC 7627) */
static void make_master(tls12_conn_t* t, const uint8_t pms[32]) {
    if (t->ems) {
        uint8_t sh[32];
        th_hash(t, sh);                      /* through ClientKeyExchange */
        tls12_prf(pms, 32, "extended master secret", sh, 32, t->master, 48);
    } else {
        uint8_t seed[64];
        memcpy(seed, t->crand, 32);
        memcpy(seed + 32, t->srand, 32);
        tls12_prf(pms, 32, "master secret", seed, 64, t->master, 48);
    }
}

/* key block: client key, server key, client salt, server salt (section 6.3) */
static void make_keys(tls12_conn_t* t, dir_t* cw, dir_t* sw) {
    uint8_t seed[64], kb[40];
    memcpy(seed, t->srand, 32);
    memcpy(seed + 32, t->crand, 32);
    tls12_prf(t->master, 48, "key expansion", seed, 64, kb, 40);
    aes_gcm_init(&cw->gcm, kb);
    aes_gcm_init(&sw->gcm, kb + 16);
    memcpy(cw->salt, kb + 32, 4);
    memcpy(sw->salt, kb + 36, 4);
    cw->seq = sw->seq = 0;
    memset(kb, 0, sizeof(kb));
}

static void verify_data(const tls12_conn_t* t, int from_server, uint8_t out[12]) {
    uint8_t h[32];
    th_hash(t, h);
    tls12_prf(t->master, 48, from_server ? "server finished" : "client finished", h, 32, out, 12);
}

/* ── record layer (section 6.2) ───────────────────────────────────── */

static int send_record(tls12_conn_t* t, uint8_t type, const uint8_t* data, uint32_t len) {
    uint32_t extra = t->tx.on ? 8 + 16 : 0;
    uint8_t* rec = (uint8_t*)kmalloc(5 + len + extra);
    if (!rec) return NET_ERR_NOMEM;
    rec[0] = type;
    rec[1] = 3;
    rec[2] = 3;
    wr16(rec + 3, (uint16_t)(len + extra));
    if (!t->tx.on) {
        memcpy(rec + 5, data, len);
    } else {
        /* nonce = salt || explicit part (the sequence number, sent along);
         * additional data = seq || type || version || plaintext length */
        uint8_t nonce[12], aad[13];
        memcpy(nonce, t->tx.salt, 4);
        for (int i = 0; i < 8; i++) nonce[4 + i] = aad[i] = (uint8_t)(t->tx.seq >> (56 - 8 * i));
        aad[8] = type; aad[9] = 3; aad[10] = 3;
        wr16(aad + 11, (uint16_t)len);
        memcpy(rec + 5, nonce + 4, 8);
        memcpy(rec + 13, data, len);
        aes_gcm_seal(&t->tx.gcm, nonce, aad, 13, rec + 13, len, rec + 13 + len);
        t->tx.seq++;
    }
    int r = tcp_send(t->tcp, rec, 5 + len + extra, t->timeout_ms ? t->timeout_ms : 30000);
    kfree(rec);
    return r < 0 ? r : NET_OK;
}

static int send_alert(tls12_conn_t* t, uint8_t level, uint8_t desc) {
    uint8_t a[2] = { level, desc };
    return send_record(t, CT_ALERT, a, 2);
}

/* until `need` raw bytes are buffered; timeout 0 only takes what is there */
static int fill(tls12_conn_t* t, uint32_t need, uint32_t timeout_ms) {
    uint32_t start = timer_ms();
    while (t->in_len < need) {
        uint32_t left = 0;
        if (timeout_ms) {
            uint32_t el = timer_ms() - start;
            if (el >= timeout_ms) return NET_ERR_TIMEOUT;
            left = timeout_ms - el;
        }
        int r = tcp_recv(t->tcp, t->in + t->in_len, IN_CAP - t->in_len, left);
        if (r == 0) return NET_ERR_CLOSED;
        if (r < 0) return r;
        t->in_len += (uint32_t)r;
    }
    return NET_OK;
}

/* the next record's plaintext into t->pt: its content type, or < 0 */
static int read_record(tls12_conn_t* t, uint32_t timeout_ms) {
    int r = fill(t, 5, timeout_ms);
    if (r != NET_OK) return r;
    uint8_t type = t->in[0];
    uint32_t n = rd16(t->in + 3);
    if (t->in[1] != 3 || n > MAX_RECORD) { fail(t, "not a TLS record (%02x %02x %02x)", t->in[0], t->in[1], t->in[2]); return NET_ERR_PROTO; }
    r = fill(t, 5 + n, timeout_ms);
    if (r != NET_OK) return r;
    task_maybe_yield();
    uint8_t* p = t->in + 5;
    uint32_t len = n;
    if (t->rx.on) {
        if (n < 8 + 16) { fail(t, "short encrypted record"); return NET_ERR_PROTO; }
        len = n - 24;
        uint8_t nonce[12], aad[13];
        memcpy(nonce, t->rx.salt, 4);
        memcpy(nonce + 4, p, 8);
        for (int i = 0; i < 8; i++) aad[i] = (uint8_t)(t->rx.seq >> (56 - 8 * i));
        aad[8] = type; aad[9] = 3; aad[10] = 3;
        wr16(aad + 11, (uint16_t)len);
        if (aes_gcm_open(&t->rx.gcm, nonce, aad, 13, p + 8, len, p + 8 + len) != 0) {
            fail(t, "record authentication failed (bad_record_mac)");
            return NET_ERR_PROTO;
        }
        t->rx.seq++;
        p += 8;
    }
    if (len > MAX_PLAINTEXT + 1024) { fail(t, "oversized record"); return NET_ERR_PROTO; }
    memcpy(t->pt, p, len);
    t->pt_pos = 0;
    t->pt_len = len;
    memmove(t->in, t->in + 5 + n, t->in_len - 5 - n);
    t->in_len -= 5 + n;
    return type;
}

static const char* alert_name(uint8_t d) {
    switch (d) {
    case 0:   return "close_notify";
    case 10:  return "unexpected_message";
    case 20:  return "bad_record_mac";
    case 40:  return "handshake_failure";
    case 42:  return "bad_certificate";
    case 47:  return "illegal_parameter";
    case 50:  return "decode_error";
    case 51:  return "decrypt_error";
    case 70:  return "protocol_version";
    case 71:  return "insufficient_security";
    case 80:  return "internal_error";
    case 100: return "no_renegotiation";
    default:  return "alert";
    }
}

/* ── handshake messages ───────────────────────────────────────────── */

static int hs_append(tls12_conn_t* t, const uint8_t* d, uint32_t n) {
    if (t->hs_len + n > t->hs_cap) {
        uint32_t cap = t->hs_cap ? t->hs_cap : 8192;
        while (cap < t->hs_len + n) cap *= 2;
        if (cap > 256u * 1024u) { fail(t, "handshake message too large"); return NET_ERR_PROTO; }
        uint8_t* p = (uint8_t*)krealloc(t->hs, cap);
        if (!p) return NET_ERR_NOMEM;
        t->hs = p;
        t->hs_cap = cap;
    }
    memcpy(t->hs + t->hs_len, d, n);
    t->hs_len += n;
    return NET_OK;
}

/* the next whole handshake message (header included); a ChangeCipherSpec
 * in between sets *ccs and switches the receive keys on */
static int next_msg(tls12_conn_t* t, uint8_t** msg, uint32_t* len, int* ccs) {
    for (;;) {
        if (t->hs_len >= 4) {
            uint32_t body = ((uint32_t)t->hs[1] << 16) | ((uint32_t)t->hs[2] << 8) | t->hs[3];
            if (t->hs_len >= 4 + body) { *msg = t->hs; *len = 4 + body; return NET_OK; }
        }
        int type = read_record(t, t->timeout_ms);
        if (type < 0) {
            if (type == NET_ERR_CLOSED) fail(t, "the peer closed the connection during the handshake");
            else if (type == NET_ERR_TIMEOUT) fail(t, "timed out during the handshake");
            return type;
        }
        if (type == CT_ALERT) {
            fail(t, "peer sent alert: %s (%u)", t->pt_len >= 2 ? alert_name(t->pt[1]) : "?", t->pt_len >= 2 ? t->pt[1] : 0);
            return NET_ERR_PROTO;
        }
        if (type == CT_CCS) {
            if (t->pt_len != 1 || t->pt[0] != 1 || t->hs_len) { fail(t, "bad ChangeCipherSpec"); return NET_ERR_PROTO; }
            if (!ccs) { fail(t, "unexpected ChangeCipherSpec"); return NET_ERR_PROTO; }
            *ccs = 1;
            t->rx.on = 1;
            continue;
        }
        if (type != CT_HANDSHAKE) { fail(t, "expected a handshake message, got record type %d", type); return NET_ERR_PROTO; }
        int r = hs_append(t, t->pt, t->pt_len);
        if (r != NET_OK) return r;
    }
}

static void consume(tls12_conn_t* t, uint32_t n) {
    memmove(t->hs, t->hs + n, t->hs_len - n);
    t->hs_len -= n;
}

static void put_hs_header(uint8_t* m, uint8_t type, uint32_t body) {
    m[0] = type;
    m[1] = (uint8_t)(body >> 16);
    m[2] = (uint8_t)(body >> 8);
    m[3] = (uint8_t)body;
}

/* ── DER reading (just enough for a certificate's public key) ─────── */

/* one TLV at p (within end): tag, content and its length; NULL if bad */
static const uint8_t* der_next(const uint8_t* p, const uint8_t* end, uint8_t* tag, const uint8_t** content, uint32_t* clen) {
    if (p + 2 > end) return NULL;
    *tag = p[0];
    uint32_t l = p[1];
    p += 2;
    if (l & 0x80) {
        int nb = (int)(l & 0x7F);
        if (nb < 1 || nb > 3 || p + nb > end) return NULL;
        l = 0;
        while (nb--) l = (l << 8) | *p++;
    }
    if (l > (uint32_t)(end - p)) return NULL;
    *content = p;
    *clen = l;
    return p + l;
}

/* Certificate -> tbsCertificate -> subjectPublicKeyInfo -> BIT STRING */
static int cert_pubkey(const uint8_t* der, uint32_t len, const uint8_t** key, uint32_t* klen) {
    const uint8_t* end = der + len;
    uint8_t tag;
    const uint8_t* c;
    uint32_t cl;
    if (!der_next(der, end, &tag, &c, &cl) || tag != 0x30) return -1;          /* Certificate */
    if (!der_next(c, c + cl, &tag, &c, &cl) || tag != 0x30) return -1;         /* tbsCertificate */
    const uint8_t* p = c;
    const uint8_t* tend = c + cl;
    const uint8_t* f;
    uint32_t fl;
    p = der_next(p, tend, &tag, &f, &fl);
    if (p && tag == 0xA0) p = der_next(p, tend, &tag, &f, &fl);                /* [0] version, serial */
    for (int i = 0; p && i < 4; i++) p = der_next(p, tend, &tag, &f, &fl);     /* sigAlg issuer validity subject */
    if (!p || !der_next(p, tend, &tag, &f, &fl) || tag != 0x30) return -1;    /* subjectPublicKeyInfo */
    const uint8_t* q = der_next(f, f + fl, &tag, &c, &cl);                     /* algorithm */
    if (!q || !der_next(q, f + fl, &tag, &c, &cl) || tag != 0x03 || cl < 2) return -1;
    *key = c + 1;                                                              /* minus the unused-bits byte */
    *klen = cl - 1;
    return 0;
}

/* ── client ───────────────────────────────────────────────────────── */

static uint32_t build_client_hello(tls12_conn_t* t, uint8_t* m, const char* host) {
    uint32_t o = 4;
    m[o++] = 3; m[o++] = 3;
    random_bytes(t->crand, 32);
    memcpy(m + o, t->crand, 32); o += 32;
    m[o++] = 0;                                         /* no session id */
    wr16(m + o, 4); o += 2;
    wr16(m + o, SUITE_ECDHE_RSA_AES128_GCM); o += 2;
    wr16(m + o, SUITE_ECDHE_ECDSA_AES128_GCM); o += 2;
    m[o++] = 1; m[o++] = 0;                             /* compression: null */
    uint32_t ext = o;
    o += 2;
    ip4_t dummy;
    if (host && host[0] && !str_to_ip4(host, &dummy)) {
        uint32_t hl = (uint32_t)strlen(host);
        if (hl > 200) hl = 200;
        wr16(m + o, EXT_SNI); o += 2;
        wr16(m + o, (uint16_t)(hl + 5)); o += 2;
        wr16(m + o, (uint16_t)(hl + 3)); o += 2;
        m[o++] = 0;
        wr16(m + o, (uint16_t)hl); o += 2;
        memcpy(m + o, host, hl); o += hl;
    }
    /* X25519 is the only key exchange; P-256/P-384 are listed after it
     * because an ECDSA server's certificate must be on a listed curve
     * (RFC 8422 5.1) - servers take the client's first choice */
    wr16(m + o, EXT_GROUPS); o += 2;
    wr16(m + o, 8); o += 2; wr16(m + o, 6); o += 2;
    wr16(m + o, GROUP_X25519); o += 2; wr16(m + o, 0x0017); o += 2; wr16(m + o, 0x0018); o += 2;
    wr16(m + o, EXT_POINT_FORMATS); o += 2;
    wr16(m + o, 2); o += 2; m[o++] = 1; m[o++] = 0;
    static const uint16_t sigs[] = { 0x0401, 0x0403, 0x0804, 0x0501, 0x0503, 0x0805, 0x0601, 0x0806, 0x0201, 0x0203 };
    uint32_t ns = sizeof(sigs) / sizeof(sigs[0]);
    wr16(m + o, EXT_SIGALGS); o += 2;
    wr16(m + o, (uint16_t)(2 + 2 * ns)); o += 2;
    wr16(m + o, (uint16_t)(2 * ns)); o += 2;
    for (uint32_t i = 0; i < ns; i++) { wr16(m + o, sigs[i]); o += 2; }
    wr16(m + o, EXT_EMS); o += 2; wr16(m + o, 0); o += 2;
    wr16(m + o, EXT_RENEGOTIATION); o += 2; wr16(m + o, 1); o += 2; m[o++] = 0;
    wr16(m + ext, (uint16_t)(o - ext - 2));
    put_hs_header(m, HS_CLIENT_HELLO, o - 4);
    return o;
}

static int parse_server_hello(tls12_conn_t* t, const uint8_t* m, uint32_t len) {
    const uint8_t* p = m + 4;
    const uint8_t* end = m + len;
    if (len < 4 + 2 + 32 + 1 + 3) goto bad;
    if (p[0] != 3 || p[1] != 3) {
        fail(t, "the server wants TLS %u.%u, not 1.2", p[0] == 3 ? 1 : p[0], p[0] == 3 ? (unsigned)p[1] - 1 : p[1]);
        return NET_ERR_PROTO;
    }
    memcpy(t->srand, p + 2, 32);
    p += 34;
    uint32_t sid = *p++;
    if (p + sid + 3 > end) goto bad;
    p += sid;
    uint16_t suite = rd16(p);
    p += 2;
    if (suite != SUITE_ECDHE_RSA_AES128_GCM && suite != SUITE_ECDHE_ECDSA_AES128_GCM) {
        fail(t, "the server chose cipher suite 0x%04x", suite);
        return NET_ERR_PROTO;
    }
    if (*p++ != 0) goto bad;
    if (p + 2 <= end) {
        uint32_t el = rd16(p);
        p += 2;
        if (p + el > end) goto bad;
        const uint8_t* e = p;
        while (e + 4 <= p + el) {
            uint16_t type = rd16(e), l = rd16(e + 2);
            if (e + 4 + l > p + el) goto bad;
            if (type == EXT_EMS) t->ems = 1;
            e += 4 + l;
        }
    }
    return NET_OK;
bad:
    fail(t, "malformed ServerHello");
    return NET_ERR_PROTO;
}

static int client_handshake(tls12_conn_t* t, const char* host) {
    uint8_t* m = (uint8_t*)kmalloc(1024);
    if (!m) return NET_ERR_NOMEM;
    uint32_t n = build_client_hello(t, m, host);
    sha256_update(&t->th, m, n);
    int r = send_record(t, CT_HANDSHAKE, m, n);
    kfree(m);
    if (r != NET_OK) { fail(t, "could not send ClientHello (%s)", net_strerror(r)); return r; }

    uint8_t* msg;
    uint32_t len;
    uint8_t server_pub[65];
    int have_kx = 0, cert_req = 0, srv_p256 = 0;
    r = next_msg(t, &msg, &len, NULL);
    if (r != NET_OK) return r;
    if (msg[0] != HS_SERVER_HELLO) { fail(t, "expected ServerHello, got message %u", msg[0]); return NET_ERR_PROTO; }
    if ((r = parse_server_hello(t, msg, len)) != NET_OK) return r;
    sha256_update(&t->th, msg, len);
    consume(t, len);

    for (;;) {
        if ((r = next_msg(t, &msg, &len, NULL)) != NET_OK) return r;
        uint8_t type = msg[0];
        if (type == HS_CERTIFICATE && len >= 4 + 3 + 3) {
            uint32_t cl = ((uint32_t)msg[7] << 16) | ((uint32_t)msg[8] << 8) | msg[9];
            if (10 + cl <= len && !t->cert) {
                t->cert = (uint8_t*)kmalloc(cl);
                if (!t->cert) return NET_ERR_NOMEM;
                memcpy(t->cert, msg + 10, cl);
                t->cert_len = cl;
                if (cert_pubkey(t->cert, cl, &t->pubkey, &t->pubkey_len) != 0) { t->pubkey = NULL; t->pubkey_len = 0; }
            }
        } else if (type == HS_SERVER_KX) {
            /* ECParameters (named_curve, X25519 or P-256) || point || signature -
             * the signature is not checked: the certificate is not either */
            if (len < 4 + 4 || msg[4] != 3) { fail(t, "malformed ServerKeyExchange"); return NET_ERR_PROTO; }
            uint16_t grp = rd16(msg + 5);
            if (grp == GROUP_X25519 && msg[7] == 32 && len >= 4 + 4 + 32) {
                memcpy(server_pub, msg + 8, 32);
            } else if (grp == GROUP_SECP256R1 && msg[7] == 65 && len >= 4 + 4 + 65) {
                memcpy(server_pub, msg + 8, 65);
                srv_p256 = 1;
            } else {
                fail(t, "the server's key exchange curve is not supported");
                return NET_ERR_PROTO;
            }
            have_kx = 1;
        } else if (type == HS_CERT_REQUEST) {
            cert_req = 1;
        } else if (type == HS_SERVER_DONE) {
            sha256_update(&t->th, msg, len);
            consume(t, len);
            break;
        } else {
            fail(t, "unexpected handshake message %u", type);
            return NET_ERR_PROTO;
        }
        sha256_update(&t->th, msg, len);
        consume(t, len);
    }
    if (!have_kx) { fail(t, "the server sent no ECDHE key exchange"); return NET_ERR_PROTO; }

    /* our flight: [empty Certificate], ClientKeyExchange, CCS, Finished */
    uint8_t priv[32], pub[65], pms[32];
    uint32_t publen;
    int kx_ok = 1;
    random_bytes(priv, 32);
    if (srv_p256) {
        p256_public(pub, priv);
        publen = 65;
        if (p256_ecdh(pms, priv, server_pub) != 0) kx_ok = 0;
    } else {
        x25519_base(pub, priv);
        publen = 32;
        x25519(pms, priv, server_pub);
    }
    memset(priv, 0, 32);
    if (!kx_ok) { fail(t, "the server sent an invalid P-256 key share"); return NET_ERR_PROTO; }
    task_maybe_yield();
    uint8_t flight[8 + 4 + 66];
    uint32_t o = 0;
    if (cert_req) {
        static const uint8_t empty_cert[7] = { HS_CERTIFICATE, 0, 0, 3, 0, 0, 0 };
        memcpy(flight, empty_cert, 7);
        o = 7;
    }
    put_hs_header(flight + o, HS_CLIENT_KX, 1 + publen);
    flight[o + 4] = (uint8_t)publen;
    memcpy(flight + o + 5, pub, publen);
    o += 4 + 1 + publen;
    sha256_update(&t->th, flight, o);
    if ((r = send_record(t, CT_HANDSHAKE, flight, o)) != NET_OK) return r;
    make_master(t, pms);
    memset(pms, 0, 32);
    make_keys(t, &t->tx, &t->rx);

    static const uint8_t ccs = 1;
    if ((r = send_record(t, CT_CCS, &ccs, 1)) != NET_OK) return r;
    t->tx.on = 1;
    uint8_t fin[16];
    put_hs_header(fin, HS_FINISHED, 12);
    verify_data(t, 0, fin + 4);
    sha256_update(&t->th, fin, 16);
    if ((r = send_record(t, CT_HANDSHAKE, fin, 16)) != NET_OK) return r;

    int got_ccs = 0;
    if ((r = next_msg(t, &msg, &len, &got_ccs)) != NET_OK) return r;
    uint8_t want[12];
    verify_data(t, 1, want);
    if (!got_ccs || msg[0] != HS_FINISHED || len != 16 || ct_memcmp(want, msg + 4, 12) != 0) {
        fail(t, "the server's Finished message is wrong");
        return NET_ERR_PROTO;
    }
    consume(t, len);
    return NET_OK;
}

/* ── server ───────────────────────────────────────────────────────── */

static int server_handshake(tls12_conn_t* t, const tls12_identity_t* id) {
    uint8_t* msg;
    uint32_t len;
    int r = next_msg(t, &msg, &len, NULL);
    if (r != NET_OK) return r;
    if (msg[0] != HS_CLIENT_HELLO || len < 4 + 2 + 32 + 1) { fail(t, "expected ClientHello"); return NET_ERR_PROTO; }

    /* ClientHello: version, random, session id, suites, compression, extensions */
    const uint8_t* p = msg + 4;
    const uint8_t* end = msg + len;
    if (p[0] != 3 || p[1] < 3) { send_alert(t, 2, 70); fail(t, "the client does not speak TLS 1.2"); return NET_ERR_PROTO; }
    memcpy(t->crand, p + 2, 32);
    p += 34;
    uint32_t sl = *p++;
    if (p + sl + 2 > end) goto bad;
    p += sl;
    uint32_t cl = rd16(p);
    p += 2;
    if (p + cl + 1 > end) goto bad;
    int suite_ok = 0, reneg = 0, x25519_ok = 0, p256_ok = 0, groups_sent = 0, rsa_sha256 = 0, sigalgs_sent = 0;
    for (uint32_t i = 0; i + 1 < cl; i += 2) {
        uint16_t s = rd16(p + i);
        if (s == SUITE_ECDHE_RSA_AES128_GCM) suite_ok = 1;
        if (s == SUITE_EMPTY_RENEGOTIATION) reneg = 1;
    }
    p += cl;
    uint32_t comp = *p++;
    if (p + comp > end) goto bad;
    p += comp;
    if (p + 2 <= end) {
        uint32_t el = rd16(p);
        p += 2;
        if (p + el > end) goto bad;
        const uint8_t* e = p;
        while (e + 4 <= p + el) {
            uint16_t type = rd16(e), l = rd16(e + 2);
            const uint8_t* v = e + 4;
            if (v + l > p + el) goto bad;
            if (type == EXT_EMS) t->ems = 1;
            if (type == EXT_RENEGOTIATION) reneg = 1;
            if (type == EXT_GROUPS && l >= 2) {
                groups_sent = 1;
                for (uint32_t i = 2; i + 1 < l; i += 2) {
                    uint16_t g = rd16(v + i);
                    if (g == GROUP_X25519) x25519_ok = 1;
                    else if (g == GROUP_SECP256R1) p256_ok = 1;
                }
            }
            if (type == EXT_SIGALGS && l >= 2) {
                sigalgs_sent = 1;
                for (uint32_t i = 2; i + 1 < l; i += 2) if (rd16(v + i) == SIG_RSA_PKCS1_SHA256) rsa_sha256 = 1;
            }
            e = v + l;
        }
    }
    if (!suite_ok || (groups_sent && !x25519_ok && !p256_ok) || (sigalgs_sent && !rsa_sha256)) {
        send_alert(t, 2, 40);
        fail(t, "no common cipher suite (needs ECDHE-RSA-AES128-GCM-SHA256 with X25519 or P-256)");
        return NET_ERR_PROTO;
    }
    /* prefer X25519 when the client offers it; otherwise P-256 (Windows
     * Schannel offers only the NIST curves) */
    int use_p256 = (groups_sent && !x25519_ok);
    sha256_update(&t->th, msg, len);
    consume(t, len);

    /* ServerHello, Certificate, ServerKeyExchange, ServerHelloDone - one record */
    uint32_t cap = 128 + 10 + id->cert_len + 4 + 4 + 66 + 4 + RSA_BYTES + 4;
    uint8_t* f = (uint8_t*)kmalloc(cap);
    if (!f) return NET_ERR_NOMEM;
    uint32_t o = 4;
    f[o++] = 3; f[o++] = 3;
    random_bytes(t->srand, 32);
    memcpy(f + o, t->srand, 32); o += 32;
    f[o++] = 0;                                          /* no session id: no resumption */
    wr16(f + o, SUITE_ECDHE_RSA_AES128_GCM); o += 2;
    f[o++] = 0;
    uint32_t ext = o;
    o += 2;
    if (reneg) { wr16(f + o, EXT_RENEGOTIATION); o += 2; wr16(f + o, 1); o += 2; f[o++] = 0; }
    if (t->ems) { wr16(f + o, EXT_EMS); o += 2; wr16(f + o, 0); o += 2; }
    wr16(f + o, EXT_POINT_FORMATS); o += 2; wr16(f + o, 2); o += 2; f[o++] = 1; f[o++] = 0;
    wr16(f + ext, (uint16_t)(o - ext - 2));
    put_hs_header(f, HS_SERVER_HELLO, o - 4);

    uint32_t c0 = o;
    o += 4;
    f[o++] = (uint8_t)((id->cert_len + 3) >> 16); f[o++] = (uint8_t)((id->cert_len + 3) >> 8); f[o++] = (uint8_t)(id->cert_len + 3);
    f[o++] = (uint8_t)(id->cert_len >> 16); f[o++] = (uint8_t)(id->cert_len >> 8); f[o++] = (uint8_t)id->cert_len;
    memcpy(f + o, id->cert, id->cert_len); o += id->cert_len;
    put_hs_header(f + c0, HS_CERTIFICATE, o - c0 - 4);

    uint8_t priv[32], pub[65];
    uint32_t publen;
    random_bytes(priv, 32);
    if (use_p256) { p256_public(pub, priv); publen = 65; }
    else { x25519_base(pub, priv); publen = 32; }
    uint32_t k0 = o;
    o += 4;
    uint32_t params = o;
    f[o++] = 3; wr16(f + o, use_p256 ? GROUP_SECP256R1 : GROUP_X25519); o += 2; f[o++] = (uint8_t)publen;
    memcpy(f + o, pub, publen); o += publen;
    {
        /* signed: client_random || server_random || params */
        sha256_ctx_t c;
        uint8_t h[32];
        sha256_init(&c);
        sha256_update(&c, t->crand, 32);
        sha256_update(&c, t->srand, 32);
        sha256_update(&c, f + params, o - params);
        sha256_final(&c, h);
        wr16(f + o, SIG_RSA_PKCS1_SHA256); o += 2;
        wr16(f + o, RSA_BYTES); o += 2;
        rsa_sign_sha256(id->key, h, f + o);
        o += RSA_BYTES;
    }
    put_hs_header(f + k0, HS_SERVER_KX, o - k0 - 4);
    put_hs_header(f + o, HS_SERVER_DONE, 0);
    o += 4;
    sha256_update(&t->th, f, o);
    r = send_record(t, CT_HANDSHAKE, f, o);
    kfree(f);
    if (r != NET_OK) { memset(priv, 0, 32); return r; }

    /* ClientKeyExchange, CCS, Finished */
    r = next_msg(t, &msg, &len, NULL);
    if (r != NET_OK) { memset(priv, 0, 32); return r; }
    if (msg[0] == HS_CERTIFICATE) {                     /* (we never ask for one) */
        sha256_update(&t->th, msg, len);
        consume(t, len);
        if ((r = next_msg(t, &msg, &len, NULL)) != NET_OK) { memset(priv, 0, 32); return r; }
    }
    uint32_t kxlen = use_p256 ? 65 : 32;
    if (msg[0] != HS_CLIENT_KX || len != 4 + 1 + kxlen || msg[4] != kxlen) {
        memset(priv, 0, 32);
        send_alert(t, 2, 10);
        fail(t, "expected the client's %s key exchange", use_p256 ? "P-256" : "X25519");
        return NET_ERR_PROTO;
    }
    uint8_t pms[32];
    if (use_p256) {
        if (p256_ecdh(pms, priv, msg + 5) != 0) {
            memset(priv, 0, 32);
            send_alert(t, 2, 47);
            fail(t, "the client's P-256 key share is not on the curve");
            return NET_ERR_PROTO;
        }
    } else {
        x25519(pms, priv, msg + 5);
    }
    memset(priv, 0, 32);
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++) acc |= pms[i];
    if (!acc) { fail(t, "bad client key share"); return NET_ERR_PROTO; }
    sha256_update(&t->th, msg, len);
    consume(t, len);
    make_master(t, pms);
    memset(pms, 0, 32);
    make_keys(t, &t->rx, &t->tx);

    int got_ccs = 0;
    if ((r = next_msg(t, &msg, &len, &got_ccs)) != NET_OK) return r;
    uint8_t want[12];
    verify_data(t, 0, want);
    if (!got_ccs || msg[0] != HS_FINISHED || len != 16 || ct_memcmp(want, msg + 4, 12) != 0) {
        send_alert(t, 2, 51);
        fail(t, "the client's Finished message is wrong");
        return NET_ERR_PROTO;
    }
    sha256_update(&t->th, msg, len);
    consume(t, len);

    static const uint8_t ccs = 1;
    if ((r = send_record(t, CT_CCS, &ccs, 1)) != NET_OK) return r;
    t->tx.on = 1;
    uint8_t fin[16];
    put_hs_header(fin, HS_FINISHED, 12);
    verify_data(t, 1, fin + 4);
    return send_record(t, CT_HANDSHAKE, fin, 16);
bad:
    send_alert(t, 2, 50);
    fail(t, "malformed ClientHello");
    return NET_ERR_PROTO;
}

/* ── connections ──────────────────────────────────────────────────── */

static tls12_conn_t* new_conn(tcp_conn_t* tcp, int server, uint32_t timeout_ms, char* errmsg, uint32_t errmsg_len) {
    if (errmsg && errmsg_len) errmsg[0] = '\0';
    tls12_conn_t* t = (tls12_conn_t*)kzalloc(sizeof(tls12_conn_t));
    if (!t) return NULL;
    t->in = (uint8_t*)kmalloc(IN_CAP);
    t->pt = (uint8_t*)kmalloc(MAX_RECORD);
    if (!t->in || !t->pt) { kfree(t->in); kfree(t->pt); kfree(t); return NULL; }
    t->tcp = tcp;
    t->server = server;
    t->timeout_ms = timeout_ms;
    t->errmsg = errmsg;
    t->errmsg_len = errmsg_len;
    sha256_init(&t->th);
    return t;
}

static void free_conn(tls12_conn_t* t) {
    memset(&t->rx, 0, sizeof(t->rx));
    memset(&t->tx, 0, sizeof(t->tx));
    memset(t->master, 0, sizeof(t->master));
    kfree(t->in);
    kfree(t->pt);
    kfree(t->hs);
    kfree(t->cert);
    kfree(t);
}

static tls12_conn_t* finish(tls12_conn_t* t, int r, int* err, const char* who) {
    if (r != NET_OK) {
        *err = r;
        klog("tls12: %s handshake failed: %s\n", who, t->errmsg ? t->errmsg : "?");
        free_conn(t);
        return NULL;
    }
    kfree(t->hs);
    t->hs = NULL;
    t->hs_len = t->hs_cap = 0;
    t->errmsg = NULL;
    t->pt_pos = t->pt_len = 0;
    *err = NET_OK;
    return t;
}

tls12_conn_t* tls12_connect(tcp_conn_t* tcp, const char* server_name, uint32_t timeout_ms,
                            int* err, char* errmsg, uint32_t errmsg_len) {
    tls12_conn_t* t = new_conn(tcp, 0, timeout_ms, errmsg, errmsg_len);
    if (!t) { *err = NET_ERR_NOMEM; return NULL; }
    return finish(t, client_handshake(t, server_name), err, "client");
}

tls12_conn_t* tls12_accept(tcp_conn_t* tcp, const tls12_identity_t* id, uint32_t timeout_ms,
                           int* err, char* errmsg, uint32_t errmsg_len) {
    tls12_conn_t* t = new_conn(tcp, 1, timeout_ms, errmsg, errmsg_len);
    if (!t) { *err = NET_ERR_NOMEM; return NULL; }
    return finish(t, server_handshake(t, id), err, "server");
}

int tls12_pending(tls12_conn_t* t) {
    if (t->pt_pos < t->pt_len || t->eof) return 1;
    if (t->in_len >= 5 && t->in_len >= 5u + rd16(t->in + 3)) return 1;
    return tcp_readable(t->tcp) != 0;
}

int tls12_read(tls12_conn_t* t, void* buf, uint32_t max, uint32_t timeout_ms) {
    uint32_t start = timer_ms();
    while (t->pt_pos >= t->pt_len) {
        if (t->eof) return 0;
        uint32_t left = 0;
        if (timeout_ms) {
            uint32_t el = timer_ms() - start;
            left = el >= timeout_ms ? 1 : timeout_ms - el;
        }
        int type = read_record(t, left);
        if (type == NET_ERR_CLOSED) { t->eof = 1; return 0; }
        if (type < 0) return type;
        if (type == CT_APPDATA) continue;
        uint32_t plen = t->pt_len;
        t->pt_len = 0;                                  /* not application data */
        if (type == CT_ALERT) {
            uint8_t d = plen >= 2 ? t->pt[1] : 0;
            if (d == 0) { t->eof = 1; return 0; }        /* close_notify */
            klog("tls12: alert %u (%s)\n", d, alert_name(d));
            return NET_ERR_PROTO;
        }
        if (type == CT_HANDSHAKE) {                     /* HelloRequest / renegotiation: refused */
            send_alert(t, 1, 100);
            continue;
        }
        return NET_ERR_PROTO;
    }
    uint32_t n = t->pt_len - t->pt_pos;
    if (n > max) n = max;
    memcpy(buf, t->pt + t->pt_pos, n);
    t->pt_pos += n;
    return (int)n;
}

int tls12_write(tls12_conn_t* t, const void* data, uint32_t len, uint32_t timeout_ms) {
    t->timeout_ms = timeout_ms;
    const uint8_t* p = (const uint8_t*)data;
    uint32_t done = 0;
    while (done < len) {
        uint32_t n = len - done;
        if (n > MAX_PLAINTEXT) n = MAX_PLAINTEXT;
        int r = send_record(t, CT_APPDATA, p + done, n);
        if (r != NET_OK) return r;
        done += n;
    }
    return (int)len;
}

void tls12_close(tls12_conn_t* t) {
    if (!t) return;
    tcp_state_t s = tcp_state(t->tcp);
    if (s == TCP_ESTABLISHED || s == TCP_CLOSE_WAIT) send_alert(t, 1, 0);
    free_conn(t);
}

int tls12_peer_cert(tls12_conn_t* t, const uint8_t** der, uint32_t* len) {
    if (!t->cert) return -1;
    *der = t->cert;
    *len = t->cert_len;
    return 0;
}

int tls12_peer_pubkey(tls12_conn_t* t, const uint8_t** key, uint32_t* len) {
    if (!t->pubkey) return -1;
    *key = t->pubkey;
    *len = t->pubkey_len;
    return 0;
}

void tls12_fingerprint(const uint8_t* der, uint32_t len, char out[96]) {
    static const char hx[] = "0123456789ABCDEF";
    uint8_t h[32];
    sha256(der, len, h);
    int o = 0;
    for (int i = 0; i < 32; i++) {
        if (i) out[o++] = ':';
        out[o++] = hx[h[i] >> 4];
        out[o++] = hx[h[i] & 15];
    }
    out[o] = '\0';
}

/* ── a self-signed certificate ────────────────────────────────────── */

typedef struct { uint8_t* p; uint32_t len, cap; int err; } dw_t;

static void dw_bytes(dw_t* w, const void* d, uint32_t n) {
    if (w->len + n > w->cap) { w->err = 1; return; }
    memcpy(w->p + w->len, d, n);
    w->len += n;
}

static void dw_hdr(dw_t* w, uint8_t tag, uint32_t n) {
    uint8_t h[5];
    uint32_t o = 0;
    h[o++] = tag;
    if (n < 0x80) h[o++] = (uint8_t)n;
    else if (n < 0x100) { h[o++] = 0x81; h[o++] = (uint8_t)n; }
    else { h[o++] = 0x82; h[o++] = (uint8_t)(n >> 8); h[o++] = (uint8_t)n; }
    dw_bytes(w, h, o);
}

static void dw_tlv(dw_t* w, uint8_t tag, const void* d, uint32_t n) { dw_hdr(w, tag, n); dw_bytes(w, d, n); }

/* a positive INTEGER from big-endian bytes */
static void dw_uint(dw_t* w, const uint8_t* be, uint32_t n) {
    while (n > 1 && *be == 0) { be++; n--; }
    if (*be & 0x80) { dw_hdr(w, 0x02, n + 1); uint8_t z = 0; dw_bytes(w, &z, 1); }
    else dw_hdr(w, 0x02, n);
    dw_bytes(w, be, n);
}

/* wraps everything written since `mark` in a tag */
static void dw_wrap(dw_t* w, uint32_t mark, uint8_t tag) {
    if (w->err) return;
    uint32_t n = w->len - mark;
    uint32_t hl = n < 0x80 ? 2 : n < 0x100 ? 3 : 4;
    if (w->len + hl > w->cap) { w->err = 1; return; }
    memmove(w->p + mark + hl, w->p + mark, n);
    uint32_t save = w->len;
    w->len = mark;
    dw_hdr(w, tag, n);
    w->len = save + hl;
}

static void dw_name(dw_t* w, const char* cn) {
    static const uint8_t OID_CN[] = { 0x55, 0x04, 0x03 };
    uint32_t m = w->len;
    dw_tlv(w, 0x06, OID_CN, 3);
    dw_tlv(w, 0x0C, cn, (uint32_t)strlen(cn));         /* UTF8String */
    dw_wrap(w, m, 0x30);
    dw_wrap(w, m, 0x31);
    dw_wrap(w, m, 0x30);
}

int tls12_make_cert(const rsa_key_t* key, const char* common_name, uint8_t* out, uint32_t cap) {
    static const uint8_t OID_SHA256_RSA[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0b };
    static const uint8_t OID_RSA[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01 };
    static const uint8_t OID_KEY_USAGE[] = { 0x55, 0x1d, 0x0f };
    static const uint8_t OID_EXT_KEY_USAGE[] = { 0x55, 0x1d, 0x25 };
    static const uint8_t OID_SERVER_AUTH[] = { 0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01 };
    dw_t w = { out, 0, cap, 0 };
    uint8_t nb[RSA_BYTES], eb[4];
    for (int i = 0; i < RSA_WORDS; i++) {
        uint32_t v = key->n[RSA_WORDS - 1 - i];
        nb[4 * i] = (uint8_t)(v >> 24); nb[4 * i + 1] = (uint8_t)(v >> 16);
        nb[4 * i + 2] = (uint8_t)(v >> 8); nb[4 * i + 3] = (uint8_t)v;
    }
    wr32(eb, key->e);

    /* tbsCertificate */
    uint32_t tbs = w.len;
    { uint32_t m = w.len; uint8_t v = 2; dw_tlv(&w, 0x02, &v, 1); dw_wrap(&w, m, 0xA0); }   /* v3 */
    uint8_t serial[9];
    random_bytes(serial, 9);
    serial[0] &= 0x7F;
    serial[0] |= 0x01;
    dw_uint(&w, serial, 9);
    { uint32_t m = w.len; dw_tlv(&w, 0x06, OID_SHA256_RSA, 9); dw_tlv(&w, 0x05, "", 0); dw_wrap(&w, m, 0x30); }
    dw_name(&w, common_name);
    { uint32_t m = w.len; dw_tlv(&w, 0x17, "250101000000Z", 13); dw_tlv(&w, 0x17, "491231235959Z", 13); dw_wrap(&w, m, 0x30); }
    dw_name(&w, common_name);
    {
        uint32_t m = w.len;
        { uint32_t a = w.len; dw_tlv(&w, 0x06, OID_RSA, 9); dw_tlv(&w, 0x05, "", 0); dw_wrap(&w, a, 0x30); }
        uint32_t b = w.len;
        uint8_t z = 0;
        dw_bytes(&w, &z, 1);                           /* no unused bits */
        uint32_t k = w.len;
        dw_uint(&w, nb, RSA_BYTES);
        dw_uint(&w, eb, 4);
        dw_wrap(&w, k, 0x30);                          /* RSAPublicKey */
        dw_wrap(&w, b, 0x03);
        dw_wrap(&w, m, 0x30);
    }
    {
        /* extensions: keyUsage (digitalSignature, keyEncipherment),
         * extKeyUsage serverAuth */
        uint32_t m = w.len;
        { uint32_t e = w.len; dw_tlv(&w, 0x06, OID_KEY_USAGE, 3);
          static const uint8_t ku[] = { 0x03, 0x02, 0x05, 0xA0 };
          dw_tlv(&w, 0x04, ku, 4); dw_wrap(&w, e, 0x30); }
        { uint32_t e = w.len; dw_tlv(&w, 0x06, OID_EXT_KEY_USAGE, 3);
          uint32_t o = w.len; dw_tlv(&w, 0x06, OID_SERVER_AUTH, 8); dw_wrap(&w, o, 0x30); dw_wrap(&w, o, 0x04);
          dw_wrap(&w, e, 0x30); }
        dw_wrap(&w, m, 0x30);
        dw_wrap(&w, m, 0xA3);
    }
    dw_wrap(&w, tbs, 0x30);
    if (w.err) return -1;

    uint8_t h[32], sig[RSA_BYTES];
    sha256(out + tbs, w.len - tbs, h);
    rsa_sign_sha256(key, h, sig);
    { uint32_t m = w.len; dw_tlv(&w, 0x06, OID_SHA256_RSA, 9); dw_tlv(&w, 0x05, "", 0); dw_wrap(&w, m, 0x30); }
    { uint32_t m = w.len; uint8_t z = 0; dw_bytes(&w, &z, 1); dw_bytes(&w, sig, RSA_BYTES); dw_wrap(&w, m, 0x03); }
    dw_wrap(&w, 0, 0x30);
    return w.err ? -1 : (int)w.len;
}
