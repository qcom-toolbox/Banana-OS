/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TASKMGR_H
#define TASKMGR_H

#include "types.h"
#include "winframe.h"
#include "fb.h"

/*
 * "Task Manager", like Windows 11's: Processes (the apps and the kernel's
 * tasks with their CPU, memory, disk and network use; Switch to / End
 * task), Performance (live graphs of the CPU - each core too -, memory,
 * disk and Ethernet, and the GPU's details), Details (every task, its
 * core, CPU time and priority) and Users. Open it from the desktop, the
 * Start menu, a right-click on the taskbar, or `taskmgr` in a terminal.
 */

void taskmgr_open(void);
void taskmgr_show_perf(int what);        /* Performance: 0 CPU, 1 each core, 2 GPU */
void taskmgr_close(void);
int  taskmgr_is_open(void);
void taskmgr_draw(const fb_info_t* fi);
int  taskmgr_contains(int mx, int my);
void taskmgr_click(int mx, int my);
void taskmgr_mouse(int mx, int my, int left);
void taskmgr_rclick(int mx, int my);
void taskmgr_wheel(int mx, int my, int dz);
uint32_t taskmgr_signature(void);

int  taskmgr_navkey(int code);       /* kbnav.h keys */

/* its window's geometry, for the window manager (kernel/gui.c) */
win_geom_t* taskmgr_geom(void);

#endif
