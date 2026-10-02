#ifndef WINFRAME_H
#define WINFRAME_H

#include "types.h"

/*
 * Window geometry shared by the desktop's app windows: dragging by the
 * title bar, resizing by the grip in the bottom-right corner, and
 * maximize / restore by double-clicking the title bar.
 */

#define WIN_GRIP    14
#define WIN_TASKBAR 28

typedef struct {
    int x, y, w, h;
    int min_w, min_h;
    int dragging, resizing, dx, dy;
    int maxed, sx, sy, sw, sh;      /* geometry to restore */
    uint32_t title_ms;              /* last title-bar click (double-click) */
} win_geom_t;

/* left button went down in the title bar: starts a drag, or maximizes /
 * restores on a double-click (returns 1 then: the size changed) */
int  win_title_press(win_geom_t* g, int mx, int my);
/* 1 if (mx, my) is the resize grip; starts resizing */
int  win_grip_press(win_geom_t* g, int mx, int my);
/* mouse moves while dragging/resizing; 1 if the geometry changed */
int  win_mouse(win_geom_t* g, int mx, int my, int left);
/* keeps the window on screen and at least its minimum size */
void win_clamp(win_geom_t* g);

#endif
