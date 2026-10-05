#include "winframe.h"
#include "fb.h"
#include "timer.h"

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
