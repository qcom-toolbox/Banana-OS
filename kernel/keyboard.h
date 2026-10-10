#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "types.h"

void keyboard_init(void);
char keyboard_getchar(void);
char keyboard_try_getchar(void); /* non-blocking: returns 0 if no key */
void keyboard_readline(char* buf, int maxlen);
const char* keyboard_layout_name(void);
const char* keyboard_layouts_help(void);
int keyboard_set_layout(const char* name);
int keyboard_caps_lock(void);    /* 1 while Caps Lock is on */

/* One-shot: 1 exactly once per Ctrl+Alt+Delete press, then clears. */
int keyboard_ctrl_alt_del_pending(void);
/* the modifiers held now: 1 shift, 2 ctrl, 4 alt */
int keyboard_mods(void);

/* USB HID keyboards feed set-1 scancodes through the PS/2 decoder */
void keyboard_feed_scancode(uint8_t sc);
/* queues text as typed input for whoever reads the keyboard next */
void keyboard_inject(const char* s);
/* only task `pid` reads the keyboard from now on (-1: everyone again) - the lock screen */
void keyboard_set_owner(int pid);

/* Function keys, the Windows key and the multimedia keys (on laptops:
 * Fn + F-key) are not characters: each press is an event of its own,
 * taken with keyboard_take_fkey() (0 if none). The volume keys also change
 * the volume themselves, wherever they are pressed. */
#define KEYF_F1      1           /* F1..F12: KEYF_F1 .. KEYF_F1 + 11 */
#define KEYF_WIN     13          /* the Windows key: the Start menu */
#define KEYF_MUTE    16
#define KEYF_VOLDOWN 17
#define KEYF_VOLUP   18
#define KEYF_PLAY    19          /* play / pause */
#define KEYF_STOP    20
#define KEYF_NEXT    21
#define KEYF_PREV    22
#define KEYF_SHIFT   0x100       /* held with an F-key */
#define KEYF_CTRL    0x200
#define KEYF_ALT     0x400
#define KEYF_CODE(k) ((k) & 0xFF)
int keyboard_take_fkey(void);

#endif
