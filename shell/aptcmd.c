#include "aptcmd.h"
#include "../kernel/terminal.h"
#include "../kernel/kstring.h"
#include "../kernel/fs.h"
#include "../kernel/pkg.h"
#include "../kernel/repo.h"

/*
 * apt: packages from repositories (kernel/repo.c) - update, search, show,
 * install, remove, upgrade, autoremove, list, and the sources. `apt-get`
 * is the same command; `pkg` keeps installing .bpk files.
 */

static void say(const char* fmt, ...) {
    char buf[320];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    terminal_write(buf);
}

static void fail(const char* msg) {
    terminal_write_color("E: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    terminal_write_color(msg, VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    terminal_putchar('\n');
}

static void ok(const char* msg) {
    terminal_write_color(msg, VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    terminal_putchar('\n');
}

static void log_line(void* ctx, const char* line) {
    (void)ctx;
    terminal_writeln(line);
}

static int has_word_ci(const char* hay, const char* w) {
    int n = (int)strlen(w);
    for (const char* h = hay; *h; h++) if (!strncasecmp(h, w, (size_t)n)) return 1;
    return 0;
}

static void need_lists(void) {
    if (repo_never_updated()) terminal_writeln("(the package lists were never fetched - run `apt update`)");
}

static void cmd_update(void) {
    char msg[200];
    int n = repo_update(log_line, NULL, msg, sizeof(msg));
    if (n < 0) fail(msg);
    else ok(msg);
}

static void cmd_search(int argc, char** argv) {
    need_lists();
    int n = repo_count(), shown = 0;
    for (int i = 0; i < n; i++) {
        const repo_pkg_t* p = repo_at(i);
        if (p != repo_candidate(p->name)) continue;            /* one line per package */
        int all = 1;
        for (int a = 2; a < argc; a++)
            if (!has_word_ci(p->name, argv[a]) && !has_word_ci(p->title, argv[a]) && !has_word_ci(p->description, argv[a]) &&
                !has_word_ci(p->category, argv[a])) all = 0;
        if (!all) continue;
        pkg_info_t inst;
        int have = pkg_get(p->name, &inst) == 0;
        terminal_write_color(p->name, VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        say(" %s%s%s%s\n", p->version, have ? " [installed" : "", have && strcmp(inst.version, p->version) ? ", upgradable" : "",
            have ? "]" : "");
        say("  %s%s%s\n", p->title, p->description[0] ? " - " : "", p->description);
        shown++;
    }
    if (!shown) terminal_writeln("nothing found");
}

static void cmd_show(const char* name) {
    need_lists();
    const repo_pkg_t* p = repo_candidate(name);
    pkg_info_t inst;
    int have = pkg_get(name, &inst) == 0;
    if (!p && !have) { char m[96]; ksnprintf(m, sizeof(m), "no package %s", name); fail(m); return; }
    if (p) {
        static repo_source_t src[16];
        int ns = repo_sources(src, 16);
        say("Package: %s\nVersion: %s\nTitle: %s\nType: %s\n", p->name, p->version, p->title, p->type);
        if (p->category[0]) say("Category: %s\n", p->category);
        if (p->author[0]) say("Author: %s\n", p->author);
        if (p->depends[0]) say("Depends: %s\n", p->depends);
        say("Download-Size: %u KB\nArch: %s\n", (p->size + 1023) / 1024, p->arch[0] ? p->arch : "all");
        if (p->source < ns) say("Source: %s\n", src[p->source].url);
        if (p->description[0]) say("Description: %s\n", p->description);
    }
    if (have) say("Installed: %s%s\n", inst.version[0] ? inst.version : "-", repo_is_auto(name) ? " (automatically, for another package)" : "");
    else say("Installed: no\n");
}

static void cmd_list(int argc, char** argv) {
    int installed = argc >= 3 && !strcmp(argv[2], "--installed");
    int upgradable = argc >= 3 && !strcmp(argv[2], "--upgradable");
    if (installed) {
        static pkg_info_t l[64];
        int n = pkg_list(l, 64);
        for (int i = 0; i < n && i < 64; i++) {
            terminal_write_color(l[i].name, VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
            say(" %s [installed%s]\n", l[i].version, repo_is_auto(l[i].name) ? ",automatic" : "");
        }
        if (!n) terminal_writeln("nothing installed");
        return;
    }
    need_lists();
    int n = repo_count(), shown = 0;
    for (int i = 0; i < n; i++) {
        const repo_pkg_t* p = repo_at(i);
        if (p != repo_candidate(p->name)) continue;
        pkg_info_t inst;
        int have = pkg_get(p->name, &inst) == 0;
        int up = have && repo_vercmp(p->version, inst.version) > 0;
        if (upgradable && !up) continue;
        terminal_write_color(p->name, VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        if (up) say(" %s [upgradable from: %s]\n", p->version, inst.version);
        else say(" %s%s\n", p->version, have ? " [installed]" : "");
        shown++;
    }
    if (!shown) terminal_writeln(upgradable ? "everything is up to date" : "no packages known - apt update");
}

static void cmd_sources(void) {
    static repo_source_t src[16];
    int n = repo_sources(src, 16);
    for (int i = 0; i < n; i++)
        say("%d  %s  %s\n", i + 1, src[i].url, src[i].has_key ? "(signed)" : src[i].trusted ? "(trusted, not signed)" : "(no key: ignored)");
    if (!n) terminal_writeln("no sources");
    say("(%s - `apt add-repo <url> key=<key>` adds one, `edit %s` changes them)\n", REPO_SOURCES, REPO_SOURCES);
}

static void usage(void) {
    terminal_writeln("usage: apt update                       fetch the package lists");
    terminal_writeln("       apt search <words>               find packages");
    terminal_writeln("       apt show <name>                  details");
    terminal_writeln("       apt install <name|file.bpk>...   install (with what it needs) or upgrade");
    terminal_writeln("       apt reinstall <name>...          download and install it again");
    terminal_writeln("       apt remove <name>...             uninstall");
    terminal_writeln("       apt autoremove                   remove what was only needed by removed apps");
    terminal_writeln("       apt upgrade                      upgrade everything that has a newer version");
    terminal_writeln("       apt list [--installed|--upgradable]");
    terminal_writeln("       apt sources                      the repositories (" REPO_SOURCES ")");
    terminal_writeln("       apt add-repo <url> [key=<hex>|trusted]   apt remove-repo <url|number>");
    terminal_writeln("The App Store does the same with the mouse. Make a repository: tools/repo/banana-repo.");
}

void cmd_apt(int argc, char** argv) {
    const char* sub = argc >= 2 ? argv[1] : "help";
    char msg[200];
    if (!strcmp(sub, "update")) cmd_update();
    else if (!strcmp(sub, "search") && argc >= 3) cmd_search(argc, argv);
    else if ((!strcmp(sub, "show") || !strcmp(sub, "info") || !strcmp(sub, "policy")) && argc >= 3) cmd_show(argv[2]);
    else if ((!strcmp(sub, "install") || !strcmp(sub, "reinstall")) && argc >= 3) {
        for (int i = 2; i < argc; i++) {
            const char* a = argv[i];
            if (a[0] == '-') continue;                          /* -y and such: never asks anyway */
            int l = (int)strlen(a);
            if (l > 4 && !strcmp(a + l - 4, ".bpk")) {          /* a package file, like pkg install */
                char path[FS_PATH_LEN];
                if (a[0] == '/' || a[0] == '~') kstrlcpy(path, a, sizeof(path));
                else {
                    char cwd[FS_PATH_LEN];
                    fs_cwd_path(cwd, sizeof(cwd));
                    ksnprintf(path, sizeof(path), "%s%s%s", cwd, strcmp(cwd, "/") ? "/" : "", a);
                }
                if (pkg_install(path, msg, sizeof(msg)) == 0) ok(msg);
                else fail(msg);
                continue;
            }
            if (repo_install(a, !strcmp(sub, "reinstall"), log_line, NULL, msg, sizeof(msg)) == 0) ok(msg);
            else fail(msg);
        }
    } else if ((!strcmp(sub, "remove") || !strcmp(sub, "purge") || !strcmp(sub, "uninstall")) && argc >= 3) {
        for (int i = 2; i < argc; i++) {
            if (argv[i][0] == '-') continue;
            if (repo_remove(argv[i], msg, sizeof(msg)) == 0) ok(msg);
            else fail(msg);
        }
    } else if (!strcmp(sub, "autoremove")) {
        int n = repo_autoremove(log_line, NULL, msg, sizeof(msg));
        if (n < 0) fail(msg);
        else ok(msg);
    } else if (!strcmp(sub, "upgrade") || !strcmp(sub, "full-upgrade") || !strcmp(sub, "dist-upgrade")) {
        need_lists();
        int n = repo_upgrade(log_line, NULL, msg, sizeof(msg));
        if (n < 0) fail(msg);
        else ok(msg);
    } else if (!strcmp(sub, "list")) cmd_list(argc, argv);
    else if (!strcmp(sub, "sources")) cmd_sources();
    else if (!strcmp(sub, "add-repo") && argc >= 3) {
        const char* key = NULL;
        int trusted = 0;
        for (int i = 3; i < argc; i++) {
            if (!strncmp(argv[i], "key=", 4)) key = argv[i] + 4;
            else if (!strcmp(argv[i], "trusted")) trusted = 1;
        }
        if (!key && !trusted && strncmp(argv[2], "file://", 7)) {
            fail("a repository on the network needs its key: apt add-repo <url> key=<64 hex digits> (banana-repo key prints it)");
            return;
        }
        if (!key && !strncmp(argv[2], "file://", 7)) trusted = 1;
        if (repo_add_source(argv[2], key, trusted, msg, sizeof(msg)) == 0) ok(msg);
        else fail(msg);
    } else if (!strcmp(sub, "remove-repo") && argc >= 3) {
        if (repo_remove_source(argv[2], msg, sizeof(msg)) == 0) ok(msg);
        else fail(msg);
    } else usage();
}
