#include "fs.h"
#include "terminal.h"
#include "types.h"
#include "kheap.h"
#include "kstring.h"
#include "serial.h"
#include "task.h"

#define FS_HOME_PATH "/home/banana"

/* The folder and file tables grow a chunk at a time as they fill; a chunk
 * never moves once made, so an entry's address (fs_get_file) stays valid. */
#define CHUNK 256
static fs_dir_t*  g_dch[FS_MAX_DIRS / CHUNK];
static fs_file_t* g_fch[FS_MAX_FILES / CHUNK];
static int        g_dslots, g_fslots;      /* entries made so far */
static int        g_dfree = 1, g_ffree;    /* no free entry below these */
#define D(i) (g_dch[(i) / CHUNK][(i) % CHUNK])
#define F(i) (g_fch[(i) / CHUNK][(i) % CHUNK])

/* Names are found through hash tables keyed by (folder, name), and every
 * folder keeps lists of its subfolders and files: a lookup or a listing
 * costs the same with 50 files as with 50000. */
#define DHASH 4096
#define FHASH 16384
static int32_t g_dhash[DHASH], g_fhash[FHASH];

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
static void clear_tree(void);

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

/* ── the tables ─────────────────────────────────────────────────── */

/* makes entries up to (not including) n: 0, or -1 (limit / no memory) */
static int grow_dirs(int n) {
    while (g_dslots < n) {
        if (g_dslots >= FS_MAX_DIRS) return -1;
        fs_dir_t* c = (fs_dir_t*)kzalloc(CHUNK * sizeof(fs_dir_t));
        if (!c) return -1;
        for (int k = 0; k < CHUNK; k++) {
            c[k].hnext = -2;
            c[k].sib = c[k].kid_dir = c[k].last_dir = c[k].kid_file = c[k].last_file = -1;
        }
        g_dch[g_dslots / CHUNK] = c;
        g_dslots += CHUNK;
    }
    return 0;
}

static int grow_files(int n) {
    while (g_fslots < n) {
        if (g_fslots >= FS_MAX_FILES) return -1;
        fs_file_t* c = (fs_file_t*)kzalloc(CHUNK * sizeof(fs_file_t));
        if (!c) return -1;
        for (int k = 0; k < CHUNK; k++) { c[k].hnext = -2; c[k].sib = -1; }
        g_fch[g_fslots / CHUNK] = c;
        g_fslots += CHUNK;
    }
    return 0;
}

/* a free entry (not marked used yet), or -1 */
static int new_dir_slot(void) {
    for (int i = g_dfree; ; i++) {
        if (i >= g_dslots && grow_dirs(i + 1) != 0) return -1;
        if (!D(i).used) { g_dfree = i; return i; }
    }
}

static int new_file_slot(void) {
    for (int i = g_ffree; ; i++) {
        if (i >= g_fslots && grow_files(i + 1) != 0) return -1;
        if (!F(i).used && !F(i).loading) { g_ffree = i; return i; }
    }
}

static int file_index(const fs_file_t* f) {
    for (int c = 0; c < g_fslots / CHUNK; c++)
        if (f >= g_fch[c] && f < g_fch[c] + CHUNK) return c * CHUNK + (int)(f - g_fch[c]);
    return -1;
}

static uint32_t name_hash(int parent, const char* s) {
    uint32_t h = 2166136261u ^ ((uint32_t)parent * 0x9E3779B1u);
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

/* Adds an entry (used, its name and parent set) to its bucket and to its
 * folder's list (at the end: listings keep the creation order). */
static void link_dir(int i) {
    fs_dir_t* d = &D(i);
    if (d->hnext != -2 || d->parent_dir < 0) return;
    uint32_t b = name_hash(d->parent_dir, d->name) & (DHASH - 1);
    d->hnext = g_dhash[b];
    g_dhash[b] = i;
    d->sib = -1;
    if (d->parent_dir < g_dslots) {
        fs_dir_t* p = &D(d->parent_dir);
        if (p->last_dir >= 0) D(p->last_dir).sib = i; else p->kid_dir = i;
        p->last_dir = i;
    }
}

static void link_file(int i) {
    fs_file_t* f = &F(i);
    if (f->hnext != -2 || f->parent_dir < 0) return;
    uint32_t b = name_hash(f->parent_dir, f->name) & (FHASH - 1);
    f->hnext = g_fhash[b];
    g_fhash[b] = i;
    f->sib = -1;
    if (f->parent_dir < g_dslots) {
        fs_dir_t* p = &D(f->parent_dir);
        if (p->last_file >= 0) F(p->last_file).sib = i; else p->kid_file = i;
        p->last_file = i;
    }
}

/* takes an entry out of both again (before its name or parent change) */
static void unlink_dir(int i) {
    fs_dir_t* d = &D(i);
    if (d->hnext == -2) return;
    int32_t* pp = &g_dhash[name_hash(d->parent_dir, d->name) & (DHASH - 1)];
    while (*pp >= 0 && *pp != i) pp = &D(*pp).hnext;
    if (*pp == i) *pp = d->hnext;
    if (d->parent_dir >= 0 && d->parent_dir < g_dslots) {
        fs_dir_t* p = &D(d->parent_dir);
        for (int c = p->kid_dir, prev = -1; c >= 0; prev = c, c = D(c).sib) {
            if (c != i) continue;
            if (prev >= 0) D(prev).sib = d->sib; else p->kid_dir = d->sib;
            if (p->last_dir == i) p->last_dir = prev;
            break;
        }
    }
    d->hnext = -2;
    d->sib = -1;
}

static void unlink_file(int i) {
    fs_file_t* f = &F(i);
    if (f->hnext == -2) return;
    int32_t* pp = &g_fhash[name_hash(f->parent_dir, f->name) & (FHASH - 1)];
    while (*pp >= 0 && *pp != i) pp = &F(*pp).hnext;
    if (*pp == i) *pp = f->hnext;
    if (f->parent_dir >= 0 && f->parent_dir < g_dslots) {
        fs_dir_t* p = &D(f->parent_dir);
        for (int c = p->kid_file, prev = -1; c >= 0; prev = c, c = F(c).sib) {
            if (c != i) continue;
            if (prev >= 0) F(prev).sib = f->sib; else p->kid_file = f->sib;
            if (p->last_file == i) p->last_file = prev;
            break;
        }
    }
    f->hnext = -2;
    f->sib = -1;
}

/* empty buckets and lists, nothing linked */
static void reset_links(void) {
    memset(g_dhash, 0xFF, sizeof(g_dhash));
    memset(g_fhash, 0xFF, sizeof(g_fhash));
    for (int i = 0; i < g_dslots; i++) {
        fs_dir_t* d = &D(i);
        d->hnext = -2;
        d->sib = d->kid_dir = d->last_dir = d->kid_file = d->last_file = -1;
    }
    for (int i = 0; i < g_fslots; i++) { F(i).hnext = -2; F(i).sib = -1; }
}

/* after a whole tree was put in the tables directly (loading a disk) */
static void relink_all(void) {
    reset_links();
    for (int i = 1; i < g_dslots; i++) if (D(i).used) link_dir(i);
    for (int i = 0; i < g_fslots; i++) if (F(i).used) link_file(i);
}

static int find_dir_in(int parent, const char* name_in) {
    char name[FS_NAME_LEN];
    k_strcpy(name, name_in, FS_NAME_LEN);
    for (int i = g_dhash[name_hash(parent, name) & (DHASH - 1)]; i >= 0; i = D(i).hnext)
        if (D(i).used && D(i).parent_dir == parent && k_strcmp(D(i).name, name) == 0)
            return i;
    return -1;
}

static int find_file_in(int parent, const char* name_in) {
    char name[FS_NAME_LEN];
    k_strcpy(name, name_in, FS_NAME_LEN);
    for (int i = g_fhash[name_hash(parent, name) & (FHASH - 1)]; i >= 0; i = F(i).hnext)
        if (F(i).used && F(i).parent_dir == parent && k_strcmp(F(i).name, name) == 0)
            return i;
    return -1;
}

/* a file's data being read in (a stick, the disk) - another task waits for it */
static void settle(fs_file_t* f) {
    while (f->loading) task_sleep_ms(1);
}

/* reads a file kept on the installed disk (kernel/fsdisk.c) */
static int (*g_disk_read)(int idx, uint8_t* buf, uint32_t size);
void fs_set_disk_reader(int (*read)(int idx, uint8_t* buf, uint32_t size)) { g_disk_read = read; }

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
    int i = new_dir_slot();
    if (i < 0) return -1;                           /* FS_MAX_DIRS folders */
    uint32_t node = 0;
    mount_t* m = mount_of(D(parent).mnt);
    if (m && m->ops->create(m->ctx, D(parent).node, name, 1, &node) != 0) {
        g_io_err = 1;
        return -1;
    }
    D(i).used       = 1;
    D(i).parent_dir = parent;
    D(i).mnt        = m ? D(parent).mnt : 0;
    D(i).node       = node;
    D(i).kid_dir = D(i).last_dir = D(i).kid_file = D(i).last_file = -1;
    k_strcpy(D(i).name, name, FS_NAME_LEN);
    link_dir(i);
    return i;
}

/* a folder is no more (its contents already gone) */
static void free_dir(int i) {
    unlink_dir(i);
    D(i).used = 0;
    D(i).mnt = 0;
    D(i).node = 0;
    if (i < g_dfree) g_dfree = i;
}

/* ── file data (heap-backed) ────────────────────────────────────── */

/* forgets a file (memory only - see remove_file) */
static void free_file(int i) {
    settle(&F(i));
    unlink_file(i);
    if (i < g_ffree) g_ffree = i;
    kfree(F(i).content);
    F(i).content = NULL;
    F(i).size = F(i).cap = 0;
    F(i).used = 0;
    F(i).mnt = 0;
    F(i).loaded = 0;
    F(i).node = 0;
}

/* deletes a file, on its volume too */
static int remove_file(int i) {
    touched();
    mount_t* m = mount_of(F(i).mnt);
    if (m && m->ops->remove(m->ctx, F(i).node, 0) != 0) { g_io_err = 1; return -1; }
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

/* A file on a USB stick or on the installed disk is read in the first time
 * it is used: what is kept is not bounded by the RAM, only what is in use. */
static int ensure_loaded(fs_file_t* f) {
    if (!f->used) return 0;
    settle(f);
    if (f->loaded) return 0;
    mount_t* m = mount_of(f->mnt);
    if (!m && (f->mnt || !g_disk_read)) return -1;
    if (reserve(f, f->size) != 0) return -1;
    f->loading = 1;                       /* (the read lets the other tasks run) */
    int rc = m ? m->ops->read(m->ctx, f->node, (uint8_t*)f->content, f->size)
               : g_disk_read(file_index(f), (uint8_t*)f->content, f->size);
    f->loading = 0;
    if (rc != 0) {
        g_io_err = 1;
        klog("fs: cannot read %s from %s\n", f->name, m ? "its volume" : "the disk");
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
    int i = new_file_slot();
    if (i < 0) return -1;                          /* FS_MAX_FILES files */
    uint32_t node = 0;
    mount_t* m = mount_of(D(parent).mnt);
    if (m && m->ops->create(m->ctx, D(parent).node, name, 0, &node) != 0) {
        g_io_err = 1;
        return -1;
    }
    F(i).content = NULL;
    F(i).size = F(i).cap = 0;
    if (reserve(&F(i), 0) != 0) {
        if (m) m->ops->remove(m->ctx, node, 0);
        return -1;
    }
    F(i).content[0] = '\0';
    F(i).used       = 1;
    F(i).parent_dir = parent;
    F(i).mnt        = m ? D(parent).mnt : 0;
    F(i).loaded     = 1;
    F(i).node       = node;
    F(i).data_gen   = g_fs_gen;
    k_strcpy(F(i).name, name, FS_NAME_LEN);
    link_file(i);
    return i;
}

int fs_write(int idx, const void* data, uint32_t len) {
    touched();
    fs_file_t* f = fs_file_info(idx);
    if (!f || !f->used) return -1;
    settle(f);
    if (reserve(f, len) != 0) return -1;
    f->data_gen = g_fs_gen;
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
    f->data_gen = g_fs_gen;
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
        if (D(*cur).parent_dir >= 0) *cur = D(*cur).parent_dir;
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
        for (int a = mounts[m].root; a >= 0; a = D(a).parent_dir)
            if (a == idx) return 1;
    }
    return 0;
}

/* deletes a folder and everything in it (on its volume too); 0 = ok */
static int delete_dir_recursive(int idx) {
    touched();
    int rc = 0;
    for (int i = D(idx).kid_file, nx; i >= 0; i = nx) {
        nx = F(i).sib;
        if (remove_file(i) != 0) rc = -1;
    }
    for (int i = D(idx).kid_dir, nx; i >= 0; i = nx) {
        nx = D(i).sib;
        if (delete_dir_recursive(i) != 0) rc = -1;
    }
    if (rc) return rc;
    mount_t* m = mount_of(D(idx).mnt);
    if (m && m->ops->remove(m->ctx, D(idx).node, 1) != 0) { g_io_err = 1; return -1; }
    free_dir(idx);
    return 0;
}

/* ── init ────────────────────────────────────────────────────────── */
void fs_init(void) {
    clear_tree();
    if (grow_dirs(1) != 0) return;

    /* create root dir */
    D(0).used       = 1;
    D(0).parent_dir = -1;
    k_strcpy(D(0).name, "/", FS_NAME_LEN);
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
            if (D(cur).parent_dir >= 0) cur = D(cur).parent_dir;
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
    for (int i = D(target).kid_dir; i >= 0; i = D(i).sib) {
        terminal_write_color(D(i).name, VGA_COLOR_LIGHT_BLUE, VGA_COLOR_BLACK);
        terminal_write("/  ");
        found = 1;
    }
    for (int i = D(target).kid_file; i >= 0; i = F(i).sib) {
        terminal_write(F(i).name);
        terminal_write("  ");
        found = 1;
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
    for (int i = D(target).kid_dir; i >= 0; i = D(i).sib) {
        print_perm_size(1, 4096);
        terminal_write_color(D(i).name, VGA_COLOR_LIGHT_BLUE, VGA_COLOR_BLACK);
        terminal_writeln("/");
        any = 1;
    }
    for (int i = D(target).kid_file; i >= 0; i = F(i).sib) {
        print_perm_size(0, F(i).size);
        terminal_writeln(F(i).name);
        any = 1;
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
    if (idx < 0 || idx >= g_fslots) return (void*)0;
    return &F(idx);
}

fs_file_t* fs_get_file(int idx) {
    if (idx < 0 || idx >= g_fslots) return (void*)0;
    fs_file_t* f = &F(idx);
    if (f->used && !f->loaded && ensure_loaded(f) != 0) {
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
            a = D(a).parent_dir;
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
    if (ensure_loaded(&F(sidx)) != 0) return -1;
    int tidx = (existing >= 0) ? existing : create_file_in(target_dir, target_name);
    if (tidx < 0) return -1;

    if (fs_write(tidx, F(sidx).content, F(sidx).size) != 0) return -1;
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
        if (F(sfile).mnt != D(target_dir).mnt) {
            /* onto / off a USB stick: copy, then delete the original */
            if (ensure_loaded(&F(sfile)) != 0) return -1;
            int tidx = existing >= 0 ? existing : create_file_in(target_dir, target_name);
            if (tidx < 0) return -1;
            if (fs_write(tidx, F(sfile).content, F(sfile).size) != 0) return -1;
            if (remove_file(sfile) != 0) return -1;
            return tidx;
        }
        if (existing >= 0 && remove_file(existing) != 0) return -1;
        mount_t* m = mount_of(F(sfile).mnt);
        if (m && m->ops->rename(m->ctx, F(sfile).node, 0, D(target_dir).node, target_name) != 0) {
            g_io_err = 1;
            return -1;
        }
        unlink_file(sfile);
        F(sfile).parent_dir = target_dir;
        k_strcpy(F(sfile).name, target_name, FS_NAME_LEN);
        link_file(sfile);
        return sfile;
    }

    /* moving a directory: refuse creating a cycle (into itself/descendant) */
    int a = target_dir;
    while (a >= 0) {
        if (a == sdir) return -1;
        a = D(a).parent_dir;
    }
    /* folders stay on their volume, and mount points stay put */
    if (D(sdir).mnt != D(target_dir).mnt || has_mount_inside(sdir)) return -1;
    int existing = find_dir_in(target_dir, target_name);
    if (existing >= 0 && existing != sdir) return -1; /* don't clobber a dir */
    if (find_file_in(target_dir, target_name) >= 0) return -1;
    mount_t* m = mount_of(D(sdir).mnt);
    if (m && m->ops->rename(m->ctx, D(sdir).node, 1, D(target_dir).node, target_name) != 0) {
        g_io_err = 1;
        return -1;
    }
    unlink_dir(sdir);
    D(sdir).parent_dir = target_dir;
    k_strcpy(D(sdir).name, target_name, FS_NAME_LEN);
    link_dir(sdir);
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
        k_strcpy(parts[depth], D(cur).name, FS_NAME_LEN);
        depth++;
        cur = D(cur).parent_dir;
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
    if (idx < 0 || idx >= g_dslots || !D(idx).used) { k_strcpy(buf, "", buflen); return; }
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
    for (int i = D(dir_idx).kid_dir; i >= 0; i = D(i).sib) {
        char path[FS_PATH_LEN];
        join_path(prefix, D(i).name, path, sizeof(path));
        if (!filter[0] || k_strstr(D(i).name, filter)) {
            terminal_write_color(path, VGA_COLOR_LIGHT_BLUE, VGA_COLOR_BLACK);
            terminal_writeln("/");
        }
        find_recursive(i, path, filter);
    }
    for (int i = D(dir_idx).kid_file; i >= 0; i = F(i).sib) {
        if (!filter[0] || k_strstr(F(i).name, filter)) {
            char path[FS_PATH_LEN];
            join_path(prefix, F(i).name, path, sizeof(path));
            terminal_writeln(path);
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
    return D(cwd).name;
}

uint32_t fs_used_files(void) {
    uint32_t n = 0;
    for (int i = 0; i < g_fslots; i++) if (F(i).used) n++;
    return n;
}

uint32_t fs_used_dirs(void) {
    uint32_t n = 0;
    for (int i = 0; i < g_dslots; i++) if (D(i).used) n++;
    return n;
}

uint32_t fs_max_files(void) { return FS_MAX_FILES; }
uint32_t fs_max_dirs(void)  { return FS_MAX_DIRS; }


int fs_file_slots(void) { return g_fslots; }
int fs_dir_slots(void)  { return g_dslots; }

uint32_t fs_ram_used_bytes(void) {
    /* the tables, and the data of the files read in (on the heap) */
    uint32_t used = (uint32_t)g_dslots * (uint32_t)sizeof(fs_dir_t) + (uint32_t)g_fslots * (uint32_t)sizeof(fs_file_t) +
                    (uint32_t)sizeof(g_dhash) + (uint32_t)sizeof(g_fhash);
    for (int i = 0; i < g_fslots; i++)
        if (F(i).used && F(i).loaded) used += F(i).cap;
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
    if (dir <= 0 || D(dir).mnt) return -1;
    if (D(dir).kid_dir >= 0 || D(dir).kid_file >= 0) return -1;     /* not empty */
    mounts[id].used = 1;
    mounts[id].root = dir;
    mounts[id].ops = ops;
    mounts[id].ctx = ctx;
    dir_abs_path(dir, mounts[id].point, FS_PATH_LEN);
    D(dir).mnt = (uint16_t)id;
    D(dir).node = root_node;
    return id;
}

int fs_mount_root(int mnt) {
    mount_t* m = mount_of(mnt);
    return m ? m->root : -1;
}

int fs_mount_add_dir(int mnt, int parent, const char* name, uint32_t node) {
    if (!mount_of(mnt) || parent < 0 || parent >= g_dslots || D(parent).mnt != mnt) return -1;
    if (find_dir_in(parent, name) >= 0 || find_file_in(parent, name) >= 0) return -1;
    int i = new_dir_slot();
    if (i < 0) return -1;
    D(i).used = 1;
    D(i).parent_dir = parent;
    D(i).mnt = (uint16_t)mnt;
    D(i).node = node;
    D(i).kid_dir = D(i).last_dir = D(i).kid_file = D(i).last_file = -1;
    k_strcpy(D(i).name, name, FS_NAME_LEN);
    link_dir(i);
    return i;
}

int fs_mount_add_file(int mnt, int parent, const char* name, uint32_t node, uint32_t size) {
    if (!mount_of(mnt) || parent < 0 || parent >= g_dslots || D(parent).mnt != mnt) return -1;
    if (size > FS_MAX_FILE_SIZE) return -1;
    if (find_dir_in(parent, name) >= 0 || find_file_in(parent, name) >= 0) return -1;
    int i = new_file_slot();
    if (i < 0) return -1;
    F(i).used = 1;
    F(i).parent_dir = parent;
    F(i).mnt = (uint16_t)mnt;
    F(i).node = node;
    F(i).loaded = 0;
    F(i).content = NULL;
    F(i).size = size;
    F(i).cap = 0;
    k_strcpy(F(i).name, name, FS_NAME_LEN);
    link_file(i);
    return i;
}

int fs_unmount(int mnt) {
    mount_t* m = mount_of(mnt);
    if (!m) return 0;
    for (int a = cwd; a >= 0; a = D(a).parent_dir)
        if (D(a).mnt == mnt) { cwd = home_dir; break; }
    for (int i = 0; i < g_fslots; i++) if (F(i).used && F(i).mnt == mnt) free_file(i);
    int root = m->root;
    for (int i = 0; i < g_dslots; i++)
        if (D(i).used && D(i).mnt == mnt && i != root) free_dir(i);
    /* the mount point itself goes too (mount created it) */
    free_dir(root);
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
    return D(d).mnt;
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
    return F(i).used && !F(i).mnt;
}

uint32_t fs_snapshot_size(void) {
    uint32_t n = 4u /* dir count */ + (uint32_t)g_dslots * DIR_REC + 4u /* home_dir */ + 4u /* file count */;
    for (int i = 0; i < g_fslots; i++)
        if (file_saved(i)) n += FILE_REC_HDR + F(i).size;
    return n;
}

void fs_snapshot_save(uint8_t* buf) {
    uint32_t off = 0;
    uint32_t nd = (uint32_t)g_dslots;
    memcpy(buf + off, &nd, 4);
    off += 4;
    for (int i = 0; i < g_dslots; i++) {
        int32_t used = D(i).used && !D(i).mnt;
        int32_t parent = D(i).parent_dir;
        memset(buf + off, 0, FS_NAME_LEN);
        if (used) memcpy(buf + off, D(i).name, FS_NAME_LEN);
        memcpy(buf + off + FS_NAME_LEN, &used, 4);
        memcpy(buf + off + FS_NAME_LEN + 4, &parent, 4);
        off += DIR_REC;
    }
    int32_t hd = (int32_t)home_dir;
    memcpy(buf + off, &hd, 4);
    off += 4;
    uint32_t count = 0;
    for (int i = 0; i < g_fslots; i++) if (file_saved(i)) count++;
    memcpy(buf + off, &count, 4);
    off += 4;
    for (int i = 0; i < g_fslots; i++) {
        if (!file_saved(i)) continue;
        memcpy(buf + off, F(i).name, FS_NAME_LEN);
        int32_t parent = F(i).parent_dir;
        memcpy(buf + off + FS_NAME_LEN, &parent, 4);
        memcpy(buf + off + FS_NAME_LEN + 4, &F(i).size, 4);
        off += FILE_REC_HDR;
        if (ensure_loaded(&F(i)) == 0) memcpy(buf + off, F(i).content, F(i).size);
        else memset(buf + off, 0, F(i).size);
        off += F(i).size;
    }
}

/* drops every mount and file, and marks every dir unused */
static void clear_tree(void) {
    for (int m = 1; m <= FS_MAX_MOUNTS; m++) if (mounts[m].used) fs_unmount(m);
    reset_links();
    for (int i = 0; i < g_fslots; i++) free_file(i);
    for (int i = 0; i < g_dslots; i++) { D(i).used = 0; D(i).mnt = 0; D(i).node = 0; }
    g_dfree = 1;
    g_ffree = 0;
}

/* nd dir records from p */
static void load_dirs(const uint8_t* p, uint32_t nd) {
    grow_dirs((int)nd);
    for (uint32_t i = 0; i < nd && (int)i < g_dslots; i++) {
        int32_t used, parent;
        memcpy(D(i).name, p + i * DIR_REC, FS_NAME_LEN);
        D(i).name[FS_NAME_LEN - 1] = '\0';
        memcpy(&used, p + i * DIR_REC + FS_NAME_LEN, 4);
        memcpy(&parent, p + i * DIR_REC + FS_NAME_LEN + 4, 4);
        D(i).used = used ? 1 : 0;
        D(i).parent_dir = parent;
        if (D(i).used && (parent < -1 || parent >= (int32_t)nd)) D(i).used = 0;
    }
    D(0).used = 1;
    D(0).parent_dir = -1;
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
        int idx = new_file_slot();
        if (idx < 0) break;
        k_strcpy(F(idx).name, vf.name, FS_NAME_LEN);
        F(idx).parent_dir = vf.parent_dir;
        F(idx).used = 1;
        F(idx).content = NULL;
        F(idx).size = F(idx).cap = 0;
        if (fs_set_text(idx, vf.content) != 0) { F(idx).used = 0; break; }
    }
    int32_t hd;
    memcpy(&hd, p + V1_FILES * sizeof(v1_file_t), 4);
    home_dir = (hd > 0 && hd < g_dslots) ? (int)hd : 0;
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
    if (grow_files((int)count) != 0) return -1;
    load_dirs(buf + dir_off, nd);
    int32_t hd;
    memcpy(&hd, buf + hdr, 4);
    home_dir = (hd > 0 && hd < g_dslots) ? (int)hd : 0;

    off = hdr + 8u;
    for (uint32_t i = 0; i < count; i++) {
        fs_file_t* f = &F(i);
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

static void finish_load(void);

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
    finish_load();
    return 0;
}

/* ── layout-5 restore (kernel/fsdisk.c) ─────────────────────────── */

int fs_home_dir(void) { return home_dir; }

void fs_restore_begin(void) {
    clear_tree();
    grow_dirs(1);
    D(0).used = 1;
    D(0).parent_dir = -1;
    home_dir = 0;
}

void fs_restore_dir(int idx, const char* name, int parent) {
    if (idx <= 0 || idx >= FS_MAX_DIRS || parent < 0 || parent >= FS_MAX_DIRS) return;
    if (grow_dirs((idx > parent ? idx : parent) + 1) != 0) return;
    k_strcpy(D(idx).name, name, FS_NAME_LEN);
    D(idx).used = 1;
    D(idx).parent_dir = parent;
}

int fs_restore_file(int idx, const char* name, int parent, const void* data, uint32_t size) {
    if (idx < 0 || idx >= FS_MAX_FILES || size > FS_MAX_FILE_SIZE || grow_files(idx + 1) != 0 || F(idx).used) return -1;
    fs_file_t* f = &F(idx);
    k_strcpy(f->name, name, FS_NAME_LEN);
    f->parent_dir = parent;
    f->used = 1;
    f->content = NULL;
    f->size = f->cap = 0;
    f->loaded = 1;
    if (fs_write(idx, data, size) != 0) { f->used = 0; return -1; }
    return 0;
}

int fs_restore_file_lazy(int idx, const char* name, int parent, uint32_t size) {
    if (idx < 0 || idx >= FS_MAX_FILES || size > FS_MAX_FILE_SIZE || grow_files(idx + 1) != 0 || F(idx).used) return -1;
    fs_file_t* f = &F(idx);
    k_strcpy(f->name, name, FS_NAME_LEN);
    f->parent_dir = parent;
    f->used = 1;
    f->content = NULL;
    f->size = size;
    f->cap = 0;
    f->mnt = 0;
    f->node = 0;
    f->loaded = 0;
    f->data_gen = g_fs_gen;
    return 0;
}

void fs_restore_end(int home) {
    home_dir = (home > 0 && home < g_dslots && D(home).used) ? home : 0;
    finish_load();
}

/* after a whole tree was loaded */
static void finish_load(void) {
    /* anything not reachable from / (corrupt parent links) is dropped */
    for (int i = 0; i < g_fslots; i++)
        if (F(i).used && (F(i).parent_dir < 0 || F(i).parent_dir >= g_dslots ||
                              !D(F(i).parent_dir).used))
            free_file(i);
    relink_all();
    cwd = home_dir; /* real shells start in $HOME, same as fs_init() */
    g_ready = 1;
    /* folders newer releases expect (0.4 disks predate ~/Pictures, 0.5 ~/Downloads) */
    if (home_dir > 0 && find_dir_in(home_dir, "Pictures") < 0) mkdir_in(home_dir, "Pictures");
    if (home_dir > 0 && find_dir_in(home_dir, "Downloads") < 0) mkdir_in(home_dir, "Downloads");
    if (find_dir_in(0, "mnt") < 0) mkdir_in(0, "mnt");
    if (find_dir_in(0, "apps") < 0) mkdir_in(0, "apps");
}

int fs_list_files(const char* path, int* out_idx, int max) {
    char p[FS_PATH_LEN];
    expand_tilde(path, p, sizeof(p));
    int dir = resolve_dir(p);
    if (dir < 0) return -1;
    int n = 0;
    for (int i = D(dir).kid_file; i >= 0; i = F(i).sib) {
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
    for (int i = D(dir).kid_dir; i >= 0; i = D(i).sib) {
        if (n < max) out_idx[n] = i;
        n++;
    }
    return n;
}

const fs_dir_t* fs_get_dir(int idx) {
    return (idx >= 0 && idx < g_dslots && D(idx).used) ? &D(idx) : NULL;
}

int fs_is_ready(void) {
    return g_ready;
}
