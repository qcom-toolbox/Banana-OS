#ifndef WINFRAME_H
#define WINFRAME_H

#include "types.h"

/*
 * Window geometry shared by the desktop's app windows: dragging by the
 * title bar, resizing by the grip in the bottom-right corner, and
 * maximize / restore by double-clicking the title bar or with its button,
 * and the title-bar buttons themselves (minimize, maximize, close).
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

/* the title-bar buttons */
#define WIN_BTN_NONE  0
#define WIN_BTN_MIN   1
#define WIN_BTN_MAX   2
#define WIN_BTN_CLOSE 3
/* draws [_] [#] [x] ending at x = right, top y, h tall (has_max: with the
 * maximize/restore button; maxed: it shows "restore") */
void win_draw_button_row(int right, int y, int h, int has_max, int maxed);
/* which of those buttons (mx, my) is on, WIN_BTN_NONE if none */
int  win_button_hit(int right, int y, int h, int has_max, int mx, int my);
/* maximize to the whole desktop / restore the old geometry */
void win_toggle_max(win_geom_t* g);
/* the buttons of a window with this geometry, by px from its top, bh tall */
void win_draw_buttons(const win_geom_t* g, int by, int bh);
/* a click on them: maximize/restore is done here, minimize is requested
 * (the desktop takes it with winframe_take_minimize()), close is the
 * caller's job; returns the button */
int  win_button_press(win_geom_t* g, int by, int bh, int mx, int my);
int  winframe_take_minimize(void);
/* 1 if (mx, my) is the resize grip; starts resizing */
int  win_grip_press(win_geom_t* g, int mx, int my);
/* mouse moves while dragging/resizing; 1 if the geometry changed */
int  win_mouse(win_geom_t* g, int mx, int my, int left);
/* keeps the window on screen and at least its minimum size */
void win_clamp(win_geom_t* g);

#endif
