/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The system's own commands as programs: startx, install, pkg, ifconfig,
 * theme, selftest... - each /bin/<name> is a link to this program, which
 * hands its name and arguments to the system (banana_api_t's kcmd, API 30).
 * The command runs there as it did in the old built-in shell, with this
 * program's input and output: a terminal gets its colours and cursor
 * moves as escapes, a pipe or a file the plain text.
 *   kcmd COMMAND [ARGS]       (the same, named) */
#include <banana.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <pwd.h>
#include <grp.h>

/* ── users (API 35): doas, su, and the system's commands that need root ── */

static int has_users(void) { return __banana->version >= 35 && BANANA_HAS(__banana, auth); }

/* a password from the terminal, not echoed; -1 if there is none to ask on */
static int read_password(const char* prompt, char* buf, int cap) {
    int fd = open("/dev/tty", O_RDWR);
    if (fd < 0) fd = isatty(0) ? 0 : -1;
    if (fd < 0) return -1;
    struct termios old, quiet;
    int tty = tcgetattr(fd, &old) == 0;
    if (tty) {
        quiet = old;
        quiet.c_lflag &= ~(tcflag_t)ECHO;
        quiet.c_lflag |= ICANON;
        tcsetattr(fd, TCSANOW, &quiet);
    }
    if (write(fd, prompt, strlen(prompt)) < 0) {}
    int n = 0;
    char c;
    while (n < cap - 1 && read(fd, &c, 1) == 1 && c != '\n' && c != '\r') {
        if (c == 3) { n = -1; break; }                          /* ^C */
        if ((c == 127 || c == 8)) { if (n > 0) n--; continue; }
        buf[n++] = c;
    }
    if (tty) tcsetattr(fd, TCSANOW, &old);
    if (write(fd, "\n", 1) < 0) {}
    if (fd != 0) close(fd);
    if (n < 0) { memset(buf, 0, (size_t)cap); return -1; }
    buf[n] = 0;
    return n;
}

static const char* my_name(void) {
    struct passwd* pw = getpwuid(getuid());
    return pw ? pw->pw_name : "you";
}

/* be target (NULL: root) for cmd, as doas does: asked for the password if
 * needed (three tries). 0, or the exit status to give */
static int become(const char* target, const char* cmd, int never_ask, const char* tool) {
    int r = __banana->auth(BANANA_AUTH_DOAS_CHECK, target, NULL, cmd);
    if (r == -2) { fprintf(stderr, "%s: no such user: %s\n", tool, target); return 1; }
    if (r < 0) { fprintf(stderr, "%s: not permitted - %s is not an administrator (/etc/doas.conf)\n", tool, my_name()); return 1; }
    if (r == 0) return __banana->auth(BANANA_AUTH_DOAS, target, NULL, cmd) == 0 ? 0 : 1;
    if (never_ask) { fprintf(stderr, "%s: authorization required\n", tool); return 1; }
    char host[64] = "banana-os", prompt[160], pw[128];
    gethostname(host, sizeof(host));
    snprintf(prompt, sizeof(prompt), "%s (%s@%s) password: ", tool, my_name(), host);
    for (int tries = 0; tries < 3; tries++) {
        if (read_password(prompt, pw, sizeof(pw)) < 0) { fprintf(stderr, "%s: no terminal to ask the password on\n", tool); return 1; }
        r = __banana->auth(BANANA_AUTH_DOAS, target, pw, cmd);
        memset(pw, 0, sizeof(pw));
        if (r == 0) return 0;
        if (r != -13) break;
        fprintf(stderr, "%s: authentication failed\n", tool);
    }
    return 1;
}

/* the environment of the user the process is now (doas, su -) */
static void user_env(int login) {
    struct passwd* pw = getpwuid(geteuid());
    if (!pw) return;
    setenv("HOME", pw->pw_dir, 1);
    setenv("USER", pw->pw_name, 1);
    setenv("LOGNAME", pw->pw_name, 1);
    setenv("SHELL", pw->pw_shell && pw->pw_shell[0] ? pw->pw_shell : "/bin/bash", 1);
    if (login && chdir(pw->pw_dir) != 0) { if (chdir("/") != 0) {} }
}

static int doas_main(int argc, char** argv) {
    if (!has_users()) { fprintf(stderr, "doas: this system has no users to act as (API 35)\n"); return 1; }
    const char* target = NULL;
    int never = 0, shell = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char* o = argv[i] + 1; *o; o++) {
            if (*o == 'n') never = 1;
            else if (*o == 's') shell = 1;
            else if (*o == 'L') { __banana->auth(BANANA_AUTH_FORGET, NULL, NULL, NULL); return 0; }
            else if (*o == 'u' && i + 1 < argc) { target = argv[++i]; break; }
            else { fprintf(stderr, "usage: doas [-Lns] [-u user] command [args]\n"); return 1; }
        }
    }
    if (i >= argc && !shell) { fprintf(stderr, "usage: doas [-Lns] [-u user] command [args]\n"); return 1; }
    char sh[128] = "/bin/bash";
    const char* cmd = shell ? sh : argv[i];
    int r = become(target, cmd, never, "doas");
    if (r) return r;
    char* who = getenv("USER");
    if (who) setenv("DOAS_USER", who, 1);
    user_env(0);
    if (shell) {
        snprintf(sh, sizeof(sh), "%s", getenv("SHELL") ? getenv("SHELL") : "/bin/bash");
        char* a[] = { sh, NULL };
        execv(sh, a);
        perror("doas: the shell");
        return 127;
    }
    execvp(argv[i], argv + i);
    fprintf(stderr, "doas: %s: command not found\n", argv[i]);
    return 127;
}

static int su_main(int argc, char** argv) {
    if (!has_users()) { fprintf(stderr, "su: this system has no users to become (API 35)\n"); return 1; }
    int login = 0;
    const char* cmd = NULL;
    const char* target = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-") || !strcmp(argv[i], "-l") || !strcmp(argv[i], "--login")) login = 1;
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) cmd = argv[++i];
        else if (argv[i][0] == '-') { fprintf(stderr, "usage: su [-] [-c command] [user]\n"); return 1; }
        else target = argv[i];
    }
    struct passwd* tp = getpwnam(target ? target : "root");
    if (!tp) { fprintf(stderr, "su: no such user: %s\n", target); return 1; }
    int r = __banana->auth(BANANA_AUTH_SU, target, NULL, NULL);
    if (r == -13) {                                           /* (its password: asked) */
        char pw[128];
        if (read_password("Password: ", pw, sizeof(pw)) < 0) { fprintf(stderr, "su: no terminal to ask the password on\n"); return 1; }
        r = __banana->auth(BANANA_AUTH_SU, target, pw, NULL);
        memset(pw, 0, sizeof(pw));
        if (r == -13) { fprintf(stderr, "su: authentication failure\n"); return 1; }
    }
    if (r == -1) {
        fprintf(stderr, target ? "su: %s cannot log in\n" : "su: root has no password - use doas (doas su -)\n", target);
        return 1;
    }
    if (r) { fprintf(stderr, "su: authentication failure\n"); return 1; }
    user_env(login);
    static char shell[128];
    snprintf(shell, sizeof(shell), "%s", tp->pw_shell && tp->pw_shell[0] && access(tp->pw_shell, X_OK) == 0 ? tp->pw_shell : "/bin/bash");
    char dash[130];
    snprintf(dash, sizeof(dash), "-%s", strrchr(shell, '/') ? strrchr(shell, '/') + 1 : shell);
    char* a[4] = { login ? dash : shell, NULL, NULL, NULL };
    if (cmd) { a[1] = "-c"; a[2] = (char*)cmd; }
    execv(shell, a);
    perror("su: the shell");
    return 127;
}

int main(int argc, char** argv) {
    const char* me = strrchr(argv[0], '/');
    me = me ? me + 1 : argv[0];
    int first = 0;
    if (!strcmp(me, "kcmd") || !strncmp(me, "app-", 4)) {
        if (argc < 2) { fprintf(stderr, "usage: kcmd COMMAND [ARGS...]  (a command of the system's)\n"); return 2; }
        me = argv[1];
        first = 1;
    }
    if (!strcmp(me, "doas")) return doas_main(argc - first, argv + first);
    if (!strcmp(me, "su")) return su_main(argc - first, argv + first);
    if (!BANANA_HAS(__banana, kcmd)) { fprintf(stderr, "%s: this system cannot run its commands for programs (API 30)\n", me); return 126; }
    /* The line codec below uses quotes as syntax. A hostname cannot
     * contain literal quotes: reject them before that codec can drop them. */
    if (!strcmp(me, "hostname") && argc > first + 1 &&
        strpbrk(argv[first + 1], "'\"")) {
        fprintf(stderr, "hostname: use only letters, digits or hyphens\n");
        return 1;
    }
    /* the line: the name, then each argument - quoted when it has spaces
     * (the system's commands take "a b" as one word) */
    static char line[4096];
    size_t n = (size_t)snprintf(line, sizeof line, "%s", me);
    for (int i = first + 1; i < argc && n < sizeof line - 4; i++) {
        const char* a = argv[i];
        int q = !*a || strpbrk(a, " \t") != NULL;
        char qc = strchr(a, '"') ? '\'' : '"';
        if (q) n += (size_t)snprintf(line + n, sizeof line - n, " %c%s%c", qc, a, qc);
        else n += (size_t)snprintf(line + n, sizeof line - n, " %s", a);
    }
    if (n >= sizeof line) { fprintf(stderr, "%s: the line is too long\n", me); return 2; }
    /* a command that changes the system: root's - the password first, as doas (once in a while) */
    if (has_users() && __banana->auth(BANANA_AUTH_KCMD, NULL, NULL, line) == 1) {
        int r = become(NULL, me, 0, me);
        if (r) return r == 1 ? 126 : r;
    }
    fflush(stdout);
    /* a terminal: keys one by one, unechoed, ^C a key - the command reads
     * them itself (top, edit, a yes / no question) */
    struct termios old, raw;
    int tty = isatty(0) && tcgetattr(0, &old) == 0;
    if (tty) {
        raw = old;
        raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG | IEXTEN);
        raw.c_iflag &= ~(tcflag_t)(IXON);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &raw);
    }
    int rc = __banana->kcmd(line);
    if (tty) tcsetattr(0, TCSANOW, &old);
    if (rc == 127) fprintf(stderr, "%s: not a command of this system\n", me);
    return rc < 0 ? 1 : rc;
}
