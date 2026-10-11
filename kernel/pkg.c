#include "pkg.h"
#include "driver.h"
#include "app.h"
#include "fs.h"
#include "gui.h"
#include "kheap.h"
#include "kstring.h"
#include "explorer.h"

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
    char dir[FS_PATH_LEN], path[FS_PATH_LEN];
    app_dir(info.name, dir, sizeof(dir));
    int upgrade = fs_find_dir(dir) >= 0;
    if (upgrade) fs_delete(dir, 1);
    if (fs_mkdir_p(dir) < 0) { kstrlcpy(msg, "cannot create the app folder in /apps", (size_t)mcap); kfree(d); return -1; }
    uint32_t count = rd32(d + 4);
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
    }
    kfree(d);
    if (!strcmp(info.type, "driver")) {             /* a driver: loaded right away (and at every boot) */
        char err[96];
        if (driver_load_package(info.name, err, sizeof(err)) == 0)
            ksnprintf(msg, (size_t)mcap, "%s driver %s %s - loaded (see: drivers)", upgrade ? "upgraded" : "installed", info.title, info.version);
        else
            ksnprintf(msg, (size_t)mcap, "%s driver %s %s - %s", upgrade ? "upgraded" : "installed", info.title, info.version, err);
        return 0;
    }
    ksnprintf(msg, (size_t)mcap, "%s %s %s (%s app) - %s", upgrade ? "upgraded" : "installed", info.title,
              info.version[0] ? info.version : "", info.type,
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
    pkg_info_t info;
    if (pkg_get(name, &info) != 0) { ksnprintf(msg, (size_t)mcap, "%s is not installed", name); return -1; }
    char dir[FS_PATH_LEN];
    app_dir(name, dir, sizeof(dir));
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
