#ifndef KBNAV_H
#define KBNAV_H

#include "types.h"

/*
 * Keyboard focus inside a window. While it draws, a window adds the places
 * a click does something (buttons, list rows, check boxes, thumbnails...);
 * then Tab / Shift+Tab go through them in order, the arrow keys move to the
 * nearest one in that direction, and Enter or Space clicks it. A ring shows
 * the focus once the keyboard is used (a mouse click hides it again).
 */

/* keys, as the desktop hands them to windows (ESC [ A ... decoded) */
enum {
    KB_UP = 0x101, KB_DOWN, KB_RIGHT, KB_LEFT, KB_HOME, KB_END, KB_PGUP, KB_PGDN, KB_DEL,
    KB_TAB, KB_BACKTAB, KB_ENTER, KB_SPACE, KB_ESC,
};

#define KBNAV_MAX 160
typedef struct {
    int16_t x[KBNAV_MAX], y[KBNAV_MAX], w[KBNAV_MAX], h[KBNAV_MAX];
    int n, focus, shown;
} kbnav_t;

void kbnav_begin(kbnav_t* k);                          /* a new frame: no places yet */
void kbnav_add(kbnav_t* k, int x, int y, int w, int h);
void kbnav_draw(const kbnav_t* k, uint32_t color);     /* the focus ring (when shown) */
/* a key: 1 and (*cx, *cy) to click (Enter / Space), 2 if the focus moved, 0 not ours */
int  kbnav_key(kbnav_t* k, int code, int* cx, int* cy);
void kbnav_mouse(kbnav_t* k);                          /* a click: the ring goes */
int  kbnav_focused(const kbnav_t* k, int* x, int* y, int* w, int* h);   /* 1 if shown */

#endif
