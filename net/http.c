#include "http.h"
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
        char dir[1024];
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

static int do_request(const url_t* u, const http_request_t* req, http_response_t* resp,
                      int* redirect, char* errmsg, uint32_t errlen) {
    const char* method = req->method ? req->method : "GET";
    int is_head = strcmp(method, "HEAD") == 0;
    uint32_t timeout = req->timeout_ms ? req->timeout_ms : 20000;

    ip4_t ip;
    info(req, "Resolving %s...", u->host);
    int rc = dns_resolve(u->host, &ip, timeout);
    if (rc != NET_OK) {
        ksnprintf(errmsg, errlen, "could not resolve host %s (%s)", u->host, net_strerror(rc));
        return rc;
    }
    char ips[16];
    ip4_to_str(ip, ips);
    info(req, "Connecting to %s (%s):%u...", u->host, ips, u->port);

    stream_t* s = (stream_t*)kzalloc(sizeof(stream_t));
    if (!s) return NET_ERR_NOMEM;
    s->timeout_ms = timeout;
    s->tcp = tcp_connect(ip, u->port, timeout, &rc);
    if (!s->tcp) {
        ksnprintf(errmsg, errlen, "failed to connect to %s port %u: %s", u->host, u->port, net_strerror(rc));
        kfree(s);
        return rc;
    }
    info(req, "Connected to %s (%s) port %u", u->host, ips, u->port);

    if (u->https) {
        char tmsg[128] = "";
        s->tls = tls_connect(s->tcp, u->host, req->tls_ciphers, timeout, &rc, tmsg, sizeof(tmsg));
        if (!s->tls) {
            ksnprintf(errmsg, errlen, "TLS handshake with %s failed: %s", u->host, tmsg);
            tcp_abort(s->tcp);
            kfree(s);
            return rc ? rc : NET_ERR_PROTO;
        }
        kstrlcpy(resp->tls_cipher, tls_cipher_name(s->tls), sizeof(resp->tls_cipher));
        info(req, "TLS 1.3 connection using %s (certificate NOT verified)", resp->tls_cipher);
    }

    /* request */
    char host_hdr[300];
    int default_port = (u->https && u->port == 443) || (!u->https && u->port == 80);
    if (default_port) kstrlcpy(host_hdr, u->host, sizeof(host_hdr));
    else ksnprintf(host_hdr, sizeof(host_hdr), "%s:%u", u->host, u->port);
    char* rq = (char*)kmalloc(2048);
    if (!rq) { s_close(s); kfree(s); return NET_ERR_NOMEM; }
    int rqlen = ksnprintf(rq, 2048,
        "%s %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: %s\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n"
        "\r\n",
        method, u->path, host_hdr, req->user_agent ? req->user_agent : "BananaOS/0.5");
    if (rqlen >= 2048) rqlen = 2047;
    if (req->on_request) req->on_request(req->ctx, rq);
    rc = s_write(s, rq, (uint32_t)rqlen);
    kfree(rq);
    if (rc < 0) {
        ksnprintf(errmsg, errlen, "failed to send request: %s", net_strerror(rc));
        s_close(s); kfree(s);
        return rc;
    }

    /* status line + headers (skipping any 1xx interim responses) */
    char* raw = (char*)kmalloc(16384);
    char* line = (char*)kmalloc(4096);
    if (!raw || !line) { kfree(raw); kfree(line); s_close(s); kfree(s); return NET_ERR_NOMEM; }
    int chunked = 0;
    for (;;) {
        uint32_t raw_len = 0;
        raw[0] = '\0';
        rc = s_readline(s, line, 4096);
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
        (void)major; (void)minor;
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
            rc = s_readline(s, line, 4096);
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
    char cur[1024];
    kstrlcpy(cur, url, sizeof(cur));
    int max = req->max_redirects ? req->max_redirects : 10;

    for (int hop = 0; hop <= max; hop++) {
        url_t u;
        if (!url_parse(cur, &u)) {
            ksnprintf(errmsg, errmsg_len, "unsupported or malformed URL: %s", cur);
            return NET_ERR_PROTO;
        }
        kstrlcpy(resp->final_url, cur, sizeof(resp->final_url));
        int redirect = 0;
        int rc = do_request(&u, req, resp, &redirect, errmsg, errmsg_len);
        if (rc != NET_OK || !redirect) return rc;
        char next[1024];
        resolve_location(&u, resp->location, next, sizeof(next));
        info(req, "Redirected (%d) to %s", resp->status, next);
        kstrlcpy(cur, next, sizeof(cur));
        resp->body_bytes = 0;
    }
    ksnprintf(errmsg, errmsg_len, "too many redirects");
    return NET_ERR_PROTO;
}
