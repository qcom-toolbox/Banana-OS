#ifndef LAUNCHER_H
#define LAUNCHER_H

#include "types.h"
#include "fb.h"

/*
 * "Apps": the desktop's app launcher - every installed app (pkg) as a
 * tile; click one to start it, right-click for Open / Details /
 * Uninstall. kernel/gui.c drives it like the other app windows.
 */

void launcher_open(void);
void launcher_close(void);
int  launcher_is_open(void);
void launcher_draw(const fb_info_t* fi);
int  launcher_contains(int mx, int my);
void launcher_click(int mx, int my);
void launcher_mouse(int mx, int my, int left);
void launcher_rclick(int mx, int my);
uint32_t launcher_signature(void);

#endif
