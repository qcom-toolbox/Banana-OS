#include "builtin_apps.h"
#include "fs.h"
#include "kheap.h"
#include "kstring.h"
#include "pkg.h"
#include "serial.h"

extern const uint8_t app_mediaplayer[], app_mediaplayer_end[], app_music[], app_music_end[], app_amethyst[], app_amethyst_end[], app_photos[], app_photos_end[], app_code[], app_code_end[];

/* (stb_image's zlib inflater, third_party/stb) */
char* stbi_zlib_decode_malloc(const char* buffer, int len, int* outlen);

static const struct { const char* name; const uint8_t* start; const uint8_t* end; } APPS[] = {
    { "mediaplayer", app_mediaplayer, app_mediaplayer_end },
    { "music", app_music, app_music_end },
    { "amethyst", app_amethyst, app_amethyst_end },
    { "photos", app_photos, app_photos_end },
    { "code", app_code, app_code_end },
};

/* /etc/builtin-apps: "name=version" of each one installed from here */
#define MARKS "/etc/builtin-apps"

static int marked(const char* name, const char* version) {
    int fi = fs_find_file(MARKS);
    if (fi < 0) return 0;
    fs_file_t* f = fs_get_file(fi);
    if (!f || !f->content) return 0;
    char want[64];
    ksnprintf(want, sizeof(want), "%s=%s\n", name, version);
    return strstr(f->content, want) != NULL;
}

static void mark(const char* name, const char* version) {
    char line[64];
    ksnprintf(line, sizeof(line), "%s=%s\n", name, version);
    int fi = fs_find_file(MARKS);
    if (fi < 0) { fs_mkdir_p("/etc"); fi = fs_create(MARKS); }
    if (fi >= 0) fs_append(fi, line, (uint32_t)strlen(line));
}

void builtin_apps_install(void) {
    for (unsigned i = 0; i < sizeof(APPS) / sizeof(APPS[0]); i++) {
        int zlen = (int)(APPS[i].end - APPS[i].start), len = 0;
        if (zlen <= 0) continue;
        uint8_t* bpk = (uint8_t*)stbi_zlib_decode_malloc((const char*)APPS[i].start, zlen, &len);
        if (!bpk || len <= 0) { klog("apps: %s: the built-in package is damaged\n", APPS[i].name); continue; }
        pkg_info_t mine, have;
        char err[96];
        if (pkg_inspect_mem(bpk, (uint32_t)len, &mine, err, sizeof(err)) == 0) {
            int installed = pkg_get(mine.name, &have) == 0;
            /* installed already (this version), or removed by the user: left as it is */
            if (!(installed && !strcmp(have.version, mine.version)) && !marked(mine.name, mine.version)) {
                char msg[160];
                if (pkg_install_mem(bpk, (uint32_t)len, msg, sizeof(msg)) == 0) {
                    mark(mine.name, mine.version);
                    klog("apps: %s\n", msg);
                } else klog("apps: %s: %s\n", mine.name, msg);
            }
        }
        kfree(bpk);
    }
}
