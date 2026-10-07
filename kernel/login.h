#ifndef LOGIN_H
#define LOGIN_H

/*
 * The login screen at boot. Shown when a password is set and the login
 * is switched on (Settings > Startup; on by default): the console and the
 * desktop only come up after the right password. SSH logins check the
 * same password themselves.
 */

int  login_enabled(void);           /* the switch ("login" in /etc/rc.conf) */
void login_set_enabled(int on);
int  login_required(void);          /* switched on and a password is set */
void login_screen(void);            /* returns once the password was right */

/* locks the desktop until the password is typed (-1: no password set) */
int  login_lock(void);
int  login_is_locked(void);

#endif
