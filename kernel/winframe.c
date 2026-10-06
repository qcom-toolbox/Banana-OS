#include "winframe.h"
#include "fb.h"
#include "timer.h"
#include "gfx.h"

void win_clamp(win_geom_t* g) {
    const fb_info_t* fi = fb_info();
    int sw = fi ? (int)fi->width : 800, sh = fi ? (int)fi->height - WIN_TASKBAR : 572;
    if (sw > 2560) sw = 2560;               /* the desktop's backbuffer (kernel/gui.c) */
    if (sh > 1600 - WIN_TASKBAR) sh = 1600 - WIN_TASKBAR;
    if (g->w < g->min_w) g->w = g->min_w;
    if (g->h < g->min_h) g->h = g->min_h;
    if (g->w > sw) g->w = sw;
    if (g->h > sh) g->h = sh;
    if (g->x + g->w > sw) g->x = sw - g->w;
    if (g->y + g->h > sh) g->y = sh - g->h;
    if (g->x < 0) g->x = 0;
    if (g->y < 0) g->y = 0;
}

int win_title_press(win_geom_t* g, int mx, int my) {
    uint32_t now = timer_ms();
    if (now - g->title_ms < 400) {
        g->title_ms = 0;
        if (!g->maxed) {
            g->sx = g->x; g->sy = g->y; g->sw = g->w; g->sh = g->h;
            g->x = 0; g->y = 0; g->w = 10000; g->h = 10000;
            g->maxed = 1;
        } else {
            g->x = g->sx; g->y = g->sy; g->w = g->sw; g->h = g->sh;
            g->maxed = 0;
        }
        win_clamp(g);
        return 1;
    }
    g->title_ms = now;
    g->dragging = 1;
    g->dx = mx - g->x;
    g->dy = my - g->y;
    return 0;
}

int win_grip_press(win_geom_t* g, int mx, int my) {
    if (mx < g->x + g->w - WIN_GRIP || my < g->y + g->h - WIN_GRIP) return 0;
    g->resizing = 1;
    g->maxed = 0;
    g->dx = g->x + g->w - mx;
    g->dy = g->y + g->h - my;
    return 1;
}

int win_mouse(win_geom_t* g, int mx, int my, int left) {
    if (!left) { g->dragging = 0; g->resizing = 0; return 0; }
    int ox = g->x, oy = g->y, ow = g->w, oh = g->h;
    if (g->dragging) {
        g->x = mx - g->dx;
        g->y = my - g->dy;
        g->maxed = 0;
    } else if (g->resizing) {
        g->w = mx + g->dx - g->x;
        g->h = my + g->dy - g->y;
    } else {
        return 0;
    }
    win_clamp(g);
    return g->x != ox || g->y != oy || g->w != ow || g->h != oh;
}

/* ── title-bar buttons ─────────────────────────────────────────────
 * [_] minimize   [#] maximize / restore   [x] close, 20 px wide with 4 px
 * between them, the close button 8 px from the right edge. */

static int g_min_request;

static void btn_box(int x, int y, int w, int h, uint32_t base, uint32_t hi, uint32_t lo) {
    gfx_fill_rect(x, y, w, h, base);
    gfx_fill_rect(x, y, w, 1, hi);
    gfx_fill_rect(x, y, 1, h, hi);
    gfx_fill_rect(x, y + h - 1, w, 1, lo);
    gfx_fill_rect(x + w - 1, y, 1, h, lo);
}

static void outline(int x, int y, int w, int h, uint32_t c) {
    gfx_fill_rect(x, y, w, 2, c);              /* a thicker top edge, like a window's title bar */
    gfx_fill_rect(x, y + h - 1, w, 1, c);
    gfx_fill_rect(x, y, 1, h, c);
    gfx_fill_rect(x + w - 1, y, 1, h, c);
}

void win_draw_button_row(int right, int y, int h, int has_max, int maxed) {
    const uint32_t gb = 0x00303740u, ghi = 0x00535D6Eu, glo = 0x0015191Fu, fg = 0x00FFFFFFu;
    int cx = right - 28;
    btn_box(cx, y, 20, h, 0x006D2F2Fu, 0x00A14747u, 0x00301717u);
    gfx_draw_text(cx + 6, y + (h - 8) / 2, "x", fg, 0x006D2F2Fu);
    int mx = cx - 24;
    if (has_max) {
        btn_box(mx, y, 20, h, gb, ghi, glo);
        int iy = y + (h - 8) / 2;
        if (maxed) {                            /* restore: two windows */
            outline(mx + 8, iy, 7, 6, fg);
            gfx_fill_rect(mx + 5, iy + 3, 7, 6, gb);
            outline(mx + 5, iy + 3, 7, 6, fg);
        } else {
            outline(mx + 5, iy, 10, 8, fg);
        }
        mx -= 24;
    }
    btn_box(mx, y, 20, h, gb, ghi, glo);
    gfx_fill_rect(mx + 6, y + h - 5, 8, 2, fg);  /* _ */
}

int win_button_hit(int right, int y, int h, int has_max, int mx, int my) {
    if (my < y || my >= y + h) return WIN_BTN_NONE;
    int cx = right - 28;
    if (mx >= cx && mx < cx + 20) return WIN_BTN_CLOSE;
    int bx = cx - 24;
    if (has_max) {
        if (mx >= bx && mx < bx + 20) return WIN_BTN_MAX;
        bx -= 24;
    }
    if (mx >= bx && mx < bx + 20) return WIN_BTN_MIN;
    return WIN_BTN_NONE;
}

void win_toggle_max(win_geom_t* g) {
    if (!g->maxed) {
        g->sx = g->x; g->sy = g->y; g->sw = g->w; g->sh = g->h;
        g->x = 0; g->y = 0; g->w = 10000; g->h = 10000;
        g->maxed = 1;
    } else {
        g->x = g->sx; g->y = g->sy; g->w = g->sw; g->h = g->sh;
        g->maxed = 0;
    }
    g->dragging = g->resizing = 0;
    win_clamp(g);
}

void win_draw_buttons(const win_geom_t* g, int by, int bh) {
    win_draw_button_row(g->x + g->w, g->y + by, bh, 1, g->maxed);
}

int win_button_press(win_geom_t* g, int by, int bh, int mx, int my) {
    int b = win_button_hit(g->x + g->w, g->y + by, bh, 1, mx, my);
    if (b == WIN_BTN_MAX) win_toggle_max(g);
    else if (b == WIN_BTN_MIN) g_min_request = 1;
    return b;
}

int winframe_take_minimize(void) {
    int r = g_min_request;
    g_min_request = 0;
    return r;
}
