#include "browser.h"
#include "utf8.h"
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
#include "clipboard.h"
#include "winframe.h"
#include "ctxmenu.h"
#include "../net/http.h"
#include "../web/page.h"
#include "../web/render.h"
#include "media.h"

/*
 * The web browser window. Everything that touches a page (loading,
 * scripts, layout, rendering) happens in the "browser" task; the GUI
 * (any task calling gui_poll()) only paints the frame that task left in
 * g_view and queues clicks/commands for it.
 *
 * Tabs: each holds its own page, history, scroll position and status;
 * only the current one is rendered, but every tab's timers keep running.
 */

#define TITLE_H   20
#define TABS_Y    (TITLE_H + 3)
#define TAB_H     20
#define TOOL_Y    (TABS_Y + TAB_H + 2)
#define TOOL_H    22
#define VIEW_X    4
#define VIEW_Y    (TOOL_Y + TOOL_H + 4)
#define STATUS_H  16
#define SB_W      12                    /* scrollbar */
#define ADDR_X    124
#define TAB_W     150
#define MAX_TABS  8

#define HOME_URL  "about:home"
#define HIST_MAX  24
#define URL_MAX   HTTP_URL_MAX        /* addresses can carry kilobytes (challenge tokens) */
#define MAX_DOC   (8u << 20)

#define C_PANEL   0x001D232Cu
#define C_TITLE   0x00384562u
#define C_TEXT    0x00E8EEF6u
#define C_DIM     0x00AAB6C6u
#define C_ERR     0x00F08070u

enum { CMD_NONE = 0, CMD_CLICK, CMD_RCLICK, CMD_GO, CMD_BACK, CMD_FWD, CMD_RELOAD, CMD_HOME,
       CMD_NEWTAB, CMD_CLOSETAB, CMD_PASTE, CMD_DOWNLOAD, CMD_SOURCE, CMD_COPY };

typedef struct { int kind, x, y; } cmd_t;

typedef struct {
    page_t*  page;
    char     addr[URL_MAX];               /* the address bar's text for this tab */
    char     title[128];
    char     status[160];
    int      status_err;
    int      loading;
    int      scroll, page_h;
    char     hist[HIST_MAX][URL_MAX];
    int      hist_n, hist_pos;
} tab_t;

static int        g_open;
static win_geom_t g_win = { .x = 30, .y = 14, .w = 740, .h = 540, .min_w = 360, .min_h = 240 };
static int        g_sb_drag, g_sb_drag_dy;
static uint32_t   g_gen;

static int        g_task = -1;
static uint32_t*  g_view;               /* the current tab's rendered page */
static int        g_vw, g_vh;           /* its size */
static int        g_render_req;

static tab_t*     g_tabs[MAX_TABS];
static int        g_ntabs, g_cur;
static tab_t*     g_status_tab;          /* the tab a load reports to */

static int        g_addr_focus, g_addr_all;  /* address bar focused; its text all selected */
static char       g_go_url[URL_MAX];        /* CMD_GO / CMD_NEWTAB target */
static char       g_alert[256];

/* page text selection, in page coordinates (the task maps it to text) */
static int        g_press, g_selecting, g_sel_on;
static int        g_px, g_py, g_sx, g_sy;

static cmd_t      g_cmds[32];
static int        g_cmd_head, g_cmd_tail;

static page_env_t g_env;
static uint32_t   g_page_mem = 48u << 20;

static int view_w(void) { return g_win.w - 2 * VIEW_X - SB_W; }
static int view_h(void) { return g_win.h - VIEW_Y - STATUS_H - 4; }
static int addr_w(void) { return g_win.w - ADDR_X - 52; }

static tab_t* cur_tab(void) { return g_ntabs ? g_tabs[g_cur] : NULL; }

/* ══ cookies ══════════════════════════════════════════════════════════
 * A jar of name=value per host (or per Domain=), sent back to that host
 * and its subdomains. Cookies with Expires / Max-Age (a site's "remember
 * me", Google's consent choice...) are kept in ~/.config/browser/cookies,
 * so they survive a reboot on an installed system; session cookies are
 * forgotten when the browser closes. */

#define COOKIES     128
#define COOKIE_FILE "/home/banana/.config/browser/cookies"
typedef struct { char domain[96]; char name[64]; char value[1024]; int persist; int httponly; } cookie_t;
static cookie_t g_cookies[COOKIES];
static int g_ncookies;
static int g_cookies_dirty, g_cookies_loaded;

static void url_host(const char* url, char* host, int cap) {
    const char* p = strstr(url, "://");
    p = p ? p + 3 : url;
    int n = 0;
    while (p[n] && p[n] != '/' && p[n] != ':' && p[n] != '?' && p[n] != '#' && n < cap - 1) { host[n] = p[n]; n++; }
    host[n] = 0;
    for (char* c = host; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
}

static int domain_match(const char* host, const char* domain) {
    size_t h = strlen(host), d = strlen(domain);
    if (h < d) return 0;
    if (strcmp(host + h - d, domain) != 0) return 0;
    return h == d || host[h - d - 1] == '.';
}

static void cookie_header(const char* url, char* out, int cap) {
    char host[96];
    url_host(url, host, sizeof(host));
    out[0] = 0;
    int any = 0;
    for (int i = 0; i < g_ncookies; i++) {
        if (!domain_match(host, g_cookies[i].domain)) continue;
        if (!any) kstrlcat(out, "Cookie: ", (size_t)cap);
        else kstrlcat(out, "; ", (size_t)cap);
        kstrlcat(out, g_cookies[i].name, (size_t)cap);
        kstrlcat(out, "=", (size_t)cap);
        kstrlcat(out, g_cookies[i].value, (size_t)cap);
        any = 1;
    }
    if (any) kstrlcat(out, "\r\n", (size_t)cap);
}

/* http.c asks for every request of a redirect chain: a consent page that
 * sets a cookie and redirects back must see it sent on the next hop */
static char g_cookie_hdr[8192];
static const char* headers_for(void* ctx, const char* url) {
    (void)ctx;
    cookie_header(url, g_cookie_hdr, sizeof(g_cookie_hdr));
    return g_cookie_hdr[0] ? g_cookie_hdr : NULL;
}

/* "Wed, 21 Oct 2015 07:28:00 GMT": 1 if that is before now (rough: the
 * RTC keeps local time, a day either way is fine for cookies) */
static int expires_past(const char* s) {
    static const char* const mon[12] = { "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec" };
    while (*s && !k_isdigit(*s)) s++;
    uint32_t day = 0, year = 0;
    s += k_parse_u32(s, &day);
    while (*s == ' ' || *s == '-') s++;
    int m = -1;
    for (int i = 0; i < 12; i++) if (strncasecmp(s, mon[i], 3) == 0) m = i;
    while (*s && !k_isdigit(*s)) s++;
    k_parse_u32(s, &year);
    if (year < 100) year += 2000;
    if (m < 0 || !year) return 0;
    rtc_datetime_t now;
    if (rtc_read_datetime(&now) != 0) return 0;
    uint32_t a = year * 400 + (uint32_t)m * 32 + day, b = now.year * 400u + (now.month - 1u) * 32u + now.day;
    return a < b;
}

/* from_js: document.cookie - it can neither make nor change an HttpOnly cookie */
static void cookie_set(const char* host, const char* line, int from_js) {
    /* name=value; Domain=x; Path=/; Expires=...; Max-Age=...; HttpOnly */
    char name[64], value[1024], domain[96];
    int n = 0;
    while (line[n] && line[n] != '=' && line[n] != ';' && n < 63) { name[n] = line[n]; n++; }
    name[n] = 0;
    while (n && name[n - 1] == ' ') name[--n] = 0;
    if (line[n] != '=' || !name[0]) return;
    const char* v = strchr(line, '=') + 1;
    int vn = 0;
    while (v[vn] && v[vn] != ';' && vn < (int)sizeof(value) - 1) { value[vn] = v[vn]; vn++; }
    value[vn] = 0;
    kstrlcpy(domain, host, sizeof(domain));
    int expired = 0, persist = 0, httponly = 0;
    for (const char* a = strchr(line, ';'); a; a = strchr(a + 1, ';')) {
        const char* s = a + 1;
        while (*s == ' ') s++;
        if (strncasecmp(s, "httponly", 8) == 0 && !from_js) httponly = 1;
        else if (strncasecmp(s, "domain=", 7) == 0) {
            s += 7;
            if (*s == '.') s++;
            int k = 0;
            while (s[k] && s[k] != ';' && k < 95) { domain[k] = (char)(s[k] >= 'A' && s[k] <= 'Z' ? s[k] + 32 : s[k]); k++; }
            domain[k] = 0;
            if (!domain_match(host, domain)) return;          /* not for another site */
        } else if (strncasecmp(s, "max-age=", 8) == 0) {
            if (s[8] == '0' || s[8] == '-') expired = 1;
            else persist = 1;
        } else if (strncasecmp(s, "expires=", 8) == 0) {
            if (expires_past(s + 8)) expired = 1;
            else persist = 1;
        }
    }
    g_cookies_dirty = 1;
    for (int i = 0; i < g_ncookies; i++) {
        if (strcmp(g_cookies[i].name, name) == 0 && strcmp(g_cookies[i].domain, domain) == 0) {
            if (from_js && g_cookies[i].httponly) return;
            if (expired) { g_cookies[i] = g_cookies[--g_ncookies]; return; }
            kstrlcpy(g_cookies[i].value, value, sizeof(g_cookies[i].value));
            g_cookies[i].persist = persist;
            g_cookies[i].httponly = httponly;
            return;
        }
    }
    if (expired) return;
    if (g_ncookies == COOKIES) { memmove(&g_cookies[0], &g_cookies[1], sizeof(cookie_t) * (COOKIES - 1)); g_ncookies--; }
    cookie_t* c = &g_cookies[g_ncookies++];
    kstrlcpy(c->domain, domain, sizeof(c->domain));
    kstrlcpy(c->name, name, sizeof(c->name));
    kstrlcpy(c->value, value, sizeof(c->value));
    c->persist = persist;
    c->httponly = httponly;
}

/* document.cookie: what the page's scripts may read */
static void env_cookie_get(void* ctx, const char* url, char* out, int cap) {
    (void)ctx;
    char host[96];
    url_host(url, host, sizeof(host));
    out[0] = 0;
    for (int i = 0; i < g_ncookies; i++) {
        if (g_cookies[i].httponly || !domain_match(host, g_cookies[i].domain)) continue;
        if (out[0]) kstrlcat(out, "; ", (size_t)cap);
        kstrlcat(out, g_cookies[i].name, (size_t)cap);
        kstrlcat(out, "=", (size_t)cap);
        kstrlcat(out, g_cookies[i].value, (size_t)cap);
    }
}

static void env_cookie_set(void* ctx, const char* url, const char* line) {
    (void)ctx;
    char host[96];
    url_host(url, host, sizeof(host));
    if (host[0]) cookie_set(host, line, 1);
}

/* the persistent cookies, one per line: domain TAB name TAB value */
static void cookies_save(void) {
    if (!g_cookies_dirty) return;
    g_cookies_dirty = 0;
    uint32_t cap = 256, n = 0;
    for (int i = 0; i < g_ncookies; i++) cap += (uint32_t)(strlen(g_cookies[i].domain) + strlen(g_cookies[i].name) + strlen(g_cookies[i].value) + 14);
    char* buf = (char*)kmalloc(cap);
    if (!buf) return;
    buf[0] = 0;
    for (int i = 0; i < g_ncookies; i++) {
        if (!g_cookies[i].persist) continue;
        n += (uint32_t)ksnprintf(buf + n, cap - n, "%s%s\t%s\t%s\n", g_cookies[i].httponly ? "#HttpOnly_" : "",
                                 g_cookies[i].domain, g_cookies[i].name, g_cookies[i].value);
    }
    fs_mkdir_p("/home/banana/.config/browser");
    fs_write_path(COOKIE_FILE, buf, n);
    kfree(buf);
}

static const char* tab_in(const char* s, char c, size_t n) {
    for (size_t i = 0; i < n; i++) if (s[i] == c) return s + i;
    return NULL;
}

static void cookies_load(void) {
    if (g_cookies_loaded) return;
    g_cookies_loaded = 1;
    int fi = fs_find_file(COOKIE_FILE);
    if (fi < 0) return;
    fs_file_t* f = fs_get_file(fi);
    if (!f) return;
    const char* p = f->content;
    while (*p && g_ncookies < COOKIES) {
        const char* e = strchr(p, '\n');
        if (!e) e = p + strlen(p);
        const char* t1 = tab_in(p, '\t', (size_t)(e - p));
        const char* t2 = t1 ? tab_in(t1 + 1, '\t', (size_t)(e - t1 - 1)) : NULL;
        if (t1 && t2) {
            cookie_t* c = &g_cookies[g_ncookies++];
            memset(c, 0, sizeof(*c));
            int dl = (int)(t1 - p), nl = (int)(t2 - t1 - 1), vl = (int)(e - t2 - 1);
            if (dl > 95) dl = 95;
            if (nl > 63) nl = 63;
            if (vl > 1023) vl = 1023;
            memcpy(c->domain, p, (size_t)dl);
            memcpy(c->name, t1 + 1, (size_t)nl);
            memcpy(c->value, t2 + 1, (size_t)vl);
            c->persist = 1;
            if (strncmp(c->domain, "#HttpOnly_", 10) == 0) {
                memmove(c->domain, c->domain + 10, strlen(c->domain + 10) + 1);
                c->httponly = 1;
            }
        }
        p = *e ? e + 1 : e;
    }
}

/* ══ downloads ════════════════════════════════════════════════════════
 * A page load whose answer is not something to show (a program, an
 * archive, a .bpk, "Content-Disposition: attachment") is saved to
 * ~/Downloads instead; "Save link as" / "Save page as" always save. */

#define DOWNLOAD_DIR "/home/banana/Downloads"

typedef struct {
    int      force;          /* save whatever comes */
    int      active;         /* the answer turned out to be a download */
    char     name[96];       /* from Content-Disposition */
    uint32_t reported;       /* bytes at the last progress message */
} dl_t;

typedef struct { char* buf; uint32_t n, cap; int too_big; uint32_t limit; dl_t* dl; } body_t;

static void set_status(const char* s, int err);

static int displayable(const char* ct) {
    if (!ct[0]) return 1;
    static const char* const show[] = {
        "text/", "image/", "application/xhtml", "application/json", "application/javascript",
        "application/x-javascript", "application/xml", "application/rss", "application/atom", "application/ld+json",
    };
    for (unsigned i = 0; i < sizeof(show) / sizeof(show[0]); i++)
        if (strncasecmp(ct, show[i], strlen(show[i])) == 0) return 1;
    return 0;
}

/* files (by their name) that are downloaded whatever the server calls
 * them: videos, sound, archives, documents, programs, disk images */
static int download_ext(const char* url) {
    static const char* const EXT[] = {
        "mp4", "m4v", "mkv", "webm", "avi", "mov", "wmv", "flv", "mpg", "mpeg", "ts", "3gp", "ogv",
        "mp3", "m4a", "aac", "flac", "ogg", "oga", "opus", "wav", "wma",
        "zip", "7z", "rar", "gz", "tgz", "bz2", "xz", "tar", "iso", "img", "dmg",
        "pdf", "doc", "docx", "xls", "xlsx", "ppt", "pptx", "odt", "ods", "epub",
        "exe", "msi", "deb", "rpm", "apk", "bin", "bpk",
    };
    const char* p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char* path = strchr(p, '/');
    if (!path) return 0;
    const char* end = path;
    while (*end && *end != '?' && *end != '#') end++;
    const char* dot = NULL;
    for (const char* q = path; q < end; q++) { if (*q == '.') dot = q; if (*q == '/') dot = NULL; }
    if (!dot) return 0;
    int n = (int)(end - dot - 1);
    for (unsigned i = 0; i < sizeof(EXT) / sizeof(EXT[0]); i++)
        if ((int)strlen(EXT[i]) == n && strncasecmp(dot + 1, EXT[i], (size_t)n) == 0) return 1;
    return 0;
}

/* filename="x" (or filename=x) from a Content-Disposition header, if any */
static int disposition(const char* raw, char* name, int cap, int* attachment) {
    *attachment = 0;
    name[0] = 0;
    for (const char* l = raw; l && *l; ) {
        const char* e = strchr(l, '\n');
        if (strncasecmp(l, "Content-Disposition:", 20) == 0) {
            const char* v = l + 20;
            const char* end = e ? e : v + strlen(v);
            while (*v == ' ') v++;
            if (strncasecmp(v, "attachment", 10) == 0) *attachment = 1;
            for (const char* p = v; p < end; p++) {
                if (strncasecmp(p, "filename=", 9) != 0) continue;
                p += 9;
                int q = *p == '"';
                if (q) p++;
                int n = 0;
                while (p < end && *p != '\r' && (q ? *p != '"' : *p != ';') && n < cap - 1) name[n++] = *p++;
                name[n] = 0;
                break;
            }
            return 1;
        }
        l = e ? e + 1 : NULL;
    }
    return 0;
}

/* every response's headers (redirects included): keep its cookies */
static void on_headers(void* ctx, const http_response_t* r, const char* raw) {
    body_t* b = (body_t*)ctx;
    if (b && b->dl && r->status >= 200 && r->status < 300) {
        int attach;
        char fname[96];
        disposition(raw, fname, sizeof(fname), &attach);
        if (b->dl->force || attach || !displayable(r->content_type) || download_ext(r->final_url)) {
            b->dl->active = 1;
            b->limit = FS_MAX_FILE_SIZE;
            if (fname[0]) kstrlcpy(b->dl->name, fname, sizeof(b->dl->name));
        }
    }
    char host[96];
    url_host(r->final_url, host, sizeof(host));
    for (const char* l = raw; l && *l; ) {
        const char* e = strchr(l, '\n');
        if (strncasecmp(l, "Set-Cookie:", 11) == 0) {
            char line[2048];
            const char* v = l + 11;
            while (*v == ' ') v++;
            int n = 0;
            while (v + n < (e ? e : v + strlen(v)) && v[n] != '\r' && n < 2047) { line[n] = v[n]; n++; }
            line[n] = 0;
            cookie_set(host, line, 0);
        }
        l = e ? e + 1 : NULL;
    }
}


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
    int next = (g_cmd_head + 1) % 32;
    if (next == g_cmd_tail) return;           /* full: drop */
    g_cmds[g_cmd_head].kind = kind;
    g_cmds[g_cmd_head].x = x;
    g_cmds[g_cmd_head].y = y;
    g_cmd_head = next;
}

static int g_quiet;                         /* a web view is fetching: not the browser's business */

static void set_status(const char* s, int err) {
    if (g_quiet) return;
    tab_t* t = g_status_tab ? g_status_tab : cur_tab();
    if (!t) return;
    kstrlcpy(t->status, s, sizeof(t->status));
    t->status_err = err;
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
    "<div class=\"card\"><h2>Keys</h2>Ctrl+L: address bar - Ctrl+N: new tab - Ctrl+W: close tab - "
    "Backspace: back - arrows, space, PgUp, PgDn: scroll - Ctrl+R: reload - drag over text to select it, "
    "Ctrl+C: copy - Ctrl+V: paste - right-click: a menu (open a link in a new tab, save a link, back, reload, page source...) - files that are not web pages (apps, archives...) are saved to ~/Downloads</div>\n"
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
    static int idx[FS_MAX_FILES > FS_MAX_DIRS ? FS_MAX_FILES : FS_MAX_DIRS];   /* (too big for a task's stack) */
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
        fs_file_t* f = fs_file_info(idx[i]);
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

static int on_body(void* ctx, const uint8_t* d, uint32_t n) {
    body_t* b = (body_t*)ctx;
    if (b->n + n + 1 > b->limit) { b->too_big = 1; return -1; }
    if (b->dl && b->dl->active && b->n + n - b->dl->reported >= (256u << 10)) {
        char msg[96];
        b->dl->reported = b->n + n;
        ksnprintf(msg, sizeof(msg), "Downloading... %u.%u MB", (b->n + n) >> 20, (((b->n + n) >> 10) & 1023) * 10 / 1024);
        set_status(msg, 0);
    }
    if (b->n + n + 1 > b->cap) {
        uint32_t cap = (b->n + n + 1) * 2;
        if (cap > b->limit) cap = b->limit;
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

/* GET (or POST with a form body) url into a kmalloc'd buffer; 0 = ok */
/* method NULL: GET, or POST when there is a body (body_type NULL: a form) */
/* dl: a top-level load (may turn into a download), or NULL */
static int fetch_url(const char* url, const char* method, const char* post, uint32_t post_len, const char* body_type,
                     char** data, uint32_t* len, char* ctype, int ccap, char* final_url, int fcap, char* err, int ecap,
                     dl_t* dl) {
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
    body_t b = { 0, 0, 0, 0, (dl && dl->force) ? FS_MAX_FILE_SIZE : MAX_DOC, dl };
    http_request_t req;
    memset(&req, 0, sizeof(req));
    req.method = method ? method : post ? "POST" : "GET";
    req.body = post;
    req.body_len = post_len;
    if (post) req.content_type = body_type ? body_type : "application/x-www-form-urlencoded";
    req.headers_for = headers_for;              /* cookies, recomputed for every redirect hop */
    req.on_headers = on_headers;
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
        if (b.too_big) kstrlcpy(err, b.limit > MAX_DOC ? "the file is too big (32 MiB max)" : "the page is too big (8 MiB max)", (size_t)ecap);
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

static int env_fetch(void* ctx, const char* url, char** data, uint32_t* len, char* ctype, int ccap,
                     char* final_url, int fcap, char* err, int ecap) {
    (void)ctx;
    return fetch_url(url, NULL, NULL, 0, NULL, data, len, ctype, ccap, final_url, fcap, err, ecap, NULL);
}

/* fetch() / XMLHttpRequest with a method or a body */
static int env_request(void* ctx, const char* url, const char* method, const char* body, uint32_t blen,
                       const char* btype, char** data, uint32_t* len, char* rtype, int rcap, char* err, int ecap) {
    (void)ctx;
    char fin[URL_MAX];
    return fetch_url(url, method, body ? body : "", body ? blen : 0, btype, data, len, rtype, rcap, fin, sizeof(fin),
                     err, ecap, NULL);
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


/* ══ the browser task ═════════════════════════════════════════════════ */

/* the frame buffer follows the window's size */
static void ensure_view(void) {
    int w = view_w(), h = view_h();
    if (w == g_vw && h == g_vh && g_view) return;
    uint32_t* nv = (uint32_t*)kmalloc((uint32_t)w * (uint32_t)h * 4);
    if (!nv) return;
    memset32(nv, 0xFFFFFF, (size_t)w * (size_t)h);
    uint32_t* old = g_view;
    g_view = nv;                        /* pointer and size change together (no yield here) */
    g_vw = w;
    g_vh = h;
    if (old) kfree(old);
    g_render_req = 1;
}

/* the current text selection, mapped onto the layout's text runs */
static int map_selection(layout_t* L) {
    L->sel_on = 0;
    if (!g_sel_on) return 0;
    int i0, o0, i1, o1;
    if (!layout_text_pos(L, g_px, g_py, &i0, &o0) || !layout_text_pos(L, g_sx, g_sy, &i1, &o1)) return 0;
    if (i0 == i1 && o0 == o1) return 0;
    L->sel_on = 1;
    L->sel_i0 = i0; L->sel_o0 = o0; L->sel_i1 = i1; L->sel_o1 = o1;
    return 1;
}

static void render_view(void) {
    ensure_view();
    if (!g_view) return;
    tab_t* t = cur_tab();
    page_t* p = t ? t->page : NULL;
    if (p && !t->loading) page_update(p, g_vw);
    layout_t* L = p && !t->loading ? p->layout : NULL;
    if (t) {
        t->page_h = L ? L->height : 0;
        int max = t->page_h - g_vh;
        if (t->scroll > max) t->scroll = max;
        if (t->scroll < 0) t->scroll = 0;
    }
    if (L) map_selection(L);
    render_page(L, g_view, g_vw, g_vw, g_vh, 0, 0, g_vw, g_vh, t ? t->scroll : 0);
    g_render_req = 0;
    g_gen++;
}

static void copy_selection(void) {
    tab_t* t = cur_tab();
    if (!t || !t->page || !t->page->layout) return;
    layout_t* L = t->page->layout;
    if (!map_selection(L)) return;
    char* buf = (char*)kmalloc(CLIPBOARD_MAX);
    if (!buf) return;
    uint32_t n = layout_text_range(L, L->sel_i0, L->sel_o0, L->sel_i1, L->sel_o1, buf, CLIPBOARD_MAX);
    clipboard_set(buf, n);
    kfree(buf);
    set_status("Copied", 0);
}

/* words typed in the address bar (not an address) are searched for:
 * DuckDuckGo's HTML version works without scripts (Google's results page
 * now needs its anti-bot script to pass, which this browser does not) */
#define SEARCH_URL "https://html.duckduckgo.com/html/?q="
static int looks_like_address(const char* s) {
    if (strchr(s, ' ')) return 0;
    if (strncasecmp(s, "localhost", 9) == 0) return 1;
    const char* dot = strchr(s, '.');
    return dot && dot != s && dot[1] && dot[1] != '/';
}

static void normalize_url(const char* in, char* out, int cap) {
    while (*in == ' ') in++;
    if (!strstr(in, "://") && strncasecmp(in, "about:", 6) != 0 && in[0] != '/' && in[0] && !looks_like_address(in)) {
        static const char hx[] = "0123456789ABCDEF";
        kstrlcpy(out, SEARCH_URL, (size_t)cap);
        int n = (int)strlen(out);
        for (const unsigned char* s = (const unsigned char*)in; *s && n < cap - 4; s++) {
            if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '-' || *s == '.' || *s == '_') out[n++] = (char)*s;
            else if (*s == ' ') out[n++] = '+';
            else { out[n++] = '%'; out[n++] = hx[*s >> 4]; out[n++] = hx[*s & 15]; }
        }
        while (n > 0 && out[n - 1] == '+') n--;
        out[n] = 0;
        return;
    }
    if (strstr(in, "://") || strncasecmp(in, "about:", 6) == 0) kstrlcpy(out, in, (size_t)cap);
    else if (in[0] == '/') ksnprintf(out, (size_t)cap, "file://%s", in);
    else ksnprintf(out, (size_t)cap, "http://%s", in);
    int n = (int)strlen(out);
    while (n > 0 && out[n - 1] == ' ') out[--n] = 0;
}

static int starts_ci(const char* s, const char* p) { return strncasecmp(s, p, strlen(p)) == 0; }

/* plain text and pictures get a little HTML around them */
static char* wrap_content(const char* url, const char* ctype, char* data, uint32_t* len) {
    if (!ctype[0] || starts_ci(ctype, "text/html") || starts_ci(ctype, "application/xhtml")) return data;
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

/* a file name for a download: Content-Disposition's, else the URL's last part */
static void download_name(const char* url, const char* hint, const char* ctype, char* out, int cap) {
    char name[128];
    name[0] = 0;
    if (hint && hint[0]) {
        const char* s = strrchr(hint, '/');
        kstrlcpy(name, s ? s + 1 : hint, sizeof(name));
    } else {
        const char* p = strstr(url, "://");
        p = p ? p + 3 : url;
        const char* path = strchr(p, '/');
        const char* last = path ? path : "";
        for (const char* q = last; *q && *q != '?' && *q != '#'; q++) if (*q == '/') last = q + 1;
        int n = 0;
        for (const char* q = last; *q && *q != '?' && *q != '#' && n < 127; q++) {
            if (*q == '%' && q[1] && q[2]) {
                int hi = q[1] <= '9' ? q[1] - '0' : (q[1] | 32) - 'a' + 10;
                int lo = q[2] <= '9' ? q[2] - '0' : (q[2] | 32) - 'a' + 10;
                name[n++] = (char)(hi * 16 + lo);
                q += 2;
            } else {
                name[n++] = *q;
            }
        }
        name[n] = 0;
        if (!name[0]) {                    /* a site's front page */
            url_host(url, name, 100);
            kstrlcat(name, strncasecmp(ctype, "text/html", 9) == 0 ? ".html" : "", sizeof(name));
        }
    }
    for (char* c = name; *c; c++)
        if ((unsigned char)*c < 32 || strchr("/\\:*?\"<>|", *c)) *c = '_';
    if (!name[0]) kstrlcpy(name, "download", sizeof(name));
    /* the filesystem keeps 31 characters: keep the extension */
    int l = (int)strlen(name);
    if (l >= FS_NAME_LEN) {
        const char* dot = strrchr(name, '.');
        int el = dot ? (int)strlen(dot) : 0;
        if (el > 8) el = 0;
        char t[FS_NAME_LEN];
        int keep = FS_NAME_LEN - 1 - el;
        memcpy(t, name, (size_t)keep);
        if (el) memcpy(t + keep, dot, (size_t)el);
        t[keep + el] = 0;
        kstrlcpy(name, t, sizeof(name));
    }
    kstrlcpy(out, name, (size_t)cap);
}

static void save_download(const char* url, const char* hint, const char* ctype, const char* data, uint32_t len) {
    char name[FS_NAME_LEN], path[FS_PATH_LEN], msg[160];
    download_name(url, hint, ctype, name, sizeof(name));
    fs_mkdir_p(DOWNLOAD_DIR);
    ksnprintf(path, sizeof(path), "%s/%s", DOWNLOAD_DIR, name);
    for (int n = 2; fs_find_file(path) >= 0 && n < 100; n++) {
        /* "name (2).ext" */
        char base[FS_NAME_LEN];
        kstrlcpy(base, name, sizeof(base));
        char* dot = strrchr(base, '.');
        char ext[12] = "";
        if (dot && strlen(dot) < sizeof(ext)) { kstrlcpy(ext, dot, sizeof(ext)); *dot = 0; }
        base[FS_NAME_LEN - 8 - strlen(ext)] = 0;
        ksnprintf(path, sizeof(path), "%s/%s (%d)%s", DOWNLOAD_DIR, base, n, ext);
    }
    const char* leaf = strrchr(path, '/') + 1;
    if (fs_write_path(path, data, len) < 0) {
        ksnprintf(msg, sizeof(msg), "Could not save %s (out of space?)", leaf);
        set_status(msg, 1);
        return;
    }
    const char* tail = "";
    const char* dot = strrchr(leaf, '.');
    if (dot && strcasecmp(dot, ".bpk") == 0) tail = " - an app: install it from Files (right-click)";
    else if (dot && strcasecmp(dot, ".wav") == 0) tail = " - play it from Files";
    else if (dot && download_ext(leaf - 1 > path ? path : leaf)) tail = " - open it from Files";
    ksnprintf(msg, sizeof(msg), "Downloaded %s (%u KB) to ~/Downloads%s", leaf, (len + 1023) / 1024, tail);
    set_status(msg, 0);
}

/* "Save link as" / "Save page as": always a download, the page stays */
static void download(tab_t* t, const char* url_in) {
    char url[URL_MAX], final_url[URL_MAX], ctype[96], err[200], msg[200];
    normalize_url(url_in, url, sizeof(url));
    g_status_tab = t;
    ksnprintf(msg, sizeof(msg), "Downloading %s ...", url);
    set_status(msg, 0);
    dl_t dl;
    memset(&dl, 0, sizeof(dl));
    dl.force = 1;
    char* data = NULL;
    uint32_t len = 0;
    if (fetch_url(url, NULL, NULL, 0, NULL, &data, &len, ctype, sizeof(ctype), final_url, sizeof(final_url), err,
                  sizeof(err), &dl) != 0) {
        ksnprintf(msg, sizeof(msg), "Download failed: %s", err[0] ? err : "error");
        set_status(msg, 1);
    } else {
        save_download(final_url, dl.name, ctype, data, len);
        kfree(data);
    }
    g_status_tab = NULL;
}

static void load(tab_t* t, const char* url_in, const char* post, uint32_t post_len, int add_history) {
    char url[URL_MAX], final_url[URL_MAX], ctype[96], err[200];
    normalize_url(url_in, url, sizeof(url));
    char prev_addr[URL_MAX];
    kstrlcpy(prev_addr, t->page ? t->page->url : "", sizeof(prev_addr));
    kstrlcpy(t->addr, url, sizeof(t->addr));
    if (t == cur_tab()) { g_addr_focus = 0; g_sel_on = 0; }
    t->loading = 1;
    g_status_tab = t;
    set_status("Loading...", 0);

    char* data = NULL;
    uint32_t len = 0;
    dl_t dl;
    memset(&dl, 0, sizeof(dl));
    /* view-source:URL shows the page's text */
    int vsrc = strncmp(url, "view-source:", 12) == 0;
    int frc = fetch_url(vsrc ? url + 12 : url, NULL, post, post_len, NULL, &data, &len, ctype, sizeof(ctype), final_url,
                        sizeof(final_url), err, sizeof(err), vsrc ? NULL : &dl);
    if (frc == 0 && vsrc) {
        char fin[URL_MAX];
        ksnprintf(fin, sizeof(fin), "view-source:%s", final_url);
        kstrlcpy(final_url, fin, sizeof(final_url));
        kstrlcpy(ctype, "text/plain", sizeof(ctype));
    }
    /* a binary answer the server called text (or nothing): a file, not a page */
    if (frc == 0 && !vsrc && !dl.active && len && strncasecmp(ctype, "image/", 6) != 0) {
        uint32_t n = len < 1024 ? len : 1024;
        for (uint32_t i = 0; i < n; i++) if (!data[i]) { dl.active = 1; break; }
    }
    if (frc == 0 && dl.active) {
        /* not a page: save it, and stay on the page we were on */
        save_download(final_url, dl.name, ctype, data, len);
        kfree(data);
        if (prev_addr[0]) kstrlcpy(t->addr, prev_addr, sizeof(t->addr));
        t->loading = 0;
        g_status_tab = NULL;
        g_render_req = 1;
        g_gen++;
        return;
    }
    if (frc != 0) {
        data = error_page(url, err[0] ? err : "the page could not be loaded", &len);
        kstrlcpy(final_url, url, sizeof(final_url));
        kstrlcpy(ctype, "text/html", sizeof(ctype));
        set_status(err[0] ? err : "Error", 1);
    }
    if (data) data = wrap_content(final_url, ctype, data, &len);
    if (!data) { set_status("out of memory", 1); t->loading = 0; g_status_tab = NULL; return; }

    if (t->page) { page_free(t->page); t->page = NULL; }
    if (t == cur_tab()) render_view();      /* blank while the new page loads */
    page_t* p = page_new(&g_env, g_page_mem);
    if (!p) { kfree(data); set_status("out of memory", 1); t->loading = 0; g_status_tab = NULL; return; }
    p->view_h = g_vh;
    uint32_t t0 = timer_ms();
    page_load(p, final_url, data, len, g_vw);
    kfree(data);
    t->page = p;
    t->scroll = p->scroll_req > 0 ? p->scroll_req : 0;
    p->scroll_req = -1;
    kstrlcpy(t->addr, final_url, sizeof(t->addr));
    kstrlcpy(t->title, p->title[0] ? p->title : final_url, sizeof(t->title));

    if (add_history) {
        if (t->hist_pos < t->hist_n - 1) t->hist_n = t->hist_pos + 1;   /* drop the forward part */
        if (t->hist_n == HIST_MAX) {
            for (int i = 1; i < HIST_MAX; i++) kstrlcpy(t->hist[i - 1], t->hist[i], sizeof(t->hist[0]));
            t->hist_n--;
        }
        kstrlcpy(t->hist[t->hist_n], final_url, sizeof(t->hist[0]));
        t->hist_pos = t->hist_n++;
    } else if (t->hist_pos >= 0) {
        kstrlcpy(t->hist[t->hist_pos], final_url, sizeof(t->hist[0]));
    }

    if (!t->status_err) {
        char msg[160];
        if (p->status[0]) ksnprintf(msg, sizeof(msg), "Script error: %s", p->status);
        else ksnprintf(msg, sizeof(msg), "Done (%u ms)%s", timer_ms() - t0, p->A.oom ? " - the page ran out of memory" : "");
        set_status(msg, p->status[0] != 0);
    }
    p->status[0] = 0;
    t->loading = 0;
    g_status_tab = NULL;
    if (t == cur_tab()) render_view();
    g_gen++;
}

static void new_tab(const char* url) {
    if (g_ntabs == MAX_TABS) { set_status("Too many tabs (8 at most)", 1); return; }
    tab_t* t = (tab_t*)kzalloc(sizeof(tab_t));
    if (!t) return;
    t->hist_pos = -1;
    int at = g_ntabs ? g_cur + 1 : 0;
    for (int i = g_ntabs; i > at; i--) g_tabs[i] = g_tabs[i - 1];
    g_tabs[at] = t;
    g_ntabs++;
    g_cur = at;
    g_sel_on = 0;
    load(t, url && *url ? url : HOME_URL, NULL, 0, 1);
}

static void close_tab(int i) {
    if (i < 0 || i >= g_ntabs) return;
    tab_t* t = g_tabs[i];
    if (t->page) page_free(t->page);
    kfree(t);
    for (int k = i; k < g_ntabs - 1; k++) g_tabs[k] = g_tabs[k + 1];
    g_ntabs--;
    if (g_cur >= g_ntabs) g_cur = g_ntabs - 1;
    if (g_cur < 0) g_cur = 0;
    g_sel_on = 0;
    g_render_req = 1;
    g_gen++;
    if (!g_ntabs) g_open = 0;                 /* the last tab closes the window */
}

/* after scripts or a click: navigation, alerts, titles, relayout */
static void after_page_event(tab_t* t) {
    page_t* p = t->page;
    if (!p) return;
    if (p->alert_pending) {
        if (t == cur_tab()) kstrlcpy(g_alert, p->alert, sizeof(g_alert));
        p->alert_pending = 0;
        g_gen++;
    }
    if (p->status[0]) {
        g_status_tab = t;
        char msg[200];
        ksnprintf(msg, sizeof(msg), "Script error: %s", p->status);
        set_status(msg, 1);
        g_status_tab = NULL;
        p->status[0] = 0;
    }
    if (p->title[0] && strcmp(p->title, t->title) != 0) {
        kstrlcpy(t->title, p->title, sizeof(t->title));
        g_gen++;
    }
    if (p->nav_pending) {
        p->nav_pending = 0;
        char nav[URL_MAX];
        kstrlcpy(nav, p->nav, sizeof(nav));
        if (p->nav_newtab) { p->nav_newtab = 0; new_tab(nav); return; }
        if (p->nav_post) {
            /* the body lives in the page's arena, which the load frees */
            uint32_t n = p->nav_post_len;
            char* body = (char*)kmalloc(n + 1);
            if (body) {
                memcpy(body, p->nav_post, n);
                body[n] = 0;
                load(t, nav, body, n, 1);
                kfree(body);
            }
            return;
        }
        load(t, nav, NULL, 0, 1);
        return;
    }
    if (t != cur_tab()) return;               /* hidden tabs lay out when shown */
    int changed = p->dirty;
    page_update(p, g_vw);
    if (p->scroll_req >= 0) {
        t->scroll = p->scroll_req;
        p->scroll_req = -1;
        changed = 1;
    }
    if (changed) g_render_req = 1;
}

static void scroll_by(int dy) {
    tab_t* t = cur_tab();
    if (!t) return;
    t->scroll += dy;
    g_render_req = 1;
}

static void history_go(int delta) {
    tab_t* t = cur_tab();
    if (!t) return;
    int np = t->hist_pos + delta;
    if (np < 0 || np >= t->hist_n) return;
    t->hist_pos = np;
    char url[URL_MAX];
    kstrlcpy(url, t->hist[np], sizeof(url));
    load(t, url, NULL, 0, 0);
}

static void paste_now(void) {
    uint32_t n;
    const char* clip = clipboard_get(&n);
    tab_t* t = cur_tab();
    if (!n || !t) return;
    if (g_addr_focus) {
        if (g_addr_all) { t->addr[0] = 0; g_addr_all = 0; }
        size_t l = strlen(t->addr);
        for (uint32_t i = 0; i < n && l < sizeof(t->addr) - 1; i++)
            if ((unsigned char)clip[i] >= 32) t->addr[l++] = clip[i];
        t->addr[l] = 0;
        g_gen++;
        return;
    }
    if (t->page && t->page->focus) {
        for (uint32_t i = 0; i < n; i++) {
            char c = clip[i];
            if (c == '\r') continue;
            if (c == '\n' && strcmp(t->page->focus->tag, "textarea") != 0) c = ' ';
            page_key(t->page, c);
        }
        after_page_event(t);
        return;
    }
    set_status("Click a text field first, then paste", 0);
}

static void handle_key(char c) {
    tab_t* t = cur_tab();
    if (!t) return;
    if (c == 12) { g_addr_focus = 1; g_addr_all = 1; g_gen++; return; }   /* Ctrl+L */
    if (c == 18) { push_cmd(CMD_RELOAD, 0, 0); return; }                    /* Ctrl+R */
    if (c == 14) { g_go_url[0] = 0; push_cmd(CMD_NEWTAB, 0, 0); return; }  /* Ctrl+N: new tab */
    if (c == 23) { push_cmd(CMD_CLOSETAB, g_cur, 0); return; }              /* Ctrl+W */
    if (c == 22) { paste_now(); return; }                                    /* Ctrl+V */
    if (c == 3) {                                                            /* Ctrl+C */
        if (g_addr_focus) { clipboard_set(t->addr, (uint32_t)strlen(t->addr)); set_status("Address copied", 0); }
        else copy_selection();
        return;
    }
    if (g_alert[0]) {
        if (c == '\n' || c == 27 || c == ' ') { g_alert[0] = 0; g_gen++; }
        return;
    }
    if (g_addr_focus) {
        if (c == 1) { g_addr_all = 1; g_gen++; return; }                     /* Ctrl+A */
        size_t n = strlen(t->addr);
        if (c == '\n') { kstrlcpy(g_go_url, t->addr, sizeof(g_go_url)); push_cmd(CMD_GO, 0, 0); }
        else if (c == '\b') { if (g_addr_all) t->addr[0] = 0; else if (n) u8_backspace(t->addr, (int)n); }
        else if (c == 27) { g_addr_focus = 0; if (t->page) kstrlcpy(t->addr, t->page->url, sizeof(t->addr)); }
        else if ((unsigned char)c >= 32) {
            if (g_addr_all) { t->addr[0] = 0; n = 0; }
            if (n < sizeof(t->addr) - 1) { t->addr[n] = c; t->addr[n + 1] = 0; }
        }
        g_addr_all = 0;
        g_gen++;
        return;
    }
    if (t->page && !t->loading && page_key(t->page, c)) { after_page_event(t); return; }
    if (c == ' ') scroll_by(g_vh - 40);
    else if (c == '\b') history_go(-1);
}

static void handle_arrow(char a) {
    if (g_addr_focus) return;
    if (a == 'A') scroll_by(-40);
    else if (a == 'B') scroll_by(40);
    else if (a == 'I' || a == '5') scroll_by(-(g_vh - 40));   /* PgUp: ESC [ I (ESC [ 5 ~) */
    else if (a == 'G' || a == '6') scroll_by(g_vh - 40);
    else if (a == 'H') { if (cur_tab()) cur_tab()->scroll = 0; g_render_req = 1; }
    else if (a == 'F') { if (cur_tab()) cur_tab()->scroll = 1 << 28; g_render_req = 1; }
}

static void read_keys(void) {
    if (!gui_browser_focused()) return;
    for (int k = 0; k < 64; k++) {
        char c = keyboard_try_getchar();
        if (!c) return;
        if (c == 27) {
            char c2 = keyboard_try_getchar();
            if (c2 == '[') {
                char c3 = keyboard_try_getchar();
                if (c3 >= '0' && c3 <= '9') keyboard_try_getchar();   /* '~' */
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

/* the right-click menu: chosen in the desktop's task, carried out here */
enum { BM_OPEN = 1, BM_NEWTAB, BM_SAVELINK, BM_COPYLINK, BM_BACK, BM_FWD, BM_RELOAD, BM_COPY, BM_PASTE,
       BM_SAVEPAGE, BM_SOURCE, BM_DOWNLOADS };
static char g_menu_link[URL_MAX];
static char g_dl_url[URL_MAX];
static int  g_rc_mx, g_rc_my;

static void bmenu_cb(int id, void* arg) {
    (void)arg;
    tab_t* t = cur_tab();
    switch (id) {
    case BM_OPEN: kstrlcpy(g_go_url, g_menu_link, sizeof(g_go_url)); push_cmd(CMD_GO, 0, 0); break;
    case BM_NEWTAB: kstrlcpy(g_go_url, g_menu_link, sizeof(g_go_url)); push_cmd(CMD_NEWTAB, 0, 0); break;
    case BM_SAVELINK: kstrlcpy(g_dl_url, g_menu_link, sizeof(g_dl_url)); push_cmd(CMD_DOWNLOAD, 0, 0); break;
    case BM_COPYLINK: clipboard_set(g_menu_link, (uint32_t)strlen(g_menu_link)); set_status("Link copied", 0); break;
    case BM_BACK: push_cmd(CMD_BACK, 0, 0); break;
    case BM_FWD: push_cmd(CMD_FWD, 0, 0); break;
    case BM_RELOAD: push_cmd(CMD_RELOAD, 0, 0); break;
    case BM_COPY: push_cmd(CMD_COPY, 0, 0); break;
    case BM_PASTE: push_cmd(CMD_PASTE, 0, 0); break;
    case BM_SAVEPAGE:
        if (t && t->page) { kstrlcpy(g_dl_url, t->page->url, sizeof(g_dl_url)); push_cmd(CMD_DOWNLOAD, 0, 0); }
        break;
    case BM_SOURCE: push_cmd(CMD_SOURCE, 0, 0); break;
    case BM_DOWNLOADS: fs_mkdir_p(DOWNLOAD_DIR); gui_open_files(DOWNLOAD_DIR); break;
    }
    g_gen++;
}

static void run_commands(void) {
    while (g_cmd_tail != g_cmd_head) {
        cmd_t c = g_cmds[g_cmd_tail];
        g_cmd_tail = (g_cmd_tail + 1) % 32;
        tab_t* t = cur_tab();
        switch (c.kind) {
        case CMD_CLICK:
            if (t && t->page && !t->loading) {
                g_addr_focus = 0;
                page_click(t->page, c.x, c.y);
                after_page_event(t);
                g_render_req = 1;
            }
            break;
        case CMD_RCLICK: {
            /* the right-click menu: a link's, or the page's */
            int link = t && t->page && !t->loading && page_link_at(t->page, c.x, c.y, g_menu_link, sizeof(g_menu_link));
            if (!link) g_menu_link[0] = 0;
            uint32_t clip;
            clipboard_get(&clip);
            ctx_item_t items[CTX_MAX_ITEMS];
            int n = 0;
            if (link) {
                items[n++] = (ctx_item_t){ "Open link", BM_OPEN, 0 };
                items[n++] = (ctx_item_t){ "Open link in new tab", BM_NEWTAB, 0 };
                items[n++] = (ctx_item_t){ "Save link as (download)", BM_SAVELINK, 0 };
                items[n++] = (ctx_item_t){ "Copy link address", BM_COPYLINK, 0 };
                items[n++] = (ctx_item_t){ CTX_SEP, 0, 0 };
            }
            items[n++] = (ctx_item_t){ "Back", BM_BACK, !t || t->hist_pos <= 0 };
            items[n++] = (ctx_item_t){ "Forward", BM_FWD, !t || t->hist_pos >= t->hist_n - 1 };
            items[n++] = (ctx_item_t){ "Reload", BM_RELOAD, 0 };
            items[n++] = (ctx_item_t){ CTX_SEP, 0, 0 };
            items[n++] = (ctx_item_t){ "Copy", BM_COPY, !g_sel_on };
            items[n++] = (ctx_item_t){ "Paste", BM_PASTE, clip == 0 };
            items[n++] = (ctx_item_t){ CTX_SEP, 0, 0 };
            items[n++] = (ctx_item_t){ "Save page as (download)", BM_SAVEPAGE, !t };
            items[n++] = (ctx_item_t){ "View page source", BM_SOURCE, !t };
            if (n < CTX_MAX_ITEMS) items[n++] = (ctx_item_t){ "Open Downloads folder", BM_DOWNLOADS, 0 };
            ctxmenu_open(g_rc_mx, g_rc_my, items, n, bmenu_cb, NULL);
            break;
        }
        case CMD_DOWNLOAD: {
            char url[URL_MAX];
            kstrlcpy(url, g_dl_url, sizeof(url));
            if (t && url[0]) download(t, url);
            break;
        }
        case CMD_SOURCE: {
            char url[URL_MAX + 16];
            if (!t || !t->page) break;
            ksnprintf(url, sizeof(url), "view-source:%s", t->page->url);
            new_tab(url);
            break;
        }
        case CMD_COPY: copy_selection(); break;
        case CMD_GO: {
            char url[URL_MAX];
            kstrlcpy(url, g_go_url, sizeof(url));
            if (t) load(t, url, NULL, 0, 1);
            else new_tab(url);
            break;
        }
        case CMD_NEWTAB: {
            char url[URL_MAX];
            kstrlcpy(url, g_go_url, sizeof(url));
            g_go_url[0] = 0;
            new_tab(url);
            break;
        }
        case CMD_CLOSETAB: close_tab(c.x); break;
        case CMD_BACK: history_go(-1); break;
        case CMD_FWD: history_go(1); break;
        case CMD_RELOAD:
            if (t && t->hist_pos >= 0) { char url[URL_MAX]; kstrlcpy(url, t->hist[t->hist_pos], sizeof(url)); load(t, url, NULL, 0, 0); }
            else if (t) load(t, HOME_URL, NULL, 0, 1);
            break;
        case CMD_HOME: if (t) load(t, HOME_URL, NULL, 0, 1); break;
        case CMD_PASTE: paste_now(); break;
        }
    }
}

static int  env_media_open(void* ctx, void* owner, const char* url);
static void env_media_cmd(void* ctx, int id, int cmd, int a, int b, int c);
static int  env_media_status(void* ctx, int id, page_media_status_t* out);

static void env_setup(void) {
    static int done;
    if (done) return;
    done = 1;
    cookies_load();                 /* the cookies kept from earlier sessions */
    g_env.fetch = env_fetch;
    g_env.request = env_request;
    g_env.decode_image = env_decode_image;
    g_env.now_ms = timer_ms;
    g_env.yield = task_maybe_yield;     /* a long script must not freeze the desktop */
    web_yield_hook = task_maybe_yield;  /* nor parsing, styling and layout */
    g_env.log = env_log;
    g_env.cookie_get = env_cookie_get;
    g_env.cookie_set = env_cookie_set;
    g_env.media_open = env_media_open;
    g_env.media_cmd = env_media_cmd;
    g_env.media_status = env_media_status;
    g_env.ctx = NULL;
    /* a page may use a share of the heap (big pages, images, script-heavy
     * sites like GitHub need ~200 MB), within reason */
    uint32_t heap = kheap_total_bytes();
    g_page_mem = heap / 3;
    if (g_page_mem > (384u << 20)) g_page_mem = 384u << 20;
    if (g_page_mem < (16u << 20)) g_page_mem = 16u << 20;
}

/* ── for web views (webview.c): the same network, cookies and images ── */

struct page_env* browser_env(void) { env_setup(); return &g_env; }
uint32_t    browser_page_mem(void) { env_setup(); return g_page_mem; }
void        browser_cookies_save(void) { cookies_save(); }

/* url (or a search) as a document page_load can take: the page, an
 * error page, or a text / image / listing wrapped as HTML. *data is
 * kmalloc'd; final_url is where it ended up after redirects */
/* a sound file (up to 32 MiB), with the browser's cookies; 0, or -1 with err */
int browser_fetch_media(const char* url_in, char** data, uint32_t* len, char* err, int ecap) {
    env_setup();
    char url[URL_MAX], ctype[96], fin[URL_MAX];
    normalize_url(url_in, url, sizeof(url));
    dl_t dl;
    memset(&dl, 0, sizeof(dl));
    dl.force = 1;                                    /* (the big limit) */
    g_quiet++;
    int rc = fetch_url(url, NULL, NULL, 0, NULL, data, len, ctype, sizeof(ctype), fin, sizeof(fin), err, ecap, &dl);
    g_quiet--;
    return rc;
}

/* <audio> in pages: the media streams (media.c) */
static int env_media_open(void* ctx, void* owner, const char* url) { (void)ctx; return media_open(owner, url); }
static void env_media_cmd(void* ctx, int id, int cmd, int a, int b, int c) {
    (void)ctx;
    switch (cmd) {
    case PAGE_MEDIA_PLAY:  media_play(id); break;
    case PAGE_MEDIA_PAUSE: media_pause(id); break;
    case PAGE_MEDIA_SEEK:  media_seek(id, (uint32_t)(a < 0 ? 0 : a)); break;
    case PAGE_MEDIA_SET:   media_set(id, a, b, c); break;
    case PAGE_MEDIA_CLOSE: media_close(id); break;
    }
}
static int env_media_status(void* ctx, int id, page_media_status_t* out) {
    (void)ctx;
    media_status_t st;
    if (media_status(id, &st) != 0) return -1;
    out->state = st.state;
    out->playing = st.playing;
    out->ended = st.ended;
    out->pos_ms = st.pos_ms;
    out->dur_ms = st.dur_ms;
    out->seq = st.seq;
    kstrlcpy(out->error, st.error, sizeof(out->error));
    return 0;
}

int browser_fetch_document(const char* url_in, const char* post, uint32_t post_len,
                           char** data, uint32_t* len, char* final_url, int fcap) {
    env_setup();
    char url[URL_MAX], ctype[96], err[200];
    normalize_url(url_in, url, sizeof(url));
    g_quiet++;
    int rc = fetch_url(url, NULL, post, post_len, NULL, data, len, ctype, sizeof(ctype), final_url, fcap,
                       err, sizeof(err), NULL);
    g_quiet--;
    if (rc != 0) {
        *data = error_page(url, err[0] ? err : "the page could not be loaded", len);
        kstrlcpy(final_url, url, (size_t)fcap);
        kstrlcpy(ctype, "text/html", sizeof(ctype));
    }
    if (*data) *data = wrap_content(final_url, ctype, *data, len);
    return *data ? rc : -1;
}

static void browser_task(void) {
    env_setup();
    for (;;) {
        if (!g_open) {
            /* closed: drop the tabs (frees their pages), wait */
            while (g_ntabs) close_tab(g_ntabs - 1);
            g_open = 0;
            task_sleep_ms(100);
            continue;
        }
        ensure_view();
        read_keys();
        run_commands();
        if (!g_ntabs) { g_go_url[0] = 0; new_tab(NULL); }
        /* every tab's timers run; the current one is drawn */
        for (int i = 0; i < g_ntabs; i++) {
            tab_t* t = g_tabs[i];
            if (!t->page || t->loading) continue;
            if (page_tick(t->page)) t->page->dirty = 1;
            if (t->page->dirty || t->page->nav_pending || t->page->alert_pending || t->page->status[0])
                after_page_event(t);
        }
        cookies_save();                 /* new long-lived cookies (no-op otherwise) */
        if (g_render_req) render_view();
        task_sleep_ms(10);
    }
}

/* ══ window (GUI side) ════════════════════════════════════════════════ */

void browser_open(const char* url) {
    if (g_task < 0) {
        g_task = task_create_stack("browser", browser_task, 2u << 20);
        if (g_task < 0) return;
    }
    if (url && *url) { kstrlcpy(g_go_url, url, sizeof(g_go_url)); push_cmd(g_open && g_ntabs ? CMD_NEWTAB : CMD_GO, 0, 0); }
    g_open = 1;
    win_clamp(&g_win);
    g_gen++;
}

void browser_close(void) {
    g_open = 0;
    g_win.dragging = g_win.resizing = 0;
    g_alert[0] = 0;
    g_gen++;
}

int browser_is_open(void) { return g_open; }
void browser_wheel(int mx, int my, int dz) { (void)mx; (void)my; scroll_by(dz * 48); }
int browser_busy(void) { tab_t* t = cur_tab(); return g_open && t && t->loading; }

int browser_contains(int mx, int my) {
    return g_open && inside(mx, my, g_win.x, g_win.y, g_win.w, g_win.h);
}

uint32_t browser_signature(void) {
    if (!g_open) return 0;
    return g_gen * 2654435761u ^ (uint32_t)(g_win.x << 16 | g_win.y) ^ (uint32_t)(g_win.w << 20 | g_win.h << 4) ^
           (uint32_t)gui_browser_focused() * 977u ^ (uint32_t)g_cur * 131u;
}

void browser_paste(void) { push_cmd(CMD_PASTE, 0, 0); }

void browser_rclick(int mx, int my) {
    tab_t* t = cur_tab();
    int lx = mx - g_win.x, ly = my - g_win.y;
    g_rc_mx = mx;
    g_rc_my = my;
    if (t && inside(lx, ly, VIEW_X, VIEW_Y, view_w(), view_h()))
        push_cmd(CMD_RCLICK, lx - VIEW_X, ly - VIEW_Y + t->scroll);
    else
        push_cmd(CMD_RCLICK, -100000, -100000);     /* outside the page: no link */
}

static void tool_button(int x, int y, int w, const char* label, int enabled) {
    uint32_t base = enabled ? 0x00303740u : 0x00262B33u;
    bevel(x, y, w, TOOL_H - 2, base, 0x00535D6Eu, 0x0015191Fu);
    int tw = (int)strlen(label) * 8;
    gfx_draw_text(x + (w - tw) / 2, y + 6, label, enabled ? C_TEXT : 0x00707A88u, base);
}

static void draw_clipped(int x, int y, const char* s, int max_chars, uint32_t fg, uint32_t bg, int tail) {
    char buf[160];
    int n = (int)strlen(s);
    if (max_chars > (int)sizeof(buf) - 1) max_chars = (int)sizeof(buf) - 1;
    if (max_chars < 1) return;
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

static void blit_view(int x0, int y0, int w, int h) {
    int stride, tw, th;
    uint32_t* dst = fb_target(&stride, &tw, &th);
    if (!dst) return;
    uint32_t* src = g_view;
    int sw = g_vw, sh = g_vh;
    if (!src) { gfx_fill_rect(x0, y0, w, h, 0x00FFFFFFu); return; }
    for (int y = 0; y < h; y++) {
        int ty = y0 + y;
        if (ty < 0 || ty >= th) continue;
        if (y >= sh) { gfx_fill_rect(x0, ty, w, 1, 0x00E0E0E0u); continue; }
        int cw = w < sw ? w : sw;
        int xs = x0 < 0 ? -x0 : 0;
        int xe = x0 + cw > tw ? tw - x0 : cw;
        if (xe > xs) memcpy(dst + (size_t)ty * (size_t)stride + x0 + xs, src + (size_t)y * (size_t)sw + xs, (size_t)(xe - xs) * 4);
        if (cw < w) gfx_fill_rect(x0 + cw, ty, w - cw, 1, 0x00E0E0E0u);   /* resizing: until re-rendered */
    }
}

static void sb_geometry(tab_t* t, int* thumb_y, int* thumb_h) {
    int track = view_h();
    int ph = t ? t->page_h : 0;
    if (ph <= track) { *thumb_y = 0; *thumb_h = track; return; }
    int h = track * track / ph;
    if (h < 20) h = 20;
    int range = ph - track;
    *thumb_h = h;
    *thumb_y = (track - h) * (t->scroll > range ? range : t->scroll) / range;
}

/* tab i's rectangle in the tab bar (window coordinates); width shrinks with many tabs */
static int tab_width(void) {
    int avail = g_win.w - 8 - 28;
    int w = g_ntabs ? avail / g_ntabs : TAB_W;
    return w > TAB_W ? TAB_W : w;
}

void browser_draw(const fb_info_t* fi) {
    (void)fi;
    if (!g_open) return;
    int x = g_win.x, y = g_win.y, W = g_win.w, H = g_win.h;
    int focused = gui_browser_focused();
    tab_t* t = cur_tab();
    bevel(x, y, W, H, C_PANEL, 0x00505D72u, 0x0010141Cu);
    uint32_t tbg = focused ? C_TITLE : 0x002A3240u;
    bevel(x + 3, y + 3, W - 6, TITLE_H - 2, tbg, 0x00647692u, 0x00111923u);
    char title[160];
    ksnprintf(title, sizeof(title), "%s - Banana Browser", t && t->title[0] ? t->title : "New tab");
    draw_clipped(x + 10, y + 8, title, (W - 60) / 8, 0x00FFFFFFu, tbg, 0);
    win_draw_buttons(&g_win, 5, 14);

    /* tabs */
    int tw = tab_width();
    for (int i = 0; i < g_ntabs; i++) {
        int tx = x + 4 + i * tw;
        uint32_t bg = i == g_cur ? 0x00F2F2F2u : 0x003A4250u;
        uint32_t fg = i == g_cur ? 0x00101010u : C_TEXT;
        bevel(tx, y + TABS_Y, tw - 2, TAB_H, bg, 0x00808A9Au, 0x00202630u);
        tab_t* ti = g_tabs[i];
        const char* label = ti->loading ? "Loading..." : ti->title[0] ? ti->title : "New tab";
        draw_clipped(tx + 5, y + TABS_Y + 6, label, (tw - 26) / 8, fg, bg, 0);
        gfx_draw_text(tx + tw - 13, y + TABS_Y + 6, "x", i == g_cur ? 0x00804040u : 0x00C08080u, bg);
    }
    int px = x + 4 + g_ntabs * tw;
    bevel(px, y + TABS_Y, 22, TAB_H, 0x00303740u, 0x00535D6Eu, 0x0015191Fu);
    gfx_draw_text(px + 7, y + TABS_Y + 6, "+", C_TEXT, 0x00303740u);

    /* toolbar: < > R Hm [address] Go */
    int ty = y + TOOL_Y;
    tool_button(x + 4, ty, 26, "<", t && t->hist_pos > 0);
    tool_button(x + 32, ty, 26, ">", t && t->hist_pos < t->hist_n - 1);
    tool_button(x + 60, ty, 26, t && t->loading ? "*" : "R", 1);
    tool_button(x + 88, ty, 32, "Hm", 1);
    int aw = addr_w();
    uint32_t abg = g_addr_focus ? 0x00FFFFFFu : 0x00E8E8E8u;
    bevel(x + ADDR_X, ty, aw, TOOL_H - 2, abg, 0x00606060u, 0x00C0C0C0u);
    int maxc = (aw - 12) / 8;
    const char* addr = t ? t->addr : "";
    if (g_addr_focus && g_addr_all && addr[0]) {
        int n = (int)strlen(addr);
        if (n > maxc) n = maxc;
        gfx_fill_rect(x + ADDR_X + 4, ty + 4, n * 8 + 2, 12, 0x00B4D5FEu);
        draw_clipped(x + ADDR_X + 5, ty + 6, addr, maxc, 0x00101010u, 0x00B4D5FEu, 0);
    } else {
        draw_clipped(x + ADDR_X + 5, ty + 6, addr, maxc, 0x00101010u, abg, g_addr_focus);
    }
    if (g_addr_focus && !g_addr_all) {
        int n = (int)strlen(addr);
        if (n > maxc) n = maxc;
        gfx_fill_rect(x + ADDR_X + 5 + n * 8, ty + 4, 2, 12, 0x00202020u);
    }
    tool_button(x + ADDR_X + aw + 4, ty, 40, "Go", 1);

    /* page */
    int vw = view_w(), vh = view_h();
    blit_view(x + VIEW_X, y + VIEW_Y, vw, vh);
    int sx = x + VIEW_X + vw;
    gfx_fill_rect(sx, y + VIEW_Y, SB_W, vh, 0x002A3038u);
    int th, tyy;
    sb_geometry(t, &tyy, &th);
    bevel(sx + 1, y + VIEW_Y + tyy, SB_W - 2, th, 0x00596678u, 0x007A889Cu, 0x00303844u);

    /* status bar */
    int sy = y + H - STATUS_H - 2;
    gfx_fill_rect(x + 3, sy, W - 6, STATUS_H, 0x00161B22u);
    if (t) draw_clipped(x + 8, sy + 4, t->status, (W - 30) / 8, t->status_err ? C_ERR : C_DIM, 0x00161B22u, 0);
    gfx_draw_grip(x + W, y + H);

    /* alert() */
    if (g_alert[0]) {
        int aw2 = 420, lines = 1;
        for (const char* s = g_alert; *s; s++) lines += *s == '\n';
        if (lines > 8) lines = 8;
        int ah = 60 + lines * 12;
        int ax = x + (W - aw2) / 2, ay = y + 120;
        bevel(ax, ay, aw2, ah, 0x00262C36u, 0x00707C90u, 0x000C0F14u);
        gfx_fill_rect(ax + 3, ay + 3, aw2 - 6, 16, C_TITLE);
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
        tool_button(ax + aw2 / 2 - 30, ay + ah - 26, 60, "OK", 1);
    }
}

void browser_click(int mx, int my) {
    if (!browser_contains(mx, my)) return;
    int lx = mx - g_win.x, ly = my - g_win.y;
    tab_t* t = cur_tab();
    g_gen++;
    if (g_alert[0]) { g_alert[0] = 0; return; }
    if (ly < TITLE_H + 2) {
        int b = win_button_press(&g_win, 5, 14, mx, my);
        if (b == WIN_BTN_CLOSE) { browser_close(); return; }
        if (b) { g_render_req = 1; return; }
        if (win_title_press(&g_win, mx, my)) g_render_req = 1;
        return;
    }
    if (win_grip_press(&g_win, mx, my)) return;
    if (ly >= TABS_Y && ly < TABS_Y + TAB_H) {
        int tw = tab_width();
        int i = (lx - 4) / tw;
        if (lx >= 4 && i < g_ntabs) {
            if (lx - 4 - i * tw >= tw - 16) { push_cmd(CMD_CLOSETAB, i, 0); return; }
            if (i != g_cur) { g_cur = i; g_sel_on = 0; g_addr_focus = 0; g_render_req = 1; }
            return;
        }
        if (lx >= 4 + g_ntabs * tw && lx < 4 + g_ntabs * tw + 22) { g_go_url[0] = 0; push_cmd(CMD_NEWTAB, 0, 0); }
        return;
    }
    if (ly >= TOOL_Y && ly < TOOL_Y + TOOL_H) {
        g_addr_focus = 0;
        int aw = addr_w();
        if (lx >= 4 && lx < 30) push_cmd(CMD_BACK, 0, 0);
        else if (lx >= 32 && lx < 58) push_cmd(CMD_FWD, 0, 0);
        else if (lx >= 60 && lx < 86) push_cmd(CMD_RELOAD, 0, 0);
        else if (lx >= 88 && lx < 120) push_cmd(CMD_HOME, 0, 0);
        else if (lx >= ADDR_X && lx < ADDR_X + aw) { g_addr_focus = 1; g_addr_all = 1; }
        else if (lx >= ADDR_X + aw + 4 && lx < ADDR_X + aw + 44 && t) {
            kstrlcpy(g_go_url, t->addr, sizeof(g_go_url));
            push_cmd(CMD_GO, 0, 0);
        }
        return;
    }
    int vw = view_w(), vh = view_h();
    if (inside(lx, ly, VIEW_X + vw, VIEW_Y, SB_W, vh)) {
        int th, tyy;
        sb_geometry(t, &tyy, &th);
        int rel = ly - VIEW_Y;
        if (rel >= tyy && rel < tyy + th) { g_sb_drag = 1; g_sb_drag_dy = rel - tyy; }
        else if (rel < tyy) scroll_by(-(vh - 40));
        else scroll_by(vh - 40);
        return;
    }
    if (inside(lx, ly, VIEW_X, VIEW_Y, vw, vh) && t) {
        /* a click, or the start of a text selection: decided when the button goes up */
        g_addr_focus = 0;
        g_press = 1;
        g_selecting = 0;
        g_px = lx - VIEW_X;
        g_py = ly - VIEW_Y + t->scroll;
        if (g_sel_on) { g_sel_on = 0; g_render_req = 1; }
    }
}

void browser_mouse(int mx, int my, int left) {
    if (!g_open) return;
    if (win_mouse(&g_win, mx, my, left)) { g_render_req = 1; g_gen++; }
    tab_t* t = cur_tab();
    if (!left) {
        if (g_press && !g_selecting && t) push_cmd(CMD_CLICK, g_px, g_py);
        g_press = 0;
        g_selecting = 0;
        g_sb_drag = 0;
        return;
    }
    if (g_sb_drag && t) {
        int th, tyy;
        sb_geometry(t, &tyy, &th);
        int track = view_h() - th;
        if (track > 0 && t->page_h > view_h()) {
            int pos = my - g_win.y - VIEW_Y - g_sb_drag_dy;
            int ns = pos * (t->page_h - view_h()) / track;
            if (ns != t->scroll) { t->scroll = ns; g_render_req = 1; }
        }
        return;
    }
    if (g_press && t) {
        int lx = mx - g_win.x - VIEW_X, ly = my - g_win.y - VIEW_Y;
        /* dragging past the top or bottom scrolls */
        if (ly < 0 && t->scroll > 0) { t->scroll -= 16; g_render_req = 1; }
        if (ly > view_h() && t->scroll < t->page_h - view_h()) { t->scroll += 16; g_render_req = 1; }
        int px = lx, py = ly + t->scroll;
        if (!g_selecting && (px - g_px) * (px - g_px) + (py - g_py) * (py - g_py) > 16) g_selecting = 1;
        if (g_selecting && (px != g_sx || py != g_sy)) {
            g_sx = px;
            g_sy = py;
            g_sel_on = 1;
            g_render_req = 1;
        }
    }
}

void browser_fkey(int k) {
    if (KEYF_CODE(k) == KEYF_F1 + 4) push_cmd(CMD_RELOAD, 0, 0);   /* F5 */
}
