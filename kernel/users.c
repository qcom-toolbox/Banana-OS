/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "users.h"
#include "passwd.h"
#include "fs.h"
#include "kheap.h"
#include "kstring.h"
#include "config.h"
#include "settings.h"
#include "serial.h"

/*
 * /etc/passwd and /etc/group (users.h). Parsed into a small table when
 * they changed (their size and change time); written back whole, every
 * line kept but the one being changed.
 */

#define GROUPS_MAX 48

typedef struct {
    char     name[USER_NAME_MAX];
    uint32_t gid;
    char     members[256];                 /* "a,b,c" */
} group_t;

static user_t  g_users[USERS_MAX];
static int     g_nusers;
static group_t g_groups[GROUPS_MAX];
static int     g_ngroups;
static uint32_t g_pw_key, g_gr_key;        /* what the parsed files were (0: not parsed) */
static int     g_parsed;

/* ── reading ──────────────────────────────────────────────────────── */

/* the file's text (NUL-terminated, kmalloc'd) or NULL */
static char* read_text(const char* path, uint32_t* len) {
    int fi = fs_find_file(path);
    fs_file_t* f = fi >= 0 ? fs_get_file(fi) : NULL;
    if (!f || !f->content) return NULL;
    char* t = (char*)kmalloc(f->size + 1);
    if (!t) return NULL;
    memcpy(t, f->content, f->size);
    t[f->size] = 0;
    if (len) *len = f->size;
    return t;
}

static uint32_t file_key(const char* path) {
    int fi = fs_find_file(path);
    fs_file_t* f = fi >= 0 ? fs_file_info(fi) : NULL;
    if (!f) return 1;
    return (f->size * 2654435761u) ^ (uint32_t)f->mtime ^ ((uint32_t)fi << 20) ^ 0x5bd1e995u;
}

/* field k (0-based) of a ':' line into out */
static int field(const char* line, int k, char* out, int cap) {
    const char* p = line;
    for (int i = 0; i < k; i++) {
        p = strchr(p, ':');
        if (!p) { if (cap) out[0] = 0; return 0; }
        p++;
    }
    int n = 0;
    while (p[n] && p[n] != ':' && p[n] != '\n' && p[n] != '\r') n++;
    if (cap) {
        int c = n < cap - 1 ? n : cap - 1;
        memcpy(out, p, (size_t)c);
        out[c] = 0;
    }
    return 1;
}

static uint32_t num(const char* s) {
    uint32_t v = 0;
    k_parse_u32(s, &v);
    return v;
}

static void parse(void) {
    uint32_t pk = file_key(USERS_PASSWD), gk = file_key(USERS_GROUP);
    if (g_parsed && pk == g_pw_key && gk == g_gr_key) return;
    g_pw_key = pk;
    g_gr_key = gk;
    g_parsed = 1;
    g_nusers = g_ngroups = 0;
    char* t = read_text(USERS_PASSWD, NULL);
    for (char* line = t; line && *line && g_nusers < USERS_MAX;) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char idbuf[16];
        if (line[0] != '#' && strchr(line, ':') && field(line, 2, idbuf, sizeof(idbuf)) && idbuf[0]) {
            user_t* u = &g_users[g_nusers];
            memset(u, 0, sizeof(*u));
            field(line, 0, u->name, sizeof(u->name));
            u->uid = num(idbuf);
            field(line, 3, idbuf, sizeof(idbuf));
            u->gid = num(idbuf);
            field(line, 4, u->gecos, sizeof(u->gecos));
            field(line, 5, u->home, sizeof(u->home));
            field(line, 6, u->shell, sizeof(u->shell));
            if (u->name[0]) g_nusers++;
        }
        line = nl ? nl + 1 : NULL;
    }
    kfree(t);
    t = read_text(USERS_GROUP, NULL);
    for (char* line = t; line && *line && g_ngroups < GROUPS_MAX;) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char idbuf[16];
        if (line[0] != '#' && strchr(line, ':') && field(line, 2, idbuf, sizeof(idbuf)) && idbuf[0]) {
            group_t* g = &g_groups[g_ngroups];
            memset(g, 0, sizeof(*g));
            field(line, 0, g->name, sizeof(g->name));
            g->gid = num(idbuf);
            field(line, 3, g->members, sizeof(g->members));
            if (g->name[0]) g_ngroups++;
        }
        line = nl ? nl + 1 : NULL;
    }
    kfree(t);
}

int users_count(void) { parse(); return g_nusers; }
const user_t* users_at(int i) { parse(); return i >= 0 && i < g_nusers ? &g_users[i] : NULL; }

const user_t* user_by_name(const char* name) {
    parse();
    if (!name) return NULL;
    for (int i = 0; i < g_nusers; i++) if (!strcmp(g_users[i].name, name)) return &g_users[i];
    return NULL;
}

const user_t* user_by_uid(uint32_t uid) {
    parse();
    for (int i = 0; i < g_nusers; i++) if (g_users[i].uid == uid) return &g_users[i];
    return NULL;
}

int user_is_person(const user_t* u) {
    return u && u->uid >= UID_FIRST && u->uid <= UID_LAST && !strstr(u->shell, "nologin") && !strstr(u->shell, "false");
}

void user_name_of(uint32_t uid, char* out, int cap) {
    const user_t* u = user_by_uid(uid);
    if (u) kstrlcpy(out, u->name, (size_t)cap);
    else ksnprintf(out, (size_t)cap, "%u", uid);
}

static const group_t* group_by_gid(uint32_t gid) {
    parse();
    for (int i = 0; i < g_ngroups; i++) if (g_groups[i].gid == gid) return &g_groups[i];
    return NULL;
}

int group_name_of(uint32_t gid, char* out, int cap) {
    const group_t* g = group_by_gid(gid);
    if (g) { kstrlcpy(out, g->name, (size_t)cap); return 1; }
    ksnprintf(out, (size_t)cap, "%u", gid);
    return 0;
}

int group_by_name(const char* name, uint32_t* gid) {
    parse();
    for (int i = 0; i < g_ngroups; i++)
        if (!strcmp(g_groups[i].name, name)) { if (gid) *gid = g_groups[i].gid; return 1; }
    return 0;
}

static int lists(const char* members, const char* name) {
    size_t n = strlen(name);
    for (const char* p = members; *p;) {
        const char* c = strchr(p, ',');
        size_t l = c ? (size_t)(c - p) : strlen(p);
        if (l == n && !strncmp(p, name, n)) return 1;
        if (!c) break;
        p = c + 1;
    }
    return 0;
}

int user_groups(uint32_t uid, uint32_t* out, int max) {
    parse();
    const user_t* u = user_by_uid(uid);
    int n = 0;
    if (max <= 0) return 0;
    out[n++] = u ? u->gid : uid;
    if (!u) return n;
    for (int i = 0; i < g_ngroups && n < max; i++) {
        if (g_groups[i].gid == out[0] || !lists(g_groups[i].members, u->name)) continue;
        out[n++] = g_groups[i].gid;
    }
    return n;
}

int user_in_group(uint32_t uid, uint32_t gid) {
    uint32_t g[USER_GROUPS_MAX];
    int n = user_groups(uid, g, USER_GROUPS_MAX);
    for (int i = 0; i < n; i++) if (g[i] == gid) return 1;
    return 0;
}

int user_is_admin(uint32_t uid) { return uid == UID_ROOT || user_in_group(uid, GID_WHEEL); }

int users_admin_count(void) {
    parse();
    int n = 0;
    for (int i = 0; i < g_nusers; i++) if (user_is_person(&g_users[i]) && user_is_admin(g_users[i].uid)) n++;
    return n;
}

int users_any_password(void) {
    parse();
    for (int i = 0; i < g_nusers; i++) if (user_is_person(&g_users[i]) && passwd_is_set(g_users[i].name)) return 1;
    return 0;
}

int user_name_ok(const char* s) {
    if (!s || !s[0] || strlen(s) >= USER_NAME_MAX - 1) return 0;
    if (!((s[0] >= 'a' && s[0] <= 'z') || s[0] == '_')) return 0;
    for (const char* p = s; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return 0;
    return 1;
}

/* ── writing ──────────────────────────────────────────────────────── */

/* the file with the line whose first field is key replaced by line (NULL:
 * removed; a new key: added at the end). 0, or -1 */
static int file_line(const char* path, const char* key, const char* line, uint32_t mode);

/* (the user database is the system's: written with the kernel's rights -
 * who may change what is decided by the callers) */
int users_file_line(const char* path, const char* key, const char* line, uint32_t mode) {
    const fs_cred_t* old = fs_cred_swap(NULL);
    int r = file_line(path, key, line, mode);
    fs_cred_swap(old);
    return r;
}

static int file_line(const char* path, const char* key, const char* line, uint32_t mode) {
    uint32_t len = 0;
    char* t = read_text(path, &len);
    uint32_t cap = len + (line ? (uint32_t)strlen(line) : 0) + 4;
    char* out = (char*)kmalloc(cap);
    if (!out) { kfree(t); return -1; }
    uint32_t o = 0;
    int done = 0;
    size_t kl = strlen(key);
    for (char* p = t; p && *p;) {
        char* nl = strchr(p, '\n');
        size_t l = nl ? (size_t)(nl - p) : strlen(p);
        int match = l > kl && !strncmp(p, key, kl) && p[kl] == ':';
        if (match) {
            if (line && !done) { size_t ll = strlen(line); memcpy(out + o, line, ll); o += (uint32_t)ll; out[o++] = '\n'; }
            done = 1;
        } else if (l || nl) {
            memcpy(out + o, p, l);
            o += (uint32_t)l;
            out[o++] = '\n';
        }
        p = nl ? nl + 1 : NULL;
    }
    if (line && !done) { size_t ll = strlen(line); memcpy(out + o, line, ll); o += (uint32_t)ll; out[o++] = '\n'; }
    kfree(t);
    int r = fs_write_path(path, out, o) >= 0 ? 0 : -1;
    kfree(out);
    if (r == 0) { fs_chown(path, (int)UID_ROOT, (int)GID_ROOT); fs_chmod(path, mode); }
    g_parsed = 0;
    return r;
}

static int put_user(const user_t* u) {
    char line[256];
    ksnprintf(line, sizeof(line), "%s:x:%u:%u:%s:%s:%s", u->name, u->uid, u->gid, u->gecos, u->home, u->shell);
    return users_file_line(USERS_PASSWD, u->name, line, 0644);
}

static int put_group(const char* name, uint32_t gid, const char* members) {
    char line[320];
    ksnprintf(line, sizeof(line), "%s:x:%u:%s", name, gid, members);
    return users_file_line(USERS_GROUP, name, line, 0644);
}

/* name added to / taken from group gname's members */
static int set_member(const char* gname, const char* name, int in) {
    parse();
    for (int i = 0; i < g_ngroups; i++) {
        group_t* g = &g_groups[i];
        if (strcmp(g->name, gname)) continue;
        if (lists(g->members, name) == in) return 0;
        char m[256];
        m[0] = 0;
        for (const char* p = g->members; *p;) {          /* (the others, as they were) */
            const char* c = strchr(p, ',');
            size_t l = c ? (size_t)(c - p) : strlen(p);
            if (l && !(l == strlen(name) && !strncmp(p, name, l))) {
                if (m[0]) kstrlcat(m, ",", sizeof(m));
                size_t ml = strlen(m);
                if (ml + l < sizeof(m)) { memcpy(m + ml, p, l); m[ml + l] = 0; }
            }
            if (!c) break;
            p = c + 1;
        }
        if (in) { if (m[0]) kstrlcat(m, ",", sizeof(m)); kstrlcat(m, name, sizeof(m)); }
        uint32_t gid = g->gid;
        char gn[USER_NAME_MAX];
        kstrlcpy(gn, g->name, sizeof(gn));
        return put_group(gn, gid, m);
    }
    return -1;
}

static void home_skeleton(const char* home, uint32_t uid, uint32_t gid, uint32_t mode) {
    char p[FS_PATH_LEN];
    fs_mkdir_p(home);
    fs_chown(home, (int)uid, (int)gid);
    fs_chmod(home, mode);
    static const char* const subs[] = { "Pictures", "Downloads" };
    for (unsigned i = 0; i < sizeof(subs) / sizeof(subs[0]); i++) {
        ksnprintf(p, sizeof(p), "%s/%s", home, subs[i]);
        if (fs_find_dir(p) < 0) fs_mkdir_p(p);
        fs_chown(p, (int)uid, (int)gid);
    }
}

int user_add(const char* name, const char* full_name, int admin, char* err, int ecap) {
    parse();
    if (!user_name_ok(name)) { kstrlcpy(err, "a user name is a-z, 0-9, _ and - (starting with a letter), 31 at most", (size_t)ecap); return -1; }
    uint32_t gid_taken;
    if (user_by_name(name) || group_by_name(name, &gid_taken)) { kstrlcpy(err, "that name is taken", (size_t)ecap); return -1; }
    if (g_nusers >= USERS_MAX - 1) { kstrlcpy(err, "too many users", (size_t)ecap); return -1; }
    uint32_t uid = UID_FIRST;
    for (;;) {                                           /* the first free id (a group of the same number too) */
        int used = user_by_uid(uid) != NULL || group_by_gid(uid) != NULL;
        if (!used) break;
        if (++uid > UID_LAST) { kstrlcpy(err, "no user id left", (size_t)ecap); return -1; }
    }
    user_t u;
    memset(&u, 0, sizeof(u));
    kstrlcpy(u.name, name, sizeof(u.name));
    u.uid = u.gid = uid;
    kstrlcpy(u.gecos, full_name && full_name[0] ? full_name : name, sizeof(u.gecos));
    for (char* c = u.gecos; *c; c++) if (*c == ':' || *c == '\n') *c = ' ';
    ksnprintf(u.home, sizeof(u.home), "/home/%s", name);
    kstrlcpy(u.shell, "/bin/bash", sizeof(u.shell));
    if (put_group(name, uid, "") != 0 || put_user(&u) != 0) { kstrlcpy(err, "/etc/passwd could not be written", (size_t)ecap); return -1; }
    set_member("users", name, 1);
    if (admin) set_member("wheel", name, 1);
    home_skeleton(u.home, uid, uid, 0700);
    cfg_persist();
    klog("users: %s added (uid %u%s)\n", name, uid, admin ? ", administrator" : "");
    return 0;
}

int user_del(const char* name, int remove_home, char* err, int ecap) {
    const user_t* u = user_by_name(name);
    if (!u) { kstrlcpy(err, "no such user", (size_t)ecap); return -1; }
    if (u->uid == UID_ROOT || !user_is_person(u)) { kstrlcpy(err, "not a person's account", (size_t)ecap); return -1; }
    if (user_is_admin(u->uid) && users_admin_count() <= 1) { kstrlcpy(err, "the last administrator cannot go", (size_t)ecap); return -1; }
    user_t copy = *u;
    for (int i = 0; i < g_ngroups; i++) if (lists(g_groups[i].members, copy.name)) { char gn[USER_NAME_MAX]; kstrlcpy(gn, g_groups[i].name, sizeof(gn)); set_member(gn, copy.name, 0); }
    users_file_line(USERS_GROUP, copy.name, NULL, 0644);
    users_file_line(USERS_PASSWD, copy.name, NULL, 0644);
    passwd_remove(copy.name);
    if (remove_home && !strncmp(copy.home, "/home/", 6) && copy.home[6]) fs_delete(copy.home, 1);
    cfg_persist();
    klog("users: %s removed\n", copy.name);
    return 0;
}

int user_set_admin(const char* name, int admin, char* err, int ecap) {
    const user_t* u = user_by_name(name);
    if (!u || !user_is_person(u)) { kstrlcpy(err, "no such user", (size_t)ecap); return -1; }
    if (!admin && user_is_admin(u->uid) && users_admin_count() <= 1) { kstrlcpy(err, "the last administrator stays one", (size_t)ecap); return -1; }
    char n[USER_NAME_MAX];
    kstrlcpy(n, u->name, sizeof(n));
    if (set_member("wheel", n, admin) != 0) { kstrlcpy(err, "/etc/group could not be written", (size_t)ecap); return -1; }
    cfg_persist();
    return 0;
}

/* ── credentials and the session ──────────────────────────────────── */

void users_make_cred(uint32_t uid, void* out) {
    fs_cred_t* c = (fs_cred_t*)out;
    memset(c, 0, sizeof(*c));
    c->uid = uid;
    const user_t* u = user_by_uid(uid);
    c->gid = u ? u->gid : uid;
    uint32_t g[USER_GROUPS_MAX];
    int n = user_groups(uid, g, USER_GROUPS_MAX);
    for (int i = 0; i < n && i < FS_CRED_GROUPS; i++) c->groups[c->ngroups++] = g[i];
}

static uint32_t g_session = UID_FIRST;
static char     g_session_home[64] = "/home/banana";

uint32_t session_uid(void) { return g_session; }
const user_t* session_user(void) { return user_by_uid(g_session); }
const char* session_home(void) { return g_session_home; }

void session_set(uint32_t uid) {
    const user_t* u = user_by_uid(uid);
    g_session = uid;
    kstrlcpy(g_session_home, u && u->home[0] ? u->home : "/home/banana", sizeof(g_session_home));
    fs_set_home(g_session_home);
    klog("session: %s (uid %u)\n", u ? u->name : "?", uid);
}

const void* session_cred(void) {
    static fs_cred_t c;
    users_make_cred(g_session, &c);
    return &c;
}

/* ── older programs: /home/banana moved (app.c) ─────────────────────── */

int home_redirect_enabled(void) {
    char v[8];
    return !(settings_get("home_redirect", v, sizeof(v)) && v[0] == '0');
}
void home_redirect_set(int on) { settings_set("home_redirect", on ? "1" : "0"); }

const char* home_redirect_list(void) {
    static char v[256];
    if (!settings_get("home_redirect_skip", v, sizeof(v))) v[0] = 0;
    return v;
}

int home_redirect_skips(const char* app) { return app && app[0] && lists(home_redirect_list(), app); }

void home_redirect_skip(const char* app, int skip) {
    if (!app || !app[0] || strchr(app, ',') || strchr(app, '\n') || home_redirect_skips(app) == skip) return;
    char now[256], out[256];
    kstrlcpy(now, home_redirect_list(), sizeof(now));
    out[0] = 0;
    for (char* p = now; *p;) {
        char* c = strchr(p, ',');
        if (c) *c = 0;
        if (p[0] && strcmp(p, app)) { if (out[0]) kstrlcat(out, ",", sizeof(out)); kstrlcat(out, p, sizeof(out)); }
        if (!c) break;
        p = c + 1;
    }
    if (skip) { if (out[0]) kstrlcat(out, ",", sizeof(out)); kstrlcat(out, app, sizeof(out)); }
    settings_set("home_redirect_skip", out);
}

int home_redirect_wanted(const char* app) { return home_redirect_enabled() && !home_redirect_skips(app); }

int users_people(const user_t** out, int max) {
    parse();
    int n = 0;
    for (int i = 0; i < g_nusers && n < max; i++) if (user_is_person(&g_users[i])) out[n++] = &g_users[i];
    return n;
}

/* ── at every start ───────────────────────────────────────────────── */

static const char* DEFAULT_GROUP =
    "root:x:0:\n"
    "wheel:x:10:banana\n"
    "users:x:100:banana\n"
    "nogroup:x:65534:\n"
    "banana:x:1000:\n";

static const char* DEFAULT_DOAS =
    "# Who may act as another user (usually root) with doas - see `man doas.conf`.\n"
    "#   permit [nopass] [persist] <user | :group> [as <target>] [cmd <command>]\n"
    "#   deny <user | :group> ...\n"
    "# The last line that matches decides. persist: no password again for a while\n"
    "# (Settings > Users > Administrator prompt sets how long).\n"
    "permit persist :wheel\n";

void users_boot(void) {
    /* /etc/passwd: root and banana there (a disk from before keeps its lines) */
    if (fs_find_file(USERS_PASSWD) < 0)
    {
        static const char* d = "root:x:0:0:root:/root:/bin/sh\nbanana:x:1000:1000:banana:/home/banana:/bin/sh\n";
        fs_write_path(USERS_PASSWD, d, (uint32_t)strlen(d));
    }
    parse();
    if (!user_by_name("root")) users_file_line(USERS_PASSWD, "root", "root:x:0:0:root:/root:/bin/sh", 0644);
    if (!user_by_uid(UID_NOBODY)) users_file_line(USERS_PASSWD, "nobody", "nobody:x:65534:65534:nobody:/nonexistent:/bin/nologin", 0644);
    /* /etc/group: made once with banana an administrator (afterwards it is the admin's) */
    if (fs_find_file(USERS_GROUP) < 0) {
        fs_write_path(USERS_GROUP, DEFAULT_GROUP, (uint32_t)strlen(DEFAULT_GROUP));
        klog("users: /etc/group made (banana: wheel - an administrator)\n");
    }
    g_parsed = 0;
    parse();
    /* every person has their group (an older /etc/passwd's users) */
    for (int i = 0; i < g_nusers; i++) {
        if (!user_is_person(&g_users[i]) || group_by_gid(g_users[i].gid)) continue;
        user_t u = g_users[i];
        put_group(u.name, u.gid, "");
        parse();
    }
    /* nobody left to administer the computer (an edited group file): the first user is one again */
    if (users_admin_count() == 0) {
        const user_t* first = NULL;
        for (int i = 0; i < g_nusers; i++) if (user_is_person(&g_users[i]) && (!first || g_users[i].uid < first->uid)) first = &g_users[i];
        if (first) {
            char n[USER_NAME_MAX];
            kstrlcpy(n, first->name, sizeof(n));
            if (!group_by_name("wheel", NULL)) put_group("wheel", GID_WHEEL, n);
            else set_member("wheel", n, 1);
            klog("users: no administrator was left - %s is one again\n", n);
        }
    }
    if (fs_find_file("/etc/doas.conf") < 0) fs_write_path("/etc/doas.conf", DEFAULT_DOAS, (uint32_t)strlen(DEFAULT_DOAS));
    fs_chown("/etc/doas.conf", 0, 0);
    fs_chmod("/etc/doas.conf", 0644);
    fs_chown(USERS_PASSWD, 0, 0);
    fs_chmod(USERS_PASSWD, 0644);
    fs_chown(USERS_GROUP, 0, 0);
    fs_chmod(USERS_GROUP, 0644);
    if (fs_find_dir("/root") < 0) fs_mkdir_p("/root");
    fs_chown("/root", 0, 0);
    fs_chmod("/root", 0700);
    passwd_boot();                                      /* /etc/shadow: the standard format, root's only */
    /* each person's home, theirs (an older system made only banana's) */
    for (int i = 0; i < g_nusers; i++) {
        const user_t* u = &g_users[i];
        if (!user_is_person(u) || !u->home[0]) continue;
        if (fs_find_dir(u->home) < 0) home_skeleton(u->home, u->uid, u->gid, u->uid == UID_FIRST ? 0755 : 0700);
    }
}
