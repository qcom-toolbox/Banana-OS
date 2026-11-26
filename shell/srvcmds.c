/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../net/firewall.h"
#include "../net/ipv6.h"
#include "../kernel/sysinfo.h"
#include "../kernel/utf8.h"
#include "../kernel/settings.h"
#include "../net/upnp.h"
#include "srvcmds.h"
#include "../kernel/users.h"
#include "../net/wifi.h"
#include "netcmds.h"
#include "../kernel/terminal.h"
#include "../kernel/kstring.h"
#include "../kernel/config.h"
#include "../kernel/fsdisk.h"
#include "../net/netconf.h"
#include "../net/httpd.h"
#include "../net/sshd.h"
#include "../net/rdpd.h"
#include "../kernel/rdpclient.h"
#include "../kernel/passwd.h"
#include "../kernel/keyboard.h"
#include "../kernel/gui.h"
#include "../kernel/browser.h"
#include "../kernel/task.h"
#include "../kernel/fs.h"

#define RC_HEADER "# Banana OS services started at boot - `httpd enable`, `sshd enable`, ...\n"

static void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
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

static uint16_t g_exposed;     /* Internet port the router forwards to httpd (httpd expose), 0 = none */

static void cmd_httpd(int argc, char** argv) {
    const char* sub = argc >= 2 ? argv[1] : "status";
    uint16_t port = httpd_running() ? httpd_port() : 80;
    if (argc >= 3 && !parse_port(argv[2], &port)) { fail("httpd", "bad port number"); return; }
    char e[64];

    if (strcmp(sub, "status") == 0) {
        httpd_print_status();
        uint16_t bp = 80;
        say("       at boot: %s\n", boot_enabled("httpd", &bp) ? "enabled" : "disabled (`httpd enable`)");
        if (g_exposed) say("       exposed to the Internet on port %u (router UPnP)\n", (unsigned)g_exposed);
    } else if (strcmp(sub, "start") == 0 || strcmp(sub, "enable") == 0) {
        if (httpd_start(port, e, sizeof(e)) != 0) { fail("httpd", e); return; }
        if (strcmp(sub, "enable") == 0) boot_setting("httpd", 1, port);
        httpd_print_status();
    } else if (strcmp(sub, "stop") == 0 || strcmp(sub, "disable") == 0) {
        httpd_stop();
        if (strcmp(sub, "disable") == 0) boot_setting("httpd", 0, 0);
        terminal_writeln("httpd: stopped");
    } else if (strcmp(sub, "expose") == 0) {
        /* httpd expose [internet port]: the router forwards that port to us */
        uint16_t ext = 80;
        if (argc >= 3 && !parse_port(argv[2], &ext)) { fail("httpd", "bad port number"); return; }
        if (!httpd_running()) {
            if (httpd_start(80, e, sizeof(e)) != 0) { fail("httpd", e); return; }
            terminal_writeln("httpd: started on port 80");
        }
        terminal_writeln("httpd: asking the router (UPnP) to forward the port...");
        char pub[48], err[200];
        if (upnp_add_port(ext, httpd_port(), "Banana OS httpd", pub, sizeof(pub), err, sizeof(err)) != 0) {
            fail("httpd", err);
            char me[16];
            ip4_to_str(net_if()->ip, me);
            say("       You can also forward TCP port %u by hand in the router's settings, to %s port %u.\n",
                (unsigned)ext, me, (unsigned)httpd_port());
            return;
        }
        g_exposed = ext;
        ip4_t pip = 0;
        if (pub[0] && str_to_ip4(pub, &pip) && !upnp_is_private(pip)) {
            if (ext == 80) say("httpd: exposed - from the Internet: http://%s/\n", pub);
            else say("httpd: exposed - from the Internet: http://%s:%u/\n", pub, (unsigned)ext);
        } else {
            say("httpd: the router forwards port %u to this machine, but its own address (%s) is not\n"
                "       a public one: your provider has another NAT in front of it (CGNAT), so the\n"
                "       Internet still can't reach it. Ask the provider for a public IPv4 address.\n",
                ext, pub[0] ? pub : "unknown");
        }
        terminal_writeln("       Anyone can now read " HTTPD_ROOT " and run its .php pages. Undo: httpd unexpose");
    } else if (strcmp(sub, "unexpose") == 0) {
        uint16_t ext = g_exposed ? g_exposed : 80;
        if (argc >= 3 && !parse_port(argv[2], &ext)) { fail("httpd", "bad port number"); return; }
        char err[200];
        if (upnp_delete_port(ext, err, sizeof(err)) != 0) { fail("httpd", err); return; }
        g_exposed = 0;
        say("httpd: the router no longer forwards port %u\n", ext);
    } else {
        terminal_writeln("usage: httpd [status | start [port] | stop | enable [port] | disable |");
        terminal_writeln("              expose [internet port] | unexpose]");
        terminal_writeln("       serves the files in " HTTPD_ROOT " (default port 80); expose asks the");
        terminal_writeln("       router (UPnP) to make it reachable from the Internet");
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
        if (!users_any_password()) terminal_writeln("      nobody has a password yet: `passwd`");
    } else if (strcmp(sub, "start") == 0 || strcmp(sub, "enable") == 0) {
        if (sshd_start(port, e, sizeof(e)) != 0) { fail("sshd", e); return; }
        if (fw_enabled() && !fw_allowed(FW_TCP, (uint16_t)port)) say("note: the firewall keeps port %d closed - `firewall allow tcp %d` opens it\n", port, port);
        if (strcmp(sub, "enable") == 0) boot_setting("sshd", 1, port);
        sshd_print_status();
    } else if (strcmp(sub, "stop") == 0 || strcmp(sub, "disable") == 0) {
        sshd_stop();
        if (strcmp(sub, "disable") == 0) boot_setting("sshd", 0, 0);
        terminal_writeln("sshd: stopped (open sessions stay until they log out)");
    } else {
        terminal_writeln("usage: sshd [status | start [port] | stop | enable [port] | disable]");
        terminal_writeln("       SSH server, default port 22; any user with a password logs in (as that user)");
    }
}

/* ── rdpd ─────────────────────────────────────────────────────────── */

static void cmd_rdpd(int argc, char** argv) {
    const char* sub = argc >= 2 ? argv[1] : "status";
    uint16_t port = rdpd_running() ? rdpd_port() : 3389;
    if (argc >= 3 && !parse_port(argv[2], &port)) { fail("rdpd", "bad port number"); return; }
    char e[96];

    if (strcmp(sub, "status") == 0) {
        rdpd_print_status();
        uint16_t bp = 3389;
        say("      at boot: %s\n", boot_enabled("rdpd", &bp) ? "enabled" : "disabled (`rdpd enable`)");
        if (!passwd_is_set(PASSWD_USER)) terminal_writeln("      no password set yet: `passwd`");
    } else if (strcmp(sub, "start") == 0 || strcmp(sub, "enable") == 0) {
        terminal_writeln("rdpd: starting (the first start generates an RSA key - a moment)...");
        terminal_flush();
        if (rdpd_start(port, e, sizeof(e)) != 0) { fail("rdpd", e); return; }
        if (fw_enabled() && !fw_allowed(FW_TCP, (uint16_t)port)) say("note: the firewall keeps port %d closed - `firewall allow tcp %d` opens it\n", port, port);
        if (strcmp(sub, "enable") == 0) boot_setting("rdpd", 1, port);
        rdpd_print_status();
    } else if (strcmp(sub, "stop") == 0 || strcmp(sub, "disable") == 0) {
        rdpd_stop();
        if (strcmp(sub, "disable") == 0) boot_setting("rdpd", 0, 0);
        terminal_writeln("rdpd: stopped");
    } else {
        terminal_writeln("usage: rdpd [status | start [port] | stop | enable [port] | disable]");
        terminal_writeln("       Remote Desktop server, default port 3389; log in as " PASSWD_USER " with the");
        terminal_writeln("       `passwd` password (mstsc, FreeRDP, the Remote Desktop apps). TLS, no NLA.");
    }
}

/* ── passwd ───────────────────────────────────────────────────────── */

/* reads a line without echoing it; -1 if cancelled with Ctrl+C */
int shell_read_secret(const char* prompt, char* buf, int cap) {
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
        if (c == '\b') { if (n) n = u8_prev(buf, n); continue; }
        if ((unsigned char)c >= 32 && n < cap - 1) buf[n++] = c;
    }
    buf[n] = '\0';
    terminal_putchar('\n');
    return n;
}

/* ── rdp (the client) ─────────────────────────────────────────────── */

static void cmd_rdp(int argc, char** argv) {
    rdp_params_t p;
    memset(&p, 0, sizeof(p));
    const char* host = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) kstrlcpy(p.user, argv[++i], sizeof(p.user));
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) kstrlcpy(p.domain, argv[++i], sizeof(p.domain));
        else if (argv[i][0] != '-') host = argv[i];
    }
    if (!host) {
        terminal_writeln("usage: rdp [-u user] [-d domain] host[:port]");
        terminal_writeln("       connect to a Remote Desktop server (default port 3389). It opens a");
        terminal_writeln("       window on the desktop - run `startx` first. NLA is used if a password");
        terminal_writeln("       is given. The password is asked next, and not shown.");
        return;
    }
    /* split host[:port] */
    char hb[128];
    kstrlcpy(hb, host, sizeof(hb));
    p.port = 3389;
    for (int i = 0; hb[i]; i++)
        if (hb[i] == ':') { hb[i] = 0; uint32_t v; if (k_parse_u32(hb + i + 1, &v) && v > 0 && v < 65536) p.port = (uint16_t)v; break; }
    kstrlcpy(p.host, hb, sizeof(p.host));
    if (!p.user[0]) kstrlcpy(p.user, PASSWD_USER, sizeof(p.user));

    char pw[128];
    int n = shell_read_secret("Password (empty for none): ", pw, sizeof(pw));
    if (n < 0) return;
    kstrlcpy(p.password, pw, sizeof(p.password));
    memset(pw, 0, sizeof(pw));
    p.nla = p.password[0] ? 1 : 0;

    say("rdp: connecting to %s:%u as %s...\n", p.host, p.port, p.user);
    if (rdpclient_launch(&p) != 0)
        fail("rdp", "could not start (is the desktop running? `startx`)");
    memset(&p, 0, sizeof(p));
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
    if (strcmp(argv[0], "rdpd") == 0)   { cmd_rdpd(argc, argv); return 1; }
    if (strcmp(argv[0], "rdp") == 0)    { cmd_rdp(argc, argv); return 1; }
    if (strcmp(argv[0], "notepad") == 0) {
        /* the desktop's text editor, optionally with a file (relative: to the current folder) */
        char path[FS_PATH_LEN];
        const char* p = argc >= 2 ? argv[1] : NULL;
        if (p && p[0] != '/' && p[0] != '~') {
            char cwd[FS_PATH_LEN];
            fs_cwd_path(cwd, sizeof(cwd));
            ksnprintf(path, sizeof(path), "%s%s%s", cwd, strcmp(cwd, "/") ? "/" : "", p);
            p = path;
        }
        if (!gui_open_notepad(p)) terminal_writeln("notepad: Notepad is part of the desktop - run `startx` first (or use `edit`)");
        return 1;
    }
    if (strcmp(argv[0], "installer") == 0) {
        /* the live CD's graphical installer */
        if (!sysinfo_live_boot()) terminal_writeln("installer: only on the live CD (this system is already installed - see 'update')");
        else if (!gui_open_installer()) terminal_writeln("installer: it is part of the desktop - run `startx` first (or use `install`)");
        return 1;
    }
    if (strcmp(argv[0], "browser") == 0) {
        /* the desktop's web browser, optionally at an address */
        int index=1;
        if(argc==2&&!strcmp(argv[1],"--engine")){
            terminal_writeln(browser_uses_libjs()?"browser engine: LibJS":"browser engine: BananaScript (legacy)");
            return 1;
        }
        if(argc>1&&(!strcmp(argv[1],"--libjs")||!strcmp(argv[1],"--default")||!strcmp(argv[1],"--legacy"))){
#ifdef __x86_64__
            int enabled=strcmp(argv[1],"--legacy")!=0;
#else
            int enabled=!strcmp(argv[1],"--libjs");
#endif
            if(browser_use_libjs(enabled)<0){
                terminal_writeln("browser: the integrated LibJS renderer is unavailable (requires 64-bit Banana OS)");return 1;
            }
            index++;
        }
        if(argc>index+1){terminal_writeln("usage: browser [--libjs|--default|--legacy] [address] | browser --engine");return 1;}
        if (!gui_open_browser(argc>index?argv[index]:NULL))
            terminal_writeln("browser: the web browser is part of the desktop - run `startx` first");
        return 1;
    }
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
            ksnprintf(path, sizeof(path), "%s%s", fs_home_path(), p + 1);
            p = path;
        }
        if (p && fs_find_dir(p) < 0) { fail("files", "no such folder"); return 1; }
        if (!gui_open_files(p)) terminal_writeln("files: the file explorer is part of the desktop - run `startx` first");
        return 1;
    }
    return 0;
}

void services_boot(void) {
    wifi_boot_files();
    netconf_boot();
    settings_boot();                    /* volume, keyboard layout */
    ipv6_boot();                        /* IPv6 only if it was turned on */
    fw_boot();                          /* the firewall, if it was turned on */
    uint16_t port = 80;
    char e[96];
    if (boot_enabled("httpd", &port) && httpd_start(port, e, sizeof(e)) != 0)
        say("httpd: could not start at boot: %s\n", e);
    port = 22;
    if (boot_enabled("sshd", &port) && sshd_start(port, e, sizeof(e)) != 0)
        say("sshd: could not start at boot: %s\n", e);
    port = 3389;
    if (boot_enabled("rdpd", &port) && rdpd_start(port, e, sizeof(e)) != 0)
        say("rdpd: could not start at boot: %s\n", e);
}

/* Settings > Startup: the desktop instead of the text console (after the login) */
int services_desktop_at_boot(void) {
    char d[8];
    return cfg_get(CFG_SERVICES, "desktop", d, sizeof(d)) && strcmp(d, "yes") == 0;
}
