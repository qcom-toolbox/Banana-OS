#include "wpcmd.h"
#include "../kernel/terminal.h"
#include "../kernel/kstring.h"
#include "../kernel/kheap.h"
#include "../kernel/timer.h"
#include "../kernel/fs.h"
#include "../kernel/fb.h"
#include "../kernel/gui.h"
#include "../kernel/wallpaper.h"
#include "../net/net.h"
#include "../net/http.h"

/* `wallpaper` - pick the desktop wallpaper from the shell. */

static void out(const char* fmt, ...) {
    char buf[256];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    terminal_write(buf);
}

static void fail(const char* msg) {
    terminal_write_color("wallpaper: ", VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    terminal_write_color(msg, VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    terminal_putchar('\n');
}

static void usage(void) {
    terminal_writeln("usage: wallpaper                       show the current wallpaper");
    terminal_writeln("       wallpaper list                  built-in presets + your ~/Pictures");
    terminal_writeln("       wallpaper <image> [mode]        use a PNG/JPEG/BMP/GIF file");
    terminal_writeln("       wallpaper url <URL> [mode]      download to ~/Pictures and use it");
    terminal_writeln("       wallpaper preset <name|number>  use a built-in wallpaper");
    terminal_writeln("       wallpaper reset                 back to the default (Azure Flow)");
    terminal_writeln("  mode: fill (default, crop to cover) | fit (letterbox) | stretch | center");
}

static void show_current(void) {
    const char* f = wallpaper_current_file();
    if (f[0]) out("Current wallpaper: %s (%s)\n", f, image_mode_name(wallpaper_current_mode()));
    else out("Current wallpaper: %s (built-in)\n", wallpaper_preset(wallpaper_current_preset())->name);
}

static void cmd_list(void) {
    terminal_write_color("Built-in presets:\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    for (int i = 0; i < wallpaper_preset_count(); i++)
        out("  %2d  %s%s\n", i + 1, wallpaper_preset(i)->name,
            i == wallpaper_current_preset() ? "   <- current" : "");
    terminal_write_color("Your pictures (" WALLPAPER_DIR "):\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    int idx[64];
    int n = fs_list_files(WALLPAPER_DIR, idx, 64), shown = 0;
    for (int i = 0; i < n && i < 64; i++) {
        fs_file_t* f = fs_get_file(idx[i]);
        if (!wallpaper_is_image_name(f->name)) continue;
        char path[FS_PATH_LEN];
        fs_file_path(idx[i], path, sizeof(path));
        out("      %-32s %8u bytes%s\n", f->name, f->size,
            strcmp(path, wallpaper_current_file()) == 0 ? "   <- current" : "");
        shown++;
    }
    if (!shown) terminal_writeln("      (none yet - try: wallpaper url https://.../picture.jpg)");
}

static int apply_file(const char* path, image_mode_t mode) {
    char err[128];
    uint32_t t0 = timer_ms();
    out("Decoding %s...\n", path);
    if (wallpaper_set_file(path, mode, err, sizeof(err)) != 0) {
        fail(err);
        return -1;
    }
    out("Wallpaper set: %s (%s, %u ms)%s\n", wallpaper_current_file(), image_mode_name(mode),
        timer_ms() - t0, gui_is_enabled() ? "" : " - run `startx` to see it");
    return 0;
}

/* ── wallpaper url ──────────────────────────────────────────────── */

typedef struct {
    int      file_idx;
    int      failed;
    uint32_t last_ms, start_ms;
} dl_ctx_t;

static void dl_headers(void* ctx, const http_response_t* r, const char* raw) {
    (void)raw;
    dl_ctx_t* d = (dl_ctx_t*)ctx;
    if (r->status >= 200 && r->status < 300) {
        fs_write(d->file_idx, "", 0);    /* drop any redirect page body */
        d->start_ms = timer_ms();
    }
}

static int dl_body(void* ctx, const uint8_t* data, uint32_t len) {
    dl_ctx_t* d = (dl_ctx_t*)ctx;
    if (fs_append(d->file_idx, data, len) != 0) { d->failed = 1; return -1; }
    return 0;
}

static void dl_progress(void* ctx, uint32_t got, int32_t total) {
    dl_ctx_t* d = (dl_ctx_t*)ctx;
    uint32_t now = timer_ms();
    if (now - d->last_ms < 250) return;
    d->last_ms = now;
    if (total > 0) out("\r  downloading... %u / %d KiB (%u%%)   ", got / 1024u, total / 1024,
                       (uint32_t)(((uint64_t)got * 100u) / (uint32_t)total));
    else out("\r  downloading... %u KiB   ", got / 1024u);
}

static void cmd_url(const char* url, image_mode_t mode) {
    if (!net_if()->dev) { fail("no network card"); return; }
    url_t u;
    if (!url_parse(url, &u)) { fail("malformed URL"); return; }

    /* ~/Pictures/<last path segment>, keeping an image extension */
    char name[FS_NAME_LEN];
    {
        char p[1024];
        kstrlcpy(p, u.path, sizeof(p));
        char* q = strchr(p, '?');
        if (q) *q = '\0';
        char* s = strrchr(p, '/');
        kstrlcpy(name, (s && s[1]) ? s + 1 : "download", sizeof(name));
        if (!wallpaper_is_image_name(name)) {
            /* e.g. ".../photo?w=1920": the real type is sniffed on decode */
            name[FS_NAME_LEN - 5] = '\0';
            kstrlcat(name, ".jpg", sizeof(name));
        }
    }
    fs_mkdir_p(WALLPAPER_DIR);
    char path[FS_PATH_LEN];
    ksnprintf(path, sizeof(path), "%s/%s", WALLPAPER_DIR, name);

    dl_ctx_t d;
    memset(&d, 0, sizeof(d));
    d.file_idx = fs_create(path);
    if (d.file_idx < 0 || fs_write(d.file_idx, "", 0) != 0) { fail("cannot create the file"); return; }

    http_request_t req;
    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.follow_redirects = 1;
    req.max_redirects = 20;
    req.user_agent = "Wget/1.21 (BananaOS 0.5)";
    req.ctx = &d;
    req.on_headers = dl_headers;
    req.on_body = dl_body;
    req.on_progress = dl_progress;

    http_response_t* resp = (http_response_t*)kmalloc(sizeof(http_response_t));
    if (!resp) { fail("out of memory"); return; }
    char emsg[200];
    out("Downloading %s\n", url);
    int rc = http_fetch(url, &req, resp, emsg, sizeof(emsg));
    terminal_putchar('\r');
    if (d.failed) {
        fail("download too big for memory");
    } else if (rc != NET_OK) {
        fail(emsg);
    } else if (resp->status < 200 || resp->status >= 300) {
        char m[96];
        ksnprintf(m, sizeof(m), "server said %d %s", resp->status, resp->reason);
        fail(m);
    } else {
        out("Saved %u bytes to %s                    \n", resp->body_bytes, path);
        apply_file(path, mode);
        kfree(resp);
        return;
    }
    kfree(resp);
    fs_delete(path, 0);
}

void cmd_wallpaper(int argc, char** argv) {
    if (!fb_available()) {
        fail("no graphical framebuffer - wallpapers need the GUI mode");
        return;
    }
    if (argc < 2) {
        show_current();
        terminal_writeln("");
        usage();
        return;
    }
    const char* a = argv[1];
    image_mode_t mode = IMAGE_FILL;

    if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0 || strcmp(a, "help") == 0) { usage(); return; }
    if (strcmp(a, "list") == 0) { cmd_list(); return; }
    if (strcmp(a, "reset") == 0) {
        wallpaper_set_preset(WALLPAPER_DEFAULT_PRESET);
        show_current();
        return;
    }
    if (strcmp(a, "preset") == 0) {
        if (argc < 3) { fail("which preset? see: wallpaper list"); return; }
        /* allow unquoted multi-word names: wallpaper preset Ocean Wave */
        char name[64] = "";
        for (int i = 2; i < argc; i++) {
            if (i > 2) kstrlcat(name, " ", sizeof(name));
            kstrlcat(name, argv[i], sizeof(name));
        }
        int p = wallpaper_find_preset(name);
        if (p < 0) { fail("no such preset (see: wallpaper list)"); return; }
        wallpaper_set_preset(p);
        show_current();
        return;
    }
    if (strcmp(a, "url") == 0) {
        if (argc < 3) { fail("usage: wallpaper url <URL> [mode]"); return; }
        if (argc >= 4 && !image_mode_parse(argv[3], &mode)) { fail("mode must be fill, fit, stretch or center"); return; }
        cmd_url(argv[2], mode);
        return;
    }
    if (argc >= 3 && !image_mode_parse(argv[2], &mode)) { fail("mode must be fill, fit, stretch or center"); return; }
    /* a bare name also finds pictures in ~/Pictures */
    const char* path = a;
    char alt[FS_PATH_LEN];
    if (fs_find_file(path) < 0 && !strchr(path, '/')) {
        ksnprintf(alt, sizeof(alt), "%s/%s", WALLPAPER_DIR, path);
        if (fs_find_file(alt) >= 0) path = alt;
    }
    if (fs_find_file(path) < 0) {
        int p = wallpaper_find_preset(a);
        if (p >= 0) { wallpaper_set_preset(p); show_current(); return; }
        char m[160];
        ksnprintf(m, sizeof(m), "no such file: %s", a);
        fail(m);
        return;
    }
    apply_file(path, mode);
}
