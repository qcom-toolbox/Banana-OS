/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "usercmds.h"
#include "netcmds.h"
#include "../kernel/users.h"
#include "../kernel/passwd.h"
#include "../kernel/elevate.h"
#include "../kernel/fs.h"
#include "../kernel/fsdisk.h"
#include "../kernel/kstring.h"
#include "../kernel/terminal.h"

/*
 * The users' commands (kernel/users.h): passwd, adduser / useradd,
 * deluser / userdel, usermod, lsusers. Changing another user, or the users,
 * is an administrator's (shell/shell.c lets only an administrator's run:
 * elevate.h); one's own password is anyone's, with the current one.
 */

uint32_t shell_caller_uid(void);
int shell_read_secret(const char* prompt, char* buf, int cap);

static void say(const char* s) { terminal_writeln(s); }

static void fail(const char* cmd, const char* why) {
    terminal_write_color(cmd, VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    terminal_write_color(": ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    terminal_writeln(why);
}

/* a new password, twice; "" allowed (none) when empty_ok. 1, or 0 (cancelled / different) */
static int new_password(const char* cmd, char* a, int cap, int empty_ok) {
    char b[128];
    if (shell_read_secret(empty_ok ? "New password (empty: none): " : "New password: ", a, cap) < 0) return 0;
    if (!a[0] && empty_ok) return 1;
    if ((int)strlen(a) < PASSWD_MIN) {
        char m[64];
        ksnprintf(m, sizeof(m), "too short - at least %d characters", PASSWD_MIN);
        fail(cmd, m);
        memset(a, 0, (size_t)cap);
        return 0;
    }
    if (shell_read_secret("Retype new password: ", b, sizeof(b)) < 0) { memset(a, 0, (size_t)cap); return 0; }
    int same = !strcmp(a, b);
    memset(b, 0, sizeof(b));
    if (!same) { fail(cmd, "the passwords do not match"); memset(a, 0, (size_t)cap); return 0; }
    return 1;
}

/* passwd [-d] [user] */
static void cmd_passwd(int argc, char** argv) {
    int del = 0;
    const char* who = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d")) del = 1;
        else if (argv[i][0] == '-') { say("usage: passwd [-d] [user]    (-d: no password; another user's: an administrator's)"); return; }
        else who = argv[i];
    }
    uint32_t caller = shell_caller_uid();
    char me[USER_NAME_MAX];
    user_name_of(caller, me, sizeof(me));
    if (!who) who = caller == UID_ROOT ? PASSWD_USER : me;     /* (root's own: banana's, as before) */
    const user_t* u = user_by_name(who);
    if (!u) { fail("passwd", "no such user"); return; }
    int own = u->uid == caller;
    char line[96];
    ksnprintf(line, sizeof(line), "Changing the password of %s.", u->name);
    say(line);
    /* one's own: the current one first (an administrator changing another's: not asked) */
    if (own && caller != UID_ROOT && passwd_is_set(u->name)) {
        char cur[128];
        if (shell_read_secret("Current password: ", cur, sizeof(cur)) < 0) return;
        int ok = passwd_check(u->name, cur);
        memset(cur, 0, sizeof(cur));
        if (!ok) { fail("passwd", "that is not the current password"); return; }
    }
    if (del) {
        if (!strcmp(u->name, "root")) { passwd_clear("root"); say("passwd: root is locked (no login; doas is the way)"); return; }
        passwd_clear(u->name);
        say("passwd: no password now - the login and doas do not ask, SSH refuses");
        return;
    }
    char a[128];
    if (!new_password("passwd", a, sizeof(a), 0)) return;
    say("passwd: hashing...");
    terminal_flush();
    int r = passwd_set(u->name, a);
    memset(a, 0, sizeof(a));
    if (r != 0) { fail("passwd", "could not write " PASSWD_FILE); return; }
    say("passwd: password updated");
    if (!fsdisk_is_installed()) say("        (kept after reboot once Banana OS is installed: `install`)");
}

/* adduser [--admin] [--name "Full Name"] [--no-password] <user> */
static void cmd_adduser(int argc, char** argv) {
    int admin = 0, nopw = 0;
    const char* name = NULL;
    const char* full = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--admin") || !strcmp(argv[i], "-a")) admin = 1;
        else if (!strcmp(argv[i], "--no-password")) nopw = 1;
        else if ((!strcmp(argv[i], "--name") || !strcmp(argv[i], "-c")) && i + 1 < argc) full = argv[++i];
        else if (argv[i][0] == '-') { name = NULL; break; }
        else name = argv[i];
    }
    if (!name) { say("usage: adduser [--admin] [--name \"Full Name\"] [--no-password] <user>"); return; }
    char err[96];
    if (user_add(name, full, admin, err, sizeof(err)) != 0) { fail("adduser", err); return; }
    const user_t* u = user_by_name(name);
    char line[160];
    ksnprintf(line, sizeof(line), "adduser: %s (uid %u), home %s%s", name, u ? u->uid : 0, u ? u->home : "",
              admin ? " - an administrator" : "");
    say(line);
    if (!nopw) {
        char a[128];
        if (new_password("adduser", a, sizeof(a), 1) && a[0]) {
            passwd_set(name, a);
            say("adduser: password set");
        } else say("adduser: no password - logs in without one (`passwd` sets it)");
        memset(a, 0, sizeof(a));
    }
}

/* deluser [--remove-home] <user> */
static void cmd_deluser(int argc, char** argv) {
    int home = 0;
    const char* name = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--remove-home") || !strcmp(argv[i], "-r")) home = 1;
        else if (argv[i][0] != '-') name = argv[i];
    }
    if (!name) { say("usage: deluser [--remove-home] <user>"); return; }
    if (user_by_name(name) && user_by_name(name)->uid == shell_caller_uid()) { fail("deluser", "not yourself"); return; }
    char err[96];
    if (user_del(name, home, err, sizeof(err)) != 0) { fail("deluser", err); return; }
    say(home ? "deluser: removed, and the home folder" : "deluser: removed (the home folder is kept)");
}

/* usermod -aG wheel <user> | -rG wheel <user> | --admin / --no-admin <user> */
static void cmd_usermod(int argc, char** argv) {
    int admin = -1;
    const char* name = argc > 1 ? argv[argc - 1] : NULL;
    for (int i = 1; i < argc - 1; i++) {
        if (!strcmp(argv[i], "--admin")) admin = 1;
        else if (!strcmp(argv[i], "--no-admin")) admin = 0;
        else if ((!strcmp(argv[i], "-aG") || !strcmp(argv[i], "-G")) && i + 1 < argc - 1 && !strcmp(argv[i + 1], "wheel")) { admin = 1; i++; }
        else if ((!strcmp(argv[i], "-rG")) && i + 1 < argc - 1 && !strcmp(argv[i + 1], "wheel")) { admin = 0; i++; }
    }
    if (!name || admin < 0) { say("usage: usermod --admin | --no-admin | -aG wheel | -rG wheel <user>"); return; }
    char err[96];
    if (user_set_admin(name, admin, err, sizeof(err)) != 0) { fail("usermod", err); return; }
    say(admin ? "usermod: an administrator now (wheel)" : "usermod: a standard user now");
}

/* lsusers: the people, their rights, whether they have a password */
static void cmd_users(void) {
    const user_t* p[USERS_MAX];
    int n = users_people(p, USERS_MAX);
    char line[160];
    for (int i = 0; i < n; i++) {
        ksnprintf(line, sizeof(line), "%-16s uid %-5u %-14s %-12s %s%s", p[i]->name, p[i]->uid,
                  user_is_admin(p[i]->uid) ? "administrator" : "standard", passwd_is_set(p[i]->name) ? "password" : "no password",
                  p[i]->home, p[i]->uid == session_uid() ? "  (logged in)" : "");
        terminal_writeln(line);
    }
}

/* elevation [password on|off] [remember <minutes>] | elevation test: the
 * administrator prompt's settings (Settings > Users), and a try of it */
static void cmd_elevation(int argc, char** argv) {
    char line[120];
    if (argc >= 2 && !strcmp(argv[1], "test")) {
        char me[USER_NAME_MAX];
        user_name_of(shell_caller_uid(), me, sizeof(me));
        say("elevation: asking on the desktop...");
        terminal_flush();
        int ok = elevate_request(shell_caller_uid(), 0x7FFFFFFFu, "elevation test", "test the administrator prompt");
        say(ok ? "elevation: allowed" : "elevation: refused (or no desktop)");
        return;
    }
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "password")) elevate_set_gui_password(!strcmp(argv[i + 1], "on"));
        else if (!strcmp(argv[i], "remember")) { uint32_t m = 5; k_parse_u32(argv[i + 1], &m); elevate_set_remember_minutes((int)m); }
        else { say("usage: elevation [password on|off] [remember <minutes 0-60>] | elevation test"); return; }
    }
    ksnprintf(line, sizeof(line), "desktop prompt: %s; remembered for %d minute%s%s",
              elevate_gui_wants_password() ? "the password" : "Yes / No (an administrator)", elevate_remember_minutes(),
              elevate_remember_minutes() == 1 ? "" : "s", elevate_remember_minutes() ? "" : " (asked every time)");
    say(line);
}

int usercmd_dispatch(const char* line) {
    char buf[512];
    kstrlcpy(buf, line, sizeof(buf));
    char* argv[12];
    int argc = shell_split_args(buf, argv, 12);
    if (argc == 0) return 0;
    if (!strcmp(argv[0], "passwd")) { cmd_passwd(argc, argv); return 1; }
    if (!strcmp(argv[0], "adduser") || !strcmp(argv[0], "useradd")) { cmd_adduser(argc, argv); return 1; }
    if (!strcmp(argv[0], "deluser") || !strcmp(argv[0], "userdel")) { cmd_deluser(argc, argv); return 1; }
    if (!strcmp(argv[0], "usermod")) { cmd_usermod(argc, argv); return 1; }
    if (!strcmp(argv[0], "lsusers")) { cmd_users(); return 1; }
    if (!strcmp(argv[0], "elevation")) { cmd_elevation(argc, argv); return 1; }
    return 0;
}
