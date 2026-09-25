#include "sshd.h"
#include "tcp.h"
#include "tty.h"
#include "task.h"
#include "timer.h"
#include "kstring.h"
#include "kheap.h"
#include "random.h"
#include "serial.h"
#include "terminal.h"
#include "fs.h"
#include "config.h"
#include "passwd.h"
#include "sha256.h"
#include "x25519.h"
#include "ed25519.h"
#include "aead.h"
#include "chacha20.h"
#include "../shell/shell.h"

#define SERVER_VERSION "SSH-2.0-BananaOS_0.5"
#define HOSTKEY_FILE   "/etc/ssh/ssh_host_ed25519_key"
#define PKT_MAX        35000u          /* largest packet_length accepted (RFC 4253 6.1) */
#define BUF_SIZE       (PKT_MAX + 64u)
#define OUR_WINDOW     (2u << 20)
#define OUR_MAXPKT     32768u
#define LOGIN_GRACE_MS 120000u
#define MAX_AUTH_FAIL  5

enum {
    MSG_DISCONNECT = 1, MSG_IGNORE = 2, MSG_UNIMPLEMENTED = 3, MSG_DEBUG = 4,
    MSG_SERVICE_REQUEST = 5, MSG_SERVICE_ACCEPT = 6,
    MSG_KEXINIT = 20, MSG_NEWKEYS = 21, MSG_KEX_ECDH_INIT = 30, MSG_KEX_ECDH_REPLY = 31,
    MSG_USERAUTH_REQUEST = 50, MSG_USERAUTH_FAILURE = 51, MSG_USERAUTH_SUCCESS = 52,
    MSG_GLOBAL_REQUEST = 80, MSG_REQUEST_FAILURE = 82,
    MSG_CHANNEL_OPEN = 90, MSG_CHANNEL_OPEN_CONFIRMATION = 91, MSG_CHANNEL_OPEN_FAILURE = 92,
    MSG_CHANNEL_WINDOW_ADJUST = 93, MSG_CHANNEL_DATA = 94, MSG_CHANNEL_EXTENDED_DATA = 95,
    MSG_CHANNEL_EOF = 96, MSG_CHANNEL_CLOSE = 97, MSG_CHANNEL_REQUEST = 98,
    MSG_CHANNEL_SUCCESS = 99, MSG_CHANNEL_FAILURE = 100,
};

#define DISC_PROTOCOL_ERROR       2
#define DISC_KEY_EXCHANGE_FAILED  3
#define DISC_MAC_ERROR            5
#define DISC_SERVICE_NOT_AVAIL    7
#define DISC_BY_APPLICATION       11
#define DISC_NO_MORE_AUTH         14

enum { CIPHER_NONE, CIPHER_CHACHA, CIPHER_AESGCM };
static const char* const CIPHERS[] = { "chacha20-poly1305@openssh.com", "aes128-gcm@openssh.com" };

typedef struct {
    int        cipher;
    uint8_t    key[64];         /* chacha: main key || header key */
    uint8_t    iv[12];          /* aes-gcm: fixed(4) || invocation counter(8) */
    aes_gcm_t  gcm;
} keys_t;

typedef struct {
    tcp_conn_t* c;
    int      slot, tty;
    char     peer[16];
    uint32_t start_ms;
    int      dead;

    /* transport */
    int      got_version;
    char     v_c[256];
    uint8_t* i_c; uint32_t i_c_len;     /* KEXINIT payloads, for the exchange hash */
    uint8_t* i_s; uint32_t i_s_len;
    uint8_t  sid[32];
    int      have_sid;
    uint32_t seq_in, seq_out;
    keys_t   in, out, next_in, next_out;
    int      kexinit_sent, in_kex, kex_done_once, strict, ignore_next;
    int      c2s, s2c;                  /* negotiated ciphers */

    uint8_t* rx;  uint32_t rx_len;      /* raw bytes from the client */
    uint8_t* pkt;                       /* current decrypted payload */
    uint8_t* tx;                        /* outgoing packet */
    uint8_t* pl;                        /* payload being built */

    /* auth */
    int      authed, failures;
    char     user[32];

    /* the one session channel */
    int      chan_open, shell, closing, close_rcvd;
    uint32_t peer_chan, peer_win, peer_max, our_win;
    uint32_t close_deadline;
} ssh_t;

static tcp_listener_t* g_listener;
static volatile int    g_running;
static uint16_t        g_port;
static int             g_tasks_started;
static uint8_t         g_seed[32], g_pk[32];
static int             g_have_key;
static struct { int active; char user[32]; char peer[16]; uint32_t since; } g_info[TTY_MAX];
static int             g_ttys[TTY_MAX];

/* ── byte buffers ─────────────────────────────────────────────────── */

typedef struct { uint8_t* p; uint32_t len, cap; } wbuf_t;
typedef struct { const uint8_t* p; uint32_t len, off; int err; } rbuf_t;

static void w_bytes(wbuf_t* w, const void* d, uint32_t n) {
    if (w->len + n > w->cap) { w->len = w->cap + 1; return; }
    memcpy(w->p + w->len, d, n);
    w->len += n;
}
static void w_u8(wbuf_t* w, uint8_t v) { w_bytes(w, &v, 1); }
static void w_u32(wbuf_t* w, uint32_t v) { uint8_t b[4]; wr32(b, v); w_bytes(w, b, 4); }
static void w_str(wbuf_t* w, const void* d, uint32_t n) { w_u32(w, n); w_bytes(w, d, n); }
static void w_cstr(wbuf_t* w, const char* s) { w_str(w, s, (uint32_t)strlen(s)); }
/* a big-endian unsigned number as an SSH mpint */
static void w_mpint(wbuf_t* w, const uint8_t* be, uint32_t n) {
    while (n && *be == 0) { be++; n--; }
    if (n && (*be & 0x80)) { w_u32(w, n + 1); w_u8(w, 0); }
    else w_u32(w, n);
    w_bytes(w, be, n);
}

static uint8_t r_u8(rbuf_t* r) {
    if (r->off + 1 > r->len) { r->err = 1; return 0; }
    return r->p[r->off++];
}
static uint32_t r_u32(rbuf_t* r) {
    if (r->off + 4 > r->len) { r->err = 1; return 0; }
    uint32_t v = rd32(r->p + r->off);
    r->off += 4;
    return v;
}
static const uint8_t* r_str(rbuf_t* r, uint32_t* n) {
    uint32_t l = r_u32(r);
    if (r->err || r->off + l > r->len) { r->err = 1; *n = 0; return (const uint8_t*)""; }
    const uint8_t* p = r->p + r->off;
    r->off += l;
    *n = l;
    return p;
}
/* copies a string field into a C string (truncating) */
static void r_cstr(rbuf_t* r, char* out, uint32_t cap) {
    uint32_t n;
    const uint8_t* p = r_str(r, &n);
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}
static int str_eq(const uint8_t* p, uint32_t n, const char* s) {
    return strlen(s) == n && memcmp(p, s, n) == 0;
}

/* is `name` in the comma-separated list? */
static int list_has(const uint8_t* list, uint32_t n, const char* name) {
    uint32_t i = 0, nl = (uint32_t)strlen(name);
    while (i <= n) {
        uint32_t j = i;
        while (j < n && list[j] != ',') j++;
        if (j - i == nl && memcmp(list + i, name, nl) == 0) return 1;
        i = j + 1;
    }
    return 0;
}

/* the client's first choice that we support: index into ours[], or -1 */
static int choose(const uint8_t* list, uint32_t n, const char* const* ours, int count) {
    uint32_t i = 0;
    while (i < n) {
        uint32_t j = i;
        while (j < n && list[j] != ',') j++;
        for (int k = 0; k < count; k++)
            if (strlen(ours[k]) == j - i && memcmp(list + i, ours[k], j - i) == 0) return k;
        i = j + 1;
    }
    return -1;
}

/* ── host key ─────────────────────────────────────────────────────── */

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int load_hostkey(void) {
    if (g_have_key) return 0;
    int idx = fs_find_file(HOSTKEY_FILE);
    int ok = 0;
    if (idx >= 0) {
        fs_file_t* f = fs_get_file(idx);
        if (f->size >= 64) {
            ok = 1;
            for (int i = 0; i < 32 && ok; i++) {
                int h = hexv(f->content[2 * i]), l = hexv(f->content[2 * i + 1]);
                if (h < 0 || l < 0) ok = 0;
                else g_seed[i] = (uint8_t)(h * 16 + l);
            }
        }
    }
    if (!ok) {
        /* first start: a new host key, kept so clients recognise us */
        static const char hx[] = "0123456789abcdef";
        char text[80];
        random_bytes(g_seed, 32);
        for (int i = 0; i < 32; i++) {
            text[2 * i] = hx[g_seed[i] >> 4];
            text[2 * i + 1] = hx[g_seed[i] & 15];
        }
        text[64] = '\n';
        fs_mkdir_p("/etc/ssh");
        if (fs_write_path(HOSTKEY_FILE, text, 65) < 0) return -1;
        cfg_persist();
        klog("sshd: generated a new ed25519 host key\n");
    }
    ed25519_public_key(g_pk, g_seed);
    g_have_key = 1;
    return 0;
}

static uint32_t hostkey_blob(uint8_t* out) {
    wbuf_t w = { out, 0, 64 };
    w_cstr(&w, "ssh-ed25519");
    w_str(&w, g_pk, 32);
    return w.len;
}

int sshd_fingerprint(char* out, int cap) {
    if (load_hostkey() != 0) return -1;
    uint8_t blob[64], h[32];
    sha256(blob, hostkey_blob(blob), h);
    /* base64 without padding, as ssh-keygen -l prints it */
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char s[48];
    int n = 0;
    for (int i = 0; i < 32; i += 3) {
        uint32_t v = (uint32_t)h[i] << 16;
        if (i + 1 < 32) v |= (uint32_t)h[i + 1] << 8;
        if (i + 2 < 32) v |= h[i + 2];
        s[n++] = b64[(v >> 18) & 63];
        s[n++] = b64[(v >> 12) & 63];
        if (i + 1 < 32) s[n++] = b64[(v >> 6) & 63];
        if (i + 2 < 32) s[n++] = b64[v & 63];
    }
    s[n] = '\0';
    ksnprintf(out, (size_t)cap, "SHA256:%s", s);
    return 0;
}

/* ── packets ──────────────────────────────────────────────────────── */

static void chacha_nonce(uint8_t nonce[12], uint32_t seq) {
    memset(nonce, 0, 12);
    wr32(nonce + 8, seq);               /* 64-bit big-endian sequence number */
}

static void iv_increment(uint8_t iv[12]) {
    for (int i = 11; i >= 4; i--)
        if (++iv[i]) break;
}

static int send_packet(ssh_t* s, const uint8_t* payload, uint32_t plen) {
    keys_t* k = &s->out;
    int aead = k->cipher != CIPHER_NONE;
    uint32_t bs = k->cipher == CIPHER_AESGCM ? 16 : 8;
    uint32_t base = 1 + plen + (aead ? 0 : 4);   /* the AEAD modes leave the length out */
    uint32_t pad = bs - (base % bs);
    if (pad < 4) pad += bs;
    uint32_t pktlen = 1 + plen + pad;
    if (4 + pktlen + 16 > BUF_SIZE) return -1;
    uint8_t* b = s->tx;
    wr32(b, pktlen);
    b[4] = (uint8_t)pad;
    memcpy(b + 5, payload, plen);
    random_bytes(b + 5 + plen, pad);
    uint32_t total = 4 + pktlen;

    if (k->cipher == CIPHER_AESGCM) {
        aes_gcm_seal(&k->gcm, k->iv, b, 4, b + 4, pktlen, b + total);
        iv_increment(k->iv);
        total += 16;
    } else if (k->cipher == CIPHER_CHACHA) {
        uint8_t nonce[12], poly[64];
        chacha_nonce(nonce, s->seq_out);
        chacha20_xor(k->key + 32, 0, nonce, b, 4);          /* length: header key */
        chacha20_block(k->key, 0, nonce, poly);             /* Poly1305 key */
        chacha20_xor(k->key, 1, nonce, b + 4, pktlen);      /* body: main key */
        poly1305_mac(b + total, b, total, poly);
        total += 16;
    }
    s->seq_out++;
    if (tcp_send(s->c, b, total, 30000) != (int)total) { s->dead = 1; return -1; }
    return 0;
}

static int send_wbuf(ssh_t* s, wbuf_t* w) {
    if (w->len > w->cap) return -1;
    return send_packet(s, w->p, w->len);
}

static void disconnect(ssh_t* s, uint32_t reason, const char* msg) {
    wbuf_t w = { s->pl, 0, BUF_SIZE };
    w_u8(&w, MSG_DISCONNECT);
    w_u32(&w, reason);
    w_cstr(&w, msg);
    w_cstr(&w, "");
    send_wbuf(s, &w);
    klog("sshd: %s: disconnect: %s\n", s->peer, msg);
    s->dead = 1;
}

/* One complete packet from the receive buffer: payload length (payload in
 * s->pkt), 0 if more bytes are needed, -1 on a protocol/MAC error. */
static int next_packet(ssh_t* s) {
    keys_t* k = &s->in;
    if (s->rx_len < 4) return 0;
    uint8_t* b = s->rx;
    uint8_t nonce[12];
    uint32_t pktlen;
    if (k->cipher == CIPHER_CHACHA) {
        uint8_t l[4];
        memcpy(l, b, 4);
        chacha_nonce(nonce, s->seq_in);
        chacha20_xor(k->key + 32, 0, nonce, l, 4);
        pktlen = rd32(l);
    } else {
        pktlen = rd32(b);
    }
    uint32_t bs = k->cipher == CIPHER_AESGCM ? 16 : 8;
    uint32_t mac = k->cipher != CIPHER_NONE ? 16 : 0;
    if (pktlen < 5 || pktlen > PKT_MAX) return -1;
    if (((k->cipher != CIPHER_NONE) ? pktlen : pktlen + 4) % bs != 0) return -1;
    uint32_t total = 4 + pktlen + mac;
    if (s->rx_len < total) return 0;

    if (k->cipher == CIPHER_AESGCM) {
        if (aes_gcm_open(&k->gcm, k->iv, b, 4, b + 4, pktlen, b + 4 + pktlen) != 0) return -1;
        iv_increment(k->iv);
    } else if (k->cipher == CIPHER_CHACHA) {
        uint8_t poly[64], tag[16];
        chacha20_block(k->key, 0, nonce, poly);
        poly1305_mac(tag, b, 4 + pktlen, poly);
        if (ct_memcmp(tag, b + 4 + pktlen, 16) != 0) return -1;
        chacha20_xor(k->key, 1, nonce, b + 4, pktlen);
    }
    s->seq_in++;
    uint32_t pad = b[4];
    if (pad < 4 || pad + 1 >= pktlen) return -1;
    uint32_t plen = pktlen - 1 - pad;
    memcpy(s->pkt, b + 5, plen);
    memmove(s->rx, s->rx + total, s->rx_len - total);
    s->rx_len -= total;
    return (int)plen;
}

/* ── key exchange ─────────────────────────────────────────────────── */

static void send_kexinit(ssh_t* s) {
    wbuf_t w = { s->pl, 0, BUF_SIZE };
    uint8_t cookie[16];
    random_bytes(cookie, 16);
    w_u8(&w, MSG_KEXINIT);
    w_bytes(&w, cookie, 16);
    w_cstr(&w, "curve25519-sha256,curve25519-sha256@libssh.org,kex-strict-s-v00@openssh.com");
    w_cstr(&w, "ssh-ed25519");
    w_cstr(&w, "chacha20-poly1305@openssh.com,aes128-gcm@openssh.com");
    w_cstr(&w, "chacha20-poly1305@openssh.com,aes128-gcm@openssh.com");
    w_cstr(&w, "hmac-sha2-256-etm@openssh.com,hmac-sha2-256");   /* unused with AEAD ciphers */
    w_cstr(&w, "hmac-sha2-256-etm@openssh.com,hmac-sha2-256");
    w_cstr(&w, "none");
    w_cstr(&w, "none");
    w_cstr(&w, "");
    w_cstr(&w, "");
    w_u8(&w, 0);
    w_u32(&w, 0);
    kfree(s->i_s);
    s->i_s = (uint8_t*)kmalloc(w.len);
    if (s->i_s) { memcpy(s->i_s, w.p, w.len); s->i_s_len = w.len; }
    send_wbuf(s, &w);
    s->kexinit_sent = 1;
}

static int on_kexinit(ssh_t* s, const uint8_t* p, uint32_t n) {
    kfree(s->i_c);
    s->i_c = (uint8_t*)kmalloc(n);
    if (!s->i_c) return -1;
    memcpy(s->i_c, p, n);
    s->i_c_len = n;

    rbuf_t r = { p, n, 17, 0 };                     /* type + cookie */
    uint32_t ln[10];
    const uint8_t* lists[10];
    for (int i = 0; i < 10; i++) lists[i] = r_str(&r, &ln[i]);
    int follows = r_u8(&r);
    if (r.err) return -1;

    static const char* const KEX[] = { "curve25519-sha256", "curve25519-sha256@libssh.org" };
    if (choose(lists[0], ln[0], KEX, 2) < 0) {
        disconnect(s, DISC_KEY_EXCHANGE_FAILED, "no common key exchange (need curve25519-sha256)");
        return -1;
    }
    if (!s->kex_done_once && list_has(lists[0], ln[0], "kex-strict-c-v00@openssh.com")) s->strict = 1;
    if (!list_has(lists[1], ln[1], "ssh-ed25519")) {
        disconnect(s, DISC_KEY_EXCHANGE_FAILED, "no common host key type (need ssh-ed25519)");
        return -1;
    }
    int c2s = choose(lists[2], ln[2], CIPHERS, 2);
    int s2c = choose(lists[3], ln[3], CIPHERS, 2);
    if (c2s < 0 || s2c < 0) {
        disconnect(s, DISC_KEY_EXCHANGE_FAILED,
                   "no common cipher (need chacha20-poly1305@openssh.com or aes128-gcm@openssh.com)");
        return -1;
    }
    if (!list_has(lists[6], ln[6], "none") || !list_has(lists[7], ln[7], "none")) {
        disconnect(s, DISC_KEY_EXCHANGE_FAILED, "compression is not supported");
        return -1;
    }
    s->c2s = c2s == 0 ? CIPHER_CHACHA : CIPHER_AESGCM;
    s->s2c = s2c == 0 ? CIPHER_CHACHA : CIPHER_AESGCM;
    /* The client may send its first key exchange packet right away,
     * guessing the algorithms; a wrong guess (its first choices are not
     * what was negotiated) must be ignored (RFC 4253 7). */
    if (follows) {
        uint32_t k0 = 0, h0 = 0;
        while (k0 < ln[0] && lists[0][k0] != ',') k0++;
        while (h0 < ln[1] && lists[1][h0] != ',') h0++;
        int kex_ok = choose(lists[0], k0, KEX, 2) >= 0;
        int hk_ok = str_eq(lists[1], h0, "ssh-ed25519");
        s->ignore_next = !(kex_ok && hk_ok);
    }
    if (!s->kexinit_sent) send_kexinit(s);
    s->in_kex = 1;
    return 0;
}

/* key = HASH(K || H || letter || session_id), extended with
 * HASH(K || H || K1) when more than 32 bytes are needed (RFC 4253 7.2) */
static void derive(ssh_t* s, const uint8_t* kmp, uint32_t kmp_len, const uint8_t h[32],
                   char letter, uint8_t* out, uint32_t need) {
    sha256_ctx_t c;
    uint8_t k1[32], k2[32];
    sha256_init(&c);
    sha256_update(&c, kmp, kmp_len);
    sha256_update(&c, h, 32);
    sha256_update(&c, &letter, 1);
    sha256_update(&c, s->sid, 32);
    sha256_final(&c, k1);
    memcpy(out, k1, need < 32 ? need : 32);
    if (need > 32) {
        sha256_init(&c);
        sha256_update(&c, kmp, kmp_len);
        sha256_update(&c, h, 32);
        sha256_update(&c, k1, 32);
        sha256_final(&c, k2);
        memcpy(out + 32, k2, need - 32);
    }
}

static void setup_keys(ssh_t* s, keys_t* k, int cipher, const uint8_t* kmp, uint32_t kmp_len,
                       const uint8_t h[32], char iv_letter, char key_letter) {
    memset(k, 0, sizeof(*k));
    k->cipher = cipher;
    if (cipher == CIPHER_CHACHA) {
        derive(s, kmp, kmp_len, h, key_letter, k->key, 64);
    } else {
        derive(s, kmp, kmp_len, h, iv_letter, k->iv, 12);
        derive(s, kmp, kmp_len, h, key_letter, k->key, 16);
        aes_gcm_init(&k->gcm, k->key);
    }
}

static int on_ecdh_init(ssh_t* s, const uint8_t* p, uint32_t n) {
    rbuf_t r = { p, n, 1, 0 };
    uint32_t qlen;
    const uint8_t* q_c = r_str(&r, &qlen);
    if (r.err || qlen != 32 || !s->i_c || !s->i_s) return -1;

    uint8_t e[32], q_s[32], k[32];
    random_bytes(e, 32);
    x25519_base(q_s, e);
    x25519(k, e, q_c);
    memset(e, 0, 32);
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++) acc |= k[i];
    if (!acc) return -1;                           /* low-order point from the client */

    uint8_t blob[64];
    uint32_t blob_len = hostkey_blob(blob);

    /* K as an mpint: the X25519 output read as a big-endian number (RFC 8731) */
    uint8_t kmp[40];
    wbuf_t km = { kmp, 0, sizeof(kmp) };
    w_mpint(&km, k, 32);

    /* H = SHA256(V_C || V_S || I_C || I_S || K_S || Q_C || Q_S || K) */
    sha256_ctx_t c;
    uint8_t h[32], tmp[4];
    sha256_init(&c);
#define HSTR(d, l) do { wr32(tmp, (uint32_t)(l)); sha256_update(&c, tmp, 4); sha256_update(&c, d, (uint32_t)(l)); } while (0)
    HSTR(s->v_c, strlen(s->v_c));
    HSTR(SERVER_VERSION, strlen(SERVER_VERSION));
    HSTR(s->i_c, s->i_c_len);
    HSTR(s->i_s, s->i_s_len);
    HSTR(blob, blob_len);
    HSTR(q_c, 32);
    HSTR(q_s, 32);
#undef HSTR
    sha256_update(&c, kmp, km.len);
    sha256_final(&c, h);
    if (!s->have_sid) { memcpy(s->sid, h, 32); s->have_sid = 1; }

    uint8_t sig[64];
    ed25519_sign(sig, h, 32, g_seed, g_pk);

    wbuf_t w = { s->pl, 0, BUF_SIZE };
    w_u8(&w, MSG_KEX_ECDH_REPLY);
    w_str(&w, blob, blob_len);
    w_str(&w, q_s, 32);
    w_u32(&w, 4 + 11 + 4 + 64);
    w_cstr(&w, "ssh-ed25519");
    w_str(&w, sig, 64);
    if (send_wbuf(s, &w) != 0) return -1;

    setup_keys(s, &s->next_in, s->c2s, kmp, km.len, h, 'A', 'C');
    setup_keys(s, &s->next_out, s->s2c, kmp, km.len, h, 'B', 'D');
    memset(k, 0, 32);
    memset(kmp, 0, sizeof(kmp));

    uint8_t nk = MSG_NEWKEYS;
    if (send_packet(s, &nk, 1) != 0) return -1;
    s->out = s->next_out;                          /* everything after NEWKEYS uses the new keys */
    if (s->strict) s->seq_out = 0;
    return 0;
}

static void on_newkeys(ssh_t* s) {
    s->in = s->next_in;
    if (s->strict) s->seq_in = 0;
    s->in_kex = 0;
    s->kexinit_sent = 0;
    s->kex_done_once = 1;
    kfree(s->i_c); s->i_c = NULL;
    kfree(s->i_s); s->i_s = NULL;
}

/* ── authentication ───────────────────────────────────────────────── */

static void auth_failure(ssh_t* s) {
    wbuf_t w = { s->pl, 0, BUF_SIZE };
    w_u8(&w, MSG_USERAUTH_FAILURE);
    w_cstr(&w, "password");
    w_u8(&w, 0);
    send_wbuf(s, &w);
}

static void on_userauth(ssh_t* s, const uint8_t* p, uint32_t n) {
    if (s->authed) return;                         /* repeated requests are ignored */
    rbuf_t r = { p, n, 1, 0 };
    char user[32], service[32], method[32];
    r_cstr(&r, user, sizeof(user));
    r_cstr(&r, service, sizeof(service));
    r_cstr(&r, method, sizeof(method));
    if (r.err) { disconnect(s, DISC_PROTOCOL_ERROR, "bad userauth request"); return; }
    if (strcmp(method, "password") != 0) { auth_failure(s); return; }   /* "none", keys, ... */

    r_u8(&r);                                      /* "change password" flag */
    char pw[128];
    r_cstr(&r, pw, sizeof(pw));
    int ok = !r.err && strcmp(service, "ssh-connection") == 0 &&
             strcmp(user, PASSWD_USER) == 0 && passwd_check(user, pw);
    memset(pw, 0, sizeof(pw));
    if (ok) {
        uint8_t m = MSG_USERAUTH_SUCCESS;
        send_packet(s, &m, 1);
        s->authed = 1;
        kstrlcpy(s->user, user, sizeof(s->user));
        klog("sshd: %s: %s logged in\n", s->peer, user);
        return;
    }
    s->failures++;
    klog("sshd: %s: failed password for %s\n", s->peer, user);
    task_sleep_ms(1500);                           /* slows down password guessing */
    if (s->failures >= MAX_AUTH_FAIL) {
        disconnect(s, DISC_NO_MORE_AUTH, "too many authentication failures");
        return;
    }
    auth_failure(s);
}

/* ── the session channel ──────────────────────────────────────────── */

static void chan_reply(ssh_t* s, int ok) {
    wbuf_t w = { s->pl, 0, BUF_SIZE };
    w_u8(&w, ok ? MSG_CHANNEL_SUCCESS : MSG_CHANNEL_FAILURE);
    w_u32(&w, s->peer_chan);
    send_wbuf(s, &w);
}

static void on_channel_open(ssh_t* s, const uint8_t* p, uint32_t n) {
    rbuf_t r = { p, n, 1, 0 };
    uint32_t tl;
    const uint8_t* type = r_str(&r, &tl);
    uint32_t sender = r_u32(&r), win = r_u32(&r), maxp = r_u32(&r);
    if (r.err) { disconnect(s, DISC_PROTOCOL_ERROR, "bad channel open"); return; }
    wbuf_t w = { s->pl, 0, BUF_SIZE };
    if (!str_eq(type, tl, "session") || s->chan_open) {
        w_u8(&w, MSG_CHANNEL_OPEN_FAILURE);
        w_u32(&w, sender);
        w_u32(&w, 1);                              /* administratively prohibited */
        w_cstr(&w, s->chan_open ? "one session per connection" : "only session channels");
        w_cstr(&w, "");
        send_wbuf(s, &w);
        return;
    }
    s->chan_open = 1;
    s->peer_chan = sender;
    s->peer_win = win;
    s->peer_max = maxp < 1024 ? 1024 : maxp;
    s->our_win = OUR_WINDOW;
    w_u8(&w, MSG_CHANNEL_OPEN_CONFIRMATION);
    w_u32(&w, sender);
    w_u32(&w, 0);                                  /* our channel number */
    w_u32(&w, OUR_WINDOW);
    w_u32(&w, OUR_MAXPKT);
    send_wbuf(s, &w);
}

static void start_shell(ssh_t* s, const char* cmd, int want_reply) {
    if (s->shell) { if (want_reply) chan_reply(s, 0); return; }
    tty_attach(s->tty, s->user, cmd);
    s->shell = 1;
    g_info[s->slot].active = 1;
    if (want_reply) chan_reply(s, 1);
}

static void on_channel_request(ssh_t* s, const uint8_t* p, uint32_t n) {
    rbuf_t r = { p, n, 1, 0 };
    r_u32(&r);
    uint32_t tl;
    const uint8_t* type = r_str(&r, &tl);
    int want = r_u8(&r);
    if (r.err) return;
    if (str_eq(type, tl, "pty-req")) {
        if (want) chan_reply(s, 1);
    } else if (str_eq(type, tl, "shell")) {
        start_shell(s, "", want);
    } else if (str_eq(type, tl, "exec")) {
        char cmd[256];
        r_cstr(&r, cmd, sizeof(cmd));
        start_shell(s, cmd, want);
    } else if (str_eq(type, tl, "window-change") || str_eq(type, tl, "env")) {
        if (want) chan_reply(s, str_eq(type, tl, "window-change"));
    } else {
        /* subsystems (sftp), X11, agent forwarding, signals: not here */
        if (want) chan_reply(s, 0);
    }
}

static void on_channel_data(ssh_t* s, const uint8_t* p, uint32_t n) {
    rbuf_t r = { p, n, 1, 0 };
    r_u32(&r);
    uint32_t dl;
    const uint8_t* d = r_str(&r, &dl);
    if (r.err) return;
    if (s->shell) tty_input(s->tty, d, dl);
    s->our_win = s->our_win > dl ? s->our_win - dl : 0;
    if (s->our_win < OUR_WINDOW / 2) {
        wbuf_t w = { s->pl, 0, BUF_SIZE };
        w_u8(&w, MSG_CHANNEL_WINDOW_ADJUST);
        w_u32(&w, s->peer_chan);
        w_u32(&w, OUR_WINDOW - s->our_win);
        send_wbuf(s, &w);
        s->our_win = OUR_WINDOW;
    }
}

/* shell output -> CHANNEL_DATA, as far as the client's window allows */
static int pump_output(ssh_t* s) {
    int sent = 0;
    while (s->shell && !s->in_kex && s->peer_win && tty_output_pending(s->tty)) {
        uint32_t n = tty_output_pending(s->tty);
        uint32_t lim = s->peer_max - 64 < 16384 ? s->peer_max - 64 : 16384;
        if (n > lim) n = lim;
        if (n > s->peer_win) n = s->peer_win;
        s->pl[0] = MSG_CHANNEL_DATA;
        wr32(s->pl + 1, s->peer_chan);
        n = tty_output(s->tty, s->pl + 9, n);
        wr32(s->pl + 5, n);
        if (send_packet(s, s->pl, 9 + n) != 0) return sent;
        s->peer_win -= n;
        sent = 1;
    }
    return sent;
}

static void close_channel(ssh_t* s) {
    if (s->closing) return;
    wbuf_t w = { s->pl, 0, BUF_SIZE };
    if (s->shell) {
        w_u8(&w, MSG_CHANNEL_REQUEST);
        w_u32(&w, s->peer_chan);
        w_cstr(&w, "exit-status");
        w_u8(&w, 0);
        w_u32(&w, 0);
        send_wbuf(s, &w);
        w.len = 0;
        w_u8(&w, MSG_CHANNEL_EOF);
        w_u32(&w, s->peer_chan);
        send_wbuf(s, &w);
    }
    w.len = 0;
    w_u8(&w, MSG_CHANNEL_CLOSE);
    w_u32(&w, s->peer_chan);
    send_wbuf(s, &w);
    s->closing = 1;
    s->close_deadline = timer_ms() + 3000;
}

/* ── message dispatch ─────────────────────────────────────────────── */

static void handle(ssh_t* s, const uint8_t* p, uint32_t n) {
    uint8_t t = p[0];
    if (s->ignore_next) { s->ignore_next = 0; return; }

    /* strict KEX: nothing but key exchange before the first NEWKEYS */
    if (!s->kex_done_once && t != MSG_KEXINIT && t != MSG_KEX_ECDH_INIT && t != MSG_NEWKEYS) {
        if (s->strict || (t != MSG_IGNORE && t != MSG_DEBUG))
            disconnect(s, DISC_PROTOCOL_ERROR, "unexpected message during key exchange");
        return;
    }
    switch (t) {
    case MSG_KEXINIT:
        if (on_kexinit(s, p, n) != 0 && !s->dead) disconnect(s, DISC_PROTOCOL_ERROR, "bad KEXINIT");
        return;
    case MSG_KEX_ECDH_INIT:
        if (!s->in_kex || on_ecdh_init(s, p, n) != 0)
            disconnect(s, DISC_KEY_EXCHANGE_FAILED, "key exchange failed");
        return;
    case MSG_NEWKEYS:
        if (!s->in_kex) { disconnect(s, DISC_PROTOCOL_ERROR, "unexpected NEWKEYS"); return; }
        on_newkeys(s);
        return;
    case MSG_DISCONNECT:
        s->dead = 1;
        return;
    case MSG_IGNORE: case MSG_DEBUG: case MSG_UNIMPLEMENTED:
        return;
    case MSG_SERVICE_REQUEST: {
        rbuf_t r = { p, n, 1, 0 };
        uint32_t l;
        const uint8_t* name = r_str(&r, &l);
        if (r.err || !str_eq(name, l, "ssh-userauth")) {
            disconnect(s, DISC_SERVICE_NOT_AVAIL, "unknown service");
            return;
        }
        wbuf_t w = { s->pl, 0, BUF_SIZE };
        w_u8(&w, MSG_SERVICE_ACCEPT);
        w_cstr(&w, "ssh-userauth");
        send_wbuf(s, &w);
        return;
    }
    case MSG_USERAUTH_REQUEST:
        on_userauth(s, p, n);
        return;
    default:
        break;
    }

    if (!s->authed) { disconnect(s, DISC_PROTOCOL_ERROR, "not authenticated"); return; }
    switch (t) {
    case MSG_GLOBAL_REQUEST: {                     /* keepalives etc.: politely refused */
        rbuf_t r = { p, n, 1, 0 };
        uint32_t l;
        r_str(&r, &l);
        if (r_u8(&r)) { uint8_t m = MSG_REQUEST_FAILURE; send_packet(s, &m, 1); }
        return;
    }
    case MSG_CHANNEL_OPEN:          on_channel_open(s, p, n); return;
    case MSG_CHANNEL_REQUEST:       on_channel_request(s, p, n); return;
    case MSG_CHANNEL_DATA:          on_channel_data(s, p, n); return;
    case MSG_CHANNEL_EXTENDED_DATA: return;
    case MSG_CHANNEL_WINDOW_ADJUST: {
        rbuf_t r = { p, n, 1, 0 };
        r_u32(&r);
        uint32_t add = r_u32(&r);
        if (!r.err) s->peer_win = (s->peer_win + add < s->peer_win) ? 0xFFFFFFFFu : s->peer_win + add;
        return;
    }
    case MSG_CHANNEL_EOF:
        return;
    case MSG_CHANNEL_CLOSE:
        s->close_rcvd = 1;
        return;
    case MSG_CHANNEL_SUCCESS: case MSG_CHANNEL_FAILURE:
        return;
    default: {
        uint8_t m[5];
        m[0] = MSG_UNIMPLEMENTED;
        wr32(m + 1, s->seq_in - 1);
        send_packet(s, m, 5);
        return;
    }
    }
}

/* ── one connection ───────────────────────────────────────────────── */

/* 1 once the client's "SSH-2.0-..." line is in, 0 for more data, -1 bad */
static int read_version(ssh_t* s) {
    for (;;) {
        uint8_t* nl = NULL;
        for (uint32_t i = 0; i < s->rx_len; i++) if (s->rx[i] == '\n') { nl = s->rx + i; break; }
        if (!nl) return s->rx_len > 1024 ? -1 : 0;
        uint32_t len = (uint32_t)(nl - s->rx);
        uint32_t l = len;
        if (l && s->rx[l - 1] == '\r') l--;
        int is_ver = l >= 4 && memcmp(s->rx, "SSH-", 4) == 0;
        if (is_ver) {
            if (l >= sizeof(s->v_c)) return -1;
            memcpy(s->v_c, s->rx, l);
            s->v_c[l] = '\0';
        }
        memmove(s->rx, nl + 1, s->rx_len - len - 1);
        s->rx_len -= len + 1;
        if (is_ver) {
            if (strncmp(s->v_c, "SSH-2.0-", 8) != 0 && strncmp(s->v_c, "SSH-1.99-", 9) != 0) return -1;
            s->got_version = 1;
            return 1;
        }
    }
}

static void run_session(int slot, tcp_conn_t* c) {
    ssh_t* s = (ssh_t*)kzalloc(sizeof(ssh_t));
    uint8_t* bufs = (uint8_t*)kmalloc(4 * BUF_SIZE);
    if (!s || !bufs) { kfree(s); kfree(bufs); tcp_abort(c); return; }
    s->c = c;
    s->slot = slot;
    s->tty = g_ttys[slot];
    s->rx = bufs;
    s->pkt = bufs + BUF_SIZE;
    s->tx = bufs + 2 * BUF_SIZE;
    s->pl = bufs + 3 * BUF_SIZE;
    s->start_ms = timer_ms();
    ip4_t pip;
    tcp_peer(c, &pip, NULL);
    ip4_to_str(pip, s->peer);
    kstrlcpy(g_info[slot].peer, s->peer, sizeof(g_info[slot].peer));
    g_info[slot].user[0] = '\0';
    g_info[slot].since = timer_ms();
    klog("sshd: connection from %s\n", s->peer);

    static const char banner[] = SERVER_VERSION "\r\n";
    if (tcp_send(c, banner, sizeof(banner) - 1, 10000) != (int)(sizeof(banner) - 1)) s->dead = 1;
    if (!s->dead) send_kexinit(s);

    while (!s->dead) {
        int busy = 0;
        /* 1. whatever the network has for us */
        if (s->rx_len < BUF_SIZE) {
            int r = tcp_recv(c, s->rx + s->rx_len, BUF_SIZE - s->rx_len, 0);
            if (r > 0) { s->rx_len += (uint32_t)r; busy = 1; }
            else if (r != NET_ERR_TIMEOUT) break;          /* closed or reset */
        }
        if (!s->got_version) {
            int v = read_version(s);
            if (v < 0) break;
            if (v == 0) { if (!busy) net_wait(10); continue; }
        }
        /* 2. complete packets */
        for (;;) {
            int n = next_packet(s);
            if (n == 0) break;
            if (n < 0) { disconnect(s, DISC_MAC_ERROR, "corrupt packet"); break; }
            handle(s, s->pkt, (uint32_t)n);
            busy = 1;
            if (s->dead) break;
        }
        if (s->dead) break;
        if (!s->authed && timer_ms() - s->start_ms > LOGIN_GRACE_MS) {
            disconnect(s, DISC_BY_APPLICATION, "login timeout");
            break;
        }
        if (s->authed && !g_info[slot].user[0]) kstrlcpy(g_info[slot].user, s->user, sizeof(g_info[slot].user));

        /* 3. shell output, and the end of the session */
        if (pump_output(s)) busy = 1;
        if (s->close_rcvd && s->shell && !tty_finished(s->tty)) tty_hangup(s->tty);
        if (s->chan_open && !s->closing &&
            ((s->shell && tty_finished(s->tty) && !tty_output_pending(s->tty)) || s->close_rcvd))
            close_channel(s);
        if (s->closing && (s->close_rcvd || (int32_t)(timer_ms() - s->close_deadline) > 0)) break;
        if (!busy) net_wait(10);
    }

    /* the client is gone: the shell ends its session before the tty is reused */
    if (s->shell) {
        tty_hangup(s->tty);
        while (!tty_finished(s->tty)) task_sleep_ms(50);
        tty_detach(s->tty);
    }
    klog("sshd: %s: session closed\n", s->peer);
    tcp_close(c);
    kfree(s->i_c);
    kfree(s->i_s);
    memset(s, 0, sizeof(*s));                      /* keys */
    kfree(s);
    kfree(bufs);
    g_info[slot].active = 0;
    g_info[slot].peer[0] = '\0';
}

/* ── tasks and control ────────────────────────────────────────────── */

static void session_loop(int slot) {
    task_set_background();
    for (;;) {
        if (!g_running || !g_listener) { task_sleep_ms(200); continue; }
        tcp_conn_t* c = tcp_accept(g_listener, 300);
        if (c) run_session(slot, c);
    }
}
static void session_task_0(void) { session_loop(0); }
static void session_task_1(void) { session_loop(1); }
static void shell_task_0(void) { shell_run_remote(g_ttys[0]); }
static void shell_task_1(void) { shell_run_remote(g_ttys[1]); }

int sshd_start(uint16_t port, char* err, int errcap) {
    if (!passwd_is_set(PASSWD_USER)) {
        ksnprintf(err, (size_t)errcap, "set a password first with `passwd` (it is the SSH login)");
        return -1;
    }
    if (load_hostkey() != 0) {
        ksnprintf(err, (size_t)errcap, "cannot write the host key (%s)", HOSTKEY_FILE);
        return -1;
    }
    if (g_running) {
        if (port == g_port) return 0;
        sshd_stop();
    }
    int e;
    tcp_listener_t* l = tcp_listen(port, &e);
    if (!l) {
        ksnprintf(err, (size_t)errcap, e == NET_ERR_PROTO ? "port %u is already in use" : "no free listener slot", port);
        return -1;
    }
    if (!g_tasks_started) {
        static void (*const sess[TTY_MAX])(void) = { session_task_0, session_task_1 };
        static void (*const sh[TTY_MAX])(void) = { shell_task_0, shell_task_1 };
        for (int i = 0; i < TTY_MAX; i++) {
            g_ttys[i] = tty_create();
            if (g_ttys[i] < 0 || task_create("ssh-sh", sh[i]) < 0 || task_create("sshd", sess[i]) < 0) {
                tcp_unlisten(l);
                ksnprintf(err, (size_t)errcap, "no free terminal or task slot");
                return -1;
            }
        }
        g_tasks_started = 1;
    }
    g_listener = l;
    g_port = port;
    g_running = 1;
    return 0;
}

void sshd_stop(void) {
    g_running = 0;
    if (g_listener) tcp_unlisten(g_listener);
    g_listener = NULL;
}

int sshd_running(void) { return g_running; }
uint16_t sshd_port(void) { return g_port; }

void sshd_print_status(void) {
    char line[128], fp[64];
    if (!g_running) {
        terminal_writeln("sshd: stopped");
    } else {
        char ip[16];
        ip4_to_str(net_if()->ip, ip);
        if (g_port == 22) ksnprintf(line, sizeof(line), "sshd: running on port 22 - ssh %s@%s", PASSWD_USER, ip);
        else ksnprintf(line, sizeof(line), "sshd: running on port %u - ssh -p %u %s@%s", g_port, g_port, PASSWD_USER, ip);
        terminal_writeln(line);
    }
    if (g_have_key && sshd_fingerprint(fp, sizeof(fp)) == 0) {
        ksnprintf(line, sizeof(line), "      host key: ED25519 %s", fp);
        terminal_writeln(line);
    }
    for (int i = 0; i < TTY_MAX; i++) {
        if (!g_info[i].peer[0]) continue;
        uint32_t secs = (timer_ms() - g_info[i].since) / 1000u;
        ksnprintf(line, sizeof(line), "      session: %s from %s, %um%02us",
                  g_info[i].user[0] ? g_info[i].user : "(logging in)", g_info[i].peer, secs / 60u, secs % 60u);
        terminal_writeln(line);
    }
}
