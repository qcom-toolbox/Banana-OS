#include "http.h"
#include "../kernel/timer.h"
#include "tcp.h"
#include "tls.h"
#include "kstring.h"
#include "kheap.h"

/* ── URLs ───────────────────────────────────────────────────────── */

int url_parse(const char* s, url_t* out) {
    memset(out, 0, sizeof(*out));
    if (strncasecmp(s, "https://", 8) == 0) { out->https = 1; s += 8; }
    else if (strncasecmp(s, "http://", 7) == 0) { s += 7; }
    else if (strstr(s, "://")) return 0;             /* ftp:// etc. */
    out->port = out->https ? 443 : 80;

    /* host[:port] up to the first '/', '?' or '#' */
    uint32_t i = 0;
    while (s[i] && s[i] != '/' && s[i] != ':' && s[i] != '?' && s[i] != '#') {
        if (i + 1 >= sizeof(out->host)) return 0;
        out->host[i] = s[i];
        i++;
    }
    out->host[i] = '\0';
    if (!out->host[0]) return 0;
    s += i;
    if (*s == ':') {
        uint32_t port;
        int n = k_parse_u32(s + 1, &port);
        if (n == 0 || port == 0 || port > 65535) return 0;
        out->port = (uint16_t)port;
        s += 1 + n;
    }
    /* path: keep the query, drop any #fragment (never sent to servers) */
    uint32_t o = 0;
    if (*s != '/') out->path[o++] = '/';
    for (; *s && *s != '#'; s++) {
        if (o + 4 >= sizeof(out->path)) return 0;
        if (*s == ' ') {                    /* be forgiving about spaces */
            out->path[o++] = '%'; out->path[o++] = '2'; out->path[o++] = '0';
        } else {
            out->path[o++] = *s;
        }
    }
    out->path[o] = '\0';
    return 1;
}

/* Resolves a Location header against the URL it came from. */
static void resolve_location(const url_t* base, const char* loc, char* out, uint32_t cap) {
    if (strncasecmp(loc, "http://", 7) == 0 || strncasecmp(loc, "https://", 8) == 0) {
        kstrlcpy(out, loc, cap);
        return;
    }
    char origin[300];
    int default_port = (base->https && base->port == 443) || (!base->https && base->port == 80);
    if (default_port) ksnprintf(origin, sizeof(origin), "%s://%s", base->https ? "https" : "http", base->host);
    else ksnprintf(origin, sizeof(origin), "%s://%s:%u", base->https ? "https" : "http", base->host, base->port);

    if (loc[0] == '/' && loc[1] == '/') {            /* scheme-relative */
        ksnprintf(out, cap, "%s:%s", base->https ? "https" : "http", loc);
    } else if (loc[0] == '/') {                      /* absolute path */
        ksnprintf(out, cap, "%s%s", origin, loc);
    } else {                                         /* relative to the current directory */
        static char dir[HTTP_URL_MAX];               /* no yield here: one is enough (small task stacks) */
        kstrlcpy(dir, base->path, sizeof(dir));
        char* q = strchr(dir, '?');
        if (q) *q = '\0';
        char* slash = strrchr(dir, '/');
        if (slash) slash[1] = '\0';
        ksnprintf(out, cap, "%s%s%s", origin, dir, loc);
    }
}

/* ── stream: plain TCP or TLS ───────────────────────────────────── */

typedef struct {
    tcp_conn_t* tcp;
    tls_conn_t* tls;
    uint32_t    timeout_ms;
    /* read buffer, for line-oriented header/chunk parsing */
    uint8_t     buf[4096];
    uint32_t    pos, len;
    int         eof;
} stream_t;

static int s_write(stream_t* s, const void* data, uint32_t len) {
    return s->tls ? tls_write(s->tls, data, len, s->timeout_ms)
                  : tcp_send(s->tcp, data, len, s->timeout_ms);
}

/* refills the buffer; returns bytes read, 0 at EOF, <0 error */
static int s_fill(stream_t* s) {
    if (s->eof) return 0;
    int n = s->tls ? tls_read(s->tls, s->buf, sizeof(s->buf), s->timeout_ms)
                   : tcp_recv(s->tcp, s->buf, sizeof(s->buf), s->timeout_ms);
    if (n == 0) s->eof = 1;
    if (n < 0) return n;
    s->pos = 0;
    s->len = (uint32_t)n;
    return n;
}

/* reads one CRLF (or LF) terminated line without the terminator.
 * Returns length, or <0 on error / EOF before any byte. */
static int s_readline(stream_t* s, char* out, uint32_t cap) {
    uint32_t o = 0;
    for (;;) {
        if (s->pos >= s->len) {
            int r = s_fill(s);
            if (r < 0) return r;
            if (r == 0) return o ? (int)o : NET_ERR_CLOSED;
        }
        char c = (char)s->buf[s->pos++];
        if (c == '\n') break;
        if (c == '\r') continue;
        if (o + 1 < cap) out[o++] = c;
    }
    out[o] = '\0';
    return (int)o;
}

static void s_close(stream_t* s) {
    if (s->tls) tls_close(s->tls);
    if (s->tcp) tcp_close(s->tcp);
    s->tls = NULL;
    s->tcp = NULL;
}

/* ── keep-alive: open connections waiting for the next request ──────
 * A page pulls dozens of style sheets, scripts and images from the same
 * few hosts; a new TCP connection + TLS handshake for each costs far more
 * than the request itself. Finished connections wait here (per host and
 * port) for a while. */

#define POOL_MAX     4
#define POOL_IDLE_MS 15000

static struct {
    int       used, https;
    char      host[256];
    uint16_t  port;
    stream_t* s;
    uint32_t  since;
} g_pool[POOL_MAX];

static void pool_drop(int i) {
    s_close(g_pool[i].s);
    kfree(g_pool[i].s);
    g_pool[i].used = 0;
}

static stream_t* pool_take(const url_t* u, uint32_t timeout) {
    for (int i = 0; i < POOL_MAX; i++) {
        if (!g_pool[i].used) continue;
        if ((uint32_t)(timer_ms() - g_pool[i].since) > POOL_IDLE_MS) { pool_drop(i); continue; }
        if (g_pool[i].https == u->https && g_pool[i].port == u->port && strcasecmp(g_pool[i].host, u->host) == 0) {
            stream_t* st = g_pool[i].s;
            g_pool[i].used = 0;
            st->timeout_ms = timeout;
            return st;
        }
    }
    return NULL;
}

static void pool_put(const url_t* u, stream_t* st) {
    int slot = -1;
    for (int i = 0; i < POOL_MAX; i++) if (!g_pool[i].used) { slot = i; break; }
    if (slot < 0) {                             /* full: the one idle longest goes */
        slot = 0;
        for (int i = 1; i < POOL_MAX; i++) if ((int32_t)(g_pool[i].since - g_pool[slot].since) < 0) slot = i;
        pool_drop(slot);
    }
    g_pool[slot].used = 1;
    g_pool[slot].https = u->https;
    g_pool[slot].port = u->port;
    kstrlcpy(g_pool[slot].host, u->host, sizeof(g_pool[slot].host));
    g_pool[slot].s = st;
    g_pool[slot].since = timer_ms();
}

/* ── body delivery ──────────────────────────────────────────────── */

typedef struct {
    const http_request_t* req;
    http_response_t*      resp;
    int                   aborted;
} body_ctx_t;

static int deliver(body_ctx_t* b, const uint8_t* data, uint32_t len) {
    if (!len) return 0;
    b->resp->body_bytes += len;
    if (b->req->on_body && b->req->on_body(b->req->ctx, data, len) < 0) {
        b->aborted = 1;
        return -1;
    }
    if (b->req->on_progress) b->req->on_progress(b->req->ctx, b->resp->body_bytes, b->resp->content_length);
    return 0;
}

/* copies up to `want` bytes (or until EOF if want < 0) to the sink */
static int read_body_bytes(stream_t* s, body_ctx_t* b, int64_t want) {
    while (want != 0) {
        if (s->pos >= s->len) {
            int r = s_fill(s);
            if (r < 0) return r;
            if (r == 0) return want < 0 ? NET_OK : NET_ERR_CLOSED;
        }
        uint32_t n = s->len - s->pos;
        if (want > 0 && (int64_t)n > want) n = (uint32_t)want;
        if (deliver(b, s->buf + s->pos, n) < 0) return NET_ERR_INTR;
        s->pos += n;
        if (want > 0) want -= n;
    }
    return NET_OK;
}

static int read_chunked(stream_t* s, body_ctx_t* b) {
    char line[128];
    for (;;) {
        int r = s_readline(s, line, sizeof(line));
        if (r < 0) return r;
        uint32_t size = 0;
        const char* p = line;
        int digits = 0;
        for (; *p; p++, digits++) {
            char c = *p;
            int v = (c >= '0' && c <= '9') ? c - '0' :
                    (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
                    (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (v < 0) break;                       /* ";ext" or whitespace */
            if (size > 0x07FFFFFFu) return NET_ERR_PROTO;
            size = size * 16u + (uint32_t)v;
        }
        if (!digits) return NET_ERR_PROTO;
        if (size == 0) {
            /* trailers until the empty line */
            do { r = s_readline(s, line, sizeof(line)); } while (r > 0);
            return NET_OK;
        }
        r = read_body_bytes(s, b, size);
        if (r != NET_OK) return r;
        r = s_readline(s, line, sizeof(line));      /* CRLF after the chunk */
        if (r < 0) return r;
    }
}

/* ── one request ────────────────────────────────────────────────── */

static void info(const http_request_t* req, const char* fmt, ...) {
    if (!req->on_info) return;
    char msg[300];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvsnprintf(msg, sizeof(msg), fmt, ap);
    __builtin_va_end(ap);
    req->on_info(req->ctx, msg);
}

static int header_is(const char* line, const char* name, const char** value) {
    size_t n = strlen(name);
    if (strncasecmp(line, name, n) != 0 || line[n] != ':') return 0;
    const char* v = line + n + 1;
    while (*v == ' ' || *v == '\t') v++;
    *value = v;
    return 1;
}

/* a new connection (TCP, and TLS for https://) - NULL with *rc and errmsg */
static stream_t* open_stream(const url_t* u, const http_request_t* req, http_response_t* resp, uint32_t timeout,
                             char* errmsg, uint32_t errlen, int* rcp) {
    ip4_t ip;
    info(req, "Resolving %s...", u->host);
    int rc = dns_resolve(u->host, &ip, timeout);
    if (rc != NET_OK) {
        ksnprintf(errmsg, errlen, "could not resolve host %s (%s)", u->host, net_strerror(rc));
        *rcp = rc;
        return NULL;
    }
    char ips[16];
    ip4_to_str(ip, ips);
    info(req, "Connecting to %s (%s):%u...", u->host, ips, u->port);

    stream_t* s = (stream_t*)kzalloc(sizeof(stream_t));
    if (!s) { *rcp = NET_ERR_NOMEM; return NULL; }
    s->timeout_ms = timeout;
    s->tcp = tcp_connect(ip, u->port, timeout, &rc);
    if (!s->tcp) {
        ksnprintf(errmsg, errlen, "failed to connect to %s port %u: %s", u->host, u->port, net_strerror(rc));
        kfree(s);
        *rcp = rc;
        return NULL;
    }
    info(req, "Connected to %s (%s) port %u", u->host, ips, u->port);

    if (u->https) {
        char tmsg[128] = "";
        s->tls = tls_connect(s->tcp, u->host, req->tls_ciphers, timeout, &rc, tmsg, sizeof(tmsg));
        if (!s->tls) {
            ksnprintf(errmsg, errlen, "TLS handshake with %s failed: %s", u->host, tmsg);
            tcp_abort(s->tcp);
            kfree(s);
            *rcp = rc ? rc : NET_ERR_PROTO;
            return NULL;
        }
        kstrlcpy(resp->tls_cipher, tls_cipher_name(s->tls), sizeof(resp->tls_cipher));
        info(req, "TLS 1.3 connection using %s (certificate NOT verified)", resp->tls_cipher);
    }
    *rcp = NET_OK;
    return s;
}

static int do_request(const url_t* u, const http_request_t* req, http_response_t* resp,
                      int* redirect, char* errmsg, uint32_t errlen) {
    const char* method = req->method ? req->method : "GET";
    int is_head = strcmp(method, "HEAD") == 0;
    uint32_t timeout = req->timeout_ms ? req->timeout_ms : 20000;
    int rc;
    stream_t* s = pool_take(u, timeout);
    int reused = s != NULL;
    if (reused) {
        info(req, "Re-using the connection to %s", u->host);
        if (u->https && s->tls) kstrlcpy(resp->tls_cipher, tls_cipher_name(s->tls), sizeof(resp->tls_cipher));
    } else {
        s = open_stream(u, req, resp, timeout, errmsg, errlen, &rc);
        if (!s) return rc;
    }

    /* request */
    char host_hdr[300];
    int default_port = (u->https && u->port == 443) || (!u->https && u->port == 80);
    if (default_port) kstrlcpy(host_hdr, u->host, sizeof(host_hdr));
    else ksnprintf(host_hdr, sizeof(host_hdr), "%s:%u", u->host, u->port);
    uint32_t rqcap = 2048 + (uint32_t)strlen(u->path) + (req->extra_headers ? (uint32_t)strlen(req->extra_headers) : 0);
    char* rq = (char*)kmalloc(rqcap);
    if (!rq) { s_close(s); kfree(s); return NET_ERR_NOMEM; }
    char body_hdr[160] = "";
    if (req->body)
        ksnprintf(body_hdr, sizeof(body_hdr), "Content-Type: %s\r\nContent-Length: %u\r\n",
                  req->content_type ? req->content_type : "application/x-www-form-urlencoded", req->body_len);
    int rqlen = ksnprintf(rq, rqcap,
        "%s %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: %s\r\n"
        "Accept: */*\r\n"
        "%s%s"
        "Connection: keep-alive\r\n"
        "\r\n",
        method, u->path, host_hdr, req->user_agent ? req->user_agent : "BananaOS/0.5",
        body_hdr, req->extra_headers ? req->extra_headers : "");
    if (rqlen >= (int)rqcap) rqlen = (int)rqcap - 1;
    if (req->on_request) req->on_request(req->ctx, rq);
    rc = s_write(s, rq, (uint32_t)rqlen);
    if (rc >= 0 && req->body && req->body_len) rc = s_write(s, req->body, req->body_len);
    if (rc >= 0 && reused && s->pos >= s->len) {
        /* a pooled connection the server may have closed while it waited:
         * the first byte of the answer tells */
        int r = s_fill(s);
        if (r <= 0) rc = NET_ERR_CLOSED;
    }
    if (rc < 0 && reused) {
        /* it had been closed: once more, on a new connection */
        s_close(s);
        kfree(s);
        s = open_stream(u, req, resp, timeout, errmsg, errlen, &rc);
        if (!s) { kfree(rq); return rc; }
        reused = 0;
        rc = s_write(s, rq, (uint32_t)rqlen);
        if (rc >= 0 && req->body && req->body_len) rc = s_write(s, req->body, req->body_len);
    }
    kfree(rq);
    if (rc < 0) {
        ksnprintf(errmsg, errlen, "failed to send request: %s", net_strerror(rc));
        s_close(s); kfree(s);
        return rc;
    }

    /* status line + headers (skipping any 1xx interim responses) */
    char* raw = (char*)kmalloc(16384);
    char* line = (char*)kmalloc(HTTP_URL_MAX + 256);  /* a Location: line can be a whole long URL */
    if (!raw || !line) { kfree(raw); kfree(line); s_close(s); kfree(s); return NET_ERR_NOMEM; }
    int chunked = 0;
    int conn_close = 0;                         /* the server ends the connection after this answer */
    for (;;) {
        uint32_t raw_len = 0;
        raw[0] = '\0';
        rc = s_readline(s, line, HTTP_URL_MAX + 256);
        if (rc < 0) {
            ksnprintf(errmsg, errlen, "no response from server (%s)", net_strerror(rc));
            goto fail;
        }
        int major = 0, minor = 0;
        uint32_t status = 0;
        if (strncmp(line, "HTTP/", 5) != 0 || !k_isdigit((unsigned char)line[5])) {
            ksnprintf(errmsg, errlen, "not an HTTP response");
            rc = NET_ERR_PROTO;
            goto fail;
        }
        major = line[5] - '0';
        minor = (line[6] == '.' && k_isdigit((unsigned char)line[7])) ? line[7] - '0' : 0;
        conn_close = major < 1 || (major == 1 && minor == 0);   /* HTTP/1.0: no keep-alive by default */
        const char* sp = strchr(line, ' ');
        if (!sp || k_parse_u32(sp + 1, &status) != 3) {
            ksnprintf(errmsg, errlen, "malformed status line");
            rc = NET_ERR_PROTO;
            goto fail;
        }
        resp->status = (int)status;
        const char* reason = sp + 4;
        while (*reason == ' ') reason++;
        kstrlcpy(resp->reason, reason, sizeof(resp->reason));
        resp->content_length = -1;
        resp->content_type[0] = '\0';
        resp->location[0] = '\0';
        raw_len = (uint32_t)ksnprintf(raw, 16384, "%s\n", line);
        chunked = 0;

        for (;;) {
            rc = s_readline(s, line, HTTP_URL_MAX + 256);
            if (rc < 0) {
                ksnprintf(errmsg, errlen, "connection closed while reading headers");
                goto fail;
            }
            if (rc == 0) break;          /* blank line: end of headers */
            if (raw_len + (uint32_t)rc + 2 < 16384) {
                memcpy(raw + raw_len, line, (uint32_t)rc);
                raw_len += (uint32_t)rc;
                raw[raw_len++] = '\n';
                raw[raw_len] = '\0';
            }
            const char* v;
            if (header_is(line, "Content-Length", &v)) {
                uint32_t n;
                if (k_parse_u32(v, &n)) resp->content_length = (int32_t)n;
            } else if (header_is(line, "Transfer-Encoding", &v)) {
                if (strstr(v, "chunked") || strstr(v, "Chunked")) chunked = 1;
            } else if (header_is(line, "Location", &v)) {
                kstrlcpy(resp->location, v, sizeof(resp->location));
            } else if (header_is(line, "Connection", &v)) {
                if (strstr(v, "close") || strstr(v, "Close")) conn_close = 1;
                else if (strstr(v, "keep-alive") || strstr(v, "Keep-Alive")) conn_close = 0;
            } else if (header_is(line, "Content-Type", &v)) {
                kstrlcpy(resp->content_type, v, sizeof(resp->content_type));
            }
        }
        if (status >= 100 && status < 200) continue;   /* 100 Continue & co */
        break;
    }
    if (req->on_headers) req->on_headers(req->ctx, resp, raw);

    int is_redirect = (resp->status == 301 || resp->status == 302 || resp->status == 303 ||
                       resp->status == 307 || resp->status == 308) && resp->location[0];
    if (is_redirect && req->follow_redirects) {
        *redirect = 1;
        rc = NET_OK;
        goto done;
    }

    body_ctx_t b = { req, resp, 0 };
    if (is_head || resp->status == 204 || resp->status == 304) rc = NET_OK;
    else if (chunked) rc = read_chunked(s, &b);
    else if (resp->content_length >= 0) rc = read_body_bytes(s, &b, resp->content_length);
    else rc = read_body_bytes(s, &b, -1);
    if (rc != NET_OK) {
        if (b.aborted) ksnprintf(errmsg, errlen, "transfer aborted");
        else if (rc == NET_ERR_INTR) ksnprintf(errmsg, errlen, "interrupted");
        else ksnprintf(errmsg, errlen, "transfer failed after %u bytes: %s",
                       resp->body_bytes, net_strerror(rc));
        goto fail;
    }
done:
    kfree(raw);
    kfree(line);
    /* a complete answer whose end we know: the connection can be used again */
    if (rc == NET_OK && !*redirect && !conn_close && !s->eof && s->pos >= s->len &&
        (chunked || resp->content_length >= 0 || is_head || resp->status == 204 || resp->status == 304)) {
        pool_put(u, s);
        return rc;
    }
    s_close(s);
    kfree(s);
    return rc;
fail:
    kfree(raw);
    kfree(line);
    if (s->tls) tls_close(s->tls);
    s->tls = NULL;
    tcp_abort(s->tcp);
    kfree(s);
    return rc;
}

int http_fetch(const char* url, const http_request_t* req, http_response_t* resp,
               char* errmsg, uint32_t errmsg_len) {
    memset(resp, 0, sizeof(*resp));
    errmsg[0] = '\0';
    /* URLs can be HTTP_URL_MAX long: off the (small) task stack */
    struct { url_t u; char cur[HTTP_URL_MAX]; } *w = kmalloc(sizeof(*w));
    if (!w) { ksnprintf(errmsg, errmsg_len, "out of memory"); return NET_ERR_NOMEM; }
    kstrlcpy(w->cur, url, sizeof(w->cur));
    int max = req->max_redirects ? req->max_redirects : 10;
    http_request_t cur_req = *req;          /* a 303 (or 301/302 after POST) turns into a GET */
    req = &cur_req;
    int rc = NET_ERR_PROTO;

    for (int hop = 0; hop <= max; hop++) {
        if (!url_parse(w->cur, &w->u)) {
            ksnprintf(errmsg, errmsg_len, "unsupported or malformed URL: %s", w->cur);
            rc = NET_ERR_PROTO;
            goto out;
        }
        kstrlcpy(resp->final_url, w->cur, sizeof(resp->final_url));
        /* cookies are per host - and the previous hop may have set one */
        if (cur_req.headers_for) cur_req.extra_headers = cur_req.headers_for(cur_req.ctx, w->cur);
        int redirect = 0;
        rc = do_request(&w->u, req, resp, &redirect, errmsg, errmsg_len);
        if (rc != NET_OK || !redirect) goto out;
        resolve_location(&w->u, resp->location, w->cur, sizeof(w->cur));
        info(req, "Redirected (%d) to %s", resp->status, w->cur);
        resp->body_bytes = 0;
        if (cur_req.body && (resp->status == 303 || resp->status == 301 || resp->status == 302)) {
            cur_req.method = "GET";
            cur_req.body = NULL;
            cur_req.body_len = 0;
        }
    }
    ksnprintf(errmsg, errmsg_len, "too many redirects");
    rc = NET_ERR_PROTO;
out:
    kfree(w);
    return rc;
}
