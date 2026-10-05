#ifndef APPWIN_H
#define APPWIN_H

#include "types.h"
#include "fb.h"
#include "../sdk/include/banana_api.h"

/*
 * Windows of installed apps (kernel/app.c's win_* calls). Each has a
 * pixel buffer the app draws into and a queue of input events; the
 * desktop (kernel/gui.c) draws them like its own windows and routes the
 * mouse and - while one is in front - the keyboard to them.
 */

#define APPWIN_MAX 6
#define APPWIN_VT  1002          /* gui_focused_vt() while an app window is in front */

/* ── app side (owner: the app's process id) ── */
int       appwin_open(int owner, const char* title, int w, int h);   /* id, or -1 */
uint32_t* appwin_pixels(int id, int owner);
void      appwin_update(int id, int owner);
int       appwin_event(int id, int owner, banana_event_t* ev);
void      appwin_close(int id, int owner);
void      appwin_close_owner(int owner);        /* every window of an app that ends */
void      appwin_set_title(int id, int owner, const char* title);
void      appwin_size(int id, int owner, int* w, int* h);
/* 1 once the user clicked an app window's close button twice (the app
 * did not quit on the first BANANA_EV_CLOSE): the app is stopped */
int       appwin_kill_requested(int owner);

/* ── desktop side ── */
int      appwin_is_open(void);
void     appwin_draw(const fb_info_t* fi);
int      appwin_contains(int mx, int my);
void     appwin_click(int mx, int my);
void     appwin_mouse(int mx, int my, int left);
uint32_t appwin_signature(void);
void     appwin_close_all(void);                /* the desktop quits: ask every app to close */
void     appwin_key(char c);                    /* a key while an app window is in front */
void     appwin_rclick(int mx, int my);
void     appwin_focus(int focused);             /* the app windows went to front / back */

/* taskbar / Task Manager: window id 0..APPWIN_MAX-1 */
int      appwin_info(int id, char* title, int cap, int* minimized);   /* 0 if unused */
int      appwin_front_id(void);                 /* the topmost visible one, -1 */
void     appwin_minimize(int id, int min);
void     appwin_activate(int id);               /* restore + raise */
void     appwin_request_close(int id);          /* as its close button */
int      appwin_any_visible(void);
/* 1 once after a window opened (the desktop brings it to the front) */
int      appwin_take_new(void);

#endif
