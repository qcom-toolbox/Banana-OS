#include "srvcmds.h"
#include "netcmds.h"
#include "../kernel/terminal.h"
#include "../kernel/kstring.h"
#include "../kernel/config.h"
#include "../kernel/fsdisk.h"
#include "../net/netconf.h"
#include "../net/httpd.h"
#include "../net/sshd.h"
#include "../kernel/passwd.h"
#include "../kernel/keyboard.h"
#include "../kernel/gui.h"
#include "../kernel/task.h"
#include "../kernel/fs.h"

#define RC_HEADER "# Banana OS services started at boot - `httpd enable`, `sshd enable`, ...\n"

static void say(const char* fmt, ...) {
    char buf[256];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    terminal_write(buf);
}

static void fail(const char* cmd, const char* msg) {
    terminal_write_color(cmd, VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    terminal_write_color(": ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    terminal_write_color(msg, VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    terminal_putchar('\n');
}

static int parse_port(const char* s, uint16_t* out) {
    uint32_t v;
    if (!s || k_parse_u32(s, &v) != (int)strlen(s) || v == 0 || v > 65535) return 0;
    *out = (uint16_t)v;
    return 1;
}

/* rc.conf: "<name>=yes" plus "<name>_port=<n>" */
static void boot_setting(const char* name, int on, uint16_t port) {
    char key[32], val[8];
    ksnprintf(key, sizeof(key), "%s_port", name);
    ksnprintf(val, sizeof(val), "%u", port);
    cfg_set(CFG_SERVICES, name, on ? "yes" : NULL, RC_HEADER);
    cfg_set(CFG_SERVICES, key, on ? val : NULL, RC_HEADER);
    if (cfg_persist()) say("(saved in %s and written to disk)\n", CFG_SERVICES);
    else if (!fsdisk_is_installed())
        say("(saved in %s - takes effect at boot once Banana OS is installed: `install`)\n", CFG_SERVICES);
}

static int boot_enabled(const char* name, uint16_t* port) {
    char v[8], key[32];
    if (!cfg_get(CFG_SERVICES, name, v, sizeof(v)) || strcmp(v, "yes") != 0) return 0;
    ksnprintf(key, sizeof(key), "%s_port", name);
    if (cfg_get(CFG_SERVICES, key, v, sizeof(v))) parse_port(v, port);
    return 1;
}

/* ── httpd ────────────────────────────────────────────────────────── */

static void cmd_httpd(int argc, char** argv) {
    const char* sub = argc >= 2 ? argv[1] : "status";
    uint16_t port = httpd_running() ? httpd_port() : 80;
    if (argc >= 3 && !parse_port(argv[2], &port)) { fail("httpd", "bad port number"); return; }
    char e[64];

    if (strcmp(sub, "status") == 0) {
        httpd_print_status();
        uint16_t bp = 80;
        say("       at boot: %s\n", boot_enabled("httpd", &bp) ? "enabled" : "disabled (`httpd enable`)");
    } else if (strcmp(sub, "start") == 0 || strcmp(sub, "enable") == 0) {
        if (httpd_start(port, e, sizeof(e)) != 0) { fail("httpd", e); return; }
        if (strcmp(sub, "enable") == 0) boot_setting("httpd", 1, port);
        httpd_print_status();
    } else if (strcmp(sub, "stop") == 0 || strcmp(sub, "disable") == 0) {
        httpd_stop();
        if (strcmp(sub, "disable") == 0) boot_setting("httpd", 0, 0);
        terminal_writeln("httpd: stopped");
    } else {
        terminal_writeln("usage: httpd [status | start [port] | stop | enable [port] | disable]");
        terminal_writeln("       serves the files in " HTTPD_ROOT " (default port 80)");
    }
}

/* ── sshd ─────────────────────────────────────────────────────────── */

static void cmd_sshd(int argc, char** argv) {
    const char* sub = argc >= 2 ? argv[1] : "status";
    uint16_t port = sshd_running() ? sshd_port() : 22;
    if (argc >= 3 && !parse_port(argv[2], &port)) { fail("sshd", "bad port number"); return; }
    char e[96];

    if (strcmp(sub, "status") == 0) {
        sshd_print_status();
        uint16_t bp = 22;
        say("      at boot: %s\n", boot_enabled("sshd", &bp) ? "enabled" : "disabled (`sshd enable`)");
        if (!passwd_is_set(PASSWD_USER)) terminal_writeln("      no password set yet: `passwd`");
    } else if (strcmp(sub, "start") == 0 || strcmp(sub, "enable") == 0) {
        if (sshd_start(port, e, sizeof(e)) != 0) { fail("sshd", e); return; }
        if (strcmp(sub, "enable") == 0) boot_setting("sshd", 1, port);
        sshd_print_status();
    } else if (strcmp(sub, "stop") == 0 || strcmp(sub, "disable") == 0) {
        sshd_stop();
        if (strcmp(sub, "disable") == 0) boot_setting("sshd", 0, 0);
        terminal_writeln("sshd: stopped (open sessions stay until they log out)");
    } else {
        terminal_writeln("usage: sshd [status | start [port] | stop | enable [port] | disable]");
        terminal_writeln("       SSH server, default port 22; log in as " PASSWD_USER " with the `passwd` password");
    }
}

/* ── passwd ───────────────────────────────────────────────────────── */

/* reads a line without echoing it; -1 if cancelled with Ctrl+C */
static int read_secret(const char* prompt, char* buf, int cap) {
    terminal_write(prompt);
    terminal_flush();
    int my_vt = terminal_vt_get_active();
    int n = 0, skip = 0;
    for (;;) {
        gui_poll();
        terminal_vt_set_active(my_vt);
        if (gui_focused_vt() != my_vt) { task_sleep_ms(10); continue; }
        char c = keyboard_try_getchar();
        if (!c) { task_sleep_ms(10); continue; }
        if (skip) { skip--; continue; }
        if (c == 27) { skip = 2; continue; }          /* arrow keys: ESC [ X */
        if (c == 3) { terminal_writeln("^C"); memset(buf, 0, (size_t)cap); return -1; }
        if (c == '\n') break;
        if (c == '\b') { if (n) n--; continue; }
        if ((unsigned char)c >= 32 && n < cap - 1) buf[n++] = c;
    }
    buf[n] = '\0';
    terminal_putchar('\n');
    return n;
}

static void cmd_passwd(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "-d") == 0) {
        passwd_clear(PASSWD_USER);
        terminal_writeln("passwd: password removed - SSH logins are refused until a new one is set");
        return;
    }
    if (argc >= 2) { terminal_writeln("usage: passwd [-d]    (sets the password of " PASSWD_USER ", used by SSH)"); return; }
    char a[128], b[128];
    say("Changing password for %s.\n", PASSWD_USER);
    if (read_secret("New password: ", a, sizeof(a)) < 0) return;
    if ((int)strlen(a) < PASSWD_MIN) {
        say("passwd: too short - at least %d characters\n", PASSWD_MIN);
        memset(a, 0, sizeof(a));
        return;
    }
    if (read_secret("Retype new password: ", b, sizeof(b)) < 0) { memset(a, 0, sizeof(a)); return; }
    int same = strcmp(a, b) == 0;
    memset(b, 0, sizeof(b));
    if (!same) { memset(a, 0, sizeof(a)); fail("passwd", "passwords do not match"); return; }
    terminal_writeln("passwd: hashing...");
    terminal_flush();
    int r = passwd_set(PASSWD_USER, a);
    memset(a, 0, sizeof(a));
    if (r != 0) { fail("passwd", "could not write " PASSWD_FILE); return; }
    terminal_writeln("passwd: password updated");
    if (!fsdisk_is_installed())
        terminal_writeln("        (kept after reboot once Banana OS is installed: `install`)");
}

/* ── dispatch / boot ──────────────────────────────────────────────── */

int srvcmd_dispatch(const char* line) {
    char buf[1024];
    kstrlcpy(buf, line, sizeof(buf));
    char* argv[8];
    int argc = shell_split_args(buf, argv, 8);
    if (argc == 0) return 0;
    if (strcmp(argv[0], "httpd") == 0)  { cmd_httpd(argc, argv); return 1; }
    if (strcmp(argv[0], "sshd") == 0)   { cmd_sshd(argc, argv); return 1; }
    if (strcmp(argv[0], "passwd") == 0) { cmd_passwd(argc, argv); return 1; }
    if (strcmp(argv[0], "files") == 0) {
        /* the desktop's file explorer, opened at a folder */
        char path[FS_PATH_LEN];
        const char* p = argc >= 2 ? argv[1] : NULL;
        if (p && p[0] != '/' && p[0] != '~') {
            char cwd[FS_PATH_LEN];
            fs_cwd_path(cwd, sizeof(cwd));
            ksnprintf(path, sizeof(path), "%s%s%s", cwd, strcmp(cwd, "/") ? "/" : "", p);
            p = path;
        } else if (p && p[0] == '~') {
            ksnprintf(path, sizeof(path), "/home/banana%s", p + 1);
            p = path;
        }
        if (p && fs_find_dir(p) < 0) { fail("files", "no such folder"); return 1; }
        if (!gui_open_files(p)) terminal_writeln("files: the file explorer is part of the desktop - run `startx` first");
        return 1;
    }
    return 0;
}

void services_boot(void) {
    netconf_boot();
    uint16_t port = 80;
    char e[96];
    if (boot_enabled("httpd", &port) && httpd_start(port, e, sizeof(e)) != 0)
        say("httpd: could not start at boot: %s\n", e);
    port = 22;
    if (boot_enabled("sshd", &port) && sshd_start(port, e, sizeof(e)) != 0)
        say("sshd: could not start at boot: %s\n", e);
}
