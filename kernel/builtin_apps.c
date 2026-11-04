/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "builtin_apps.h"
#include "fs.h"
#include "kheap.h"
#include "kstring.h"
#include "pkg.h"
#include "serial.h"
#include "examples.h"
#include "repo.h"
#include "crc32c.h"
#include "../crypto/sha256.h"
#include "timer.h"
#include "fsdisk.h"

extern const uint8_t app_webworker[],app_webworker_end[];
extern const uint8_t app_mediaplayer[], app_mediaplayer_end[], app_music[], app_music_end[], app_amethyst[], app_amethyst_end[], app_photos[], app_photos_end[], app_code[], app_code_end[], app_store[], app_store_end[], app_terminal[], app_terminal_end[];
#ifdef __x86_64__
extern const uint8_t app_libjsworker[], app_libjsworker_end[];
/* the ports (the Makefile's PORTS64: build/ports64.h) */
#define PORT(n) extern const uint8_t port_##n[], port_##n##_end[];
#define OPTPORT PORT
#include "../build/ports64.h"
#undef PORT
#undef OPTPORT
#endif

/* (stb_image's zlib inflater, third_party/stb) */
char* stbi_zlib_decode_malloc(const char* buffer, int len, int* outlen);

/* optional: in the App Store (builtin:), installed only when asked for -
 * then kept up to date as the others */
static const struct { const char* name; const uint8_t* start; const uint8_t* end; int optional; } APPS[] = {
    { "mediaplayer", app_mediaplayer, app_mediaplayer_end, 0 },
    { "music", app_music, app_music_end, 0 },
    { "amethyst", app_amethyst, app_amethyst_end, 0 },
    { "photos", app_photos, app_photos_end, 0 },
    { "code", app_code, app_code_end, 0 },
    { "store", app_store, app_store_end, 0 },
#ifdef __x86_64__
    { "webworker", app_webworker, app_webworker_end, 0 },
    { "libjsworker", app_libjsworker, app_libjsworker_end, 0 },
    { "terminal", app_terminal, app_terminal_end, 0 }, /* (apps/terminal: bash in windows and tabs) */
#define PORT(n) { #n, port_##n, port_##n##_end, 0 },
#define OPTPORT(n) { #n, port_##n, port_##n##_end, 1 },  /* (the Makefile's OPTPORTS64: Neovim) */
#include "../build/ports64.h"
#undef PORT
#undef OPTPORT
#endif
};

/* /etc/builtin-apps: "name=version#crc" of each one installed from here - the
 * CRC-32C of the package, as an app can change without a new version number
 * (systems from before build 161 have "name=version" lines) */
#define MARKS "/etc/builtin-apps"

static int g_released;                 /* the kernel's copies of the packages are given back (below) */

/* 1 if a line of MARKS starts with "name=version" followed by one of ends */
static int marked_as(const char* name, const char* version, const char* key_end, const char* ends) {
    int fi = fs_find_file(MARKS);
    if (fi < 0) return 0;
    fs_file_t* f = fs_get_file(fi);
    if (!f || !f->content) return 0;
    char want[72];
    ksnprintf(want, sizeof(want), "%s=%s%s", name, version, key_end);
    int n = (int)strlen(want);
    for (const char* p = f->content; (p = strstr(p, want)); p++)
        if ((p == f->content || p[-1] == '\n') && p[n] && strchr(ends, p[n])) return 1;
    return 0;
}

/* this very package was installed from here (and may have been removed since) */
static int marked(const char* name, const char* version, uint32_t crc) {
    char k[16];
    ksnprintf(k, sizeof(k), "#%08x", crc);
    return marked_as(name, version, k, "\n~");
}

/* the same, known from the compressed package's CRC ("name=version#crc~zcrc"
 * lines, build 177): nothing needs unpacking to know it is the one installed */
static int marked_z(const char* name, uint32_t zcrc) {
    int fi = fs_find_file(MARKS);
    fs_file_t* f = fi >= 0 ? fs_get_file(fi) : NULL;
    if (!f || !f->content) return 0;
    char k[16];
    ksnprintf(k, sizeof(k), "~%08x\n", zcrc);
    int n = (int)strlen(name);
    for (const char* p = f->content; (p = strstr(p, k)); p++) {
        const char* line = p;
        while (line > f->content && line[-1] != '\n') line--;
        if (!strncmp(line, name, (size_t)n) && line[n] == '=') return 1;
    }
    return 0;
}

/* some package of this version was (a removed app stays removed) */
static int marked_version(const char* name, const char* version) { return marked_as(name, version, "", "\n#"); }

static void mark(const char* name, const char* version, uint32_t crc, uint32_t zcrc) {
    char line[96];
    ksnprintf(line, sizeof(line), "%s=%s#%08x~%08x\n", name, version, crc, zcrc);
    int fi = fs_find_file(MARKS);
    if (fi < 0) { fs_mkdir_p("/etc"); fi = fs_create(MARKS); }
    if (fi >= 0) fs_append(fi, line, (uint32_t)strlen(line));
}

/* /etc/builtin-apps-repair: "name\n" of each app `pkg verify` found damaged
 * after the kernel's copies were given back: installed again at the next boot */
#define REPAIRS "/etc/builtin-apps-repair"

static int repair_requested(const char* name) {
    int fi = fs_find_file(REPAIRS);
    fs_file_t* f = fi >= 0 ? fs_get_file(fi) : NULL;
    if (!f || !f->content) return 0;
    /* "*": every one (the live system's repair) */
    if (f->size >= 2 && f->content[0] == '*' && f->content[1] == '\n') return 1;
    char want[40];
    ksnprintf(want, sizeof(want), "%s\n", name);
    for (const char* p = f->content; (p = strstr(p, want)); p++)
        if (p == f->content || p[-1] == '\n') return 1;
    return 0;
}

/* /etc/builtin-apps-skip: "name\n" of each app an install profile left out */
#define SKIPS "/etc/builtin-apps-skip"

static int skipped(const char* name) {
    int fi = fs_find_file(SKIPS);
    fs_file_t* f = fi >= 0 ? fs_get_file(fi) : NULL;
    if (!f || !f->content) return 0;
    char want[40];
    ksnprintf(want, sizeof(want), "%s\n", name);
    for (const char* p = f->content; (p = strstr(p, want)); p++)
        if (p == f->content || p[-1] == '\n') return 1;
    return 0;
}

void builtin_apps_install(void) {
    if (g_released) return;                       /* (the kernel's copies are gone: checked at boot) */
    uint32_t t0 = timer_ms();
    int unpacked = 0;
    for (unsigned i = 0; i < sizeof(APPS) / sizeof(APPS[0]); i++) {
        int zlen = (int)(APPS[i].end - APPS[i].start), len = 0;
        if (zlen <= 0) continue;
        /* the package this system already has (or left out on purpose): not unpacked again */
        uint32_t zcrc = crc32c(0, APPS[i].start, (uint32_t)zlen);
        /* an installed copy whose files went missing or changed size (a
         * damaged disk, an update cut short): put back from this copy */
        char first[64];
        pkg_info_t present;
        int damaged = 0;
        if (pkg_get(APPS[i].name, &present) == 0) {
            /* (the programs' and libraries' contents: a program that is the
             *  right size but not right any more - "not an ELF program",
             *  bash among them - is put back too, and the system starts) */
            int v = pkg_verify(APPS[i].name, 2, first, sizeof(first));
            if (v > 0) damaged = 1;
            else if (repair_requested(APPS[i].name)) { damaged = 1; kstrlcpy(first, "a file (pkg verify)", sizeof(first)); }
            /* installed from a built-in copy before there were file lists: once
             * more, so it has one (and whatever an update broke is put back) */
            else if (v == -1 && marked_version(APPS[i].name, present.version)) { damaged = 2; }
        }
        if (damaged == 1) klog("apps: %s: %s is damaged or missing - installed again\n", APPS[i].name, first);
        if (damaged == 2) klog("apps: %s: installed again to record its files (pkg verify)\n", APPS[i].name);
        if (!damaged && marked_z(APPS[i].name, zcrc)) {
            if (!banana_system_component(APPS[i].name) || pkg_get(APPS[i].name, &present) == 0) continue;
        }
        pkg_info_t have;
        if (APPS[i].optional && pkg_get(APPS[i].name, &have) != 0) continue;   /* (not asked for) */
        unpacked++;
        uint8_t* bpk = (uint8_t*)stbi_zlib_decode_malloc((const char*)APPS[i].start, zlen, &len);
        if (!bpk || len <= 0) { klog("apps: %s: the built-in package is damaged\n", APPS[i].name); continue; }
        pkg_info_t mine;
        char err[96];
        if (pkg_inspect_mem(bpk, (uint32_t)len, &mine, err, sizeof(err)) == 0) {
            int installed = pkg_get(mine.name, &have) == 0;
            uint32_t crc = crc32c(0, bpk, (uint32_t)len);
            int want;
            if (installed && damaged)
                want = repo_vercmp(have.version, mine.version) <= 0;   /* (not one from a repository that is newer) */
            else if (installed)
                /* left as it is when a newer version came from a repository, or it
                 * is this very package; else upgraded - also when the app changed
                 * under the same version number (an `update`d system gets it) */
                want = repo_vercmp(have.version, mine.version) <= 0 && !marked(mine.name, mine.version, crc);
            else
                /* removed by the user, or left out by the install profile (and not
                 * installed again since): stays out */
                want = banana_system_component(mine.name) || (!skipped(mine.name) && !marked_version(mine.name, mine.version));
            if (want) {
                char msg[160];
                if (pkg_install_mem(bpk, (uint32_t)len, msg, sizeof(msg)) == 0) {
                    mark(mine.name, mine.version, crc, zcrc);
                    klog("apps: %s\n", msg);
                } else klog("apps: %s: %s\n", mine.name, msg);
            } else if (installed && !marked_z(mine.name, zcrc)) {
                mark(mine.name, mine.version, crc, zcrc);        /* (known now: not unpacked at the next boots) */
            }
        }
        kfree(bpk);
    }
    if (fs_find_file(REPAIRS) >= 0) fs_delete(REPAIRS, 0);     /* (done) */
    if (unpacked) klog("apps: built-in apps checked in %u ms (%d unpacked)\n", timer_ms() - t0, unpacked);
    repo_builtin_refresh();          /* the App Store can install them again (builtin: source) */
}

/* name installed again from the kernel's own copy (pkg verify): 0; 1 at
 * the next start (the copy was given back); -1 no built-in copy; -2 failed */
int builtin_app_reinstall(const char* name, char* msg, int mcap) {
    for (unsigned i = 0; i < sizeof(APPS) / sizeof(APPS[0]); i++) {
        if (strcmp(APPS[i].name, name)) continue;
        if (g_released) {
            /* (the kernel's copy was given back after boot: at the next one) */
            char line[40];
            ksnprintf(line, sizeof(line), "%s\n", name);
            int fi = fs_find_file(REPAIRS);
            if (fi < 0) { fs_mkdir_p("/etc"); fi = fs_create(REPAIRS); }
            if (fi < 0 || fs_append(fi, line, (uint32_t)strlen(line)) != 0) {
                kstrlcpy(msg, "cannot write " REPAIRS, (size_t)mcap);
                return -2;
            }
            ksnprintf(msg, (size_t)mcap, "%s will be installed again at the next start", name);
            return 1;
        }
        int zlen = (int)(APPS[i].end - APPS[i].start), len = 0;
        uint8_t* bpk = zlen > 0 ? (uint8_t*)stbi_zlib_decode_malloc((const char*)APPS[i].start, zlen, &len) : NULL;
        if (!bpk || len <= 0) { kstrlcpy(msg, "the built-in package is damaged", (size_t)mcap); kfree(bpk); return -2; }
        int rc = pkg_install_mem(bpk, (uint32_t)len, msg, mcap);
        if (rc == 0) {
            pkg_info_t mine;
            char err[8];
            if (pkg_inspect_mem(bpk, (uint32_t)len, &mine, err, sizeof(err)) == 0)
                mark(mine.name, mine.version, crc32c(0, bpk, (uint32_t)len), crc32c(0, APPS[i].start, (uint32_t)zlen));
        }
        kfree(bpk);
        return rc == 0 ? 0 : -2;
    }
    return -1;
}

/* ── the App Store's builtin: source ─────────────────────────────────── */

#define NAPPS ((int)(sizeof(APPS) / sizeof(APPS[0])))

int builtin_package_count(void) { return NAPPS + examples_count(); }

const char* builtin_package_file(int i) {
    static char name[40];
    if (i < 0) return "";
    if (i < NAPPS) { ksnprintf(name, sizeof(name), "%s.bpk", APPS[i].name); return name; }
    return examples_file(i - NAPPS);
}

/* ── the packages out of RAM ─────────────────────────────────────────
 * On an installed system the built-in packages are kept as files on the
 * disk (STORE, read when the App Store or selftest wants one) and the
 * kernel's copy of them - its .payload pages, 12 MB - goes to the heap
 * after the boot's check. The live CD keeps them in RAM (no disk). */
#define STORE "/usr/share/builtin"

extern char __payload_start[], __payload_end[];
static uint32_t g_released_bytes;

uint32_t builtin_released_bytes(void) { return g_released_bytes; }

/* package k (the apps, then the examples - every one a compressed .bpk.z):
 * its name in STORE, and where the kernel has it */
static void blob_of(int k, char* name, int cap, const uint8_t** p, uint32_t* n) {
    if (k < NAPPS) {
        ksnprintf(name, (size_t)cap, "%s.bpk.z", APPS[k].name);
        *p = APPS[k].start;
        *n = (uint32_t)(APPS[k].end - APPS[k].start);
    } else {
        ksnprintf(name, (size_t)cap, "%s.z", examples_file(k - NAPPS));
        *p = examples_data(k - NAPPS, n);
    }
}

/* package k's bytes: a kmalloc'd copy (from the kernel, or the file once released) */
static char* blob_copy(int k, uint32_t* n) {
    char name[48];
    const uint8_t* p;
    blob_of(k, name, sizeof(name), &p, n);
    if (!g_released) {
        char* d = (char*)kmalloc(*n ? *n : 1);
        if (d) memcpy(d, p, *n);
        return d;
    }
    char path[FS_PATH_LEN];
    ksnprintf(path, sizeof(path), STORE "/%s", name);
    int fi = fs_find_file(path);
    fs_file_t* f = fi >= 0 ? fs_get_file(fi) : NULL;
    if (!f || !f->content) return NULL;
    char* d = (char*)kmalloc(f->size ? f->size : 1);
    if (d) memcpy(d, f->content, f->size);
    *n = f->size;
    return d;
}

void builtin_release(void) {
    if (g_released || !fsdisk_is_installed()) return;
    uint32_t t0 = timer_ms();
    fs_mkdir_p(STORE);
    /* STORE/.index: "name crc" of each package saved there */
#define INDEX_CAP 4096
    char* index = (char*)kzalloc(INDEX_CAP);
    if (!index) return;
    int ifi = fs_find_file(STORE "/.index");
    fs_file_t* ix = ifi >= 0 ? fs_get_file(ifi) : NULL;
    const char* old = ix && ix->content ? ix->content : NULL;
    for (int k = 0; k < builtin_package_count(); k++) {
        char name[48], path[FS_PATH_LEN];
        const uint8_t* p;
        uint32_t n;
        blob_of(k, name, sizeof(name), &p, &n);
        if (k >= NAPPS) {                                    /* (kernels before build 203 kept the examples unpacked) */
            ksnprintf(path, sizeof(path), STORE "/%s", examples_file(k - NAPPS));
            if (fs_find_file(path) >= 0) fs_delete(path, 0);
        }
        ksnprintf(path, sizeof(path), STORE "/%s", name);
        /* (there already from an earlier boot: the same bytes - the index has
         * their CRC, and the file its size; it is not read) */
        char line[80];
        ksnprintf(line, sizeof(line), "%s %08x\n", name, crc32c(0, p, n));
        kstrlcat(index, line, INDEX_CAP);
        int fi = fs_find_file(path);
        fs_file_t* f = fi >= 0 ? fs_file_info(fi) : NULL;
        if (f && f->size == n && old && strstr(old, line)) continue;
        if (fs_write_path(path, p, n) < 0) {
            klog("builtin: %s could not be saved - the packages stay in RAM\n", path);
            kfree(index);
            return;
        }
        ifi = fs_find_file(STORE "/.index");                /* (the file table may have moved) */
        ix = ifi >= 0 ? fs_get_file(ifi) : NULL;
        old = ix && ix->content ? ix->content : NULL;
    }
    if (!old || strcmp(old, index)) fs_write_path(STORE "/.index", index, (uint32_t)strlen(index));
    kfree(index);
    uintptr_t lo = (uintptr_t)__payload_start, hi = (uintptr_t)__payload_end;
    kheap_add_region(lo, hi);
    g_released = 1;
    g_released_bytes = (uint32_t)(hi - lo);
    klog("builtin: %u KB of built-in packages kept on the disk, their RAM given back (%u ms)\n",
         g_released_bytes >> 10, timer_ms() - t0);
}

int builtin_package(const char* file_in, char** data, uint32_t* len) {
    char file[40];                                           /* (file_in may be builtin_package_file's buffer, reused below) */
    kstrlcpy(file, file_in, sizeof(file));
    for (int k = 0; k < builtin_package_count(); k++) {
        if (strcmp(builtin_package_file(k), file)) continue;
        uint32_t n = 0;
        char* b = blob_copy(k, &n);
        if (!b) return -1;
        int out = 0;                                         /* (compressed, every one: unpacked) */
        char* d = n ? stbi_zlib_decode_malloc(b, (int)n, &out) : NULL;
        kfree(b);
        if (!d || out <= 0) { if (d) kfree(d); return -1; }
        *data = d;
        *len = (uint32_t)out;
        return 0;
    }
    return -1;
}

/* ── install profiles ────────────────────────────────────────────────── */

static const char* const PROFILE_NAMES[PROFILE_COUNT] = { "Full", "Standard", "Minimal" };
static const char* const PROFILE_APPS[PROFILE_COUNT] = {
    "every app: App Store, Media Player, Music, Photos, Amethyst Music, Banana Code, and the examples",
    "the everyday apps: App Store, Media Player, Music, Photos",
    "the App Store only - the other apps can be installed from it",
};
/* what each profile leaves out (package names; "" ends the list) */
static const char* const PROFILE_OUT[PROFILE_COUNT][6] = {
    { "" },
    { "amethyst", "code", "" },
    { "mediaplayer", "music", "photos", "amethyst", "code", "" },
};

const char* builtin_profile_name(int p) { return p >= 0 && p < PROFILE_COUNT ? PROFILE_NAMES[p] : ""; }
const char* builtin_profile_apps(int p) { return p >= 0 && p < PROFILE_COUNT ? PROFILE_APPS[p] : ""; }

int builtin_profile_apply(int p) {
    if (p <= PROFILE_FULL || p >= PROFILE_COUNT) return 0;
    int removed = 0;
    for (int k = 0; PROFILE_OUT[p][k][0]; k++) {
        const char* name = PROFILE_OUT[p][k];
        pkg_info_t have;
        char msg[160];
        if (pkg_get(name, &have) == 0 && pkg_remove(name, msg, sizeof(msg)) == 0) removed++;
        if (!skipped(name)) {                          /* not put back at the next boots */
            char line[40];
            ksnprintf(line, sizeof(line), "%s\n", name);
            int fi = fs_find_file(SKIPS);
            if (fi < 0) { fs_mkdir_p("/etc"); fi = fs_create(SKIPS); }
            if (fi >= 0) fs_append(fi, line, (uint32_t)strlen(line));
        }
    }
    /* the examples: in the App Store (builtin:), not in the home folder */
    if (fs_find_dir("/home/banana/Examples") >= 0) fs_delete("/home/banana/Examples", 1);
    char msg[96];
    repo_remove_source("file:///home/banana/Examples", msg, sizeof(msg));
    klog("install profile %s: %d apps left out (installable from the App Store)\n", PROFILE_NAMES[p], removed);
    return removed;
}

/* changes when this kernel's packages do (the App Store's list is made again) */
extern const uint8_t builtin_meta[], builtin_meta_end[];

int builtin_package_meta(const char* name_in, uint32_t* size, uint8_t sha256[32], const uint8_t** skel, uint32_t* skel_len) {
    /* (a copy: builtin_package_file's buffer, which name_in may well be, is
     * written again by the loop below) */
    char file[40];
    kstrlcpy(file, name_in, sizeof(file));
    const uint8_t* p = builtin_meta;
    const uint8_t* end = builtin_meta_end;
    if (end - p < 8 || memcmp(p, "BMT1", 4) != 0) return -1;
    uint32_t count;
    memcpy(&count, p + 4, 4);
    p += 8;
    for (uint32_t i = 0; i < count; i++) {
        if (end - p < 40 + 4 + 4 + 32 + 4) return -1;
        const char* name = (const char*)p;
        uint32_t blob_len, sz, sl;
        memcpy(&blob_len, p + 40, 4);
        memcpy(&sz, p + 44, 4);
        memcpy(&sl, p + 80, 4);
        const uint8_t* s = p + 84;
        if (sl > (uint32_t)(end - s)) return -1;
        p = s + ((sl + 3u) & ~3u);
        if (strncmp(name, file, 40) != 0) continue;
        /* (it must be about the very copy this kernel has) */
        for (int k = 0; k < builtin_package_count(); k++) {
            if (strcmp(builtin_package_file(k), file)) continue;
            char bn[48];
            const uint8_t* bp;
            uint32_t n;
            blob_of(k, bn, sizeof(bn), &bp, &n);
            if (n != blob_len) return -1;
            *size = sz;
            memcpy(sha256, name + 48, 32);
            *skel = s;
            *skel_len = sl;
            return 0;
        }
        return -1;
    }
    return -1;
}

int builtin_meta_check(char* msg, int cap) {
    int checked = 0;
    msg[0] = 0;
    for (int k = 0; k < builtin_package_count(); k++) {
        char file[40];                              /* (builtin_package_file's buffer is reused by the calls below) */
        kstrlcpy(file, builtin_package_file(k), sizeof(file));
        const uint8_t* skel;
        uint32_t skl, size;
        uint8_t h[32], real[32];
        if (builtin_package_meta(file, &size, h, &skel, &skl) != 0) {
            ksnprintf(msg, (size_t)cap, "%s: no build-time facts", file);
            return -1;
        }
        char* d;
        uint32_t dl;
        if (builtin_package(file, &d, &dl) != 0) { ksnprintf(msg, (size_t)cap, "%s: cannot unpack it", file); return -1; }
        sha256(d, dl, real);
        pkg_info_t a, b;
        char err[96];
        int ia = pkg_inspect_mem((const uint8_t*)d, dl, &a, err, sizeof(err));
        int ib = pkg_inspect_mem(skel, skl, &b, err, sizeof(err));
        kfree(d);
        a.files = b.files = 0;                      /* (the skeleton's own size, of course, differs) */
        a.bytes = b.bytes = 0;
        if (dl != size || memcmp(h, real, 32) != 0) { ksnprintf(msg, (size_t)cap, "%s: size or SHA-256 differ", file); return -1; }
        if (ia != 0 || ib != 0 || memcmp(&a, &b, sizeof(a)) != 0) { ksnprintf(msg, (size_t)cap, "%s: its manifest reads differently", file); return -1; }
        checked++;
    }
    return checked;
}

uint32_t builtin_packages_stamp(void) {
    uint32_t s = 2166136261u;
    for (int i = 0; i < NAPPS; i++) s = (s ^ (uint32_t)(APPS[i].end - APPS[i].start)) * 16777619u;
    for (int i = 0; i < examples_count(); i++) { uint32_t n = 0; examples_data(i, &n); s = (s ^ n) * 16777619u; }
    return s;
}
