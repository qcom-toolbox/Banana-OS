#include "net.h"
#include "kstring.h"
#include "timer.h"
#include "random.h"

/* Stub DNS resolver (RFC 1035): recursive A-record queries over UDP to
 * the DHCP-provided server, following CNAMEs inside the answer, with a
 * small TTL-respecting cache. */

#define DNS_CACHE 16

typedef struct {
    char     name[64];
    ip4_t    ip;
    uint32_t expires_ms;
} dns_cache_t;

static dns_cache_t g_cache[DNS_CACHE];

/* the one outstanding query (resolution is synchronous per caller, and
 * each waiting task polls its own id) */
typedef struct {
    uint16_t id;
    int      done;
    int      rcode;
    ip4_t    ip;
    uint32_t ttl;
} dns_query_t;

static int cache_get(const char* name, ip4_t* out) {
    for (int i = 0; i < DNS_CACHE; i++) {
        if (g_cache[i].name[0] && strcasecmp(g_cache[i].name, name) == 0 &&
            (int32_t)(g_cache[i].expires_ms - timer_ms()) > 0) {
            *out = g_cache[i].ip;
            return 1;
        }
    }
    return 0;
}

static void cache_put(const char* name, ip4_t ip, uint32_t ttl_s) {
    if (strlen(name) >= sizeof(g_cache[0].name)) return;
    if (ttl_s > 3600) ttl_s = 3600;
    if (ttl_s < 5) ttl_s = 5;
    int slot = 0;
    for (int i = 0; i < DNS_CACHE; i++) {
        if (!g_cache[i].name[0] || strcasecmp(g_cache[i].name, name) == 0) { slot = i; break; }
        if ((int32_t)(g_cache[i].expires_ms - g_cache[slot].expires_ms) < 0) slot = i;
    }
    kstrlcpy(g_cache[slot].name, name, sizeof(g_cache[slot].name));
    g_cache[slot].ip = ip;
    g_cache[slot].expires_ms = timer_ms() + ttl_s * 1000u;
}

/* skips a (possibly compressed) name; returns new offset or 0 on error */
static uint32_t skip_name(const uint8_t* p, uint32_t len, uint32_t off) {
    while (off < len) {
        uint8_t l = p[off];
        if (l == 0) return off + 1;
        if ((l & 0xC0) == 0xC0) return (off + 2 <= len) ? off + 2 : 0;
        off += 1u + l;
    }
    return 0;
}

static void on_reply(ip4_t src, uint16_t sport, const uint8_t* p, uint32_t len, void* ctx) {
    (void)src; (void)sport;
    dns_query_t* q = (dns_query_t*)ctx;
    if (len < 12 || rd16(p) != q->id || !(p[2] & 0x80)) return;
    q->rcode = p[3] & 0x0F;
    uint16_t qd = rd16(p + 4), an = rd16(p + 6);
    uint32_t off = 12;
    for (uint16_t i = 0; i < qd; i++) {
        off = skip_name(p, len, off);
        if (!off || off + 4 > len) { q->done = 1; return; }
        off += 4;
    }
    /* CNAMEs are followed implicitly: a recursive server puts the final
     * A record in the same answer section, so the first A record wins */
    for (uint16_t i = 0; i < an; i++) {
        off = skip_name(p, len, off);
        if (!off || off + 10 > len) break;
        uint16_t type = rd16(p + off), cls = rd16(p + off + 2);
        uint32_t ttl = rd32(p + off + 4);
        uint16_t rdlen = rd16(p + off + 8);
        off += 10;
        if (off + rdlen > len) break;
        if (type == 1 && cls == 1 && rdlen == 4) {
            q->ip = rd32(p + off);
            q->ttl = ttl;
            break;
        }
        off += rdlen;
    }
    q->done = 1;
}

static int encode_name(const char* name, uint8_t* out, uint32_t cap) {
    uint32_t o = 0;
    const char* s = name;
    while (*s) {
        const char* dot = strchr(s, '.');
        uint32_t l = dot ? (uint32_t)(dot - s) : (uint32_t)strlen(s);
        if (l == 0 || l > 63 || o + l + 2 > cap) return -1;
        out[o++] = (uint8_t)l;
        memcpy(out + o, s, l);
        o += l;
        s += l;
        if (*s == '.') s++;
    }
    if (o + 1 > cap) return -1;
    out[o++] = 0;
    return (int)o;
}

int dns_resolve(const char* name, ip4_t* out, uint32_t timeout_ms) {
    if (str_to_ip4(name, out)) return NET_OK;
    if (strcasecmp(name, "localhost") == 0) { *out = IP4(127, 0, 0, 1); return NET_OK; }
    if (cache_get(name, out)) return NET_OK;

    netif_t* nif = net_if();
    if (!nif->dev) return NET_ERR_NODEV;
    if (!net_wait_configured(timeout_ms)) return NET_ERR_NOTREADY;
    if (!nif->dns) return NET_ERR_DNS;

    uint8_t pkt[300];
    memset(pkt, 0, 12);
    dns_query_t q;
    memset(&q, 0, sizeof(q));
    q.id = (uint16_t)random_u32();
    wr16(pkt, q.id);
    wr16(pkt + 2, 0x0100);      /* standard query, recursion desired */
    wr16(pkt + 4, 1);
    int nl = encode_name(name, pkt + 12, sizeof(pkt) - 16);
    if (nl < 0) return NET_ERR_DNS;
    uint32_t qlen = 12u + (uint32_t)nl;
    wr16(pkt + qlen, 1);        /* A */
    wr16(pkt + qlen + 2, 1);    /* IN */
    qlen += 4;

    uint16_t port = udp_ephemeral_port();
    if (udp_bind(port, on_reply, &q) != 0) return NET_ERR_NOMEM;

    int rc = NET_ERR_TIMEOUT;
    uint32_t start = timer_ms();
    for (int attempt = 0; attempt < 3 && !q.done; attempt++) {
        udp_send(nif->dns, port, 53, pkt, qlen);
        uint32_t sent = timer_ms();
        while (!q.done && timer_ms() - sent < 2000u && timer_ms() - start < timeout_ms) {
            if (net_interrupted()) { rc = NET_ERR_INTR; goto out; }
            net_wait(20);
        }
        if (timer_ms() - start >= timeout_ms) break;
    }
    if (q.done) {
        if (q.rcode == 0 && q.ip) {
            *out = q.ip;
            cache_put(name, q.ip, q.ttl);
            rc = NET_OK;
        } else {
            rc = NET_ERR_DNS;
        }
    }
out:
    udp_unbind(port);
    return rc;
}
