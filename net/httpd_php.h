#ifndef HTTPD_PHP_H
#define HTTPD_PHP_H

#include "types.h"

/* one request for a .php page (net/httpd.c fills it in) */
typedef struct {
    const char* method;
    const char* uri;            /* as requested, with the query string */
    const char* path;           /* decoded path, no query */
    const char* query;          /* after '?', or "" */
    const char* docroot;
    const char* remote_addr;
    const char* host;           /* Host: header, or "" */
    const char* user_agent;     /* User-Agent: header, or "" */
} php_request_t;

typedef struct {
    int      status;            /* 200 unless header()/http_response_code() said otherwise */
    char     content_type[96];
    char     location[512];     /* header("Location: ...") */
    char*    body;              /* kmalloc'd output (caller kfree()s), may be NULL */
    uint32_t len, cap;
    int      overflow;          /* output was cut at 4 MiB */
} php_result_t;

/* runs a PHP file's code; 0 = ok, -1 = it failed (the error is in the body) */
int httpd_run_php(const char* fspath, const char* code, uint32_t code_len,
                  const php_request_t* rq, php_result_t* res);

#endif
