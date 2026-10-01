#include "browser.h"
#include "gfx.h"
#include "fs.h"
#include "kstring.h"
#include "kheap.h"
#include "timer.h"
#include "task.h"
#include "image.h"
#include "keyboard.h"
#include "rtc.h"
#include "serial.h"
#include "gui.h"
#include "../net/http.h"
#include "../web/page.h"
#include "../web/render.h"

/*
 * The web browser window. Everything that touches a page (loading,
 * scripts, layout, rendering) happens in the "browser" task; the GUI
 * (any task calling gui_poll()) only paints the frame that task left in
 * g_view and queues clicks/commands for it.
 */

#define WIN_W     720
#define WIN_H     520
#define TITLE_H   20
#define TOOL_Y    (TITLE_H + 3)
#define TOOL_H    22
#define VIEW_X    4
#define VIEW_Y    (TOOL_Y + TOOL_H + 4)
#define STATUS_H  16
#define SB_W      12                    /* scrollbar */
#define VIEW_W    (WIN_W - 2 * VIEW_X - SB_W)
#define VIEW_H    (WIN_H - VIEW_Y - STATUS_H - 4)
#define ADDR_X    124
#define ADDR_W    (WIN_W - ADDR_X - 52)

#define HOME_URL  "about:home"
#define HIST_MAX  24
#define MAX_DOC   (8u << 20)
#define PAGE_MEM  (48u << 20)

#define C_PANEL   0x001D232Cu
#define C_TITLE   0x00384562u
#define C_TEXT    0x00E8EEF6u
#define C_DIM     0x00AAB6C6u
#define C_ERR     0x00F08070u

enum { CMD_NONE = 0, CMD_CLICK, CMD_GO, CMD_BACK, CMD_FWD, CMD_RELOAD, CMD_HOME };

typedef struct { int kind, x, y; } cmd_t;

static int      g_open;
static int      g_x = 40, g_y = 20;
static int      g_dragging, g_drag_dx, g_drag_dy;
static int      g_sb_drag, g_sb_drag_dy;
static uint32_t g_gen;

static int      g_task = -1;
static uint32_t* g_view;                /* rendered page, VIEW_W x VIEW_H */
static int      g_scroll, g_page_h;
static int      g_render_req;

static char     g_addr[1024];           /* address bar text */
static int      g_addr_focus;
static char     g_go_url[1024];         /* CMD_GO target */
static char     g_title[128];
static char     g_status[160];
static int      g_status_err;
static int      g_loading;
static char     g_alert[256];

static cmd_t    g_cmds[16];
static int      g_cmd_head, g_cmd_tail;

static char     g_hist[HIST_MAX][512];
static int      g_hist_n, g_hist_pos = -1;

static page_t*  g_page;
static page_env_t g_env;

/* ══ clock for Date / PHP date() ══════════════════════════════════════ */

static void kernel_clock(script_tm_t* tm) {
    rtc_datetime_t dt;
    memset(tm, 0, sizeof(*tm));
    if (rtc_read_datetime(&dt) != 0) { tm->year = 2026; tm->month = 1; tm->day = 1; return; }
    tm->year = dt.year;
    tm->month = dt.month;
    tm->day = dt.day;
    tm->hour = dt.hour;
    tm->minute = dt.minute;
    tm->second = dt.second;
    /* Zeller-style weekday, 0 = Sunday */
    int y = dt.year, m = dt.month;
    if (m < 3) { m += 12; y--; }
    int k = y % 100, j = y / 100;
    int h = (dt.day + 13 * (m + 1) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;   /* 0 = Saturday */
    tm->wday = (h + 6) % 7;
}

void web_init(void) {
    script_clock = kernel_clock;
}

/* ══ small helpers ════════════════════════════════════════════════════ */

static void push_cmd(int kind, int x, int y) {
    int next = (g_cmd_head + 1) % 16;
    if (next == g_cmd_tail) return;           /* full: drop */
    g_cmds[g_cmd_head].kind = kind;
    g_cmds[g_cmd_head].x = x;
    g_cmds[g_cmd_head].y = y;
    g_cmd_head = next;
}

static void set_status(const char* s, int err) {
    kstrlcpy(g_status, s, sizeof(g_status));
    g_status_err = err;
    g_gen++;
}

static void bevel(int x, int y, int w, int h, uint32_t base, uint32_t hi, uint32_t lo) {
    gfx_fill_rect(x, y, w, h, base);
    gfx_fill_rect(x, y, w, 1, hi);
    gfx_fill_rect(x, y, 1, h, hi);
    gfx_fill_rect(x, y + h - 1, w, 1, lo);
    gfx_fill_rect(x + w - 1, y, 1, h, lo);
}

static int inside(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

/* growable text buffer for generated pages */
typedef struct { char* s; uint32_t n, cap; } sbuf_t;

static void sb_add(sbuf_t* b, const char* s, uint32_t n) {
    if (b->n + n + 1 > b->cap) {
        uint32_t cap = (b->n + n + 1) * 2 + 1024;
        char* ns = (char*)kmalloc(cap);
        if (!ns) return;
        if (b->s) { memcpy(ns, b->s, b->n); kfree(b->s); }
        b->s = ns;
        b->cap = cap;
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}

static void sb_str(sbuf_t* b, const char* s) { sb_add(b, s, (uint32_t)strlen(s)); }

static void sb_esc(sbuf_t* b, const char* s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '<') sb_str(b, "&lt;");
        else if (c == '>') sb_str(b, "&gt;");
        else if (c == '&') sb_str(b, "&amp;");
        else if (c == '"') sb_str(b, "&quot;");
        else sb_add(b, &c, 1);
    }
}

/* ══ built-in pages ═══════════════════════════════════════════════════ */

static const char HOME_HTML[] =
    "<!DOCTYPE html><html><head><title>Banana Browser</title>\n"
    "<style>\n"
    " body { background: #fdf6d8; color: #222; margin: 0; }\n"
    " .top { background: #f4d35e; padding: 14px 20px; border-bottom: 3px solid #c9a227; }\n"
    " .top h1 { margin: 0; font-size: 28px; color: #4a3b00; }\n"
    " .main { padding: 6px 20px; }\n"
    " .card { background: #fff; border: 1px solid #e0d39a; padding: 8px 12px; margin: 10px 0; }\n"
    " h2 { font-size: 16px; color: #6b5300; margin: 2px 0 6px 0; }\n"
    " a { color: #1a5fb4; }\n"
    " #clock { font-weight: bold; color: #2a7a3a; }\n"
    " code { background: #f1ead0; }\n"
    "</style></head><body>\n"
    "<div class=\"top\"><h1>Banana Browser</h1>The web browser of Banana OS</div>\n"
    "<div class=\"main\">\n"
    "<div class=\"card\"><h2>Start here</h2><ul>\n"
    " <li><a href=\"http://localhost/\">http://localhost/</a> - this machine's web server (start it with <code>httpd start</code>)</li>\n"
    " <li><a href=\"http://localhost/demo.php\">http://localhost/demo.php</a> - a PHP page run by httpd</li>\n"
    " <li><a href=\"http://example.com/\">http://example.com/</a> - a page on the Internet</li>\n"
    " <li><a href=\"file:///home/banana\">file:///home/banana</a> - your files</li>\n"
    "</ul></div>\n"
    "<div class=\"card\"><h2>JavaScript</h2>\n"
    "<p>The time is <span id=\"clock\">...</span>. You clicked <b id=\"n\">0</b> times.\n"
    "<button onclick=\"document.getElementById('n').textContent = ++clicks\">Click me</button></p>\n"
    "<p><input id=\"who\" placeholder=\"Type your name\" oninput=\"hello()\"> <span id=\"hi\"></span></p>\n"
    "</div>\n"
    "<div class=\"card\"><h2>Keys</h2>Ctrl+L: address bar - Enter: go - Backspace: back - "
    "arrows / space: scroll - Ctrl+R: reload</div>\n"
    "</div>\n"
    "<script>\n"
    "var clicks = 0;\n"
    "function pad(n) { return (n < 10 ? '0' : '') + n; }\n"
    "function tick() { var d = new Date(); document.getElementById('clock').textContent ="
    " pad(d.getHours()) + ':' + pad(d.getMinutes()) + ':' + pad(d.getSeconds()); }\n"
    "function hello() { var v = document.getElementById('who').value;"
    " document.getElementById('hi').innerHTML = v ? 'Hello, <b>' + v + '</b>!' : ''; }\n"
    "tick(); setInterval(tick, 1000);\n"
    "</script></body></html>\n";

static char* error_page(const char* url, const char* why, uint32_t* len) {
    sbuf_t b = { 0, 0, 0 };
    sb_str(&b, "<html><head><title>Problem loading page</title><style>body{background:#2b2f36;color:#eee;"
               "padding:20px} h1{color:#f4d35e} code{color:#9cf}</style></head><body><h1>Cannot open this page</h1><p><code>");
    sb_esc(&b, url, (uint32_t)strlen(url));
    sb_str(&b, "</code></p><p>");
    sb_esc(&b, why, (uint32_t)strlen(why));
    sb_str(&b, "</p><p><a href=\"about:home\">Go to the start page</a></p></body></html>");
    *len = b.n;
    return b.s;
}

/* file:///path - a file, or a folder listing */
static int fetch_file(const char* path, char** data, uint32_t* len, char* ctype, int ccap, char* err, int ecap) {
    char p[FS_PATH_LEN];
    kstrlcpy(p, *path ? path : "/", sizeof(p));
    /* %20 and friends */
    char* w = p;
    for (const char* r = p; *r; r++) {
        if (*r == '%' && r[1] && r[2]) {
            int hi = r[1] <= '9' ? r[1] - '0' : (r[1] | 32) - 'a' + 10;
            int lo = r[2] <= '9' ? r[2] - '0' : (r[2] | 32) - 'a' + 10;
            *w++ = (char)(hi * 16 + lo);
            r += 2;
        } else if (*r == '?' || *r == '#') {
            break;
        } else {
            *w++ = *r;
        }
    }
    *w = 0;
    size_t pl = strlen(p);
    while (pl > 1 && p[pl - 1] == '/') p[--pl] = 0;
    int fi = fs_find_file(p);
    if (fi >= 0) {
        fs_file_t* f = fs_get_file(fi);
        char* buf = (char*)kmalloc(f->size + 1);
        if (!buf) { kstrlcpy(err, "out of memory", (size_t)ecap); return -1; }
        memcpy(buf, f->content, f->size);
        buf[f->size] = 0;
        *data = buf;
        *len = f->size;
        const char* dot = strrchr(f->name, '.');
        if (dot && (strcasecmp(dot, ".html") == 0 || strcasecmp(dot, ".htm") == 0)) kstrlcpy(ctype, "text/html", (size_t)ccap);
        else if (dot && strcasecmp(dot, ".css") == 0) kstrlcpy(ctype, "text/css", (size_t)ccap);
        else if (dot && strcasecmp(dot, ".js") == 0) kstrlcpy(ctype, "text/javascript", (size_t)ccap);
        else if (image_format((const uint8_t*)f->content, f->size)) kstrlcpy(ctype, "image/any", (size_t)ccap);
        else kstrlcpy(ctype, fs_is_binary(fi) ? "application/octet-stream" : "text/plain", (size_t)ccap);
        return 0;
    }
    if (fs_find_dir(p) < 0) { kstrlcpy(err, "no such file or folder", (size_t)ecap); return -1; }
    sbuf_t b = { 0, 0, 0 };
    sb_str(&b, "<html><head><title>");
    sb_esc(&b, p, (uint32_t)strlen(p));
    sb_str(&b, "</title><style>body{background:#fff} td{padding:1px 10px} .d a{font-weight:bold;color:#8a6d00}"
               " tr.odd{background:#f6f3e6}</style></head><body><h2>Index of ");
    sb_esc(&b, p, (uint32_t)strlen(p));
    sb_str(&b, "</h2><table>");
    const char* slash = strcmp(p, "/") == 0 ? "" : "/";
    if (strcmp(p, "/") != 0) sb_str(&b, "<tr class=\"d\"><td><a href=\"..\">..</a></td><td></td></tr>");
    int idx[FS_MAX_FILES > FS_MAX_DIRS ? FS_MAX_FILES : FS_MAX_DIRS];
    int nd = fs_list_dirs(p, idx, FS_MAX_DIRS);
    int row = 0;
    for (int i = 0; i < nd; i++, row++) {
        const fs_dir_t* d = fs_get_dir(idx[i]);
        char link[FS_PATH_LEN + 16];
        ksnprintf(link, sizeof(link), "file://%s%s%s/", p, slash, d->name);
        sb_str(&b, row & 1 ? "<tr class=\"d odd\"><td><a href=\"" : "<tr class=\"d\"><td><a href=\"");
        sb_esc(&b, link, (uint32_t)strlen(link));
        sb_str(&b, "\">");
        sb_esc(&b, d->name, (uint32_t)strlen(d->name));
        sb_str(&b, "/</a></td><td>folder</td></tr>");
    }
    int nf = fs_list_files(p, idx, FS_MAX_FILES);
    for (int i = 0; i < nf; i++, row++) {
        fs_file_t* f = fs_get_file(idx[i]);
        char link[FS_PATH_LEN + 16], size[24];
        ksnprintf(link, sizeof(link), "file://%s%s%s", p, slash, f->name);
        ksnprintf(size, sizeof(size), "%u bytes", f->size);
        sb_str(&b, row & 1 ? "<tr class=\"odd\"><td><a href=\"" : "<tr><td><a href=\"");
        sb_esc(&b, link, (uint32_t)strlen(link));
        sb_str(&b, "\">");
        sb_esc(&b, f->name, (uint32_t)strlen(f->name));
        sb_str(&b, "</a></td><td>");
        sb_str(&b, size);
        sb_str(&b, "</td></tr>");
    }
    sb_str(&b, "</table></body></html>");
    *data = b.s;
    *len = b.n;
    kstrlcpy(ctype, "text/html", (size_t)ccap);
    return 0;
}

typedef struct { char* buf; uint32_t n, cap; int too_big; } body_t;

static int on_body(void* ctx, const uint8_t* d, uint32_t n) {
    body_t* b = (body_t*)ctx;
    if (b->n + n + 1 > MAX_DOC) { b->too_big = 1; return -1; }
    if (b->n + n + 1 > b->cap) {
        uint32_t cap = (b->n + n + 1) * 2;
        if (cap > MAX_DOC) cap = MAX_DOC;
        char* nb = (char*)kmalloc(cap);
        if (!nb) { b->too_big = 1; return -1; }
        if (b->buf) { memcpy(nb, b->buf, b->n); kfree(b->buf); }
        b->buf = nb;
        b->cap = cap;
    }
    memcpy(b->buf + b->n, d, n);
    b->n += n;
    b->buf[b->n] = 0;
    return 0;
}

static int env_fetch(void* ctx, const char* url, char** data, uint32_t* len, char* ctype, int ccap,
                     char* final_url, int fcap, char* err, int ecap) {
    (void)ctx;
    ctype[0] = 0;
    err[0] = 0;
    kstrlcpy(final_url, url, (size_t)fcap);
    if (strncasecmp(url, "about:", 6) == 0) {
        const char* src = strcasecmp(url, "about:home") == 0 ? HOME_HTML : "<html><body></body></html>";
        uint32_t n = (uint32_t)strlen(src);
        char* b = (char*)kmalloc(n + 1);
        if (!b) return -1;
        memcpy(b, src, n + 1);
        *data = b;
        *len = n;
        kstrlcpy(ctype, "text/html", (size_t)ccap);
        return 0;
    }
    if (strncasecmp(url, "file://", 7) == 0) return fetch_file(url + 7, data, len, ctype, ccap, err, ecap);
    if (strncasecmp(url, "http://", 7) != 0 && strncasecmp(url, "https://", 8) != 0) {
        kstrlcpy(err, "unsupported address (use http://, https://, file:// or about:)", (size_t)ecap);
        return -1;
    }
    char msg[200];
    ksnprintf(msg, sizeof(msg), "Loading %s ...", url);
    set_status(msg, 0);
    body_t b = { 0, 0, 0, 0 };
    http_request_t req;
    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.follow_redirects = 1;
    req.max_redirects = 10;
    req.timeout_ms = 15000;
    req.user_agent = "Mozilla/5.0 (Banana OS) BananaBrowser/1.0";
    req.ctx = &b;
    req.on_body = on_body;
    http_response_t* resp = (http_response_t*)kmalloc(sizeof(http_response_t));
    if (!resp) return -1;
    int rc = http_fetch(url, &req, resp, err, (uint32_t)ecap);
    if (rc != NET_OK || b.too_big) {
        if (b.too_big) kstrlcpy(err, "the page is too big (8 MiB max)", (size_t)ecap);
        if (b.buf) kfree(b.buf);
        kfree(resp);
        return -1;
    }
    kstrlcpy(ctype, resp->content_type, (size_t)ccap);
    if (resp->final_url[0]) kstrlcpy(final_url, resp->final_url, (size_t)fcap);
    if (resp->status >= 400 && !b.n) {
        ksnprintf(err, (size_t)ecap, "HTTP %d %s", resp->status, resp->reason);
        kfree(resp);
        return -1;
    }
    kfree(resp);
    if (!b.buf) { b.buf = (char*)kmalloc(1); if (!b.buf) return -1; b.buf[0] = 0; }
    *data = b.buf;
    *len = b.n;
    return 0;
}

static int env_decode_image(void* ctx, const uint8_t* data, uint32_t len, img_data_t* out, arena_t* A) {
    (void)ctx;
    image_t img;
    char err[64];
    if (!image_format(data, len)) return -1;
    if (image_decode(data, len, &img, err, sizeof(err)) != 0) return -1;
    if ((uint64_t)img.w * (uint64_t)img.h > 4000000u) { image_free(&img); return -1; }
    uint32_t n = (uint32_t)(img.w * img.h);
    uint32_t* px = (uint32_t*)arena_alloc(A, n * 4);
    if (A->oom) { image_free(&img); return -1; }
    for (uint32_t i = 0; i < n; i++)
        px[i] = (uint32_t)img.rgb[i * 3] << 16 | (uint32_t)img.rgb[i * 3 + 1] << 8 | img.rgb[i * 3 + 2];
    out->px = px;
    out->w = img.w;
    out->h = img.h;
    image_free(&img);
    return 0;
}

static void env_log(void* ctx, const char* line) {
    (void)ctx;
    klog("browser: %s\n", line);
}

/* ══ the browser task ═════════════════════════════════════════════════ */

static void render_view(void) {
    if (!g_view) return;
    layout_t* L = g_page ? g_page->layout : NULL;
    g_page_h = L ? L->height : 0;
    int max = g_page_h - VIEW_H;
    if (g_scroll > max) g_scroll = max;
    if (g_scroll < 0) g_scroll = 0;
    render_page(L, g_view, VIEW_W, VIEW_W, VIEW_H, 0, 0, VIEW_W, VIEW_H, g_scroll);
    g_gen++;
}

static void normalize_url(const char* in, char* out, int cap) {
    while (*in == ' ') in++;
    if (strstr(in, "://") || strncasecmp(in, "about:", 6) == 0) kstrlcpy(out, in, (size_t)cap);
    else if (in[0] == '/') ksnprintf(out, (size_t)cap, "file://%s", in);
    else ksnprintf(out, (size_t)cap, "http://%s", in);
    int n = (int)strlen(out);
    while (n > 0 && out[n - 1] == ' ') out[--n] = 0;
}

static int starts_ci(const char* s, const char* p) { return strncasecmp(s, p, strlen(p)) == 0; }

/* plain text and pictures get a little HTML around them */
static char* wrap_content(const char* url, const char* ctype, char* data, uint32_t* len) {
    if (!ctype[0] || starts_ci(ctype, "text/html") || starts_ci(ctype, "application/xhtml")) {
        /* sniff: binary data without a type is not HTML */
        return data;
    }
    sbuf_t b = { 0, 0, 0 };
    if (starts_ci(ctype, "image/") || image_format((const uint8_t*)data, *len)) {
        sb_str(&b, "<html><head><title>");
        const char* slash = strrchr(url, '/');
        sb_esc(&b, slash ? slash + 1 : url, (uint32_t)strlen(slash ? slash + 1 : url));
        sb_str(&b, "</title></head><body style=\"background:#333;margin:8px;text-align:center\"><img src=\"");
        sb_esc(&b, url, (uint32_t)strlen(url));
        sb_str(&b, "\"></body></html>");
    } else if (starts_ci(ctype, "text/") || starts_ci(ctype, "application/json") || starts_ci(ctype, "application/javascript")) {
        sb_str(&b, "<html><body><pre>");
        sb_esc(&b, data, *len);
        sb_str(&b, "</pre></body></html>");
    } else {
        sb_str(&b, "<html><body><h2>Cannot show this file</h2><p>Type: ");
        sb_esc(&b, ctype, (uint32_t)strlen(ctype));
        sb_str(&b, "</p><p>Save it from a terminal with <code>wget</code> instead.</p></body></html>");
    }
    kfree(data);
    *len = b.n;
    return b.s;
}

static void load(const char* url_in, int add_history) {
    char url[1024], final_url[1024], ctype[96], err[200];
    normalize_url(url_in, url, sizeof(url));
    kstrlcpy(g_addr, url, sizeof(g_addr));
    g_addr_focus = 0;
    g_loading = 1;
    set_status("Loading...", 0);

    char* data = NULL;
    uint32_t len = 0;
    if (env_fetch(NULL, url, &data, &len, ctype, sizeof(ctype), final_url, sizeof(final_url), err, sizeof(err)) != 0) {
        data = error_page(url, err[0] ? err : "the page could not be loaded", &len);
        kstrlcpy(final_url, url, sizeof(final_url));
        kstrlcpy(ctype, "text/html", sizeof(ctype));
        set_status(err[0] ? err : "Error", 1);
    }
    if (!data) { set_status("out of memory", 1); g_loading = 0; return; }
    data = wrap_content(final_url, ctype, data, &len);
    if (!data) { set_status("out of memory", 1); g_loading = 0; return; }

    if (g_page) { page_free(g_page); g_page = NULL; }
    render_view();                          /* blank while the new page loads */
    page_t* p = page_new(&g_env, PAGE_MEM);
    if (!p) { kfree(data); set_status("out of memory", 1); g_loading = 0; return; }
    p->view_h = VIEW_H;
    uint32_t t0 = timer_ms();
    page_load(p, final_url, data, len, VIEW_W);
    kfree(data);
    g_page = p;
    g_scroll = p->scroll_req > 0 ? p->scroll_req : 0;
    p->scroll_req = -1;
    kstrlcpy(g_addr, final_url, sizeof(g_addr));
    kstrlcpy(g_title, p->title[0] ? p->title : final_url, sizeof(g_title));

    if (add_history) {
        if (g_hist_pos < g_hist_n - 1) g_hist_n = g_hist_pos + 1;   /* drop the forward part */
        if (g_hist_n == HIST_MAX) {
            for (int i = 1; i < HIST_MAX; i++) kstrlcpy(g_hist[i - 1], g_hist[i], sizeof(g_hist[0]));
            g_hist_n--;
        }
        kstrlcpy(g_hist[g_hist_n], final_url, sizeof(g_hist[0]));
        g_hist_pos = g_hist_n++;
    } else if (g_hist_pos >= 0) {
        kstrlcpy(g_hist[g_hist_pos], final_url, sizeof(g_hist[0]));
    }

    if (!g_status_err) {
        char msg[160];
        if (p->status[0]) { kstrlcpy(msg, p->status, sizeof(msg)); g_status_err = 1; }
        else ksnprintf(msg, sizeof(msg), "Done (%u ms)%s", timer_ms() - t0, p->A.oom ? " - out of page memory" : "");
        set_status(msg, p->status[0] != 0);
    }
    p->status[0] = 0;
    g_loading = 0;
    render_view();
}

/* after scripts or a click: navigation, alerts, scrolling, relayout */
static void after_page_event(void) {
    page_t* p = g_page;
    if (!p) return;
    if (p->alert_pending) {
        kstrlcpy(g_alert, p->alert, sizeof(g_alert));
        p->alert_pending = 0;
        g_gen++;
    }
    if (p->status[0]) {
        set_status(p->status, 1);
        p->status[0] = 0;
    }
    if (p->title[0] && strcmp(p->title, g_title) != 0) {
        kstrlcpy(g_title, p->title, sizeof(g_title));
        g_gen++;
    }
    if (p->nav_pending) {
        p->nav_pending = 0;
        char nav[1024];
        kstrlcpy(nav, p->nav, sizeof(nav));
        load(nav, 1);
        return;
    }
    int changed = p->dirty;
    page_update(p, VIEW_W);
    if (p->scroll_req >= 0) {
        g_scroll = p->scroll_req;
        p->scroll_req = -1;
        changed = 1;
    }
    if (changed || g_render_req) {
        g_render_req = 0;
        render_view();
    }
}

static void scroll_by(int dy) {
    g_scroll += dy;
    g_render_req = 1;
}

static void history_go(int delta) {
    int np = g_hist_pos + delta;
    if (np < 0 || np >= g_hist_n) return;
    g_hist_pos = np;
    char url[512];
    kstrlcpy(url, g_hist[np], sizeof(url));
    load(url, 0);
}

static void handle_key(char c) {
    if (c == 12) { g_addr_focus = 1; g_gen++; return; }            /* Ctrl+L */
    if (c == 18) { push_cmd(CMD_RELOAD, 0, 0); return; }            /* Ctrl+R */
    if (g_alert[0]) {
        if (c == '\n' || c == 27 || c == ' ') { g_alert[0] = 0; g_gen++; }
        return;
    }
    if (g_addr_focus) {
        size_t n = strlen(g_addr);
        if (c == '\n') { kstrlcpy(g_go_url, g_addr, sizeof(g_go_url)); push_cmd(CMD_GO, 0, 0); }
        else if (c == '\b') { if (n) g_addr[n - 1] = 0; }
        else if (c == 27) { g_addr_focus = 0; if (g_page) kstrlcpy(g_addr, g_page->url, sizeof(g_addr)); }
        else if ((unsigned char)c >= 32 && n < sizeof(g_addr) - 1) { g_addr[n] = c; g_addr[n + 1] = 0; }
        g_gen++;
        return;
    }
    if (g_page && page_key(g_page, c)) { after_page_event(); return; }
    if (c == ' ') scroll_by(VIEW_H - 40);
    else if (c == '\b') history_go(-1);
    after_page_event();
}

static void handle_arrow(char a) {
    if (g_addr_focus) return;
    if (a == 'A') scroll_by(-40);
    else if (a == 'B') scroll_by(40);
    else if (a == '5') scroll_by(-(VIEW_H - 40));       /* PgUp: ESC [ 5 ~ */
    else if (a == '6') scroll_by(VIEW_H - 40);
    else if (a == 'H') { g_scroll = 0; g_render_req = 1; }
    else if (a == 'F') { g_scroll = 1 << 28; g_render_req = 1; }
    after_page_event();
}

static void read_keys(void) {
    if (!gui_browser_focused()) return;
    for (int k = 0; k < 32; k++) {
        char c = keyboard_try_getchar();
        if (!c) return;
        if (c == 27) {
            char c2 = keyboard_try_getchar();
            if (c2 == '[') {
                char c3 = keyboard_try_getchar();
                if ((c3 == '5' || c3 == '6') ) keyboard_try_getchar();   /* '~' */
                if (!gui_handle_arrow(c3)) handle_arrow(c3);
                continue;
            }
            if (!gui_handle_key(27)) handle_key(27);
            if (c2) handle_key(c2);
            continue;
        }
        if (gui_handle_key(c)) continue;                  /* Ctrl+T: Start menu */
        handle_key(c);
    }
}

static void run_commands(void) {
    while (g_cmd_tail != g_cmd_head) {
        cmd_t c = g_cmds[g_cmd_tail];
        g_cmd_tail = (g_cmd_tail + 1) % 16;
        switch (c.kind) {
        case CMD_CLICK:
            if (g_page) {
                g_addr_focus = 0;
                page_click(g_page, c.x, c.y);
                after_page_event();
            }
            break;
        case CMD_GO: {
            char url[1024];
            kstrlcpy(url, g_go_url, sizeof(url));
            load(url, 1);
            break;
        }
        case CMD_BACK: history_go(-1); break;
        case CMD_FWD: history_go(1); break;
        case CMD_RELOAD:
            if (g_hist_pos >= 0) { char url[512]; kstrlcpy(url, g_hist[g_hist_pos], sizeof(url)); load(url, 0); }
            else load(HOME_URL, 1);
            break;
        case CMD_HOME: load(HOME_URL, 1); break;
        }
    }
}

static void browser_task(void) {
    g_env.fetch = env_fetch;
    g_env.decode_image = env_decode_image;
    g_env.now_ms = timer_ms;
    g_env.log = env_log;
    g_env.ctx = NULL;
    for (;;) {
        if (!g_open) {
            /* closed: drop the page (frees its memory), wait */
            if (g_page) { page_free(g_page); g_page = NULL; }
            task_sleep_ms(100);
            continue;
        }
        read_keys();
        run_commands();
        if (g_page && !g_loading) {
            if (page_tick(g_page)) g_page->dirty = 1;
            if (g_page->dirty || g_page->nav_pending || g_page->alert_pending || g_render_req) after_page_event();
        }
        task_sleep_ms(10);
    }
}

/* ══ window (GUI side) ════════════════════════════════════════════════ */

void browser_open(const char* url) {
    if (!g_view) {
        g_view = (uint32_t*)kmalloc((uint32_t)VIEW_W * VIEW_H * 4);
        if (!g_view) return;
        memset32(g_view, 0xFFFFFF, (size_t)VIEW_W * VIEW_H);
    }
    if (g_task < 0) {
        g_task = task_create_stack("browser", browser_task, 2u << 20);
        if (g_task < 0) return;
    }
    if (url && *url) { kstrlcpy(g_go_url, url, sizeof(g_go_url)); push_cmd(CMD_GO, 0, 0); }
    else if (!g_page && g_hist_pos < 0) { kstrlcpy(g_go_url, HOME_URL, sizeof(g_go_url)); push_cmd(CMD_GO, 0, 0); }
    else if (!g_page) push_cmd(CMD_RELOAD, 0, 0);
    g_open = 1;
    g_gen++;
}

void browser_close(void) {
    g_open = 0;
    g_dragging = 0;
    g_alert[0] = 0;
    g_gen++;
}

int browser_is_open(void) { return g_open; }

int browser_contains(int mx, int my) {
    return g_open && inside(mx, my, g_x, g_y, WIN_W, WIN_H);
}

uint32_t browser_signature(void) {
    if (!g_open) return 0;
    return g_gen * 2654435761u ^ (uint32_t)(g_x << 16 | g_y) ^ (uint32_t)gui_browser_focused() * 977u;
}

static void tool_button(int x, int y, int w, const char* label, int enabled) {
    uint32_t base = enabled ? 0x00303740u : 0x00262B33u;
    bevel(x, y, w, TOOL_H - 2, base, 0x00535D6Eu, 0x0015191Fu);
    int tw = (int)strlen(label) * 8;
    gfx_draw_text(x + (w - tw) / 2, y + 6, label, enabled ? C_TEXT : 0x00707A88u, base);
}

static void draw_clipped(int x, int y, const char* s, int max_chars, uint32_t fg, uint32_t bg, int tail) {
    char buf[128];
    int n = (int)strlen(s);
    if (max_chars > (int)sizeof(buf) - 1) max_chars = (int)sizeof(buf) - 1;
    if (n <= max_chars) { gfx_draw_text(x, y, s, fg, bg); return; }
    if (tail) {                                     /* keep the end visible (typing) */
        kstrlcpy(buf, s + n - max_chars, sizeof(buf));
        buf[0] = '<';
    } else {
        memcpy(buf, s, (size_t)max_chars);
        buf[max_chars] = 0;
        buf[max_chars - 1] = '>';
    }
    gfx_draw_text(x, y, buf, fg, bg);
}

static void blit_view(int x0, int y0) {
    int stride, tw, th;
    uint32_t* dst = fb_target(&stride, &tw, &th);
    if (!dst || !g_view) return;
    for (int y = 0; y < VIEW_H; y++) {
        int ty = y0 + y;
        if (ty < 0 || ty >= th) continue;
        int xs = x0 < 0 ? -x0 : 0;
        int xe = x0 + VIEW_W > tw ? tw - x0 : VIEW_W;
        if (xe <= xs) continue;
        memcpy(dst + (size_t)ty * (size_t)stride + x0 + xs, g_view + (size_t)y * VIEW_W + xs, (size_t)(xe - xs) * 4);
    }
}

static void sb_geometry(int* thumb_y, int* thumb_h) {
    int track = VIEW_H;
    if (g_page_h <= VIEW_H) { *thumb_y = 0; *thumb_h = track; return; }
    int h = track * VIEW_H / g_page_h;
    if (h < 20) h = 20;
    int range = g_page_h - VIEW_H;
    *thumb_h = h;
    *thumb_y = (track - h) * g_scroll / range;
}

void browser_draw(const fb_info_t* fi) {
    (void)fi;
    if (!g_open) return;
    int x = g_x, y = g_y;
    int focused = gui_browser_focused();
    bevel(x, y, WIN_W, WIN_H, C_PANEL, 0x00505D72u, 0x0010141Cu);
    uint32_t tbg = focused ? C_TITLE : 0x002A3240u;
    bevel(x + 3, y + 3, WIN_W - 6, TITLE_H - 2, tbg, 0x00647692u, 0x00111923u);
    char title[96];
    ksnprintf(title, sizeof(title), "%s - Banana Browser", g_title[0] ? g_title : "New page");
    draw_clipped(x + 10, y + 8, title, (WIN_W - 60) / 8, 0x00FFFFFFu, tbg, 0);
    bevel(x + WIN_W - 28, y + 5, 20, 14, 0x00553333u, 0x00885555u, 0x00221111u);
    gfx_draw_text(x + WIN_W - 22, y + 8, "x", 0x00FFFFFFu, 0x00553333u);

    /* toolbar: < > R H [address] Go */
    int ty = y + TOOL_Y;
    tool_button(x + 4, ty, 26, "<", g_hist_pos > 0);
    tool_button(x + 32, ty, 26, ">", g_hist_pos < g_hist_n - 1);
    tool_button(x + 60, ty, 26, g_loading ? "*" : "R", 1);
    tool_button(x + 88, ty, 32, "Hm", 1);
    uint32_t abg = g_addr_focus ? 0x00FFFFFFu : 0x00E8E8E8u;
    bevel(x + ADDR_X, ty, ADDR_W, TOOL_H - 2, abg, 0x00606060u, 0x00C0C0C0u);
    int maxc = (ADDR_W - 12) / 8;
    draw_clipped(x + ADDR_X + 5, ty + 6, g_addr, maxc, 0x00101010u, abg, g_addr_focus);
    if (g_addr_focus) {
        int n = (int)strlen(g_addr);
        if (n > maxc) n = maxc;
        gfx_fill_rect(x + ADDR_X + 5 + n * 8, ty + 4, 2, 12, 0x00202020u);
    }
    tool_button(x + ADDR_X + ADDR_W + 4, ty, 40, "Go", 1);

    /* page */
    blit_view(x + VIEW_X, y + VIEW_Y);
    /* scrollbar */
    int sx = x + VIEW_X + VIEW_W;
    gfx_fill_rect(sx, y + VIEW_Y, SB_W, VIEW_H, 0x002A3038u);
    int th, tyy;
    sb_geometry(&tyy, &th);
    bevel(sx + 1, y + VIEW_Y + tyy, SB_W - 2, th, 0x00596678u, 0x007A889Cu, 0x00303844u);

    /* status bar */
    int sy = y + WIN_H - STATUS_H - 2;
    gfx_fill_rect(x + 3, sy, WIN_W - 6, STATUS_H, 0x00161B22u);
    draw_clipped(x + 8, sy + 4, g_status, (WIN_W - 20) / 8, g_status_err ? C_ERR : C_DIM, 0x00161B22u, 0);

    /* alert() */
    if (g_alert[0]) {
        int aw = 420, lines = 1;
        for (const char* s = g_alert; *s; s++) lines += *s == '\n';
        if (lines > 8) lines = 8;
        int ah = 60 + lines * 12;
        int ax = x + (WIN_W - aw) / 2, ay = y + 120;
        bevel(ax, ay, aw, ah, 0x00262C36u, 0x00707C90u, 0x000C0F14u);
        gfx_fill_rect(ax + 3, ay + 3, aw - 6, 16, C_TITLE);
        gfx_draw_text(ax + 8, ay + 7, "Message from the page", 0x00FFFFFFu, C_TITLE);
        const char* s = g_alert;
        for (int l = 0; l < lines && *s; l++) {
            char line[64];
            int n = 0;
            while (*s && *s != '\n' && n < 50) line[n++] = *s++;
            line[n] = 0;
            while (*s && *s != '\n') s++;
            if (*s == '\n') s++;
            gfx_draw_text(ax + 12, ay + 28 + l * 12, line, C_TEXT, 0x00262C36u);
        }
        tool_button(ax + aw / 2 - 30, ay + ah - 26, 60, "OK", 1);
    }
}

void browser_click(int mx, int my) {
    if (!browser_contains(mx, my)) return;
    int lx = mx - g_x, ly = my - g_y;
    g_gen++;
    if (g_alert[0]) { g_alert[0] = 0; return; }
    if (ly < TITLE_H + 2) {
        if (lx >= WIN_W - 28 && lx < WIN_W - 8) { browser_close(); return; }
        g_dragging = 1;
        g_drag_dx = lx;
        g_drag_dy = ly;
        return;
    }
    if (ly >= TOOL_Y && ly < TOOL_Y + TOOL_H) {
        g_addr_focus = 0;
        if (lx >= 4 && lx < 30) push_cmd(CMD_BACK, 0, 0);
        else if (lx >= 32 && lx < 58) push_cmd(CMD_FWD, 0, 0);
        else if (lx >= 60 && lx < 86) push_cmd(CMD_RELOAD, 0, 0);
        else if (lx >= 88 && lx < 120) push_cmd(CMD_HOME, 0, 0);
        else if (lx >= ADDR_X && lx < ADDR_X + ADDR_W) g_addr_focus = 1;
        else if (lx >= ADDR_X + ADDR_W + 4 && lx < ADDR_X + ADDR_W + 44) {
            kstrlcpy(g_go_url, g_addr, sizeof(g_go_url));
            push_cmd(CMD_GO, 0, 0);
        }
        return;
    }
    if (inside(lx, ly, VIEW_X + VIEW_W, VIEW_Y, SB_W, VIEW_H)) {
        int th, tyy;
        sb_geometry(&tyy, &th);
        int rel = ly - VIEW_Y;
        if (rel >= tyy && rel < tyy + th) { g_sb_drag = 1; g_sb_drag_dy = rel - tyy; }
        else if (rel < tyy) scroll_by(-(VIEW_H - 40));
        else scroll_by(VIEW_H - 40);
        return;
    }
    if (inside(lx, ly, VIEW_X, VIEW_Y, VIEW_W, VIEW_H)) {
        g_addr_focus = 0;
        push_cmd(CMD_CLICK, lx - VIEW_X, ly - VIEW_Y + g_scroll);
    }
}

void browser_mouse(int mx, int my, int left) {
    if (!left) { g_dragging = 0; g_sb_drag = 0; return; }
    if (g_sb_drag) {
        int th, tyy;
        sb_geometry(&tyy, &th);
        int track = VIEW_H - th;
        if (track > 0 && g_page_h > VIEW_H) {
            int pos = my - g_y - VIEW_Y - g_sb_drag_dy;
            int ns = pos * (g_page_h - VIEW_H) / track;
            if (ns != g_scroll) { g_scroll = ns; g_render_req = 1; }
        }
        return;
    }
    if (!g_dragging) return;
    const fb_info_t* fi = fb_info();
    int nx = mx - g_drag_dx, ny = my - g_drag_dy;
    if (fi && nx + WIN_W > (int)fi->width) nx = (int)fi->width - WIN_W;
    if (fi && ny + WIN_H > (int)fi->height - 28) ny = (int)fi->height - 28 - WIN_H;
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if (nx != g_x || ny != g_y) { g_x = nx; g_y = ny; g_gen++; }
}
