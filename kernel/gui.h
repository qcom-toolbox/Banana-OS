/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GUI_H
#define GUI_H

#include "fs.h"

#include "types.h"

void gui_init(void);
void gui_poll(void);
/* the timer interrupt: moves the mouse pointer while no task draws the desktop */
void gui_cursor_tick(void);

void gui_set_enabled(int enabled);
int  gui_is_enabled(void);
void gui_request_redraw(void);            /* the desktop drawn again soon (any task) */

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
/* opens a shortcut's target (kernel/shortcut.h): app:NAME, a web address, a folder, a file; 0 if it cannot */
int gui_open_target(const char* target);
/* puts a shortcut to target on the desktop (name.link in ~/Desktop); 0 = ok */
int gui_desktop_shortcut(const char* name, const char* target);
/* opens Notepad (path NULL: what it has open, or a new text); 0 if the desktop is not running */
int gui_open_notepad(const char* path);
/* 1 while Notepad is the front window (it gets the keyboard) */
int gui_notepad_focused(void);
/* 1 while an installed app's window is in front (it gets the keyboard) */
int gui_appwin_focused(void);

/* opens Apps / the Task Manager; 0 if the desktop is not running */
int gui_open_apps(void);
int gui_open_taskmgr(void);
int gui_open_devmgr(void);
int gui_open_diskmgr(void);
int gui_open_settings_page(int page);   /* SETTINGS_PAGE_* */
int gui_open_settings(void);   /* the Settings window (0 without the desktop) */
int gui_open_settings_app(const char* name);   /* Settings > Apps (name selected, or NULL) */
void gui_raise_files(void);
int  gui_open_installer(void);       /* the live CD's installer window; 0 if not here */
void gui_text_changed(void);
/* drag and drop: Files (and the desktop) hand the desktop what is dragged;
 * it follows the mouse and lands where the button comes up */
#define GUI_DRAG_FILES   1
#define GUI_DRAG_DESKTOP 2
void gui_drag_begin(const char (*paths)[FS_PATH_LEN], int n, int source);
int  gui_drag_has(const char* dir, const char* name);   /* dir/name is being dragged */
/* the taskbar: 0 icons and titles, 1 icons only, 2 titles only */
void gui_set_taskbar_style(int style);
int  gui_taskbar_style(void);
/* the virtual desktops' buttons on the taskbar: shown (1, the default) or not */
void gui_set_taskbar_desks(int on);
int  gui_taskbar_desks(void);         /* the text size / interface font (Settings > Fonts) */
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

