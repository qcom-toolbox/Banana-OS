/* App Store - Banana OS's apps from the package repositories.
 *
 * The same packages as `apt` (kernel/repo.c, API version 11): Discover
 * shows everything the sources offer, by category; Updates the installed
 * apps that have a newer version; Installed what is on this computer.
 * Click an app for its page: Install (with what it needs), Open, Update,
 * Remove. Refresh fetches the lists again (apt update); the sources are
 * /etc/pkg/sources.list (apt add-repo adds one).
 *
 * Keys: type to search, Esc clears it / goes back, Up / Down / Enter pick
 * an app, Tab moves between the pages, F5 refreshes.
 */
#include <banana.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIDE_W   188
#define TOP_H    56
#define CARD_H   88
#define CARD_MINW 280
#define GAP      12
#define ICON     52
#define MAXPKG   512
#define MAXCAT   24

#define C_BG     0x161A20u
#define C_SIDE   0x1D232Cu
#define C_CARD   0x232A35u
#define C_CARDHI 0x2C3644u
#define C_SEL    0x2F4A6Eu
#define C_TEXT   0xE8EEF6u
#define C_DIM    0x9AA6B6u
#define C_ACCENT 0x3A7BD5u
#define C_GREEN  0x3FAE5Au
#define C_WARN   0xFFD27Au
#define C_LINE   0x2C3440u

#define SANS  BANANA_FONT_SANS
#define BOLD  BANANA_FONT_SANS_BOLD

enum { PG_DISCOVER = 0, PG_UPDATES, PG_INSTALLED, PG_CATEGORY };
enum { JOB_NONE = 0, JOB_UPDATE, JOB_INSTALL, JOB_REMOVE, JOB_UPGRADE_ALL, JOB_ICONS };

static bwin_t win;
static banana_pkg_t pkgs[MAXPKG];
static int    npkgs;

/* each package's picture: path found by the worker, pixels loaded here */
typedef struct { volatile int state; char path[160]; unsigned int* px; } icon_t;   /* 0 unknown, 1 path, 2 none, 3 loaded */
static icon_t icons[MAXPKG];

static char   cats[MAXCAT][24];
static int    ncats;

static int    page = PG_DISCOVER, cat = -1;
static char   search[64];
static int    shown[MAXPKG], nshown;
static int    scroll;                      /* pixels */
static int    sel = -1;                    /* index in shown (keyboard) */
static int    detail = -1;                 /* index in pkgs, or -1: the list */
static int    hover_btn = -1;

static char   status[200];
static int    status_err;

/* the worker thread: one job at a time */
static volatile int job, job_done = 1, job_rc;
static char   job_name[32], job_msg[200];

/* ── small helpers ── */
static int has_ci(const char* hay, const char* w) {
    int n = (int)strlen(w);
    if (!n) return 1;
    for (const char* h = hay; *h; h++) if (!strncasecmp(h, w, n)) return 1;
    return 0;
}

static unsigned int name_color(const char* s) {
    static const unsigned int pal[] = { 0x3A7BD5u, 0x57B65Au, 0xE07040u, 0x8A5CD0u, 0xD0A030u, 0x30A8A8u, 0xC04C70u };
    unsigned int h = 0;
    while (*s) h = h * 31u + (unsigned char)*s++;
    return pal[h % 7];
}

/* text cut to fit w pixels, with "..." */
static int text_fit(int x, int y, int font, int size, const char* s, int w, unsigned int color) {
    if (banana_font_width(font, size, s) <= w) return bwin_font(&win, x, y, font, size, s, color);
    char buf[256];
    int n = (int)strlen(s);
    if (n > 250) n = 250;
    while (n > 0) {
        memcpy(buf, s, n);
        /* not inside a UTF-8 character */
        while (n > 0 && (buf[n] & 0xC0) == 0x80) n--;
        strcpy(buf + n, "...");
        if (banana_font_width(font, size, buf) <= w) break;
        n--;
    }
    return bwin_font(&win, x, y, font, size, buf, color);
}

/* wrapped text in w pixels from y; returns the y below it */
static int text_wrap(int x, int y, int font, int size, const char* s, int w, unsigned int color, int max_lines) {
    int asc, desc, lh;
    banana_font_metrics(font, size, &asc, &desc, &lh);
    char line[256];
    int lines = 0;
    while (*s && lines < max_lines) {
        int n = 0, last_space = -1;
        while (s[n] && n < 250) {
            line[n] = s[n];
            line[n + 1] = 0;
            if (s[n] == ' ') last_space = n;
            if (banana_font_width(font, size, line) > w) break;
            n++;
        }
        int take = n;
        if (s[n] && s[n] != ' ' && last_space > 0) take = last_space;
        if (take <= 0) take = n > 0 ? n : 1;
        memcpy(line, s, take);
        line[take] = 0;
        bwin_font(&win, x, y, font, size, line, color);
        y += lh + 2;
        s += take;
        while (*s == ' ') s++;
        lines++;
    }
    return y;
}

static void set_status(int err, const char* fmt, const char* a) {
    snprintf(status, sizeof(status), fmt, a ? a : "");
    status_err = err;
}

/* ── the package list ── */
static int is_installed(const banana_pkg_t* p) { return (p->flags & BANANA_PKG_INSTALLED) != 0; }
static int is_upgradable(const banana_pkg_t* p) { return (p->flags & BANANA_PKG_UPGRADABLE) != 0; }

static int cmp_title(const void* a, const void* b) {
    return strcasecmp(((const banana_pkg_t*)a)->title, ((const banana_pkg_t*)b)->title);
}

static void filter(void) {
    nshown = 0;
    for (int i = 0; i < npkgs; i++) {
        const banana_pkg_t* p = &pkgs[i];
        if (!strcmp(p->type, "driver")) continue;            /* drivers: apt */
        if (page == PG_UPDATES && !is_upgradable(p)) continue;
        if (page == PG_INSTALLED && !is_installed(p)) continue;
        if (page == PG_DISCOVER && !(p->flags & BANANA_PKG_AVAILABLE)) continue;
        if (page == PG_CATEGORY && cat >= 0 && strcmp(p->category[0] ? p->category : "Other", cats[cat])) continue;
        if (search[0] && !has_ci(p->title, search) && !has_ci(p->name, search) && !has_ci(p->description, search) &&
            !has_ci(p->category, search)) continue;
        shown[nshown++] = i;
    }
    if (sel >= nshown) sel = nshown - 1;
}

static int updates_count(void) {
    int n = 0;
    for (int i = 0; i < npkgs; i++) if (is_upgradable(&pkgs[i])) n++;
    return n;
}

static void reload(void) {
    char keep[32] = "";
    if (detail >= 0) snprintf(keep, sizeof(keep), "%s", pkgs[detail].name);
    for (int i = 0; i < npkgs; i++) { free(icons[i].px); }
    memset(icons, 0, sizeof(icons));
    npkgs = banana_repo_list(pkgs, MAXPKG);
    qsort(pkgs, npkgs, sizeof(pkgs[0]), cmp_title);
    detail = -1;
    for (int i = 0; i < npkgs; i++) if (keep[0] && !strcmp(pkgs[i].name, keep)) detail = i;
    /* the categories that have something */
    ncats = 0;
    for (int i = 0; i < npkgs; i++) {
        if (!(pkgs[i].flags & BANANA_PKG_AVAILABLE) || !strcmp(pkgs[i].type, "driver")) continue;
        const char* c = pkgs[i].category[0] ? pkgs[i].category : "Other";
        int found = 0;
        for (int k = 0; k < ncats; k++) if (!strcmp(cats[k], c)) found = 1;
        if (!found && ncats < MAXCAT) snprintf(cats[ncats++], sizeof(cats[0]), "%s", c);
    }
    for (int a = 1; a < ncats; a++)                        /* A-Z, "Other" last */
        for (int b = a; b > 0 && (strcmp(cats[b - 1], "Other") == 0 || (strcmp(cats[b], "Other") && strcasecmp(cats[b - 1], cats[b]) > 0)); b--) {
            char t[24];
            memcpy(t, cats[b], 24); memcpy(cats[b], cats[b - 1], 24); memcpy(cats[b - 1], t, 24);
        }
    if (cat >= ncats) { cat = -1; if (page == PG_CATEGORY) page = PG_DISCOVER; }
    filter();
}

/* ── the worker ── */
static int worker(void* arg) {
    (void)arg;
    int j = job;
    job_msg[0] = 0;
    if (j == JOB_UPDATE) job_rc = banana_repo_update(job_msg, sizeof(job_msg));
    else if (j == JOB_INSTALL) job_rc = banana_repo_install(job_name, job_msg, sizeof(job_msg));
    else if (j == JOB_REMOVE) job_rc = banana_repo_remove(job_name, job_msg, sizeof(job_msg));
    else if (j == JOB_UPGRADE_ALL) job_rc = banana_repo_upgrade(job_msg, sizeof(job_msg));
    else if (j == JOB_ICONS) {
        for (int i = 0; i < npkgs; i++) {
            if (icons[i].state) continue;
            char p[160];
            if (banana_repo_icon(pkgs[i].name, p, sizeof(p)) == 0) { snprintf(icons[i].path, sizeof(icons[i].path), "%s", p); icons[i].state = 1; }
            else icons[i].state = 2;
        }
        job_rc = 0;
    }
    job_done = 1;
    return 0;
}

static int busy(void) { return !job_done; }

static void start_job(int j, const char* name) {
    if (busy()) return;
    job = j;
    snprintf(job_name, sizeof(job_name), "%s", name ? name : "");
    job_done = 0;
    if (banana_thread(worker, NULL) < 0) { worker(NULL); }
}

static void job_finished(void) {
    int j = job;
    job = JOB_NONE;
    if (j == JOB_ICONS) return;
    if (job_rc < 0) set_status(1, "%s", job_msg[0] ? job_msg : "it did not work");
    else if (job_msg[0]) set_status(0, "%s", job_msg);
    reload();
    start_job(JOB_ICONS, NULL);                 /* pictures of new packages */
}

/* ── layout ── */
static int main_x(void) { return SIDE_W; }
static int main_w(void) { return win.w - SIDE_W; }
static int cols(void) { int c = (main_w() - GAP) / (CARD_MINW + GAP); return c < 1 ? 1 : c; }
static int card_w(void) { return (main_w() - GAP * (cols() + 1)) / cols(); }
static int list_y(void) { return TOP_H; }
static int list_h(void) { return win.h - TOP_H - (busy() && job != JOB_ICONS ? 34 : 0) - 24; }

static void card_rect(int k, int* x, int* y) {
    int c = cols();
    *x = main_x() + GAP + (k % c) * (card_w() + GAP);
    *y = list_y() + GAP + (k / c) * (CARD_H + GAP) - scroll;
}

static int content_h(void) {
    int rows = (nshown + cols() - 1) / cols();
    return GAP + rows * (CARD_H + GAP);
}

static void clamp_scroll(void) {
    int max = content_h() - list_h();
    if (scroll > max) scroll = max;
    if (scroll < 0) scroll = 0;
}

/* the sidebar's entries: 0 Discover, 1 Updates, 2 Installed, 3+ categories */
static int side_y(int i) { return 64 + i * 30 + (i >= 3 ? 30 : 0); }

/* ── drawing ── */
static void draw_icon(int i, int x, int y, int size) {
    icon_t* ic = &icons[i];
    if (ic->state == 1) {                       /* load it now (once) */
        int w, h;
        char e[64];
        unsigned int* src = banana_image_load(ic->path, &w, &h, e, sizeof(e));
        ic->state = 2;
        if (src && w > 0 && h > 0) {
            ic->px = malloc((size_t)ICON * 2 * ICON * 2 * 4);
            if (ic->px) {
                int S = ICON * 2;               /* kept at 104 px: enough for the app's page */
                for (int yy = 0; yy < S; yy++)
                    for (int xx = 0; xx < S; xx++) ic->px[yy * S + xx] = src[(long)(yy * h / S) * w + xx * w / S];
                ic->state = 3;
            }
        }
        free(src);
    }
    if (ic->state == 3 && ic->px) {
        int S = ICON * 2;
        for (int yy = 0; yy < size; yy++) {
            int py = y + yy;
            if (py < 0 || py >= win.h) continue;
            for (int xx = 0; xx < size; xx++) {
                int px = x + xx;
                if (px < 0 || px >= win.w) continue;
                win.px[py * win.w + px] = ic->px[(yy * S / size) * S + xx * S / size];
            }
        }
        return;
    }
    unsigned int c = name_color(pkgs[i].name);
    bwin_fill_rect(&win, x, y, size, size, c);
    bwin_fill_rect(&win, x, y, size, 2, c + 0x202020u);
    char l[2] = { pkgs[i].title[0], 0 };
    if (l[0] >= 'a' && l[0] <= 'z') l[0] -= 32;
    int fs = size * 6 / 10;
    int tw = banana_font_width(BOLD, fs, l);
    bwin_font(&win, x + (size - tw) / 2, y + (size - fs) / 2 - 2, BOLD, fs, l, 0xFFFFFFu);
}

/* the action a package's button does, and its label */
enum { ACT_NONE = 0, ACT_INSTALL, ACT_OPEN, ACT_UPDATE, ACT_REMOVE };
static int main_action(const banana_pkg_t* p, const char** label) {
    if (is_upgradable(p)) { *label = "Update"; return ACT_UPDATE; }
    if (is_installed(p)) { *label = !strcmp(p->type, "gui") ? "Open" : "Run"; return ACT_OPEN; }
    if (p->flags & BANANA_PKG_AVAILABLE) { *label = "Get"; return ACT_INSTALL; }
    *label = "";
    return ACT_NONE;
}

static void pill(int x, int y, int w, int h, const char* label, unsigned int bg, unsigned int fg) {
    bwin_fill_rect(&win, x + 2, y, w - 4, h, bg);
    bwin_fill_rect(&win, x, y + 2, w, h - 4, bg);
    bwin_fill_rect(&win, x + 1, y + 1, w - 2, h - 2, bg);
    int tw = banana_font_width(BOLD, 13, label);
    bwin_font(&win, x + (w - tw) / 2, y + (h - 13) / 2 - 1, BOLD, 13, label, fg);
}

static int working_on(const banana_pkg_t* p) {
    return busy() && (job == JOB_INSTALL || job == JOB_REMOVE) && !strcmp(job_name, p->name);
}

static void draw_card(int k) {
    int i = shown[k];
    const banana_pkg_t* p = &pkgs[i];
    int x, y;
    card_rect(k, &x, &y);
    int w = card_w();
    if (y + CARD_H < list_y() || y > list_y() + list_h()) return;
    bwin_fill_rect(&win, x, y, w, CARD_H, k == sel ? C_SEL : C_CARD);
    draw_icon(i, x + 14, y + (CARD_H - ICON) / 2, ICON);
    int tx = x + 14 + ICON + 14, bw = 78, tw = w - (tx - x) - bw - 22;
    text_fit(tx, y + 14, BOLD, 15, p->title, tw, C_TEXT);
    char sub[96];
    snprintf(sub, sizeof(sub), "%s  -  %s", p->category[0] ? p->category : "Other", is_installed(p) ? p->installed : p->version);
    text_fit(tx, y + 36, SANS, 12, sub, tw, C_DIM);
    text_fit(tx, y + 56, SANS, 13, p->description[0] ? p->description : p->name, tw, 0xC8D0DCu);
    const char* label;
    int a = main_action(p, &label);
    if (working_on(p)) pill(x + w - bw - 14, y + (CARD_H - 28) / 2, bw, 28, "...", C_LINE, C_DIM);
    else if (a) pill(x + w - bw - 14, y + (CARD_H - 28) / 2, bw, 28, label, a == ACT_INSTALL || a == ACT_UPDATE ? C_ACCENT : C_LINE, 0xFFFFFFu);
}

/* the app's page: buttons from x, y; returns how many */
static int detail_buttons(const banana_pkg_t* p, int* acts, const char** labels) {
    int n = 0;
    if (is_upgradable(p)) { acts[n] = ACT_UPDATE; labels[n++] = "Update"; }
    if (is_installed(p)) {
        if (strcmp(p->type, "driver")) { acts[n] = ACT_OPEN; labels[n++] = !strcmp(p->type, "gui") ? "Open" : "Run"; }
        acts[n] = ACT_REMOVE; labels[n++] = "Remove";
    } else if (p->flags & BANANA_PKG_AVAILABLE) { acts[n] = ACT_INSTALL; labels[n++] = "Install"; }
    return n;
}

static void draw_detail(void) {
    const banana_pkg_t* p = &pkgs[detail];
    int x = main_x() + 28, y = TOP_H + 16, w = main_w() - 56;
    pill(main_x() + 16, 14, 70, 28, "< Back", C_LINE, C_TEXT);
    draw_icon(detail, x, y, ICON * 2);
    int tx = x + ICON * 2 + 24;
    text_fit(tx, y + 6, BOLD, 24, p->title, w - (tx - x), C_TEXT);
    char line[200];
    snprintf(line, sizeof(line), "%s%s%s", p->author[0] ? p->author : "", p->author[0] ? "  -  " : "", p->category[0] ? p->category : "Other");
    text_fit(tx, y + 40, SANS, 14, line, w - (tx - x), C_DIM);
    int acts[3];
    const char* labels[3];
    int nb = detail_buttons(p, acts, labels);
    for (int b = 0; b < nb; b++) {
        int bx = tx + b * 112;
        if (working_on(p)) { pill(bx, y + 68, 100, 32, b == 0 ? "Working..." : "", C_LINE, C_DIM); break; }
        pill(bx, y + 68, 100, 32, labels[b], acts[b] == ACT_REMOVE || acts[b] == ACT_OPEN ? C_LINE : C_ACCENT,
             acts[b] == ACT_REMOVE ? 0xFF8A80u : 0xFFFFFFu);
    }
    y += ICON * 2 + 30;
    bwin_fill_rect(&win, x, y, w, 1, C_LINE);
    y += 16;
    y = text_wrap(x, y, SANS, 15, p->description[0] ? p->description : "No description.", w, C_TEXT, 8) + 14;
    struct { const char* k; char v[160]; } rows[7];
    int nr = 0;
    rows[nr].k = "Package"; snprintf(rows[nr++].v, 160, "%s", p->name);
    if (p->flags & BANANA_PKG_AVAILABLE) { rows[nr].k = "Version"; snprintf(rows[nr++].v, 160, "%s", p->version); }
    rows[nr].k = "Installed";
    if (is_installed(p)) snprintf(rows[nr++].v, 160, "%s%s", p->installed, (p->flags & BANANA_PKG_AUTO) ? " (for another app)" : "");
    else snprintf(rows[nr++].v, 160, "no");
    rows[nr].k = "Kind"; snprintf(rows[nr++].v, 160, "%s", !strcmp(p->type, "gui") ? "desktop app" : !strcmp(p->type, "driver") ? "driver" : "terminal app");
    if (p->size) { rows[nr].k = "Download"; snprintf(rows[nr++].v, 160, "%u KB", (p->size + 1023) / 1024); }
    if (p->depends[0]) { rows[nr].k = "Needs"; snprintf(rows[nr++].v, 160, "%s", p->depends); }
    for (int r = 0; r < nr; r++) {
        bwin_font(&win, x, y, SANS, 13, rows[r].k, C_DIM);
        text_fit(x + 110, y, SANS, 13, rows[r].v, w - 110, C_TEXT);
        y += 22;
    }
}

static void draw_sidebar(void) {
    bwin_fill_rect(&win, 0, 0, SIDE_W, win.h, C_SIDE);
    bwin_fill_rect(&win, SIDE_W - 1, 0, 1, win.h, C_LINE);
    bwin_font(&win, 18, 18, BOLD, 20, "App Store", C_TEXT);
    const char* names[3] = { "Discover", "Updates", "Installed" };
    int n_up = updates_count();
    for (int i = 0; i < 3 + ncats; i++) {
        int y = side_y(i);
        int active = (i < 3 && page == i && detail < 0) || (i >= 3 && page == PG_CATEGORY && cat == i - 3 && detail < 0);
        if (active) bwin_fill_rect(&win, 8, y - 6, SIDE_W - 16, 28, C_SEL);
        if (i < 3) {
            bwin_font(&win, 22, y, active ? BOLD : SANS, 14, names[i], C_TEXT);
            if (i == 1 && n_up) {
                char c[8];
                snprintf(c, sizeof(c), "%d", n_up);
                pill(SIDE_W - 48, y - 3, 30, 20, c, C_ACCENT, 0xFFFFFFu);
            }
        } else {
            text_fit(22, y, active ? BOLD : SANS, 13, cats[i - 3], SIDE_W - 40, active ? C_TEXT : 0xC8D0DCu);
        }
    }
    if (ncats) bwin_font(&win, 22, side_y(3) - 26, BOLD, 11, "CATEGORIES", C_DIM);
    pill(16, win.h - 64, SIDE_W - 32, 30, busy() && job == JOB_UPDATE ? "Refreshing..." : "Refresh", C_LINE, C_TEXT);
}

static void draw_top(void) {
    int x = main_x() + 16, w = main_w() - 32;
    if (detail >= 0) return;
    /* search box */
    int sw = w > 420 ? 360 : w - 60;
    bwin_fill_rect(&win, x, 12, sw, 32, 0x0F1217u);
    bwin_rect(&win, x, 12, sw, 32, C_LINE);
    if (search[0]) text_fit(x + 12, 20, SANS, 14, search, sw - 24, C_TEXT);
    else bwin_font(&win, x + 12, 20, SANS, 14, "Search apps", 0x6A7686u);
    if (search[0]) {
        int cx = x + 12 + banana_font_width(SANS, 14, search) + 1;
        if (cx < x + sw - 8) bwin_fill_rect(&win, cx, 19, 1, 18, C_TEXT);
    }
    if (page == PG_UPDATES && updates_count() > 0)
        pill(x + w - 120, 12, 120, 32, busy() && job == JOB_UPGRADE_ALL ? "Updating..." : "Update all", C_ACCENT, 0xFFFFFFu);
}

static void draw_list(void) {
    clamp_scroll();
    if (!nshown) {
        const char* m;
        if (search[0]) m = "No app matches your search.";
        else if (page == PG_UPDATES) m = "Everything is up to date.";
        else if (page == PG_INSTALLED) m = "Nothing installed from a repository yet.";
        else if (busy()) m = "Getting the list of apps...";
        else m = "No apps yet - Refresh, or add a repository: apt add-repo <url> key=<key>";
        int tw = banana_font_width(SANS, 15, m);
        int x = main_x() + (main_w() - tw) / 2;
        if (x < main_x() + 16) x = main_x() + 16;
        bwin_font(&win, x, TOP_H + 80, SANS, 15, m, C_DIM);
        return;
    }
    for (int k = 0; k < nshown; k++) draw_card(k);
    /* the scroll bar */
    int ch = content_h(), lh = list_h();
    if (ch > lh) {
        int bh = lh * lh / ch;
        if (bh < 24) bh = 24;
        int by = list_y() + (lh - bh) * scroll / (ch - lh);
        bwin_fill_rect(&win, win.w - 6, by, 4, bh, 0x4A5566u);
    }
}

static void draw(void) {
    bwin_fill_rect(&win, SIDE_W, 0, win.w - SIDE_W, win.h, C_BG);
    if (detail >= 0) draw_detail();
    else draw_list();
    /* cards scrolled under the top bar are covered by it */
    if (detail < 0) bwin_fill_rect(&win, SIDE_W, 0, win.w - SIDE_W, TOP_H, C_BG);
    draw_top();
    draw_sidebar();
    /* progress */
    int by = win.h - 24;
    if (busy() && job != JOB_ICONS) {
        char m[200];
        int pct = banana_repo_status(m, sizeof(m));
        if (pct < 0) pct = 0;
        int y = win.h - 58, x = main_x() + 16, w = main_w() - 32;
        bwin_fill_rect(&win, SIDE_W, y - 6, win.w - SIDE_W, 40, C_SIDE);
        text_fit(x, y, SANS, 12, m[0] ? m : "Working...", w, C_TEXT);
        bwin_fill_rect(&win, x, y + 18, w, 6, C_LINE);
        bwin_fill_rect(&win, x, y + 18, w * pct / 100, 6, C_ACCENT);
    }
    bwin_fill_rect(&win, SIDE_W, by, win.w - SIDE_W, 24, C_SIDE);
    char st[220];
    if (status[0]) snprintf(st, sizeof(st), "%s", status);
    else snprintf(st, sizeof(st), "%d app%s", nshown, nshown == 1 ? "" : "s");
    text_fit(main_x() + 12, by + 5, SANS, 12, st, main_w() - 24, status[0] && status_err ? 0xFF8A80u : status[0] ? C_GREEN : C_DIM);
    bwin_update(&win);
}

/* ── actions ── */
static void do_action(int i, int a) {
    banana_pkg_t* p = &pkgs[i];
    char e[128];
    if (busy()) { set_status(1, "%s", "Wait for the current install to finish"); return; }
    status[0] = 0;
    if (a == ACT_INSTALL || a == ACT_UPDATE) start_job(JOB_INSTALL, p->name);
    else if (a == ACT_REMOVE) start_job(JOB_REMOVE, p->name);
    else if (a == ACT_OPEN) {
        if (banana_app_run(p->name, 0, NULL, e, sizeof(e)) == 0) set_status(0, "Started %s", p->title);
        else set_status(1, "%s", e);
    }
}

static void go_page(int pg, int c) {
    search[0] = 0;
    page = pg;
    cat = c;
    detail = -1;
    scroll = 0;
    sel = -1;
    filter();
}

static void click(int mx, int my) {
    /* the sidebar */
    if (mx < SIDE_W) {
        if (my >= win.h - 64 && my < win.h - 34) { if (!busy()) { status[0] = 0; start_job(JOB_UPDATE, NULL); } return; }
        for (int i = 0; i < 3 + ncats; i++) {
            int y = side_y(i);
            if (my >= y - 6 && my < y + 22) { if (i < 3) go_page(i, -1); else go_page(PG_CATEGORY, i - 3); return; }
        }
        return;
    }
    if (detail >= 0) {
        if (mx >= main_x() + 16 && mx < main_x() + 86 && my >= 14 && my < 42) { detail = -1; filter(); return; }
        const banana_pkg_t* p = &pkgs[detail];
        int acts[3];
        const char* labels[3];
        int nb = detail_buttons(p, acts, labels);
        int tx = main_x() + 28 + ICON * 2 + 24, y = TOP_H + 16 + 68;
        for (int b = 0; b < nb; b++)
            if (mx >= tx + b * 112 && mx < tx + b * 112 + 100 && my >= y && my < y + 32) { do_action(detail, acts[b]); return; }
        return;
    }
    if (my < TOP_H) {
        int x = main_x() + 16, w = main_w() - 32;
        if (page == PG_UPDATES && updates_count() > 0 && mx >= x + w - 120 && mx < x + w && !busy()) { status[0] = 0; start_job(JOB_UPGRADE_ALL, NULL); }
        return;
    }
    for (int k = 0; k < nshown; k++) {
        int x, y;
        card_rect(k, &x, &y);
        int w = card_w();
        if (mx < x || mx >= x + w || my < y || my >= y + CARD_H || my >= list_y() + list_h()) continue;
        const char* label;
        int a = main_action(&pkgs[shown[k]], &label);
        if (a && mx >= x + w - 92 && mx < x + w - 14) do_action(shown[k], a);
        else { detail = shown[k]; sel = k; }
        return;
    }
}

static void key(int k) {
    if (k == 27) {
        if (detail >= 0) { detail = -1; filter(); }
        else if (search[0]) { search[0] = 0; filter(); scroll = 0; }
        return;
    }
    if (k == BANANA_KEY_F1 + 4) { if (!busy()) start_job(JOB_UPDATE, NULL); return; }
    if (k == '\t') { go_page(page == PG_CATEGORY ? PG_DISCOVER : (page + 1) % 3, -1); return; }
    if (detail >= 0) {
        if (k == '\b') { detail = -1; filter(); }
        else if (k == '\n' || k == '\r') {
            int acts[3];
            const char* labels[3];
            if (detail_buttons(&pkgs[detail], acts, labels) > 0) do_action(detail, acts[0]);
        }
        return;
    }
    if (k == BANANA_KEY_DOWN || k == BANANA_KEY_RIGHT) { if (sel + 1 < nshown) sel++; }
    else if (k == BANANA_KEY_UP || k == BANANA_KEY_LEFT) { if (sel > 0) sel--; }
    else if (k == BANANA_KEY_PGDN) scroll += list_h() - CARD_H;
    else if (k == BANANA_KEY_PGUP) scroll -= list_h() - CARD_H;
    else if ((k == '\n' || k == '\r') && sel >= 0) { detail = shown[sel]; return; }
    else if (k == '\b') { int n = (int)strlen(search); if (n) { search[n - 1] = 0; filter(); scroll = 0; } return; }
    else if (k >= 32 && k < 127) {
        int n = (int)strlen(search);
        if (n < (int)sizeof(search) - 1) { search[n] = (char)k; search[n + 1] = 0; }
        if (page == PG_UPDATES || page == PG_INSTALLED) page = PG_DISCOVER, cat = -1;
        filter();
        scroll = 0;
        sel = nshown ? 0 : -1;
        return;
    }
    /* keep the selected card in view */
    if (sel >= 0) {
        int x, y;
        card_rect(sel, &x, &y);
        if (y < list_y()) scroll -= list_y() - y + GAP;
        else if (y + CARD_H > list_y() + list_h()) scroll += y + CARD_H - (list_y() + list_h()) + GAP;
    }
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    if (bwin_open(&win, "App Store", 900, 600) != 0) {
        printf("store: the App Store is a desktop app - start the desktop (startx)\n");
        return 1;
    }
    bwin_resizable(&win, 660, 420);
    bwin_wheel(&win);
    reload();
    /* the lists come from the sources: fetched when opened */
    set_status(0, "%s", "Checking the repositories...");
    start_job(JOB_UPDATE, NULL);
    draw();
    banana_event_t ev;
    for (;;) {
        int got = bwin_wait_event(&win, &ev, busy() ? 150 : 1000);
        int redraw = 0;
        if (job != JOB_NONE && job_done) { job_finished(); redraw = 1; }
        if (busy()) redraw = 1;                 /* the progress bar */
        if (!got) { if (redraw) draw(); continue; }
        redraw = 1;
        if (ev.type == BANANA_EV_CLOSE) break;
        if (ev.type == BANANA_EV_KEY) key(ev.key);
        else if (ev.type == BANANA_EV_MOUSE_DOWN && ev.button == 1) click(ev.x, ev.y);
        else if (ev.type == BANANA_EV_WHEEL) { if (detail < 0) scroll += ev.y * 48; }
        else if (ev.type == BANANA_EV_RESIZE) {}
        else if (ev.type == BANANA_EV_MOUSE_MOVE) redraw = hover_btn != -1 ? (hover_btn = -1, 1) : 0;
        else redraw = 0;
        if (redraw) draw();
    }
    /* an install still running finishes in the system; just leave */
    while (busy()) banana_sleep(50);
    bwin_close(&win);
    return 0;
}
