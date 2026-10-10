#include "launcher.h"
#include "gfx.h"
#include "kstring.h"
#include "timer.h"
#include "winframe.h"
#include "pkg.h"
#include "ctxmenu.h"
#include "explorer.h"
#include "gui.h"

#define TITLE_H  20
#define TOOL_Y   26
#define GRID_Y   54
#define TILE_W   120
#define TILE_H   76
#define MAX_APPS 48

#define C_PANEL  0x001D232Cu
#define C_TITLE  0x00384562u
#define C_TEXT   0x00E8EEF6u
#define C_DIM    0x00AAB6C6u
#define C_TILE   0x00262E3Au
#define C_HOVER  0x00324058u

static int        g_open;
static win_geom_t g_win = { .x = 150, .y = 70, .w = 520, .h = 380, .min_w = 280, .min_h = 200 };
static pkg_info_t g_apps[MAX_APPS];
static int        g_napps;
static uint32_t   g_scan_ms;
static int        g_scroll;
static char       g_status[120];
static uint32_t   g_gen;
static int        g_menu_app = -1;

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

static void set_status(const char* s) { kstrlcpy(g_status, s, sizeof(g_status)); g_gen++; }

static void rescan(void) {
    int n = pkg_list(g_apps, MAX_APPS);
    g_napps = n > MAX_APPS ? MAX_APPS : n;
    g_scan_ms = timer_ms();
    g_gen++;
}

static int cols(void) { int c = (g_win.w - 16) / (TILE_W + 8); return c < 1 ? 1 : c; }
static int rows_visible(void) { int r = (g_win.h - GRID_Y - 24) / (TILE_H + 8); return r < 1 ? 1 : r; }

/* tile i's rectangle in screen coordinates (-1 y if scrolled away) */
static int tile_rect(int i, int* x, int* y) {
    int c = cols();
    int row = i / c - g_scroll;
    if (row < 0 || row >= rows_visible()) return 0;
    *x = g_win.x + 8 + (i % c) * (TILE_W + 8);
    *y = g_win.y + GRID_Y + row * (TILE_H + 8);
    return 1;
}

static int tile_at(int mx, int my) {
    for (int i = 0; i < g_napps; i++) {
        int x, y;
        if (tile_rect(i, &x, &y) && inside(mx, my, x, y, TILE_W, TILE_H)) return i;
    }
    return -1;
}

/* a color from the name, for the app's letter icon */
static uint32_t name_color(const char* s) {
    static const uint32_t pal[] = { 0x003A7BD5u, 0x0057B65Au, 0x00E07040u, 0x008A5CD0u, 0x00D0A030u, 0x0030A8A8u, 0x00C04C70u };
    uint32_t h = 0;
    while (*s) h = h * 31u + (uint8_t)*s++;
    return pal[h % 7];
}

static void run_app(int i) {
    char err[128];
    err[0] = 0;
    char msg[160];
    if (pkg_run(g_apps[i].name, 0, NULL, 1, err, sizeof(err)) < 0) ksnprintf(msg, sizeof(msg), "%s: %s", g_apps[i].title, err);
    else ksnprintf(msg, sizeof(msg), "Started %s", g_apps[i].title);
    set_status(msg);
}

/* ── public ───────────────────────────────────────────────────────── */

void launcher_open(void) {
    g_open = 1;
    g_scroll = 0;
    g_status[0] = 0;
    rescan();
    win_clamp(&g_win);
}

void launcher_close(void) {
    g_open = 0;
    g_win.dragging = g_win.resizing = 0;
    g_gen++;
}

int launcher_is_open(void) { return g_open; }
int launcher_contains(int mx, int my) { return g_open && inside(mx, my, g_win.x, g_win.y, g_win.w, g_win.h); }

uint32_t launcher_signature(void) {
    if (!g_open) return 0;
    if (timer_ms() - g_scan_ms > 1500) rescan();          /* apps installed meanwhile */
    return g_gen * 2654435761u ^ (uint32_t)(g_win.x << 16 | g_win.y) ^ (uint32_t)(g_win.w << 20 | g_win.h << 4) ^
           (uint32_t)g_napps * 7919u;
}

static void menu_cb(int id, void* arg) {
    (void)arg;
    int i = g_menu_app;
    if (i < 0 || i >= g_napps) return;
    char msg[160];
    if (id == 1) run_app(i);
    else if (id == 2) {
        ksnprintf(msg, sizeof(msg), "%s %s (%s app) - %s", g_apps[i].title, g_apps[i].version, g_apps[i].type,
                  g_apps[i].description[0] ? g_apps[i].description : "no description");
        set_status(msg);
    } else if (id == 3) {
        if (pkg_remove(g_apps[i].name, msg, sizeof(msg)) == 0) rescan();
        set_status(msg);
    }
}

void launcher_rclick(int mx, int my) {
    int i = tile_at(mx, my);
    if (i < 0) return;
    g_menu_app = i;
    ctx_item_t items[] = {
        { "Open", 1, 0 },
        { "Details", 2, 0 },
        { CTX_SEP, 0, 0 },
        { "Uninstall", 3, 0 },
    };
    ctxmenu_open(mx, my, items, 4, menu_cb, NULL);
}

#include "kbnav.h"
static kbnav_t g_nav;

/* the keyboard: Tab / arrows between the tiles, Enter opens one */
int launcher_navkey(int code) {
    if (!g_open) return 0;
    g_gen++;
    int cx, cy;
    int r = kbnav_key(&g_nav, code, &cx, &cy);
    if (r == 1) {
        int shown = g_nav.shown, focus = g_nav.focus;
        launcher_click(cx, cy);
        g_nav.shown = shown;
        g_nav.focus = focus;
    }
    return r != 0;
}

void launcher_click(int mx, int my) {
    if (!launcher_contains(mx, my)) return;
    int lx = mx - g_win.x, ly = my - g_win.y;
    g_gen++;
    if (ly < TITLE_H + 2) {
        int b = win_button_press(&g_win, 4, 12, mx, my);
        if (b == WIN_BTN_CLOSE) { launcher_close(); return; }
        if (b) return;
        win_title_press(&g_win, mx, my);
        return;
    }
    if (win_grip_press(&g_win, mx, my)) return;
    kbnav_mouse(&g_nav);
    if (ly >= TOOL_Y && ly < TOOL_Y + 18) {
        if (lx >= 8 && lx < 120) { explorer_open("/home/banana/Examples"); gui_raise_files(); set_status("Double-click a .bpk to install it"); }
        else if (lx >= 126 && lx < 206) { rescan(); set_status("Refreshed"); }
        return;
    }
    /* scroll arrows at the right of the toolbar */
    if (ly >= TOOL_Y && ly < TOOL_Y + 18 && lx >= g_win.w - 60) return;
    int i = tile_at(mx, my);
    if (i >= 0) run_app(i);
    else if (ly > GRID_Y) {
        int total_rows = (g_napps + cols() - 1) / cols();
        if (ly > g_win.h / 2 && g_scroll + rows_visible() < total_rows) g_scroll++;
        else if (ly <= g_win.h / 2 && g_scroll > 0) g_scroll--;
    }
}

void launcher_mouse(int mx, int my, int left) {
    if (win_mouse(&g_win, mx, my, left)) g_gen++;
}

void launcher_draw(const fb_info_t* fi) {
    (void)fi;
    if (!g_open) return;
    int x = g_win.x, y = g_win.y, W = g_win.w, H = g_win.h;
    bevel(x, y, W, H, C_PANEL, 0x00505D72u, 0x0010141Cu);
    bevel(x + 3, y + 3, W - 6, TITLE_H - 1, C_TITLE, 0x00647692u, 0x00111923u);
    gfx_draw_text(x + 10, y + 7, "Apps", 0x00FFFFFFu, C_TITLE);
    win_draw_buttons(&g_win, 4, 12);
    kbnav_begin(&g_nav);
    kbnav_add(&g_nav, x + 8, y + TOOL_Y, 112, 18);
    kbnav_add(&g_nav, x + 126, y + TOOL_Y, 80, 18);

    bevel(x + 8, y + TOOL_Y, 112, 18, 0x00303740u, 0x00535D6Eu, 0x0015191Fu);
    gfx_draw_text(x + 16, y + TOOL_Y + 5, "Get apps...", C_TEXT, 0x00303740u);
    bevel(x + 126, y + TOOL_Y, 80, 18, 0x00303740u, 0x00535D6Eu, 0x0015191Fu);
    gfx_draw_text(x + 138, y + TOOL_Y + 5, "Refresh", C_TEXT, 0x00303740u);

    if (g_napps == 0) {
        gfx_draw_text(x + 16, y + GRID_Y + 10, "No apps installed yet.", C_TEXT, C_PANEL);
        gfx_draw_text(x + 16, y + GRID_Y + 30, "Install one: double-click a .bpk file in Files", C_DIM, C_PANEL);
        gfx_draw_text(x + 16, y + GRID_Y + 44, "(try ~/Examples, a USB stick in /mnt/usb, or a", C_DIM, C_PANEL);
        gfx_draw_text(x + 16, y + GRID_Y + 58, "download), or type: pkg install <file.bpk>", C_DIM, C_PANEL);
        gfx_draw_text(x + 16, y + GRID_Y + 82, "Build your own with the Linux SDK (sdk/).", C_DIM, C_PANEL);
    }
    for (int i = 0; i < g_napps; i++) {
        int tx, ty;
        if (!tile_rect(i, &tx, &ty)) continue;
        bevel(tx, ty, TILE_W, TILE_H, C_TILE, 0x00404C60u, 0x00141920u);
        kbnav_add(&g_nav, tx, ty, TILE_W, TILE_H);
        uint32_t ic = name_color(g_apps[i].name);
        int ix = tx + (TILE_W - 32) / 2;
        bevel(ix, ty + 8, 32, 32, ic, 0x00FFFFFFu & (ic + 0x00303030u), 0x00101010u);
        char letter[2] = { g_apps[i].title[0], 0 };
        if (letter[0] >= 'a' && letter[0] <= 'z') letter[0] -= 32;
        gfx_draw_text_scaled(ix + 8, ty + 16, 2, letter, 0x00FFFFFFu, ic);
        char t[15];                       /* 14 characters fit a tile */
        kstrlcpy(t, g_apps[i].title, sizeof(t));
        int tl = (int)strlen(t);
        gfx_draw_text(tx + (TILE_W - tl * 8) / 2, ty + 48, t, C_TEXT, C_TILE);
        const char* kind = strcmp(g_apps[i].type, "gui") == 0 ? "desktop" : "terminal";
        gfx_draw_text(tx + (TILE_W - (int)strlen(kind) * 8) / 2, ty + 60, kind, C_DIM, C_TILE);
    }
    int total_rows = (g_napps + cols() - 1) / cols();
    if (total_rows > rows_visible()) {
        char more[48];
        ksnprintf(more, sizeof(more), "rows %d-%d of %d (click above/below to scroll)", g_scroll + 1,
                  g_scroll + rows_visible(), total_rows);
        gfx_draw_text(x + 220, y + TOOL_Y + 5, more, C_DIM, C_PANEL);
    }
    gfx_fill_rect(x + 3, y + H - 18, W - 6, 15, 0x00161B22u);
    char st[120];
    if (g_status[0]) kstrlcpy(st, g_status, sizeof(st));
    else ksnprintf(st, sizeof(st), "%d app%s - click to open, right-click for more", g_napps, g_napps == 1 ? "" : "s");
    int maxc = (W - 24) / 8;
    if (maxc < (int)sizeof(st) && maxc > 0) st[maxc] = 0;
    gfx_draw_text(x + 8, y + H - 14, st, C_DIM, 0x00161B22u);
    gfx_draw_grip(x + W, y + H);
    kbnav_draw(&g_nav, 0x00FFD34Eu);
}
