/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../web/framework_fixtures.h"
#include "browser.h"
#include "../web/image_decode.h"
#include "utf8.h"
#include "gfx.h"
#include "fs.h"
#include "kstring.h"
#include "kheap.h"
#include "timer.h"
#include "task.h"
#include "spinlock.h"
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
#include "filechooser.h"
#include "kbnav.h"
#include "dlmgr.h"
#include "bhistory.h"
#include "shortcut.h"
#include "explorer.h"
#include "remote_page.h"
#include "web_policy.h"

/*
 * The web browser window. Everything that touches a page (loading,
 * scripts, layout, rendering) happens in restricted user processes; the
 * "browser" task brokers requests and keeps validated pixels. The GUI
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
       CMD_NEWTAB, CMD_CLOSETAB, CMD_PASTE, CMD_DOWNLOAD, CMD_SOURCE, CMD_COPY, CMD_DOWNLOADS };

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
static uint32_t   g_dl_seen;                 /* dlm_gen() when the downloads page was last opened */
static int        g_dl_new;                  /* a download finished since: a dot on the button */
static int        g_dl_announced;            /* the newest finished download told in the status line */
/* the download the status line follows (its progress, then its end) */
static int        g_dl_follow;
static void*      g_dl_follow_tab;
static char       g_dl_follow_msg[200];       /* what it last said: another message replaces it, and it stops */
static char       g_go_url[URL_MAX];        /* CMD_GO / CMD_NEWTAB target */
static char       g_alert[256];

/* page text selection, in page coordinates (the task maps it to text) */
static int        g_press, g_selecting, g_sel_on;
static int        g_px, g_py, g_sx, g_sy;

static cmd_t      g_cmds[32];
static int        g_cmd_head, g_cmd_tail;

static page_env_t g_env;
static uint32_t   g_page_mem = 48u << 20;
#ifdef __x86_64__
static int g_use_libjs=1;
#else
static int g_use_libjs;
#endif
int browser_uses_libjs(void){return g_use_libjs;}
int browser_use_libjs(int enabled){
    if(enabled&&fs_find_file("/apps/libjsworker/app-x86_64")<0)return -1;
    g_use_libjs=!!enabled;return 0;
}
static page_t* browser_new_page(void){
    return g_use_libjs?remote_page_new_libjs(&g_env,g_page_mem):remote_page_new(&g_env,g_page_mem);
}

static int view_w(void) { return g_win.w - 2 * VIEW_X - SB_W; }
static int view_h(void) { return g_win.h - VIEW_Y - STATUS_H - 4; }
#define DLBTN_W   30                 /* the downloads button, right of Go */
static int addr_w(void) { return g_win.w - ADDR_X - 52 - DLBTN_W - 4; }

static tab_t* cur_tab(void) { return g_ntabs ? g_tabs[g_cur] : NULL; }

/* ══ cookies ══════════════════════════════════════════════════════════
 * A jar of name=value per host (or per Domain=), sent back to that host
 * and its subdomains. Cookies with Expires / Max-Age (a site's "remember
 * me", Google's consent choice...) are kept in ~/.config/browser/cookies,
 * so they survive a reboot on an installed system; session cookies are
 * forgotten when the browser closes. */

#define COOKIES     128
#define COOKIE_FILE fs_home_join("/.config/browser/cookies")
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
    uint32_t utc = rtc_now_utc();                   /* (a cookie's date is GMT's) */
    if (!utc) return 0;
    rtc_split_time(utc, &now);
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
    fs_mkdir_p(fs_home_join("/.config/browser"));
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

#define DOWNLOAD_DIR fs_home_join("/Downloads")

typedef struct {
    int      force;          /* save whatever comes */
    int      ram;            /* wanted in RAM (a sound for a page), not saved */
    int      handoff;        /* a page load: a download goes to the download manager (dlmgr.c) */
    int      handed;         /* ... and did: this fetch was stopped at its first bytes */
    int      active;         /* the answer turned out to be a download */
    char     name[96];       /* from Content-Disposition */
    uint32_t reported;       /* bytes at the last progress message */
    /* a download goes straight into its file in ~/Downloads as it comes (it
     * was all kept in RAM first: the free memory capped its size) */
    int      open;           /* the file is made: fidx, path */
    int      fidx;
    int      werr;           /* the file could not be written */
    uint32_t got;
    char     path[FS_PATH_LEN];
    char     fname[FS_NAME_LEN];   /* its name, from the answer's address and type */
    char     ctype[96];            /* (kept from the headers for its first bytes) */
} dl_t;

typedef struct { char* buf; uint32_t n, cap; int too_big; uint32_t limit; dl_t* dl; } body_t;

static void set_status(const char* s, int err);
static void download_name(const char* url, const char* hint, const char* ctype, char* out, int cap);
static void download_path(const char* name, char* path, int cap);
static void download_done(const char* path, uint32_t len);
static void watch_downloads(void);

/* the download's file, made when the answer turns out to be one */
static void dl_open(dl_t* dl) {
    if (dl->open || !dl->fname[0]) return;
    download_path(dl->fname, dl->path, sizeof(dl->path));
    dl->fidx = fs_create(dl->path);
    if (dl->fidx < 0 || fs_write(dl->fidx, "", 0) != 0) { dl->fidx = -1; return; }   /* (else: in RAM, as before) */
    dl->open = 1;
}

/* a download that did not finish: its part goes */
static void dl_abort(dl_t* dl) {
    if (!dl || !dl->open) return;
    fs_delete(dl->path, 0);
    dl->open = 0;
}

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
        /* what its file would be called (the answer may still turn out to be one) */
        kstrlcpy(b->dl->ctype, r->content_type, sizeof(b->dl->ctype));
        download_name(r->final_url, b->dl->name, r->content_type, b->dl->fname, sizeof(b->dl->fname));
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

/* a web view or a media stream is fetching: not the browser's business - its
 * own task stays quiet (the browser's task, loading a page meanwhile, not) */
#define QUIET_MAX 8
static int g_quiet_pid[QUIET_MAX], g_quiet_n[QUIET_MAX];
static spinlock_t g_quiet_lock;
static void quiet_on(void) {
    int me = task_current_pid(), free_slot = -1;
    spin_lock(&g_quiet_lock);
    for (int i = 0; i < QUIET_MAX; i++) {
        if (g_quiet_n[i] && g_quiet_pid[i] == me) { g_quiet_n[i]++; spin_unlock(&g_quiet_lock); return; }
        if (!g_quiet_n[i] && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) { g_quiet_pid[free_slot] = me; g_quiet_n[free_slot] = 1; }
    spin_unlock(&g_quiet_lock);
}
static void quiet_off(void) {
    int me = task_current_pid();
    spin_lock(&g_quiet_lock);
    for (int i = 0; i < QUIET_MAX; i++) if (g_quiet_n[i] && g_quiet_pid[i] == me) { g_quiet_n[i]--; break; }
    spin_unlock(&g_quiet_lock);
}
static int quiet(void) {
    int me = task_current_pid();
    for (int i = 0; i < QUIET_MAX; i++) if (g_quiet_n[i] && g_quiet_pid[i] == me) return 1;
    return 0;
}

static void set_status(const char* s, int err) {
    if (quiet()) return;
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

/* ── about:downloads: the downloads, like Chrome's page ──────────────── */

static void fmt_bytes(uint32_t b, char* o, int cap) {
    if (b >= (1u << 30)) ksnprintf(o, (size_t)cap, "%u.%u GB", b >> 30, ((b >> 20) & 1023) * 10 / 1024);
    else if (b >= (10u << 20)) ksnprintf(o, (size_t)cap, "%u MB", (b + (1u << 19)) >> 20);
    else if (b >= (1u << 20)) ksnprintf(o, (size_t)cap, "%u.%u MB", b >> 20, ((b >> 10) & 1023) * 10 / 1024);
    else if (b >= 1024) ksnprintf(o, (size_t)cap, "%u KB", (b + 512) >> 10);
    else ksnprintf(o, (size_t)cap, "%u B", b);
}

static const char DL_CSS[] =
    "body{margin:0;background:#f1f3f4;font-family:sans-serif;color:#202124}"
    ".bar{background:#fff;padding:14px 24px;border-bottom:1px solid #dadce0;display:flex;align-items:center}"
    ".bar h1{font-size:20px;font-weight:normal;margin:0;flex:1}"
    ".bar a,.acts a{color:#1a73e8;text-decoration:none}"
    ".list{max-width:680px;margin:24px auto;padding:0 16px}"
    ".card{background:#fff;border-radius:8px;box-shadow:0 1px 3px rgba(60,64,67,.3);padding:14px 16px;margin-bottom:12px;display:flex}"
    ".icon{width:40px;height:40px;margin-right:16px;background:#e8f0fe;border-radius:6px;text-align:center;line-height:40px;color:#1a73e8;font-weight:bold;font-size:12px}"
    ".main{flex:1;min-width:0}"
    ".name{font-weight:bold;color:#1a73e8;text-decoration:none}"
    ".gone{color:#80868b;text-decoration:line-through;font-weight:bold}"
    ".url{color:#5f6368;font-size:13px;margin:2px 0 6px;overflow:hidden;white-space:nowrap}"
    ".st{font-size:13px;color:#5f6368;margin-bottom:6px}"
    ".err{color:#d93025}"
    ".prog{height:4px;background:#e8eaed;border-radius:2px;margin:8px 0}"
    ".prog div{height:4px;background:#1a73e8;border-radius:2px}"
    ".acts a{margin-right:18px;font-size:14px}"
    ".x{color:#5f6368;text-decoration:none;font-size:16px;margin-left:12px}"
    ".empty{text-align:center;color:#5f6368;margin-top:90px}";

static char* downloads_html(uint32_t* len) {
    sbuf_t b = { 0, 0, 0 };
    char t[160], s1[24], s2[24], s3[24];
    sb_str(&b, "<html><head><title>Downloads</title><style>");
    sb_str(&b, DL_CSS);
    sb_str(&b, "</style></head><body><div class=bar><h1>Downloads</h1>"
               "<a href=\"about:downloads?folder=1\">Open Downloads folder</a>&nbsp;&nbsp;&nbsp;"
               "<a href=\"about:downloads?clear=1\">Clear all</a></div><div class=list>");
    dlm_info_t d;
    int n = 0, running = 0;
    for (int i = 0; dlm_get(i, &d); i++, n++) {
        int live = d.state == DLM_RUNNING || d.state == DLM_QUEUED;
        running |= live;
        int gone = d.state == DLM_DONE && fs_find_file(d.path) < 0;
        /* the file's kind, as Chrome's icon */
        const char* dot = strrchr(d.name, '.');
        char ext[6] = "FILE";
        if (dot && dot[1] && strlen(dot + 1) <= 4) { kstrlcpy(ext, dot + 1, sizeof(ext)); for (char* p = ext; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32; }
        sb_str(&b, "<div class=card><div class=icon>");
        sb_esc(&b, ext, (uint32_t)strlen(ext));
        sb_str(&b, "</div><div class=main>");
        if (d.state == DLM_DONE && !gone) {
            ksnprintf(t, sizeof(t), "<a class=name href=\"about:downloads?open=%d\">", d.id);
            sb_str(&b, t);
            sb_esc(&b, d.name, (uint32_t)strlen(d.name));
            sb_str(&b, "</a>");
        } else {
            sb_str(&b, gone || d.state == DLM_CANCELLED || d.state == DLM_FAILED ? "<span class=gone>" : "<span class=name>");
            sb_esc(&b, d.name, (uint32_t)strlen(d.name));
            sb_str(&b, "</span>");
        }
        sb_str(&b, "<div class=url>");
        sb_esc(&b, d.url, (uint32_t)strlen(d.url));
        sb_str(&b, "</div>");
        if (d.state == DLM_RUNNING) {
            fmt_bytes(d.got, s1, sizeof(s1));
            if (d.total) {
                fmt_bytes(d.total, s2, sizeof(s2));
                ksnprintf(t, sizeof(t), "<div class=prog><div style=\"width:%u%%\"></div></div>", (uint32_t)((uint64_t)d.got * 100 / d.total));
                sb_str(&b, t);
            }
            fmt_bytes(d.rate, s3, sizeof(s3));
            if (d.total && d.rate) {
                uint32_t left = (d.total - d.got) / d.rate;
                char eta[32];
                if (left >= 3600) ksnprintf(eta, sizeof(eta), "%u h %u min left", left / 3600, left / 60 % 60);
                else if (left >= 60) ksnprintf(eta, sizeof(eta), "%u min left", (left + 30) / 60);
                else ksnprintf(eta, sizeof(eta), "%u s left", left);
                ksnprintf(t, sizeof(t), "<div class=st>%s/s - %s of %s, %s</div>", s3, s1, s2, eta);
            } else if (d.total) ksnprintf(t, sizeof(t), "<div class=st>%s of %s</div>", s1, s2);
            else ksnprintf(t, sizeof(t), "<div class=st>%s%s%s</div>", s1, d.rate ? ", " : "", d.rate ? s3 : "");
            sb_str(&b, t);
        } else if (d.state == DLM_QUEUED) sb_str(&b, "<div class=st>Waiting for another download to finish...</div>");
        else if (d.state == DLM_DONE) {
            fmt_bytes(d.got, s1, sizeof(s1));
            ksnprintf(t, sizeof(t), "<div class=st>%s%s</div>", s1, gone ? " - Deleted" : "");
            sb_str(&b, t);
        } else if (d.state == DLM_FAILED) {
            sb_str(&b, "<div class=\"st err\">Failed - ");
            sb_esc(&b, d.err, (uint32_t)strlen(d.err));
            sb_str(&b, "</div>");
        } else sb_str(&b, "<div class=st>Cancelled</div>");
        sb_str(&b, "<div class=acts>");
        if (live) ksnprintf(t, sizeof(t), "<a href=\"about:downloads?cancel=%d\">Cancel</a>", d.id);
        else if (d.state == DLM_DONE && !gone) ksnprintf(t, sizeof(t), "<a href=\"about:downloads?show=%d\">Show in folder</a>", d.id);
        else ksnprintf(t, sizeof(t), "<a href=\"about:downloads?retry=%d\">Retry</a>", d.id);
        sb_str(&b, t);
        sb_str(&b, "</div></div>");
        if (!live) {
            ksnprintf(t, sizeof(t), "<a class=x href=\"about:downloads?remove=%d\" title=\"Remove from list\">X</a>", d.id);
            sb_str(&b, t);
        }
        sb_str(&b, "</div>");
    }
    if (!n) sb_str(&b, "<div class=empty>Files you download appear here</div>");
    sb_str(&b, "</div>");
    if (running) sb_str(&b, "<script>setTimeout(function(){location.reload()},1000)</script>");
    sb_str(&b, "</body></html>");
    *len = b.n;
    return b.s;
}

/* a button of about:downloads: "cancel=3", "open=3", "clear=1"... */
static void downloads_action(const char* q) {
    const char* eq = strchr(q, '=');
    uint32_t id = 0;
    if (eq) k_parse_u32(eq + 1, &id);
    dlm_info_t d;
    if (!strncmp(q, "cancel=", 7)) dlm_cancel((int)id);
    else if (!strncmp(q, "retry=", 6)) dlm_retry((int)id);
    else if (!strncmp(q, "remove=", 7)) dlm_remove((int)id);
    else if (!strncmp(q, "clear=", 6)) dlm_clear();
    else if (!strncmp(q, "folder=", 7)) { fs_mkdir_p(DOWNLOAD_DIR); gui_open_files(DOWNLOAD_DIR); }
    else if (!strncmp(q, "open=", 5) && dlm_find((int)id, &d) && d.state == DLM_DONE) explorer_launch(d.path);
    else if (!strncmp(q, "show=", 5) && dlm_find((int)id, &d)) {
        char dir[FS_PATH_LEN];
        kstrlcpy(dir, d.path, sizeof(dir));
        char* sl = strrchr(dir, '/');
        if (sl) *sl = 0;
        gui_open_files(dir);
        explorer_select(d.name);
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
    " <li><a href=\"about:frameworks\">Framework demos</a> - React, Vue, Alpine, Astro and Tailwind CSS (offline)</li>\n"
    " <li><a href=\"https://swm.mxtpasontop.fr/\">SuperWebManager</a> - the real-site browser compatibility test target</li>\n"
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
        if(!f || f->size>MAX_DOC){kstrlcpy(err,"file exceeds the 8 MiB page quota",ecap);return -1;}
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
    int* idx = (int*)kmalloc(FS_LIST_MAX * sizeof(int));   /* (too big for a task's stack; given back below) */
    int nd = idx ? fs_list_dirs(p, idx, FS_LIST_MAX) : 0;
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
    int nf = idx ? fs_list_files(p, idx, FS_LIST_MAX) : 0;
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
    kfree(idx);
    sb_str(&b, "</table></body></html>");
    *data = b.s;
    *len = b.n;
    kstrlcpy(ctype, "text/html", (size_t)ccap);
    return 0;
}

static int on_body(void* ctx, const uint8_t* d, uint32_t n) {
    body_t* b = (body_t*)ctx;
    dl_t* dl = b->dl;
    /* a binary answer the server called text (or nothing): a file, found
     * by its first bytes, before any of it is kept as a page */
    if (dl && !dl->active && !dl->force && b->n == 0 && n && strncasecmp(dl->ctype, "image/", 6) != 0) {
        uint32_t k = n < 1024 ? n : 1024;
        for (uint32_t i = 0; i < k; i++) if (!d[i]) { dl->active = 1; b->limit = FS_MAX_FILE_SIZE; break; }
    }
    if (dl && dl->active && dl->handoff && !dl->force) { dl->handed = 1; return -1; }   /* (the manager fetches it again) */
    if (dl && dl->active && !dl->open && !dl->ram && b->n == 0) dl_open(dl);
    uint32_t total = (dl && dl->open ? dl->got : b->n) + n;
    if (dl && dl->active && total - dl->reported >= (256u << 10)) {
        char msg[96];
        dl->reported = total;
        ksnprintf(msg, sizeof(msg), "Downloading... %u.%u MB", total >> 20, ((total >> 10) & 1023) * 10 / 1024);
        set_status(msg, 0);
    }
    if (dl && dl->open) {                           /* into the file, not RAM */
        if ((uint64_t)dl->got + n > FS_MAX_FILE_SIZE) { b->too_big = 1; return -1; }
        if (fs_append(dl->fidx, d, n) != 0) { dl->werr = 1; return -1; }
        dl->got += n;
        return 0;
    }
    if ((uint64_t)b->n + n + 1 > b->limit) { b->too_big = 1; return -1; }
    if (b->n + n + 1 > b->cap) {
        uint64_t want = ((uint64_t)b->n + n + 1) * 2;
        uint32_t cap = want > b->limit ? b->limit : (uint32_t)want;
        char* nb = (char*)kmalloc(cap);
        if (!nb) { b->too_big = 2; return -1; }
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
    if (strcasecmp(url, "about:downloads") == 0) {
        *data = downloads_html(len);
        if (!*data) return -1;
        kstrlcpy(ctype, "text/html", (size_t)ccap);
        return 0;
    }
    if (strncasecmp(url, "about:", 6) == 0) {
        const char* src = strcasecmp(url, "about:home") == 0 ? HOME_HTML : "<html><body></body></html>";
        const char* type = "text/html";
        uint32_t n;
        if (!framework_fixture(url, &src, &n, &type)) n = (uint32_t)strlen(src);
        char* b = (char*)kmalloc(n + 1);
        if (!b) return -1;
        memcpy(b, src, n);
        b[n] = 0;
        *data = b;
        *len = n;
        kstrlcpy(ctype, type, (size_t)ccap);
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
    req.user_agent = "Mozilla/5.0 (Banana OS; x86_64) BananaBrowser/2.0";
    req.ctx = &b;
    req.on_body = on_body;
    http_response_t* resp = (http_response_t*)kmalloc(sizeof(http_response_t));
    if (!resp) return -1;
    int rc = http_fetch(url, &req, resp, err, (uint32_t)ecap);
    if (dl && dl->handed) {                     /* a download: the manager's now (2) */
        if (resp->final_url[0]) kstrlcpy(final_url, resp->final_url, (size_t)fcap);
        kstrlcpy(ctype, resp->content_type, (size_t)ccap);
        if (b.buf) kfree(b.buf);
        kfree(resp);
        err[0] = 0;
        return 2;
    }
    if (rc != NET_OK || b.too_big) {
        if (dl && dl->werr) kstrlcpy(err, "the file could not be written (is the disk full?)", (size_t)ecap);
        else if (b.too_big == 2) kstrlcpy(err, "not enough memory for it", (size_t)ecap);
        else if (b.too_big) kstrlcpy(err, b.limit > MAX_DOC ? "the file is too big (2 GiB max)" : "the page is too big (8 MiB max)", (size_t)ecap);
        dl_abort(dl);
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

/* A worker's network requests carry a principal fixed by the kernel.
 * Public cross-origin assets get no cookies; scripts may request only their
 * own origin. Every redirect and resolved endpoint is checked by http.c. */
typedef struct {
    body_t body;                         /* on_body accepts this prefix */
    const char* principal;
    int xhr;
    char headers[8192];
} worker_http_t;
static int worker_allow_url(void* ctx,const char* url) {
    worker_http_t* w=ctx;
    return web_network_url(url)&&(!w->xhr||web_same_origin(w->principal,url));
}
static int worker_allow_address(void* ctx,const char* host,uint16_t port,int family,const void* address) {
    worker_http_t* w=ctx;return web_endpoint_allowed(w->principal,host,port,family,address);
}
static const char* worker_headers(void* ctx,const char* url) {
    worker_http_t* w=ctx;w->headers[0]=0;
    if(web_same_origin(w->principal,url))cookie_header(url,w->headers,sizeof(w->headers));
    return w->headers[0]?w->headers:NULL;
}
static void worker_on_headers(void* ctx,const http_response_t* response,const char* raw) {
    worker_http_t* w=ctx;
    if(web_same_origin(w->principal,response->final_url))on_headers(&w->body,response,raw);
}
int browser_worker_fetch(const char* principal,int xhr,const char* url,const char* method,
    const char* body,unsigned int length,const char* content_type,char** data,unsigned int* size,
    char* type,int type_cap,char* final,int final_cap,char* error,int error_cap) {
    worker_http_t* w=kzalloc(sizeof(*w));http_response_t* response=kmalloc(sizeof(*response));
    if(!w||!response){kfree(w);kfree(response);return -1;}
    w->principal=principal;w->xhr=xhr;w->body.limit=MAX_DOC;
    http_request_t request;memset(&request,0,sizeof(request));
    request.ctx=w;request.method=method;
    if(strcmp(method,"GET")){request.body=body;request.body_len=length;request.content_type=content_type;}
    request.follow_redirects=1;request.max_redirects=6;request.timeout_ms=10000;
    request.user_agent="Mozilla/5.0 (Banana OS; x86_64) BananaBrowser/2.0";
    request.on_body=on_body;request.on_headers=worker_on_headers;request.headers_for=worker_headers;
    request.allow_url=worker_allow_url;request.allow_address=worker_allow_address;request.restricted=1;
    int rc=http_fetch(url,&request,response,error,error_cap);
    if(rc==NET_OK&&!w->body.too_big){
        if(!w->body.buf)w->body.buf=kzalloc(1);
        *data=w->body.buf;*size=w->body.n;
        kstrlcpy(type,response->content_type,type_cap);kstrlcpy(final,response->final_url,final_cap);
        rc=*data?0:-1;
    }else{if(w->body.buf)kfree(w->body.buf);rc=-1;}
    kfree(response);kfree(w);return rc;
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
    int rc = fetch_url(url, method, body ? body : "", body ? blen : 0, btype, data, len, rtype, rcap, fin, sizeof(fin),
                       err, ecap, NULL);
    if (rc == 0) set_status("", 0);              /* a script's request (an upload...) is done */
    return rc;
}


/* a file the user chose for an upload */
static int env_read_file(void* ctx, const char* path, char** data, uint32_t* len) {
    (void)ctx;
    int fi = fs_find_file(path);
    if (fi < 0) return -1;
    fs_pin(fi);
    fs_file_t* f = fs_get_file(fi);
    if (!f || f->size>MAX_DOC || (!f->content && f->size)) { fs_unpin(fi); return -1; }
    char* d = (char*)kmalloc(f->size + 1);
    if (!d) { fs_unpin(fi); return -1; }
    if (f->size) memcpy(d, f->content, f->size);
    d[f->size] = 0;
    fs_unpin(fi);
    *data = d;
    *len = f->size;
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

/* Keep the existing search provider until Google challenge compatibility passes. */
#define SEARCH_URL "https://duckduckgo.com/html/?q="
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
    /* on the wire, as other browsers send them: spaces and non-ASCII bytes
     * of an address percent-encoded ("Vidéo.mp4" -> "Vid%C3%A9o.mp4") */
    if (strncasecmp(out, "http://", 7) == 0 || strncasecmp(out, "https://", 8) == 0) {
        int need = 0;
        for (const unsigned char* c = (const unsigned char*)out; *c; c++) if (*c >= 0x80 || *c == ' ') need = 1;
        if (need) {
            static const char hx[] = "0123456789ABCDEF";
            char* t = kmalloc((size_t)cap);
            if (!t) return;
            int k = 0;
            for (const unsigned char* c = (const unsigned char*)out; *c && k < cap - 4; c++) {
                if (*c >= 0x80 || *c == ' ') { t[k++] = '%'; t[k++] = hx[*c >> 4]; t[k++] = hx[*c & 15]; }
                else t[k++] = (char)*c;
            }
            t[k] = 0;
            kstrlcpy(out, t, (size_t)cap);
            kfree(t);
        }
    }
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

/* ~/Downloads/name, or "name (2).ext"... when it is taken */
static void download_path(const char* name, char* path, int cap) {
    fs_mkdir_p(DOWNLOAD_DIR);
    ksnprintf(path, (size_t)cap, "%s/%s", DOWNLOAD_DIR, name);
    for (int n = 2; fs_find_file(path) >= 0 && n < 100; n++) {
        char base[FS_NAME_LEN];
        kstrlcpy(base, name, sizeof(base));
        char* dot = strrchr(base, '.');
        char ext[12] = "";
        if (dot && strlen(dot) < sizeof(ext)) { kstrlcpy(ext, dot, sizeof(ext)); *dot = 0; }
        base[FS_NAME_LEN - 8 - strlen(ext)] = 0;
        ksnprintf(path, (size_t)cap, "%s/%s (%d)%s", DOWNLOAD_DIR, base, n, ext);
    }
}

static void save_download(const char* url, const char* hint, const char* ctype, const char* data, uint32_t len) {
    char name[FS_NAME_LEN], path[FS_PATH_LEN], msg[160];
    download_name(url, hint, ctype, name, sizeof(name));
    download_path(name, path, sizeof(path));
    if (fs_write_path(path, data, len) < 0) {
        ksnprintf(msg, sizeof(msg), "Could not save %s (out of space?)", strrchr(path, '/') + 1);
        set_status(msg, 1);
        return;
    }
    download_done(path, len);
}

/* the status line once a download is in its file */
static void download_done(const char* path, uint32_t len) {
    char msg[160];
    const char* leaf = strrchr(path, '/') + 1;
    const char* tail = "";
    const char* dot = strrchr(leaf, '.');
    if (dot && strcasecmp(dot, ".bpk") == 0) tail = " - an app: install it from Files (right-click)";
    else if (dot && strcasecmp(dot, ".wav") == 0) tail = " - play it from Files";
    else if (dot && download_ext(leaf - 1 > path ? path : leaf)) tail = " - open it from Files";
    if (len >= (10u << 20))
        ksnprintf(msg, sizeof(msg), "Downloaded %s (%u MB) to ~/Downloads%s", leaf, (len + (1u << 19)) >> 20, tail);
    else
        ksnprintf(msg, sizeof(msg), "Downloaded %s (%u KB) to ~/Downloads%s", leaf, (len + 1023) / 1024, tail);
    set_status(msg, 0);
}

/* a finished download: already in its file, or saved from RAM now */
static void finish_download(dl_t* dl, const char* url, const char* ctype, const char* data, uint32_t len) {
    if (dl->open) download_done(dl->path, dl->got);
    else save_download(url, dl->name, ctype, data, len);
}

/* "Save link as" / "Save page as": always a download, the page stays */
/* a download for the manager (kernel/dlmgr.c): its file in ~/Downloads, its
 * entry on about:downloads; the tab goes on */
static void hand_to_manager(tab_t* t, const char* url, const char* fname) {
    char path[FS_PATH_LEN], msg[200];
    download_path(fname && fname[0] ? fname : "download", path, sizeof(path));
    g_status_tab = t;
    int id = dlm_start(url, path);
    if (id < 0) set_status("Cannot start the download (too many at once)", 1);
    else {
        ksnprintf(msg, sizeof(msg), "Downloading %s - the arrow (or Ctrl+J) shows your downloads", strrchr(path, '/') + 1);
        set_status(msg, 0);
        g_dl_follow = id;
        g_dl_follow_tab = t;
        kstrlcpy(g_dl_follow_msg, t->status, sizeof(g_dl_follow_msg));
    }
    g_status_tab = NULL;
    g_gen++;
}

static void download(tab_t* t, const char* url_in) {
    char url[URL_MAX], final_url[URL_MAX], ctype[96], err[200], msg[200];
    normalize_url(url_in, url, sizeof(url));
    if (!strncasecmp(url, "http://", 7) || !strncasecmp(url, "https://", 8)) {
        char name[FS_NAME_LEN];
        download_name(url, NULL, "", name, sizeof(name));
        hand_to_manager(t, url, name);
        return;
    }
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
        finish_download(&dl, final_url, ctype, data, len);
        kfree(data);
    }
    g_status_tab = NULL;
}

/* the next load's POST body type (a multipart form: uploads) */
static char g_load_btype[128];

/* "Choose a file" for an <input type=file>: opened from the page's task,
 * drawn and clicked in the desktop's, the answer handed to the page here */
static volatile int g_pick_on, g_pick_done;
static page_t*      g_pick_page;
static dom_node_t*  g_pick_node;
static char         g_pick_path[FS_PATH_LEN];

static void pick_chosen(const char* path) {
    kstrlcpy(g_pick_path, path, sizeof(g_pick_path));
    g_pick_done = 1;
}

static tab_t* g_work_tab;               /* whose page the engine works on (engine_crashed) */

static void load(tab_t* t, const char* url_in, const char* post, uint32_t post_len, int add_history) {
    char url[URL_MAX], final_url[URL_MAX], ctype[96], err[200];
    g_work_tab = t;
    normalize_url(url_in, url, sizeof(url));
    /* about:downloads?cancel=3 and the like: the page's buttons; then the list again */
    if (!strncasecmp(url, "about:downloads?", 16)) {
        downloads_action(url + 16);
        kstrlcpy(url, "about:downloads", sizeof(url));
        if (t->page && !strcmp(t->page->url, "about:downloads")) add_history = 0;
    }
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
    dl.handoff = !post;                          /* (a form's answer: kept in the tab) */
    /* view-source:URL shows the page's text */
    /* about:crash - the engine crashes on purpose (what a bad page could do): the
     * tab shows the crash, the rest of the system goes on */
    if (!strcmp(url, "about:crash")) __asm__ volatile("ud2");
    int vsrc = strncmp(url, "view-source:", 12) == 0;
    int frc = fetch_url(vsrc ? url + 12 : url, NULL, post, post_len, post && g_load_btype[0] ? g_load_btype : NULL,
                        &data, &len, ctype, sizeof(ctype), final_url, sizeof(final_url), err, sizeof(err), vsrc ? NULL : &dl);
    g_load_btype[0] = 0;
    if (frc == 2) {                               /* a download: the manager has it, the page stays */
        hand_to_manager(t, final_url, dl.fname);
        if (prev_addr[0]) kstrlcpy(t->addr, prev_addr, sizeof(t->addr));
        t->loading = 0;
        g_render_req = 1;
        g_gen++;
        return;
    }
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
        finish_download(&dl, final_url, ctype, data, len);
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
    page_t* p = browser_new_page();
    if (!p) { kfree(data); set_status("Isolated renderer unavailable (quota or 64-bit kernel required)", 1); t->loading = 0; g_status_tab = NULL; return; }
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

    if (!t->status_err) bh_visit(final_url, t->title);        /* (what the address bar suggests later) */
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
    if (p->file_pick_pending) {
        p->file_pick_pending = 0;
        if (t == cur_tab() && !g_pick_on && p->file_pick) {
            g_pick_page = p;
            g_pick_node = p->file_pick;
            g_pick_done = 0;
            g_pick_on = 1;
            fc_open("Choose a file to upload", fs_home_path(), NULL, pick_chosen);
            g_gen++;
        }
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
                kstrlcpy(g_load_btype, p->nav_post_type, sizeof(g_load_btype));
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

/* ── the address bar: a caret, and what it suggests (Chrome's omnibox) ──
 * The text is edited where the caret is (Left / Right / Home / End,
 * Delete, Backspace), and scrolls to keep it in sight; while typing, the
 * pages visited before whose address or title holds the text are offered
 * under it (kernel/bhistory.c): Up / Down choose, Enter or a click goes. */

#define SUG_MAX 6
#define SUG_H   36
static int g_addr_cur;                   /* the caret: a byte of the address */
static int g_addr_view;                  /* the first byte shown (focused) */
static int g_sug[SUG_MAX], g_sug_n, g_sug_sel = -1;

static void addr_suggest(tab_t* t) {
    g_sug_n = g_addr_focus && t->addr[0] ? bh_match(t->addr, g_sug, SUG_MAX) : 0;
    g_sug_sel = -1;
}

static void addr_focus_all(tab_t* t) {
    g_addr_focus = 1;
    g_addr_all = 1;
    g_addr_cur = (int)strlen(t->addr);
    g_addr_view = 0;
    g_sug_n = 0;
    g_sug_sel = -1;
    g_gen++;
}

static void addr_go(tab_t* t) {
    kstrlcpy(g_go_url, g_sug_sel >= 0 && g_sug_sel < g_sug_n ? bh_url(g_sug[g_sug_sel]) : t->addr, sizeof(g_go_url));
    g_sug_n = 0;
    g_sug_sel = -1;
    push_cmd(CMD_GO, 0, 0);
}

static void addr_insert(tab_t* t, const char* s, int n) {
    if (g_addr_all) { t->addr[0] = 0; g_addr_cur = 0; g_addr_view = 0; g_addr_all = 0; }
    int len = (int)strlen(t->addr);
    if (g_addr_cur > len) g_addr_cur = len;
    if (len + n > (int)sizeof(t->addr) - 1) n = (int)sizeof(t->addr) - 1 - len;
    if (n <= 0) return;
    memmove(t->addr + g_addr_cur + n, t->addr + g_addr_cur, (size_t)(len - g_addr_cur + 1));
    memcpy(t->addr + g_addr_cur, s, (size_t)n);
    g_addr_cur += n;
}

/* Backspace (back) or Delete: the character before / after the caret */
static void addr_delete(tab_t* t, int back) {
    int len = (int)strlen(t->addr);
    if (g_addr_all) { t->addr[0] = 0; g_addr_cur = g_addr_view = 0; g_addr_all = 0; return; }
    if (g_addr_cur > len) g_addr_cur = len;
    int a = back ? u8_prev(t->addr, g_addr_cur) : g_addr_cur;
    int b = back ? g_addr_cur : u8_next(t->addr, len, g_addr_cur);
    if (a == b) return;
    memmove(t->addr + a, t->addr + b, (size_t)(len - b + 1));
    g_addr_cur = a;
}

/* a typed character while the address bar has the keys */
static void addr_key(tab_t* t, char c) {
    if (c == 1) { g_addr_all = 1; g_gen++; return; }                       /* Ctrl+A */
    if (c == '\n') { addr_go(t); return; }
    if (c == 27) {                                                          /* Esc: the list, then the edit */
        if (g_sug_n) g_sug_n = 0;
        else {
            g_addr_focus = 0;
            if (t->page) kstrlcpy(t->addr, t->page->url, sizeof(t->addr));
        }
        g_gen++;
        return;
    }
    if (c == '\b') addr_delete(t, 1);
    else if ((unsigned char)c >= 32) addr_insert(t, &c, 1);
    else return;
    g_addr_all = 0;
    addr_suggest(t);
    g_gen++;
}

/* the arrows (and Delete: ESC [3~) in the address bar */
static void addr_arrow(tab_t* t, char a) {
    int len = (int)strlen(t->addr);
    if (g_addr_cur > len) g_addr_cur = len;
    if (a == 'A' || a == 'B') {                                             /* through the suggestions */
        if (!g_sug_n) return;
        if (a == 'B') g_sug_sel = g_sug_sel + 1 < g_sug_n ? g_sug_sel + 1 : g_sug_n - 1;
        else g_sug_sel = g_sug_sel >= 0 ? g_sug_sel - 1 : -1;
        g_gen++;
        return;
    }
    if (a == '3') { addr_delete(t, 0); addr_suggest(t); }
    else if (a == 'D') g_addr_cur = g_addr_all ? 0 : u8_prev(t->addr, g_addr_cur);
    else if (a == 'C') g_addr_cur = g_addr_all ? len : u8_next(t->addr, len, g_addr_cur);
    else if (a == 'H') g_addr_cur = 0;
    else if (a == 'F') g_addr_cur = len;
    else return;
    g_addr_all = 0;
    g_gen++;
}

static void paste_now(void) {
    uint32_t n;
    const char* clip = clipboard_get(&n);
    tab_t* t = cur_tab();
    if (!n || !t) return;
    if (g_addr_focus) {
        for (uint32_t i = 0; i < n; i++)
            if ((unsigned char)clip[i] >= 32) addr_insert(t, clip + i, 1);
        g_addr_all = 0;
        addr_suggest(t);
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

/* ── keys for games: keydown / keyup as each key goes down and up ──
 * Typing repeats only the last key held; a game wants every key down at
 * once. While the page has no text field focused its document gets these
 * (with event.code) instead of the typed keys. */
typedef struct { uint8_t sc; uint8_t kc; const char* key; const char* code; } live_key_t;
static const live_key_t LIVE[] = {     /* key NULL: what the key types */
    {0x01,27,"Escape","Escape"}, {0x02,49,0,"Digit1"}, {0x03,50,0,"Digit2"}, {0x04,51,0,"Digit3"},
    {0x05,52,0,"Digit4"}, {0x06,53,0,"Digit5"}, {0x07,54,0,"Digit6"}, {0x08,55,0,"Digit7"},
    {0x09,56,0,"Digit8"}, {0x0A,57,0,"Digit9"}, {0x0B,48,0,"Digit0"}, {0x0C,189,0,"Minus"},
    {0x0D,187,0,"Equal"}, {0x0E,8,"Backspace","Backspace"}, {0x0F,9,"Tab","Tab"},
    {0x10,81,0,"KeyQ"}, {0x11,87,0,"KeyW"}, {0x12,69,0,"KeyE"}, {0x13,82,0,"KeyR"}, {0x14,84,0,"KeyT"},
    {0x15,89,0,"KeyY"}, {0x16,85,0,"KeyU"}, {0x17,73,0,"KeyI"}, {0x18,79,0,"KeyO"}, {0x19,80,0,"KeyP"},
    {0x1A,219,0,"BracketLeft"}, {0x1B,221,0,"BracketRight"}, {0x1C,13,"Enter","Enter"},
    {0x1D,17,"Control","ControlLeft"},
    {0x1E,65,0,"KeyA"}, {0x1F,83,0,"KeyS"}, {0x20,68,0,"KeyD"}, {0x21,70,0,"KeyF"}, {0x22,71,0,"KeyG"},
    {0x23,72,0,"KeyH"}, {0x24,74,0,"KeyJ"}, {0x25,75,0,"KeyK"}, {0x26,76,0,"KeyL"},
    {0x27,186,0,"Semicolon"}, {0x28,222,0,"Quote"}, {0x29,192,0,"Backquote"},
    {0x2A,16,"Shift","ShiftLeft"}, {0x2B,220,0,"Backslash"},
    {0x2C,90,0,"KeyZ"}, {0x2D,88,0,"KeyX"}, {0x2E,67,0,"KeyC"}, {0x2F,86,0,"KeyV"}, {0x30,66,0,"KeyB"},
    {0x31,78,0,"KeyN"}, {0x32,77,0,"KeyM"}, {0x33,188,0,"Comma"}, {0x34,190,0,"Period"},
    {0x35,191,0,"Slash"}, {0x36,16,"Shift","ShiftRight"}, {0x38,18,"Alt","AltLeft"},
    {0x39,32," ","Space"},
    {0x3B,112,"F1","F1"}, {0x3C,113,"F2","F2"}, {0x3D,114,"F3","F3"}, {0x3E,115,"F4","F4"},
    {0x3F,116,"F5","F5"}, {0x40,117,"F6","F6"}, {0x41,118,"F7","F7"}, {0x42,119,"F8","F8"},
    {0x43,120,"F9","F9"}, {0x44,121,"F10","F10"}, {0x57,122,"F11","F11"}, {0x58,123,"F12","F12"},
    {0x9C,13,"Enter","NumpadEnter"}, {0x9D,17,"Control","ControlRight"}, {0xB8,18,"Alt","AltRight"},
    {0xC7,36,"Home","Home"}, {0xC8,38,"ArrowUp","ArrowUp"}, {0xC9,33,"PageUp","PageUp"},
    {0xCB,37,"ArrowLeft","ArrowLeft"}, {0xCD,39,"ArrowRight","ArrowRight"}, {0xCF,35,"End","End"},
    {0xD0,40,"ArrowDown","ArrowDown"}, {0xD1,34,"PageDown","PageDown"}, {0xD3,46,"Delete","Delete"},
};
#define NLIVE ((int)(sizeof(LIVE) / sizeof(LIVE[0])))
static uint8_t g_live_down[NLIVE];     /* the page was told it is down */
static uint8_t g_live_prev[NLIVE];     /* ... and its keydown prevented the default */
static page_t* g_live_page;

static int live_send(tab_t* t, int i, int down, int mods) {
    char kb[2];
    const char* key = LIVE[i].key;
    if (!key) { kb[0] = keyboard_us_char(LIVE[i].sc, mods & 1); kb[1] = 0; key = kb; }
    return page_key_live(t->page, down, key, LIVE[i].code, LIVE[i].kc, mods);
}

/* tells the page which keys went down or up since the last look */
static void live_keys(void) {
    tab_t* t = cur_tab();
    page_t* p = t && !t->loading ? t->page : NULL;
    if (p != g_live_page) {             /* another page: it knows of no key */
        memset(g_live_down, 0, sizeof(g_live_down));
        memset(g_live_prev, 0, sizeof(g_live_prev));
        for (int i = 0; i < NLIVE; i++) keyboard_take_pressed(LIVE[i].sc);   /* old taps: not its */
        g_live_page = p;
    }
    int on = p && p->js && !p->focus && !g_addr_focus && !g_alert[0] && !g_pick_on && gui_browser_focused();
    if (on) p->live_keys = 1;
    int mods = keyboard_mods(), any = 0;
    for (int i = 0; i < NLIVE; i++) {
        int tap = keyboard_take_pressed(LIVE[i].sc);
        if (!p) continue;
        int d = on && keyboard_held(LIVE[i].sc);
        if (on && tap && !d && !g_live_down[i]) {       /* down and up again between two looks */
            g_live_prev[i] = (uint8_t)live_send(t, i, 1, mods);
            live_send(t, i, 0, mods);
            any = 1;
            continue;
        }
        if (d == g_live_down[i]) continue;
        g_live_down[i] = (uint8_t)d;                    /* (off: what was down goes up) */
        int pr = live_send(t, i, d, mods);
        if (d) g_live_prev[i] = (uint8_t)pr;
        any = 1;
    }
    if (any) after_page_event(t);
}

/* the page's keydown for set-1 key `sc` called preventDefault(): no scrolling */
static int live_prevented(int sc) {
    tab_t* t = cur_tab();
    if (!t || !g_live_page || t->page != g_live_page) return 0;
    for (int i = 0; i < NLIVE; i++) if (LIVE[i].sc == sc) return g_live_prev[i];
    return 0;
}

static void handle_key(char c) {
    tab_t* t = cur_tab();
    if (!t) return;
    if (c == 12) { addr_focus_all(t); return; }                             /* Ctrl+L */
    if (c == 18) { push_cmd(CMD_RELOAD, 0, 0); return; }                    /* Ctrl+R */
    if (c == 14) { g_go_url[0] = 0; push_cmd(CMD_NEWTAB, 0, 0); return; }  /* Ctrl+N: new tab */
    if (c == 10 && (keyboard_mods() & 2)) { push_cmd(CMD_DOWNLOADS, 0, 0); return; }   /* Ctrl+J: the downloads */
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
    if (g_addr_focus) { addr_key(t, c); return; }
    if (t->page && !t->loading && page_key(t->page, c)) { after_page_event(t); return; }
    if (c == ' ' && !live_prevented(0x39)) scroll_by(g_vh - 40);
    else if (c == '\b' && !live_prevented(0x0E)) history_go(-1);
}

static void handle_arrow(char a) {
    if (g_addr_focus) { if (cur_tab()) addr_arrow(cur_tab(), a); return; }
    int sc = a == 'A' ? 0xC8 : a == 'B' ? 0xD0 : (a == 'I' || a == '5') ? 0xC9 :
             (a == 'G' || a == '6') ? 0xD1 : a == 'H' ? 0xC7 : a == 'F' ? 0xCF : 0;
    if (sc && live_prevented(sc)) return;            /* a game has the arrows */
    if (a == 'A') scroll_by(-40);
    else if (a == 'B') scroll_by(40);
    else if (a == 'I' || a == '5') scroll_by(-(g_vh - 40));   /* PgUp: ESC [ I (ESC [ 5 ~) */
    else if (a == 'G' || a == '6') scroll_by(g_vh - 40);
    else if (a == 'H') { if (cur_tab()) cur_tab()->scroll = 0; g_render_req = 1; }
    else if (a == 'F') { if (cur_tab()) cur_tab()->scroll = 1 << 28; g_render_req = 1; }
}

static void read_keys(void) {
    live_keys();                        /* (also when unfocused: what was down goes up) */
    if (!gui_browser_focused()) return;
    for (int k = 0; k < 64; k++) {
        char c = keyboard_try_getchar();
        live_keys();                    /* the keydown before the typing it caused */
        if (!c) return;
        if (g_pick_on && fc_active()) {                /* the file chooser has the keys */
            if (c == 27) {
                char c2 = keyboard_try_getchar();
                if (c2 == '[') {
                    char c3 = keyboard_try_getchar();
                    if (c3 >= '0' && c3 <= '9') keyboard_try_getchar();
                    int code = c3 == 'A' ? KB_UP : c3 == 'B' ? KB_DOWN : c3 == 'C' ? KB_RIGHT : c3 == 'D' ? KB_LEFT :
                               c3 == 'H' ? KB_HOME : c3 == 'F' ? KB_END : (c3 == '5' || c3 == 'I') ? KB_PGUP :
                               (c3 == '6' || c3 == 'G') ? KB_PGDN : 0;
                    if (code) fc_nav(code);
                } else fc_key(27);
            } else if (c == '\t') fc_nav(KB_TAB);
            else fc_key(c);
            g_gen++;
            continue;
        }
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
       BM_SAVEPAGE, BM_SOURCE, BM_DOWNLOADS, BM_LINKDESK, BM_PAGEDESK };
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
    case BM_LINKDESK: case BM_PAGEDESK: {          /* a web shortcut on the desktop: the link, or this page */
        const char* url = id == BM_LINKDESK ? g_menu_link : (t && t->page ? t->page->url : "");
        char target[FS_PATH_LEN], name[FS_NAME_LEN];
        if (!url[0] || shortcut_resolve(url, target, sizeof(target), name, sizeof(name)) != SC_WEB) break;
        if (id == BM_PAGEDESK && t && t->title[0]) kstrlcpy(name, t->title, sizeof(name));
        if (gui_desktop_shortcut(name, target) == 0) set_status("A shortcut to it is on the desktop", 0);
        break;
    }
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
                items[n++] = (ctx_item_t){ "Create desktop shortcut", BM_LINKDESK, 0 };
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
            items[n++] = (ctx_item_t){ "Create desktop shortcut", BM_PAGEDESK, !t || !t->page || strncasecmp(t->page->url, "http", 4) != 0 };
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
        case CMD_DOWNLOADS: {                    /* the downloads page: its tab, or a new one */
            int k = 0;
            for (; k < g_ntabs; k++) if (g_tabs[k]->page && !strcmp(g_tabs[k]->page->url, "about:downloads")) break;
            if (k < g_ntabs) { g_cur = k; load(g_tabs[k], "about:downloads", NULL, 0, 0); }
            else new_tab("about:downloads");
            g_dl_seen = dlm_gen();
            g_dl_new = 0;
            break;
        }
        case CMD_PASTE: paste_now(); break;
        }
    }
}



static void env_setup(void) {
    static int done;
    if (done) return;
    done = 1;
    cookies_load();                 /* the cookies kept from earlier sessions */
    dlm_set_cookies(cookie_header);     /* the download manager sends them too */
    g_env.fetch = env_fetch;
    g_env.request = env_request;
    g_env.decode_image = NULL;                 /* decoding belongs to the worker */
    g_env.now_ms = timer_ms;
    g_env.yield = task_maybe_yield;     /* a long script must not freeze the desktop */
    web_yield_hook = task_maybe_yield;  /* nor parsing, styling and layout */
    g_env.log = env_log;
    g_env.cookie_get = env_cookie_get;
    g_env.cookie_set = env_cookie_set;
    /* Untrusted page media never enters the kernel decoders. */
    g_env.read_file = env_read_file;
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
/* a sound file (in RAM: as big as the memory allows), with the browser's cookies; 0, or -1 with err */
int browser_fetch_media(const char* url_in, char** data, uint32_t* len, char* err, int ecap) {
    env_setup();
    char url[URL_MAX], ctype[96], fin[URL_MAX];
    normalize_url(url_in, url, sizeof(url));
    dl_t dl;
    memset(&dl, 0, sizeof(dl));
    dl.force = 1;                                    /* (the big limit) */
    dl.ram = 1;
    quiet_on();
    int rc = fetch_url(url, NULL, NULL, 0, NULL, data, len, ctype, sizeof(ctype), fin, sizeof(fin), err, ecap, &dl);
    quiet_off();
    return rc;
}

/* <audio> in pages: the media streams (media.c) */


int browser_fetch_document(const char* url_in, const char* post, uint32_t post_len,
                           char** data, uint32_t* len, char* final_url, int fcap) {
    env_setup();
    char url[URL_MAX], ctype[96], err[200];
    normalize_url(url_in, url, sizeof(url));
    quiet_on();
    int rc = fetch_url(url, NULL, post, post_len, NULL, data, len, ctype, sizeof(ctype), final_url, fcap,
                       err, sizeof(err), NULL);
    quiet_off();
    if (rc != 0) {
        *data = error_page(url, err[0] ? err : "the page could not be loaded", len);
        kstrlcpy(final_url, url, (size_t)fcap);
        kstrlcpy(ctype, "text/html", sizeof(ctype));
    }
    if (*data) *data = wrap_content(final_url, ctype, *data, len);
    return *data ? rc : -1;
}

/* The web engine crashed - a CPU exception in this task, on something a
 * page did - while working on g_work_tab's page. That page goes (its memory
 * freed if that works too) and the tab says what happened; the other tabs,
 * and the system, go on. */
static void engine_crashed(void) {
    static kjmp_buf_t again;
    tab_t* volatile t = NULL;
    for (int i = 0; i < g_ntabs; i++) if (g_tabs[i] == (g_work_tab ? g_work_tab : cur_tab())) t = g_tabs[i];
    g_work_tab = NULL;
    g_status_tab = NULL;
    g_live_page = NULL;
    g_pick_on = 0;
    g_alert[0] = 0;
    if (!t) return;
    page_t* p = t->page;
    t->page = NULL;
    t->loading = 0;
    if (p && !kjmp_set(&again)) {
        task_set_recover(&again);
        page_free(p);
    }
    task_set_recover(NULL);
    char why[200];
    ksnprintf(why, sizeof(why), "This page crashed the browser's engine, so it was closed - the rest of "
              "Banana OS was not touched. (%s)", task_crash_msg());
    uint32_t len = 0;
    char* data = error_page(t->addr, why, &len);
    page_t* np = data ? browser_new_page() : NULL;
    if (np) {
        np->view_h = g_vh;
        page_load(np, "about:crashed", data, len, g_vw);
        t->page = np;
        t->scroll = 0;
        kstrlcpy(t->title, "Page crashed", sizeof(t->title));
    }
    if (data) kfree(data);
    set_status("The page crashed and was closed", 1);
    g_render_req = 1;
    g_gen++;
}

static void browser_task(void) {
    env_setup();
    static kjmp_buf_t recover;
    while (kjmp_set(&recover)) engine_crashed();      /* (and armed again) */
    task_set_recover(&recover);
    for (;;) {
        if (!g_open) {
            /* closed: drop the tabs (frees their pages), wait */
            while (g_ntabs) close_tab(g_ntabs - 1);
            g_open = 0;
            task_sleep_ms(100);
            continue;
        }
        ensure_view();
        task_set_recover(&recover);         /* (engine_crashed used it) */
        g_work_tab = cur_tab();
        read_keys();
        run_commands();
        if (!g_ntabs) { g_go_url[0] = 0; new_tab(NULL); }
        if (g_pick_on && (g_pick_done || !fc_active())) {
            int done = g_pick_done;
            g_pick_on = 0;
            g_pick_done = 0;
            for (int i = 0; i < g_ntabs; i++) {
                tab_t* pt = g_tabs[i];
                if (pt->page != g_pick_page || pt->loading) continue;
                page_set_files(pt->page, g_pick_node, done ? g_pick_path : NULL);
                after_page_event(pt);
                g_render_req = 1;
            }
            if (done) fc_close();
            g_gen++;
        }
        /* every tab's timers run; the current one is drawn */
        for (int i = 0; i < g_ntabs; i++) {
            tab_t* t = g_tabs[i];
            if (!t->page || t->loading) continue;
            g_work_tab = t;
            if (page_tick(t->page)) t->page->dirty = 1;
            if (t->page->dirty || t->page->nav_pending || t->page->alert_pending || t->page->status[0])
                after_page_event(t);
        }
        cookies_save();                 /* new long-lived cookies (no-op otherwise) */
        watch_downloads();
        g_work_tab = cur_tab();
        if (g_render_req) render_view();
        task_sleep_ms(10);
    }
}

/* ══ window (GUI side) ════════════════════════════════════════════════ */

void browser_open(const char* url) {
    if (g_task < 0) {
        g_task = task_create_stack("browser", browser_task, 8u << 20);
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
    return g_gen * 2654435761u ^ (uint32_t)((uint32_t)g_win.x << 16 | g_win.y) ^ (uint32_t)(g_win.w << 20 | g_win.h << 4) ^
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

/* the downloads button: an arrow into a tray; blue with a bar under it
 * while downloads run (how far, or a moving piece when the size is not
 * known), a dot when one finished since the downloads page was looked at */
static void draw_dl_button(int x, int y) {
    uint32_t got, total;
    int run = dlm_running(&got, &total);
    tool_button(x, y, DLBTN_W, "", 1);
    uint32_t c = run ? 0x0068A8F0u : C_TEXT;
    int cx = x + DLBTN_W / 2;
    gfx_fill_rect(cx - 1, y + 3, 3, 6, c);                        /* the shaft */
    for (int i = 0; i < 4; i++) gfx_fill_rect(cx - 4 + i, y + 9 + i, 9 - 2 * i, 1, c);   /* its head */
    gfx_fill_rect(cx - 6, y + 14, 2, 3, c);                       /* the tray */
    gfx_fill_rect(cx + 5, y + 14, 2, 3, c);
    gfx_fill_rect(cx - 6, y + 16, 13, 1, c);
    if (run) {
        int w = DLBTN_W - 6;
        gfx_fill_rect(x + 3, y + TOOL_H - 4, w, 2, 0x00202630u);
        if (total) gfx_fill_rect(x + 3, y + TOOL_H - 4, (int)((uint64_t)w * (got > total ? total : got) / total), 2, 0x0068A8F0u);
        else gfx_fill_rect(x + 3 + (int)(timer_ms() / 60 % (uint32_t)(w - 8)), y + TOOL_H - 4, 8, 2, 0x0068A8F0u);
    } else if (g_dl_new) gfx_fill_rect(x + DLBTN_W - 8, y + 3, 4, 4, 0x0068A8F0u);
}

/* the browser's loop: a download that finished is told in the status line
 * (and the button redrawn while they run) */
static void watch_downloads(void) {
    static uint32_t last_gen, last_tick;
    static int started;
    if (!started) {                              /* the list from earlier starts: nothing to tell */
        started = 1;                             /* (this start's are told, even one begun before this first look) */
        g_dl_announced = dlm_session_base() - 1;
    }
    uint32_t gen = dlm_gen();
    if (dlm_running(NULL, NULL) && timer_ms() - last_tick >= 250) {
        last_tick = timer_ms();
        g_gen++;                                 /* (the bar moves) */
        /* the status line follows the download it announced: how far it is */
        tab_t* ft = NULL;
        for (int i = 0; i < g_ntabs; i++) if ((void*)g_tabs[i] == g_dl_follow_tab) ft = g_tabs[i];
        dlm_info_t f;
        if (g_dl_follow && ft && !strcmp(ft->status, g_dl_follow_msg) && dlm_find(g_dl_follow, &f) &&
            (f.state == DLM_RUNNING || f.state == DLM_QUEUED)) {
            char msg[200], got[24], tot[24], rate[24];
            fmt_bytes(f.got, got, sizeof(got));
            if (f.state == DLM_QUEUED) ksnprintf(msg, sizeof(msg), "Waiting to download %s (another one runs) - Ctrl+J shows your downloads", f.name);
            else if (f.total) {
                fmt_bytes(f.total, tot, sizeof(tot));
                fmt_bytes(f.rate, rate, sizeof(rate));
                ksnprintf(msg, sizeof(msg), "Downloading %s - %s of %s (%u%%)%s%s/s - Ctrl+J shows your downloads", f.name, got, tot,
                          (uint32_t)((uint64_t)f.got * 100u / f.total), f.rate ? ", " : "", f.rate ? rate : "");
                if (!f.rate) { char* s = strstr(msg, "/s - "); if (s) memmove(s, s + 2, strlen(s + 2) + 1); }
            } else ksnprintf(msg, sizeof(msg), "Downloading %s - %s so far - Ctrl+J shows your downloads", f.name, got);
            if (strcmp(msg, g_dl_follow_msg)) {
                g_status_tab = ft;
                set_status(msg, 0);
                g_status_tab = NULL;
                kstrlcpy(g_dl_follow_msg, ft->status, sizeof(g_dl_follow_msg));
            }
        }
    }
    if (gen == last_gen) return;
    last_gen = gen;
    dlm_info_t d;
    for (int i = 0; i < 8 && dlm_get(i, &d); i++) {
        if (d.id <= g_dl_announced) break;
        if (d.state != DLM_DONE && d.state != DLM_FAILED) continue;
        g_dl_announced = d.id;
        char msg[200];
        if (d.state == DLM_DONE) {
            char sz[24];
            fmt_bytes(d.got, sz, sizeof(sz));
            ksnprintf(msg, sizeof(msg), "Downloaded %s (%s) - the arrow (or Ctrl+J) shows your downloads", d.name, sz);
            g_dl_new = 1;
        } else ksnprintf(msg, sizeof(msg), "Download failed: %s - %s", d.name, d.err);
        /* told where it was started (if that tab is still there), else in the one shown */
        tab_t* where = cur_tab();
        for (int k = 0; k < g_ntabs; k++) if (d.id == g_dl_follow && (void*)g_tabs[k] == g_dl_follow_tab) where = g_tabs[k];
        if (d.id == g_dl_follow) g_dl_follow = 0;
        g_status_tab = where;
        set_status(msg, d.state != DLM_DONE);
        g_status_tab = NULL;
        break;
    }
    g_gen++;
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

/* ── the toolbar's icons, drawn: 13x13 or so around cx, cy ─────────── */

/* back (dir -1) / forward (+1): a shaft and a head */
static void ic_arrow(int cx, int cy, int dir, uint32_t c) {
    gfx_fill_rect(cx - 5, cy - 1, 11, 2, c);
    int tip = cx + dir * 5;
    for (int i = 0; i < 5; i++) {
        int x = tip - dir * i - (dir > 0 ? 1 : 0);
        gfx_fill_rect(x, cy - 1 - i, 2, 2, c);
        gfx_fill_rect(x, cy - 1 + i, 2, 2, c);
    }
}

/* reload: a ring open at the top right, its end an arrowhead */
static void ic_reload(int cx, int cy, uint32_t c) {
    for (int dy = -6; dy <= 6; dy++)
        for (int dx = -6; dx <= 6; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 < 20 || d2 > 38) continue;
            if (dx > 0 && dy < 0 && -dy > dx / 2) continue;           /* the opening */
            gfx_fill_rect(cx + dx, cy + dy, 1, 1, c);
        }
    for (int i = 0; i < 4; i++) gfx_fill_rect(cx + 1 + i, cy - 9 + i, 1, 2 * (3 - i) + 1, c);
}

/* stop (while a page loads): an X */
static void ic_stop(int cx, int cy, uint32_t c) {
    for (int i = -4; i <= 4; i++) {
        gfx_fill_rect(cx + i, cy + i, 2, 1, c);
        gfx_fill_rect(cx + i, cy - i, 2, 1, c);
    }
}

/* home: a house - roof, walls, a door */
static void ic_home(int cx, int cy, uint32_t c, uint32_t bg) {
    for (int i = 0; i < 7; i++) gfx_fill_rect(cx - i, cy - 7 + i, 2 * i + 1, 1, c);
    gfx_fill_rect(cx - 5, cy, 11, 6, c);
    gfx_fill_rect(cx - 1, cy + 2, 3, 4, bg);
}

enum { IC_BACK, IC_FWD, IC_RELOAD, IC_STOP, IC_HOME };

static void icon_button(int x, int y, int w, int kind, int enabled) {
    uint32_t base = enabled ? 0x00303740u : 0x00262B33u, c = enabled ? C_TEXT : 0x00707A88u;
    tool_button(x, y, w, "", enabled);
    int cx = x + w / 2, cy = y + (TOOL_H - 2) / 2;
    switch (kind) {
    case IC_BACK:   ic_arrow(cx, cy, -1, c); break;
    case IC_FWD:    ic_arrow(cx, cy, 1, c); break;
    case IC_RELOAD: ic_reload(cx, cy + 1, c); break;
    case IC_STOP:   ic_stop(cx - 1, cy, c); break;
    case IC_HOME:   ic_home(cx, cy + 1, c, base); break;
    }
}

/* a padlock: https */
static void ic_lock(int x, int y, uint32_t c, uint32_t bg) {
    gfx_fill_rect(x + 2, y, 5, 5, c);                /* the shackle */
    gfx_fill_rect(x + 3, y + 1, 3, 4, bg);
    gfx_fill_rect(x, y + 4, 9, 7, c);                /* the body */
}

/* the part of an address in its colour; how many columns it took */
static int addr_part(int x, int y, const char* s, int n, int maxc, uint32_t fg, uint32_t bg) {
    char buf[200];
    if (n <= 0 || maxc <= 0) return 0;
    int nb = u8_byte_at_col(s, n, maxc);
    if (nb > (int)sizeof(buf) - 1) nb = (int)sizeof(buf) - 1;
    memcpy(buf, s, (size_t)nb);
    buf[nb] = 0;
    gfx_draw_text(x, y, buf, fg, bg);
    return u8_width(buf, nb);
}

/* the address bar: focused, the text being edited with its caret (all of
 * it selected right after the click); else the address as Chrome shows
 * it - a padlock and no "https://", the host dark and the rest grey */
static void draw_addr_bar(tab_t* t, int x0, int ty, int aw) {
    uint32_t abg = g_addr_focus ? 0x00FFFFFFu : 0x00E8EAEDu;
    gfx_fill_rect(x0 + 1, ty, aw - 2, TOOL_H - 2, abg);              /* (rounded a little) */
    gfx_fill_rect(x0, ty + 1, aw, TOOL_H - 4, abg);
    if (g_addr_focus) {
        gfx_fill_rect(x0 + 1, ty, aw - 2, 1, 0x001A73E8u);
        gfx_fill_rect(x0 + 1, ty + TOOL_H - 3, aw - 2, 1, 0x001A73E8u);
        gfx_fill_rect(x0, ty + 1, 1, TOOL_H - 4, 0x001A73E8u);
        gfx_fill_rect(x0 + aw - 1, ty + 1, 1, TOOL_H - 4, 0x001A73E8u);
    }
    const char* a = t ? t->addr : "";
    int len = (int)strlen(a);
    if (g_addr_focus) {
        int maxc = (aw - 12) / 8;
        if (g_addr_cur > len) g_addr_cur = len;
        if (g_addr_view > g_addr_cur) g_addr_view = g_addr_cur;
        while (u8_width(a + g_addr_view, g_addr_cur - g_addr_view) > maxc - 1) g_addr_view = u8_next(a, len, g_addr_view);
        int from = g_addr_all ? 0 : g_addr_view;
        if (g_addr_all && len) {
            int cols = u8_width(a, u8_byte_at_col(a, len, maxc));
            gfx_fill_rect(x0 + 4, ty + 4, cols * 8 + 2, 12, 0x00B4D5FEu);
            addr_part(x0 + 5, ty + 6, a, len, maxc, 0x00101010u, 0x00B4D5FEu);
        } else {
            addr_part(x0 + 5, ty + 6, a + from, len - from, maxc, 0x00101010u, abg);
            gfx_fill_rect(x0 + 5 + u8_width(a + g_addr_view, g_addr_cur - g_addr_view) * 8, ty + 4, 1, 12, 0x00202020u);
        }
        return;
    }
    int tx = x0 + 6;
    const char* host = a;
    const char* sep = strstr(a, "://");
    if (!strncasecmp(a, "https://", 8)) {
        ic_lock(tx, ty + 4, 0x005F6368u, abg);
        tx += 15;
        host = a + 8;
    } else if (sep) host = sep + 3;
    int maxc = (x0 + aw - 6 - tx) / 8, col = 0;
    if (host != a && strncasecmp(a, "https://", 8))                   /* http://, file://: grey */
        col += addr_part(tx, ty + 6, a, (int)(host - a), maxc, 0x005F6368u, abg);
    int hn = 0;
    while (host[hn] && host[hn] != '/' && host[hn] != '?' && host[hn] != '#') hn++;
    col += addr_part(tx + col * 8, ty + 6, host, hn, maxc - col, 0x00202124u, abg);
    addr_part(tx + col * 8, ty + 6, host + hn, (int)strlen(host + hn), maxc - col, 0x005F6368u, abg);
}

/* the suggestions under the address bar: each a title and an address */
static void draw_suggestions(int x0, int y0, int aw) {
    if (!g_addr_focus || !g_sug_n) return;
    int h = g_sug_n * SUG_H + 4;
    gfx_fill_rect(x0 + 3, y0 + 3, aw, h, 0x00101318u);               /* a shadow */
    gfx_fill_rect(x0, y0, aw, h, 0x00FFFFFFu);
    gfx_fill_rect(x0, y0 + h - 1, aw, 1, 0x00C0C4CCu);
    int maxc = (aw - 40) / 8;
    for (int i = 0; i < g_sug_n; i++) {
        int ry = y0 + 2 + i * SUG_H;
        uint32_t bg = i == g_sug_sel ? 0x00E8F0FEu : 0x00FFFFFFu;
        gfx_fill_rect(x0 + 1, ry, aw - 2, SUG_H, bg);
        /* a small globe: a page visited before */
        for (int dy = -5; dy <= 5; dy++) for (int dx = -5; dx <= 5; dx++) {
            int d2 = dx * dx + dy * dy;
            if ((d2 >= 18 && d2 <= 30) || (dx == 0 && d2 <= 30) || (dy == 0 && d2 <= 30))
                gfx_fill_rect(x0 + 16 + dx, ry + SUG_H / 2 + dy, 1, 1, 0x005F6368u);
        }
        const char* title = bh_title(g_sug[i]);
        const char* url = bh_url(g_sug[i]);
        const char* shown = !strncasecmp(url, "https://", 8) ? url + 8 : url;
        const char* top = title[0] ? title : shown;
        addr_part(x0 + 32, ry + 4, top, (int)strlen(top), maxc, 0x00202124u, bg);
        addr_part(x0 + 32, ry + 19, shown, (int)strlen(shown), maxc, 0x001A73E8u, bg);
    }
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
    icon_button(x + 4, ty, 26, IC_BACK, t && t->hist_pos > 0);
    icon_button(x + 32, ty, 26, IC_FWD, t && t->hist_pos < t->hist_n - 1);
    icon_button(x + 60, ty, 26, t && t->loading ? IC_STOP : IC_RELOAD, 1);
    icon_button(x + 88, ty, 32, IC_HOME, 1);
    int aw = addr_w();
    draw_addr_bar(t, x + ADDR_X, ty, aw);
    icon_button(x + ADDR_X + aw + 4, ty, 40, IC_FWD, 1);      /* Go */
    draw_dl_button(x + ADDR_X + aw + 48, ty);

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
    draw_suggestions(x + ADDR_X, ty + TOOL_H, aw);     /* over the page, under the address bar */

    /* "Choose a file to upload" */
    if (g_pick_on && fc_active()) {
        int fw = W - 40 < 640 ? W - 40 : 640, fh = H - VIEW_Y - 40 < 420 ? H - VIEW_Y - 40 : 420;
        fc_draw(x + (W - fw) / 2, y + VIEW_Y + 10, fw, fh);
    }

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
    if (g_pick_on && fc_active() && ly >= VIEW_Y) { fc_click(mx, my); return; }
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
        int was = g_addr_focus;
        g_addr_focus = 0;
        int aw = addr_w();
        if (lx >= 4 && lx < 30) push_cmd(CMD_BACK, 0, 0);
        else if (lx >= 32 && lx < 58) push_cmd(CMD_FWD, 0, 0);
        else if (lx >= 60 && lx < 86) push_cmd(CMD_RELOAD, 0, 0);
        else if (lx >= 88 && lx < 120) push_cmd(CMD_HOME, 0, 0);
        else if (lx >= ADDR_X && lx < ADDR_X + aw && t) {
            if (!was) addr_focus_all(t);                 /* the first click takes it all (Chrome) */
            else {                                       /* the next ones put the caret there */
                int len = (int)strlen(t->addr), col = (lx - ADDR_X - 5 + 4) / 8;
                g_addr_focus = 1;
                g_addr_all = 0;
                if (g_addr_view > len) g_addr_view = len;
                g_addr_cur = g_addr_view + u8_byte_at_col(t->addr + g_addr_view, len - g_addr_view, col < 0 ? 0 : col);
            }
        }
        else if (lx >= ADDR_X + aw + 48 && lx < ADDR_X + aw + 48 + DLBTN_W) push_cmd(CMD_DOWNLOADS, 0, 0);
        else if (lx >= ADDR_X + aw + 4 && lx < ADDR_X + aw + 44 && t) addr_go(t);
        return;
    }
    /* a suggestion under the address bar */
    if (g_addr_focus && g_sug_n && t && lx >= ADDR_X && lx < ADDR_X + addr_w() &&
        ly >= TOOL_Y + TOOL_H && ly < TOOL_Y + TOOL_H + 2 + g_sug_n * SUG_H) {
        int i = (ly - TOOL_Y - TOOL_H - 2) / SUG_H;
        if (i >= 0 && i < g_sug_n) { g_sug_sel = i; addr_go(t); }
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

/* the window manager (kernel/gui.c) moves, snaps and maximizes it through its geometry */
win_geom_t* browser_geom(void) { return &g_win; }
