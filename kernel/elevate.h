/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ELEVATE_H
#define ELEVATE_H

#include "types.h"

/*
 * Acting as root (an administrator's rights): doas, su, the system's
 * commands that change the system, the App Store's installs.
 *
 * Who may: /etc/doas.conf - by default "permit persist :wheel", every
 * administrator. An administrator with no password set is never asked
 * (as before users). Otherwise:
 *   - in a terminal (doas, pkg install...): the password, typed;
 *   - on the desktop: a small window over the others - not over the whole
 *     screen, nothing else is stopped - with Yes / No for an
 *     administrator, or the password if Settings > Users > Administrator
 *     prompt says so; a standard user picks an administrator and types
 *     that one's password.
 * Remembered for a while once given (Settings: 0 - every time - to 60
 * minutes; 5 by default), per user and per terminal / the desktop.
 */

/* the settings (/etc/settings.conf: elevate_password, elevate_minutes) */
int  elevate_gui_wants_password(void);
void elevate_set_gui_password(int on);
int  elevate_remember_minutes(void);
void elevate_set_remember_minutes(int minutes);

/* doas.conf: may uid act as target (cmd: the command, may be NULL)?
 * 1 permitted (*nopass: without a password, *persist: remembered), 0 not */
int  elevate_permitted(uint32_t uid, uint32_t target, const char* cmd, int* nopass, int* persist);

/* may uid become target here (session: a terminal's or the desktop's key)
 * without being asked now: root, nopass, no password set, or remembered */
int  elevate_silent(uint32_t uid, uint32_t target, uint32_t session, const char* cmd);
/* uid's password was given (or it was allowed on the desktop): remembered
 * for session for the set minutes (if doas.conf says persist) */
void elevate_remember(uint32_t uid, uint32_t session);
void elevate_forget(uint32_t uid, uint32_t session);  /* doas -L */

#define ELEVATE_DESKTOP 0u                        /* the desktop's session key */

/* A system command (the kernel's, by kcmd or the built-in shell) that
 * changes the system - pkg install, apt upgrade, update, ifconfig eth0
 * ..., adduser, hostname <name>...: what it does (for the prompt), or NULL */
const char* elevate_cmd_privileged(int argc, char** argv);

/* An app (uid, session) wants the system to do something an administrator
 * may (what: "install apps"): 1 allowed - silently or by the desktop's
 * prompt, which this waits for (a task of its own, not the desktop's) -
 * 0 refused or no desktop to ask on. */
int  elevate_request(uint32_t uid, uint32_t session, const char* app, const char* what);

/* The desktop's prompt (kernel/gui.c draws it, gives it clicks and keys).
 * A kernel window (Settings) asks without waiting: done(ctx, 1 / 0) is
 * called from the desktop once it is answered. */
typedef void (*elevate_done_fn)(void* ctx, int allowed);
void elevate_ask_async(uint32_t uid, const char* app, const char* what, elevate_done_fn done, void* ctx);

typedef struct {
    int         open;
    const char* app;
    const char* what;
    int         need_password;                     /* the field shown */
    int         admin_pick;                        /* a standard user: whose password (an administrator's) */
    char        admin[32];
    char        typed[64];
    int         tries;
    char        err[64];
    int         x, y, w, h;                         /* where the desktop drew it */
} elevate_prompt_t;
elevate_prompt_t* elevate_prompt(void);            /* the one shown (open 0: none) */
void elevate_prompt_answer(int allow);              /* Yes / No / Enter / Escape */
void elevate_prompt_next_admin(void);               /* a standard user: the next administrator */
int  elevate_prompt_focused(void);                   /* it has the keys (clicked into) */
void elevate_prompt_focus(int on);

#endif
