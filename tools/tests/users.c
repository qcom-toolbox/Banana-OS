/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Users on Linux, without Banana OS: kernel/fs.c (permissions), users.c,
 * passwd.c and elevate.c built for the host. The boot's migration (an older
 * disk: /etc/passwd only, the old "banana=pbkdf2..." /etc/shadow), adding,
 * removing and promoting users, the last administrator kept, Unix's file
 * rights under users' credentials (owner / group / others, the folders on
 * the way, /tmp's sticky bit, chmod / chown), doas.conf (last match wins,
 * nopass, persist, as, cmd), the remembered password and its setting, the
 * commands that change the system. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern char* u32_to_str(uint32_t, char*, int);
#include "../../kernel/fs.c"
#include "../../kernel/users.h"
#include "../../kernel/passwd.h"
#include "../../kernel/elevate.h"

static int checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)

/* the kernel's world, as much as these files need */
static uint32_t now = 1000;
void* kmalloc(size_t n) { return malloc(n ? n : 1); }
void* kzalloc(size_t n) { return calloc(1, n ? n : 1); }
void* krealloc(void* p, size_t n) { return realloc(p, n ? n : 1); }
void kfree(void* p) { free(p); }
uint32_t kheap_largest_free(void) { return 16u << 20; }
uint32_t timer_ms(void) { return now; }
uint32_t rtc_now(void) { return 1795000000; }
static int cwd_slot;
int* task_cwd_slot(void) { return &cwd_slot; }
static const void* cred_slot;
const void** task_cred_slot(void) { return &cred_slot; }
int task_current_pid(void) { return 3; }
void task_sleep_ms(uint32_t n) { now += n; }
const char* hostname_get(void) { return "test"; }
void klog(const char* fmt, ...) { (void)fmt; }
void terminal_write(const char* s) { (void)s; }
void terminal_writeln(const char* s) { (void)s; }
void terminal_write_color(const char* s, uint8_t a, uint8_t b) { (void)s; (void)a; (void)b; }
void terminal_putchar(char c) { (void)c; }
int cfg_persist(void) { return 0; }
void random_bytes(void* p, uint32_t n) { for (uint32_t i = 0; i < n; i++) ((uint8_t*)p)[i] = (uint8_t)(i * 37 + now); }
int sysinfo_live_boot(void) { return 0; }
int gui_is_enabled(void) { return 0; }
void gui_request_redraw(void) { }
static char g_set_pw[8] = "0", g_set_min[8] = "5";
int settings_get(const char* key, char* out, int cap) {
    const char* v = !strcmp(key, "elevate_password") ? g_set_pw : !strcmp(key, "elevate_minutes") ? g_set_min : NULL;
    if (!v) return 0;
    snprintf(out, (size_t)cap, "%s", v);
    return 1;
}
void settings_set(const char* key, const char* value) {
    if (!strcmp(key, "elevate_password")) snprintf(g_set_pw, sizeof(g_set_pw), "%s", value);
    if (!strcmp(key, "elevate_minutes")) snprintf(g_set_min, sizeof(g_set_min), "%s", value);
}

static const char* text(const char* path) {
    int fi = fs_find_file(path);
    fs_file_t* f = fi >= 0 ? fs_get_file(fi) : NULL;
    return f && f->content ? f->content : "";
}

static fs_cred_t cred_of(uint32_t uid) { fs_cred_t c; users_make_cred(uid, &c); return c; }

static void boot_fresh(void) {
    fs_init();
    users_boot();
    session_set(UID_FIRST);
    /* the files, root's, the right modes */
    fs_stat_t st;
    CHECK(fs_stat("/etc/shadow", 1, &st) == 0 && (st.mode & 0777) == 0600 && st.uid == 0);
    CHECK(fs_stat("/etc/group", 1, &st) == 0 && (st.mode & 0777) == 0644);
    CHECK(fs_stat("/etc/doas.conf", 1, &st) == 0);
    CHECK(fs_stat("/root", 1, &st) == 0 && (st.mode & 0777) == 0700);
    CHECK(strstr(text("/etc/group"), "wheel:x:10:banana"));
    CHECK(strstr(text("/etc/shadow"), "root:!:"));
    CHECK(user_is_admin(UID_FIRST) && !user_is_admin(UID_NOBODY));
    CHECK(passwd_locked("root") && !passwd_is_set("banana") && !passwd_locked("banana"));
    CHECK(users_admin_count() == 1);
}

static void migration(void) {
    /* an older disk: /etc/passwd with banana, the old shadow format with its password */
    fs_init();
    fs_delete("/etc/group", 0);
    fs_delete("/etc/doas.conf", 0);
    passwd_boot();                                   /* (a standard one, to make the old line from) */
    CHECK(passwd_set("banana", "secret1") == 0);
    char hash[200];
    const char* t = strstr(text("/etc/shadow"), "banana:");
    CHECK(t);
    const char* h = t + 7, *e = strchr(h, ':');
    snprintf(hash, sizeof(hash), "%.*s", (int)(e - h), h);
    char old[300];
    snprintf(old, sizeof(old), "# Banana OS password hashes (PBKDF2-HMAC-SHA256) - set with `passwd`.\nbanana=%s\n", hash + 1);
    fs_write_path("/etc/shadow", old, (uint32_t)strlen(old));
    CHECK(passwd_check("banana", "secret1"));        /* (the old line is read as it is) */
    users_boot();
    CHECK(!strstr(text("/etc/shadow"), "banana=") && strstr(text("/etc/shadow"), "banana:$pbkdf2-sha256$"));
    CHECK(!strstr(text("/etc/shadow"), "# Banana OS"));
    CHECK(passwd_check("banana", "secret1") && !passwd_check("banana", "secret2"));
    CHECK(strstr(text("/etc/group"), "wheel:x:10:banana") && user_is_admin(UID_FIRST));
    /* run again: nothing changes (an installed system at its next start) */
    char before[512];
    snprintf(before, sizeof(before), "%s", text("/etc/shadow"));
    users_boot();
    CHECK(!strcmp(before, text("/etc/shadow")));
    /* an edited group file with no administrator left: the first user is one again */
    fs_write_path("/etc/group", "root:x:0:\nwheel:x:10:\nbanana:x:1000:\n", 33);
    users_boot();
    CHECK(user_is_admin(UID_FIRST));
}

static void accounts(void) {
    boot_fresh();
    char err[96];
    CHECK(user_add("Bad", NULL, 0, err, sizeof(err)) != 0);
    CHECK(user_add("root", NULL, 0, err, sizeof(err)) != 0);
    CHECK(user_add("alice", "Alice A", 0, err, sizeof(err)) == 0);
    CHECK(user_add("alice", NULL, 0, err, sizeof(err)) != 0);
    const user_t* a = user_by_name("alice");
    CHECK(a && a->uid == 1001 && a->gid == 1001 && !strcmp(a->home, "/home/alice") && !strcmp(a->gecos, "Alice A"));
    CHECK(!user_is_admin(1001) && user_in_group(1001, GID_USERS));
    fs_stat_t st;
    CHECK(fs_stat("/home/alice", 1, &st) == 0 && st.uid == 1001 && (st.mode & 0777) == 0700);
    CHECK(fs_stat("/home/alice/Pictures", 1, &st) == 0 && st.uid == 1001);
    CHECK(user_add("bob", NULL, 1, err, sizeof(err)) == 0 && user_is_admin(user_by_name("bob")->uid));
    CHECK(users_admin_count() == 2);
    const user_t* p[8];
    CHECK(users_people(p, 8) == 3);
    /* the last administrator stays one */
    CHECK(user_set_admin("bob", 0, err, sizeof(err)) == 0 && users_admin_count() == 1);
    CHECK(user_set_admin("banana", 0, err, sizeof(err)) != 0);
    CHECK(user_del("banana", 0, err, sizeof(err)) != 0);
    CHECK(user_del("root", 0, err, sizeof(err)) != 0);
    CHECK(passwd_set("alice", "alice123") == 0 && passwd_is_set("alice"));
    CHECK(user_del("alice", 1, err, sizeof(err)) == 0);
    CHECK(!user_by_name("alice") && fs_find_dir("/home/alice") < 0 && !strstr(text("/etc/shadow"), "alice:"));
    CHECK(!strstr(text("/etc/group"), "alice"));
    /* other lines are kept as they were */
    CHECK(strstr(text("/etc/passwd"), "root:x:0:0:") && strstr(text("/etc/passwd"), "bob:x:"));
}

static void rights(void) {
    boot_fresh();
    char err[96];
    CHECK(user_add("alice", NULL, 0, err, sizeof(err)) == 0);
    fs_cred_t banana = cred_of(UID_FIRST), alice = cred_of(1001), root = cred_of(0);
    fs_write_path("/home/banana/notes", "x", 1);          /* (the kernel's: banana's, as the folder) */
    CHECK(fs_find_file("/home/banana/notes") >= 0 && F(fs_find_file("/home/banana/notes")).uid == UID_FIRST);
    fs_cred_swap(&banana);
    CHECK(fs_create("/etc/evil") < 0 && fs_perm_error());
    CHECK(fs_mkdir("/usr/x") < 0);
    CHECK(fs_write(fs_find_file("/etc/passwd"), "", 0) != 0);
    int fi = fs_create("/home/banana/mine");
    CHECK(fi >= 0 && F(fi).uid == UID_FIRST && F(fi).gid == UID_FIRST);
    CHECK(!fs_access_entry(0, fs_find_file("/etc/shadow"), 4));
    CHECK(fs_access_entry(0, fs_find_file("/etc/passwd"), 4));
    CHECK(!fs_access_entry(1, fs_find_dir("/home/alice"), 4));                /* (alice's: 0700) */
    CHECK(fs_create("/home/alice/x") < 0);
    CHECK(fs_chmod("/etc/passwd", 0777) != 0 && fs_chown("/home/banana/mine", 0, -1) != 0);
    CHECK(fs_chmod("/home/banana/mine", 0600) == 0);
    /* the sticky /tmp: one's own goes, another's does not */
    int t1 = fs_create("/tmp/banana1");
    CHECK(t1 >= 0);
    fs_cred_swap(&alice);
    CHECK(fs_access_entry(1, fs_find_dir("/home/banana"), 5));               /* (banana's home: 0755) */
    CHECK(!fs_access_entry(0, fs_find_file("/home/banana/mine"), 4));          /* (0600: banana's own) */
    int t2 = fs_create("/tmp/alice1");
    CHECK(t2 >= 0 && F(t2).uid == 1001);
    fs_delete("/tmp/banana1", 0);
    CHECK(fs_find_file("/tmp/banana1") >= 0);
    fs_delete("/tmp/alice1", 0);
    CHECK(fs_find_file("/tmp/alice1") < 0);
    CHECK(fs_move("/tmp/banana1", "/tmp/stolen") < 0);
    /* root: everything (but running what nobody may run) */
    fs_cred_swap(&root);
    CHECK(fs_access_entry(0, fs_find_file("/etc/shadow"), 6) && fs_access_entry(1, fs_find_dir("/home/alice"), 7));
    CHECK(!fs_access_entry(0, fs_find_file("/etc/passwd"), 1));
    int r1 = fs_create("/etc/rootfile");
    CHECK(r1 >= 0 && F(r1).uid == 0);
    fs_cred_swap(NULL);
    /* the kernel's own work: as before users */
    CHECK(fs_create("/etc/kernelfile") >= 0);
}

static void doas_rules(void) {
    boot_fresh();
    char err[96];
    CHECK(user_add("alice", NULL, 0, err, sizeof(err)) == 0);
    CHECK(user_add("bob", NULL, 0, err, sizeof(err)) == 0);
    int np, ps;
    CHECK(elevate_permitted(UID_FIRST, 0, NULL, &np, &ps) && ps && !np);
    CHECK(!elevate_permitted(1001, 0, NULL, &np, &ps));
    CHECK(elevate_permitted(0, 1001, NULL, &np, &ps) && np);
    const char* conf =
        "permit persist :wheel\n"
        "permit nopass alice as root cmd pkg\n"
        "permit bob as alice\n"
        "deny banana cmd rm\n";
    fs_write_path("/etc/doas.conf", conf, (uint32_t)strlen(conf));
    CHECK(elevate_permitted(1001, 0, "pkg", &np, &ps) && np && !ps);
    CHECK(elevate_permitted(1001, 0, "/bin/pkg", &np, &ps));
    CHECK(!elevate_permitted(1001, 0, "vim", &np, &ps));
    CHECK(elevate_permitted(1002, 1001, NULL, &np, &ps) && !elevate_permitted(1002, 0, NULL, &np, &ps));
    CHECK(!elevate_permitted(UID_FIRST, 0, "rm", &np, &ps) && elevate_permitted(UID_FIRST, 0, "ls", &np, &ps));
    CHECK(!elevate_permitted(UID_FIRST, 1001, NULL, &np, &ps));            /* (no "as": root only) */
    /* no doas.conf: administrators */
    fs_delete("/etc/doas.conf", 0);
    CHECK(elevate_permitted(UID_FIRST, 0, NULL, &np, &ps) && !elevate_permitted(1001, 0, NULL, &np, &ps));
}

static void remembered(void) {
    boot_fresh();
    CHECK(elevate_silent(UID_FIRST, 0, 7, NULL));                         /* (no password: never asked) */
    CHECK(passwd_set("banana", "pw12345") == 0);
    CHECK(!elevate_silent(UID_FIRST, 0, 7, NULL));
    elevate_remember(UID_FIRST, 7);
    CHECK(elevate_silent(UID_FIRST, 0, 7, NULL) && !elevate_silent(UID_FIRST, 0, 8, NULL));   /* (that terminal only) */
    now += 4 * 60000;
    CHECK(elevate_silent(UID_FIRST, 0, 7, NULL));
    now += 2 * 60000;
    CHECK(!elevate_silent(UID_FIRST, 0, 7, NULL));                        /* (5 minutes by default) */
    elevate_remember(UID_FIRST, 7);
    elevate_forget(UID_FIRST, 7);
    CHECK(!elevate_silent(UID_FIRST, 0, 7, NULL));
    elevate_set_remember_minutes(0);                                        /* every time */
    elevate_remember(UID_FIRST, 7);
    CHECK(!elevate_silent(UID_FIRST, 0, 7, NULL));
    elevate_set_remember_minutes(60);
    elevate_remember(UID_FIRST, 7);
    elevate_set_remember_minutes(1);                                        /* lowered: what was given goes by it */
    now += 2 * 60000;
    CHECK(!elevate_silent(UID_FIRST, 0, 7, NULL));
    CHECK(elevate_remember_minutes() == 1);
    elevate_set_remember_minutes(500);
    CHECK(elevate_remember_minutes() == 60);
    CHECK(!elevate_gui_wants_password());
    elevate_set_gui_password(1);
    CHECK(elevate_gui_wants_password());
}

static void commands(void) {
    char* a1[] = { "pkg", "install", "x.bpk" }; CHECK(elevate_cmd_privileged(3, a1));
    char* a2[] = { "pkg", "list" };             CHECK(!elevate_cmd_privileged(2, a2));
    char* a3[] = { "apt", "upgrade" };          CHECK(elevate_cmd_privileged(2, a3));
    char* a4[] = { "apt", "search", "x" };      CHECK(!elevate_cmd_privileged(3, a4));
    char* a5[] = { "hostname" };                CHECK(!elevate_cmd_privileged(1, a5));
    char* a6[] = { "hostname", "pc" };          CHECK(elevate_cmd_privileged(2, a6));
    char* a7[] = { "update" };                  CHECK(elevate_cmd_privileged(1, a7));
    char* a8[] = { "install" };                 CHECK(elevate_cmd_privileged(1, a8));       /* (installed: live 0) */
    char* a9[] = { "passwd" };                  CHECK(!elevate_cmd_privileged(1, a9));
    char* b1[] = { "passwd", "alice" };         CHECK(elevate_cmd_privileged(2, b1));
    char* b2[] = { "ls", "-l" };                CHECK(!elevate_cmd_privileged(2, b2));
    char* b3[] = { "adduser", "x" };            CHECK(elevate_cmd_privileged(2, b3));
    char* b4[] = { "ifconfig" };                CHECK(!elevate_cmd_privileged(1, b4));
    char* b5[] = { "ifconfig", "eth0", "10.0.0.2" }; CHECK(elevate_cmd_privileged(3, b5));
    char* b6[] = { "httpd", "status" };         CHECK(!elevate_cmd_privileged(2, b6));
    char* b7[] = { "httpd", "start" };          CHECK(elevate_cmd_privileged(2, b7));
}

int main(void) {
    boot_fresh();
    migration();
    accounts();
    rights();
    doas_rules();
    remembered();
    commands();
    printf("users: %d checks passed\n", checks);
    return 0;
}
int ct_memcmp(const void* a, const void* b, uint32_t len) {
    const uint8_t* x = a; const uint8_t* y = b; uint8_t d = 0;
    for (uint32_t i = 0; i < len; i++) d |= x[i] ^ y[i];
    return d;
}
char* u32_to_str(uint32_t v, char* b, int n) { snprintf(b, (size_t)n, "%u", v); return b; }
