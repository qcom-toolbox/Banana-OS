#ifndef GUI_H
#define GUI_H

#include "types.h"

void gui_init(void);
void gui_poll(void);
/* the timer interrupt: moves the mouse pointer while no task draws the desktop */
void gui_cursor_tick(void);

void gui_set_enabled(int enabled);
int  gui_is_enabled(void);

/* returns 1 if key was consumed by GUI */
int gui_handle_key(char c);

/* pass 'A' (up) or 'B' (down) for ESC [ sequences */
int gui_handle_arrow(char esc_code);

/* Which vt currently owns keyboard focus: the frontmost open GUI terminal
 * window's vt, or 0 for the plain console shell when the GUI is off (or
 * on, but no window is open/focused yet - nothing reads keys then).
 * Each per-window shell task must check this before consuming a
 * keystroke, so typing only ever reaches the window you're looking at. */
int gui_focused_vt(void);

/* Closes the GUI terminal window that owns `vt` (same as clicking its 'x'
 * button). Used by the shell's "exit" builtin. Returns 1 if a window was
 * closed. */
int gui_close_terminal_by_vt(int vt);

/* opens the Files window at `path` (NULL: home); 0 if the desktop is not running */
int gui_open_files(const char* path);
/* opens the Browser window (url NULL: its current page); 0 if the desktop is not running */
int gui_open_browser(const char* url);
/* opens Notepad (path NULL: what it has open, or a new text); 0 if the desktop is not running */
int gui_open_notepad(const char* path);
/* 1 while Notepad is the front window (it gets the keyboard) */
int gui_notepad_focused(void);
/* 1 while an installed app's window is in front (it gets the keyboard) */
int gui_appwin_focused(void);

/* opens Apps / the Task Manager; 0 if the desktop is not running */
int gui_open_apps(void);
int gui_open_taskmgr(void);
int gui_open_settings(void);   /* the Settings window (0 without the desktop) */
void gui_raise_files(void);
int  gui_open_installer(void);       /* the live CD's installer window; 0 if not here */
void gui_screen_changed(void);       /* a new resolution (kernel/display.c) */

/* the open windows (taskbar order), for the Task Manager */
typedef struct {
    int  handle;
    char title[48];
    int  minimized;
    int  focused;
} gui_win_info_t;
int  gui_windows(gui_win_info_t* out, int max);
void gui_window_close(int handle);
void gui_window_activate(int handle);   /* restore + bring to front */

#endif

