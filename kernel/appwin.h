/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef APPWIN_H
#define APPWIN_H

#include "fs.h"
#include "types.h"
#include "fb.h"
#include "winframe.h"
#include "usb.h"
#include "../sdk/include/banana_api.h"

/*
 * Windows of installed apps (kernel/app.c's win_* calls). Each has a
 * pixel buffer the app draws into and a queue of input events; the
 * desktop (kernel/gui.c) draws them like its own windows and routes the
 * mouse and - while one is in front - the keyboard to them.
 */

#define APPWIN_MAX 32
#define APPWIN_VT  1002          /* gui_focused_vt() while an app window is in front */

/* ── app side (owner: the app's process id) ── */
int       appwin_open(int owner, const char* title, int w, int h);   /* id, or -1 */
/* an app's window (owner: its app id): the picture is in the app's memory */
int       appwin_open_app(int app, const char* title, int w, int h);
uint32_t* appwin_pixels(int id, int owner);
void      appwin_update(int id, int owner);
int       appwin_event(int id, int owner, banana_event_t* ev);
void      appwin_close(int id, int owner);
void      appwin_close_owner(int owner);        /* every window of an app that ends */
void      appwin_set_title(int id, int owner, const char* title);
void      appwin_size(int id, int owner, int* w, int* h);
/* the user may resize / maximize it (the app gets BANANA_EV_RESIZE) */
void      appwin_set_resizable(int id, int owner, int min_w, int min_h);
int       appwin_capture_mouse(int id,int owner,int capture);
int       appwin_mouse_captured(int id,int owner);
int       appwin_capture_active(void); /* check focus/Escape, release if needed */
int       appwin_capture_pending(void); /* read-only; cursor timer may use it */
void      appwin_release_mouse(void);
int       appwin_capture_input(const mouse_state_t* m,int screen_w,int screen_h);
/* 1 once the user clicked an app window's close button twice (the app
 * did not quit on the first BANANA_EV_CLOSE): the app is stopped */
int       appwin_kill_requested(int owner);

/* ── desktop side ── */
int      appwin_is_open(void);
void     appwin_draw(const fb_info_t* fi);
/* only the front window's pixels changed: drawn alone, its client rectangle (1), or 0 */
int  appwin_fast_frame(int* x, int* y, int* w, int* h);
int      appwin_contains(int mx, int my);
/* files let go over an app window: it gets BANANA_EV_DROP (1 if one was there) */
int      appwin_drop(int mx, int my, const char (*paths)[FS_PATH_LEN], int n);
int      appwin_dropped(int id, int owner, int index, char* path, int cap);
int      appwin_resize_cursor(int mx, int my);
void     appwin_wheel(int mx, int my, int dz);   /* as arrow keys to the window under it */   /* the mouse shows the resize arrows */
void     appwin_click(int mx, int my);
void     appwin_mouse(int mx, int my, int left);
uint32_t appwin_signature(void);
void     appwin_close_all(void);                /* the desktop quits: ask every app to close */
void     appwin_key(char c);                    /* a key while an app window is in front */
void     appwin_key_esc(void);                  /* an Esc with no [ after it: sent alone */
void     appwin_fkey(int k);                    /* F1-F12 (KEYF_*) for the front app window */
/* media keys: the window that asked for them (win_media_keys) gets them, wherever the focus is */
void     appwin_set_media(int id, int owner);
void     appwin_set_wheel(int id, int owner);     /* wheel events instead of arrow keys */
int      appwin_media_key(int k);             /* 1 if a window took it */
void     appwin_rclick(int mx, int my);
void     appwin_focus(int focused);             /* the app windows went to front / back */

/* taskbar / Task Manager: window id 0..APPWIN_MAX-1 */
int      appwin_info(int id, char* title, int cap, int* minimized);   /* 0 if unused */
int      appwin_is_terminal(int id);             /* integrated Terminal glyph */
int appwin_fullscreen(int id);
void appwin_toggle_fullscreen(int id);
int      appwin_front_id(void);                 /* the topmost visible one, -1 */
int      appwin_focused_owner(void);            /* the app whose window has the keyboard, -1 */
void     appwin_minimize(int id, int min);
void     appwin_activate(int id);               /* restore + raise */
void     appwin_request_close(int id);          /* as its close button */
int      appwin_any_visible(void);
/* 1 once after a window opened (the desktop brings it to the front) */
int      appwin_take_new(void);

/* the window manager (kernel/gui.c): virtual desktops hide windows, and
 * snapping, moving and dragging (Alt+drag) work on any of them */
void     appwin_set_hidden(int id, int hidden);
int      appwin_get_rect(int id, int* x, int* y, int* w, int* h);   /* outer; 0 if unused */
int      appwin_state(int id, int* maxed, int* snapped, int* resizable);
void     appwin_snap(int id, const win_rect_t* r, int where);        /* SNAP_NONE: back to its size */
void     appwin_move(int id, int x, int y);
void     appwin_start_drag(int id, int mx, int my);
int      appwin_at(int mx, int my, int* title);                     /* the window there, -1 */

#endif
