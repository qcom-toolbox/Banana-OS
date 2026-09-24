#include "tls.h"
#include "sha256.h"
#include "x25519.h"
#include "aead.h"
#include "kstring.h"
#include "kheap.h"
#include "random.h"
#include "serial.h"

/* TLS 1.3 client - see tls.h for scope (notably: no certificate
 * validation). Section numbers refer to RFC 8446. */

#define TLS_AES_128_GCM_SHA256       0x1301
#define TLS_CHACHA20_POLY1305_SHA256 0x1303

#define CT_CCS       20
#define CT_ALERT     21
#define CT_HANDSHAKE 22
#define CT_APPDATA   23

#define HS_CLIENT_HELLO   1
#define HS_SERVER_HELLO   2
#define HS_NEW_TICKET     4
#define HS_ENC_EXTENSIONS 8
#define HS_CERTIFICATE    11
#define HS_CERT_REQUEST   13
#define HS_CERT_VERIFY    15
#define HS_FINISHED       20
#define HS_KEY_UPDATE     24

#define MAX_PLAINTEXT  16384u
#define MAX_CIPHERTEXT (MAX_PLAINTEXT + 256u)

typedef struct {
    int      active;
    uint8_t  secret[32];
    uint8_t  key[32];
    uint8_t  iv[12];
    uint64_t seq;
    aes_gcm_t gcm;
} tls_dir_t;

struct tls_conn {
    tcp_conn_t* tcp;
    uint32_t    timeout_ms;
    uint16_t    suite;
    int         ciphers;                   /* TLS_CIPHERS_* offered */
    tls_dir_t   rx, tx;
    sha256_ctx_t transcript;

    uint8_t     rec[5 + MAX_CIPHERTEXT];   /* one record as read from TCP */
    uint8_t*    app;                       /* decrypted app data inside rec */
    uint32_t    app_pos, app_len;
    int         eof;

    uint8_t*    hs;                        /* handshake message reassembly */
    uint32_t    hs_len, hs_cap;
    char*       errmsg;
    uint32_t    errmsg_len;
};

static void fail(tls_conn_t* t, const char* fmt, ...) {
    if (!t->errmsg || !t->errmsg_len) return;
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvsnprintf(t->errmsg, t->errmsg_len, fmt, ap);
    __builtin_va_end(ap);
}

const char* tls_cipher_name(const tls_conn_t* t) {
    return t->suite == TLS_AES_128_GCM_SHA256 ? "TLS_AES_128_GCM_SHA256"
                                              : "TLS_CHACHA20_POLY1305_SHA256";
}

/* ── key schedule (section 7) ───────────────────────────────────── */

static void hkdf_label(const uint8_t secret[32], const char* label, const uint8_t* ctx, uint32_t ctx_len,
                       uint8_t* out, uint32_t out_len) {
    uint8_t info[2 + 1 + 6 + 32 + 1 + 32];
    uint32_t l = (uint32_t)strlen(label);
    uint32_t o = 0;
    info[o++] = (uint8_t)(out_len >> 8);
    info[o++] = (uint8_t)out_len;
    info[o++] = (uint8_t)(6 + l);
    memcpy(info + o, "tls13 ", 6); o += 6;
    memcpy(info + o, label, l); o += l;
    info[o++] = (uint8_t)ctx_len;
    if (ctx_len) memcpy(info + o, ctx, ctx_len);
    o += ctx_len;
    hkdf_expand(secret, info, o, out, out_len);
}

static void transcript_hash(const tls_conn_t* t, uint8_t out[32]) {
    sha256_ctx_t copy = t->transcript;
    sha256_final(&copy, out);
}

static void set_keys(tls_conn_t* t, tls_dir_t* d, const uint8_t secret[32]) {
    uint32_t klen = (t->suite == TLS_AES_128_GCM_SHA256) ? 16 : 32;
    memcpy(d->secret, secret, 32);
    hkdf_label(secret, "key", NULL, 0, d->key, klen);
    hkdf_label(secret, "iv", NULL, 0, d->iv, 12);
    if (t->suite == TLS_AES_128_GCM_SHA256) aes_gcm_init(&d->gcm, d->key);
    d->seq = 0;
    d->active = 1;
}

static void make_nonce(const tls_dir_t* d, uint8_t nonce[12]) {
    memcpy(nonce, d->iv, 12);
    for (int i = 0; i < 8; i++) nonce[4 + i] ^= (uint8_t)(d->seq >> (56 - 8 * i));
}

/* ── record layer (section 5) ───────────────────────────────────── */

static int tcp_read_exact(tls_conn_t* t, uint8_t* buf, uint32_t n) {
    uint32_t got = 0;
    while (got < n) {
        int r = tcp_recv(t->tcp, buf + got, n - got, t->timeout_ms);
        if (r == 0) return NET_ERR_CLOSED;
        if (r < 0) return r;
        got += (uint32_t)r;
    }
    return NET_OK;
}

static int send_record(tls_conn_t* t, uint8_t type, const uint8_t* data, uint32_t len) {
    uint8_t* rec = (uint8_t*)kmalloc(5 + len + 1 + 16);
    if (!rec) return NET_ERR_NOMEM;
    uint32_t total;
    if (!t->tx.active) {
        rec[0] = type;
        rec[1] = 3;
        rec[2] = (type == CT_HANDSHAKE) ? 1 : 3;   /* ClientHello: legacy 0x0301 */
        rec[3] = (uint8_t)(len >> 8);
        rec[4] = (uint8_t)len;
        memcpy(rec + 5, data, len);
        total = 5 + len;
    } else {
        /* TLSInnerPlaintext = content || type, sealed inside an
         * application_data record (section 5.2) */
        uint32_t inner = len + 1;
        rec[0] = CT_APPDATA;
        rec[1] = 3;
        rec[2] = 3;
        rec[3] = (uint8_t)((inner + 16) >> 8);
        rec[4] = (uint8_t)(inner + 16);
        memcpy(rec + 5, data, len);
        rec[5 + len] = type;
        uint8_t nonce[12];
        make_nonce(&t->tx, nonce);
        if (t->suite == TLS_AES_128_GCM_SHA256)
            aes_gcm_seal(&t->tx.gcm, nonce, rec, 5, rec + 5, inner, rec + 5 + inner);
        else
            chacha20poly1305_seal(t->tx.key, nonce, rec, 5, rec + 5, inner, rec + 5 + inner);
        t->tx.seq++;
        total = 5 + inner + 16;
    }
    int r = tcp_send(t->tcp, rec, total, t->timeout_ms);
    kfree(rec);
    return r < 0 ? r : NET_OK;
}

/* Reads one record, decrypting it when keys are active. Returns the
 * (inner) content type, or a negative error. Plaintext is left in
 * t->rec + 5, length in *len. ChangeCipherSpec records are skipped. */
static int read_record(tls_conn_t* t, uint32_t* len) {
    for (;;) {
        int r = tcp_read_exact(t, t->rec, 5);
        if (r != NET_OK) return r;
        uint8_t type = t->rec[0];
        uint32_t n = ((uint32_t)t->rec[3] << 8) | t->rec[4];
        if (n > MAX_CIPHERTEXT) { fail(t, "oversized record (%u bytes)", n); return NET_ERR_PROTO; }
        r = tcp_read_exact(t, t->rec + 5, n);
        if (r != NET_OK) return r;

        if (type == CT_CCS) continue;                /* middlebox compatibility, section D.4 */
        if (!t->rx.active || type != CT_APPDATA) {
            if (t->rx.active && type != CT_ALERT) {
                fail(t, "unexpected plaintext record type %u", type);
                return NET_ERR_PROTO;
            }
            *len = n;
            return type;
        }
        if (n < 17) { fail(t, "short encrypted record"); return NET_ERR_PROTO; }
        uint8_t nonce[12];
        make_nonce(&t->rx, nonce);
        uint32_t clen = n - 16;
        int bad = (t->suite == TLS_AES_128_GCM_SHA256)
            ? aes_gcm_open(&t->rx.gcm, nonce, t->rec, 5, t->rec + 5, clen, t->rec + 5 + clen)
            : chacha20poly1305_open(t->rx.key, nonce, t->rec, 5, t->rec + 5, clen, t->rec + 5 + clen);
        if (bad) { fail(t, "record authentication failed (bad_record_mac)"); return NET_ERR_PROTO; }
        t->rx.seq++;
        /* strip zero padding; the last non-zero byte is the real type */
        while (clen > 0 && t->rec[5 + clen - 1] == 0) clen--;
        if (clen == 0) { fail(t, "record without content type"); return NET_ERR_PROTO; }
        type = t->rec[5 + clen - 1];
        *len = clen - 1;
        return type;
    }
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
    case 109: return "missing_extension";
    case 110: return "unsupported_extension";
    case 112: return "unrecognized_name";
    case 120: return "no_application_protocol";
    default:  return "alert";
    }
}

static int hs_append(tls_conn_t* t, const uint8_t* data, uint32_t len) {
    if (t->hs_len + len > t->hs_cap) {
        uint32_t cap = t->hs_cap ? t->hs_cap : 8192;
        while (cap < t->hs_len + len) cap *= 2;
        if (cap > 256u * 1024u) { fail(t, "handshake message too large"); return NET_ERR_PROTO; }
        uint8_t* p = (uint8_t*)krealloc(t->hs, cap);
        if (!p) return NET_ERR_NOMEM;
        t->hs = p;
        t->hs_cap = cap;
    }
    memcpy(t->hs + t->hs_len, data, len);
    t->hs_len += len;
    return NET_OK;
}

/* Next complete handshake message (header + body) into *msg / *msg_len;
 * the caller must hs_consume() it. */
static int next_hs_msg(tls_conn_t* t, uint8_t** msg, uint32_t* msg_len) {
    for (;;) {
        if (t->hs_len >= 4) {
            uint32_t body = ((uint32_t)t->hs[1] << 16) | ((uint32_t)t->hs[2] << 8) | t->hs[3];
            if (t->hs_len >= 4 + body) {
                *msg = t->hs;
                *msg_len = 4 + body;
                return NET_OK;
            }
        }
        uint32_t len;
        int type = read_record(t, &len);
        if (type < 0) return type;
        if (type == CT_ALERT) {
            fail(t, "server sent alert: %s (%u)", len >= 2 ? alert_name(t->rec[6]) : "?",
                 len >= 2 ? t->rec[6] : 0);
            return NET_ERR_PROTO;
        }
        if (type != CT_HANDSHAKE) { fail(t, "expected handshake, got record type %d", type); return NET_ERR_PROTO; }
        int r = hs_append(t, t->rec + 5, len);
        if (r != NET_OK) return r;
    }
}

static void hs_consume(tls_conn_t* t, uint32_t n) {
    memmove(t->hs, t->hs + n, t->hs_len - n);
    t->hs_len -= n;
}

/* ── ClientHello (section 4.1.2) ────────────────────────────────── */

static uint32_t build_client_hello(uint8_t* m, const char* host, const uint8_t pub[32], int ciphers) {
    uint32_t o = 4;   /* handshake header filled in at the end */
    m[o++] = 3; m[o++] = 3;                         /* legacy_version TLS 1.2 */
    random_bytes(m + o, 32); o += 32;               /* random */
    m[o++] = 32;                                    /* legacy_session_id (compat mode) */
    random_bytes(m + o, 32); o += 32;
    /* cipher suites: ChaCha20 first - without AES instructions it's the
     * faster of the two, and servers like Cloudflare honour the hint */
    {
        uint32_t n = 0, pos = o;
        o += 2;
        if (ciphers != TLS_CIPHERS_AES)    { m[o++] = 0x13; m[o++] = 0x03; n++; }
        if (ciphers != TLS_CIPHERS_CHACHA) { m[o++] = 0x13; m[o++] = 0x01; n++; }
        wr16(m + pos, (uint16_t)(n * 2));
    }
    m[o++] = 1; m[o++] = 0;                         /* compression: null */

    uint32_t ext_len_pos = o;
    o += 2;

    ip4_t dummy;
    if (!str_to_ip4(host, &dummy)) {                /* SNI (never for IP literals) */
        uint32_t hl = (uint32_t)strlen(host);
        m[o++] = 0x00; m[o++] = 0x00;
        wr16(m + o, (uint16_t)(hl + 5)); o += 2;
        wr16(m + o, (uint16_t)(hl + 3)); o += 2;
        m[o++] = 0;                                 /* host_name */
        wr16(m + o, (uint16_t)hl); o += 2;
        memcpy(m + o, host, hl); o += hl;
    }
    /* supported_groups: x25519 */
    m[o++] = 0x00; m[o++] = 0x0a; m[o++] = 0; m[o++] = 4; m[o++] = 0; m[o++] = 2;
    m[o++] = 0x00; m[o++] = 0x1d;
    /* signature_algorithms (we don't verify, but must offer the usual ones) */
    static const uint16_t sigs[] = { 0x0403, 0x0804, 0x0401, 0x0503, 0x0805, 0x0501,
                                     0x0806, 0x0601, 0x0807, 0x0201, 0x0203 };
    uint32_t ns = sizeof(sigs) / sizeof(sigs[0]);
    m[o++] = 0x00; m[o++] = 0x0d;
    wr16(m + o, (uint16_t)(2 + ns * 2)); o += 2;
    wr16(m + o, (uint16_t)(ns * 2)); o += 2;
    for (uint32_t i = 0; i < ns; i++) { wr16(m + o, sigs[i]); o += 2; }
    /* supported_versions: TLS 1.3 only */
    m[o++] = 0x00; m[o++] = 0x2b; m[o++] = 0; m[o++] = 3; m[o++] = 2; m[o++] = 3; m[o++] = 4;
    /* psk_key_exchange_modes: psk_dhe_ke */
    m[o++] = 0x00; m[o++] = 0x2d; m[o++] = 0; m[o++] = 2; m[o++] = 1; m[o++] = 1;
    /* key_share: one x25519 share */
    m[o++] = 0x00; m[o++] = 0x33; m[o++] = 0; m[o++] = 38; m[o++] = 0; m[o++] = 36;
    m[o++] = 0x00; m[o++] = 0x1d; m[o++] = 0; m[o++] = 32;
    memcpy(m + o, pub, 32); o += 32;
    /* ALPN: http/1.1 */
    m[o++] = 0x00; m[o++] = 0x10; m[o++] = 0; m[o++] = 11; m[o++] = 0; m[o++] = 9;
    m[o++] = 8; memcpy(m + o, "http/1.1", 8); o += 8;

    wr16(m + ext_len_pos, (uint16_t)(o - ext_len_pos - 2));
    m[0] = HS_CLIENT_HELLO;
    m[1] = (uint8_t)((o - 4) >> 16);
    m[2] = (uint8_t)((o - 4) >> 8);
    m[3] = (uint8_t)(o - 4);
    return o;
}

/* ── ServerHello (section 4.1.3) ────────────────────────────────── */

static const uint8_t HRR_RANDOM[32] = {
    0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91,
    0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C,
};

static int parse_server_hello(tls_conn_t* t, const uint8_t* m, uint32_t len, uint8_t server_pub[32]) {
    if (len < 4 + 2 + 32 + 1 || m[0] != HS_SERVER_HELLO) {
        fail(t, "expected ServerHello");
        return NET_ERR_PROTO;
    }
    const uint8_t* p = m + 4;
    const uint8_t* end = m + len;
    if (memcmp(p + 2, HRR_RANDOM, 32) == 0) {
        fail(t, "server asked for a HelloRetryRequest (it doesn't support X25519)");
        return NET_ERR_PROTO;
    }
    p += 2 + 32;
    uint32_t sid = *p++;
    if (p + sid + 3 > end) goto bad;
    p += sid;
    t->suite = rd16(p);
    p += 2;
    if (t->suite != TLS_AES_128_GCM_SHA256 && t->suite != TLS_CHACHA20_POLY1305_SHA256) {
        fail(t, "server chose unsupported cipher suite 0x%04x", t->suite);
        return NET_ERR_PROTO;
    }
    p++;                                   /* compression */
    if (p + 2 > end) goto bad;
    uint32_t elen = rd16(p);
    p += 2;
    if (p + elen > end) goto bad;
    int version_ok = 0, have_key = 0;
    const uint8_t* e = p;
    while (e + 4 <= p + elen) {
        uint16_t type = rd16(e), l = rd16(e + 2);
        const uint8_t* v = e + 4;
        if (v + l > p + elen) goto bad;
        if (type == 0x002b && l == 2 && rd16(v) == 0x0304) version_ok = 1;
        if (type == 0x0033 && l >= 4 && rd16(v) == 0x001d && rd16(v + 2) == 32 && l >= 36) {
            memcpy(server_pub, v + 4, 32);
            have_key = 1;
        }
        e = v + l;
    }
    if (!version_ok) { fail(t, "server does not speak TLS 1.3"); return NET_ERR_PROTO; }
    if (!have_key) { fail(t, "ServerHello without an X25519 key share"); return NET_ERR_PROTO; }
    return NET_OK;
bad:
    fail(t, "malformed ServerHello");
    return NET_ERR_PROTO;
}

/* ── handshake ──────────────────────────────────────────────────── */

static int handshake(tls_conn_t* t, const char* host) {
    uint8_t priv[32], pub[32], server_pub[32], shared[32];
    random_bytes(priv, 32);
    x25519_base(pub, priv);

    uint8_t* ch = (uint8_t*)kmalloc(1024);
    if (!ch) return NET_ERR_NOMEM;
    uint32_t ch_len = build_client_hello(ch, host, pub, t->ciphers);
    sha256_update(&t->transcript, ch, ch_len);
    int r = send_record(t, CT_HANDSHAKE, ch, ch_len);
    kfree(ch);
    if (r != NET_OK) { fail(t, "could not send ClientHello: %s", net_strerror(r)); return r; }

    uint8_t* msg;
    uint32_t mlen;
    r = next_hs_msg(t, &msg, &mlen);
    if (r != NET_OK) {
        if (t->errmsg && !t->errmsg[0]) fail(t, "no ServerHello (%s)", net_strerror(r));
        return r;
    }
    r = parse_server_hello(t, msg, mlen, server_pub);
    if (r != NET_OK) return r;
    sha256_update(&t->transcript, msg, mlen);
    hs_consume(t, mlen);

    /* handshake secrets */
    x25519(shared, priv, server_pub);
    memset(priv, 0, sizeof(priv));
    uint8_t zeros[32], empty_hash[32], early[32], derived[32], hs_secret[32], th[32];
    uint8_t c_hs[32], s_hs[32], master[32];
    memset(zeros, 0, 32);
    sha256("", 0, empty_hash);
    hkdf_extract(NULL, 0, zeros, 32, early);
    hkdf_label(early, "derived", empty_hash, 32, derived, 32);
    hkdf_extract(derived, 32, shared, 32, hs_secret);
    transcript_hash(t, th);
    hkdf_label(hs_secret, "c hs traffic", th, 32, c_hs, 32);
    hkdf_label(hs_secret, "s hs traffic", th, 32, s_hs, 32);
    hkdf_label(hs_secret, "derived", empty_hash, 32, derived, 32);
    hkdf_extract(derived, 32, zeros, 32, master);
    set_keys(t, &t->rx, s_hs);

    /* EncryptedExtensions, [CertificateRequest], Certificate,
     * CertificateVerify, Finished */
    int cert_requested = 0;
    for (;;) {
        r = next_hs_msg(t, &msg, &mlen);
        if (r != NET_OK) {
            if (t->errmsg && !t->errmsg[0]) fail(t, "handshake read failed (%s)", net_strerror(r));
            return r;
        }
        uint8_t type = msg[0];
        if (type == HS_FINISHED) {
            uint8_t fkey[32], expect[32];
            hkdf_label(s_hs, "finished", NULL, 0, fkey, 32);
            transcript_hash(t, th);
            hmac_sha256(fkey, 32, th, 32, expect);
            if (mlen != 4 + 32 || ct_memcmp(expect, msg + 4, 32) != 0) {
                fail(t, "server Finished MAC is wrong");
                return NET_ERR_PROTO;
            }
            sha256_update(&t->transcript, msg, mlen);
            hs_consume(t, mlen);
            break;
        }
        if (type == HS_CERT_REQUEST) cert_requested = 1;
        else if (type != HS_ENC_EXTENSIONS && type != HS_CERTIFICATE && type != HS_CERT_VERIFY) {
            fail(t, "unexpected handshake message %u", type);
            return NET_ERR_PROTO;
        }
        /* Certificate / CertificateVerify are hashed but not validated */
        sha256_update(&t->transcript, msg, mlen);
        hs_consume(t, mlen);
    }

    /* application secrets come from the transcript through server Finished */
    uint8_t c_ap[32], s_ap[32];
    transcript_hash(t, th);
    hkdf_label(master, "c ap traffic", th, 32, c_ap, 32);
    hkdf_label(master, "s ap traffic", th, 32, s_ap, 32);

    /* our flight: [CCS], [empty Certificate], Finished */
    static const uint8_t ccs = 1;
    send_record(t, CT_CCS, &ccs, 1);
    set_keys(t, &t->tx, c_hs);
    if (cert_requested) {
        static const uint8_t empty_cert[8] = { HS_CERTIFICATE, 0, 0, 4, 0, 0, 0, 0 };
        sha256_update(&t->transcript, empty_cert, sizeof(empty_cert));
        send_record(t, CT_HANDSHAKE, empty_cert, sizeof(empty_cert));
    }
    uint8_t fin[4 + 32], fkey[32];
    hkdf_label(c_hs, "finished", NULL, 0, fkey, 32);
    transcript_hash(t, th);
    fin[0] = HS_FINISHED; fin[1] = 0; fin[2] = 0; fin[3] = 32;
    hmac_sha256(fkey, 32, th, 32, fin + 4);
    sha256_update(&t->transcript, fin, sizeof(fin));
    r = send_record(t, CT_HANDSHAKE, fin, sizeof(fin));
    if (r != NET_OK) { fail(t, "could not send Finished"); return r; }

    set_keys(t, &t->rx, s_ap);
    set_keys(t, &t->tx, c_ap);
    memset(shared, 0, sizeof(shared));
    memset(hs_secret, 0, sizeof(hs_secret));
    memset(master, 0, sizeof(master));
    return NET_OK;
}

tls_conn_t* tls_connect(tcp_conn_t* tcp, const char* server_name, int ciphers, uint32_t timeout_ms,
                        int* err, char* errmsg, uint32_t errmsg_len) {
    if (errmsg && errmsg_len) errmsg[0] = '\0';
    tls_conn_t* t = (tls_conn_t*)kzalloc(sizeof(tls_conn_t));
    if (!t) { *err = NET_ERR_NOMEM; return NULL; }
    t->tcp = tcp;
    t->ciphers = ciphers;
    t->timeout_ms = timeout_ms;
    t->errmsg = errmsg;
    t->errmsg_len = errmsg_len;
    sha256_init(&t->transcript);

    int r = handshake(t, server_name);
    if (r != NET_OK) {
        *err = r;
        klog("tls: handshake with %s failed: %s\n", server_name, errmsg ? errmsg : "?");
        kfree(t->hs);
        kfree(t);
        return NULL;
    }
    t->errmsg = NULL;
    *err = NET_OK;
    return t;
}

/* post-handshake messages: session tickets (ignored), KeyUpdate */
static int post_handshake(tls_conn_t* t, const uint8_t* data, uint32_t len) {
    int r = hs_append(t, data, len);
    if (r != NET_OK) return r;
    while (t->hs_len >= 4) {
        uint32_t body = ((uint32_t)t->hs[1] << 16) | ((uint32_t)t->hs[2] << 8) | t->hs[3];
        if (t->hs_len < 4 + body) break;
        if (t->hs[0] == HS_KEY_UPDATE && body == 1) {
            uint8_t next[32];
            hkdf_label(t->rx.secret, "traffic upd", NULL, 0, next, 32);
            set_keys(t, &t->rx, next);
            if (t->hs[4] == 1) {                    /* update_requested */
                static const uint8_t ku[5] = { HS_KEY_UPDATE, 0, 0, 1, 0 };
                send_record(t, CT_HANDSHAKE, ku, 5);
                hkdf_label(t->tx.secret, "traffic upd", NULL, 0, next, 32);
                set_keys(t, &t->tx, next);
            }
        }
        hs_consume(t, 4 + body);
    }
    return NET_OK;
}

int tls_read(tls_conn_t* t, void* buf, uint32_t max, uint32_t timeout_ms) {
    t->timeout_ms = timeout_ms;
    while (t->app_pos >= t->app_len) {
        if (t->eof) return 0;
        uint32_t len;
        int type = read_record(t, &len);
        if (type == NET_ERR_CLOSED) { t->eof = 1; return 0; }   /* TCP FIN without close_notify */
        if (type < 0) return type;
        if (type == CT_APPDATA) {
            t->app = t->rec + 5;
            t->app_pos = 0;
            t->app_len = len;
        } else if (type == CT_HANDSHAKE) {
            int r = post_handshake(t, t->rec + 5, len);
            if (r != NET_OK) return r;
        } else if (type == CT_ALERT) {
            if (len >= 2 && t->rec[6] == 0) { t->eof = 1; return 0; }   /* close_notify */
            klog("tls: alert %u\n", len >= 2 ? t->rec[6] : 0);
            return NET_ERR_PROTO;
        } else {
            return NET_ERR_PROTO;
        }
    }
    uint32_t n = t->app_len - t->app_pos;
    if (n > max) n = max;
    memcpy(buf, t->app + t->app_pos, n);
    t->app_pos += n;
    return (int)n;
}

int tls_write(tls_conn_t* t, const void* data, uint32_t len, uint32_t timeout_ms) {
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

void tls_close(tls_conn_t* t) {
    if (!t) return;
    static const uint8_t close_notify[2] = { 1, 0 };
    if (tcp_state(t->tcp) == TCP_ESTABLISHED || tcp_state(t->tcp) == TCP_CLOSE_WAIT)
        send_record(t, CT_ALERT, close_notify, 2);
    memset(&t->rx, 0, sizeof(t->rx));
    memset(&t->tx, 0, sizeof(t->tx));
    kfree(t->hs);
    kfree(t);
}
