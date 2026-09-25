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
int  explorer_is_open(void);

void explorer_draw(const fb_info_t* fi);
int  explorer_contains(int mx, int my);  /* point inside the window */
void explorer_click(int mx, int my);     /* left button went down there */
void explorer_mouse(int mx, int my, int left);   /* dragging the window */
/* changes whenever the window needs repainting (gui.c's redraw check) */
uint32_t explorer_signature(void);

/* provided by gui.c: open a terminal window and run `cmd` in it */
void gui_terminal_run(const char* cmd);

#endif
