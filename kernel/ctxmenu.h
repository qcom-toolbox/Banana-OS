#ifndef CTXMENU_H
#define CTXMENU_H

#include "types.h"

/*
 * The desktop's right-click (context) menu: one at a time, drawn above
 * everything. A window builds the items it wants for the spot that was
 * right-clicked and gets the chosen item's id back through its callback
 * (called from gui_poll(), in whatever task runs the desktop).
 */

#define CTX_MAX_ITEMS 16
#define CTX_SEP       "-"            /* a label of "-" is a separator line */

typedef struct {
    const char* label;               /* copied: may be a temporary buffer */
    int         id;
    int         disabled;
} ctx_item_t;

typedef void (*ctx_cb_t)(int id, void* arg);

/* opens a menu at (x, y) - moved so it stays on screen */
void ctxmenu_open(int x, int y, const ctx_item_t* items, int n, ctx_cb_t cb, void* arg);
void ctxmenu_close(void);
int  ctxmenu_is_open(void);
/* a left click: chooses an item, or (outside) just closes it; 1 if the
 * click was the menu's (it never reaches the windows below then) */
int  ctxmenu_click(int mx, int my);
void ctxmenu_hover(int mx, int my);
/* the open menu covers (mx, my) */
int  ctxmenu_contains(int mx, int my);
void ctxmenu_draw(void);
uint32_t ctxmenu_signature(void);

/* the keyboard (kbnav.h codes) while a menu is open: 1 if it took the key */
int  ctxmenu_key(int code);
void ctxmenu_select_first(void);

#endif
