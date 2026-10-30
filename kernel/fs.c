/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hostname.h"
#include "fs.h"
#include "rtc.h"
#include "terminal.h"
#include "types.h"
#include "kheap.h"
#include "kstring.h"
#include "serial.h"
#include "task.h"
#include "timer.h"

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
#define DHASH 16384
#define FHASH 65536
static int32_t g_dhash[DHASH], g_fhash[FHASH];

/* the current folder: each task's own (kernel/task.c) */
#define cwd (*task_cwd_slot())
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
static int     g_perm_err;    /* the last failure was a permission check (EACCES) */
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

/* the installed disk: what "no mount" (0) is written through to, if set */
static mount_t g_sys;

static mount_t* mount_of(int m) {
    if (m == 0) return g_sys.used ? &g_sys : NULL;
    return (m > 0 && m <= FS_MAX_MOUNTS && mounts[m].used) ? &mounts[m] : NULL;
}

void fs_set_system_backend(const fs_mount_ops_t* ops, void* ctx, uint32_t root_node) {
    g_sys.used = ops != NULL;
    g_sys.ops = ops;
    g_sys.ctx = ctx;
    g_sys.root = 0;
    if (ops && g_dslots > 0) D(0).node = root_node;
}

int fs_has_system_backend(void) { return g_sys.used; }


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
        for (int k = 0; k < CHUNK; k++) { c[k].hnext = -2; c[k].sib = -1; c[k].lnext = -1; }
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

/* ── folders read when used (the ext4 disk) ─────────────────────── */

static int (*g_dir_loader)(int dir, uint32_t node);

void fs_set_dir_loader(int (*load)(int dir, uint32_t node)) { g_dir_loader = load; }

void fs_set_unread(int dir) {
    if (dir >= 0 && dir < g_dslots && D(dir).used && g_dir_loader) D(dir).unread = 1;
}

/* folder d's entries in the tables (read from the disk the first time) */
static void populate(int d) {
    if (d < 0 || d >= g_dslots) return;
    D(d).last_use = timer_ms();
    if (!D(d).unread || !g_dir_loader) return;
    D(d).unread = 0;                                 /* (first: the loader's own lookups must not come back here) */
    if (g_dir_loader(d, D(d).node) != 0) g_io_err = 1;
}

void fs_populate(int dir) { populate(dir); }

static int find_dir_in(int parent, const char* name_in) {
    populate(parent);
    char name[FS_NAME_LEN];
    k_strcpy(name, name_in, FS_NAME_LEN);
    for (int i = g_dhash[name_hash(parent, name) & (DHASH - 1)]; i >= 0; i = D(i).hnext)
        if (D(i).used && D(i).parent_dir == parent && k_strcmp(D(i).name, name) == 0)
            return i;
    return -1;
}

static int find_file_in(int parent, const char* name_in) {
    populate(parent);
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

/* files kept on the installed disk (kernel/fsdisk.c) */
static int (*g_disk_read)(int idx, uint8_t* buf, uint32_t size);
static int (*g_disk_read_range)(int idx, uint32_t off, uint8_t* buf, uint32_t len);
static int (*g_disk_clean)(int idx, uint32_t data_gen);
void fs_set_disk_ops(int (*read)(int idx, uint8_t* buf, uint32_t size),
                     int (*read_range)(int idx, uint32_t off, uint8_t* buf, uint32_t len),
                     int (*clean)(int idx, uint32_t data_gen)) {
    g_disk_read = read;
    g_disk_read_range = read_range;
    g_disk_clean = clean;
}

/* ── hard links ──
 * The names of one file are entries in a ring (lnext), each with its own
 * name and folder, sharing what an inode holds: the data (the same content
 * buffer), its size, the disk's node, the mode, owner and dates. A change
 * made through one name is copied to the others (links_sync); the data is
 * freed with the last name. */
static int in_ring(int i) { return F(i).lnext >= 0 && F(i).lnext != i; }

static void links_sync(int i) {
    if (i < 0 || !in_ring(i)) return;
    const fs_file_t* s = &F(i);
    for (int j = s->lnext, n = 0; j != i && j >= 0 && n < FS_MAX_FILES; j = F(j).lnext, n++) {
        fs_file_t* d = &F(j);
        d->content = s->content;
        d->size = s->size;
        d->cap = s->cap;
        d->loaded = s->loaded;
        d->loading = s->loading;
        d->node = s->node;
        d->data_gen = s->data_gen;
        d->last_use = s->last_use;
        d->mtime = s->mtime;
        d->ctime = s->ctime;
        d->atime = s->atime;
        d->mode = s->mode;
        d->uid = s->uid;
        d->gid = s->gid;
    }
}

/* takes name i out of its ring (the others keep the data) */
static void ring_leave(int i) {
    if (!in_ring(i)) { F(i).lnext = -1; return; }
    int p = F(i).lnext;
    while (F(p).lnext != i) p = F(p).lnext;
    F(p).lnext = F(i).lnext;
    if (F(p).lnext == p) F(p).lnext = -1;
    F(i).lnext = -1;
}

static int same_file(int a, int b) {
    if (a == b) return 1;
    if (!in_ring(a)) return 0;
    for (int j = F(a).lnext; j != a; j = F(j).lnext) if (j == b) return 1;
    return 0;
}

static int file_index(const fs_file_t* f);

/* the file is in use: the last to be dropped from RAM */
static void used_now(fs_file_t* f) {
    f->last_use = timer_ms();
    if (f->lnext >= 0) links_sync(file_index(f));
}

/* bumped by every change to the tree or a file (the disk autosave watches it) */
static uint32_t g_fs_gen = 1;
static void touched(void) { g_fs_gen++; }
static void dir_changed(int d);
uint32_t fs_generation(void) { return g_fs_gen; }

/* ── modes and permissions ──────────────────────────────────────── */

static uint32_t dmode(int d) { return D(d).mode ? D(d).mode : FS_MODE_DIR; }
static uint32_t fmode(int i) { return F(i).mode ? F(i).mode : FS_MODE_FILE; }
static int      is_link(int i) { return (F(i).mode & FS_S_IFMT) == FS_S_IFLNK; }

/* the owner's w bit decides (everything runs as the one user) */
static int dir_writable(int d) {
    if (dmode(d) & 0200u) return 1;
    g_perm_err = 1;
    return 0;
}

static int file_writable(int i) {
    if (fmode(i) & 0200u) return 1;
    g_perm_err = 1;
    return 0;
}

/* a new entry: the default mode, owned like the folder it is made in */
static void init_dir_meta(int i, int parent) {
    D(i).mode = FS_MODE_DIR;
    D(i).uid = parent >= 0 ? D(parent).uid : FS_UID_ROOT;
    D(i).gid = parent >= 0 ? D(parent).gid : FS_UID_ROOT;
    D(i).mtime = D(i).ctime = D(i).atime = rtc_now();
}

static void init_file_meta(int i, int parent) {
    F(i).mode = FS_MODE_FILE;
    F(i).uid = D(parent).uid;
    F(i).gid = D(parent).gid;
    F(i).mtime = F(i).ctime = F(i).atime = rtc_now();
}

/* relatime, ext4's default: a read updates atime only when it is older
 * than the last change, or a day old - not a metadata write per read */
static void read_now(fs_file_t* f) {
    uint32_t now = rtc_now();
    if (!now || f->mnt || g_sys.used) return;          /* (sticks and the ext4 disk: not kept) */
    if (f->atime <= f->mtime || now - f->atime > 86400u) {
        f->atime = now;
        if (f->lnext >= 0) links_sync(file_index(f));
        touched();
    }
}

static int mkdir_in(int parent, const char* name_in) {
    touched();
    char name[FS_NAME_LEN];
    k_strcpy(name, name_in, FS_NAME_LEN);
    if (find_dir_in(parent, name) >= 0)  return -1; /* already exists */
    if (find_file_in(parent, name) >= 0) return -1; /* name clash */
    if (!dir_writable(parent)) return -1;
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
    init_dir_meta(i, parent);
    link_dir(i);
    dir_changed(parent);
    return i;
}

/* the folder's contents changed: its modification date is now */
static void dir_changed(int d) {
    if (d >= 0 && d < g_dslots && D(d).used) D(d).mtime = rtc_now();
}

/* a folder is no more (its contents already gone) */
static void free_dir(int i) {
    dir_changed(D(i).parent_dir);
    unlink_dir(i);
    D(i).used = 0;
    D(i).mnt = 0;
    D(i).node = 0;
    D(i).mode = D(i).uid = D(i).gid = D(i).atime = 0;
    if (i < g_dfree) g_dfree = i;
}

/* ── file data (heap-backed) ────────────────────────────────────── */

/* forgets a file (memory only - see remove_file) */
static void free_file(int i) {
    settle(&F(i));
    unlink_file(i);
    if (i < g_ffree) g_ffree = i;
    if (in_ring(i)) F(i).content = NULL;            /* (the file's other names keep its data) */
    ring_leave(i);
    kfree(F(i).content);
    F(i).content = NULL;
    F(i).size = F(i).cap = 0;
    F(i).used = 0;
    F(i).mnt = 0;
    F(i).loaded = 0;
    F(i).node = 0;
    F(i).pins = 0;
    F(i).mode = F(i).uid = F(i).gid = F(i).atime = 0;
}

/* deletes a file, on its volume too */
static int remove_file(int i) {
    touched();
    if (!dir_writable(F(i).parent_dir)) return -1;
    mount_t* m = mount_of(F(i).mnt);
    int parent = F(i).parent_dir;
    if (m && (m->ops->remove_at ? m->ops->remove_at(m->ctx, D(parent).node, F(i).name, F(i).node, 0)
                                : m->ops->remove(m->ctx, F(i).node, 0)) != 0) { g_io_err = 1; return -1; }
    free_file(i);
    dir_changed(parent);
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
static int load_data(fs_file_t* f);
static int ensure_loaded(fs_file_t* f) {
    int rc = load_data(f);
    if (f->lnext >= 0) links_sync(file_index(f));
    return rc;
}

static int load_data(fs_file_t* f) {
    if (!f->used) return 0;
    settle(f);
    if (f->loaded) return 0;
    mount_t* m = mount_of(f->mnt);
    if (!m && (f->mnt || !g_disk_read)) return -1;
    used_now(f);
    if (reserve(f, f->size) != 0) return -1;
    f->loading = 1;                       /* (the read lets the other tasks run) */
    if (f->lnext >= 0) links_sync(file_index(f));
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
    if (!dir_writable(parent)) return -1;
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
    F(i).pins       = 0;
    used_now(&F(i));
    k_strcpy(F(i).name, name, FS_NAME_LEN);
    init_file_meta(i, parent);
    link_file(i);
    dir_changed(parent);
    return i;
}

/* A caller may pass bytes from this file's own cache. Remember the offset
 * before reserve can move that cache, and reject ranges outside its contents. */
static int cached_input(fs_file_t* f, const void* data, uint32_t len, uint32_t* off) {
    uintptr_t base = (uintptr_t)f->content, ptr = (uintptr_t)data;
    if (!base || ptr < base || ptr - base >= f->cap) return 0;
    uintptr_t delta = ptr - base;
    if (delta > f->size || len > f->size - delta) return -1;
    *off = (uint32_t)delta;
    return 1;
}

static int data_write(int idx, const void* data, uint32_t len);
/* (every name of the file sees the change) */
int fs_write(int idx, const void* data, uint32_t len) {
    fs_pin(idx);
    int rc = data_write(idx, data, len);
    if (idx >= 0 && idx < g_fslots) links_sync(idx);
    fs_unpin(idx);
    return rc;
}

static int data_write(int idx, const void* data, uint32_t len) {
    touched();
    fs_file_t* f = fs_file_info(idx);
    if (!f || !f->used) return -1;
    g_perm_err = 0;
    if (!file_writable(idx)) return -1;
    settle(f);
    used_now(f);
    uint32_t input_off = 0;
    int alias = cached_input(f, data, len, &input_off);
    if (alias < 0 || reserve(f, len) != 0) return -1;
    if (alias) data = f->content + input_off;
    links_sync(idx);                    /* (the buffer may have moved: the other names too, before the disk lets others run) */
    f->data_gen = g_fs_gen;
    f->mtime = rtc_now();
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

static int  droppable(int i);
static void drop(int i);

/* a file growing past this by appends (a download) on a volume that writes
 * at an offset (the ext4 disk) leaves RAM: the rest goes to the disk only,
 * where its whole copy in RAM - grown again and again - capped the size of
 * what could be downloaded by the free memory */
#define APPEND_STREAM_MIN (1u << 20)

static int data_append(int idx, const void* data, uint32_t len);
static int buffered_append(int idx, const void* data, uint32_t len);
/* (every name of the file sees the change) */
int fs_append(int idx, const void* data, uint32_t len) {
    int rc = data_append(idx, data, len);
    if (idx >= 0 && idx < g_fslots) links_sync(idx);
    return rc;
}

static int data_append(int idx, const void* data, uint32_t len) {
    touched();
    fs_file_t* f = fs_file_info(idx);
    if (!f || !f->used) return -1;
    g_perm_err = 0;
    if (!file_writable(idx)) return -1;
    /* The buffered path must reject the same oversized requests as the
     * streaming path, before a 32-bit addition can wrap. */
    if (f->size > FS_MAX_FILE_SIZE || len > FS_MAX_FILE_SIZE - f->size) return -1;
    used_now(f);
    f->data_gen = g_fs_gen;
    f->mtime = rtc_now();
    mount_t* m = mount_of(f->mnt);
    if (m && m->ops->write_at && f->loaded && f->size + (uint64_t)len > APPEND_STREAM_MIN) {
        settle(f);
        /* Keep an aliased input alive; ordinary downloads still discard the
         * cached copy and append directly to the disk. */
        if (droppable(idx) && !((uintptr_t)data >= (uintptr_t)f->content &&
            (uintptr_t)data - (uintptr_t)f->content < f->cap)) drop(idx);
    }
    if (m && m->ops->write_at && !f->loaded) {
        /* written at the end on the disk: nothing of the file read in */
        settle(f);
        if (f->size + (uint64_t)len > FS_MAX_FILE_SIZE) return -1;
        if (m->ops->write_at(m->ctx, f->node, f->size, data, len) != 0) { g_io_err = 1; return -1; }
        f->size += len;
        return 0;
    }
    fs_pin(idx);
    int rc = buffered_append(idx, data, len);
    fs_unpin(idx);
    return rc;
}

static int buffered_append(int idx, const void* data, uint32_t len) {
    fs_file_t* f = fs_file_info(idx);
    mount_t* m = mount_of(f->mnt);
    if (ensure_loaded(f) != 0) return -1;
    uint32_t input_off = 0;
    int alias = cached_input(f, data, len, &input_off);
    if (alias < 0 || reserve(f, f->size + len) != 0) return -1;
    if (alias) data = f->content + input_off;
    links_sync(idx);
    if (m && m->ops->append(m->ctx, f->node, f->size, data, len) != 0) {
        g_io_err = 1;
        f->loaded = 0;
        return -1;
    }
    memmove(f->content + f->size, data, len);
    f->size += len;
    f->content[f->size] = '\0';
    return 0;
}

static int data_write_at(int idx, uint32_t off, const void* data, uint32_t len);
/* (every name of the file sees the change) */
int fs_write_at(int idx, uint32_t off, const void* data, uint32_t len) {
    fs_file_t* f = fs_file_info(idx);
    /* Keep append's deliberate disk-streaming transition available. */
    if (f && f->used && off == f->size) return fs_append(idx, data, len);
    fs_pin(idx);
    int rc = data_write_at(idx, off, data, len);
    if (idx >= 0 && idx < g_fslots) links_sync(idx);
    fs_unpin(idx);
    return rc;
}

static int data_write_at(int idx, uint32_t off, const void* data, uint32_t len) {
    fs_file_t* f = fs_file_info(idx);
    if (!f || !f->used) return -1;
    if (off == f->size) return fs_append(idx, data, len);
    touched();
    g_perm_err = 0;
    if (!file_writable(idx)) return -1;
    if ((uint64_t)off + len > FS_MAX_FILE_SIZE) return -1;
    settle(f);
    used_now(f);
    f->data_gen = g_fs_gen;
    f->mtime = rtc_now();
    uint32_t input_off = 0;
    int alias = cached_input(f, data, len, &input_off);
    if (alias < 0) return -1;
    uint32_t end = off + len > f->size ? off + len : f->size;
    mount_t* m = mount_of(f->mnt);
    if (m && m->ops->write_at) {
        if (m->ops->write_at(m->ctx, f->node, off, data, len) != 0) { g_io_err = 1; f->loaded = 0; return -1; }
        if (f->loaded) {                               /* the copy in RAM follows */
            if (reserve(f, end) != 0) { kfree(f->content); f->content = NULL; f->cap = 0; f->loaded = 0; }
            else {
                if (alias) data = f->content + input_off;
                if (off > f->size) memset(f->content + f->size, 0, off - f->size);
                memmove(f->content + off, data, len);
                f->content[end] = '\0';
            }
        }
        f->size = end;
        return 0;
    }
    /* in RAM (the live CD), or a volume without it (FAT): the whole file */
    if (ensure_loaded(f) != 0 || reserve(f, end) != 0) return -1;
    if (alias) data = f->content + input_off;
    uint32_t old = f->size;
    if (off > old) memset(f->content + old, 0, off - old);
    memmove(f->content + off, data, len);
    f->content[end] = '\0';
    f->size = end;
    if (m && m->ops->write(m->ctx, f->node, f->content, end) != 0) { g_io_err = 1; f->loaded = 0; return -1; }
    return 0;
}

static int data_truncate(int idx, uint32_t size);
/* (every name of the file sees the change) */
int fs_truncate(int idx, uint32_t size) {
    fs_pin(idx);
    int rc = data_truncate(idx, size);
    if (idx >= 0 && idx < g_fslots) links_sync(idx);
    fs_unpin(idx);
    return rc;
}

static int data_truncate(int idx, uint32_t size) {
    fs_file_t* f = fs_file_info(idx);
    if (!f || !f->used) return -1;
    if (size == f->size) return 0;
    touched();
    g_perm_err = 0;
    if (!file_writable(idx)) return -1;
    if (size > FS_MAX_FILE_SIZE) return -1;
    settle(f);
    used_now(f);
    f->data_gen = g_fs_gen;
    f->mtime = rtc_now();
    mount_t* m = mount_of(f->mnt);
    if (m && m->ops->truncate) {
        if (m->ops->truncate(m->ctx, f->node, size) != 0) { g_io_err = 1; return -1; }
        if (f->loaded) {
            if (reserve(f, size) != 0) { kfree(f->content); f->content = NULL; f->cap = 0; f->loaded = 0; }
            else {
                if (size > f->size) memset(f->content + f->size, 0, size - f->size);
                f->content[size] = '\0';
            }
        }
        f->size = size;
        return 0;
    }
    if (ensure_loaded(f) != 0 || reserve(f, size) != 0) return -1;
    if (size > f->size) memset(f->content + f->size, 0, size - f->size);
    f->content[size] = '\0';
    f->size = size;
    if (m && m->ops->write(m->ctx, f->node, f->content, size) != 0) { g_io_err = 1; f->loaded = 0; return -1; }
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

/* a symbolic link's target, in a fresh buffer (kfree it), or NULL */
static char* link_target(int i) {
    fs_file_t* f = &F(i);
    if (ensure_loaded(f) != 0) return NULL;
    uint32_t n = f->size < FS_PATH_LEN - 1 ? f->size : FS_PATH_LEN - 1;
    char* t = (char*)kmalloc(n + 1);
    if (!t) return NULL;
    memcpy(t, f->content, n);
    t[n] = '\0';
    return t;
}

static int resolve_dir_at(int start, const char* path, int depth);

/* consumes "." / ".." or looks up a real child dir - or a link to one
 * (its target relative to the folder the link is in) */
static int apply_component(int* cur, const char* name, int depth) {
    if (k_strcmp(name, ".") == 0) return 1;
    if (k_strcmp(name, "..") == 0) {
        if (D(*cur).parent_dir >= 0) *cur = D(*cur).parent_dir;
        return 1;
    }
    int nxt = find_dir_in(*cur, name);
    if (nxt < 0) {
        int l = find_file_in(*cur, name);
        if (l < 0 || !is_link(l) || depth >= FS_MAX_LINKS) return 0;
        char* t = link_target(l);
        if (!t) return 0;
        nxt = resolve_dir_at(t[0] == '/' ? 0 : *cur, t, depth + 1);
        kfree(t);
        if (nxt < 0) return 0;
    }
    *cur = nxt;
    return 1;
}

/* Resolves a whole path (every component) from folder start to a
 * directory index. Returns -1 if any component doesn't exist. */
static int resolve_dir_at(int start, const char* path, int depth) {
    int cur = (path && path[0] == '/') ? 0 : start;
    const char* p = path ? path : "";
    char comp[FS_NAME_LEN];
    while (next_component(&p, comp, sizeof(comp))) {
        if (!apply_component(&cur, comp, depth)) return -1;
    }
    return cur;
}

/* "" -> cwd, "/" -> root */
static int resolve_dir(const char* path) {
    return resolve_dir_at(cwd, path, 0);
}

/* Resolves every component except the last, which is copied verbatim into
 * leaf (it need not exist yet - used for create/find/delete targets). */
static int resolve_parent_leaf_at(int start, const char* path, char* leaf, int leaf_len, int depth) {
    int cur = (path && path[0] == '/') ? 0 : start;
    const char* p = path ? path : "";
    char comp[FS_NAME_LEN];
    char pending[FS_NAME_LEN];
    int have = 0;

    while (next_component(&p, comp, sizeof(comp))) {
        if (have) {
            if (!apply_component(&cur, pending, depth)) return -1;
        }
        k_strcpy(pending, comp, FS_NAME_LEN);
        have = 1;
    }

    if (!have) { leaf[0] = '\0'; return cur; }
    k_strcpy(leaf, pending, leaf_len);
    return cur;
}

static int resolve_parent_leaf(const char* path, char* leaf, int leaf_len) {
    return resolve_parent_leaf_at(cwd, path, leaf, leaf_len, 0);
}

/* the file leaf in folder parent names, its links followed (open(), not
 * lstat()): the file's index, or -1 (none, a dangling link, a loop) */
static int follow_file(int parent, const char* leaf) {
    int i = find_file_in(parent, leaf);
    for (int depth = 0; i >= 0 && is_link(i); depth++) {
        if (depth >= FS_MAX_LINKS) return -1;
        char* t = link_target(i);
        if (!t) return -1;
        char lf[FS_NAME_LEN];
        int p = resolve_parent_leaf_at(t[0] == '/' ? 0 : parent, t, lf, sizeof(lf), depth + 1);
        kfree(t);
        if (p < 0 || !lf[0]) return -1;
        i = find_file_in(p, lf);
        parent = p;
    }
    return i;
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
    populate(idx);                                   /* (all of it, from the disk too) */
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
    if (!dir_writable(D(idx).parent_dir)) return -1;
    mount_t* m = mount_of(D(idx).mnt);
    if (m && (m->ops->remove_at ? m->ops->remove_at(m->ctx, D(D(idx).parent_dir).node, D(idx).name, D(idx).node, 1)
                                : m->ops->remove(m->ctx, D(idx).node, 1)) != 0) { g_io_err = 1; return -1; }
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
    init_dir_meta(0, -1);
    cwd = 0;

    /* seed a standard-ish Unix directory hierarchy (owned by root) */
    int bin  = mkdir_in(0, "bin");
    int etc  = mkdir_in(0, "etc");
    int home = mkdir_in(0, "home");
    mkdir_in(0, "usr");
    mkdir_in(0, "var");
    int tmp = mkdir_in(0, "tmp");
    if (tmp > 0) D(tmp).mode = FS_S_IFDIR | 01777u;       /* everyone's, sticky */
    mkdir_in(0, "dev");
    int root = mkdir_in(0, "root");
    if (root > 0) D(root).mode = FS_S_IFDIR | 0700u;
    mkdir_in(0, "mnt");                /* USB sticks are mounted here */
    mkdir_in(0, "apps");               /* installed apps (pkg) */
    (void)bin;

    home_dir = mkdir_in(home, "banana");
    if (home_dir > 0) D(home_dir).uid = D(home_dir).gid = FS_UID_BANANA;   /* (what is made in it: banana's) */
    mkdir_in(home_dir, "Pictures");    /* user wallpapers go here */
    mkdir_in(home_dir, "Downloads");   /* the browser saves files here */

    int f;
    f = create_file_in(etc, "motd");
    if (f >= 0) fs_set_text(f,
        "Welcome to Banana OS 0.6 - a from-scratch, Unix-like x86 OS.\n");

    f = create_file_in(etc, "hostname");
    if (f >= 0) fs_set_text(f, HOSTNAME_DEFAULT "\n");

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
    g_perm_err = 0;
    int parent = resolve_parent_leaf(p, leaf, sizeof(leaf));
    if (parent < 0 || !leaf[0]) return -1;
    return mkdir_in(parent, leaf);
}

int fs_mkdir_p(const char* path) {
    char p[FS_PATH_LEN];
    expand_tilde(path, p, sizeof(p));
    if (!p[0]) return -1;
    g_io_err = 0;
    g_perm_err = 0;

    int cur = (p[0] == '/') ? 0 : cwd;
    const char* q = p;
    char comp[FS_NAME_LEN];

    while (next_component(&q, comp, sizeof(comp))) {
        if (k_strcmp(comp, ".") == 0) continue;
        if (k_strcmp(comp, "..") == 0) {
            if (D(cur).parent_dir >= 0) cur = D(cur).parent_dir;
            continue;
        }
        int nxt = find_dir_in(cur, comp), via = cur;
        if (nxt < 0 && apply_component(&via, comp, 0)) nxt = via;   /* a link to a folder */
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

/* "drwxr-xr-x" */
static void mode_string(uint32_t mode, char out[11]) {
    uint32_t t = mode & FS_S_IFMT;
    out[0] = t == FS_S_IFDIR ? 'd' : t == FS_S_IFLNK ? 'l' : '-';
    static const char RWX[] = "rwxrwxrwx";
    for (int k = 0; k < 9; k++) out[1 + k] = (mode & (0400u >> k)) ? RWX[k] : '-';
    if (mode & 04000u) out[3] = (mode & 0100u) ? 's' : 'S';
    if (mode & 02000u) out[6] = (mode & 0010u) ? 's' : 'S';
    if (mode & 01000u) out[9] = (mode & 0001u) ? 't' : 'T';
    out[10] = '\0';
}

/* "root", "banana", or the number */
static void owner_name(uint32_t uid, char* out, int cap) {
    if (uid == FS_UID_ROOT) k_strcpy(out, "root", cap);
    else if (uid == FS_UID_BANANA) k_strcpy(out, "banana", cap);
    else ksnprintf(out, (size_t)cap, "%u", uid);
}

static void print_perm_size(uint32_t mode, uint32_t uid, uint32_t gid, uint32_t size, uint32_t mtime) {
    char ms[11], on[12], gn[12], col[40];
    mode_string(mode, ms);
    owner_name(uid, on, sizeof(on));
    owner_name(gid, gn, sizeof(gn));
    ksnprintf(col, sizeof(col), "%s  %-6s %-6s ", ms, on, gn);
    terminal_write(col);
    char b[16];
    char* s = u32_to_str(size, b, sizeof(b));
    int l = k_strlen(s);
    for (int i = l; i < 8; i++) terminal_putchar(' ');
    terminal_write(s);
    terminal_write("  ");
    /* "Oct 10 16:45", like ls -l */
    static const char* const MON[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    char d[24];
    if (mtime) {
        rtc_datetime_t dt;
        rtc_split_time(mtime, &dt);
        uint32_t now = rtc_now();
        if (now && (mtime > now + 3600u || now - mtime > 183u * 86400u))     /* old (or future): the year */
            ksnprintf(d, sizeof(d), "%s %2u  %4u  ", MON[(dt.month + 11) % 12], dt.day, dt.year);
        else
            ksnprintf(d, sizeof(d), "%s %2u %02u:%02u  ", MON[(dt.month + 11) % 12], dt.day, dt.hour, dt.minute);
    } else {
        k_strcpy(d, "              ", sizeof(d));   /* (no date known) */
    }
    terminal_write(d);
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
    populate(target);
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
    populate(target);
    for (int i = D(target).kid_dir; i >= 0; i = D(i).sib) {
        print_perm_size(dmode(i), D(i).uid, D(i).gid, 4096, D(i).mtime);
        terminal_write_color(D(i).name, VGA_COLOR_LIGHT_BLUE, VGA_COLOR_BLACK);
        terminal_writeln("/");
        any = 1;
    }
    for (int i = D(target).kid_file; i >= 0; i = F(i).sib) {
        print_perm_size(fmode(i), F(i).uid, F(i).gid, F(i).size, F(i).mtime);
        if (is_link(i)) {
            terminal_write_color(F(i).name, VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
            char* t = link_target(i);
            terminal_write(" -> ");
            terminal_writeln(t ? t : "?");
            kfree(t);
        } else {
            terminal_writeln(F(i).name);
        }
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
    g_perm_err = 0;
    int parent = resolve_parent_leaf(p, leaf, sizeof(leaf));
    if (parent < 0 || !leaf[0]) return -1;
    int existing = find_file_in(parent, leaf);
    if (existing >= 0) return is_link(existing) ? follow_file(parent, leaf) : existing;
    return create_file_in(parent, leaf);
}

/* a mode, owner or date change on the ext4 disk too (entries on no mount) */
static void sys_setattr(int is_dir, int idx, uint32_t perms, uint32_t uid, uint32_t gid, uint32_t atime, uint32_t mtime) {
    if (!is_dir) links_sync(idx);                       /* (a file's other names: the same inode) */
    if (!g_sys.used || !g_sys.ops->setattr) return;
    if (is_dir ? D(idx).mnt : F(idx).mnt) return;
    uint32_t node = is_dir ? D(idx).node : F(idx).node;
    if (node && g_sys.ops->setattr(g_sys.ctx, node, is_dir, perms, uid, gid, atime, mtime) != 0) g_io_err = 1;
}

int fs_touch(const char* path) {
    int i = fs_create(path);
    if (i >= 0) { touched(); F(i).mtime = rtc_now(); sys_setattr(0, i, ~0u, ~0u, ~0u, ~0u, F(i).mtime); }
    return i;
}

void fs_set_times(int is_dir, int idx, uint32_t mtime, uint32_t ctime) {
    if (is_dir) {
        if (idx < 0 || idx >= g_dslots) return;
        D(idx).mtime = mtime;
        D(idx).ctime = ctime;
    } else {
        if (idx < 0 || idx >= g_fslots) return;
        F(idx).mtime = mtime;
        F(idx).ctime = ctime;
        links_sync(idx);
    }
}

int fs_find_file(const char* path) {
    char p[FS_PATH_LEN];
    expand_tilde(path, p, sizeof(p));
    char leaf[FS_NAME_LEN];
    int parent = resolve_parent_leaf(p, leaf, sizeof(leaf));
    if (parent < 0 || !leaf[0]) return -1;
    return follow_file(parent, leaf);
}

fs_file_t* fs_file_info(int idx) {
    if (idx < 0 || idx >= g_fslots) return (void*)0;
    return &F(idx);
}

fs_file_t* fs_get_file(int idx) {
    if (idx < 0 || idx >= g_fslots) return (void*)0;
    fs_file_t* f = &F(idx);
    if (f->used) { used_now(f); read_now(f); }
    if (f->used && !f->loaded && ensure_loaded(f) != 0) {
        /* unreadable (stick pulled out...): callers get zeros, not garbage */
        if (reserve(f, f->size) != 0) return (void*)0;
        memset(f->content, 0, f->size + 1);
        links_sync(idx);
    }
    return f;
}

void fs_delete(const char* path, int recursive) {
    touched();
    char p[FS_PATH_LEN];
    expand_tilde(path, p, sizeof(p));
    char leaf[FS_NAME_LEN];
    g_io_err = 0;
    g_perm_err = 0;
    int parent = resolve_parent_leaf(p, leaf, sizeof(leaf));
    if (parent < 0 || !leaf[0]) {
        terminal_write_color("rm: no such file or directory: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        terminal_writeln(path);
        return;
    }

    int fidx = find_file_in(parent, leaf);
    if (fidx >= 0) {
        if (remove_file(fidx) != 0) {
            terminal_write_color(g_perm_err ? "rm: permission denied: " : "rm: cannot delete it on its volume: ",
                                 VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
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
            terminal_write_color(g_perm_err ? "rm: permission denied (some of it is left): "
                                            : "rm: some of it could not be deleted on its volume: ",
                                 VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
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
    g_perm_err = 0;

    char sleaf[FS_NAME_LEN];
    int sparent = resolve_parent_leaf(sp, sleaf, sizeof(sleaf));
    if (sparent < 0 || !sleaf[0]) return -1;
    int sidx = follow_file(sparent, sleaf);             /* cp copies what a link points at */
    if (sidx < 0) return -1;

    int target_dir; char target_name[FS_NAME_LEN];
    if (!resolve_target(dp, sleaf, &target_dir, target_name, sizeof(target_name)))
        return -1;

    int existing = find_file_in(target_dir, target_name);
    if (existing == sidx) return sidx;   /* cp a a */
    /* Destination creation/growth can reclaim cached file data. Hold the
     * source until every read of its buffer and metadata is finished. */
    fs_pin(sidx);
    if (ensure_loaded(&F(sidx)) != 0) { fs_unpin(sidx); return -1; }
    int tidx = (existing >= 0) ? existing : create_file_in(target_dir, target_name);
    if (tidx < 0) { fs_unpin(sidx); return -1; }

    if (fs_write(tidx, F(sidx).content, F(sidx).size) != 0) { fs_unpin(sidx); return -1; }
    if (F(sidx).mtime) { F(tidx).mtime = F(sidx).mtime; links_sync(tidx); }   /* a copy keeps the original's date */
    fs_unpin(sidx);
    return tidx;
}

int fs_move(const char* src, const char* dst) {
    touched();
    char sp[FS_PATH_LEN], dp[FS_PATH_LEN];
    expand_tilde(src, sp, sizeof(sp));
    expand_tilde(dst, dp, sizeof(dp));
    g_io_err = 0;
    g_perm_err = 0;

    char sleaf[FS_NAME_LEN];
    int sparent = resolve_parent_leaf(sp, sleaf, sizeof(sleaf));
    if (sparent < 0 || !sleaf[0]) return -1;

    int sfile = find_file_in(sparent, sleaf);
    int sdir  = (sfile < 0) ? find_dir_in(sparent, sleaf) : -1;
    if (sfile < 0 && sdir < 0) return -1;

    int target_dir; char target_name[FS_NAME_LEN];
    if (!resolve_target(dp, sleaf, &target_dir, target_name, sizeof(target_name)))
        return -1;
    if (!dir_writable(sparent) || !dir_writable(target_dir)) return -1;

    if (sfile >= 0) {
        int existing = find_file_in(target_dir, target_name);
        if (existing >= 0 && same_file(existing, sfile)) return sfile;   /* (two names of one file: nothing to do) */
        if (find_dir_in(target_dir, target_name) >= 0) return -1;
        if (F(sfile).mnt != D(target_dir).mnt) {
            /* onto / off a USB stick: copy, then delete the original */
            fs_pin(sfile);
            if (ensure_loaded(&F(sfile)) != 0) { fs_unpin(sfile); return -1; }
            int tidx = existing >= 0 ? existing : create_file_in(target_dir, target_name);
            if (tidx < 0) { fs_unpin(sfile); return -1; }
            if (fs_write(tidx, F(sfile).content, F(sfile).size) != 0) { fs_unpin(sfile); return -1; }
            fs_unpin(sfile);
            if (remove_file(sfile) != 0) return -1;
            return tidx;
        }
        if (existing >= 0 && remove_file(existing) != 0) return -1;
        mount_t* m = mount_of(F(sfile).mnt);
        if (m && (m->ops->rename_at ? m->ops->rename_at(m->ctx, D(sparent).node, F(sfile).name, F(sfile).node, 0, D(target_dir).node, target_name)
                                    : m->ops->rename(m->ctx, F(sfile).node, 0, D(target_dir).node, target_name)) != 0) {
            g_io_err = 1;
            return -1;
        }
        unlink_file(sfile);
        F(sfile).parent_dir = target_dir;
        k_strcpy(F(sfile).name, target_name, FS_NAME_LEN);
        link_file(sfile);
        dir_changed(sparent);
        dir_changed(target_dir);
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
    if (m && (m->ops->rename_at ? m->ops->rename_at(m->ctx, D(sparent).node, D(sdir).name, D(sdir).node, 1, D(target_dir).node, target_name)
                                : m->ops->rename(m->ctx, D(sdir).node, 1, D(target_dir).node, target_name)) != 0) {
        g_io_err = 1;
        return -1;
    }
    unlink_dir(sdir);
    D(sdir).parent_dir = target_dir;
    k_strcpy(D(sdir).name, target_name, FS_NAME_LEN);
    link_dir(sdir);
    dir_changed(sparent);
    dir_changed(target_dir);
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

/* Written from the leaf back to the root, each name at its final place
 * (a path longer than buf is cut at the end, as before) - no copy of the
 * names on the stack, however deep the folder. */
static void dir_abs_path(int idx, char* buf, int buflen) {
    if (buflen <= 0) return;
    int total = 0, depth = 0;
    for (int c = idx; c > 0 && depth < FS_MAX_DIRS; c = D(c).parent_dir, depth++)
        total += 1 + (int)k_strlen(D(c).name);
    if (!total) { k_strcpy(buf, "/", buflen); return; }
    int end = total, d = 0;
    for (int c = idx; c > 0 && d < depth; c = D(c).parent_dir, d++) {
        int l = (int)k_strlen(D(c).name), start = end - l;
        for (int k = 0; k < l; k++) if (start + k < buflen - 1) buf[start + k] = D(c).name[k];
        if (start - 1 < buflen - 1) buf[start - 1] = '/';
        end = start - 1;
    }
    buf[total < buflen - 1 ? total : buflen - 1] = '\0';
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

/* path holds the folder's own path (len bytes); each level adds its name
 * and takes it off again - one buffer for the whole walk, not one a level */
static void find_recursive(int dir_idx, char* path, int len, const char* filter) {
    populate(dir_idx);
    for (int i = D(dir_idx).kid_dir; i >= 0; i = D(i).sib) {
        join_path(path, D(i).name, path, FS_PATH_LEN);
        int l2 = (int)k_strlen(path);
        if (!filter[0] || k_strstr(D(i).name, filter)) {
            terminal_write_color(path, VGA_COLOR_LIGHT_BLUE, VGA_COLOR_BLACK);
            terminal_writeln("/");
        }
        find_recursive(i, path, l2, filter);
        path[len] = '\0';
    }
    for (int i = D(dir_idx).kid_file; i >= 0; i = F(i).sib) {
        if (!filter[0] || k_strstr(F(i).name, filter)) {
            join_path(path, F(i).name, path, FS_PATH_LEN);
            terminal_writeln(path);
            path[len] = '\0';
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
    find_recursive(target, base, (int)k_strlen(base), filt);
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
    populate(dir);
    if (D(dir).kid_dir >= 0 || D(dir).kid_file >= 0) return -1;     /* not empty */
    mounts[id].used = 1;
    mounts[id].root = dir;
    mounts[id].ops = ops;
    mounts[id].ctx = ctx;
    dir_abs_path(dir, mounts[id].point, FS_PATH_LEN);
    D(dir).mnt = (uint16_t)id;
    D(dir).node = root_node;
    D(dir).uid = D(dir).gid = FS_UID_BANANA;
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
    D(i).mode = FS_MODE_DIR;
    D(i).uid = D(i).gid = FS_UID_BANANA;      /* (a FAT or exFAT volume has no owners: the user's) */
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
    F(i).mode = FS_MODE_FILE;
    F(i).uid = F(i).gid = FS_UID_BANANA;
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

/* names in the whole-tree snapshots of layouts 1-4 (and of version-5/6 metadata) are 32 bytes */
#define LEGACY_NAME 32u
/* a dirs record on disk (v2, v3): name[LEGACY_NAME], int32 used, int32 parent */
#define DIR_REC (LEGACY_NAME + 8u)
/* per used file: name[LEGACY_NAME], int32 parent, uint32 size, data */
#define FILE_REC_HDR (LEGACY_NAME + 8u)
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
        memset(buf + off, 0, LEGACY_NAME);
        if (used) memcpy(buf + off, D(i).name, LEGACY_NAME);
        memcpy(buf + off + LEGACY_NAME, &used, 4);
        memcpy(buf + off + LEGACY_NAME + 4, &parent, 4);
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
        memcpy(buf + off, F(i).name, LEGACY_NAME);
        int32_t parent = F(i).parent_dir;
        memcpy(buf + off + LEGACY_NAME, &parent, 4);
        memcpy(buf + off + LEGACY_NAME + 4, &F(i).size, 4);
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
    for (int i = 0; i < g_dslots; i++) { D(i).used = 0; D(i).mnt = 0; D(i).node = 0; D(i).mode = D(i).uid = D(i).gid = D(i).atime = 0; }
    g_dfree = 1;
    g_ffree = 0;
}

/* nd dir records from p */
static void load_dirs(const uint8_t* p, uint32_t nd) {
    grow_dirs((int)nd);
    for (uint32_t i = 0; i < nd && (int)i < g_dslots; i++) {
        int32_t used, parent;
        memcpy(D(i).name, p + i * DIR_REC, LEGACY_NAME);
        D(i).name[LEGACY_NAME - 1] = '\0';
        memcpy(&used, p + i * DIR_REC + LEGACY_NAME, 4);
        memcpy(&parent, p + i * DIR_REC + LEGACY_NAME + 4, 4);
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
    char    name[LEGACY_NAME];
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
        vf.name[LEGACY_NAME - 1] = '\0';
        vf.content[V1_CONTENT - 1] = '\0';
        int idx = new_file_slot();
        if (idx < 0) break;
        k_strcpy(F(idx).name, vf.name, LEGACY_NAME);
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
        memcpy(&size, buf + off + LEGACY_NAME + 4, 4);
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
        memcpy(f->name, buf + off, LEGACY_NAME);
        f->name[LEGACY_NAME - 1] = '\0';
        int32_t parent;
        uint32_t size;
        memcpy(&parent, buf + off + LEGACY_NAME, 4);
        memcpy(&size, buf + off + LEGACY_NAME + 4, 4);
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
    f->mtime = f->ctime = 0;
    return 0;
}

void fs_restore_end(int home) {
    home_dir = (home > 0 && home < g_dslots && D(home).used) ? home : 0;
    finish_load();
}

/* 1 if folder d is the home folder or inside it */
static int in_home(int d) {
    for (int n = 0; d > 0 && n < FS_MAX_DIRS; d = D(d).parent_dir, n++)
        if (d == home_dir) return 1;
    return 0;
}

/* after a whole tree was loaded */
static void finish_load(void) {
    /* anything not reachable from / (corrupt parent links) is dropped */
    for (int i = 0; i < g_fslots; i++)
        if (F(i).used && (F(i).parent_dir < 0 || F(i).parent_dir >= g_dslots ||
                              !D(F(i).parent_dir).used))
            free_file(i);
    /* saved before modes and owners (layouts 1-4, metadata 5/6): the
     * defaults - the home folder's things banana's, the rest root's */
    for (int i = 0; i < g_dslots; i++) {
        if (!D(i).used || D(i).mode) continue;
        D(i).mode = FS_MODE_DIR;
        D(i).uid = D(i).gid = (home_dir > 0 && in_home(i)) ? FS_UID_BANANA : FS_UID_ROOT;
        if (!D(i).atime) D(i).atime = D(i).mtime;
    }
    for (int i = 0; i < g_fslots; i++) {
        if (!F(i).used || F(i).mode) continue;
        F(i).mode = FS_MODE_FILE;
        F(i).uid = F(i).gid = (home_dir > 0 && in_home(F(i).parent_dir)) ? FS_UID_BANANA : FS_UID_ROOT;
        if (!F(i).atime) F(i).atime = F(i).mtime;
    }
    if (D(0).mode == FS_MODE_DIR) { D(0).uid = D(0).gid = FS_UID_ROOT; }
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
    populate(dir);
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
    populate(dir);
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

/* ── file data as a cache (see fs.h) ────────────────────────────────── */

/* bigger than this, a file on the disk is read in pieces rather than whole */
#define STREAM_MIN (256u * 1024u)

int fs_read(int idx, uint32_t off, void* buf, uint32_t len) {
    fs_file_t* f = fs_file_info(idx);
    if (!f || !f->used) return -1;
    if (!f->loaded) settle(f);          /* (in RAM: readable even while being saved) */
    if (off >= f->size) return 0;
    if (len > f->size - off) len = f->size - off;
    used_now(f);
    read_now(f);
    mount_t* m = f->loaded ? NULL : mount_of(f->mnt);
    if (m && m->ops->read_at) {
        /* straight from the disk, through its cache: nothing else of it read in */
        f->loading = 1;
        links_sync(idx);
        int rc = m->ops->read_at(m->ctx, f->node, off, buf, len);
        f->loading = 0;
        links_sync(idx);
        return rc == (int)len ? (int)len : -1;
    }
    if (!f->loaded && !f->mnt && g_disk_read_range && f->size > STREAM_MIN) {
        f->loading = 1;                     /* (a write or a delete waits for it) */
        links_sync(idx);
        int rc = g_disk_read_range(idx, off, (uint8_t*)buf, len);
        f->loading = 0;
        links_sync(idx);
        return rc == 0 ? (int)len : -1;
    }
    if (!f->loaded && ensure_loaded(f) != 0) return -1;
    memcpy(buf, f->content + off, len);
    return (int)len;
}

void fs_pin(int idx) {
    fs_file_t* f = fs_file_info(idx);
    if (f && f->used) f->pins++;
}

void fs_unpin(int idx) {
    fs_file_t* f = fs_file_info(idx);
    if (f && f->pins) f->pins--;
}

/* in RAM, and its copy elsewhere is the same: it can be read in again */
static int droppable(int i) {
    fs_file_t* f = &F(i);
    if (!f->used || !f->loaded || f->loading || f->pins || !f->content || f->cap < 4096) return 0;
    for (int j = f->lnext; j >= 0 && j != i; j = F(j).lnext) if (F(j).pins) return 0;
    if (f->mnt || g_sys.used) return mount_of(f->mnt) != NULL;   /* written through: sticks, the ext4 disk */
    return g_disk_clean && g_disk_clean(i, f->data_gen);  /* the installed disk: once saved */
}

#define CACHE_IDLE_MS 60000u                 /* saved file data unused this long leaves RAM */

static void drop(int i) {
    fs_file_t* f = &F(i);
    kfree(f->content);
    f->content = NULL;
    f->cap = 0;
    f->loaded = 0;
    links_sync(i);
}

uint32_t fs_evict(uint32_t want, uint32_t min_idle_ms) {
    uint32_t now = timer_ms(), dropped = 0;
    while (kheap_largest_free() < want) {
        int best = -1;
        uint32_t best_age = 0;
        for (int i = 0; i < g_fslots; i++) {
            if (!F(i).used || !F(i).loaded) continue;
            uint32_t age = now - F(i).last_use;
            if (age < min_idle_ms || (best >= 0 && age <= best_age) || !droppable(i)) continue;
            best = i;
            best_age = age;
        }
        if (best < 0) break;                               /* nothing more to give */
        dropped += F(best).cap;
        drop(best);
    }
    return dropped;
}

uint32_t fs_evictable_bytes(void) {
    uint32_t n = 0;
    for (int i = 0; i < g_fslots; i++) if (F(i).used && F(i).loaded && droppable(i)) n += F(i).cap;
    return n;
}

/* Out of memory (kheap.c, nothing found for an allocation): files not used
 * for a few seconds make room. Not the ones just used: their data may be
 * what the caller is working with right now. */
static uint32_t reclaim(uint32_t need) {
    return fs_evict(need, 3000);
}

void fs_trim(void) {
    static int hooked;
    if (!hooked) { kheap_set_reclaim(reclaim); hooked = 1; }
    /* saved file data nobody used for a minute goes back to the heap, however
     * much is free: RAM holds what is in use, the disk the rest (it is read
     * in again when wanted). Checked every 5 s. */
    static uint32_t last_scan;
    uint32_t t = timer_ms();
    if (t - last_scan >= 5000u) {
        last_scan = t;
        for (int i = 0; i < g_fslots; i++)
            if (F(i).used && F(i).loaded && t - F(i).last_use >= CACHE_IDLE_MS && droppable(i)) drop(i);
    }
    /* below 1/8 of the RAM free: files idle for 10 s go until 1/5 is free */
    uint32_t total = kheap_total_bytes(), free = total - kheap_used_bytes();
    if (free >= total / 8) return;
    uint32_t want = total / 5 - free, now = timer_ms(), got = 0;
    while (got < want) {
        int best = -1;
        uint32_t best_age = 0;
        for (int i = 0; i < g_fslots; i++) {
            if (!F(i).used || !F(i).loaded) continue;
            uint32_t age = now - F(i).last_use;
            if (age < 10000u || (best >= 0 && age <= best_age) || !droppable(i)) continue;
            best = i;
            best_age = age;
        }
        if (best < 0) break;
        got += F(best).cap;
        drop(best);
    }
    if (got) klog("fs: memory low - %u KiB of file data dropped from RAM (still on the disk)\n", got / 1024u);
}

/* ── Unix metadata, symbolic links, checking (see fs.h) ─────────────── */

int fs_perm_error(void) { return g_perm_err; }

int fs_is_symlink(int idx) {
    return idx >= 0 && idx < g_fslots && F(idx).used && is_link(idx);
}

uint32_t fs_file_mode(int idx) { return (idx >= 0 && idx < g_fslots && F(idx).used) ? fmode(idx) : 0; }
uint32_t fs_dir_mode(int idx)  { return (idx >= 0 && idx < g_dslots && D(idx).used) ? dmode(idx) : 0; }

void fs_set_meta(int is_dir, int idx, uint32_t mode, uint32_t uid, uint32_t gid, uint32_t atime) {
    if (is_dir) {
        if (idx < 0 || idx >= g_dslots) return;
        D(idx).mode = FS_S_IFDIR | (mode & 07777u);
        D(idx).uid = uid;
        D(idx).gid = gid;
        D(idx).atime = atime;
    } else {
        if (idx < 0 || idx >= g_fslots) return;
        uint32_t t = mode & FS_S_IFMT;
        F(idx).mode = (t == FS_S_IFLNK ? FS_S_IFLNK : FS_S_IFREG) | (mode & 07777u);
        F(idx).uid = uid;
        F(idx).gid = gid;
        F(idx).atime = atime;
        links_sync(idx);
    }
}

/* what a path names: 1 a folder (*idx), 0 a file, -1 nothing. follow:
 * a link as the last part is followed (stat), else it is the answer (lstat) */
static int lookup(const char* path, int follow, int* idx) {
    char p[FS_PATH_LEN];
    expand_tilde(path, p, sizeof(p));
    char leaf[FS_NAME_LEN];
    int parent = resolve_parent_leaf(p, leaf, sizeof(leaf));
    if (parent < 0) return -1;
    if (!leaf[0] || k_strcmp(leaf, ".") == 0 || k_strcmp(leaf, "..") == 0) {
        int d = resolve_dir(p);
        if (d < 0) return -1;
        *idx = d;
        return 1;
    }
    int f = find_file_in(parent, leaf);
    if (f >= 0) {
        if (!follow || !is_link(f)) { *idx = f; return 0; }
        int d = resolve_dir(p);                       /* a link to a folder */
        if (d >= 0) { *idx = d; return 1; }
        f = follow_file(parent, leaf);
        if (f < 0) return -1;
        *idx = f;
        return 0;
    }
    int d = find_dir_in(parent, leaf);
    if (d < 0) return -1;
    *idx = d;
    return 1;
}

int fs_stat(const char* path, int follow, fs_stat_t* st) {
    int idx, k = lookup(path, follow, &idx);
    if (k < 0) return -1;
    memset(st, 0, sizeof(*st));
    st->index = idx;
    st->is_dir = k;
    if (k) {
        st->mode = dmode(idx);
        st->uid = D(idx).uid; st->gid = D(idx).gid;
        st->size = 4096;
        st->atime = D(idx).atime; st->mtime = D(idx).mtime; st->ctime = D(idx).ctime;
        st->mnt = D(idx).mnt;
    } else {
        st->mode = fmode(idx);
        st->is_link = is_link(idx);
        st->uid = F(idx).uid; st->gid = F(idx).gid;
        st->size = F(idx).size;
        st->atime = F(idx).atime; st->mtime = F(idx).mtime; st->ctime = F(idx).ctime;
        st->mnt = F(idx).mnt;
        st->nlink = fs_nlink(idx);
        st->ino = idx;
        for (int j = F(idx).lnext; j >= 0 && j != idx; j = F(j).lnext) if (j < st->ino) st->ino = j;
    }
    return 0;
}

int fs_chmod(const char* path, uint32_t perms) {
    g_io_err = g_perm_err = 0;
    int idx, k = lookup(path, 1, &idx);
    if (k < 0) return -1;
    if (k ? D(idx).mnt : F(idx).mnt) { g_io_err = 1; return -1; }   /* FAT / exFAT keep no modes */
    touched();
    if (k) D(idx).mode = FS_S_IFDIR | (perms & 07777u);
    else F(idx).mode = (fmode(idx) & FS_S_IFMT) | (perms & 07777u);
    sys_setattr(k, idx, perms & 07777u, ~0u, ~0u, ~0u, ~0u);
    return 0;
}

int fs_chown(const char* path, int uid, int gid) {
    g_io_err = g_perm_err = 0;
    int idx, k = lookup(path, 1, &idx);
    if (k < 0) return -1;
    if (k ? D(idx).mnt : F(idx).mnt) { g_io_err = 1; return -1; }
    touched();
    if (k) {
        if (uid >= 0) D(idx).uid = (uint32_t)uid;
        if (gid >= 0) D(idx).gid = (uint32_t)gid;
    } else {
        if (uid >= 0) F(idx).uid = (uint32_t)uid;
        if (gid >= 0) F(idx).gid = (uint32_t)gid;
    }
    sys_setattr(k, idx, ~0u, uid >= 0 ? (uint32_t)uid : ~0u, gid >= 0 ? (uint32_t)gid : ~0u, ~0u, ~0u);
    return 0;
}

int fs_symlink(const char* target, const char* linkpath) {
    if (!target || !target[0] || k_strlen(target) >= FS_PATH_LEN) return -1;
    char p[FS_PATH_LEN];
    expand_tilde(linkpath, p, sizeof(p));
    char leaf[FS_NAME_LEN];
    g_io_err = g_perm_err = 0;
    int parent = resolve_parent_leaf(p, leaf, sizeof(leaf));
    if (parent < 0 || !leaf[0]) return -1;
    if (find_file_in(parent, leaf) >= 0 || find_dir_in(parent, leaf) >= 0) return -1;
    if (D(parent).mnt) { g_io_err = 1; return -1; }    /* FAT / exFAT have no links */
    if (!dir_writable(parent)) return -1;
    if (g_sys.used && g_sys.ops->symlink) {
        /* the ext4 disk: a link of its own there (a fast link in the inode) */
        uint32_t node = 0;
        if (g_sys.ops->symlink(g_sys.ctx, D(parent).node, leaf, target, &node) != 0) { g_io_err = 1; return -1; }
        touched();
        int i = fs_mount_add_file(0, parent, leaf, node, k_strlen(target));
        if (i < 0) return -1;
        F(i).mode = FS_MODE_LINK;
        F(i).uid = D(parent).uid;
        F(i).gid = D(parent).gid;
        F(i).mtime = F(i).ctime = F(i).atime = rtc_now();
        F(i).data_gen = g_fs_gen;
        dir_changed(parent);
        return i;
    }
    int i = create_file_in(parent, leaf);
    if (i < 0) return -1;
    if (fs_write(i, target, k_strlen(target)) != 0) { remove_file(i); return -1; }
    F(i).mode = FS_MODE_LINK;
    return i;
}

/* the times of its last access and change (touch; ~0u: kept); follow:
 * through a link; 0 or -1 */
int fs_utime(const char* path, int follow, uint32_t atime, uint32_t mtime) {
    g_io_err = g_perm_err = 0;
    int idx, k = lookup(path, follow, &idx);
    if (k < 0) return -1;
    touched();
    if (k) {
        if (atime != ~0u) D(idx).atime = atime;
        if (mtime != ~0u) D(idx).mtime = mtime;
    } else {
        if (atime != ~0u) F(idx).atime = atime;
        if (mtime != ~0u) F(idx).mtime = mtime;
    }
    sys_setattr(k, idx, ~0u, ~0u, ~0u, atime, mtime);
    return 0;
}

/* the same, for an open file (by its index) */
void fs_file_utime(int idx, uint32_t atime, uint32_t mtime) {
    if (idx < 0 || idx >= g_fslots || !F(idx).used) return;
    touched();
    if (atime != ~0u) F(idx).atime = atime;
    if (mtime != ~0u) F(idx).mtime = mtime;
    sys_setattr(0, idx, ~0u, ~0u, ~0u, atime, mtime);
}

/* chmod / chown of an open file (by its index): 0 or -1 */
int fs_file_setattr(int idx, uint32_t perms, uint32_t uid, uint32_t gid) {
    if (idx < 0 || idx >= g_fslots || !F(idx).used) return -1;
    if (F(idx).mnt) { g_io_err = 1; return -1; }
    touched();
    if (perms != ~0u) F(idx).mode = (fmode(idx) & FS_S_IFMT) | (perms & 07777u);
    if (uid != ~0u) F(idx).uid = uid;
    if (gid != ~0u) F(idx).gid = gid;
    sys_setattr(0, idx, perms == ~0u ? ~0u : perms & 07777u, uid, gid, ~0u, ~0u);
    return 0;
}

uint32_t fs_nlink(int idx) {
    if (idx < 0 || idx >= g_fslots || !F(idx).used) return 0;
    uint32_t n = 1;
    for (int j = F(idx).lnext; j >= 0 && j != idx && n < 65000; j = F(j).lnext) n++;
    return n;
}

int fs_link(const char* oldpath, const char* newpath) {
    g_io_err = g_perm_err = 0;
    int src, k = lookup(oldpath, 0, &src);
    if (k < 0) return -2;
    if (k) return -1;                                    /* (no links to folders) */
    char p[FS_PATH_LEN];
    expand_tilde(newpath, p, sizeof(p));
    char leaf[FS_NAME_LEN];
    int parent = resolve_parent_leaf(p, leaf, sizeof(leaf));
    if (parent < 0 || !leaf[0]) return -2;
    if (find_file_in(parent, leaf) >= 0 || find_dir_in(parent, leaf) >= 0) return -17;
    if (F(src).mnt || D(parent).mnt) return F(src).mnt == D(parent).mnt ? -1 : -18;   /* FAT / exFAT: no links */
    if (!dir_writable(parent)) return -13;
    if (fs_nlink(src) >= 65000) return -31;              /* (EMLINK) */
    settle(&F(src));
    if (g_sys.used) {
        /* the ext4 disk: another directory entry for the inode */
        if (!g_sys.ops->link || !F(src).node) return -1;
        if (g_sys.ops->link(g_sys.ctx, D(parent).node, leaf, F(src).node) != 0) { g_io_err = 1; return -5; }
    }
    int i = new_file_slot();
    if (i < 0) return -5;
    touched();
    fs_file_t* f = &F(i);
    k_strcpy(f->name, leaf, FS_NAME_LEN);
    f->used = 1;
    f->parent_dir = parent;
    f->mnt = 0;
    f->pins = 0;
    f->lnext = in_ring(src) ? F(src).lnext : src;
    F(src).lnext = i;
    F(src).ctime = rtc_now();
    links_sync(src);                                     /* (the data and the rest: the file's) */
    link_file(i);
    dir_changed(parent);
    return i;
}

void fs_join_links(int idx) {
    if (idx < 0 || idx >= g_fslots || !F(idx).used || F(idx).mnt || !F(idx).node || in_ring(idx)) return;
    for (int j = 0; j < g_fslots; j++) {
        if (j == idx || !F(j).used || F(j).mnt || F(j).node != F(idx).node) continue;
        F(idx).lnext = in_ring(j) ? F(j).lnext : j;
        F(j).lnext = idx;
        links_sync(j);                                   /* (its data may be read in already) */
        return;
    }
}

int fs_readlink(const char* path, char* out, int cap) {
    int idx;
    if (lookup(path, 0, &idx) != 0 || !is_link(idx)) return -1;
    char* t = link_target(idx);
    if (!t) return -1;
    k_strcpy(out, t, cap);
    int n = (int)k_strlen(out);
    kfree(t);
    return n;
}

/* ── fsck for the tree in RAM ───────────────────────────────────────── */

static void check_say(void (*out)(const char*), int* n, const char* fmt, const char* a, int b) {
    (*n)++;
    if (!out) return;
    char line[FS_NAME_LEN + 96];
    ksnprintf(line, sizeof(line), fmt, a, b);
    out(line);
}

/* the folder lost things go to (fsck -y), made when first needed */
static int lost_found(void) {
    int lf = find_dir_in(0, "lost+found");
    if (lf < 0) lf = mkdir_in(0, "lost+found");
    return lf;
}

int fs_check(int fix, void (*out)(const char* line)) {
    int problems = 0;
    /* 1. every folder hangs from / (a used parent, no loop) */
    for (int i = 1; i < g_dslots; i++) {
        if (!D(i).used) continue;
        int p = D(i).parent_dir, steps = 0, a = i;
        int bad = p < 0 || p >= g_dslots || !D(p).used;
        while (!bad && a > 0) {
            a = D(a).parent_dir;
            if (a < 0 || a >= g_dslots || !D(a).used || ++steps > g_dslots) bad = 1;
        }
        if (!bad) continue;
        check_say(out, &problems, "folder '%s' (#%d) is not reachable from /", D(i).name, i);
        if (fix && !D(i).mnt) {
            int lf = lost_found();
            if (lf > 0 && lf != i) { unlink_dir(i); D(i).parent_dir = lf; link_dir(i); }
        }
    }
    /* 2. every file in a used folder, with a sane size */
    for (int i = 0; i < g_fslots; i++) {
        if (!F(i).used) continue;
        int p = F(i).parent_dir;
        if (p < 0 || p >= g_dslots || !D(p).used) {
            check_say(out, &problems, "file '%s' (#%d) is in no folder", F(i).name, i);
            if (fix && !F(i).mnt) {
                int lf = lost_found();
                if (lf > 0) { unlink_file(i); F(i).parent_dir = lf; link_file(i); }
            }
        }
        if (F(i).size > FS_MAX_FILE_SIZE || (F(i).loaded && F(i).content && F(i).size >= F(i).cap))
            check_say(out, &problems, "file '%s' (#%d) has a size past its data", F(i).name, i);
        if (!F(i).name[0])
            check_say(out, &problems, "file%s #%d has no name", "", i);
        if (is_link(i) && F(i).size == 0)
            check_say(out, &problems, "symbolic link '%s' (#%d) is empty", F(i).name, i);
    }
    /* 3. names are found again, and the folder lists hold every entry once */
    for (int i = 1; i < g_dslots; i++) {
        if (!D(i).used || D(i).parent_dir < 0) continue;
        if (find_dir_in(D(i).parent_dir, D(i).name) != i)
            check_say(out, &problems, "folder '%s' (#%d): its name leads elsewhere (duplicate, or a broken hash chain)", D(i).name, i);
    }
    for (int i = 0; i < g_fslots; i++) {
        if (!F(i).used || F(i).parent_dir < 0) continue;
        if (find_file_in(F(i).parent_dir, F(i).name) != i)
            check_say(out, &problems, "file '%s' (#%d): its name leads elsewhere (duplicate, or a broken hash chain)", F(i).name, i);
    }
    int listed_f = 0, listed_d = 0, used_f = 0, used_d = 0;
    for (int i = 0; i < g_dslots; i++) {
        if (!D(i).used) continue;
        if (i) used_d++;
        int n = 0;
        for (int c = D(i).kid_dir; c >= 0 && n <= g_dslots; c = D(c).sib, n++)
            if (!D(c).used || D(c).parent_dir != i) {
                check_say(out, &problems, "folder '%s' (#%d) lists a folder that is not in it", D(i).name, i);
                break;
            }
        listed_d += n;
        n = 0;
        for (int c = D(i).kid_file; c >= 0 && n <= g_fslots; c = F(c).sib, n++)
            if (!F(c).used || F(c).parent_dir != i) {
                check_say(out, &problems, "folder '%s' (#%d) lists a file that is not in it", D(i).name, i);
                break;
            }
        listed_f += n;
    }
    for (int i = 0; i < g_fslots; i++) if (F(i).used) used_f++;
    if (listed_d != used_d) check_say(out, &problems, "the folder lists are off by %s%d folders", "", used_d - listed_d);
    if (listed_f != used_f) check_say(out, &problems, "the folder lists are off by %s%d files", "", used_f - listed_f);
    if (home_dir < 0 || home_dir >= g_dslots || !D(home_dir).used)
        check_say(out, &problems, "the home folder %s(#%d) is gone", "", home_dir);
    if (fix && problems) {
        relink_all();
        touched();
    }
    return problems;
}

void fs_set_node(int is_dir, int idx, uint32_t node) {
    if (is_dir) { if (idx >= 0 && idx < g_dslots) D(idx).node = node; }
    else if (idx >= 0 && idx < g_fslots) F(idx).node = node;
}

/* ── giving back what nobody looks at (folders read when used) ───── */

uint32_t fs_entries(void) { return fs_used_files() + fs_used_dirs(); }

static int is_mount_point(int d) {
    for (int m = 1; m <= FS_MAX_MOUNTS; m++) if (mounts[m].used && mounts[m].root == d) return 1;
    return 0;
}

/* nothing under folder d in use: no file open, pinned or being read, the
 * current or home folder not in it, no mount, all of it idle */
static int cold(int d, uint32_t now, uint32_t idle, int depth) {
    if (depth > 64 || d == cwd || d == home_dir || D(d).mnt || is_mount_point(d)) return 0;
    if (now - D(d).last_use < idle) return 0;
    for (int i = D(d).kid_file; i >= 0; i = F(i).sib) {
        fs_file_t* f = &F(i);
        if (f->pins || f->loading || now - f->last_use < idle) return 0;
    }
    for (int i = D(d).kid_dir; i >= 0; i = D(i).sib) if (!cold(i, now, idle, depth + 1)) return 0;
    return 1;
}

/* folder d's entries out of the tables (memory only: they stay on the disk) */
static uint32_t forget_children(int d) {
    uint32_t n = 0;
    for (int i = D(d).kid_file, nx; i >= 0; i = nx) { nx = F(i).sib; free_file(i); n++; }
    for (int i = D(d).kid_dir, nx; i >= 0; i = nx) {
        nx = D(i).sib;
        n += forget_children(i);
        unlink_dir(i);
        D(i).used = 0;
        D(i).mnt = 0;
        D(i).node = 0;
        D(i).unread = 0;
        if (i < g_dfree) g_dfree = i;
        n++;
    }
    return n;
}

uint32_t fs_unload_idle(uint32_t idle_ms) {
    if (!g_dir_loader || !g_sys.used) return 0;
    uint32_t now = timer_ms(), n = 0;
    for (int d = 1; d < g_dslots; d++) {
        if (!D(d).used || D(d).unread || D(d).mnt) continue;
        if (D(d).kid_dir < 0 && D(d).kid_file < 0) continue;
        if (!cold(d, now, idle_ms, 0)) continue;
        n += forget_children(d);
        D(d).unread = 1;                            /* read again when used */
    }
    if (n) touched();                               /* (views of the tree list again) */
    return n;
}
