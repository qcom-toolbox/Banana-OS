#include "ctxmenu.h"
#include "gfx.h"
#include "fb.h"
#include "kstring.h"

#define ITEM_H 20
#define SEP_H  7
#define PAD    3

static int      g_open;
static int      g_x, g_y, g_w, g_h;
static int      g_n, g_hover = -1;
static char     g_label[CTX_MAX_ITEMS][40];
static int      g_id[CTX_MAX_ITEMS], g_dis[CTX_MAX_ITEMS];
static ctx_cb_t g_cb;
static void*    g_arg;
static uint32_t g_gen;

static int is_sep(int i) { return strcmp(g_label[i], CTX_SEP) == 0; }

static int item_y(int i) {
    int y = g_y + PAD;
    for (int k = 0; k < i; k++) y += is_sep(k) ? SEP_H : ITEM_H;
    return y;
}

void ctxmenu_open(int x, int y, const ctx_item_t* items, int n, ctx_cb_t cb, void* arg) {
    if (n > CTX_MAX_ITEMS) n = CTX_MAX_ITEMS;
    int maxl = 0;
    g_n = n;
    for (int i = 0; i < n; i++) {
        kstrlcpy(g_label[i], items[i].label, sizeof(g_label[i]));
        g_id[i] = items[i].id;
        g_dis[i] = items[i].disabled;
        int l = (int)strlen(g_label[i]);
        if (l > maxl) maxl = l;
    }
    g_w = maxl * 8 + 32;
    if (g_w < 140) g_w = 140;
    g_h = PAD * 2;
    for (int i = 0; i < n; i++) g_h += is_sep(i) ? SEP_H : ITEM_H;
    const fb_info_t* fi = fb_info();
    int sw = fi ? (int)fi->width : 800, sh = fi ? (int)fi->height : 600;
    if (x + g_w > sw) x = sw - g_w;
    if (y + g_h > sh) y = y - g_h > 0 ? y - g_h : sh - g_h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    g_x = x;
    g_y = y;
    g_cb = cb;
    g_arg = arg;
    g_hover = -1;
    g_open = 1;
    g_gen++;
}

void ctxmenu_close(void) {
    if (!g_open) return;
    g_open = 0;
    g_gen++;
}

int ctxmenu_is_open(void) { return g_open; }

static int item_at(int mx, int my) {
    if (mx < g_x || mx >= g_x + g_w) return -1;
    for (int i = 0; i < g_n; i++) {
        int y = item_y(i), h = is_sep(i) ? SEP_H : ITEM_H;
        if (my >= y && my < y + h) return is_sep(i) ? -1 : i;
    }
    return -1;
}

int ctxmenu_click(int mx, int my) {
    if (!g_open) return 0;
    int inside = mx >= g_x && mx < g_x + g_w && my >= g_y && my < g_y + g_h;
    int i = item_at(mx, my);
    if (inside && (i < 0 || g_dis[i])) return 1;     /* a separator / greyed item: stay open */
    ctx_cb_t cb = g_cb;
    void* arg = g_arg;
    ctxmenu_close();
    if (i >= 0 && cb) cb(g_id[i], arg);
    return 1;
}

void ctxmenu_hover(int mx, int my) {
    if (!g_open) return;
    int i = item_at(mx, my);
    if (i >= 0 && g_dis[i]) i = -1;
    if (i != g_hover) { g_hover = i; g_gen++; }
}

void ctxmenu_draw(void) {
    if (!g_open) return;
    /* a shadow, then a bevelled panel */
    gfx_fill_rect(g_x + 4, g_y + 4, g_w, g_h, 0x00080A0Eu);
    gfx_fill_rect(g_x, g_y, g_w, g_h, 0x00232A35u);
    gfx_fill_rect(g_x, g_y, g_w, 1, 0x00627089u);
    gfx_fill_rect(g_x, g_y, 1, g_h, 0x00627089u);
    gfx_fill_rect(g_x, g_y + g_h - 1, g_w, 1, 0x000E1117u);
    gfx_fill_rect(g_x + g_w - 1, g_y, 1, g_h, 0x000E1117u);
    for (int i = 0; i < g_n; i++) {
        int y = item_y(i);
        if (is_sep(i)) {
            gfx_fill_rect(g_x + 8, y + 3, g_w - 16, 1, 0x00475266u);
            continue;
        }
        uint32_t bg = (i == g_hover) ? 0x003A5A8Au : 0x00232A35u;
        if (i == g_hover) gfx_fill_rect(g_x + 2, y, g_w - 4, ITEM_H, bg);
        gfx_draw_text(g_x + 14, y + 6, g_label[i], g_dis[i] ? 0x00707A88u : 0x00E8EEF6u, bg);
    }
}

uint32_t ctxmenu_signature(void) {
    return g_open ? (g_gen * 2654435761u ^ (uint32_t)(g_x << 16 | g_y) ^ (uint32_t)(g_hover + 2)) : 0;
}
