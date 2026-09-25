#ifndef PASSWD_H
#define PASSWD_H

/*
 * User passwords (for SSH logins), stored in /etc/shadow as
 *   banana=pbkdf2-sha256$<iterations>$<salt hex>$<hash hex>
 * - salted and stretched, never the password itself.
 */

#define PASSWD_FILE "/etc/shadow"
#define PASSWD_USER "banana"          /* Banana OS has one user */
#define PASSWD_MIN  4

int passwd_is_set(const char* user);
int passwd_set(const char* user, const char* password);   /* 0, or -1 */
int passwd_clear(const char* user);
int passwd_check(const char* user, const char* password); /* 1 if it matches */

#endif
