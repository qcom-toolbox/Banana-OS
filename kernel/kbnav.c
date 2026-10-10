/* Keyboard focus inside a window (kbnav.h). */
#include "kbnav.h"
#include "gfx.h"

void kbnav_begin(kbnav_t* k) { k->n = 0; }

void kbnav_add(kbnav_t* k, int x, int y, int w, int h) {
    if (k->n >= KBNAV_MAX || w <= 0 || h <= 0) return;
    k->x[k->n] = (int16_t)x;
    k->y[k->n] = (int16_t)y;
    k->w[k->n] = (int16_t)w;
    k->h[k->n] = (int16_t)h;
    k->n++;
}

void kbnav_draw(const kbnav_t* k, uint32_t color) {
    if (!k->shown || k->focus < 0 || k->focus >= k->n) return;
    int x = k->x[k->focus] - 2, y = k->y[k->focus] - 2, w = k->w[k->focus] + 4, h = k->h[k->focus] + 4;
    gfx_fill_rect(x, y, w, 2, color);
    gfx_fill_rect(x, y + h - 2, w, 2, color);
    gfx_fill_rect(x, y, 2, h, color);
    gfx_fill_rect(x + w - 2, y, 2, h, color);
}

int kbnav_focused(const kbnav_t* k, int* x, int* y, int* w, int* h) {
    if (!k->shown || k->focus < 0 || k->focus >= k->n) return 0;
    *x = k->x[k->focus]; *y = k->y[k->focus]; *w = k->w[k->focus]; *h = k->h[k->focus];
    return 1;
}

void kbnav_mouse(kbnav_t* k) { k->shown = 0; }

/* the nearest place in a direction from the focused one: along the
 * direction counts once, across it three times (rows and columns win) */
static int nearest(const kbnav_t* k, int dx, int dy) {
    int f = k->focus;
    int cx = k->x[f] + k->w[f] / 2, cy = k->y[f] + k->h[f] / 2;
    int best = -1, best_d = 0x7FFFFFFF;
    for (int i = 0; i < k->n; i++) {
        if (i == f) continue;
        int ix = k->x[i] + k->w[i] / 2, iy = k->y[i] + k->h[i] / 2;
        int along = dx ? (ix - cx) * dx : (iy - cy) * dy;
        int across = dx ? iy - cy : ix - cx;
        if (across < 0) across = -across;
        /* in that direction (overlapping rows / columns count as level) */
        int edge = dx ? (dx > 0 ? k->x[i] >= k->x[f] + k->w[f] - 2 : k->x[i] + k->w[i] <= k->x[f] + 2)
                      : (dy > 0 ? k->y[i] >= k->y[f] + k->h[f] - 2 : k->y[i] + k->h[i] <= k->y[f] + 2);
        if (along <= 0 || !edge) continue;
        int d = along + 3 * across;
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

int kbnav_key(kbnav_t* k, int code, int* cx, int* cy) {
    if (!k->n) return 0;
    if (k->focus < 0 || k->focus >= k->n) k->focus = 0;
    if (!k->shown && (code == KB_TAB || code == KB_BACKTAB || (code >= KB_UP && code <= KB_LEFT))) {
        k->shown = 1;                         /* the first key shows where the focus is */
        return 2;
    }
    switch (code) {
    case KB_TAB: k->focus = (k->focus + 1) % k->n; k->shown = 1; return 2;
    case KB_BACKTAB: k->focus = (k->focus + k->n - 1) % k->n; k->shown = 1; return 2;
    case KB_HOME: k->focus = 0; k->shown = 1; return 2;
    case KB_END: k->focus = k->n - 1; k->shown = 1; return 2;
    case KB_UP: case KB_DOWN: case KB_LEFT: case KB_RIGHT: {
        int dx = code == KB_RIGHT ? 1 : code == KB_LEFT ? -1 : 0;
        int dy = code == KB_DOWN ? 1 : code == KB_UP ? -1 : 0;
        int n = nearest(k, dx, dy);
        if (n >= 0) k->focus = n;
        k->shown = 1;
        return 2;
    }
    case KB_ENTER: case KB_SPACE:
        if (!k->shown) return 0;
        *cx = k->x[k->focus] + k->w[k->focus] / 2;
        *cy = k->y[k->focus] + k->h[k->focus] / 2;
        return 1;
    }
    return 0;
}
