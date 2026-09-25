#ifndef GUI_H
#define GUI_H

#include "types.h"

void gui_init(void);
void gui_poll(void);

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

#endif

