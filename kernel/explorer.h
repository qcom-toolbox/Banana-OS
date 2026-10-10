#ifndef EXPLORER_H
#define EXPLORER_H

#include "types.h"
#include "fb.h"

/*
 * "Files": the desktop's file explorer window (kernel/gui.c drives it).
 * Browse folders, preview text and pictures, create folders, delete,
 * open a file in the editor or a terminal in the current folder, and
 * use a picture as the wallpaper.
 */

void explorer_open(const char* path);   /* NULL: home folder */
void explorer_close(void);
void explorer_select(const char* name);  /* selects that item of the folder shown */
int  explorer_is_open(void);

void explorer_draw(const fb_info_t* fi);
int  explorer_contains(int mx, int my);  /* point inside the window */
void explorer_click(int mx, int my);     /* left button went down there */
void explorer_mouse(int mx, int my, int left);   /* dragging the window */
void explorer_rclick(int mx, int my);    /* right-click: its menu */
void explorer_key(char c);               /* keys while Files is in front */
void explorer_fkey(int k);               /* F2 rename, F5 refresh (KEYF_*) */
/* changes whenever the window needs repainting (gui.c's redraw check) */
uint32_t explorer_signature(void);

/* provided by gui.c: open a terminal window and run `cmd` in it */
void gui_terminal_run(const char* cmd);

void explorer_wheel(int mx, int my, int dz);  /* scrolls the file list (+ = down) */

#endif
