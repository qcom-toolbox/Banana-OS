#ifndef HTTP_H
#define HTTP_H

#include "net.h"

/* HTTP/1.1 client (GET/HEAD) over TCP, or TLS for https:// URLs, used by
 * curl and wget. Handles Content-Length, chunked transfer encoding and
 * read-until-close bodies, and (optionally) follows redirects. */

typedef struct {
    int      https;
    char     host[256];
    uint16_t port;
    char     path[1024];      /* includes the query string, always starts with '/' */
} url_t;

/* Parses http://host[:port]/path and https://...; a missing scheme means
 * http. Returns 1 on success. */
int url_parse(const char* s, url_t* out);

typedef struct {
    int      status;           /* e.g. 200 */
    char     reason[64];       /* e.g. "OK" */
    char     content_type[96];
    int32_t  content_length;   /* -1 when not given */
    char     location[1024];   /* redirect target, if any */
    uint32_t body_bytes;       /* body bytes delivered */
    char     final_url[1024];  /* URL of the last request (after redirects) */
    char     tls_cipher[48];   /* "" for plain http */
} http_response_t;

typedef struct {
    const char* method;        /* "GET" (default) or "HEAD" */
    int      follow_redirects;
    int      max_redirects;    /* default 10 */
    uint32_t timeout_ms;       /* per connect/read wait; default 20 s */
    const char* user_agent;
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
