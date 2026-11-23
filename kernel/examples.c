/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "examples.h"
#include "fs.h"
#include "kheap.h"
#include "types.h"
#include "kstring.h"

char* stbi_zlib_decode_malloc(const char* buffer, int len, int* outlen);

extern const uint8_t ex_hello[], ex_hello_end[], ex_guess[], ex_guess_end[], ex_paint[], ex_paint_end[];
extern const uint8_t ex_clock[], ex_clock_end[], ex_tones[], ex_tones_end[], ex_mandel[], ex_mandel_end[];
extern const uint8_t ex_threads[], ex_threads_end[];
extern const uint8_t ex_webview[], ex_webview_end[];
extern const uint8_t ex_gears[], ex_gears_end[];
extern const uint8_t ex_cltest[], ex_cltest_end[];
extern const uint8_t ex_gl3test[], ex_gl3test_end[];
extern const uint8_t ex_systest[], ex_systest_end[];
extern const uint8_t ex_posixtest[], ex_posixtest_end[];
extern const uint8_t ex_mousetest[], ex_mousetest_end[];
extern const uint8_t ex_shelltest[], ex_shelltest_end[];

static const struct { const char* name; const uint8_t* start; const uint8_t* end; } g_examples[] = {
    { "hello.bpk", ex_hello, ex_hello_end },
    { "guess.bpk", ex_guess, ex_guess_end },
    { "paint.bpk", ex_paint, ex_paint_end },
    { "clock.bpk", ex_clock, ex_clock_end },
    { "tones.bpk", ex_tones, ex_tones_end },
    { "mandel.bpk", ex_mandel, ex_mandel_end },
    { "threads.bpk", ex_threads, ex_threads_end },
    { "webview.bpk", ex_webview, ex_webview_end },
    { "gears.bpk", ex_gears, ex_gears_end },
    { "cltest.bpk", ex_cltest, ex_cltest_end },
    { "gl3test.bpk", ex_gl3test, ex_gl3test_end },
    { "systest.bpk", ex_systest, ex_systest_end },
    { "posixtest.bpk", ex_posixtest, ex_posixtest_end },
    { "mousetest.bpk", ex_mousetest, ex_mousetest_end },
    { "shelltest.bpk", ex_shelltest, ex_shelltest_end },
};

/* the old place (systems before build 254): the user's home folder */
#define OLD_DIR "/home/banana/Examples"

static uint32_t stamp(void) {
    uint32_t s = 2166136261u;
    for (unsigned i = 0; i < sizeof(g_examples) / sizeof(g_examples[0]); i++) {
        s = (s ^ (uint32_t)(g_examples[i].end - g_examples[i].start)) * 16777619u;
        for (const char* c = g_examples[i].name; *c; c++) s = (s ^ (uint8_t)*c) * 16777619u;
    }
    return s;
}

static int is_example(const char* name) {
    /* (and the SDK's other examples, which some builds put there too) */
    static const char* const other[] = { "glinfo.bpk", "gles2.bpk", "languages.bpk" };
    for (unsigned i = 0; i < sizeof(g_examples) / sizeof(g_examples[0]); i++)
        if (!strcmp(name, g_examples[i].name)) return 1;
    for (unsigned i = 0; i < sizeof(other) / sizeof(other[0]); i++)
        if (!strcmp(name, other[i])) return 1;
    return 0;
}

/* ~/Examples, from an older system: gone if it holds nothing but the
 * examples it was given (anything the user put there keeps it all) */
static void move_old(void) {
    if (fs_find_dir(OLD_DIR) < 0) return;
    int idx[64];
    int n = fs_list_files(OLD_DIR, idx, 64), dirs = fs_list_dirs(OLD_DIR, NULL, 0);
    if (n < 0 || n > 64 || dirs > 0) return;
    for (int i = 0; i < n; i++) {
        fs_file_t* f = fs_file_info(idx[i]);
        if (!f || !is_example(f->name)) return;
    }
    fs_delete(OLD_DIR, 1);
}

void examples_seed(void) {
    move_old();
    if (fs_mkdir_p(EXAMPLES_DIR) < 0) return;
    /* written again only when this kernel's examples changed (.stamp):
     * an installed system does not rewrite them at every start */
    char want[16];
    ksnprintf(want, sizeof(want), "%08x\n", stamp());
    int si = fs_find_file(EXAMPLES_DIR "/.stamp");
    fs_file_t* sf = si >= 0 ? fs_get_file(si) : NULL;
    int same = sf && sf->content && sf->size == 9 && !memcmp(sf->content, want, 9);
    for (unsigned i = 0; i < sizeof(g_examples) / sizeof(g_examples[0]); i++) {
        char path[FS_PATH_LEN];
        ksnprintf(path, sizeof(path), EXAMPLES_DIR "/%s", g_examples[i].name);
        if (same && fs_find_file(path) >= 0) continue;
        /* (the kernel has them compressed: kernel/exbin.asm) */
        int len = 0;
        char* bpk = stbi_zlib_decode_malloc((const char*)g_examples[i].start, (int)(g_examples[i].end - g_examples[i].start), &len);
        if (bpk && len > 0) fs_write_path(path, bpk, (uint32_t)len);
        kfree(bpk);
    }
    if (!same) fs_write_path(EXAMPLES_DIR "/.stamp", want, 9);
}

/* the App Store's builtin: source serves them too (kernel/builtin_apps.c);
 * examples_data: the compressed package, as the kernel has it */
int examples_count(void) { return (int)(sizeof(g_examples) / sizeof(g_examples[0])); }
const char* examples_file(int i) { return i >= 0 && i < examples_count() ? g_examples[i].name : ""; }
const uint8_t* examples_data(int i, uint32_t* len) {
    if (i < 0 || i >= examples_count()) return 0;
    *len = (uint32_t)(g_examples[i].end - g_examples[i].start);
    return g_examples[i].start;
}
