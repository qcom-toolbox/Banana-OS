#ifndef NOTEPAD_H
#define NOTEPAD_H

#include "types.h"
#include "fb.h"

/*
 * "Notepad": the desktop's text editor window - mouse and keyboard
 * editing, selection, cut/copy/paste through the system clipboard,
 * find, open/save, scrollbars; resizable. kernel/gui.c drives it and
 * hands it the keyboard while it is the front window.
 */

#define NOTEPAD_VT 1001             /* gui_focused_vt() while Notepad is in front */

void notepad_open(const char* path);   /* NULL: keep what is open (or a new text) */
void notepad_close(void);
int  notepad_is_open(void);

void notepad_draw(const fb_info_t* fi);
int  notepad_contains(int mx, int my);
void notepad_click(int mx, int my);
void notepad_mouse(int mx, int my, int left);
uint32_t notepad_signature(void);

void notepad_key(char c);           /* a keystroke (ESC sequences come byte by byte) */
void notepad_fkey(int k);          /* F3: find the next one (KEYF_*) */
void notepad_paste(void);

void notepad_wheel(int mx, int my, int dz);   /* scrolls the text (+ = down) */

#endif
