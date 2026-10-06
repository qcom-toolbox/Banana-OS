#ifndef HTTP_H
#define HTTP_H

#include "net.h"

/* longest URL handled: anti-bot challenges put kilobytes of token in the query */
#define HTTP_URL_MAX 8192

/* HTTP/1.1 client (GET/HEAD) over TCP, or TLS for https:// URLs, used by
 * curl and wget. Handles Content-Length, chunked transfer encoding and
 * read-until-close bodies, and (optionally) follows redirects. */

typedef struct {
    int      https;
    char     host[256];
    uint16_t port;
    char     path[HTTP_URL_MAX]; /* includes the query string, always starts with '/' */
} url_t;

/* Parses http://host[:port]/path and https://...; a missing scheme means
 * http. Returns 1 on success. */
int url_parse(const char* s, url_t* out);

typedef struct {
    int      status;           /* e.g. 200 */
    char     reason[64];       /* e.g. "OK" */
    char     content_type[96];
    int32_t  content_length;   /* -1 when not given */
    char     location[HTTP_URL_MAX]; /* redirect target, if any */
    uint32_t body_bytes;       /* body bytes delivered */
    char     final_url[HTTP_URL_MAX]; /* URL of the last request (after redirects) */
    char     tls_cipher[48];   /* "" for plain http */
} http_response_t;

typedef struct {
    const char* method;        /* "GET" (default) or "HEAD" */
    int      follow_redirects;
    int      max_redirects;    /* default 10 */
    uint32_t timeout_ms;       /* per connect/read wait; default 20 s */
    const char* user_agent;
    /* optional request body (POST): sent with Content-Type and Content-Length */
    const char* body;
    uint32_t body_len;
    const char* content_type;
    /* optional extra header lines, each ending in "
" (cookies, SOAPAction, ...) */
    const char* extra_headers;
    /* optional: the extra header lines for each request of a redirect
     * chain (cookies differ per host, and a redirect may have just set
     * one); replaces extra_headers when given. The string must stay valid
     * until the next call. */
    const char* (*headers_for)(void* ctx, const char* url);
    int      tls_ciphers;      /* TLS_CIPHERS_* (tls.h), 0 = offer all */

    /* all optional */
    void* ctx;
    /* a response's status line + headers arrived (raw: the header block text) */
    void (*on_headers)(void* ctx, const http_response_t* r, const char* raw);
    /* body bytes; return <0 to abort the transfer */
    int  (*on_body)(void* ctx, const uint8_t* data, uint32_t len);
    /* progress: bytes so far, total (-1 unknown) */
    void (*on_progress)(void* ctx, uint32_t got, int32_t total);
    /* informational messages ("Resolving...", "Connected", redirects) */
    void (*on_info)(void* ctx, const char* msg);
    /* the exact request text (for curl -v) */
    void (*on_request)(void* ctx, const char* raw);
} http_request_t;

/* Returns NET_OK when a complete response was received (whatever its
 * status code), or a NET_ERR_* code. errmsg gets a readable reason. */
int http_fetch(const char* url, const http_request_t* req, http_response_t* resp,
               char* errmsg, uint32_t errmsg_len);

#endif
