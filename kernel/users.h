/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef USERS_H
#define USERS_H

#include "types.h"

/*
 * Users and groups, the Unix way: /etc/passwd, /etc/group, /etc/shadow
 * (passwd.h: the passwords) and /etc/doas.conf (elevate.h: who may act as
 * root). root (uid 0) has no login; banana (uid 1000) is the first user
 * and an administrator - a member of the wheel group (gid 10), whom
 * doas.conf lets act as root. More users: adduser, Settings > Users; each
 * has a group of its own (gid = uid), the users group (100) and
 * /home/<name>.
 *
 * The files are read again whenever they changed; the kernel keeps them
 * as they are written (other lines - comments, system accounts - stay).
 */

#define UID_ROOT      0u
#define GID_ROOT      0u
#define GID_WHEEL     10u
#define GID_USERS     100u
#define UID_FIRST     1000u                /* the first user's (banana's) */
#define UID_LAST      59999u               /* (higher: system ids, nobody 65534) */
#define UID_NOBODY    65534u
#define USERS_MAX     32
#define USER_NAME_MAX 32
#define USER_GROUPS_MAX 16

#define USERS_PASSWD  "/etc/passwd"
#define USERS_GROUP   "/etc/group"

typedef struct {
    char     name[USER_NAME_MAX];
    uint32_t uid, gid;
    char     gecos[64];                    /* the full name */
    char     home[64];
    char     shell[48];
} user_t;

/* the users in /etc/passwd (system accounts too); NULL past the end */
int           users_count(void);
const user_t* users_at(int i);
const user_t* user_by_name(const char* name);
const user_t* user_by_uid(uint32_t uid);
/* a person's account: uid 1000-59999 with a shell that is not nologin */
int           user_is_person(const user_t* u);
/* "banana", or "1001" for an id with no name (out: at least 12 bytes) */
void          user_name_of(uint32_t uid, char* out, int cap);
int           group_name_of(uint32_t gid, char* out, int cap);  /* 1 if it has a name */
int           group_by_name(const char* name, uint32_t* gid);   /* 1 if found */

/* the groups uid is in: its primary group first, then every group of
 * /etc/group that lists it; how many (at most max) */
int  user_groups(uint32_t uid, uint32_t* out, int max);
int  user_in_group(uint32_t uid, uint32_t gid);
/* root, or a member of wheel */
int  user_is_admin(uint32_t uid);
int  users_admin_count(void);              /* people who are administrators */
int  users_any_password(void);             /* some person has a password (SSH can log someone in) */

/* adduser: a new person, /home/<name> made (0700, theirs, with
 * Pictures / Downloads); admin: also in wheel. 0, or -1 with err. */
int  user_add(const char* name, const char* full_name, int admin, char* err, int ecap);
/* deluser: the account goes (and its home if remove_home); never root,
 * nor the last administrator. 0, or -1 with err. */
int  user_del(const char* name, int remove_home, char* err, int ecap);
/* in wheel or not (never the last administrator out). 0, or -1 with err. */
int  user_set_admin(const char* name, int admin, char* err, int ecap);
/* a valid new user name: [a-z_][a-z0-9_-]*, 1-31 characters */
int  user_name_ok(const char* name);

/* uid's rights for the filesystem: its id, primary group and groups */
void users_make_cred(uint32_t uid, void* fs_cred_out);    /* (an fs_cred_t*) */

/* The session: who is logged in on the computer's screen - what the
 * consoles, the desktop and the apps it starts run as. banana (the first
 * user) unless someone else logged in (login.c). Sets ~ (fs_set_home). */
uint32_t      session_uid(void);
const user_t* session_user(void);                   /* (NULL only if /etc/passwd lost it) */
void          session_set(uint32_t uid);
const char*   session_home(void);                   /* "/home/banana" */
/* the logged-in user's file rights (an fs_cred_t*): what Files and
 * Notepad act with - the desktop's own work keeps the kernel's */
const void*   session_cred(void);
/* Older programs (built before users) run by someone other than banana:
 * their paths in /home/banana are that user's home - on unless switched
 * off (settings home_redirect=0), and not for the apps listed in
 * home_redirect_skip (names, comma-separated). */
int  home_redirect_wanted(const char* app);
int  home_redirect_enabled(void);
void home_redirect_set(int on);
int  home_redirect_skips(const char* app);              /* app is listed */
void home_redirect_skip(const char* app, int skip);     /* add / take it off the list */
const char* home_redirect_list(void);                  /* the list ("" none) */

/* the people who can log in (uid 1000-59999), in /etc/passwd's order */
int           users_people(const user_t** out, int max);

/* At every start (after the filesystem is there): the files made or
 * brought up to date - /etc/group (banana in wheel) and /etc/doas.conf
 * where missing, /etc/shadow in the standard format and readable by root
 * only, /root. An installed system keeps its own users and settings. */
void users_boot(void);

/* (passwd.c) the line of a ':' file whose first field is key replaced by
 * line - NULL removes it, a new key is added at the end; the file root's,
 * of that mode. 0, or -1 */
int  users_file_line(const char* path, const char* key, const char* line, uint32_t mode);

#endif
