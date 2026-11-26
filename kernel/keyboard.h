/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "types.h"

void keyboard_init(void);
char keyboard_getchar(void);
char keyboard_try_getchar(void); /* non-blocking: returns 0 if no key */
void keyboard_unget(char c);     /* puts a character back in front */
void keyboard_readline(char* buf, int maxlen);
const char* keyboard_layout_name(void);
const char* keyboard_layouts_help(void);
int keyboard_set_layout(const char* name);
/* Win+Space: English (US) and back to the layout chosen: the label of the one in use */
const char* keyboard_toggle_layout(void);
/* the layouts there are (kernel/kblayouts.c): the i-th one's name (as
 * keyboard_set_layout takes it) and *label ("French (AZERTY)"), or NULL */
int keyboard_layout_count(void);
const char* keyboard_layout_at(int i, const char** label);
int keyboard_test_type(const char* name, const uint8_t* codes, int n, char* out, int cap);   /* (selftest) */
int keyboard_caps_lock(void);    /* 1 while Caps Lock is on */

/* One-shot: 1 exactly once per Ctrl+Alt+Delete press, then clears. */
int keyboard_ctrl_alt_del_pending(void);
/* the modifiers held now: 1 shift, 2 ctrl, 4 alt */
int keyboard_mods(void);

/* USB HID keyboards feed set-1 scancodes through the PS/2 decoder */
void keyboard_feed_scancode(uint8_t sc);
/* the RDP server (net/rdpd.c) feeds set-1 scancodes from its client; this
 * forces the decoder back to set 1 first (a USB keyboard may have left it
 * in set 2) so E0 prefixes and break codes are read the same way */
void keyboard_feed_set1(uint8_t sc);

/* Raw set-1 scancode capture for the RDP client (kernel/rdpclient.c).
 * While on, keys are NOT delivered locally (no characters, no F-keys); the
 * client drains the raw make/break codes and forwards them to the server,
 * so the remote side sees the exact keys. Turned on while the RDP window
 * has focus, off otherwise. */
void keyboard_capture_raw(int on);
/* next captured scancode: 1 and fills *code (set-1), *down (1 press /
 * 0 release), *e0 (extended), or 0 if none waiting */
int  keyboard_poll_raw(uint8_t* code, int* down, int* e0);
/* The keys held down now, for games: several at once (the character
 * stream only repeats the last one pressed). By position: set-1 codes,
 * + 0x80 for the E0 ones (0xC8 the Up arrow), whatever the keyboard -
 * PS/2 set 1 or 2, USB, or a Remote Desktop client. 0 for everyone but
 * the lock screen while the screen is locked. */
int  keyboard_held(int code);
/* the key that types c on a US keyboard ('w', '1', ' ', '\n', 27...):
 * WASD stays under the left hand whatever the layout */
int  keyboard_held_char(int c);
/* 1 if set-1 `code` went down since the last call (a tap too short for
 * keyboard_held to be seen), and forgets it */
int  keyboard_take_pressed(int code);
/* Physical Escape presses, independent of consumable app key state.
 * Arrow-key ESC sequences do not increment this counter. */
uint32_t keyboard_escape_sequence(void);
/* what the key at set-1 `code` types on a US keyboard, 0 if nothing */
char keyboard_us_char(int code, int shift);
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
#define KEYF_TAB     23          /* Alt+Tab (with KEYF_ALT, KEYF_SHIFT): the window switcher */
#define KEYF_ALTUP   24          /* Alt let go after an Alt+Tab */
#define KEYF_MENU    25          /* the Menu key (or Shift+F10): a context menu */
/* the Windows key held with another key: KEYF_WINKEY | that key - a
 * small letter or digit, '\t', or one of KEYW_* (the Windows key alone,
 * pressed and let go, is KEYF_WIN) */
#define KEYF_WINKEY  0x800
#define KEYW_UP      0x81
#define KEYW_DOWN    0x82
#define KEYW_RIGHT   0x83
#define KEYW_LEFT    0x84
#define KEYF_SHIFT   0x100       /* held with an F-key */
#define KEYF_CTRL    0x200
#define KEYF_ALT     0x400
#define KEYF_CODE(k) ((k) & 0xFF)
int keyboard_take_fkey(void);

/* A kernel command run for a program (kernel/kcmd.c): until _end, the
 * calling task's keys come from get(ctx, wait) - a byte (> 0), 0 none yet
 * (wait 0), -1 the end; keyboard_getchar gives ^D (4) at the end */
int  keyboard_redirect_begin(int (*get)(void* ctx, int wait), void* ctx);
void keyboard_redirect_end(void);
int  keyboard_redirected(void);   /* 1: this task's keys are a program's stdin (not the desktop's) */

/* 0: the serial line no longer types on the console (kernel/vcons.c reads it) */
void keyboard_serial_input(int on);

#endif
