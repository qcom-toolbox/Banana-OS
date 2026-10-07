#include "fs.h"
#include "terminal.h"
#include "types.h"
#include "kheap.h"
#include "kstring.h"
#include "serial.h"

#define FS_HOME_PATH "/home/banana"

static fs_dir_t  dirs[FS_MAX_DIRS];
static fs_file_t files[FS_MAX_FILES];
static int       cwd      = 0;   /* current dir index (0 = root) */
static int       home_dir = 0;   /* dir index of /home/banana */

/* mounted volumes (USB sticks): index = mount id, 0 unused */
typedef struct {
    int                   used;
    int                   root;     /* dir index of the mount point */
    const fs_mount_ops_t* ops;
    void*                 ctx;
    char                  point[FS_PATH_LEN];
} mount_t;
static mount_t mounts[FS_MAX_MOUNTS + 1];
static int     g_io_err;
static int     g_ready;       /* the tree is set up (fs_init / a loaded disk) */

static void dir_abs_path(int idx, char* buf, int buflen);

/* ── string helpers ─────────────────────────────────────────────── */
static void k_strcpy(char* dst, const char* src, int max) {
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}
static int k_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
static uint32_t k_strlen(const char* s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}
static int k_strstr(const char* hay, const char* needle) {
    if (!*needle) return 1;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j] == needle[j]) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

/* ── mounts ─────────────────────────────────────────────────────── */

static mount_t* mount_of(int m) {
    return (m > 0 && m <= FS_MAX_MOUNTS && mounts[m].used) ? &mounts[m] : NULL;
}


/* ── path helpers ───────────────────────────────────────────────── */

/* Splits the next '/'-delimited component off *p into out, skipping any
 * leading/duplicate slashes. Returns 0 once nothing is left. */
static int next_component(const char** p, char* out, int outlen) {
    const char* s = *p;
    while (*s == '/') s++;
    if (!*s) { *p = s; return 0; }
    int i = 0;
    while (s[i] && s[i] != '/') {
        if (i < outlen - 1) out[i] = s[i];
        i++;
    }
    out[(i < outlen - 1) ? i : (outlen - 1)] = '\0';
    *p = s + i;
    return 1;
}

/* Expands a leading "~" (home) into FS_HOME_PATH; otherwise copies as-is. */
static void expand_tilde(const char* in, char* out, int outlen) {
    if (in && in[0] == '~' && (in[1] == '\0' || in[1] == '/')) {
        int i = 0;
        const char* h = FS_HOME_PATH;
        while (h[i] && i < outlen - 1) { out[i] = h[i]; i++; }
        const char* rest = in + 1;
        int j = 0;
        while (rest[j] && i < outlen - 1) { out[i++] = rest[j++]; }
        out[i] = '\0';
    } else {
        k_strcpy(out, in ? in : "", outlen);
    }
}

static int find_dir_in(int parent, const char* name) {
    for (int i = 0; i < FS_MAX_DIRS; i++)
        if (dirs[i].used && dirs[i].parent_dir == parent &&
            k_strcmp(dirs[i].name, name) == 0)
            return i;
    return -1;
}

static int find_file_in(int parent, const char* name) {
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (files[i].used && files[i].parent_dir == parent &&
            k_strcmp(files[i].name, name) == 0)
            return i;
    return -1;
}

/* bumped by every change to the tree or a file (the disk autosave watches it) */
static uint32_t g_fs_gen = 1;
static void touched(void) { g_fs_gen++; }
uint32_t fs_generation(void) { return g_fs_gen; }

static int mkdir_in(int parent, const char* name_in) {
    touched();
    char name[FS_NAME_LEN];
    k_strcpy(name, name_in, FS_NAME_LEN);
    if (find_dir_in(parent, name) >= 0)  return -1; /* already exists */
    if (find_file_in(parent, name) >= 0) return -1; /* name clash */
    for (int i = 1; i < FS_MAX_DIRS; i++) {
        if (!dirs[i].used) {
            uint32_t node = 0;
            mount_t* m = mount_of(dirs[parent].mnt);
            if (m && m->ops->create(m->ctx, dirs[parent].node, name, 1, &node) != 0) {
                g_io_err = 1;
                return -1;
            }
            dirs[i].used       = 1;
            dirs[i].parent_dir = parent;
            dirs[i].mnt        = m ? dirs[parent].mnt : 0;
            dirs[i].node       = node;
            k_strcpy(dirs[i].name, name, FS_NAME_LEN);
            return i;
        }
    }
    return -1;
}

/* ── file data (heap-backed) ────────────────────────────────────── */

/* forgets a file (memory only - see remove_file) */
static void free_file(int i) {
    kfree(files[i].content);
    files[i].content = NULL;
    files[i].size = files[i].cap = 0;
    files[i].used = 0;
    files[i].mnt = 0;
    files[i].loaded = 0;
    files[i].node = 0;
}

/* deletes a file, on its volume too */
static int remove_file(int i) {
    touched();
    mount_t* m = mount_of(files[i].mnt);
    if (m && m->ops->remove(m->ctx, files[i].node, 0) != 0) { g_io_err = 1; return -1; }
    free_file(i);
    return 0;
}

/* makes room for `size` bytes + the NUL terminator */
static int reserve(fs_file_t* f, uint32_t size) {
    if (size > FS_MAX_FILE_SIZE) return -1;
    if (size + 1 <= f->cap) return 0;
    /* grow geometrically so appends (downloads) stay linear overall */
    uint32_t cap = f->cap ? f->cap : 16;
    while (cap < size + 1) cap = (cap < (1u << 20)) ? cap * 2 : cap + (1u << 20);
    char* p = (char*)krealloc(f->content, cap);
    if (!p) return -1;
    f->content = p;
    f->cap = cap;
    return 0;
}

/* a mounted file's data is read from its volume the first time it is used */
static int ensure_loaded(fs_file_t* f) {
    if (!f->used || !f->mnt || f->loaded) return 0;
    mount_t* m = mount_of(f->mnt);
    if (!m || reserve(f, f->size) != 0) return -1;
    if (m->ops->read(m->ctx, f->node, (uint8_t*)f->content, f->size) != 0) {
        g_io_err = 1;
        klog("fs: cannot read %s from its volume\n", f->name);
        return -1;
    }
    f->content[f->size] = '\0';
    f->loaded = 1;
    return 0;
}

static int create_file_in(int parent, const char* name_in) {
    touched();
    char name[FS_NAME_LEN];
    k_strcpy(name, name_in, FS_NAME_LEN);
    if (find_dir_in(parent, name) >= 0) return -1; /* name clash */
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!files[i].used) {
            uint32_t node = 0;
            mount_t* m = mount_of(dirs[parent].mnt);
            if (m && m->ops->create(m->ctx, dirs[parent].node, name, 0, &node) != 0) {
                g_io_err = 1;
                return -1;
            }
            files[i].content = NULL;
            files[i].size = files[i].cap = 0;
            if (reserve(&files[i], 0) != 0) {
                if (m) m->ops->remove(m->ctx, node, 0);
                return -1;
            }
            files[i].content[0] = '\0';
            files[i].used       = 1;
            files[i].parent_dir = parent;
            files[i].mnt        = m ? dirs[parent].mnt : 0;
            files[i].loaded     = 1;
            files[i].node       = node;
            k_strcpy(files[i].name, name, FS_NAME_LEN);
            return i;
        }
    }
    return -1;
}

int fs_write(int idx, const void* data, uint32_t len) {
    touched();
    fs_file_t* f = fs_file_info(idx);
    if (!f || !f->used) return -1;
    if (reserve(f, len) != 0) return -1;
    mount_t* m = mount_of(f->mnt);
    if (m && m->ops->write(m->ctx, f->node, data, len) != 0) {
        g_io_err = 1;
        f->loaded = 0;                  /* whatever is on the volume now: read it again */
        return -1;
    }
    if (len) memmove(f->content, data, len);
    f->size = len;
    f->content[len] = '\0';
    f->loaded = 1;
    /* give back a large buffer when the file shrank a lot */
    if (f->cap > 4096 && f->cap > 4 * (len + 1)) {
        char* p = (char*)kmalloc(len + 1);
        if (p) {
            memcpy(p, f->content, len + 1);
            kfree(f->content);
            f->content = p;
            f->cap = len + 1;
        }
    }
    return 0;
}

int fs_append(int idx, const void* data, uint32_t len) {
    touched();
    fs_file_t* f = fs_file_info(idx);
    if (!f || !f->used) return -1;
    if (ensure_loaded(f) != 0) return -1;
    if (reserve(f, f->size + len) != 0) return -1;
    mount_t* m = mount_of(f->mnt);
    if (m && m->ops->append(m->ctx, f->node, f->size, data, len) != 0) {
        g_io_err = 1;
        f->loaded = 0;
        return -1;
    }
    memcpy(f->content + f->size, data, len);
    f->size += len;
    f->content[f->size] = '\0';
    return 0;
}

int fs_set_text(int idx, const char* text) {
    return fs_write(idx, text, (uint32_t)strlen(text));
}

int fs_is_binary(int idx) {
    fs_file_t* f = fs_get_file(idx);
    if (!f || !f->used) return 0;
    for (uint32_t i = 0; i < f->size; i++)
        if (f->content[i] == '\0') return 1;
    return 0;
}

/* consumes "." / ".." or looks up a real child dir */
static int apply_component(int* cur, const char* name) {
    if (k_strcmp(name, ".") == 0) return 1;
    if (k_strcmp(name, "..") == 0) {
        if (dirs[*cur].parent_dir >= 0) *cur = dirs[*cur].parent_dir;
        return 1;
    }
    int nxt = find_dir_in(*cur, name);
    if (nxt < 0) return 0;
    *cur = nxt;
    return 1;
}

/* Resolves a whole path (every component) to a directory index.
 * "" -> cwd, "/" -> root. Returns -1 if any component doesn't exist. */
static int resolve_dir(const char* path) {
    int cur = (path && path[0] == '/') ? 0 : cwd;
    const char* p = path ? path : "";
    char comp[FS_NAME_LEN];
    while (next_component(&p, comp, sizeof(comp))) {
        if (!apply_component(&cur, comp)) return -1;
    }
    return cur;
}

/* Resolves every component except the last, which is copied verbatim into
 * leaf (it need not exist yet - used for create/find/delete targets). */
static int resolve_parent_leaf(const char* path, char* leaf, int leaf_len) {
    int cur = (path && path[0] == '/') ? 0 : cwd;
    const char* p = path ? path : "";
    char comp[FS_NAME_LEN];
    char pending[FS_NAME_LEN];
    int have = 0;

    while (next_component(&p, comp, sizeof(comp))) {
        if (have) {
            if (!apply_component(&cur, pending)) return -1;
        }
        k_strcpy(pending, comp, FS_NAME_LEN);
        have = 1;
    }

    if (!have) { leaf[0] = '\0'; return cur; }
    k_strcpy(leaf, pending, leaf_len);
    return cur;
}

/* 1 if a mount point lies at or below directory idx */
static int has_mount_inside(int idx) {
    for (int m = 1; m <= FS_MAX_MOUNTS; m++) {
        if (!mounts[m].used) continue;
        for (int a = mounts[m].root; a >= 0; a = dirs[a].parent_dir)
            if (a == idx) return 1;
    }
    return 0;
}

/* deletes a folder and everything in it (on its volume too); 0 = ok */
static int delete_dir_recursive(int idx) {
    touched();
    int rc = 0;
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (files[i].used && files[i].parent_dir == idx && remove_file(i) != 0) rc = -1;
    for (int i = 0; i < FS_MAX_DIRS; i++)
        if (dirs[i].used && dirs[i].parent_dir == idx && delete_dir_recursive(i) != 0) rc = -1;
    if (rc) return rc;
    mount_t* m = mount_of(dirs[idx].mnt);
    if (m && m->ops->remove(m->ctx, dirs[idx].node, 1) != 0) { g_io_err = 1; return -1; }
    dirs[idx].used = 0;
    dirs[idx].mnt = 0;
    return 0;
}

/* ── init ────────────────────────────────────────────────────────── */
void fs_init(void) {
    for (int m = 1; m <= FS_MAX_MOUNTS; m++) if (mounts[m].used) fs_unmount(m);
    for (int i = 0; i < FS_MAX_DIRS;  i++) { dirs[i].used = 0; dirs[i].mnt = 0; }
    for (int i = 0; i < FS_MAX_FILES; i++) free_file(i);

    /* create root dir */
    dirs[0].used       = 1;
    dirs[0].parent_dir = -1;
    k_strcpy(dirs[0].name, "/", FS_NAME_LEN);
    cwd = 0;

    /* seed a standard-ish Unix directory hierarchy */
    int bin  = mkdir_in(0, "bin");
    int etc  = mkdir_in(0, "etc");
    int home = mkdir_in(0, "home");
    mkdir_in(0, "usr");
    mkdir_in(0, "var");
    mkdir_in(0, "tmp");
    mkdir_in(0, "dev");
    mkdir_in(0, "root");
    mkdir_in(0, "mnt");                /* USB sticks are mounted here */
    mkdir_in(0, "apps");               /* installed apps (pkg) */
    (void)bin;

    home_dir = mkdir_in(home, "banana");
    mkdir_in(home_dir, "Pictures");    /* user wallpapers go here */
    mkdir_in(home_dir, "Downloads");   /* the browser saves files here */

    int f;
    f = create_file_in(etc, "motd");
    if (f >= 0) fs_set_text(f,
        "Welcome to Banana OS 0.5 - a from-scratch, Unix-like x86 OS.\n");

    f = create_file_in(etc, "hostname");
    if (f >= 0) fs_set_text(f, "banana-os-0.5\n");

    f = create_file_in(etc, "passwd");
    if (f >= 0) fs_set_text(f,
        "root:x:0:0:root:/root:/bin/sh\n"
        "banana:x:1000:1000:banana:/home/banana:/bin/sh\n");

    f = create_file_in(home_dir, "readme.txt");
    if (f >= 0) fs_set_text(f,
        "Home sweet /home/banana.\n"
        "Try: ls -l, pwd, touch, cp, mv, mkdir -p a/b/c, cd ..\n"
        "Networking: ifconfig, ping example.com, curl http://example.com\n"
        "Wallpapers: wget -O ~/Pictures/pic.jpg <url>, then: wallpaper ~/Pictures/pic.jpg\n"
        "USB sticks (FAT32) show up in /mnt/usb - `mount` lists them, `umount` ejects\n"
        "Apps: pkg install app.bpk, pkg list, then type the app's name\n");

    /* real shells start in $HOME, not / */
    cwd = home_dir;
    g_ready = 1;
}

/* ── directory ops ──────────────────────────────────────────────── */
int fs_mkdir(const char* path) {
    char p[FS_PATH_LEN];
    expand_tilde(path, p, sizeof(p));
    char leaf[FS_NAME_LEN];
    g_io_err = 0;
    int parent = resolve_parent_leaf(p, leaf, sizeof(leaf));
    if (parent < 0 || !leaf[0]) return -1;
    return mkdir_in(parent, leaf);
}

int fs_mkdir_p(const char* path) {
    char p[FS_PATH_LEN];
    expand_tilde(path, p, sizeof(p));
    if (!p[0]) return -1;
    g_io_err = 0;

    int cur = (p[0] == '/') ? 0 : cwd;
    const char* q = p;
    char comp[FS_NAME_LEN];

    while (next_component(&q, comp, sizeof(comp))) {
        if (k_strcmp(comp, ".") == 0) continue;
        if (k_strcmp(comp, "..") == 0) {
            if (dirs[cur].parent_dir >= 0) cur = dirs[cur].parent_dir;
            continue;
        }
        int nxt = find_dir_in(cur, comp);
        if (nxt < 0) {
            if (find_file_in(cur, comp) >= 0) return -1; /* name clash */
            nxt = mkdir_in(cur, comp);
            if (nxt < 0) return -1;
        }
        cur = nxt;
    }
    return cur;
}

int fs_find_dir(const char* path) {
    char p[FS_PATH_LEN];
    expand_tilde(path, p, sizeof(p));
    return resolve_dir(p);
}

static void print_perm_size(int is_dir, uint32_t size) {
    terminal_write(is_dir ? "drwxr-xr-x  " : "-rw-r--r--  ");
    terminal_write("banana  ");
    char b[16];
    char* s = u32_to_str(size, b, sizeof(b));
    int l = k_strlen(s);
    for (int i = l; i < 8; i++) terminal_putchar(' ');
    terminal_write(s);
    terminal_write("  ");
}

void fs_ls(const char* path) {
    char p[FS_PATH_LEN];
    expand_tilde(path, p, sizeof(p));
    int target = (p[0]) ? resolve_dir(p) : cwd;
    if (target < 0) {
        terminal_write_color("ls: no such directory: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        terminal_writeln(path);
        return;
    }
    int found = 0;
    for (int i = 0; i < FS_MAX_DIRS; i++) {
        if (dirs[i].used && dirs[i].parent_dir == target) {
            terminal_write_color(dirs[i].name, VGA_COLOR_LIGHT_BLUE, VGA_COLOR_BLACK);
            terminal_write("/  ");
            found = 1;
        }
    }
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used && files[i].parent_dir == target) {
            terminal_write(files[i].name);
            terminal_write("  ");
            found = 1;
        }
    }
    if (found) terminal_putchar('\n');
    else terminal_writeln("(empty)");
}

void fs_ls_long(const char* path) {
    char p[FS_PATH_LEN];
    expand_tilde(path, p, sizeof(p));
    int target = (p[0]) ? resolve_dir(p) : cwd;
    if (target < 0) {
        terminal_write_color("ls: no such directory: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        terminal_writeln(path);
        return;
    }
    int any = 0;
    for (int i = 0; i < FS_MAX_DIRS; i++) {
        if (dirs[i].used && dirs[i].parent_dir == target) {
            print_perm_size(1, 4096);
            terminal_write_color(dirs[i].name, VGA_COLOR_LIGHT_BLUE, VGA_COLOR_BLACK);
            terminal_writeln("/");
            any = 1;
        }
    }
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used && files[i].parent_dir == target) {
            print_perm_size(0, files[i].size);
            terminal_writeln(files[i].name);
            any = 1;
        }
    }
    if (!any) terminal_writeln("(empty)");
}

/* ── file ops ───────────────────────────────────────────────────── */
int fs_create(const char* path) {
    char p[FS_PATH_LEN];
    expand_tilde(path, p, sizeof(p));
    char leaf[FS_NAME_LEN];
    g_io_err = 0;
    int parent = resolve_parent_leaf(p, leaf, sizeof(leaf));
    if (parent < 0 || !leaf[0]) return -1;
    int existing = find_file_in(parent, leaf);
    if (existing >= 0) return existing;
    return create_file_in(parent, leaf);
}

int fs_find_file(const char* path) {
    char p[FS_PATH_LEN];
    expand_tilde(path, p, sizeof(p));
    char leaf[FS_NAME_LEN];
    int parent = resolve_parent_leaf(p, leaf, sizeof(leaf));
    if (parent < 0 || !leaf[0]) return -1;
    return find_file_in(parent, leaf);
}

fs_file_t* fs_file_info(int idx) {
    if (idx < 0 || idx >= FS_MAX_FILES) return (void*)0;
    return &files[idx];
}

fs_file_t* fs_get_file(int idx) {
    if (idx < 0 || idx >= FS_MAX_FILES) return (void*)0;
    fs_file_t* f = &files[idx];
    if (f->used && f->mnt && !f->loaded && ensure_loaded(f) != 0) {
        /* unreadable (stick pulled out...): callers get zeros, not garbage */
        if (reserve(f, f->size) != 0) return (void*)0;
        memset(f->content, 0, f->size + 1);
    }
    return f;
}

void fs_delete(const char* path, int recursive) {
    touched();
    char p[FS_PATH_LEN];
    expand_tilde(path, p, sizeof(p));
    char leaf[FS_NAME_LEN];
    g_io_err = 0;
    int parent = resolve_parent_leaf(p, leaf, sizeof(leaf));
    if (parent < 0 || !leaf[0]) {
        terminal_write_color("rm: no such file or directory: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        terminal_writeln(path);
        return;
    }

    int fidx = find_file_in(parent, leaf);
    if (fidx >= 0) {
        if (remove_file(fidx) != 0) {
            terminal_write_color("rm: cannot delete it on its volume: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            terminal_writeln(path);
        }
        return;
    }

    int didx = find_dir_in(parent, leaf);
    if (didx >= 0) {
        if (didx == 0) {
            terminal_write_color("rm: refusing to remove /\n", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            return;
        }
        int a = cwd;
        while (a >= 0) {
            if (a == didx) {
                terminal_write_color(
                    "rm: cannot remove current working directory (or an ancestor of it)\n",
                    VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
                return;
            }
            a = dirs[a].parent_dir;
        }
        if (has_mount_inside(didx)) {
            terminal_write_color("rm: a USB stick is mounted there - `umount` it first: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            terminal_writeln(path);
            return;
        }
        if (!recursive) {
            terminal_write_color("rm: is a directory (use -r): ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            terminal_writeln(path);
            return;
        }
        if (delete_dir_recursive(didx) != 0) {
            terminal_write_color("rm: some of it could not be deleted on its volume: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            terminal_writeln(path);
        }
        return;
    }

    terminal_write_color("rm: not found: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    terminal_writeln(path);
}

/* Resolves a cp/mv destination: if it names an existing directory, the
 * target is <that dir>/<basename>; otherwise it's used as a literal path. */
static int resolve_target(const char* dst, const char* basename,
                           int* out_dir, char* out_name, int out_name_len) {
    int ddir = resolve_dir(dst);
    if (ddir >= 0) {
        *out_dir = ddir;
        k_strcpy(out_name, basename, out_name_len);
        return 1;
    }
    char dleaf[FS_NAME_LEN];
    int dparent = resolve_parent_leaf(dst, dleaf, sizeof(dleaf));
    if (dparent < 0 || !dleaf[0]) return 0;
    *out_dir = dparent;
    k_strcpy(out_name, dleaf, out_name_len);
    return 1;
}

int fs_copy(const char* src, const char* dst) {
    char sp[FS_PATH_LEN], dp[FS_PATH_LEN];
    expand_tilde(src, sp, sizeof(sp));
    expand_tilde(dst, dp, sizeof(dp));
    g_io_err = 0;

    char sleaf[FS_NAME_LEN];
    int sparent = resolve_parent_leaf(sp, sleaf, sizeof(sleaf));
    if (sparent < 0 || !sleaf[0]) return -1;
    int sidx = find_file_in(sparent, sleaf);
    if (sidx < 0) return -1;

    int target_dir; char target_name[FS_NAME_LEN];
    if (!resolve_target(dp, sleaf, &target_dir, target_name, sizeof(target_name)))
        return -1;

    int existing = find_file_in(target_dir, target_name);
    if (existing == sidx) return sidx;   /* cp a a */
    if (ensure_loaded(&files[sidx]) != 0) return -1;
    int tidx = (existing >= 0) ? existing : create_file_in(target_dir, target_name);
    if (tidx < 0) return -1;

    if (fs_write(tidx, files[sidx].content, files[sidx].size) != 0) return -1;
    return tidx;
}

int fs_move(const char* src, const char* dst) {
    touched();
    char sp[FS_PATH_LEN], dp[FS_PATH_LEN];
    expand_tilde(src, sp, sizeof(sp));
    expand_tilde(dst, dp, sizeof(dp));
    g_io_err = 0;

    char sleaf[FS_NAME_LEN];
    int sparent = resolve_parent_leaf(sp, sleaf, sizeof(sleaf));
    if (sparent < 0 || !sleaf[0]) return -1;

    int sfile = find_file_in(sparent, sleaf);
    int sdir  = (sfile < 0) ? find_dir_in(sparent, sleaf) : -1;
    if (sfile < 0 && sdir < 0) return -1;

    int target_dir; char target_name[FS_NAME_LEN];
    if (!resolve_target(dp, sleaf, &target_dir, target_name, sizeof(target_name)))
        return -1;

    if (sfile >= 0) {
        int existing = find_file_in(target_dir, target_name);
        if (existing == sfile) return sfile;
        if (find_dir_in(target_dir, target_name) >= 0) return -1;
        if (files[sfile].mnt != dirs[target_dir].mnt) {
            /* onto / off a USB stick: copy, then delete the original */
            if (ensure_loaded(&files[sfile]) != 0) return -1;
            int tidx = existing >= 0 ? existing : create_file_in(target_dir, target_name);
            if (tidx < 0) return -1;
            if (fs_write(tidx, files[sfile].content, files[sfile].size) != 0) return -1;
            if (remove_file(sfile) != 0) return -1;
            return tidx;
        }
        if (existing >= 0 && remove_file(existing) != 0) return -1;
        mount_t* m = mount_of(files[sfile].mnt);
        if (m && m->ops->rename(m->ctx, files[sfile].node, 0, dirs[target_dir].node, target_name) != 0) {
            g_io_err = 1;
            return -1;
        }
        files[sfile].parent_dir = target_dir;
        k_strcpy(files[sfile].name, target_name, FS_NAME_LEN);
        return sfile;
    }

    /* moving a directory: refuse creating a cycle (into itself/descendant) */
    int a = target_dir;
    while (a >= 0) {
        if (a == sdir) return -1;
        a = dirs[a].parent_dir;
    }
    /* folders stay on their volume, and mount points stay put */
    if (dirs[sdir].mnt != dirs[target_dir].mnt || has_mount_inside(sdir)) return -1;
    int existing = find_dir_in(target_dir, target_name);
    if (existing >= 0 && existing != sdir) return -1; /* don't clobber a dir */
    if (find_file_in(target_dir, target_name) >= 0) return -1;
    mount_t* m = mount_of(dirs[sdir].mnt);
    if (m && m->ops->rename(m->ctx, dirs[sdir].node, 1, dirs[target_dir].node, target_name) != 0) {
        g_io_err = 1;
        return -1;
    }
    dirs[sdir].parent_dir = target_dir;
    k_strcpy(dirs[sdir].name, target_name, FS_NAME_LEN);
    return sdir;
}

/* ── navigation ─────────────────────────────────────────────────── */
void fs_cd(const char* path) {
    if (!path || !*path || k_strcmp(path, "~") == 0) {
        cwd = home_dir;
        return;
    }
    char p[FS_PATH_LEN];
    expand_tilde(path, p, sizeof(p));
    int idx = resolve_dir(p);
    if (idx < 0) {
        terminal_write_color("cd: no such directory: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        terminal_writeln(path);
        return;
    }
    cwd = idx;
}

static void dir_abs_path(int idx, char* buf, int buflen) {
    char parts[16][FS_NAME_LEN];
    int  depth = 0;
    int  cur   = idx;
    while (cur > 0 && depth < 16) {
        k_strcpy(parts[depth], dirs[cur].name, FS_NAME_LEN);
        depth++;
        cur = dirs[cur].parent_dir;
        if (cur < 0) break;
    }

    int pos = 0;
    if (pos < buflen - 1) buf[pos++] = '/';
    for (int d = depth - 1; d >= 0; d--) {
        int l = (int)k_strlen(parts[d]);
        for (int c = 0; c < l && pos < buflen - 1; c++) buf[pos++] = parts[d][c];
        if (d > 0 && pos < buflen - 1) buf[pos++] = '/';
    }
    buf[pos] = '\0';
}

void fs_cwd_path(char* buf, int buflen) {
    dir_abs_path(cwd, buf, buflen);
}

void fs_dir_path(int idx, char* buf, int buflen) {
    if (idx < 0 || idx >= FS_MAX_DIRS || !dirs[idx].used) { k_strcpy(buf, "", buflen); return; }
    dir_abs_path(idx, buf, buflen);
}

/* ── find ───────────────────────────────────────────────────────── */
static void join_path(const char* prefix, const char* name, char* out, int outlen) {
    int pos = 0;
    for (int c = 0; prefix[c] && pos < outlen - 1; c++) out[pos++] = prefix[c];
    if (pos < outlen - 1 && (pos == 0 || out[pos - 1] != '/')) out[pos++] = '/';
    for (int c = 0; name[c] && pos < outlen - 1; c++) out[pos++] = name[c];
    out[pos] = '\0';
}

static void find_recursive(int dir_idx, const char* prefix, const char* filter) {
    for (int i = 0; i < FS_MAX_DIRS; i++) {
        if (dirs[i].used && dirs[i].parent_dir == dir_idx) {
            char path[FS_PATH_LEN];
            join_path(prefix, dirs[i].name, path, sizeof(path));
            if (!filter[0] || k_strstr(dirs[i].name, filter)) {
                terminal_write_color(path, VGA_COLOR_LIGHT_BLUE, VGA_COLOR_BLACK);
                terminal_writeln("/");
            }
            find_recursive(i, path, filter);
        }
    }
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used && files[i].parent_dir == dir_idx) {
            if (!filter[0] || k_strstr(files[i].name, filter)) {
                char path[FS_PATH_LEN];
                join_path(prefix, files[i].name, path, sizeof(path));
                terminal_writeln(path);
            }
        }
    }
}

void fs_find(const char* path, const char* name_filter) {
    char p[FS_PATH_LEN];
    expand_tilde(path, p, sizeof(p));
    int target = p[0] ? resolve_dir(p) : cwd;
    if (target < 0) {
        terminal_write_color("find: no such directory: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        terminal_writeln(path);
        return;
    }
    const char* filt = name_filter ? name_filter : "";
    char base[FS_PATH_LEN];
    dir_abs_path(target, base, sizeof(base));
    if (!filt[0]) {
        terminal_write_color(base, VGA_COLOR_LIGHT_BLUE, VGA_COLOR_BLACK);
        terminal_writeln("/");
    }
    find_recursive(target, base, filt);
}

void fs_pwd(void) {
    char buf[FS_PATH_LEN];
    fs_cwd_path(buf, sizeof(buf));
    terminal_writeln(buf);
}

const char* fs_cwd_name(void) {
    return dirs[cwd].name;
}

uint32_t fs_used_files(void) {
    uint32_t n = 0;
    for (int i = 0; i < FS_MAX_FILES; i++) if (files[i].used) n++;
    return n;
}

uint32_t fs_used_dirs(void) {
    uint32_t n = 0;
    for (int i = 0; i < FS_MAX_DIRS; i++) if (dirs[i].used) n++;
    return n;
}

uint32_t fs_max_files(void) { return FS_MAX_FILES; }
uint32_t fs_max_dirs(void)  { return FS_MAX_DIRS; }


uint32_t fs_ram_used_bytes(void) {
    /* the dirs/files tables are static arrays; file data is on the heap */
    uint32_t used = (uint32_t)sizeof(dirs) + (uint32_t)sizeof(files);
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used && (!files[i].mnt || files[i].loaded)) used += files[i].size;
    }
    return used;
}

int fs_write_path(const char* path, const void* data, uint32_t len) {
    int idx = fs_create(path);
    if (idx < 0) return -1;
    if (fs_write(idx, data, len) != 0) return -1;
    return idx;
}

void fs_file_path(int idx, char* buf, int buflen) {
    fs_file_t* f = fs_file_info(idx);
    if (!f || !f->used) { k_strcpy(buf, "", buflen); return; }
    char dir[FS_PATH_LEN];
    dir_abs_path(f->parent_dir, dir, sizeof(dir));
    join_path(dir, f->name, buf, buflen);
}

int fs_io_error(void) {
    return g_io_err;
}

/* ── mount API ──────────────────────────────────────────────────── */

int fs_mount(const char* path, const fs_mount_ops_t* ops, void* ctx, uint32_t root_node) {
    int id = 0;
    for (int m = 1; m <= FS_MAX_MOUNTS; m++) if (!mounts[m].used) { id = m; break; }
    if (!id) return -1;
    int dir = fs_mkdir_p(path);
    if (dir <= 0 || dirs[dir].mnt) return -1;
    for (int i = 0; i < FS_MAX_DIRS; i++) if (dirs[i].used && dirs[i].parent_dir == dir) return -1;
    for (int i = 0; i < FS_MAX_FILES; i++) if (files[i].used && files[i].parent_dir == dir) return -1;
    mounts[id].used = 1;
    mounts[id].root = dir;
    mounts[id].ops = ops;
    mounts[id].ctx = ctx;
    dir_abs_path(dir, mounts[id].point, FS_PATH_LEN);
    dirs[dir].mnt = (uint16_t)id;
    dirs[dir].node = root_node;
    return id;
}

int fs_mount_root(int mnt) {
    mount_t* m = mount_of(mnt);
    return m ? m->root : -1;
}

int fs_mount_add_dir(int mnt, int parent, const char* name, uint32_t node) {
    if (!mount_of(mnt) || parent < 0 || parent >= FS_MAX_DIRS || dirs[parent].mnt != mnt) return -1;
    if (find_dir_in(parent, name) >= 0 || find_file_in(parent, name) >= 0) return -1;
    for (int i = 1; i < FS_MAX_DIRS; i++) {
        if (dirs[i].used) continue;
        dirs[i].used = 1;
        dirs[i].parent_dir = parent;
        dirs[i].mnt = (uint16_t)mnt;
        dirs[i].node = node;
        k_strcpy(dirs[i].name, name, FS_NAME_LEN);
        return i;
    }
    return -1;
}

int fs_mount_add_file(int mnt, int parent, const char* name, uint32_t node, uint32_t size) {
    if (!mount_of(mnt) || parent < 0 || parent >= FS_MAX_DIRS || dirs[parent].mnt != mnt) return -1;
    if (size > FS_MAX_FILE_SIZE) return -1;
    if (find_dir_in(parent, name) >= 0 || find_file_in(parent, name) >= 0) return -1;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used) continue;
        files[i].used = 1;
        files[i].parent_dir = parent;
        files[i].mnt = (uint16_t)mnt;
        files[i].node = node;
        files[i].loaded = 0;
        files[i].content = NULL;
        files[i].size = size;
        files[i].cap = 0;
        k_strcpy(files[i].name, name, FS_NAME_LEN);
        return i;
    }
    return -1;
}

int fs_unmount(int mnt) {
    mount_t* m = mount_of(mnt);
    if (!m) return 0;
    for (int a = cwd; a >= 0; a = dirs[a].parent_dir)
        if (dirs[a].mnt == mnt) { cwd = home_dir; break; }
    for (int i = 0; i < FS_MAX_FILES; i++) if (files[i].used && files[i].mnt == mnt) free_file(i);
    int root = m->root;
    for (int i = 0; i < FS_MAX_DIRS; i++)
        if (dirs[i].used && dirs[i].mnt == mnt && i != root) { dirs[i].used = 0; dirs[i].mnt = 0; }
    /* the mount point itself goes too (mount created it) */
    dirs[root].used = 0;
    dirs[root].mnt = 0;
    if (m->ops->unmount) m->ops->unmount(m->ctx);
    m->used = 0;
    return 1;
}

int fs_path_mount(const char* path) {
    char p[FS_PATH_LEN];
    expand_tilde(path, p, sizeof(p));
    int d = resolve_dir(p);
    if (d < 0) {
        char leaf[FS_NAME_LEN];
        d = resolve_parent_leaf(p, leaf, sizeof(leaf));
        if (d < 0) return 0;
    }
    return dirs[d].mnt;
}

const char* fs_mount_point(int mnt) {
    mount_t* m = mount_of(mnt);
    return m ? m->point : "";
}

/* ── on-disk persistence ────────────────────────────────────────── */

/* a dirs record on disk (v2, v3): name[FS_NAME_LEN], int32 used, int32 parent */
#define DIR_REC (FS_NAME_LEN + 8u)
/* per used file: name[FS_NAME_LEN], int32 parent, uint32 size, data */
#define FILE_REC_HDR (FS_NAME_LEN + 8u)
#define V2_DIRS 64u

static int file_saved(int i) {
    return files[i].used && !files[i].mnt;
}

uint32_t fs_snapshot_size(void) {
    uint32_t n = 4u /* dir count */ + FS_MAX_DIRS * DIR_REC + 4u /* home_dir */ + 4u /* file count */;
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (file_saved(i)) n += FILE_REC_HDR + files[i].size;
    return n;
}

void fs_snapshot_save(uint8_t* buf) {
    uint32_t off = 0;
    uint32_t nd = FS_MAX_DIRS;
    memcpy(buf + off, &nd, 4);
    off += 4;
    for (int i = 0; i < FS_MAX_DIRS; i++) {
        int32_t used = dirs[i].used && !dirs[i].mnt;
        int32_t parent = dirs[i].parent_dir;
        memset(buf + off, 0, FS_NAME_LEN);
        if (used) memcpy(buf + off, dirs[i].name, FS_NAME_LEN);
        memcpy(buf + off + FS_NAME_LEN, &used, 4);
        memcpy(buf + off + FS_NAME_LEN + 4, &parent, 4);
        off += DIR_REC;
    }
    int32_t hd = (int32_t)home_dir;
    memcpy(buf + off, &hd, 4);
    off += 4;
    uint32_t count = 0;
    for (int i = 0; i < FS_MAX_FILES; i++) if (file_saved(i)) count++;
    memcpy(buf + off, &count, 4);
    off += 4;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!file_saved(i)) continue;
        memcpy(buf + off, files[i].name, FS_NAME_LEN);
        int32_t parent = files[i].parent_dir;
        memcpy(buf + off + FS_NAME_LEN, &parent, 4);
        memcpy(buf + off + FS_NAME_LEN + 4, &files[i].size, 4);
        off += FILE_REC_HDR;
        memcpy(buf + off, files[i].content, files[i].size);
        off += files[i].size;
    }
}

/* drops every mount and file, and marks every dir unused */
static void clear_tree(void) {
    for (int m = 1; m <= FS_MAX_MOUNTS; m++) if (mounts[m].used) fs_unmount(m);
    for (int i = 0; i < FS_MAX_FILES; i++) free_file(i);
    for (int i = 0; i < FS_MAX_DIRS; i++) { dirs[i].used = 0; dirs[i].mnt = 0; dirs[i].node = 0; }
}

/* nd dir records from p */
static void load_dirs(const uint8_t* p, uint32_t nd) {
    for (uint32_t i = 0; i < nd && i < FS_MAX_DIRS; i++) {
        int32_t used, parent;
        memcpy(dirs[i].name, p + i * DIR_REC, FS_NAME_LEN);
        dirs[i].name[FS_NAME_LEN - 1] = '\0';
        memcpy(&used, p + i * DIR_REC + FS_NAME_LEN, 4);
        memcpy(&parent, p + i * DIR_REC + FS_NAME_LEN + 4, 4);
        dirs[i].used = used ? 1 : 0;
        dirs[i].parent_dir = parent;
        if (dirs[i].used && (parent < -1 || parent >= (int32_t)FS_MAX_DIRS)) dirs[i].used = 0;
    }
    dirs[0].used = 1;
    dirs[0].parent_dir = -1;
}

/* Banana OS 0.4's fixed layout: 32 dirs, 64 files of 2 KiB text each */
#define V1_DIRS     32
#define V1_FILES    64
#define V1_CONTENT  2048
typedef struct {
    char    name[FS_NAME_LEN];
    char    content[V1_CONTENT];
    int32_t used;
    int32_t parent_dir;
} v1_file_t;

static int load_v1(const uint8_t* buf, uint32_t len) {
    uint32_t need = V1_DIRS * DIR_REC + V1_FILES * (uint32_t)sizeof(v1_file_t) + 4u;
    if (len < need) return -1;
    clear_tree();
    load_dirs(buf, V1_DIRS);
    const uint8_t* p = buf + V1_DIRS * DIR_REC;
    for (int i = 0; i < V1_FILES; i++) {
        v1_file_t vf;
        memcpy(&vf, p + (uint32_t)i * sizeof(v1_file_t), sizeof(vf));
        if (!vf.used) continue;
        vf.name[FS_NAME_LEN - 1] = '\0';
        vf.content[V1_CONTENT - 1] = '\0';
        int idx = -1;
        for (int j = 0; j < FS_MAX_FILES; j++) if (!files[j].used) { idx = j; break; }
        if (idx < 0) break;
        k_strcpy(files[idx].name, vf.name, FS_NAME_LEN);
        files[idx].parent_dir = vf.parent_dir;
        files[idx].used = 1;
        files[idx].content = NULL;
        files[idx].size = files[idx].cap = 0;
        if (fs_set_text(idx, vf.content) != 0) { files[idx].used = 0; break; }
    }
    int32_t hd;
    memcpy(&hd, p + V1_FILES * sizeof(v1_file_t), 4);
    home_dir = (hd > 0 && hd < FS_MAX_DIRS) ? (int)hd : 0;
    return 0;
}

/* v2 and v3: nd dir records at buf+dir_off, then home_dir, count, files */
static int load_tree(const uint8_t* buf, uint32_t len, uint32_t dir_off, uint32_t nd) {
    if (nd > FS_MAX_DIRS) return -1;
    uint32_t hdr = dir_off + nd * DIR_REC;
    if (len < hdr + 8u) return -1;
    /* validate the whole blob before touching the live tree */
    uint32_t count;
    memcpy(&count, buf + hdr + 4, 4);
    if (count > FS_MAX_FILES) return -1;
    uint32_t off = hdr + 8u;
    for (uint32_t i = 0; i < count; i++) {
        if (off + FILE_REC_HDR > len) return -1;
        uint32_t size;
        memcpy(&size, buf + off + FS_NAME_LEN + 4, 4);
        if (size > FS_MAX_FILE_SIZE || off + FILE_REC_HDR + size > len) return -1;
        off += FILE_REC_HDR + size;
    }

    clear_tree();
    load_dirs(buf + dir_off, nd);
    int32_t hd;
    memcpy(&hd, buf + hdr, 4);
    home_dir = (hd > 0 && hd < FS_MAX_DIRS) ? (int)hd : 0;

    off = hdr + 8u;
    for (uint32_t i = 0; i < count; i++) {
        fs_file_t* f = &files[i];
        memcpy(f->name, buf + off, FS_NAME_LEN);
        f->name[FS_NAME_LEN - 1] = '\0';
        int32_t parent;
        uint32_t size;
        memcpy(&parent, buf + off + FS_NAME_LEN, 4);
        memcpy(&size, buf + off + FS_NAME_LEN + 4, 4);
        off += FILE_REC_HDR;
        f->parent_dir = parent;
        f->used = 1;
        f->content = NULL;
        f->size = f->cap = 0;
        if (fs_write((int)i, buf + off, size) != 0) { f->used = 0; return -1; }
        off += size;
    }
    return 0;
}

int fs_snapshot_load(const uint8_t* buf, uint32_t len, uint32_t version) {
    int rc = -1;
    if (version == FS_SNAPSHOT_V1) rc = load_v1(buf, len);
    else if (version == FS_SNAPSHOT_V2) rc = load_tree(buf, len, 0, V2_DIRS);
    else if (version == FS_SNAPSHOT_V3 && len >= 4) {
        uint32_t nd;
        memcpy(&nd, buf, 4);
        rc = load_tree(buf, len, 4, nd);
    }
    if (rc != 0) return rc;
    /* anything not reachable from / (corrupt parent links) is dropped */
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (files[i].used && (files[i].parent_dir < 0 || files[i].parent_dir >= FS_MAX_DIRS ||
                              !dirs[files[i].parent_dir].used))
            free_file(i);
    cwd = home_dir; /* real shells start in $HOME, same as fs_init() */
    g_ready = 1;
    /* folders newer releases expect (0.4 disks predate ~/Pictures, 0.5 ~/Downloads) */
    if (home_dir > 0 && find_dir_in(home_dir, "Pictures") < 0) mkdir_in(home_dir, "Pictures");
    if (home_dir > 0 && find_dir_in(home_dir, "Downloads") < 0) mkdir_in(home_dir, "Downloads");
    if (find_dir_in(0, "mnt") < 0) mkdir_in(0, "mnt");
    if (find_dir_in(0, "apps") < 0) mkdir_in(0, "apps");
    return 0;
}

int fs_list_files(const char* path, int* out_idx, int max) {
    char p[FS_PATH_LEN];
    expand_tilde(path, p, sizeof(p));
    int dir = resolve_dir(p);
    if (dir < 0) return -1;
    int n = 0;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!files[i].used || files[i].parent_dir != dir) continue;
        if (n < max) out_idx[n] = i;
        n++;
    }
    return n;
}

int fs_list_dirs(const char* path, int* out_idx, int max) {
    char p[FS_PATH_LEN];
    expand_tilde(path, p, sizeof(p));
    int dir = resolve_dir(p);
    if (dir < 0) return -1;
    int n = 0;
    for (int i = 0; i < FS_MAX_DIRS; i++) {
        if (!dirs[i].used || i == dir || dirs[i].parent_dir != dir) continue;
        if (n < max) out_idx[n] = i;
        n++;
    }
    return n;
}

const fs_dir_t* fs_get_dir(int idx) {
    return (idx >= 0 && idx < FS_MAX_DIRS && dirs[idx].used) ? &dirs[idx] : NULL;
}

int fs_is_ready(void) {
    return g_ready;
}
