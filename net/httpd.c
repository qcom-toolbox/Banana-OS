#include "httpd.h"
#include "tcp.h"
#include "fs.h"
#include "kstring.h"
#include "kheap.h"
#include "task.h"
#include "timer.h"
#include "terminal.h"
#include "serial.h"
#include "httpd_php.h"
#include "httpd_demo.h"

#define REQ_MAX   4096
#define LOG_LINES 8

static tcp_listener_t* g_listener;
static uint16_t g_port;
static int      g_task_started;
static volatile int g_running;
static uint32_t g_requests;
static char     g_log[LOG_LINES][96];
static int      g_log_next;

static const char DEFAULT_INDEX[] =
    "<!DOCTYPE html>\n"
    "<html><head><meta charset=\"utf-8\"><title>Banana OS</title>\n"
    "<style>body{font-family:sans-serif;max-width:40em;margin:3em auto;padding:0 1em;"
    "background:#1d232c;color:#e8eef6}code{background:#2a323e;padding:.1em .3em}"
    "a{color:#f4d35e}</style></head>\n"
    "<body><h1>&#127820; Banana OS web server</h1>\n"
    "<p>It works! This page is <code>/var/www/index.html</code> on the Banana OS machine.</p>\n"
    "<p>Put your own files in <code>/var/www</code> - for example "
    "<code>echo \"hello\" &gt; /var/www/hello.txt</code> or "
    "<code>wget -O /var/www/pic.jpg https://...</code> - and they are served here. "
    "Folders without an <code>index.html</code> get a file listing.</p>\n"
    "</body></html>\n";

/* ── helpers ──────────────────────────────────────────────────────── */

static void log_request(const char* line) {
    kstrlcpy(g_log[g_log_next], line, sizeof(g_log[0]));
    g_log_next = (g_log_next + 1) % LOG_LINES;
    klog("httpd: %s\n", line);
}

static const char* content_type(const char* name, int idx) {
    const char* dot = strrchr(name, '.');
    if (dot) {
        static const char* map[][2] = {
            { ".html", "text/html; charset=utf-8" }, { ".htm", "text/html; charset=utf-8" },
            { ".css", "text/css" }, { ".js", "text/javascript" }, { ".json", "application/json" },
            { ".png", "image/png" }, { ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" },
            { ".gif", "image/gif" }, { ".bmp", "image/bmp" }, { ".svg", "image/svg+xml" },
            { ".ico", "image/x-icon" }, { ".pdf", "application/pdf" }, { ".zip", "application/zip" },
            { ".iso", "application/octet-stream" },
        };
        for (uint32_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
            if (strcasecmp(dot, map[i][0]) == 0) return map[i][1];
    }
    return fs_is_binary(idx) ? "application/octet-stream" : "text/plain; charset=utf-8";
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* URL path -> decoded path (no query string); 0 if it tries to escape
 * the document root or is malformed */
static int decode_path(const char* in, char* out, int cap) {
    int n = 0;
    for (int i = 0; in[i] && in[i] != '?' && in[i] != '#'; i++) {
        char c = in[i];
        if (c == '%') {
            int hi = hexval(in[i + 1]), lo = hi >= 0 ? hexval(in[i + 2]) : -1;
            if (hi < 0 || lo < 0) return 0;
            c = (char)(hi * 16 + lo);
            i += 2;
        } else if (c == '+') {
            c = ' ';
        }
        if (c == 0 || c == '\\') return 0;
        if (n >= cap - 1) return 0;
        out[n++] = c;
    }
    out[n] = '\0';
    if (out[0] != '/') return 0;
    /* no ".." path segments */
    for (int i = 0; out[i]; i++)
        if (out[i] == '.' && out[i + 1] == '.' && (i == 0 || out[i - 1] == '/') &&
            (out[i + 2] == '/' || out[i + 2] == '\0'))
            return 0;
    return 1;
}

static void html_escape_append(char* buf, uint32_t cap, const char* s) {
    char one[2] = { 0, 0 };
    for (; *s; s++) {
        if (*s == '<') kstrlcat(buf, "&lt;", cap);
        else if (*s == '>') kstrlcat(buf, "&gt;", cap);
        else if (*s == '&') kstrlcat(buf, "&amp;", cap);
        else if (*s == '"') kstrlcat(buf, "&quot;", cap);
        else { one[0] = *s; kstrlcat(buf, one, cap); }
    }
}

static int send_all(tcp_conn_t* c, const void* d, uint32_t n) {
    return tcp_send(c, d, n, 30000) == (int)n ? 0 : -1;
}

static void send_response(tcp_conn_t* c, int head_only, int code, const char* status,
                          const char* type, const void* body, uint32_t len, const char* extra) {
    char hdr[512];
    ksnprintf(hdr, sizeof(hdr),
              "HTTP/1.1 %d %s\r\nServer: BananaOS-httpd/0.6\r\nContent-Type: %s\r\n"
              "Content-Length: %u\r\n%sConnection: close\r\n\r\n",
              code, status, type, len, extra ? extra : "");
    if (send_all(c, hdr, (uint32_t)strlen(hdr)) != 0) return;
    if (!head_only && len) send_all(c, body, len);
}

static void send_error(tcp_conn_t* c, int head_only, int code, const char* status) {
    char body[256];
    ksnprintf(body, sizeof(body),
              "<!DOCTYPE html><html><head><title>%d %s</title></head>"
              "<body><h1>%d %s</h1><hr><p>Banana OS httpd</p></body></html>\n",
              code, status, code, status);
    send_response(c, head_only, code, status, "text/html; charset=utf-8", body,
                  (uint32_t)strlen(body), NULL);
}

static void send_listing(tcp_conn_t* c, int head_only, const char* url, const char* fspath) {
    int dirs[64], files[128];
    int nd = fs_list_dirs(fspath, dirs, 64);
    int nf = fs_list_files(fspath, files, 128);
    if (nd > 64) nd = 64;
    if (nf > 128) nf = 128;
    uint32_t cap = 1024 + (uint32_t)(nd + nf) * 200;
    char* b = (char*)kmalloc(cap);
    if (!b) { send_error(c, head_only, 500, "Internal Server Error"); return; }
    b[0] = '\0';
    kstrlcat(b, "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>Index of ", cap);
    html_escape_append(b, cap, url);
    kstrlcat(b, "</title><style>body{font-family:sans-serif;margin:2em}td{padding:.1em 1em}"
                "</style></head><body><h1>Index of ", cap);
    html_escape_append(b, cap, url);
    kstrlcat(b, "</h1><table>", cap);
    if (strcmp(url, "/") != 0) kstrlcat(b, "<tr><td><a href=\"../\">../</a></td><td></td></tr>", cap);
    for (int i = 0; i < nd; i++) {
        const fs_dir_t* d = fs_get_dir(dirs[i]);
        if (!d) continue;
        kstrlcat(b, "<tr><td><a href=\"", cap);
        html_escape_append(b, cap, d->name);
        kstrlcat(b, "/\">", cap);
        html_escape_append(b, cap, d->name);
        kstrlcat(b, "/</a></td><td>-</td></tr>", cap);
    }
    for (int i = 0; i < nf; i++) {
        fs_file_t* f = fs_file_info(files[i]);
        char size[24];
        ksnprintf(size, sizeof(size), "%u", f->size);
        kstrlcat(b, "<tr><td><a href=\"", cap);
        html_escape_append(b, cap, f->name);
        kstrlcat(b, "\">", cap);
        html_escape_append(b, cap, f->name);
        kstrlcat(b, "</a></td><td>", cap);
        kstrlcat(b, size, cap);
        kstrlcat(b, "</td></tr>", cap);
    }
    kstrlcat(b, "</table><hr><p>Banana OS httpd</p></body></html>\n", cap);
    send_response(c, head_only, 200, "OK", "text/html; charset=utf-8", b, (uint32_t)strlen(b), NULL);
    kfree(b);
}

/* ── PHP pages ────────────────────────────────────────────────────── */

static int is_php(const char* name) {
    const char* dot = strrchr(name, '.');
    return dot && strcasecmp(dot, ".php") == 0;
}

/* value of a request header ("" if absent) */
static void req_header(const char* req, const char* name, char* out, int cap) {
    out[0] = '\0';
    size_t nl = strlen(name);
    for (const char* l = strchr(req, '\n'); l && l[1]; l = strchr(l + 1, '\n')) {
        const char* h = l + 1;
        if (*h == '\r' || *h == '\n') break;
        if (strncasecmp(h, name, nl) == 0 && h[nl] == ':') {
            h += nl + 1;
            while (*h == ' ') h++;
            int n = 0;
            while (h[n] && h[n] != '\r' && h[n] != '\n' && n < cap - 1) { out[n] = h[n]; n++; }
            out[n] = '\0';
            return;
        }
    }
}

static const char* status_text(int code) {
    switch (code) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 500: return "Internal Server Error";
    default:  return code < 400 ? "OK" : "Error";
    }
}

static int serve_php(tcp_conn_t* c, int head_only, const char* req, const char* method,
                     const char* target, const char* path, const char* fspath, const char* ip) {
    int fidx = fs_find_file(fspath);
    if (fidx < 0) { send_error(c, head_only, 404, "Not Found"); return 404; }
    fs_file_t* f = fs_get_file(fidx);
    /* the file can change while the script runs (file_put_contents): run a copy */
    char* code = (char*)kmalloc(f->size + 1);
    if (!code) { send_error(c, head_only, 500, "Internal Server Error"); return 500; }
    memcpy(code, f->content, f->size);
    code[f->size] = '\0';
    uint32_t code_len = f->size;

    char host[128], ua[160];
    req_header(req, "Host", host, sizeof(host));
    req_header(req, "User-Agent", ua, sizeof(ua));
    const char* q = strchr(target, '?');
    char query[512];
    kstrlcpy(query, q ? q + 1 : "", sizeof(query));
    char* hash = strchr(query, '#');
    if (hash) *hash = '\0';

    php_request_t rq = { method, target, path, query, HTTPD_ROOT, ip, host, ua };
    php_result_t res;
    httpd_run_php(fspath, code, code_len, &rq, &res);
    kfree(code);

    char extra[600];
    extra[0] = '\0';
    if (res.location[0]) ksnprintf(extra, sizeof(extra), "Location: %s\r\n", res.location);
    send_response(c, head_only, res.status, status_text(res.status), res.content_type,
                  res.body ? res.body : "", res.len, extra);
    if (res.body) kfree(res.body);
    return res.status;
}

/* ── one request ──────────────────────────────────────────────────── */

static void handle(tcp_conn_t* c) {
    char* req = (char*)kmalloc(REQ_MAX);
    if (!req) { tcp_abort(c); return; }
    int n = 0;
    uint32_t start = timer_ms();
    while (n < REQ_MAX - 1) {
        int r = tcp_recv(c, req + n, (uint32_t)(REQ_MAX - 1 - n), 100);
        if (r > 0) {
            n += r;
            req[n] = '\0';
            if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n")) break;
        } else if (r != NET_ERR_TIMEOUT || timer_ms() - start > 10000 || !g_running) {
            break;
        }
    }
    req[n] = '\0';

    ip4_t pip;
    char ipstr[16];
    tcp_peer(c, &pip, NULL);
    ip4_to_str(pip, ipstr);

    char method[8], target[512], path[256], fspath[FS_PATH_LEN];
    int code = 400;
    int head_only = 0;
    method[0] = target[0] = '\0';
    char* sp1 = strchr(req, ' ');
    char* sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
    if (!sp1 || !sp2 || sp1 - req >= (int)sizeof(method) || sp2 - sp1 - 1 >= (int)sizeof(target)) {
        if (n > 0) send_error(c, 0, 400, "Bad Request");
    } else {
        memcpy(method, req, (size_t)(sp1 - req));
        method[sp1 - req] = '\0';
        memcpy(target, sp1 + 1, (size_t)(sp2 - sp1 - 1));
        target[sp2 - sp1 - 1] = '\0';
        head_only = strcmp(method, "HEAD") == 0;

        if (strcmp(method, "GET") != 0 && !head_only) {
            code = 405;
            send_response(c, 0, 405, "Method Not Allowed", "text/plain", "405 Method Not Allowed\n", 23,
                          "Allow: GET, HEAD\r\n");
        } else if (!decode_path(target, path, sizeof(path)) ||
                   strlen(HTTPD_ROOT) + strlen(path) >= sizeof(fspath)) {
            code = 400;
            send_error(c, head_only, 400, "Bad Request");
        } else {
            ksnprintf(fspath, sizeof(fspath), "%s%s", HTTPD_ROOT, path);
            size_t fl = strlen(fspath);
            while (fl > 1 && fspath[fl - 1] == '/') fspath[--fl] = '\0';

            int fidx = fs_find_file(fspath);
            if (fidx >= 0 && is_php(fs_file_info(fidx)->name)) {
                code = serve_php(c, head_only, req, method, target, path, fspath, ipstr);
            } else if (fidx >= 0) {
                fs_file_t* f = fs_get_file(fidx);
                code = 200;
                fs_pin(fidx);              /* (kept in RAM while it is sent) */
                send_response(c, head_only, 200, "OK", content_type(f->name, fidx), f->content, f->size, NULL);
                fs_unpin(fidx);
            } else if (fs_find_dir(fspath) >= 0) {
                if (path[strlen(path) - 1] != '/') {
                    /* folder without the trailing slash: relative links need it */
                    char loc[300];
                    ksnprintf(loc, sizeof(loc), "Location: %s/\r\n", path);
                    code = 301;
                    send_response(c, head_only, 301, "Moved Permanently", "text/plain", "", 0, loc);
                } else {
                    char index[FS_PATH_LEN];
                    ksnprintf(index, sizeof(index), "%s/index.html", fspath);
                    int iidx = fs_find_file(index);
                    char pindex[FS_PATH_LEN];
                    ksnprintf(pindex, sizeof(pindex), "%s/index.php", fspath);
                    code = 200;
                    if (iidx < 0 && fs_find_file(pindex) >= 0) {
                        code = serve_php(c, head_only, req, method, target, path, pindex, ipstr);
                    } else if (iidx >= 0) {
                        fs_file_t* f = fs_get_file(iidx);
                        fs_pin(iidx);
                        send_response(c, head_only, 200, "OK", "text/html; charset=utf-8", f->content, f->size, NULL);
                        fs_unpin(iidx);
                    } else {
                        send_listing(c, head_only, path, fspath);
                    }
                }
            } else {
                code = 404;
                send_error(c, head_only, 404, "Not Found");
            }
        }
    }

    g_requests++;
    char line[96];
    char shown[48];
    kstrlcpy(shown, target[0] ? target : "-", sizeof(shown));
    ksnprintf(line, sizeof(line), "%s %s %s %d", ipstr, method[0] ? method : "-", shown, code);
    log_request(line);
    kfree(req);
    tcp_close(c);
}

static void httpd_task(void) {
    task_set_background();
    for (;;) {
        if (!g_running || !g_listener) { task_sleep_ms(200); continue; }
        tcp_conn_t* c = tcp_accept(g_listener, 500);
        if (c) handle(c);
    }
}

/* ── control ──────────────────────────────────────────────────────── */

static void ensure_docroot(void) {
    if (fs_find_dir(HTTPD_ROOT) < 0) fs_mkdir_p(HTTPD_ROOT);
    char index[FS_PATH_LEN];
    ksnprintf(index, sizeof(index), "%s/index.html", HTTPD_ROOT);
    int dirs[1], files[1];
    /* a brand-new, empty document root gets a welcome page */
    if (fs_list_files(HTTPD_ROOT, files, 1) == 0 && fs_list_dirs(HTTPD_ROOT, dirs, 1) == 0)
        fs_write_path(index, DEFAULT_INDEX, sizeof(DEFAULT_INDEX) - 1);
    /* next to the untouched welcome page (new, or from an older Banana OS)
     * goes a PHP demo */
    char demo[FS_PATH_LEN];
    ksnprintf(demo, sizeof(demo), "%s/demo.php", HTTPD_ROOT);
    int iidx = fs_find_file(index);
    if (iidx >= 0 && fs_find_file(demo) < 0) {
        fs_file_t* f = fs_get_file(iidx);
        if (f->size == sizeof(DEFAULT_INDEX) - 1 && memcmp(f->content, DEFAULT_INDEX, f->size) == 0)
            fs_write_path(demo, HTTPD_DEMO_PHP, HTTPD_DEMO_PHP_LEN);
    }
}

int httpd_start(uint16_t port, char* err, int errcap) {
    if (g_running) {
        if (port == g_port) return 0;
        httpd_stop();
    }
    int e;
    tcp_listener_t* l = tcp_listen(port, &e);
    if (!l) {
        ksnprintf(err, (size_t)errcap, e == NET_ERR_PROTO ? "port %u is already in use" : "no free listener slot", port);
        return -1;
    }
    if (!g_task_started) {
        /* a big stack: .php pages run a recursive interpreter */
        if (task_create_stack("httpd", httpd_task, 1u << 20) < 0) {
            tcp_unlisten(l);
            ksnprintf(err, (size_t)errcap, "no free task slot");
            return -1;
        }
        g_task_started = 1;
    }
    ensure_docroot();
    g_listener = l;
    g_port = port;
    g_running = 1;
    return 0;
}

void httpd_stop(void) {
    g_running = 0;
    if (g_listener) tcp_unlisten(g_listener);
    g_listener = NULL;
}

int httpd_running(void) { return g_running; }
uint16_t httpd_port(void) { return g_port; }

void httpd_print_status(void) {
    char line[128];
    if (!g_running) {
        terminal_writeln("httpd: stopped");
        return;
    }
    netif_t* nif = net_if();
    char ip[16];
    ip4_to_str(nif->ip, ip);
    char url[40];
    if (g_port == 80) ksnprintf(url, sizeof(url), "http://%s/", ip);
    else ksnprintf(url, sizeof(url), "http://%s:%u/", ip, g_port);
    ksnprintf(line, sizeof(line), "httpd: running on port %u, serving %s - %s", g_port, HTTPD_ROOT, url);
    terminal_writeln(line);
    ksnprintf(line, sizeof(line), "       %u requests served", g_requests);
    terminal_writeln(line);
    int any = 0;
    for (int i = 0; i < LOG_LINES; i++) {
        const char* l = g_log[(g_log_next + i) % LOG_LINES];
        if (!l[0]) continue;
        if (!any) terminal_writeln("       recent:");
        ksnprintf(line, sizeof(line), "         %s", l);
        terminal_writeln(line);
        any = 1;
    }
}
