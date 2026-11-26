/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "themecmd.h"
#include "../kernel/i18n.h"
#include "../kernel/terminal.h"
#include "../kernel/kstring.h"
#include "../kernel/theme.h"
#include "../kernel/fs.h"

/* theme [list | set NAME | show [NAME] | install FILE | new NAME [FILE] | check FILE] */

static void say(const char* s) { terminal_writeln(s); }

/* a path as typed: absolute, ~/..., or from the current folder */
static void resolve(const char* in, char* out, int cap) {
    if (in[0] == '/') kstrlcpy(out, in, (size_t)cap);
    else if (in[0] == '~') ksnprintf(out, (size_t)cap, "%s%s", fs_home_path(), in + 1);
    else {
        char cwd[FS_PATH_LEN];
        fs_cwd_path(cwd, sizeof(cwd));
        ksnprintf(out, (size_t)cap, "%s%s%s", cwd, cwd[1] ? "/" : "", in);
    }
}

static void show(const theme_t* t) {
    static const char* const K[TH_SHADES] = { "shadow", "base", "surface", "raised", "line", "text_dim", "text", "bright" };
    char line[96];
    ksnprintf(line, sizeof(line), "%s - %s theme%s%s%s", t->name, t->light ? "a light" : "a dark",
              t->author[0] ? ", by " : "", t->author, t->path[0] ? "" : " (built in)");
    say(line);
    for (int k = 0; k < TH_SHADES; k++) {
        ksnprintf(line, sizeof(line), "  %-9s #%06X", K[k], t->shade[k]);
        say(line);
    }
    ksnprintf(line, sizeof(line), "  %-9s #%06X", "accent", t->accent);
    say(line);
    ksnprintf(line, sizeof(line), "  %-9s %s", "taskmgr", TM_STYLE_KEYS[t->taskmgr]);
    say(line);
    if (t->wallpaper[0]) { ksnprintf(line, sizeof(line), "  wallpaper %s", t->wallpaper); say(line); }
}

static int lum(uint32_t c) { return (int)((((c >> 16) & 255) * 299 + ((c >> 8) & 255) * 587 + (c & 255) * 114) / 1000); }

/* how readable its text is: the WCAG-like gap of lightness between two shades */
static void contrast(const char* what, uint32_t a, uint32_t b, int min) {
    int d = lum(a) - lum(b);
    if (d < 0) d = -d;
    if (d < min) {
        char line[96];
        ksnprintf(line, sizeof(line), "  warning: %s are close (lightness %d apart, %d+ reads well)", what, d, min);
        say(line);
    }
}

/* language [en | fr | de | it | rm]: the desktop's */
static int language_cmd(int argc, char** argv) {
    char line[96];
    if (argc < 2) {
        ksnprintf(line, sizeof(line), "Language: %s (%s)", i18n_name(i18n_current()), i18n_lang());
        say(line);
        for (int i = 0; i < i18n_count(); i++) {
            ksnprintf(line, sizeof(line), "%c %s  %s", i == i18n_current() ? '*' : ' ', i18n_code(i), i18n_name(i));
            say(line);
        }
        return 1;
    }
    for (int i = 0; i < i18n_count(); i++)
        if (!strcmp(argv[1], i18n_code(i)) || !strcasecmp(argv[1], i18n_name(i))) {
            i18n_set(i18n_code(i));
            ksnprintf(line, sizeof(line), "Language: %s", i18n_name(i));
            say(line);
            return 1;
        }
    say("usage: language [en | fr | de | it | rm]");
    return 1;
}

int themecmd_dispatch(int argc, char** argv) {
    if (argc >= 1 && !strcmp(argv[0], "language")) return language_cmd(argc, argv);
    if (argc < 1 || strcmp(argv[0], "theme")) return 0;
    if (!theme_count()) theme_init();
    const char* sub = argc > 1 ? argv[1] : "";
    char line[128], err[96];
    if (!sub[0] || !strcmp(sub, "list")) {
        for (int i = 0; i < theme_count(); i++) {
            const theme_t* t = theme_get(i);
            ksnprintf(line, sizeof(line), "%c %-16s %-5s %s", i == theme_current() ? '*' : ' ', t->name, t->light ? "light" : "dark",
                      t->path[0] ? t->path : "built in");
            say(line);
        }
        if (!sub[0]) say("theme set NAME | show NAME | install FILE | new NAME | check FILE   (help theme)");
        return 1;
    }
    if (!strcmp(sub, "set") || !strcmp(sub, "use")) {
        if (argc < 3) { say("usage: theme set NAME"); return 1; }
        char name[40] = "";
        for (int a = 2; a < argc; a++) { if (a > 2) kstrlcat(name, " ", sizeof(name)); kstrlcat(name, argv[a], sizeof(name)); }
        int i = theme_find(name);
        if (i < 0) { ksnprintf(line, sizeof(line), "theme: no theme '%s' (theme list)", name); say(line); return 1; }
        theme_apply(i);
        ksnprintf(line, sizeof(line), "Theme: %s", theme_get(i)->name);
        say(line);
        return 1;
    }
    if (!strcmp(sub, "taskmgr")) {
        if (argc < 3) {
            int ov = theme_taskmgr_override();
            ksnprintf(line, sizeof(line), "Task Manager: %s (%s)", TM_STYLE_NAMES[theme_taskmgr_style()], ov < 0 ? "the theme's" : "chosen");
            say(line);
            say("theme taskmgr banana | classic | windows10 | windows11 | theme");
            return 1;
        }
        if (!strcmp(argv[2], "theme") || !strcmp(argv[2], "auto")) { theme_set_taskmgr_override(-1); say("Task Manager: the theme's look"); return 1; }
        int st = theme_taskmgr_style_find(argv[2]);
        if (st < 0) { say("theme taskmgr banana | classic | windows10 | windows11 | theme"); return 1; }
        theme_set_taskmgr_override(st);
        ksnprintf(line, sizeof(line), "Task Manager: %s", TM_STYLE_NAMES[st]);
        say(line);
        return 1;
    }
    if (!strcmp(sub, "show")) {
        int i = theme_current();
        if (argc > 2) {
            char name[40] = "";
            for (int a = 2; a < argc; a++) { if (a > 2) kstrlcat(name, " ", sizeof(name)); kstrlcat(name, argv[a], sizeof(name)); }
            i = theme_find(name);
            if (i < 0) { say("theme: no such theme (theme list)"); return 1; }
        }
        show(theme_get(i));
        return 1;
    }
    if (!strcmp(sub, "install")) {
        if (argc < 3) { say("usage: theme install FILE.theme | FILE.btheme"); return 1; }
        char path[FS_PATH_LEN];
        resolve(argv[2], path, sizeof(path));
        int i = theme_install(path, err, sizeof(err));
        if (i < 0) { ksnprintf(line, sizeof(line), "theme: %s", err); say(line); return 1; }
        ksnprintf(line, sizeof(line), "Installed %s - theme set %s to use it", theme_get(i)->name, theme_get(i)->name);
        say(line);
        return 1;
    }
    if (!strcmp(sub, "new")) {
        if (argc < 3) { say("usage: theme new NAME [FILE]   (a copy of the current theme to change)"); return 1; }
        char path[FS_PATH_LEN], file[FS_PATH_LEN];
        if (argc > 3) kstrlcpy(file, argv[3], sizeof(file));
        else ksnprintf(file, sizeof(file), "%s.theme", argv[2]);
        resolve(file, path, sizeof(path));
        if (theme_export(theme_current(), argv[2], path) < 0) { say("theme: cannot write the file"); return 1; }
        ksnprintf(line, sizeof(line), "Wrote %s - edit its colours, then: theme install %s", path, file);
        say(line);
        return 1;
    }
    if (!strcmp(sub, "check")) {
        if (argc < 3) { say("usage: theme check FILE.theme"); return 1; }
        char path[FS_PATH_LEN];
        resolve(argv[2], path, sizeof(path));
        int fi = fs_find_file(path);
        fs_file_t* f = fi >= 0 ? fs_get_file(fi) : NULL;
        if (!f || !f->content) { ksnprintf(line, sizeof(line), "theme: cannot read %s", path); say(line); return 1; }
        theme_t t;
        if (theme_parse(f->content, f->size, &t, err, sizeof(err)) < 0) { ksnprintf(line, sizeof(line), "%s: %s", path, err); say(line); return 1; }
        ksnprintf(line, sizeof(line), "%s: good - %s", path, t.name);
        say(line);
        contrast("text and base", t.shade[TH_TEXT], t.shade[TH_BASE], 120);
        contrast("text and surface", t.shade[TH_TEXT], t.shade[TH_SURFACE], 110);
        contrast("text_dim and surface", t.shade[TH_DIM], t.shade[TH_SURFACE], 60);
        contrast("raised and surface", t.shade[TH_RAISED], t.shade[TH_SURFACE], 6);
        return 1;
    }
    say("usage: theme [list | set NAME | show [NAME] | install FILE | new NAME [FILE] | check FILE | taskmgr STYLE]");
    return 1;
}
