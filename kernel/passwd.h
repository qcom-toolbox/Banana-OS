/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PASSWD_H
#define PASSWD_H

/*
 * Passwords, in /etc/shadow (root's only, 0600) in the standard format:
 *   name:$pbkdf2-sha256$<iterations>$<salt hex>$<hash hex>:<days>:0:99999:7:::
 * - salted and stretched, never the password itself. An empty second field
 * is an account with no password (a person who never set one: logs in and
 * acts as an administrator without being asked - as before); "!" or "*" is
 * locked (root: no login, no su - doas is the way). The older
 * "banana=pbkdf2-sha256$..." lines are read too, and rewritten at the
 * start (passwd_boot).
 */

#define PASSWD_FILE "/etc/shadow"
#define PASSWD_USER "banana"          /* the first user */
#define PASSWD_MIN  4

int passwd_is_set(const char* user);                     /* has a password */
int passwd_locked(const char* user);                     /* may never log in (root's default) */
int passwd_set(const char* user, const char* password);   /* 0, or -1 */
int passwd_clear(const char* user);                      /* no password */
int passwd_remove(const char* user);                     /* the account's line goes (deluser) */
int passwd_check(const char* user, const char* password); /* 1 if it matches (never for a locked one) */
void passwd_boot(void);

#endif
