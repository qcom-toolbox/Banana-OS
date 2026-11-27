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
#include "sysinfo.h"
#include "ext4.h"
#include "notification.h"

extern const uint8_t app_webworker[],app_webworker_end[];
extern const uint8_t app_mediaplayer[], app_mediaplayer_end[], app_music[], app_music_end[], app_amethyst[], app_amethyst_end[], app_photos[], app_photos_end[], app_camera[], app_camera_end[], app_code[], app_code_end[], app_store[], app_store_end[], app_terminal[], app_terminal_end[];
#ifdef __x86_64__
extern const uint8_t app_libjsworker[], app_libjsworker_end[];
extern const uint8_t app_webmedia[], app_webmedia_end[];
extern const uint8_t app_webgl[], app_webgl_end[];
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
    { "camera", app_camera, app_camera_end, 0 },
    { "code", app_code, app_code_end, 0 },
    { "store", app_store, app_store_end, 0 },
#ifdef __x86_64__
    { "webworker", app_webworker, app_webworker_end, 0 },
    { "libjsworker", app_libjsworker, app_libjsworker_end, 0 },
    { "webmedia", app_webmedia, app_webmedia_end, 0 },     /* (page <audio> / <video> decoder) */
    { "webgl", app_webgl, app_webgl_end, 0 },              /* (the browser's GPU worker: WebGL) */
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

static int g_released;                 /* the kernel's copies of the packages are given back (below):
                                          1 kept on the installed disk, 2 read from the live CD */
static char* blob_copy(int k, uint32_t* n);

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

/* ── what was just installed, read back from the disk itself ─────────
 * After an update or a repair the built-in packages are unpacked onto the
 * installed disk at this start. Then everything is written out, the RAM
 * copies of the files and the disk cache are let go, and every program and
 * library of those packages is read back from the disk and compared with
 * the package (pkg verify's CRCs): a file the disk did not keep is
 * installed again once, and if it is still wrong it is said - in dmesg and
 * as a notification - instead of being found as "not an ELF program". */
static char g_fresh[64][32];
static int  g_nfresh;

static void verify_fresh_on_disk(void) {
    if (!g_nfresh || !fsdisk_is_installed()) return;
    uint32_t t0 = timer_ms();
    for (int pass = 0; pass < 2 && g_nfresh; pass++) {
        if (fsdisk_sync_durable() != 0) { klog("apps: the disk could not be written out to check the apps\n"); return; }
        fs_evict(0xFFFFFFFFu, 0);                     /* (the files' RAM copies: read from the disk again) */
        ext4_cache_trim(0xFFFFFFFFu);                 /* (and the disk's block cache) */
        int left = 0;
        for (int i = 0; i < g_nfresh; i++) {
            char first[64], msg[160];
            int bad = pkg_verify(g_fresh[i], 1, first, sizeof(first));
            if (bad <= 0) continue;
            if (pass == 0) {
                klog("apps: %s: %s (and %d more) did not read back from the disk as written - installing again\n",
                     g_fresh[i], first, bad - 1);
                if (builtin_app_reinstall(g_fresh[i], msg, sizeof(msg)) == 0) kstrlcpy(g_fresh[left++], g_fresh[i], sizeof(g_fresh[0]));
                else klog("apps: %s: %s\n", g_fresh[i], msg);
            } else {
                char body[256];
                ksnprintf(body, sizeof(body), "%s of %s still does not read back from the disk as it was written, after "
                          "installing it twice: the disk or its controller is not keeping what is written. dmesg has more.",
                          first, g_fresh[i]);
                klog("apps: %s: %s STILL reads back wrong from the disk after a second install\n", g_fresh[i], first);
                notify_post("system", "Disk check", body, 0);
            }
        }
        g_nfresh = pass == 0 ? left : 0;
    }
    klog("apps: the apps just installed were read back from the disk and checked (%u ms)\n", timer_ms() - t0);
}

/* ── the all-in-one image's packages (/EXTRAS on it: Java) ─────────────
 * kernel/fsdisk.c reads them from the image; one is installed unless the
 * same one already is: /etc/banana-extras keeps the BANANA.SUM line of each
 * installed (its CRC and size - a new image's differ). */
#define EXTRAS_DONE "/etc/banana-extras"
static int extra_install(const char* file, const uint8_t* data, uint32_t size, const char* sumline) {
    char pkg[32], dir[64];
    int n = 0;
    for (; file[n] && file[n] != '.' && n < (int)sizeof(pkg) - 1; n++) pkg[n] = (char)(file[n] >= 'A' && file[n] <= 'Z' ? file[n] + 32 : file[n]);
    pkg[n] = 0;
    ksnprintf(dir, sizeof(dir), "/apps/%s/manifest", pkg);
    const fs_cred_t* oc = fs_cred_swap(NULL);           /* (the system's: /apps, /usr) */
    int fi = fs_find_file(EXTRAS_DONE);
    fs_file_t* f = fi >= 0 ? fs_get_file(fi) : NULL;
    if (f && f->content && strstr(f->content, sumline) && fs_find_file(dir) >= 0) { fs_cred_swap(oc); return 0; }
    char msg[160];
    uint32_t t0 = timer_ms();
    int rc = pkg_install_mem(data, size, msg, sizeof(msg));
    klog("extras: %s - %s (%u ms)\n", file, rc == 0 ? msg : "not installed", timer_ms() - t0);
    if (rc == 0) {
        /* the list again (looked up anew: installing changed the files): this package's old line out, the new one in */
        fi = fs_find_file(EXTRAS_DONE);
        f = fi >= 0 ? fs_get_file(fi) : NULL;
        char* out = (char*)kmalloc((f && f->content ? f->size : 0) + 256);
        uint32_t o = 0;
        if (out) {
            if (f && f->content) {
                for (const char* l = f->content; *l; ) {
                    const char* e = strchr(l, '\n');
                    uint32_t len = e ? (uint32_t)(e - l) + 1 : (uint32_t)strlen(l);
                    if (!strstr(l, file) || (e && strstr(l, file) > e)) { memcpy(out + o, l, len); o += len; }
                    l += len;
                }
            }
            o += (uint32_t)ksnprintf(out + o, 256, "%s\n", sumline);
            fs_write_path(EXTRAS_DONE, out, o);
            kfree(out);
        }
    }
    fs_cred_swap(oc);
    return rc == 0 ? 0 : -1;
}

/* the live all-in-one image's: at the start (installed systems: after an update) */
void builtin_extras_install(void) {
    fsdisk_set_extras_fn(extra_install);
    int n = fsdisk_live_extras();
    if (n) klog("extras: %d package%s from the image\n", n, n == 1 ? "" : "s");
}

void builtin_apps_install(void) {
    if (g_released) return;
    {
        /* the start's check of the disk's free-space map (kernel/fsdisk.c) */
        uint32_t fr = 0, xl = 0;
        if (fsdisk_is_installed()) fsdisk_scrub_result(&fr, &xl);
        if (fr || xl) {
            char body[256];
            ksnprintf(body, sizeof(body), "The disk's free-space map had %u blocks of files marked free (the next files "
                      "written would have gone over them) - fixed. %u blocks were in two files at once: those files "
                      "are damaged; the built-in apps among them are put back now. dmesg | grep fsdisk has the details.",
                      fr, xl);
            notify_post("system", "Disk check", body, 0);
        }
    }
    g_nfresh = 0;                       /* (the kernel's copies are gone: checked at boot) */
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
            if (installed && damaged == 1)
                want = 1;            /* (damaged: this copy, also over a newer one from a repository - a broken
                                      *  bash must never stay broken; the App Store can update it again) */
            else if (installed && damaged)
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
                    if (g_nfresh < 64) kstrlcpy(g_fresh[g_nfresh++], mine.name, sizeof(g_fresh[0]));
                } else klog("apps: %s: %s\n", mine.name, msg);
            } else if (installed && !marked_z(mine.name, zcrc)) {
                mark(mine.name, mine.version, crc, zcrc);        /* (known now: not unpacked at the next boots) */
            }
        }
        kfree(bpk);
    }
    if (fs_find_file(REPAIRS) >= 0) fs_delete(REPAIRS, 0);     /* (done) */
    if (unpacked) klog("apps: built-in apps checked in %u ms (%d unpacked)\n", timer_ms() - t0, unpacked);
    verify_fresh_on_disk();
    repo_builtin_refresh();          /* the App Store can install them again (builtin: source) */
}

/* name installed again from the kernel's own copy (pkg verify): 0; 1 at
 * the next start (the copy was given back); -1 no built-in copy; -2 failed */
int builtin_app_reinstall(const char* name, char* msg, int mcap) {
    for (unsigned i = 0; i < sizeof(APPS) / sizeof(APPS[0]); i++) {
        if (strcmp(APPS[i].name, name)) continue;
        if (g_released == 2) {                         /* (the live CD: the copy on the CD) */
            uint32_t zn = 0;
            char* z = blob_copy((int)i, &zn);
            int ln = 0;
            uint8_t* b = z ? (uint8_t*)stbi_zlib_decode_malloc(z, (int)zn, &ln) : NULL;
            if (!b || ln <= 0) { kfree(z); kfree(b); kstrlcpy(msg, "the built-in package cannot be read from the CD", (size_t)mcap); return -2; }
            int rc = pkg_install_mem(b, (uint32_t)ln, msg, mcap);
            if (rc == 0) {
                pkg_info_t mine;
                char err[8];
                if (pkg_inspect_mem(b, (uint32_t)ln, &mine, err, sizeof(err)) == 0)
                    mark(mine.name, mine.version, crc32c(0, b, (uint32_t)ln), crc32c(0, z, zn));
            }
            kfree(b);
            kfree(z);
            return rc == 0 ? 0 : -2;
        }
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

/* the live CD: where the kernel's .payload is on the CD, and each package's CRC */
static uint64_t  g_cd_payload;
static uint32_t* g_cd_crc;

/* package k's bytes: a kmalloc'd copy (from the kernel, the CD, or the file once released) */
static char* blob_copy(int k, uint32_t* n) {
    char name[48];
    const uint8_t* p;
    blob_of(k, name, sizeof(name), &p, n);
    if (g_released == 2) {
        uintptr_t lo = (uintptr_t)__payload_start, hi = (uintptr_t)__payload_end;
        if ((uintptr_t)p < lo || (uintptr_t)p + *n > hi) {     /* (not in the given-back pages) */
            char* d = (char*)kmalloc(*n ? *n : 1);
            if (d) memcpy(d, p, *n);
            return d;
        }
        char* d = (char*)kmalloc(*n ? *n : 1);
        if (!d) return NULL;
        if (fsdisk_boot_read(g_cd_payload + ((uintptr_t)p - lo), *n, d) != 0 || crc32c(0, d, *n) != g_cd_crc[k]) {
            klog("builtin: %s could not be read from the CD (is it still in the drive?)\n", name);
            kfree(d);
            return NULL;
        }
        return d;
    }
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

/* The live CD: the packages are read from the CD's own kernel file when
 * wanted (apt install, the App Store, a repair), and the kernel's copy of
 * them goes to the heap - what the system holds in RAM drops by their size.
 * Only when /boot/kernel64.bin on the boot image is this very kernel (its
 * .payload found through its ELF program headers, the start and the end
 * the same bytes); else they stay in RAM as before. */
static void release_live(void) {
#ifdef __x86_64__
    uint32_t t0 = timer_ms();
    uintptr_t lo = (uintptr_t)__payload_start, hi = (uintptr_t)__payload_end;
    uint64_t foff;
    uint32_t fsz;
    if (hi <= lo || fsdisk_boot_file("/boot/kernel64.bin", &foff, &fsz) != 0) return;
    uint8_t* h = (uint8_t*)kmalloc(4096);
    if (!h) return;
    uint64_t at = 0;
    if (fsz >= 4096 && fsdisk_boot_read(foff, 4096, h) == 0 && !memcmp(h, "\x7f" "ELF", 4) && h[4] == 2) {
        uint64_t phoff;
        memcpy(&phoff, h + 0x20, 8);
        uint16_t phent = (uint16_t)(h[0x36] | h[0x37] << 8), phnum = (uint16_t)(h[0x38] | h[0x39] << 8);
        for (uint32_t i = 0; i < phnum && phoff + (uint64_t)(i + 1) * phent <= 4096 && phent >= 56; i++) {
            const uint8_t* ph = h + phoff + (uint64_t)i * phent;
            uint32_t type;
            uint64_t off, va, filesz;
            memcpy(&type, ph, 4); memcpy(&off, ph + 8, 8); memcpy(&va, ph + 16, 8); memcpy(&filesz, ph + 32, 8);
            if (type == 1 && lo >= va && hi <= va + filesz && off + (hi - va) <= fsz) { at = off + (lo - va); break; }
        }
    }
    int same = 0;
    if (at) {
        uint32_t span = hi - lo < 65536u ? (uint32_t)(hi - lo) : 65536u;
        uint8_t* b = (uint8_t*)kmalloc(span);
        same = b && fsdisk_boot_read(foff + at, span, b) == 0 && !memcmp(b, (const void*)lo, span) &&
               fsdisk_boot_read(foff + at + (hi - lo) - span, span, b) == 0 && !memcmp(b, (const void*)(hi - span), span);
        if (b) kfree(b);
    }
    kfree(h);
    if (!same) { klog("builtin: the CD's kernel is not this one - the packages stay in RAM\n"); return; }
    int count = builtin_package_count();
    g_cd_crc = (uint32_t*)kmalloc((size_t)count * 4u);
    if (!g_cd_crc) return;
    for (int k = 0; k < count; k++) {
        char name[48];
        const uint8_t* p;
        uint32_t n;
        blob_of(k, name, sizeof(name), &p, &n);
        g_cd_crc[k] = crc32c(0, p, n);
    }
    g_cd_payload = foff + at;
    kheap_add_region(lo, hi);
    g_released = 2;
    g_released_bytes = (uint32_t)(hi - lo);
    klog("builtin: %u KB of built-in packages read from the CD when wanted, their RAM given back (%u ms)\n",
         g_released_bytes >> 10, timer_ms() - t0);
#endif
}

void builtin_release(void) {
    if (g_released) return;
    /* Only the live system reads its apps from the stick / CD it started
     * from. A system started from the disk never does - even when its files
     * could not be loaded (then the packages simply stay in RAM): pulling a
     * stick out must never break a program of the installed system. */
    if (sysinfo_live_boot()) { release_live(); return; }
    if (!fsdisk_is_installed()) return;
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
    "every app: App Store, Media Player, Music, Photos, Camera, Amethyst Music, Banana Code, and the examples",
    "the everyday apps: App Store, Media Player, Music, Photos, Camera",
    "the App Store only - the other apps can be installed from it",
};
/* what each profile leaves out (package names; "" ends the list) */
static const char* const PROFILE_OUT[PROFILE_COUNT][7] = {
    { "" },
    { "amethyst", "code", "" },
    { "mediaplayer", "music", "photos", "camera", "amethyst", "code", "" },
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
