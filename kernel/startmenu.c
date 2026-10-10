/*
 * The Start menu, Windows 7 style (startmenu.h).
 *
 *   +------------------------------+--------------------+
 *   | [##] Browser                 |      [picture]     |
 *   | [##] Files                   |  banana            |
 *   |------------------------------|  Documents         |
 *   | [##] Terminal                |  Pictures          |
 *   | [##] Notepad                 |  Music             |
 *   | ...                          |  Downloads         |
 *   |------------------------------|  Computer          |
 *   |  > All Programs              |  Settings ...      |
 *   +------------------------------+                    |
 *   [ Search programs and files  ]   [Shut down][>]     |
 *   +---------------------------------------------------+
 */
#include "startmenu.h"
#include "gfx.h"
#include "kstring.h"
#include "fs.h"
#include "pkg.h"
#include "timer.h"
#include "ctxmenu.h"
#include "utf8.h"
#include "fileicons.h"

#define HOME     "/home/banana"
#define LW       256                /* the white programs pane */
#define RW       160                /* the places column */
#define W        (LW + RW + 18)
#define BOTTOM_H 42                 /* the search box / Shut down strip */
#define BIG_ROW  38
#define SMALL_ROW 22
#define ALL_ROW  28                 /* "All Programs" */
#define R_ROW    26
#define BAR_H    28                 /* (kernel/gui.c's taskbar) */

#define C_GLASS1  0x00284F80u
#define C_GLASS2  0x00163659u
#define C_EDGE    0x000B2340u
#define C_EDGE_HI 0x006E9FD3u
#define C_WHITE   0x00FFFFFFu
#define C_TEXT    0x001E1E1Eu
#define C_DIM     0x006D6D6Du
#define C_HEAD    0x001E395Bu
#define C_HL      0x00DCEBFCu
#define C_HL_B    0x007DA2CEu
#define C_SEPL    0x00D5DFE9u
#define C_RHL     0x003C6EA7u
#define C_RHL_B   0x008DB8E8u

static startmenu_host_t g_host;
static uint32_t g_gen;

/* ── the left pane's entries ─────────────────────────────────────── */

enum { E_PROG = 1, E_FILE, E_HEAD, E_SEP, E_NONE };
typedef struct {
    int  kind;
    int  sm;                         /* E_PROG: SM_*, or 0 for an installed app */
    char app[PKG_NAME_MAX];
    char label[48];
    char path[FS_PATH_LEN];          /* E_FILE */
} entry_t;

#define MAX_ENTRIES 64
static entry_t g_e[MAX_ENTRIES];
static int     g_ne;
static int     g_mode;               /* 0 the usual list, 1 All Programs, 2 search results */
static char    g_query[40];
static int     g_scroll;             /* All Programs / search: first entry shown */
static int     g_sel = -1;           /* the keyboard's entry */
static int     g_hl = -1, g_hr = -1, g_hbtn;   /* under the pointer: left entry, right item, button */
static int     g_esc;

static const struct { int sm; const char* label; } BUILTIN[] = {
    { SM_BROWSER, "Browser" }, { SM_FILES, "Files" }, { SM_TERMINAL, "Terminal" }, { SM_NOTEPAD, "Notepad" },
    { SM_TASKMGR, "Task Manager" }, { SM_APPS, "Apps" }, { SM_SETTINGS, "Settings" },
};
#define NBUILTIN (int)(sizeof(BUILTIN) / sizeof(BUILTIN[0]))

static entry_t* add(int kind) {
    if (g_ne >= MAX_ENTRIES) return NULL;
    entry_t* e = &g_e[g_ne++];
    memset(e, 0, sizeof(*e));
    e->kind = kind;
    return e;
}
static void add_prog(int sm, const char* app, const char* label) {
    entry_t* e = add(E_PROG);
    if (!e) return;
    e->sm = sm;
    kstrlcpy(e->app, app ? app : "", sizeof(e->app));
    kstrlcpy(e->label, label, sizeof(e->label));
}
static void add_head(const char* label) { entry_t* e = add(E_HEAD); if (e) kstrlcpy(e->label, label, sizeof(e->label)); }

static int contains_ci(const char* s, const char* q) {
    size_t n = strlen(q);
    if (!n) return 1;
    for (; *s; s++) if (strncasecmp(s, q, n) == 0) return 1;
    return 0;
}

static pkg_info_t g_pkgs[48];

/* every program: the built-in ones and the installed apps, A-Z (All Programs, search) */
static void add_all_programs(const char* q) {
    int np = pkg_list(g_pkgs, 48);
    const char* lab[NBUILTIN + 48];
    int sm[NBUILTIN + 48], pi[NBUILTIN + 48], n = 0;
    for (int i = 0; i < NBUILTIN; i++) { lab[n] = BUILTIN[i].label; sm[n] = BUILTIN[i].sm; pi[n] = -1; n++; }
    for (int i = 0; i < np && i < 48; i++) {
        lab[n] = g_pkgs[i].title[0] ? g_pkgs[i].title : g_pkgs[i].name; sm[n] = 0; pi[n] = i; n++;
    }
    for (int i = 1; i < n; i++)                         /* A-Z */
        for (int j = i; j > 0 && strcasecmp(lab[j], lab[j - 1]) < 0; j--) {
            const char* tl = lab[j]; lab[j] = lab[j - 1]; lab[j - 1] = tl;
            int ts = sm[j]; sm[j] = sm[j - 1]; sm[j - 1] = ts;
            int tp = pi[j]; pi[j] = pi[j - 1]; pi[j - 1] = tp;
        }
    for (int i = 0; i < n; i++)
        if (contains_ci(lab[i], q) || (pi[i] >= 0 && contains_ci(g_pkgs[pi[i]].name, q)))
            add_prog(sm[i], pi[i] >= 0 ? g_pkgs[pi[i]].name : NULL, lab[i]);
}

/* files whose name has the query, under the home folder (a few) */
static void add_files(const char* q, int max) {
    static char queue[48][FS_PATH_LEN];
    static int idx[FS_MAX_FILES];
    int qh = 0, qt = 0, found = 0;
    kstrlcpy(queue[qt++], HOME, FS_PATH_LEN);
    while (qh < qt && found < max) {
        char dir[FS_PATH_LEN];
        kstrlcpy(dir, queue[qh++], sizeof(dir));
        int nf = fs_list_files(dir, idx, FS_MAX_FILES);
        for (int i = 0; i < nf && i < FS_MAX_FILES && found < max; i++) {
            fs_file_t* f = fs_file_info(idx[i]);
            if (!f || !contains_ci(f->name, q)) continue;
            entry_t* e = add(E_FILE);
            if (!e) return;
            kstrlcpy(e->label, f->name, sizeof(e->label));
            ksnprintf(e->path, sizeof(e->path), "%s/%s", dir, f->name);
            found++;
        }
        int nd = fs_list_dirs(dir, idx, FS_MAX_FILES);
        for (int i = 0; i < nd && i < FS_MAX_FILES && qt < 48; i++) {
            const fs_dir_t* d = fs_get_dir(idx[i]);
            if (!d) continue;
            if (contains_ci(d->name, q) && found < max) {
                entry_t* e = add(E_FILE);
                if (!e) return;
                kstrlcpy(e->label, d->name, sizeof(e->label));
                ksnprintf(e->path, sizeof(e->path), "%s/%s", dir, d->name);
                e->sm = 1;                               /* (a folder) */
                found++;
            }
            ksnprintf(queue[qt++], FS_PATH_LEN, "%s/%s", dir, d->name);
        }
    }
}

static void build(void) {
    g_ne = 0;
    if (g_query[0]) {
        g_mode = 2;
        int before = g_ne;
        add_head("Programs");
        add_all_programs(g_query);
        if (g_ne == before + 1) g_ne = before;           /* (no program: no heading) */
        before = g_ne;
        add_head("Files");
        add_files(g_query, 8);
        if (g_ne == before + 1) g_ne = before;
        if (!g_ne) { entry_t* e = add(E_NONE); if (e) kstrlcpy(e->label, "No items match your search.", sizeof(e->label)); }
    } else if (g_mode == 1) {
        add_all_programs("");
    } else {
        g_mode = 0;
        /* pinned at the top, then the others and the installed apps */
        add_prog(SM_BROWSER, NULL, "Browser");
        add_prog(SM_FILES, NULL, "Files");
        add(E_SEP);
        add_prog(SM_TERMINAL, NULL, "Terminal");
        add_prog(SM_NOTEPAD, NULL, "Notepad");
        add_prog(SM_TASKMGR, NULL, "Task Manager");
        int np = pkg_list(g_pkgs, 48);
        for (int i = 0; i < np && i < 48; i++)
            add_prog(0, g_pkgs[i].name, g_pkgs[i].title[0] ? g_pkgs[i].title : g_pkgs[i].name);
    }
    if (g_sel >= g_ne) g_sel = -1;
}

/* ── geometry ─────────────────────────────────────────────────────── */

static int menu_h(const fb_info_t* fi) {
    int h = (int)fi->height - BAR_H - 2;
    return h > 500 ? 500 : h;
}
static int menu_x(void) { return 0; }
static int menu_y(const fb_info_t* fi) { return (int)fi->height - BAR_H - menu_h(fi); }

/* the white pane: list area and the All Programs row */
static int pane_h(const fb_info_t* fi) { return menu_h(fi) - BOTTOM_H - 8; }
static int list_h(const fb_info_t* fi) { return pane_h(fi) - (g_mode == 2 ? 6 : ALL_ROW + 10); }

static int row_h(const entry_t* e) {
    if (e->kind == E_SEP) return 9;
    if (e->kind == E_HEAD) return 22;
    if (e->kind == E_NONE) return 28;
    return g_mode == 0 ? BIG_ROW : SMALL_ROW;
}

/* the y (relative to the pane's top) of entry i, -1 if not shown */
static int entry_y(const fb_info_t* fi, int i) {
    if (i < g_scroll) return -1;
    int y = 6;
    for (int k = g_scroll; k < i; k++) y += row_h(&g_e[k]);
    return y + row_h(&g_e[i]) <= list_h(fi) ? y : -1;
}

static int last_visible(const fb_info_t* fi) {
    int i = g_scroll;
    while (i < g_ne && entry_y(fi, i) >= 0) i++;
    return i - 1;
}

/* the places on the right */
typedef struct { const char* label; const char* path; int sm; fileicon_t icon; } place_t;
static const place_t PLACES[] = {
    { "banana", HOME, 0, FI_HOME },
    { "Documents", HOME "/Documents", 0, FI_DOCUMENTS },
    { "Pictures", HOME "/Pictures", 0, FI_PICTURES },
    { "Music", HOME "/Music", 0, FI_MUSIC },
    { "Downloads", HOME "/Downloads", 0, FI_DOWNLOADS },
    { NULL, NULL, 0, 0 },
    { "Computer", "/", 0, FI_COMPUTER },
    { NULL, NULL, 0, 0 },
    { "Settings", NULL, SM_SETTINGS, 0 },
    { "Task Manager", NULL, SM_TASKMGR, 0 },
    { "Apps", NULL, SM_APPS, 0 },
    { "Install Banana OS", NULL, SM_INSTALL, 0 },
};
#define NPLACES (int)(sizeof(PLACES) / sizeof(PLACES[0]))

static int place_shown(int i) { return PLACES[i].sm != SM_INSTALL || (g_host.installer && g_host.installer()); }

static int place_y(int i) {                              /* relative to the menu's top */
    int y = 84;
    for (int k = 0; k < i; k++) if (place_shown(k)) y += PLACES[k].label ? R_ROW : 9;
    return y;
}

/* the buttons of the bottom strip and the All Programs row */
enum { B_NONE = 0, B_SHUTDOWN, B_ARROW, B_ALL, B_SEARCH };
static void button_rect(const fb_info_t* fi, int b, int* x, int* y, int* w, int* h) {
    int mx = menu_x(), my = menu_y(fi), mh = menu_h(fi);
    switch (b) {
    case B_SHUTDOWN: *x = mx + LW + 22; *y = my + mh - 34; *w = 96; *h = 26; break;
    case B_ARROW:    *x = mx + LW + 118; *y = my + mh - 34; *w = 24; *h = 26; break;
    case B_ALL:      *x = mx + 10; *y = my + 6 + pane_h(fi) - ALL_ROW - 4; *w = LW - 8; *h = ALL_ROW; break;
    case B_SEARCH:   *x = mx + 12; *y = my + mh - 35; *w = LW - 4; *h = 26; break;
    default:         *x = *y = *w = *h = 0;
    }
}

static int inside(int px, int py, int x, int y, int w, int h) { return px >= x && px < x + w && py >= y && py < y + h; }

/* ── drawing helpers ─────────────────────────────────────────────── */

static void frame(int x, int y, int w, int h, uint32_t fill, uint32_t border) {
    gfx_fill_rect(x + 1, y, w - 2, h, border);
    gfx_fill_rect(x, y + 1, w, h - 2, border);
    gfx_fill_rect(x + 1, y + 1, w - 2, h - 2, fill);
}

static void gradient(int x, int y, int w, int h, uint32_t top, uint32_t bot) {
    for (int i = 0; i < h; i += 2) {
        uint32_t c = 0;
        for (int s = 0; s < 24; s += 8) {
            int a = (int)((top >> s) & 255), b = (int)((bot >> s) & 255);
            c |= (uint32_t)(a + (b - a) * i / (h > 1 ? h - 1 : 1)) << s;
        }
        gfx_fill_rect(x, y + i, w, i + 2 <= h ? 2 : 1, c);
    }
}

static void clip_text(int x, int y, const char* s, int max, uint32_t fg, uint32_t bg) {
    char buf[64];
    int n = (int)strlen(s);
    if (max < 1) return;
    if (max > (int)sizeof(buf) - 1) max = (int)sizeof(buf) - 1;
    if (n <= max) { gfx_draw_text(x, y, s, fg, bg); return; }
    memcpy(buf, s, (size_t)(max - 2));
    memcpy(buf + max - 2, "..", 3);
    gfx_draw_text(x, y, buf, fg, bg);
}

static int isqrt(int v) { int r = 0; while ((r + 1) * (r + 1) <= v) r++; return r; }

static void disc(int cx, int cy, int r, uint32_t top, uint32_t bot) {
    for (int dy = -r; dy <= r; dy++) {
        int w = isqrt(r * r - dy * dy);
        int t = (dy + r) * 255 / (2 * r);
        uint32_t c = 0;
        for (int s = 0; s < 24; s += 8) {
            int a = (int)((top >> s) & 255), b = (int)((bot >> s) & 255);
            c |= (uint32_t)(a + (b - a) * t / 255) << s;
        }
        gfx_fill_rect(cx - w, cy + dy, 2 * w + 1, 1, c);
    }
}

/* a little triangle pointing right (dir 1) or left (-1) */
static void tri(int x, int y, int dir, uint32_t c) {
    for (int k = 0; k < 4; k++) {
        int xx = dir > 0 ? x + k : x + 3 - k;
        gfx_fill_rect(xx, y + k, 1, 8 - 2 * k, c);
    }
}

void startmenu_draw_orb(int x, int y, int h, int pressed, int hover) {
    int cx = x + STARTMENU_ORB_W / 2, cy = y + h / 2, r = h / 2 - 1;
    disc(cx, cy, r, 0x00081C33u, 0x00081C33u);                      /* the rim */
    uint32_t top = pressed ? 0x003C7CC2u : hover ? 0x0084C4FFu : 0x0063A9EEu;
    uint32_t bot = pressed ? 0x000E3A70u : hover ? 0x00296FC0u : 0x001A579Bu;
    disc(cx, cy, r - 1, top, bot);
    /* the glassy shine on the top half */
    for (int dy = -r + 2; dy < -1; dy++) {
        int w = isqrt((r - 3) * (r - 3) - dy * dy);
        gfx_fill_rect(cx - w, cy + dy, 2 * w + 1, 1, pressed ? 0x004B8BD0u : hover ? 0x00A6D6FFu : 0x0089C1F5u);
    }
    /* a banana: a yellow crescent */
    for (int dy = -6; dy <= 6; dy++)
        for (int dx = -6; dx <= 6; dx++) {
            int in1 = dx * dx + dy * dy <= 36, in2 = (dx - 3) * (dx - 3) + (dy + 3) * (dy + 3) <= 30;
            if (in1 && !in2) gfx_fill_rect(cx + dx, cy + dy, 1, 1, dy > 2 ? 0x00D9A520u : 0x00FFD84Au);
        }
    gfx_fill_rect(cx + 4, cy - 6, 2, 2, 0x006B4A1Fu);                 /* its stalk */
}

/* ── drawing ──────────────────────────────────────────────────────── */

static void draw_left(const fb_info_t* fi) {
    int x = menu_x() + 8, y = menu_y(fi) + 6, ph = pane_h(fi);
    frame(x, y, LW, ph, C_WHITE, 0x00A9BFD6u);
    int lh = list_h(fi);
    for (int i = g_scroll; i < g_ne; i++) {
        int ey = entry_y(fi, i);
        if (ey < 0) break;
        entry_t* e = &g_e[i];
        int ry = y + ey, rh = row_h(e);
        if (e->kind == E_SEP) { gfx_fill_rect(x + 10, ry + 4, LW - 20, 1, C_SEPL); continue; }
        if (e->kind == E_HEAD) {
            gfx_draw_text(x + 10, ry + 8, e->label, C_HEAD, C_WHITE);
            gfx_fill_rect(x + 18 + (int)strlen(e->label) * 8, ry + 11, LW - 30 - (int)strlen(e->label) * 8, 1, C_SEPL);
            continue;
        }
        if (e->kind == E_NONE) { gfx_draw_text(x + 14, ry + 10, e->label, C_DIM, C_WHITE); continue; }
        uint32_t bg = C_WHITE;
        if (i == g_hl || i == g_sel) { frame(x + 4, ry, LW - 8, rh - 2, C_HL, C_HL_B); bg = C_HL; }
        int big = g_mode == 0;
        int isz = big ? 28 : 16;
        int ix = x + 10, iy = ry + (rh - 2 - isz) / 2;
        if (e->kind == E_FILE) fileicon_draw(e->sm ? FI_FOLDER : fileicon_for_name(e->label), ix, iy, 16);
        else if (g_host.program_icon) g_host.program_icon(e->sm, e->app, ix + (big ? 0 : 1), iy + (big ? 0 : 1), big, bg);
        int tx = ix + isz + 10;
        clip_text(tx, ry + (rh - 2) / 2 - 3, e->label, (LW - (tx - x) - 12) / 8, C_TEXT, bg);
    }
    /* more than fits (All Programs, search): the scroll position */
    if (g_mode != 0 && (g_scroll > 0 || last_visible(fi) < g_ne - 1)) {
        int track = lh - 12, th = track * (last_visible(fi) - g_scroll + 1) / (g_ne ? g_ne : 1);
        if (th < 12) th = 12;
        int span = g_ne - (last_visible(fi) - g_scroll + 1);
        int ty = y + 6 + (span > 0 ? (track - th) * g_scroll / span : 0);
        gfx_fill_rect(x + LW - 7, ty, 4, th, 0x00B8C7D8u);
    }
    if (g_mode == 2) return;
    /* All Programs / Back */
    int bx, by, bw, bh;
    button_rect(fi, B_ALL, &bx, &by, &bw, &bh);
    gfx_fill_rect(x + 10, by - 5, LW - 20, 1, C_SEPL);
    uint32_t bg = C_WHITE;
    if (g_hbtn == B_ALL) { frame(bx, by, bw, bh, C_HL, C_HL_B); bg = C_HL; }
    tri(bx + 12, by + 10, g_mode == 1 ? -1 : 1, 0x00346A2Bu);
    gfx_draw_text(bx + 26, by + 10, g_mode == 1 ? "Back" : "All Programs", C_TEXT, bg);
}

static void draw_right(const fb_info_t* fi) {
    int x = menu_x() + LW + 14, y = menu_y(fi);
    /* the picture: the user's, or the hovered place's icon (like Windows 7) */
    int px = x + (RW - 60) / 2, py = y + 10;
    frame(px, py, 60, 60, 0x00F4F7FBu, 0x00A9C7EAu);
    gfx_fill_rect(px + 3, py + 3, 54, 54, 0x00E9C35Au);
    if (g_hr >= 0 && PLACES[g_hr].label) {
        gfx_fill_rect(px + 3, py + 3, 54, 54, 0x00EEF4FBu);
        if (PLACES[g_hr].sm && g_host.program_icon) g_host.program_icon(PLACES[g_hr].sm, NULL, px + 16, py + 16, 1, 0x00EEF4FBu);
        else fileicon_draw(PLACES[g_hr].icon, px + 6, py + 6, 48);
    } else {
        /* a user: head and shoulders */
        for (int dy = -9; dy <= 9; dy++) { int w = isqrt(81 - dy * dy); gfx_fill_rect(px + 30 - w, py + 24 + dy, 2 * w, 1, 0x00F8F1E0u); }
        for (int dy = 0; dy < 16; dy++) { int w = 12 + dy / 2; gfx_fill_rect(px + 30 - w, py + 40 + dy, 2 * w, 1, 0x003C6FB5u); }
    }
    for (int i = 0; i < NPLACES; i++) {
        if (!place_shown(i)) continue;
        int ry = y + place_y(i);
        if (!PLACES[i].label) { gfx_fill_rect(x + 8, ry + 4, RW - 16, 1, 0x003F6A99u); continue; }
        uint32_t bg = 0;
        int hl = i == g_hr;
        /* (the glass behind is a gradient: text on it without a box) */
        if (hl) { frame(x + 2, ry, RW - 4, R_ROW - 2, C_RHL, C_RHL_B); bg = C_RHL; }
        int ty = ry + 8;
        if (hl) gfx_draw_text(x + 12, ty, PLACES[i].label, C_WHITE, bg);
        else {
            /* transparent text: draw it on the gradient's colour at that height */
            int t = (ry - y) * 255 / (menu_h(fi) > 1 ? menu_h(fi) : 1);
            uint32_t c = 0;
            for (int s = 0; s < 24; s += 8) {
                int a = (int)((C_GLASS1 >> s) & 255), b = (int)((C_GLASS2 >> s) & 255);
                c |= (uint32_t)(a + (b - a) * t / 255) << s;
            }
            gfx_fill_rect(x + 10, ty - 2, (int)strlen(PLACES[i].label) * 8 + 4, 12, c);
            gfx_draw_text(x + 12, ty, PLACES[i].label, C_WHITE, c);
        }
    }
}

static void draw_bottom(const fb_info_t* fi) {
    int bx, by, bw, bh;
    /* the search box */
    button_rect(fi, B_SEARCH, &bx, &by, &bw, &bh);
    frame(bx, by, bw, bh, C_WHITE, g_query[0] ? 0x003C7FB1u : 0x008FA9C6u);
    fileicon_draw(FI_SEARCH, bx + bw - 22, by + 5, 16);
    if (g_query[0]) {
        clip_text(bx + 8, by + 9, g_query, (bw - 36) / 8, C_TEXT, C_WHITE);
        if ((timer_ms() / 500) % 2 == 0) {
            int n = (int)strlen(g_query);
            if (n > (bw - 36) / 8) n = (bw - 36) / 8;
            gfx_fill_rect(bx + 8 + n * 8, by + 5, 1, 16, C_TEXT);
        }
    } else {
        gfx_draw_text(bx + 8, by + 9, "Search programs and files", 0x008C99A8u, C_WHITE);
    }
    /* Shut down and its arrow */
    button_rect(fi, B_SHUTDOWN, &bx, &by, &bw, &bh);
    uint32_t f1 = g_hbtn == B_SHUTDOWN ? 0x004F86C4u : 0x00315F95u;
    frame(bx, by, bw, bh, f1, 0x0091B6DEu);
    gfx_draw_text(bx + (bw - 9 * 8) / 2, by + 9, "Shut down", C_WHITE, f1);
    button_rect(fi, B_ARROW, &bx, &by, &bw, &bh);
    uint32_t f2 = g_hbtn == B_ARROW ? 0x004F86C4u : 0x00315F95u;
    frame(bx - 1, by, bw + 1, bh, f2, 0x0091B6DEu);
    tri(bx + 10, by + 9, 1, C_WHITE);
}

void startmenu_draw(const fb_info_t* fi) {
    if (!g_ne) build();
    int x = menu_x(), y = menu_y(fi), h = menu_h(fi);
    /* the glass frame */
    gfx_fill_rect(x, y, W, h, C_EDGE);
    gradient(x + 1, y + 1, W - 2, h - 2, C_GLASS1, C_GLASS2);
    gfx_fill_rect(x + 1, y + 1, W - 2, 1, C_EDGE_HI);
    gfx_fill_rect(x + 1, y + 1, 1, h - 2, 0x00406C9Eu);
    draw_left(fi);
    draw_right(fi);
    draw_bottom(fi);
}

/* ── input ────────────────────────────────────────────────────────── */

void startmenu_init(const startmenu_host_t* host) { g_host = *host; }

void startmenu_reset(void) {
    g_mode = 0;
    g_query[0] = 0;
    g_scroll = 0;
    g_sel = -1;
    g_hl = g_hr = -1;
    g_hbtn = 0;
    g_esc = 0;
    build();
    g_gen++;
}

int startmenu_contains(const fb_info_t* fi, int mx, int my) {
    return inside(mx, my, menu_x(), menu_y(fi), W, menu_h(fi));
}

static int left_at(const fb_info_t* fi, int mx, int my) {
    int x = menu_x() + 8, y = menu_y(fi) + 6;
    if (mx < x + 4 || mx >= x + LW - 4) return -1;
    for (int i = g_scroll; i < g_ne; i++) {
        int ey = entry_y(fi, i);
        if (ey < 0) break;
        if (my >= y + ey && my < y + ey + row_h(&g_e[i]) && (g_e[i].kind == E_PROG || g_e[i].kind == E_FILE)) return i;
    }
    return -1;
}

static int right_at(const fb_info_t* fi, int mx, int my) {
    int x = menu_x() + LW + 14, y = menu_y(fi);
    if (mx < x || mx >= x + RW) return -1;
    for (int i = 0; i < NPLACES; i++) {
        if (!place_shown(i) || !PLACES[i].label) continue;
        int ry = y + place_y(i);
        if (my >= ry && my < ry + R_ROW - 2) return i;
    }
    return -1;
}

static int button_at(const fb_info_t* fi, int mx, int my) {
    for (int b = B_SHUTDOWN; b <= B_SEARCH; b++) {
        if (b == B_ALL && g_mode == 2) continue;
        int x, y, w, h;
        button_rect(fi, b, &x, &y, &w, &h);
        if (inside(mx, my, x, y, w, h)) return b;
    }
    return 0;
}

void startmenu_hover(const fb_info_t* fi, int mx, int my) {
    int l = startmenu_contains(fi, mx, my) ? left_at(fi, mx, my) : -1;
    int r = startmenu_contains(fi, mx, my) ? right_at(fi, mx, my) : -1;
    int b = startmenu_contains(fi, mx, my) ? button_at(fi, mx, my) : 0;
    if (l != g_hl || r != g_hr || b != g_hbtn) {
        g_hl = l; g_hr = r; g_hbtn = b;
        if (l >= 0) g_sel = -1;                         /* the pointer takes over from the keys */
        g_gen++;
    }
}

static void open_place(int i) {
    const place_t* p = &PLACES[i];
    if (p->sm) { g_host.action(p->sm); return; }
    if (fs_find_dir(p->path) < 0 && strcmp(p->path, "/") != 0) fs_mkdir(p->path);   /* a library: made when first opened */
    g_host.open_path(p->path);
}

static void activate(int i) {
    if (i < 0 || i >= g_ne) return;
    entry_t* e = &g_e[i];
    if (e->kind == E_PROG) {
        if (e->sm) g_host.action(e->sm);
        else g_host.run_app(e->app);
    } else if (e->kind == E_FILE) {
        g_host.open_path(e->path);
    }
}

static void power_cb(int id, void* arg) { (void)arg; g_host.action(id); }

static void power_menu(const fb_info_t* fi) {
    int x, y, w, h;
    button_rect(fi, B_ARROW, &x, &y, &w, &h);
    ctx_item_t items[6];
    int n = 0;
    items[n++] = (ctx_item_t){ "Restart", SM_RESTART, 0 };
    items[n++] = (ctx_item_t){ "Lock", SM_LOCK, 0 };
    items[n++] = (ctx_item_t){ CTX_SEP, 0, 0 };
    items[n++] = (ctx_item_t){ "Exit to the shell", SM_QUIT, 0 };
    ctxmenu_open(x + w, y - 4 * 22, items, n, power_cb, NULL);
}

void startmenu_click(const fb_info_t* fi, int mx, int my) {
    g_gen++;
    int b = button_at(fi, mx, my);
    if (b == B_SHUTDOWN) { g_host.action(SM_SHUTDOWN); return; }
    if (b == B_ARROW) { power_menu(fi); return; }
    if (b == B_ALL) { g_mode = g_mode == 1 ? 0 : 1; g_scroll = 0; g_sel = -1; g_hl = -1; build(); return; }
    if (b == B_SEARCH) return;                         /* (typing goes there anyway) */
    int l = left_at(fi, mx, my);
    if (l >= 0) { activate(l); return; }
    int r = right_at(fi, mx, my);
    if (r >= 0) open_place(r);
}

void startmenu_wheel(int dz) {
    if (g_mode == 0) return;
    g_scroll += dz * 2;
    if (g_scroll > g_ne - 1) g_scroll = g_ne - 1;
    if (g_scroll < 0) g_scroll = 0;
    g_gen++;
}

/* the keyboard's entry, moved over the programs and files */
static void move_sel(int d) {
    int i = g_sel;
    for (int k = 0; k < g_ne; k++) {
        i += d;
        if (i < 0) i = g_ne - 1;
        if (i >= g_ne) i = 0;
        if (g_e[i].kind == E_PROG || g_e[i].kind == E_FILE) { g_sel = i; break; }
    }
    if (g_sel >= 0 && g_sel < g_scroll) g_scroll = g_sel;
    g_hl = -1;
}

static int first_selectable(void) {
    for (int i = 0; i < g_ne; i++) if (g_e[i].kind == E_PROG || g_e[i].kind == E_FILE) return i;
    return -1;
}

void startmenu_escape(void) {
    g_gen++;
    if (g_query[0]) { g_query[0] = 0; g_mode = 0; g_scroll = 0; g_sel = -1; build(); }
    else if (g_host.close) g_host.close();
}

void startmenu_arrow(char code) {
    g_gen++;
    if (code == 'A') move_sel(-1);
    else if (code == 'B') move_sel(1);
    else if (code == 'C' && g_mode == 0) { g_mode = 1; g_scroll = 0; g_sel = -1; build(); }
    else if (code == 'D' && g_mode == 1) { g_mode = 0; g_scroll = 0; g_sel = -1; build(); }
}

void startmenu_key(char c) {
    g_gen++;
    if (c == 27) { startmenu_escape(); return; }
    if (c == '\n') {
        int i = g_sel >= 0 ? g_sel : g_query[0] ? first_selectable() : -1;
        if (i >= 0) activate(i);
        return;
    }
    size_t n = strlen(g_query);
    if (c == '\b') {
        if (n) u8_backspace(g_query, (int)n);
        if (!g_query[0]) g_mode = 0;
    } else if ((unsigned char)c >= 32 && n < sizeof(g_query) - 1) {
        g_query[n] = c;
        g_query[n + 1] = 0;
    } else {
        return;
    }
    g_scroll = 0;
    build();
    g_sel = g_query[0] ? first_selectable() : -1;      /* Enter opens the best match */
}

uint32_t startmenu_signature(void) {
    return g_gen * 2654435761u ^ (uint32_t)(g_query[0] ? (timer_ms() / 500) : 0);
}
