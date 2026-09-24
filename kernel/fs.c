#include "fs.h"
#include "terminal.h"
#include "types.h"
#include "kheap.h"
#include "kstring.h"

#define FS_HOME_PATH "/home/banana"

static fs_dir_t  dirs[FS_MAX_DIRS];
static fs_file_t files[FS_MAX_FILES];
static int       cwd      = 0;   /* current dir index (0 = root) */
static int       home_dir = 0;   /* dir index of /home/banana */

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

static int mkdir_in(int parent, const char* name) {
    if (find_dir_in(parent, name) >= 0)  return -1; /* already exists */
    if (find_file_in(parent, name) >= 0) return -1; /* name clash */
    for (int i = 1; i < FS_MAX_DIRS; i++) {
        if (!dirs[i].used) {
            dirs[i].used       = 1;
            dirs[i].parent_dir = parent;
            k_strcpy(dirs[i].name, name, FS_NAME_LEN);
            return i;
        }
    }
    return -1;
}

/* ── file data (heap-backed) ────────────────────────────────────── */

static void free_file(int i) {
    kfree(files[i].content);
    files[i].content = NULL;
    files[i].size = files[i].cap = 0;
    files[i].used = 0;
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

static int create_file_in(int parent, const char* name) {
    if (find_dir_in(parent, name) >= 0) return -1; /* name clash */
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!files[i].used) {
            files[i].content = NULL;
            files[i].size = files[i].cap = 0;
            if (reserve(&files[i], 0) != 0) return -1;
            files[i].content[0] = '\0';
            files[i].used       = 1;
            files[i].parent_dir = parent;
            k_strcpy(files[i].name, name, FS_NAME_LEN);
            return i;
        }
    }
    return -1;
}

int fs_write(int idx, const void* data, uint32_t len) {
    fs_file_t* f = fs_get_file(idx);
    if (!f || !f->used) return -1;
    if (reserve(f, len) != 0) return -1;
    if (len) memmove(f->content, data, len);
    f->size = len;
    f->content[len] = '\0';
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
    fs_file_t* f = fs_get_file(idx);
    if (!f || !f->used) return -1;
    if (reserve(f, f->size + len) != 0) return -1;
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

static void delete_dir_recursive(int idx) {
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (files[i].used && files[i].parent_dir == idx) free_file(i);
    for (int i = 0; i < FS_MAX_DIRS; i++)
        if (dirs[i].used && dirs[i].parent_dir == idx) delete_dir_recursive(i);
    dirs[idx].used = 0;
}

/* ── init ────────────────────────────────────────────────────────── */
void fs_init(void) {
    for (int i = 0; i < FS_MAX_DIRS;  i++) dirs[i].used  = 0;
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
    (void)bin;

    home_dir = mkdir_in(home, "banana");
    mkdir_in(home_dir, "Pictures");    /* user wallpapers go here */

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
        "Wallpapers: wget -O ~/Pictures/pic.jpg <url>, then: wallpaper ~/Pictures/pic.jpg\n");

    /* real shells start in $HOME, not / */
    cwd = home_dir;
}

/* ── directory ops ──────────────────────────────────────────────── */
int fs_mkdir(const char* path) {
    char p[FS_PATH_LEN];
    expand_tilde(path, p, sizeof(p));
    char leaf[FS_NAME_LEN];
    int parent = resolve_parent_leaf(p, leaf, sizeof(leaf));
    if (parent < 0 || !leaf[0]) return -1;
    return mkdir_in(parent, leaf);
}

int fs_mkdir_p(const char* path) {
    char p[FS_PATH_LEN];
    expand_tilde(path, p, sizeof(p));
    if (!p[0]) return -1;

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
    for (int i = l; i < 6; i++) terminal_putchar(' ');
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

fs_file_t* fs_get_file(int idx) {
    if (idx < 0 || idx >= FS_MAX_FILES) return (void*)0;
    return &files[idx];
}

void fs_delete(const char* path, int recursive) {
    char p[FS_PATH_LEN];
    expand_tilde(path, p, sizeof(p));
    char leaf[FS_NAME_LEN];
    int parent = resolve_parent_leaf(p, leaf, sizeof(leaf));
    if (parent < 0 || !leaf[0]) {
        terminal_write_color("rm: no such file or directory: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        terminal_writeln(path);
        return;
    }

    int fidx = find_file_in(parent, leaf);
    if (fidx >= 0) { free_file(fidx); return; }

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
        if (!recursive) {
            terminal_write_color("rm: is a directory (use -r): ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            terminal_writeln(path);
            return;
        }
        delete_dir_recursive(didx);
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
    int tidx = (existing >= 0) ? existing : create_file_in(target_dir, target_name);
    if (tidx < 0) return -1;

    if (fs_write(tidx, files[sidx].content, files[sidx].size) != 0) return -1;
    return tidx;
}

int fs_move(const char* src, const char* dst) {
    char sp[FS_PATH_LEN], dp[FS_PATH_LEN];
    expand_tilde(src, sp, sizeof(sp));
    expand_tilde(dst, dp, sizeof(dp));

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
        if (existing >= 0 && existing != sfile) free_file(existing);
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
    int existing = find_dir_in(target_dir, target_name);
    if (existing >= 0 && existing != sdir) return -1; /* don't clobber a dir */
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
    char parts[8][FS_NAME_LEN];
    int  depth = 0;
    int  cur   = idx;
    while (cur != 0 && depth < 8) {
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
        if (files[i].used) used += files[i].size;
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
    fs_file_t* f = fs_get_file(idx);
    if (!f || !f->used) { k_strcpy(buf, "", buflen); return; }
    char dir[FS_PATH_LEN];
    dir_abs_path(f->parent_dir, dir, sizeof(dir));
    join_path(dir, f->name, buf, buflen);
}

/* ── on-disk persistence ────────────────────────────────────────── */

/* v2 record per used file: name[FS_NAME_LEN], int32 parent, uint32 size, data */
#define FILE_REC_HDR (FS_NAME_LEN + 8u)

uint32_t fs_snapshot_size(void) {
    uint32_t n = (uint32_t)sizeof(dirs) + 4u /* home_dir */ + 4u /* file count */;
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (files[i].used) n += FILE_REC_HDR + files[i].size;
    return n;
}

void fs_snapshot_save(uint8_t* buf) {
    uint32_t off = 0;
    memcpy(buf + off, dirs, sizeof(dirs));
    off += sizeof(dirs);
    int32_t hd = (int32_t)home_dir;
    memcpy(buf + off, &hd, 4);
    off += 4;
    uint32_t count = fs_used_files();
    memcpy(buf + off, &count, 4);
    off += 4;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!files[i].used) continue;
        memcpy(buf + off, files[i].name, FS_NAME_LEN);
        int32_t parent = files[i].parent_dir;
        memcpy(buf + off + FS_NAME_LEN, &parent, 4);
        memcpy(buf + off + FS_NAME_LEN + 4, &files[i].size, 4);
        off += FILE_REC_HDR;
        memcpy(buf + off, files[i].content, files[i].size);
        off += files[i].size;
    }
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
    uint32_t need = V1_DIRS * (uint32_t)sizeof(fs_dir_t) + V1_FILES * (uint32_t)sizeof(v1_file_t) + 4u;
    if (len < need) return -1;
    for (int i = 0; i < FS_MAX_DIRS; i++) dirs[i].used = 0;
    for (int i = 0; i < FS_MAX_FILES; i++) free_file(i);
    memcpy(dirs, buf, V1_DIRS * sizeof(fs_dir_t));
    const uint8_t* p = buf + V1_DIRS * sizeof(fs_dir_t);
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

static int load_v2(const uint8_t* buf, uint32_t len) {
    if (len < sizeof(dirs) + 8u) return -1;
    /* validate the whole blob before touching the live tree */
    uint32_t count;
    memcpy(&count, buf + sizeof(dirs) + 4, 4);
    if (count > FS_MAX_FILES) return -1;
    uint32_t off = sizeof(dirs) + 8u;
    for (uint32_t i = 0; i < count; i++) {
        if (off + FILE_REC_HDR > len) return -1;
        uint32_t size;
        memcpy(&size, buf + off + FS_NAME_LEN + 4, 4);
        if (size > FS_MAX_FILE_SIZE || off + FILE_REC_HDR + size > len) return -1;
        off += FILE_REC_HDR + size;
    }

    for (int i = 0; i < FS_MAX_FILES; i++) free_file(i);
    memcpy(dirs, buf, sizeof(dirs));
    int32_t hd;
    memcpy(&hd, buf + sizeof(dirs), 4);
    home_dir = (hd > 0 && hd < FS_MAX_DIRS) ? (int)hd : 0;

    off = sizeof(dirs) + 8u;
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
    int rc = (version == FS_SNAPSHOT_V1) ? load_v1(buf, len)
           : (version == FS_SNAPSHOT_V2) ? load_v2(buf, len) : -1;
    if (rc != 0) return rc;
    /* anything not reachable from / (corrupt parent links) is dropped */
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (files[i].used && (files[i].parent_dir < 0 || files[i].parent_dir >= FS_MAX_DIRS ||
                              !dirs[files[i].parent_dir].used))
            free_file(i);
    cwd = home_dir; /* real shells start in $HOME, same as fs_init() */
    /* disks from 0.4 predate ~/Pictures */
    if (home_dir > 0 && find_dir_in(home_dir, "Pictures") < 0) mkdir_in(home_dir, "Pictures");
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
