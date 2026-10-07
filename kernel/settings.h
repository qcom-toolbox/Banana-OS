#ifndef SETTINGS_H
#define SETTINGS_H

#include "types.h"
#include "fb.h"

/*
 * "Settings": the system's preferences in one window - wallpaper, sound
 * volume, keyboard layout, network, date & time and information about
 * the computer. Open it from the desktop, the Start menu or `settings`
 * in a terminal.
 *
 * Volume and keyboard layout are kept in /etc/settings.conf and applied
 * at boot (wallpaper and network keep their own files, see wallpaper.h
 * and net/netconf.h), so on an installed disk they survive a reboot.
 */

#define CFG_SETTINGS "/etc/settings.conf"

void settings_open(void);
#define SETTINGS_PAGE_DISPLAY 0
#define SETTINGS_PAGE_ABOUT   6
void settings_open_page(int page);
void settings_close(void);
int  settings_is_open(void);
void settings_draw(const fb_info_t* fi);
int  settings_contains(int mx, int my);
void settings_click(int mx, int my);
void settings_mouse(int mx, int my, int left);
void settings_rclick(int mx, int my);
uint32_t settings_signature(void);

/* applies /etc/settings.conf (boot, once the filesystem is there) */
void settings_boot(void);

/* saves one setting in /etc/settings.conf (and on the installed disk) */
void settings_set(const char* key, const char* value);
int  settings_get(const char* key, char* out, int cap);

#endif
