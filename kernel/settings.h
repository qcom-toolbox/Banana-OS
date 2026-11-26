/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SETTINGS_H
#define SETTINGS_H

#include "types.h"
#include "winframe.h"
#include "fb.h"

/*
 * "Settings": the system's preferences in one window - wallpaper, sound
 * volume, keyboard layout, network, date & time, the installed apps
 * (open, stop, update, uninstall) and information about the computer. Open it from the desktop, the Start menu or `settings`
 * in a terminal.
 *
 * Volume and keyboard layout are kept in /etc/settings.conf and applied
 * at boot (wallpaper and network keep their own files, see wallpaper.h
 * and net/netconf.h), so on an installed disk they survive a reboot.
 */

#define CFG_SETTINGS "/etc/settings.conf"

void settings_open(void);
#define SETTINGS_PAGE_DISPLAY 0
#define SETTINGS_PAGE_THEMES  1
#define SETTINGS_PAGE_FONTS   2
#define SETTINGS_PAGE_SCREEN  3
#define SETTINGS_PAGE_SOUND   5
#define SETTINGS_PAGE_KEYBOARD 6
#define SETTINGS_PAGE_MOUSE   8
#define SETTINGS_PAGE_LANGUAGE 7
#define SETTINGS_PAGE_NETWORK 9
#define SETTINGS_PAGE_TIME    10
#define SETTINGS_PAGE_TASKBAR 4
#define SETTINGS_PAGE_STARTUP 11
#define SETTINGS_PAGE_APPS    12
#define SETTINGS_PAGE_ABOUT   13
#define SETTINGS_PAGE_WIFI    14
#define SETTINGS_PAGE_POWER   15
#define SETTINGS_PAGE_UPDATE  16
#define SETTINGS_PAGE_USERS   17
/* the Apps page, with this installed app selected (the launcher's Details) */
void settings_show_app(const char* name);
void settings_show_status(const char* msg);
void settings_open_page(int page);
void settings_close(void);
void settings_key(char c);
int  settings_navkey(int code);     /* kbnav.h keys: the focus moves / clicks */          /* typing (the password fields) */
int  settings_is_open(void);
void settings_draw(const fb_info_t* fi);
int  settings_contains(int mx, int my);
void settings_click(int mx, int my);
void settings_mouse(int mx, int my, int left);
void settings_rclick(int mx, int my);
uint32_t settings_signature(void);
void settings_wheel(int mx, int my, int dz);

/* a .ttf file becomes an installed font (copied to FONT_DIR); 0, or -1 -
 * msg says what happened either way */
int  settings_install_font(const char* path, char* msg, int cap);

/* applies /etc/settings.conf (boot, once the filesystem is there) */
void settings_boot(void);

/* saves one setting in /etc/settings.conf (and on the installed disk) */
void settings_set(const char* key, const char* value);
int  settings_get(const char* key, char* out, int cap);

/* its window's geometry, for the window manager (kernel/gui.c) */
win_geom_t* settings_geom(void);

#endif
