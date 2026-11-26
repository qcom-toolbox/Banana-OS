/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef VCONS_H
#define VCONS_H

#include "types.h"

/*
 * The consoles (64-bit): tty1..tty6 on the screen - Alt+F1..F6 switch
 * (Ctrl+Alt+F1..F6 from the desktop) - and ttyS0 on the serial line. Each
 * is a pty the kernel holds the master of, with bash on its slave (started
 * again when it ends). What programs write goes through the Terminal app's
 * emulator (kernel/kvt.c: xterm's escape sequences) and is drawn into the
 * console's text grid; what the kernel itself prints for the screen goes to
 * the console shown. The shell before the desktop (shell/shell.c) runs
 * them: vcons_start, then vcons_pump and the keys in a loop.
 */

#define VCONS_SCREENS 6

/* takes over the console: what is on the screen now becomes tty1's; 0, or
 * -1 (the 32-bit kernel, no /bin/bash: the kernel's shell stays) */
int  vcons_start(void);
int  vcons_running(void);
/* bash cannot run on the first console (why): the kernel's shell takes it */
int  vcons_broken(const char** why);
void vcons_stop(void);
/* the environment the consoles' shells get (TERM...) */
char** vcons_env(void);
char** vcons_env_for(const void* user);   /* (a user_t*: its HOME, USER, LOGNAME; NULL the one logged in) */
/* output to the screen, input from the serial line, shells started again:
 * 1 if there was something to do */
int  vcons_pump(void);
/* a key for the console shown: a character, or VT_KEY_* (apps/terminal/vt.h)
 * with VT_MOD_*; an F-key event (keyboard_take_fkey: Alt+F1..F6 switch) */
void vcons_key(int key, int mods);
void vcons_fkey(int code);
/* shows console n (0..5) */
void vcons_switch(int n);
int  vcons_current(void);
/* the pty of /dev/<name>: "tty1".."tty6", "tty0" (the one shown), "ttyS0"; NULL */
struct pty* vcons_tty(const char* name);
/* t's minor if it is a console's (1..6 tty1..6, 64 ttyS0), else 0 */
int vcons_minor(struct pty* t);
/* the pty of console n (0..5, VCONS_SCREENS: the serial line), NULL */
struct pty* vcons_pty(int n);

#endif
