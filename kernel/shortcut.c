/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "shortcut.h"
#include "fileops.h"
#include "kheap.h"
#include "kstring.h"
#include "pkg.h"

/* the programs a shortcut can open by name (besides the installed apps) */
static const char* const BUILTIN[] = { "Terminal", "Files", "Browser", "Notepad", "Apps", "Task Manager", "Settings",
                                       "Device Manager", "Disk Management" };
#define NBUILTIN ((int)(sizeof(BUILTIN) / sizeof(BUILTIN[0])))

int shortcut_is(const char* name) {
    size_t n = strlen(name), e = sizeof(SHORTCUT_EXT) - 1;
    return n > e && !strcasecmp(name + n - e, SHORTCUT_EXT);
}

int shortcut_kind(const char* t) {
    if (!t || !t[0]) return SC_NONE;
    if (!strncasecmp(t, "app:", 4)) return SC_APP;
    if (!strncasecmp(t, "http://", 7) || !strncasecmp(t, "https://", 8)) return SC_WEB;
    if (t[0] == '/') return fs_find_dir(t) >= 0 ? SC_DIR : SC_FILE;
    return SC_NONE;
}

int shortcut_read(const char* path, char* target, int cap) {
    target[0] = 0;
    int idx = fs_find_file(path);
    fs_file_t* f = idx >= 0 ? fs_file_info(idx) : NULL;
    if (!f || !f->size || f->size > 8192) return SC_NONE;
    char* b = (char*)kmalloc(f->size + 1);
    if (!b) return SC_NONE;
    int n = fs_read(idx, 0, b, f->size);
    b[n > 0 ? n : 0] = 0;
    for (char* l = b; l && *l; ) {
        char* e = strchr(l, '\n');
        if (e) *e = 0;
        int ln = (int)strlen(l);
        if (ln && l[ln - 1] == '\r') l[--ln] = 0;
        if (!strncasecmp(l, "Target=", 7)) { kstrlcpy(target, l + 7, (size_t)cap); break; }
        l = e ? e + 1 : NULL;
    }
    kfree(b);
    return shortcut_kind(target);
}

/* "example.com", "www.x.org/page": a web address without its scheme */
static int looks_web(const char* s) {
    const char* dot = strchr(s, '.');
    if (!dot || dot == s || strchr(s, ' ') || s[0] == '/') return 0;
    const char* end = s;
    while (*end && *end != '/' && *end != '?' && *end != '#') end++;
    if (!*end) end = NULL;
    return !end || dot < end;
}

int shortcut_resolve(const char* typed, char* target, int tcap, char* name, int ncap) {
    char s[FS_PATH_LEN];
    kstrlcpy(s, typed, sizeof(s));
    char* p = s;
    while (*p == ' ') p++;
    int n = (int)strlen(p);
    while (n && p[n - 1] == ' ') p[--n] = 0;
    if (!*p) return SC_NONE;
    /* a folder or a file: its own name */
    if (p[0] == '/' || p[0] == '~') {
        char full[FS_PATH_LEN];
        if (p[0] == '~') ksnprintf(full, sizeof(full), "/home/banana%s", p + 1);
        else kstrlcpy(full, p, sizeof(full));
        int k = (int)strlen(full);
        while (k > 1 && full[k - 1] == '/') full[--k] = 0;
        if (fs_find_dir(full) < 0 && fs_find_file(full) < 0) return SC_NONE;
        kstrlcpy(target, full, (size_t)tcap);
        const char* b = strrchr(full, '/');
        kstrlcpy(name, b && b[1] ? b + 1 : "Computer", (size_t)ncap);
        return shortcut_kind(target);
    }
    /* a web address: its host */
    if (!strncasecmp(p, "http://", 7) || !strncasecmp(p, "https://", 8) || looks_web(p)) {
        if (strstr(p, "://")) kstrlcpy(target, p, (size_t)tcap);
        else ksnprintf(target, (size_t)tcap, "https://%s", p);
        const char* h = strstr(target, "://") + 3;
        if (!strncasecmp(h, "www.", 4)) h += 4;
        int hn = 0;
        while (h[hn] && h[hn] != '/' && h[hn] != '?' && h[hn] != '#' && h[hn] != ':' && hn < ncap - 1) hn++;
        memcpy(name, h, (size_t)hn);
        name[hn] = 0;
        return SC_WEB;
    }
    /* a program: a built-in one, or an installed app by its title or name */
    for (int i = 0; i < NBUILTIN; i++)
        if (!strcasecmp(p, BUILTIN[i])) {
            ksnprintf(target, (size_t)tcap, "app:%s", BUILTIN[i]);
            kstrlcpy(name, BUILTIN[i], (size_t)ncap);
            return SC_APP;
        }
    static pkg_info_t pk[64];
    int np = pkg_list(pk, 64);
    for (int i = 0; i < np; i++)
        if (!strcasecmp(p, pk[i].name) || !strcasecmp(p, pk[i].title)) {
            ksnprintf(target, (size_t)tcap, "app:%s", pk[i].name);
            kstrlcpy(name, pk[i].title[0] ? pk[i].title : pk[i].name, (size_t)ncap);
            return SC_APP;
        }
    return SC_NONE;
}

int shortcut_create(const char* dir, const char* name, const char* target, char* out, int ocap) {
    char base[FS_NAME_LEN], clean[FS_NAME_LEN];
    int k = 0;
    for (const char* s = name; *s && k < (int)sizeof(clean) - 8; s++)       /* (no / in a file name) */
        clean[k++] = *s == '/' ? '-' : *s;
    clean[k] = 0;
    if (!k) kstrlcpy(clean, "Shortcut", sizeof(clean));
    ksnprintf(base, sizeof(base), "%s%s", clean, SHORTCUT_EXT);
    fs_mkdir_p(dir);
    if (fileops_unique(dir, base, out, ocap) < 0) return -1;
    char body[FS_PATH_LEN + 32];
    int n = ksnprintf(body, sizeof(body), "[Shortcut]\nTarget=%s\n", target);
    return fs_write_path(out, body, (uint32_t)n) < 0 ? -1 : 0;
}
