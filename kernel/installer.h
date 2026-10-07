#ifndef INSTALLER_H
#define INSTALLER_H

#include "types.h"
#include "fb.h"

/*
 * "Install Banana OS": the live CD's installer window - the disk, the
 * keyboard layout, a password and what starts at boot, then the install
 * (or an update that keeps the files). Only offered on the live CD.
 */

int  installer_available(void);      /* started from the CD */
void installer_open(void);
void installer_close(void);
int  installer_is_open(void);
void installer_draw(const fb_info_t* fi);
int  installer_contains(int mx, int my);
void installer_click(int mx, int my);
void installer_mouse(int mx, int my, int left);
void installer_rclick(int mx, int my);
void installer_key(char c);
uint32_t installer_signature(void);

#endif
