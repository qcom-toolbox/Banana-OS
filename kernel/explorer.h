#ifndef EXPLORER_H
#define EXPLORER_H

#include "types.h"
#include "fb.h"
#include "fs.h"

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
void explorer_rclick(int mx, int my);
void explorer_menu_key(void);
/* drag and drop: a drag passing over / let go over Files (the paths dragged):
 * moved (copy: copied) into the folder under the mouse, or the one shown */
void explorer_drag_over(int mx, int my);
int  explorer_drop(const char (*paths)[FS_PATH_LEN], int n, int mx, int my, int copy);
void explorer_launch(const char* path);
void explorer_start_rename(void);       /* the selected item's name, to type over */   /* opened as a double click would */
void explorer_refresh(void);          /* the Menu key: the selected item's menu */    /* right-click: its menu */
void explorer_key(char c);               /* keys while Files is in front */
void explorer_fkey(int k);               /* F2 rename, F5 refresh (KEYF_*) */
/* changes whenever the window needs repainting (gui.c's redraw check) */
uint32_t explorer_signature(void);

/* provided by gui.c: open a terminal window and run `cmd` in it */
void gui_terminal_run(const char* cmd);

void explorer_wheel(int mx, int my, int dz);  /* scrolls the file list (+ = down) */

#endif
