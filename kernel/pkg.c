/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pkg.h"
#include "driver.h"
#include "app.h"
#include "fs.h"
#include "gui.h"
#include "kheap.h"
#include "kstring.h"
#include "explorer.h"
#include "zip.h"
#include "serial.h"
#include "crc32c.h"

#define ENT_SIZE   64u
#define ENT_NAME   56u
#define MAX_FILES  256u

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int valid_name(const char* n) {
    int l = (int)strlen(n);
    if (l < 1 || l >= PKG_NAME_MAX) return 0;
    for (int i = 0; i < l; i++) {
        char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
    }
    return 1;
}

/* key=value from a manifest into out ("" if missing) */
static void mf_get(const char* mf, uint32_t len, const char* key, char* out, int cap) {
    out[0] = 0;
    size_t kl = strlen(key);
    for (uint32_t i = 0; i < len;) {
        uint32_t e = i;
        while (e < len && mf[e] != '\n') e++;
        if (e - i > kl && strncmp(mf + i, key, kl) == 0 && mf[i + kl] == '=') {
            uint32_t vs = i + (uint32_t)kl + 1, ve = e;
            while (ve > vs && (mf[ve - 1] == '\r' || mf[ve - 1] == ' ')) ve--;
            int n = 0;
            for (uint32_t k = vs; k < ve && n < cap - 1; k++) out[n++] = mf[k];
            out[n] = 0;
            return;
        }
        i = e + 1;
    }
}

static void parse_manifest(const char* mf, uint32_t len, pkg_info_t* p) {
    mf_get(mf, len, "name", p->name, sizeof(p->name));
    mf_get(mf, len, "title", p->title, sizeof(p->title));
    mf_get(mf, len, "version", p->version, sizeof(p->version));
    mf_get(mf, len, "type", p->type, sizeof(p->type));
    mf_get(mf, len, "description", p->description, sizeof(p->description));
    mf_get(mf, len, "author", p->author, sizeof(p->author));
    mf_get(mf, len, "category", p->category, sizeof(p->category));
    mf_get(mf, len, "depends", p->depends, sizeof(p->depends));
    if (!p->title[0]) kstrlcpy(p->title, p->name, sizeof(p->title));
    if (strcmp(p->type, "gui") != 0 && strcmp(p->type, "driver") != 0) kstrlcpy(p->type, "console", sizeof(p->type));
}

/* checks the archive; *count entries at data+8 */
static int check_archive(const uint8_t* d, uint32_t size, uint32_t* count, char* err, int ecap) {
    if (size < 8 || memcmp(d, "BPK1", 4) != 0) { kstrlcpy(err, "not a Banana OS package (.bpk)", (size_t)ecap); return -1; }
    uint32_t n = rd32(d + 4);
    if (n == 0 || n > MAX_FILES || 8 + n * ENT_SIZE > size) { kstrlcpy(err, "damaged package (bad file table)", (size_t)ecap); return -1; }
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t* e = d + 8 + i * ENT_SIZE;
        uint32_t off = rd32(e + ENT_NAME), len = rd32(e + ENT_NAME + 4);
        int terminated = 0;
        for (uint32_t k = 0; k < ENT_NAME; k++) if (!e[k]) { terminated = 1; break; }
        if (off > size || len > size - off || !e[0] || !terminated) {
            kstrlcpy(err, "damaged package (bad file entry)", (size_t)ecap);
            return -1;
        }
        if (strstr((const char*)e, "..") || e[0] == '/') { kstrlcpy(err, "package has unsafe file names", (size_t)ecap); return -1; }
    }
    *count = n;
    return 0;
}

static const uint8_t* find_entry(const uint8_t* d, uint32_t count, const char* name, uint32_t* len) {
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* e = d + 8 + i * ENT_SIZE;
        if (strcmp((const char*)e, name) == 0) {
            *len = rd32(e + ENT_NAME + 4);
            return d + rd32(e + ENT_NAME);
        }
    }
    return NULL;
}

static int inspect_data(const uint8_t* d, uint32_t size, pkg_info_t* out, char* err, int ecap) {
    uint32_t count;
    memset(out, 0, sizeof(*out));
    if (check_archive(d, size, &count, err, ecap) != 0) return -1;
    uint32_t ml;
    const uint8_t* mf = find_entry(d, count, "manifest", &ml);
    if (!mf) { kstrlcpy(err, "the package has no manifest", (size_t)ecap); return -1; }
    parse_manifest((const char*)mf, ml, out);
    if (!valid_name(out->name)) {
        kstrlcpy(err, "bad app name in the manifest (a-z, 0-9, - and _, at most 24)", (size_t)ecap);
        return -1;
    }
    uint32_t l;
    out->has_i686 = find_entry(d, count, "app-i686", &l) != NULL || find_entry(d, count, "driver-i686", &l) != NULL;
    out->has_x86_64 = find_entry(d, count, "app-x86_64", &l) != NULL || find_entry(d, count, "driver-x86_64", &l) != NULL;
    out->files = count;
    out->bytes = size;
    return 0;
}

int pkg_inspect(const char* bpk_path, pkg_info_t* out, char* err, int ecap) {
    int fi = fs_find_file(bpk_path);
    if (fi < 0) { kstrlcpy(err, "no such file", (size_t)ecap); return -1; }
    fs_file_t* f = fs_get_file(fi);
    if (!f) { kstrlcpy(err, "cannot read the file", (size_t)ecap); return -1; }
    fs_pin(fi);
    int rc = inspect_data((const uint8_t*)f->content, f->size, out, err, ecap);
    fs_unpin(fi);
    return rc;
}

static void app_dir(const char* name, char* out, int cap) {
    ksnprintf(out, (size_t)cap, "%s/%s", PKG_DIR, name);
}

/* "unpack=<file>:<folder>" in a manifest: a ZIP in the package that is
 * unpacked into a folder of the system's (OpenJDK: hundreds of files in
 * folders, more than a package holds). The folder belongs to the package:
 * replaced on an upgrade, deleted with it - so only a folder of its own
 * under /usr. 1 (file, dest), 0 (none), -1 (not allowed) */
static int unpack_spec(const char* mf, uint32_t len, char* file, int fcap, char* dest, int dcap) {
    char v[FS_PATH_LEN];
    mf_get(mf, len, "unpack", v, sizeof(v));
    if (!v[0]) return 0;
    char* c = strchr(v, ':');
    if (!c) return -1;
    *c = 0;
    kstrlcpy(file, v, (size_t)fcap);
    kstrlcpy(dest, c + 1, (size_t)dcap);
    int n = (int)strlen(dest);
    while (n > 1 && dest[n - 1] == '/') dest[--n] = 0;
    if (!file[0] || strchr(file, '/') || !strcmp(file, "manifest")) return -1;
    if (strncmp(dest, "/usr/", 5) != 0 || !dest[5] || strstr(dest, "..") || strstr(dest, "//")) return -1;
    return 1;
}

/* "bin=<name>[,<name>...]" in a manifest: the program is also a command of
 * the system's - /bin/<name>, a symbolic link to /apps/<app>/app-<cpu>
 * (bash: bin=bash,sh), or to /apps/<app>/<name> when the package has a
 * file of that name (a package of several programs: findutils' xargs).
 * A name already in /bin is replaced; the links go with the package. */
static int bin_name_ok(const char* n, int l) {
    if (l < 1 || l > 32 || (l <= 2 && n[0] == '.')) return 0;
    for (int i = 0; i < l; i++) {
        char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.' || c == '+' || c == '[')) return 0;
    }
    return 1;
}

/* bin= and lib=: package-owned links in /bin or /lib. */
static void each_link(const char* mf, uint32_t len, const char* app,
                      const char* key, const char* prefix,
                      void (*fn)(const char* app, const char* link)) {
    char link[FS_PATH_LEN];
    char* v = (char*)kmalloc(4096);             /* (coreutils: 105 commands) */
    if (!v) return;
    mf_get(mf, len, key, v, 4096);
    for (char* p = v; *p;) {
        char* e = p;
        while (*e && *e != ',') e++;
        int l = (int)(e - p);
        while (l && p[l - 1] == ' ') l--;
        if (bin_name_ok(p, l)) {
            kstrlcpy(link, prefix, sizeof(link));
            memcpy(link + 5, p, (size_t)l);
            link[5 + l] = 0;
            fn(app, link);
        }
        p = *e ? e + 1 : e;
        while (*p == ' ') p++;
    }
    kfree(v);
}

#define each_bin(mf,len,app,fn) each_link(mf,len,app,"bin","/bin/",fn)
#define each_lib(mf,len,app,fn) each_link(mf,len,app,"lib","/lib/",fn)

static void bin_target(const char* app, const char* link, char* out, int cap) {
    ksnprintf(out, (size_t)cap, "%s/%s/%s", PKG_DIR, app, link + 5);        /* (/bin/<name> or /lib/<name>) */
    if (fs_find_file(out) < 0) ksnprintf(out, (size_t)cap, "%s/%s/app-%s", PKG_DIR, app, BANANA_ARCH);
}

static void bin_add(const char* app, const char* link) {
    char target[FS_PATH_LEN];
    bin_target(app, link, target, sizeof(target));
    fs_chmod(target, 0755);
    fs_mkdir_p("/bin");
    fs_stat_t st;
    if (fs_stat(link, 0, &st) == 0) {
        if (st.is_dir) { klog("pkg: %s: %s is a folder - not replaced\n", app, link); return; }
        fs_delete(link, 0);
    }
    if (fs_symlink(target, link) < 0) klog("pkg: %s: cannot make %s\n", app, link);
}

/* A library must exist in the package; never link to its CLI executable. */
static void lib_add(const char* app, const char* link) {
    char target[FS_PATH_LEN];
    ksnprintf(target, sizeof(target), "%s/%s/%s", PKG_DIR, app, link + 5);
    if (fs_find_file(target) < 0) return;
    fs_mkdir_p("/lib");
    bin_add(app, link);
}

/* only a link that is still this package's */
static void bin_remove(const char* app, const char* link) {
    char target[FS_PATH_LEN], have[FS_PATH_LEN];
    bin_target(app, link, target, sizeof(target));
    if (fs_readlink(link, have, sizeof(have)) >= 0 && !strcmp(have, target)) fs_delete(link, 0);
}

/* a console app with no bin=: its own name in /bin (pkg install
 * ~/Examples/posixtest.bpk, then posixtest in bash) - unless /bin has that
 * name already */
static void bin_default(const char* mf, uint32_t len, const pkg_info_t* info, int add) {
    if (strcmp(info->type, "console") != 0 || !bin_name_ok(info->name, (int)strlen(info->name))) return;
    char v[8];
    mf_get(mf, len, "bin", v, sizeof(v));
    if (v[0]) return;
    char link[FS_PATH_LEN];
    ksnprintf(link, sizeof(link), "/bin/%s", info->name);
    if (!add) { bin_remove(info->name, link); return; }
    fs_stat_t st;
    if (fs_stat(link, 0, &st) == 0) return;
    bin_add(info->name, link);
}

/* at boot: the links of the console apps installed before there were any */
void pkg_bin_sync(void) {
    pkg_info_t* l = (pkg_info_t*)kmalloc(sizeof(pkg_info_t) * 128);
    if (!l) return;
    int n = pkg_list(l, 128);
    for (int i = 0; i < n && i < 128; i++) {
        char path[FS_PATH_LEN];
        ksnprintf(path, sizeof(path), "%s/%s/manifest", PKG_DIR, l[i].name);
        int fi = fs_find_file(path);
        fs_file_t* f = fi >= 0 ? fs_get_file(fi) : NULL;
        if (f && f->content) bin_default(f->content, f->size, &l[i], 1);
    }
    kfree(l);
}

int pkg_install(const char* bpk_path, char* msg, int mcap) {
    int fi = fs_find_file(bpk_path);
    if (fi < 0) { kstrlcpy(msg, "no such file", (size_t)mcap); return -1; }
    fs_file_t* f = fs_get_file(fi);
    if (!f) { kstrlcpy(msg, "cannot read the file", (size_t)mcap); return -1; }
    fs_pin(fi);
    int r = pkg_install_mem((const uint8_t*)f->content, f->size, msg, mcap);
    fs_unpin(fi);
    return r;
}

/* ── the package's files, to check them later (pkg verify) ───────────
 * /apps/<name>/.files: one line per file the package put in its folder,
 * "<crc32c hex> <size> <name>" - the ZIP an unpack= package unpacks is
 * left out (it is deleted once unpacked). */
#define FILE_LIST ".files"

static void write_file_list(const char* dir, const uint8_t* d, uint32_t count, const char* skip) {
    uint32_t cap = count * (ENT_NAME + 32u) + 1u, n = 0;
    char* t = (char*)kmalloc(cap);
    if (!t) return;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* e = d + 8 + i * ENT_SIZE;
        if (skip && !strcmp((const char*)e, skip)) continue;
        uint32_t off = rd32(e + ENT_NAME), len = rd32(e + ENT_NAME + 4);
        n += (uint32_t)ksnprintf(t + n, cap - n, "%08x %u %s\n", crc32c(0, d + off, len), len, (const char*)e);
    }
    char path[FS_PATH_LEN];
    ksnprintf(path, sizeof(path), "%s/" FILE_LIST, dir);
    fs_write_path(path, t, n);
    kfree(t);
}

/* the files an unpack= package unpacked into udest, appended to its list
 * by their full paths (java.bpk: /usr/java - its JDK, LWJGL and OpenGL) */
static void append_unpacked(const char* dir, const char* udest) {
    char path[FS_PATH_LEN];
    ksnprintf(path, sizeof(path), "%s/" FILE_LIST, dir);
    int li = fs_find_file(path);
    if (li < 0) return;
    int dirs[64], nd = 0, files[256];
    int top = fs_find_dir(udest);
    if (top < 0) return;
    char line[FS_PATH_LEN + 32];
    /* (folders to look at: a stack of paths) */
    char (*stack)[FS_PATH_LEN] = (char (*)[FS_PATH_LEN])kmalloc(64 * FS_PATH_LEN);
    if (!stack) return;
    int sp = 0;
    kstrlcpy(stack[sp++], udest, FS_PATH_LEN);
    while (sp > 0) {
        char here[FS_PATH_LEN];
        kstrlcpy(here, stack[--sp], sizeof(here));
        int nf = fs_list_files(here, files, 256);
        for (int i = 0; i < nf && i < 256; i++) {
            fs_pin(files[i]);
            fs_file_t* f = fs_get_file(files[i]);
            if (f && f->content) {
                char fp[FS_PATH_LEN];
                fs_file_path(files[i], fp, sizeof(fp));
                int n = ksnprintf(line, sizeof(line), "%08x %u %s\n", crc32c(0, f->content, f->size), f->size, fp);
                fs_append(li, line, (uint32_t)n);
            }
            fs_unpin(files[i]);
        }
        nd = fs_list_dirs(here, dirs, 64);
        for (int i = 0; i < nd && i < 64 && sp < 64; i++) fs_dir_path(dirs[i], stack[sp++], FS_PATH_LEN);
    }
    kfree(stack);
}

/* how many files of package name are missing or not what it installed
 * (full: their contents too, else their sizes); -1: no list (installed
 * before there were lists), -2: not installed. The first damaged one's
 * name goes to first. */
int pkg_verify(const char* name, int full, char* first, int fcap) {
    if (first && fcap > 0) first[0] = 0;
    if (!valid_name(name)) return -2;
    char dir[FS_PATH_LEN], path[FS_PATH_LEN];
    app_dir(name, dir, sizeof(dir));
    if (fs_find_dir(dir) < 0) return -2;
    ksnprintf(path, sizeof(path), "%s/" FILE_LIST, dir);
    int li = fs_find_file(path);
    if (li < 0) return -1;
    fs_pin(li);
    fs_file_t* lf = fs_get_file(li);
    if (!lf || !lf->content) { fs_unpin(li); return -1; }
    char* list = (char*)kmalloc(lf->size + 1u);
    if (!list) { fs_unpin(li); return -1; }
    memcpy(list, lf->content, lf->size);
    list[lf->size] = 0;
    fs_unpin(li);
    int bad = 0;
    for (char* line = list; *line; ) {
        char* end = strchr(line, '\n');
        if (end) *end = 0;
        uint32_t crc = 0, size = 0;
        char* p = line;
        while (*p && *p != ' ') { char c = *p++; crc = crc * 16u + (uint32_t)(c <= '9' ? c - '0' : (c | 32) - 'a' + 10); }
        if (*p == ' ') p++;
        while (*p >= '0' && *p <= '9') size = size * 10u + (uint32_t)(*p++ - '0');
        if (*p == ' ') p++;
        if (*p) {
            if (*p == '/') kstrlcpy(path, p, sizeof(path));      /* (unpacked: its full path) */
            else ksnprintf(path, sizeof(path), "%s/%s", dir, p);
            int fi = fs_find_file(path), ok = 0;
            fs_file_t* info = fi >= 0 ? fs_file_info(fi) : NULL;
            /* (full 2: the contents of the programs and libraries only -
             *  what must be right for anything to start; sizes for the rest) */
            const char* base = strrchr(p, '/') ? strrchr(p, '/') + 1 : p;
            size_t bl = strlen(base);
            int code = !strncmp(base, "app-", 4) || !strncmp(base, "driver-", 7) ||
                       (bl > 3 && !strcmp(base + bl - 3, ".so")) || strstr(base, ".so.") != NULL;
            if (info && info->size == size) {
                if (!full || (full == 2 && !code)) ok = 1;
                else {
                    fs_pin(fi);
                    fs_file_t* f = fs_get_file(fi);
                    ok = f && f->content && f->size == size && crc32c(0, f->content, size) == crc;
                    fs_unpin(fi);
                }
            }
            if (!ok) { if (!bad && first && fcap > 0) kstrlcpy(first, p, (size_t)fcap); bad++; }
        }
        if (!end) break;
        line = end + 1;
    }
    kfree(list);
    return bad;
}

int pkg_install_mem(const uint8_t* data, uint32_t size, char* msg, int mcap) {
    /* work on a copy: the package may live on a stick, or be replaced meanwhile */
    uint8_t* d = (uint8_t*)kmalloc(size + 1);
    if (!d) { kstrlcpy(msg, "out of memory", (size_t)mcap); return -1; }
    memcpy(d, data, size);
    pkg_info_t info;
    if (inspect_data(d, size, &info, msg, mcap) != 0) { kfree(d); return -1; }
    int mine = BANANA_ARCH[0] == 'x' ? info.has_x86_64 : info.has_i686;
    if (!mine) {
        ksnprintf(msg, (size_t)mcap, "%s has no %s program (rebuild it with the SDK for both CPUs)", info.name, BANANA_ARCH);
        kfree(d);
        return -1;
    }
    char dir[FS_PATH_LEN], path[FS_PATH_LEN], ufile[ENT_NAME], udest[FS_PATH_LEN];
    uint32_t count = rd32(d + 4), ml = 0;
    const uint8_t* mf = find_entry(d, count, "manifest", &ml);
    int unpack = unpack_spec((const char*)mf, ml, ufile, sizeof(ufile), udest, sizeof(udest));
    if (unpack < 0) {
        kstrlcpy(msg, "bad unpack= in the manifest (a file of the package : a folder under /usr)", (size_t)mcap);
        kfree(d);
        return -1;
    }
    if (unpack && !find_entry(d, count, ufile, &ml)) {
        ksnprintf(msg, (size_t)mcap, "the package has no %s to unpack", ufile);
        kfree(d);
        return -1;
    }
    app_dir(info.name, dir, sizeof(dir));
    int upgrade = fs_find_dir(dir) >= 0;
    if (upgrade) {
        /* (the old version's /bin links: made again below if still wanted) */
        ksnprintf(path, sizeof(path), "%s/manifest", dir);
        int ofi = fs_find_file(path);
        fs_file_t* of = ofi >= 0 ? fs_get_file(ofi) : NULL;
        if (of && of->content) each_bin(of->content, of->size, info.name, bin_remove);
        if (of && of->content) each_lib(of->content, of->size, info.name, bin_remove);
        fs_delete(dir, 1);
    }
    if (fs_mkdir_p(dir) < 0) { kstrlcpy(msg, "cannot create the app folder in /apps", (size_t)mcap); kfree(d); return -1; }
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* e = d + 8 + i * ENT_SIZE;
        ksnprintf(path, sizeof(path), "%s/%s", dir, (const char*)e);
        /* folders inside the package */
        char* slash = strrchr(path, '/');
        if (slash && slash > path + strlen(dir)) {
            *slash = 0;
            fs_mkdir_p(path);
            *slash = '/';
        }
        if (fs_write_path(path, d + rd32(e + ENT_NAME), rd32(e + ENT_NAME + 4)) < 0) {
            ksnprintf(msg, (size_t)mcap, "cannot write %s (filesystem full?)", path);
            fs_delete(dir, 1);
            kfree(d);
            return -1;
        }
        /* the programs can be run (bash looks at the x bits) */
        if (!strncmp((const char*)e, "app-", 4) || !strncmp((const char*)e, "driver-", 7)) fs_chmod(path, 0755);
    }
    write_file_list(dir, d, count, unpack ? ufile : NULL);
    kfree(d);
    char unpacked[96] = "";
    if (unpack) {
        /* the ZIP into its folder (a fresh one: an upgrade leaves no old
         * files behind), then dropped from /apps - its files are there now */
        ksnprintf(path, sizeof(path), "%s/%s", dir, ufile);
        if (fs_find_dir(udest) >= 0) fs_delete(udest, 1);
        zip_stats_t st;
        char zerr[128];
        if (zip_extract(path, udest, ZIP_OVERWRITE, NULL, NULL, &st, zerr, sizeof(zerr)) < 0 || st.failed) {
            ksnprintf(msg, (size_t)mcap, "cannot unpack %s into %s: %s", ufile, udest, zerr[0] ? zerr : "filesystem full?");
            fs_delete(udest, 1);
            fs_delete(dir, 1);
            return -1;
        }
        fs_delete(path, 0);
        append_unpacked(dir, udest);
        ksnprintf(unpacked, sizeof(unpacked), " - %d files in %s", st.files, udest);
    }
    /* its commands in /bin (bin=) */
    ksnprintf(path, sizeof(path), "%s/manifest", dir);
    int mfi = fs_find_file(path);
    fs_file_t* mff = mfi >= 0 ? fs_get_file(mfi) : NULL;
    if (mff && mff->content) each_bin(mff->content, mff->size, info.name, bin_add);
    if (mff && mff->content) bin_default(mff->content, mff->size, &info, 1);
    if (mff && mff->content) each_lib(mff->content, mff->size, info.name, lib_add);
    if (!strcmp(info.type, "driver")) {             /* a driver: loaded right away (and at every boot) */
        char err[96];
        if (driver_load_package(info.name, err, sizeof(err)) == 0)
            ksnprintf(msg, (size_t)mcap, "%s driver %s %s - loaded (see: drivers)", upgrade ? "upgraded" : "installed", info.title, info.version);
        else
            ksnprintf(msg, (size_t)mcap, "%s driver %s %s - %s", upgrade ? "upgraded" : "installed", info.title, info.version, err);
        return 0;
    }
    ksnprintf(msg, (size_t)mcap, "%s %s %s (%s app)%s - %s", upgrade ? "upgraded" : "installed", info.title,
              info.version[0] ? info.version : "", info.type, unpacked,
              strcmp(info.type, "gui") == 0 ? "open it from Apps on the desktop" : "type its name to run it");
    return 0;
}

int pkg_get(const char* name, pkg_info_t* out) {
    if (!valid_name(name)) return -1;
    char path[FS_PATH_LEN];
    ksnprintf(path, sizeof(path), "%s/%s/manifest", PKG_DIR, name);
    int fi = fs_find_file(path);
    if (fi < 0) return -1;
    fs_file_t* f = fs_get_file(fi);
    if (!f) return -1;
    memset(out, 0, sizeof(*out));
    parse_manifest(f->content, f->size, out);
    kstrlcpy(out->name, name, sizeof(out->name));
    ksnprintf(path, sizeof(path), "%s/%s/app-i686", PKG_DIR, name);
    out->has_i686 = fs_find_file(path) >= 0;
    ksnprintf(path, sizeof(path), "%s/%s/app-x86_64", PKG_DIR, name);
    out->has_x86_64 = fs_find_file(path) >= 0;
    return 0;
}

/* the apps (drivers: 1 for the driver packages instead) */
static int list_kind(pkg_info_t* out, int max, int drivers) {
    int idx[64];
    int n = fs_list_dirs(PKG_DIR, idx, 64);
    if (n < 0) return 0;
    if (n > 64) n = 64;
    int got = 0;
    for (int i = 0; i < n; i++) {
        const fs_dir_t* d = fs_get_dir(idx[i]);
        pkg_info_t info;
        if (!d || pkg_get(d->name, &info) != 0) continue;
        if ((strcmp(info.type, "driver") == 0) != drivers) continue;
        /* A-Z by title */
        int at = got < max ? got : max;
        while (at > 0 && strcasecmp(out[at - 1].title, info.title) > 0) {
            if (at < max) out[at] = out[at - 1];
            at--;
        }
        if (at < max) out[at] = info;
        got++;
    }
    return got;
}

int pkg_list(pkg_info_t* out, int max) { return list_kind(out, max, 0); }
int pkg_list_drivers(pkg_info_t* out, int max) { return list_kind(out, max, 1); }

int pkg_remove(const char* name, char* msg, int mcap) {
    if (banana_system_component(name)) {
        ksnprintf(msg, (size_t)mcap, "%s is an integrated OS component", name);
        return -1;
    }
    pkg_info_t info;
    if (pkg_get(name, &info) != 0) { ksnprintf(msg, (size_t)mcap, "%s is not installed", name); return -1; }
    char dir[FS_PATH_LEN], path[FS_PATH_LEN], ufile[ENT_NAME], udest[FS_PATH_LEN];
    app_dir(name, dir, sizeof(dir));
    /* and the folder it unpacked (unpack=) */
    ksnprintf(path, sizeof(path), "%s/manifest", dir);
    int fi = fs_find_file(path);
    fs_file_t* f = fi >= 0 ? fs_get_file(fi) : NULL;
    if (f && unpack_spec(f->content, f->size, ufile, sizeof(ufile), udest, sizeof(udest)) > 0 && fs_find_dir(udest) >= 0)
        fs_delete(udest, 1);
    if (f && f->content) each_bin(f->content, f->size, name, bin_remove);       /* and its /bin links */
    if (f && f->content) bin_default(f->content, f->size, &info, 0);
    if (f && f->content) each_lib(f->content, f->size, name, bin_remove);
    fs_delete(dir, 1);
    ksnprintf(msg, (size_t)mcap, "removed %s", info.title);
    return 0;
}

int pkg_run(const char* name, int argc, char** argv, int from_desktop, char* err, int ecap) {
    pkg_info_t info;
    if (pkg_get(name, &info) != 0) { ksnprintf(err, (size_t)ecap, "%s is not installed", name); return -1; }
    if (!strcmp(info.type, "driver")) { ksnprintf(err, (size_t)ecap, "%s is a driver (loaded at boot: see drivers)", name); return -1; }
    char prog[FS_PATH_LEN];
    ksnprintf(prog, sizeof(prog), "%s/%s/app-%s", PKG_DIR, name, BANANA_ARCH);
    if (fs_find_file(prog) < 0) { ksnprintf(err, (size_t)ecap, "%s has no %s program", name, BANANA_ARCH); return -1; }
    char* def_argv[1] = { (char*)name };
    if (argc <= 0) { argc = 1; argv = def_argv; }
    if (strcmp(info.type, "gui") == 0) {
        if (!gui_is_enabled()) { kstrlcpy(err, "a desktop app - start the desktop first (startx)", (size_t)ecap); return -1; }
        return app_spawn(prog, argc, argv, err, ecap);
    }
    if (from_desktop) {
        /* a console app clicked on the desktop: run it in a new terminal */
        char cmd[64];
        ksnprintf(cmd, sizeof(cmd), "%s\n", name);
        gui_terminal_run(cmd);
        return 0;
    }
    return app_exec(prog, argc, argv, err, ecap);
}

int pkg_inspect_mem(const uint8_t* data, uint32_t size, pkg_info_t* out, char* err, int ecap) {
    return inspect_data(data, size, out, err, ecap);
}

int pkg_is_desktop(const pkg_info_t* info) {
    return info && !banana_system_component(info->name) && !strcmp(info->type, "gui");
}
