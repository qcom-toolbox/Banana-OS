#ifndef TASKMGR_H
#define TASKMGR_H

#include "types.h"
#include "fb.h"

/*
 * "Task Manager": what runs - the open windows and installed apps (with
 * Switch to / End task), the kernel's tasks with their CPU use, and live
 * CPU / memory graphs. Open it from the desktop, the Start menu, a
 * right-click on the taskbar, or `taskmgr` in a terminal.
 */

void taskmgr_open(void);
void taskmgr_close(void);
int  taskmgr_is_open(void);
void taskmgr_draw(const fb_info_t* fi);
int  taskmgr_contains(int mx, int my);
void taskmgr_click(int mx, int my);
void taskmgr_mouse(int mx, int my, int left);
void taskmgr_rclick(int mx, int my);
uint32_t taskmgr_signature(void);

int  taskmgr_navkey(int code);       /* kbnav.h keys */

#endif
