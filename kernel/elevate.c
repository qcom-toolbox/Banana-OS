/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "elevate.h"
#include "users.h"
#include "passwd.h"
#include "fs.h"
#include "kheap.h"
#include "kstring.h"
#include "settings.h"
#include "sysinfo.h"
#include "timer.h"
#include "task.h"
#include "serial.h"
#include "gui.h"

/* ── the settings ───────────────────────────────────────────────────── */

int elevate_gui_wants_password(void) {
    char v[8];
    return settings_get("elevate_password", v, sizeof(v)) && v[0] == '1';
}

void elevate_set_gui_password(int on) { settings_set("elevate_password", on ? "1" : "0"); }

int elevate_remember_minutes(void) {
    char v[8];
    if (!settings_get("elevate_minutes", v, sizeof(v))) return 5;
    uint32_t m = 5;
    k_parse_u32(v, &m);
    return m > 60 ? 60 : (int)m;
}

void elevate_set_remember_minutes(int minutes) {
    char v[8];
    if (minutes < 0) minutes = 0;
    if (minutes > 60) minutes = 60;
    ksnprintf(v, sizeof(v), "%d", minutes);
    settings_set("elevate_minutes", v);
}

/* ── /etc/doas.conf ─────────────────────────────────────────────────── */

/* the next word of a line (spaces between; "quoted" kept together) */
static const char* word(const char* p, char* out, int cap) {
    while (*p == ' ' || *p == '\t') p++;
    int n = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && *p != '\n') { if (n < cap - 1) out[n++] = *p; p++; }
        if (*p == '"') p++;
    } else {
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '#') { if (n < cap - 1) out[n++] = *p; p++; }
    }
    out[n] = 0;
    return p;
}

static int identity_matches(const char* id, uint32_t uid) {
    if (id[0] == ':') {
        uint32_t gid;
        return group_by_name(id + 1, &gid) && user_in_group(uid, gid);
    }
    const user_t* u = user_by_name(id);
    return u && u->uid == uid;
}

int elevate_permitted(uint32_t uid, uint32_t target, const char* cmd, int* nopass, int* persist) {
    if (nopass) *nopass = 0;
    if (persist) *persist = 0;
    if (uid == UID_ROOT) { if (nopass) *nopass = 1; return 1; }
    int fi = fs_find_file("/etc/doas.conf");
    fs_file_t* f = fi >= 0 ? fs_get_file(fi) : NULL;
    int found = 0, ok = 0, np = 0, ps = 0;
    char cmdname[64] = "";
    if (cmd) word(cmd, cmdname, sizeof(cmdname));
    /* (no doas.conf - not made yet, or removed: administrators, as by default) */
    if (!f || !f->content) { ok = user_is_admin(uid); ps = 1; found = 1; }
    for (const char* line = f && f->content ? f->content : NULL; line && *line;) {
        const char* nl = strchr(line, '\n');
        char w[64];
        const char* p = word(line, w, sizeof(w));
        int permit = !strcmp(w, "permit"), deny = !strcmp(w, "deny");
        if (permit || deny) {
            int r_np = 0, r_ps = 0, match = 1;
            p = word(p, w, sizeof(w));
            while (!strcmp(w, "nopass") || !strcmp(w, "persist") || !strcmp(w, "keepenv") || !strcmp(w, "nolog") || !strncmp(w, "setenv", 6)) {
                if (!strcmp(w, "nopass")) r_np = 1;
                if (!strcmp(w, "persist")) r_ps = 1;
                if (!strncmp(w, "setenv", 6)) { while (*p && *p != '}' && *p != '\n') p++; if (*p == '}') p++; }
                p = word(p, w, sizeof(w));
            }
            if (!w[0] || !identity_matches(w, uid)) match = 0;
            p = word(p, w, sizeof(w));
            if (match && !strcmp(w, "as")) {
                p = word(p, w, sizeof(w));
                const user_t* t = user_by_name(w);
                if (!t || t->uid != target) match = 0;
                p = word(p, w, sizeof(w));
            } else if (match && target != UID_ROOT) {
                match = 0;                                     /* (no "as": root only) */
            }
            if (match && !strcmp(w, "cmd")) {
                p = word(p, w, sizeof(w));
                const char* base = strrchr(cmdname, '/');
                base = base ? base + 1 : cmdname;
                if (!cmd || (strcmp(w, cmdname) && strcmp(w, base))) match = 0;
            }
            if (match) { found = 1; ok = permit; np = r_np; ps = r_ps; }   /* (the last match decides) */
        }
        line = nl ? nl + 1 : NULL;
    }
    if (!found || !ok) return 0;
    if (nopass) *nopass = np;
    if (persist) *persist = ps;
    return 1;
}

/* ── remembered ─────────────────────────────────────────────────────── */

#define STAMPS 16
static struct { uint32_t uid, session, until; int used; } g_stamps[STAMPS];

void elevate_remember(uint32_t uid, uint32_t session) {
    int m = elevate_remember_minutes();
    if (m <= 0) return;
    uint32_t until = timer_ms() + (uint32_t)m * 60000u;
    int at = -1;
    for (int i = 0; i < STAMPS; i++) {
        if (g_stamps[i].used && g_stamps[i].uid == uid && g_stamps[i].session == session) { at = i; break; }
        if (at < 0 && (!g_stamps[i].used || (int32_t)(timer_ms() - g_stamps[i].until) >= 0)) at = i;
    }
    if (at < 0) at = 0;
    g_stamps[at].used = 1;
    g_stamps[at].uid = uid;
    g_stamps[at].session = session;
    g_stamps[at].until = until;
}

void elevate_forget(uint32_t uid, uint32_t session) {
    for (int i = 0; i < STAMPS; i++)
        if (g_stamps[i].used && g_stamps[i].uid == uid && g_stamps[i].session == session) g_stamps[i].used = 0;
}

static int remembered(uint32_t uid, uint32_t session) {
    int m = elevate_remember_minutes();
    for (int i = 0; i < STAMPS; i++) {
        if (!g_stamps[i].used || g_stamps[i].uid != uid || g_stamps[i].session != session) continue;
        uint32_t left = g_stamps[i].until - timer_ms();
        /* (still running, and not past what the setting allows now - it may have been lowered) */
        if ((int32_t)left > 0 && left <= (uint32_t)m * 60000u) return 1;
        g_stamps[i].used = 0;
    }
    return 0;
}

int elevate_silent(uint32_t uid, uint32_t target, uint32_t session, const char* cmd) {
    if (uid == UID_ROOT) return 1;
    int nopass, persist;
    if (!elevate_permitted(uid, target, cmd, &nopass, &persist)) return 0;
    char name[USER_NAME_MAX];
    user_name_of(uid, name, sizeof(name));
    if (nopass || !passwd_is_set(name)) return 1;            /* (no password: never asked - as before users) */
    return persist && remembered(uid, session);
}

/* ── the system commands that change the system ─────────────────────── */

static int is(const char* a, const char* b) { return a && !strcmp(a, b); }

const char* elevate_cmd_privileged(int argc, char** argv) {
    if (argc < 1) return NULL;
    const char* c = argv[0];
    const char* s = argc > 1 ? argv[1] : NULL;
    if (is(c, "pkg")) return s && (is(s, "install") || is(s, "remove") || is(s, "verify") || is(s, "update") || is(s, "upgrade") || is(s, "autoremove")) ? "install or remove apps" : NULL;
    if (is(c, "apt") || is(c, "apt-get"))
        return s && (is(s, "install") || is(s, "remove") || is(s, "purge") || is(s, "upgrade") || is(s, "full-upgrade") || is(s, "update") ||
                     is(s, "autoremove") || is(s, "add-repo") || is(s, "remove-repo")) ? "install or remove apps" : NULL;
    if (is(c, "install")) return sysinfo_live_boot() ? NULL : "install Banana OS on a disk";   /* (the live system: anyone) */
    if (is(c, "update") || is(c, "repair")) return "update the system";
    if (is(c, "diskpart") || is(c, "mkfs.fat") || is(c, "mkfs.vfat") || is(c, "mkfs.exfat")) return "change the disks";
    if (is(c, "fsck") || is(c, "e2fsck")) return "check and repair the disk";
    if (is(c, "hostname")) return s ? "change the computer's name" : NULL;
    if (is(c, "timezone")) return s ? "change the time zone" : NULL;
    if (is(c, "date")) return s && (is(s, "set") || is(s, "-s")) ? "change the date and time" : NULL;
    if (is(c, "ntp")) return s && !is(s, "status") ? "change the time server" : NULL;
    if (is(c, "ifconfig")) return argc > 2 || (s && (is(s, "priority") || is(s, "static") || is(s, "dhcp"))) ? "change the network settings" : NULL;
    if (is(c, "firewall")) return s && !is(s, "status") && !is(s, "list") ? "change the firewall" : NULL;
    if (is(c, "ipv6") || is(c, "toggle6")) return s && (is(s, "on") || is(s, "off")) ? "change the network settings" : is(c, "toggle6") ? "change the network settings" : NULL;
    if (is(c, "httpd") || is(c, "sshd") || is(c, "rdpd")) return s && !is(s, "status") ? "start or stop a server" : NULL;
    if (is(c, "autologin")) return s ? "change how the computer starts" : NULL;
    if (is(c, "swap")) return s && !is(s, "status") ? "change the swap area" : NULL;
    if (is(c, "driver") || is(c, "drivers")) return s && (is(s, "load") || is(s, "unload") || is(s, "install") || is(s, "remove") || is(s, "enable") || is(s, "disable")) ? "change the drivers" : NULL;
    if (is(c, "gpu3d")) return s && (is(s, "on") || is(s, "off")) ? "change the graphics driver" : NULL;
    if (is(c, "drop_caches") || is(c, "selftest") || is(c, "kptest")) return "test the system";
    if (is(c, "crashtest")) return s && !is(s, "preview") && !is(s, "report") ? "stop the system" : NULL;
    if (is(c, "adduser") || is(c, "useradd") || is(c, "deluser") || is(c, "userdel") || is(c, "usermod")) return "change the users";
    if (is(c, "elevation")) return s && !is(s, "test") ? "change how administrator rights are asked" : NULL;
    if (is(c, "passwd")) {                                     /* another user's password, or a switch */
        for (int i = 1; i < argc; i++) if (argv[i][0] != '-') return "change another user's password";
        return NULL;
    }
    return NULL;
}

/* ── the desktop's prompt ───────────────────────────────────────────── */

#define QUEUE 8
typedef struct {
    int             used, state;                 /* 0 waiting, 1 allowed, -1 refused */
    uint32_t        uid;
    char            app[32], what[80];
    elevate_done_fn done;
    void*           ctx;
    int             waiter;                      /* a task waits for it (elevate_request) */
} req_t;
static req_t            g_q[QUEUE];
static int              g_cur = -1;              /* shown */
static elevate_prompt_t g_p;
static int              g_focus;

static void show_next(void) {
    g_cur = -1;
    memset(&g_p, 0, sizeof(g_p));
    for (int i = 0; i < QUEUE; i++) if (g_q[i].used && g_q[i].state == 0) { g_cur = i; break; }
    if (g_cur < 0) { g_focus = 0; gui_request_redraw(); return; }
    req_t* r = &g_q[g_cur];
    g_p.open = 1;
    g_p.app = r->app;
    g_p.what = r->what;
    int admin = user_is_admin(r->uid);
    g_p.admin_pick = !admin;
    if (admin) user_name_of(r->uid, g_p.admin, sizeof(g_p.admin));
    else {
        for (int i = 0; i < users_count(); i++) {
            const user_t* u = users_at(i);
            if (user_is_person(u) && user_is_admin(u->uid)) { kstrlcpy(g_p.admin, u->name, sizeof(g_p.admin)); break; }
        }
    }
    g_p.need_password = !admin || elevate_gui_wants_password();
    if (g_p.need_password && !passwd_is_set(g_p.admin)) g_p.need_password = 0;   /* (that administrator has none) */
    g_focus = g_p.need_password;                       /* (a password to type: the keys go there) */
    gui_request_redraw();
}

static int enqueue(uint32_t uid, const char* app, const char* what, elevate_done_fn done, void* ctx, int waiter) {
    for (int i = 0; i < QUEUE; i++) {
        if (g_q[i].used) continue;
        memset(&g_q[i], 0, sizeof(g_q[i]));
        g_q[i].used = 1;
        g_q[i].uid = uid;
        kstrlcpy(g_q[i].app, app && app[0] ? app : "An app", sizeof(g_q[i].app));
        kstrlcpy(g_q[i].what, what, sizeof(g_q[i].what));
        g_q[i].done = done;
        g_q[i].ctx = ctx;
        g_q[i].waiter = waiter;
        if (g_cur < 0) show_next();
        return i;
    }
    return -1;
}

static void finish(int allow) {
    if (g_cur < 0) return;
    req_t* r = &g_q[g_cur];
    r->state = allow ? 1 : -1;
    if (allow) elevate_remember(user_is_admin(r->uid) ? r->uid : (user_by_name(g_p.admin) ? user_by_name(g_p.admin)->uid : r->uid), ELEVATE_DESKTOP);
    klog("elevate: %s - %s for uid %u: %s\n", r->app, r->what, r->uid, allow ? "allowed" : "refused");
    elevate_done_fn done = r->done;
    void* ctx = r->ctx;
    if (!r->waiter) r->used = 0;                       /* (a waiter frees its own) */
    show_next();
    if (done) done(ctx, allow);
}

void elevate_prompt_answer(int allow) {
    if (!g_p.open) return;
    if (allow && g_p.need_password) {
        if (!passwd_check(g_p.admin, g_p.typed)) {
            memset(g_p.typed, 0, sizeof(g_p.typed));
            if (++g_p.tries >= 3) { finish(0); return; }
            kstrlcpy(g_p.err, "That is not the password - try again", sizeof(g_p.err));
            gui_request_redraw();
            return;
        }
    }
    memset(g_p.typed, 0, sizeof(g_p.typed));
    finish(allow);
}

void elevate_prompt_next_admin(void) {
    if (!g_p.open || !g_p.admin_pick) return;
    int n = users_count(), at = -1;
    for (int i = 0; i < n; i++) if (!strcmp(users_at(i)->name, g_p.admin)) at = i;
    for (int k = 1; k <= n; k++) {
        const user_t* u = users_at((at + k) % n);
        if (user_is_person(u) && user_is_admin(u->uid)) { kstrlcpy(g_p.admin, u->name, sizeof(g_p.admin)); break; }
    }
    g_p.need_password = passwd_is_set(g_p.admin);
    g_p.err[0] = 0;
    gui_request_redraw();
}

elevate_prompt_t* elevate_prompt(void) { return &g_p; }
int elevate_prompt_focused(void) { return g_p.open && g_focus; }
void elevate_prompt_focus(int on) { g_focus = on && g_p.open; }

void elevate_ask_async(uint32_t uid, const char* app, const char* what, elevate_done_fn done, void* ctx) {
    if (elevate_silent(uid, UID_ROOT, ELEVATE_DESKTOP, NULL)) { if (done) done(ctx, 1); return; }
    if (enqueue(uid, app, what, done, ctx, 0) < 0 && done) done(ctx, 0);
}

int elevate_request(uint32_t uid, uint32_t session, const char* app, const char* what) {
    if (elevate_silent(uid, UID_ROOT, session, NULL)) return 1;
    if (session != ELEVATE_DESKTOP && elevate_silent(uid, UID_ROOT, ELEVATE_DESKTOP, NULL)) return 1;
    if (!gui_is_enabled() || task_current_pid() == 0) return 0;   /* (no desktop to ask on; never the desktop's own task) */
    int i = enqueue(uid, app, what, NULL, NULL, 1);
    if (i < 0) return 0;
    while (g_q[i].state == 0 && gui_is_enabled()) task_sleep_ms(20);
    int ok = g_q[i].state == 1;
    if (g_q[i].state == 0 && g_cur == i) finish(0);  /* (the desktop went away) */
    g_q[i].used = 0;
    return ok;
}
