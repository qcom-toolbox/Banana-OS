/*
 * Files: the file explorer, laid out like Windows 7's Explorer.
 *
 *   [<][>]  Computer > home > banana            [Search banana    ]
 *   Organize ▾   New folder   Open in terminal         Details | Icons
 *   +-----------------+-----------------------------------------------+
 *   | * Favorites     | Name                 Type          Size       |
 *   |    Downloads    | [] Documents         File folder              |
 *   |    banana       | [] photo.png         PNG image     1.2 MB     |
 *   | # Libraries     |                                               |
 *   |    Documents... |                                               |
 *   | = Computer      |                                               |
 *   |    Local Disk   |                                               |
 *   +-----------------+-----------------------------------------------+
 *   | [icon]  photo.png                                               |
 *   |         PNG image   1.2 MB                                      |
 *   +-----------------------------------------------------------------+
 *
 * Back / forward history, a breadcrumb address bar (click a part to go
 * there, click beside it to type a path), search in the folder, Details
 * (sortable columns) or Large icons, a navigation pane and a details pane
 * with a picture's thumbnail. Right-click menus, copy / cut / paste,
 * rename (F2), delete, new folder, apps (.bpk) install, media open in
 * their app, pictures open in Photos, fonts install, USB sticks eject.
 */
#include "explorer.h"
#include "keyboard.h"
#include "utf8.h"
#include "gfx.h"
#include "fs.h"
#include "kstring.h"
#include "kheap.h"
#include "timer.h"
#include "image.h"
#include "wallpaper.h"
#include "settings.h"
#include "winframe.h"
#include "gui.h"
#include "pkg.h"
#include "audio.h"
#include "ctxmenu.h"
#include "blockdev.h"
#include "fileicons.h"
#include "rtc.h"

#define HOME      "/home/banana"
#define TITLE_H   20
#define NAVBAR_Y  22                 /* back / forward, address, search */
#define NAVBAR_H  30
#define CMD_Y     (NAVBAR_Y + NAVBAR_H)
#define CMD_H     26
#define BODY_Y    (CMD_Y + CMD_H)
#define DET_H     62                 /* the details pane at the bottom */
#define PANE_W    170                /* the navigation pane */
#define SB_W      15
#define HDR_H     22                 /* the column headers (Details) */
#define ROW_H     20
#define CELL_W    96                 /* Large icons */
#define CELL_H    86
#define MAX_ITEMS 320
#define THUMB     52

/* Windows 7 colours */
#define C_FRAME    0x001D232Cu
#define C_TITLE    0x00384562u
#define C_NAVBAR   0x00D6E3F3u
#define C_NAVBAR2  0x00C3D5EDu
#define C_CMD      0x00EEF3FAu
#define C_CMD2     0x00DDE7F3u
#define C_LINE     0x00C9D6E6u
#define C_WHITE    0x00FFFFFFu
#define C_PANE     0x00F6F9FDu
#define C_TEXT     0x001E1E1Eu
#define C_DIM      0x005B6B7Fu
#define C_NAVTEXT  0x001E395Bu
#define C_HOVER    0x00E5F3FFu
#define C_HOVER_B  0x00CCE8FFu
#define C_SEL      0x00CCE8FFu
#define C_SEL_B    0x0099D1FFu
#define C_BOX_B    0x008AA0BBu
#define C_WARN     0x00B5452Fu
#define C_DET      0x00E8EFF8u

typedef struct {
    int is_dir;
    int idx;                            /* fs_get_dir / fs_get_file index */
} item_t;

static int      g_open;
static win_geom_t g_win = { .x = 24, .y = 24, .w = 760, .h = 500, .min_w = 560, .min_h = 330 };
#define g_x g_win.x
#define g_y g_win.y
#define WIN_W g_win.w
#define WIN_H g_win.h
static char     g_path[FS_PATH_LEN] = HOME;
static item_t   g_items[MAX_ITEMS];
static int      g_count;
static int      g_scroll;               /* first visible row (Details) / row of cells (Icons) */
static int      g_sel = -1, g_hover = -1;
static int      g_confirm_delete;
static uint32_t g_last_click_ms;
static int      g_last_click_row = -1;
static char     g_status[112];
static int      g_status_warn;
static uint32_t g_gen;                  /* bumped on every state change */

static int      g_view_icons;           /* 0 Details, 1 Large icons */
static int      g_sort = 0, g_sort_desc;  /* 0 name, 1 date modified, 2 type, 3 size */

/* history */
#define HIST 24
static char     g_back[HIST][FS_PATH_LEN], g_fwd[HIST][FS_PATH_LEN];
static int      g_nback, g_nfwd;

/* the address bar being typed in, the search box */
static int      g_addr_edit;
static char     g_addr[FS_PATH_LEN];
static int      g_search_focus;
static char     g_search[48];

/* hover: the navigation pane row, the toolbar control under the pointer */
static int      g_hover_nav = -1, g_hover_tool = -1;

/* picture thumbnail for the details pane */
static uint32_t* g_thumb;
static int      g_thumb_for = -1;

static void select_name(const char* name);
static void ensure_visible(void);

/* ── helpers ──────────────────────────────────────────────────────── */

static int inside(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

static void frame(int x, int y, int w, int h, uint32_t fill, uint32_t border) {
    gfx_fill_rect(x, y, w, h, border);
    gfx_fill_rect(x + 1, y + 1, w - 2, h - 2, fill);
}

/* a vertical two-colour gradient (bands of 2 pixels) */
static void gradient(int x, int y, int w, int h, uint32_t top, uint32_t bot) {
    for (int i = 0; i < h; i += 2) {
        uint32_t c = 0;
        for (int s = 0; s < 24; s += 8) {
            int a = (int)((top >> s) & 255), b = (int)((bot >> s) & 255);
            c |= (uint32_t)(a + (b - a) * i / (h > 1 ? h - 1 : 1)) << s;
        }
        gfx_fill_rect(x, y + i, w, i + 2 <= h ? 2 : 1, c);
    }
}

/* text clipped to `max` characters, "..." when cut */
static void draw_clip(int x, int y, const char* s, int max, uint32_t fg, uint32_t bg) {
    char buf[96];
    if (max < 1) return;
    int n = (int)strlen(s);
    if (max > (int)sizeof(buf) - 1) max = (int)sizeof(buf) - 1;
    if (n <= max) { gfx_draw_text(x, y, s, fg, bg); return; }
    if (max <= 3) { memcpy(buf, s, (size_t)max); buf[max] = 0; gfx_draw_text(x, y, buf, fg, bg); return; }
    memcpy(buf, s, (size_t)(max - 3));
    memcpy(buf + max - 3, "...", 4);
    gfx_draw_text(x, y, buf, fg, bg);
}

static void human_size(uint32_t n, char* out, int cap) {
    if (n < 1024) ksnprintf(out, (size_t)cap, "%u bytes", n);
    else if (n < 1024 * 1024) ksnprintf(out, (size_t)cap, "%u KB", (n + 1023) / 1024);
    else ksnprintf(out, (size_t)cap, "%u.%u MB", n >> 20, ((n >> 10) & 1023) * 10 / 1024);
}

static const char* item_name(const item_t* it) {
    if (it->is_dir) { const fs_dir_t* d = fs_get_dir(it->idx); return d ? d->name : "?"; }
    fs_file_t* f = fs_file_info(it->idx);
    return (f && f->used) ? f->name : "?";
}

static uint32_t item_mtime(const item_t* it) {
    if (it->is_dir) { const fs_dir_t* d = fs_get_dir(it->idx); return d ? d->mtime : 0; }
    fs_file_t* f = fs_file_info(it->idx);
    return f ? f->mtime : 0;
}

static uint32_t item_ctime(const item_t* it) {
    if (it->is_dir) { const fs_dir_t* d = fs_get_dir(it->idx); return d ? d->ctime : 0; }
    fs_file_t* f = fs_file_info(it->idx);
    return f ? f->ctime : 0;
}

static uint32_t item_size(const item_t* it) {
    if (it->is_dir) return 0;
    fs_file_t* f = fs_file_info(it->idx);
    return f ? f->size : 0;
}

static void child_path(const char* name, char* out, int cap) {
    if (strcmp(g_path, "/") == 0) ksnprintf(out, (size_t)cap, "/%s", name);
    else ksnprintf(out, (size_t)cap, "%s/%s", g_path, name);
}

static const char* base_name(const char* path) {
    if (strcmp(path, "/") == 0) return "Local Disk (/)";
    const char* s = strrchr(path, '/');
    return s && s[1] ? s + 1 : path;
}

static void set_status_c(const char* s, int warn) {
    kstrlcpy(g_status, s, sizeof(g_status));
    g_status_warn = warn;
    g_gen++;
}
static void set_status(const char* s) { set_status_c(s, 0); }

static void drop_thumb(void) {
    kfree(g_thumb);
    g_thumb = NULL;
    g_thumb_for = -1;
}

static int has_ext(const char* name, const char* ext) {
    const char* dot = strrchr(name, '.');
    return dot && strcasecmp(dot, ext) == 0;
}

static int is_image_file(int fidx) {
    fs_file_t* f = fs_file_info(fidx);
    return f && f->used && wallpaper_is_image_name(f->name);
}

/* case-insensitive "contains" */
static int matches(const char* name, const char* q) {
    if (!q[0]) return 1;
    size_t n = strlen(q);
    for (const char* p = name; *p; p++) if (strncasecmp(p, q, n) == 0) return 1;
    return 0;
}

static void type_of(const item_t* it, char* buf, int cap) {
    if (it->is_dir) kstrlcpy(buf, "File folder", (size_t)cap);
    else fileicon_type_name(item_name(it), buf, cap);
}

static fileicon_t icon_of(const item_t* it) {
    return it->is_dir ? FI_FOLDER : fileicon_for_name(item_name(it));
}

/* ── the folder's contents ───────────────────────────────────────── */

static int before(const item_t* a, const item_t* b) {
    if (a->is_dir != b->is_dir) return a->is_dir;            /* folders first, always */
    int c = 0;
    if (g_sort == 1) {
        uint32_t ma = item_mtime(a), mb = item_mtime(b);
        c = ma < mb ? -1 : ma > mb ? 1 : 0;
    } else if (g_sort == 2) {
        char ta[40], tb[40];
        type_of(a, ta, sizeof(ta));
        type_of(b, tb, sizeof(tb));
        c = strcasecmp(ta, tb);
    } else if (g_sort == 3) {
        uint32_t sa = item_size(a), sb = item_size(b);
        c = sa < sb ? -1 : sa > sb ? 1 : 0;
    }
    if (!c) c = strcasecmp(item_name(a), item_name(b));
    return g_sort_desc ? c > 0 : c < 0;
}

static int content_rows(void);
static int per_row(void);

static int max_scroll(void) {
    int lines = g_view_icons ? (g_count + per_row() - 1) / per_row() : g_count;
    int m = lines - content_rows();
    return m > 0 ? m : 0;
}

static void clamp_scroll(void) {
    if (g_scroll > max_scroll()) g_scroll = max_scroll();
    if (g_scroll < 0) g_scroll = 0;
}

static void scan(void) {
    static int d[FS_MAX_DIRS], f[FS_MAX_FILES];   /* static: the GUI runs on 16 KiB task stacks */
    int nd = fs_list_dirs(g_path, d, FS_MAX_DIRS);
    if (nd < 0) {                       /* folder vanished: go home */
        kstrlcpy(g_path, HOME, sizeof(g_path));
        nd = fs_list_dirs(g_path, d, FS_MAX_DIRS);
        if (nd < 0) { kstrlcpy(g_path, "/", sizeof(g_path)); nd = fs_list_dirs(g_path, d, FS_MAX_DIRS); }
    }
    int nf = fs_list_files(g_path, f, FS_MAX_FILES);
    if (nd > FS_MAX_DIRS) nd = FS_MAX_DIRS;
    if (nf > FS_MAX_FILES) nf = FS_MAX_FILES;
    g_count = 0;
    for (int i = 0; i < nd && g_count < MAX_ITEMS; i++) {
        item_t it = { 1, d[i] };
        if (matches(item_name(&it), g_search)) g_items[g_count++] = it;
    }
    for (int i = 0; i < nf && g_count < MAX_ITEMS; i++) {
        item_t it = { 0, f[i] };
        if (matches(item_name(&it), g_search)) g_items[g_count++] = it;
    }
    for (int i = 1; i < g_count; i++) {             /* insertion sort: a few hundred at most */
        item_t t = g_items[i];
        int j = i - 1;
        while (j >= 0 && before(&t, &g_items[j])) { g_items[j + 1] = g_items[j]; j--; }
        g_items[j + 1] = t;
    }
    if (g_sel >= g_count) g_sel = -1;
    if (g_hover >= g_count) g_hover = -1;
    clamp_scroll();
}

/* to path; `record`: the folder left goes into the back history */
static void go_to(const char* path, int record) {
    if (record && strcmp(path, g_path) != 0) {
        if (g_nback == HIST) { memmove(g_back[0], g_back[1], sizeof(g_back[0]) * (HIST - 1)); g_nback--; }
        kstrlcpy(g_back[g_nback++], g_path, FS_PATH_LEN);
        g_nfwd = 0;
    }
    kstrlcpy(g_path, path, sizeof(g_path));
    g_sel = -1;
    g_hover = -1;
    g_scroll = 0;
    g_confirm_delete = 0;
    g_status[0] = '\0';
    g_search[0] = 0;
    g_search_focus = 0;
    g_addr_edit = 0;
    drop_thumb();
    scan();
    g_gen++;
}
static void go(const char* path) { go_to(path, 1); }

static void go_back(void) {
    if (!g_nback) return;
    if (g_nfwd < HIST) kstrlcpy(g_fwd[g_nfwd++], g_path, FS_PATH_LEN);
    char p[FS_PATH_LEN];
    kstrlcpy(p, g_back[--g_nback], sizeof(p));
    go_to(p, 0);
}

static void go_forward(void) {
    if (!g_nfwd) return;
    if (g_nback < HIST) kstrlcpy(g_back[g_nback++], g_path, FS_PATH_LEN);
    char p[FS_PATH_LEN];
    kstrlcpy(p, g_fwd[--g_nfwd], sizeof(p));
    go_to(p, 0);
}

static void go_up(void) {
    if (strcmp(g_path, "/") == 0) return;
    char p[FS_PATH_LEN];
    kstrlcpy(p, g_path, sizeof(p));
    char* slash = strrchr(p, '/');
    if (slash == p) p[1] = '\0';
    else if (slash) *slash = '\0';
    go(p);
}

/* a library folder in the home folder, made the first time it is opened */
static void go_library(const char* name) {
    char p[FS_PATH_LEN];
    ksnprintf(p, sizeof(p), "%s/%s", HOME, name);
    if (fs_find_dir(p) < 0) fs_mkdir(p);
    go(p);
}

/* ── public API ───────────────────────────────────────────────────── */

void explorer_open(const char* path) {
    g_open = 1;
    g_win.dragging = g_win.resizing = 0;
    win_clamp(&g_win);
    go_to(path && path[0] ? path : HOME, g_path[0] && path && path[0]);
}

void explorer_select(const char* name) {
    scan();
    g_sel = -1;
    select_name(name);
    ensure_visible();
    g_gen++;
}

void explorer_close(void) {
    g_open = 0;
    g_win.dragging = g_win.resizing = 0;
    drop_thumb();
    g_gen++;
}

int explorer_is_open(void) { return g_open; }

int explorer_contains(int mx, int my) {
    return g_open && inside(mx, my, g_x, g_y, WIN_W, WIN_H);
}

uint32_t explorer_signature(void) {
    if (!g_open) return 0;
    /* files changed by a terminal meanwhile show up too */
    return g_gen * 2654435761u ^ fs_used_files() * 40503u ^ fs_used_dirs() * 977u ^
           fs_ram_used_bytes() ^ (uint32_t)(g_x << 16 | g_y) ^ (uint32_t)(g_win.w << 20 | g_win.h << 8) ^
           (uint32_t)(timer_ms() / 500) * (uint32_t)(g_search_focus || g_addr_edit || 0);   /* (the caret blinks) */
}

/* ── actions ──────────────────────────────────────────────────────── */

static void select_name(const char* name) {
    for (int i = 0; i < g_count; i++) if (strcmp(item_name(&g_items[i]), name) == 0) g_sel = i;
}

static void new_folder(void) {
    char name[FS_NAME_LEN], path[FS_PATH_LEN];
    g_search[0] = 0;
    for (int n = 1; n < 100; n++) {
        if (n == 1) kstrlcpy(name, "New folder", sizeof(name));
        else ksnprintf(name, sizeof(name), "New folder (%d)", n);
        child_path(name, path, sizeof(path));
        if (fs_find_dir(path) >= 0 || fs_find_file(path) >= 0) continue;
        if (fs_mkdir(path) < 0) { set_status_c("Could not create the folder", 1); return; }
        scan();
        select_name(name);
        extern void explorer_start_rename(void);
        explorer_start_rename();            /* type its name right away, like Windows */
        return;
    }
}

static void delete_selected(void) {
    if (g_sel < 0) return;
    char name[FS_NAME_LEN], path[FS_PATH_LEN], cwd[FS_PATH_LEN], msg[112];
    kstrlcpy(name, item_name(&g_items[g_sel]), sizeof(name));
    child_path(name, path, sizeof(path));
    if (!g_confirm_delete) {
        g_confirm_delete = 1;
        ksnprintf(msg, sizeof(msg), "Delete \"%s\"%s? Press Delete again (Esc: no)", name,
                  g_items[g_sel].is_dir ? " and all it contains" : "");
        set_status_c(msg, 1);
        return;
    }
    g_confirm_delete = 0;
    if (g_items[g_sel].is_dir) {
        /* the shells' working directory (or a parent of it) stays */
        fs_cwd_path(cwd, sizeof(cwd));
        size_t pl = strlen(path);
        if (strncmp(cwd, path, pl) == 0 && (cwd[pl] == '\0' || cwd[pl] == '/')) {
            set_status_c("That folder is a terminal's current folder - cd elsewhere first", 1);
            return;
        }
    }
    fs_delete(path, 1);
    drop_thumb();
    g_sel = -1;
    scan();
    ksnprintf(msg, sizeof(msg), "Deleted \"%s\"", name);
    set_status(msg);
}

static void install_package(const char* path) {
    char msg[160], m2[112];
    set_status("Installing...");
    if (pkg_install(path, msg, sizeof(msg)) == 0) { kstrlcpy(m2, msg, sizeof(m2)); set_status(m2); }
    else { ksnprintf(m2, sizeof(m2), "Not installed: %s", msg); set_status_c(m2, 1); }
}

static void play_sound(const char* path) {
    char err[80], msg[112];
    if (audio_play_wav(path, err, sizeof(err)) == 0) { ksnprintf(msg, sizeof(msg), "Playing %s", strrchr(path, '/') + 1); set_status(msg); }
    else { ksnprintf(msg, sizeof(msg), "Cannot play it: %s", err); set_status_c(msg, 1); }
}

/* videos open in the Media Player, songs in Music (or the Media Player) */
static int ext_in(const char* name, const char* const* list) {
    const char* dot = strrchr(name, '.');
    if (!dot) return 0;
    for (int i = 0; list[i]; i++) if (strcasecmp(dot + 1, list[i]) == 0) return 1;
    return 0;
}
static const char* const VIDEO_EXT[] = { "mp4", "m4v", "mkv", "webm", "avi", "mov", "mpg", "mpeg", "ts", "m2ts", "ogv", "flv", "wmv", "asf", "3gp", NULL };
static const char* const AUDIO_EXT[] = { "mp3", "m4a", "aac", "flac", "ogg", "oga", "opus", "wma", "ac3", "wav", NULL };
static int is_video(const char* name) { return ext_in(name, VIDEO_EXT); }
static int is_audio(const char* name) { return ext_in(name, AUDIO_EXT); }

static const char* media_app(const char* name) {
    pkg_info_t pi;
    if (is_audio(name) && pkg_get("music", &pi) == 0) return "music";
    if ((is_video(name) || is_audio(name)) && pkg_get("mediaplayer", &pi) == 0) return "mediaplayer";
    return NULL;
}

static int open_media(const char* path, const char* name) {
    const char* app = media_app(name);
    if (!app) return 0;
    char err[96], msg[112];
    char* argv[2] = { (char*)app, (char*)path };
    if (pkg_run(app, 2, argv, 1, err, sizeof(err)) < 0) { ksnprintf(msg, sizeof(msg), "Cannot play it: %s", err); set_status_c(msg, 1); }
    else { ksnprintf(msg, sizeof(msg), "Playing %s", name); set_status(msg); }
    return 1;
}

static void set_wallpaper(const char* path, const char* name) {
    char err[80], msg[112];
    set_status("Setting the wallpaper...");
    if (wallpaper_set_file(path, IMAGE_FILL, err, sizeof(err)) == 0) { ksnprintf(msg, sizeof(msg), "Wallpaper: %s", name); set_status(msg); }
    else { ksnprintf(msg, sizeof(msg), "Not a usable picture: %s", err); set_status_c(msg, 1); }
}

/* pictures open in Photos (else the Browser shows them) */
static void open_picture(const char* path, const char* name) {
    pkg_info_t pi;
    if (pkg_get("photos", &pi) == 0) {
        char err[96], msg[112];
        char* argv[2] = { (char*)"photos", (char*)path };
        if (pkg_run("photos", 2, argv, 1, err, sizeof(err)) >= 0) { ksnprintf(msg, sizeof(msg), "Opened %s", name); set_status(msg); return; }
    }
    gui_open_browser(path);
}

/* Banana Code (the built-in editor app): a folder or a file */
static int has_code(void) { pkg_info_t pi; return pkg_get("code", &pi) == 0; }
static void open_in_code(const char* path) {
    char err[96], msg[112];
    char* argv[2] = { (char*)"code", (char*)path };
    if (pkg_run("code", 2, argv, 1, err, sizeof(err)) < 0) { ksnprintf(msg, sizeof(msg), "Banana Code: %s", err); set_status_c(msg, 1); }
}

static int is_font_name(const char* name) { return has_ext(name, ".ttf") || has_ext(name, ".TTF"); }

static void install_font(const char* path) {
    char msg[112];
    int rc = settings_install_font(path, msg, sizeof(msg));
    set_status_c(msg, rc != 0);
}

static void open_item(int i) {
    if (i < 0 || i >= g_count) return;
    const item_t* it = &g_items[i];
    char path[FS_PATH_LEN];
    child_path(item_name(it), path, sizeof(path));
    if (it->is_dir) { go(path); return; }
    if (has_ext(item_name(it), ".bpk")) { install_package(path); return; }
    if (open_media(path, item_name(it))) return;
    if (has_ext(item_name(it), ".wav")) { play_sound(path); return; }
    if (is_video(item_name(it))) { set_status_c("Install the Media Player app to play videos", 1); return; }
    if (has_ext(item_name(it), ".html") || has_ext(item_name(it), ".htm") || has_ext(item_name(it), ".svg")) {
        gui_open_browser(path);
        return;
    }
    if (is_image_file(it->idx)) { open_picture(path, item_name(it)); return; }
    if (is_font_name(item_name(it))) { install_font(path); return; }
    gui_open_notepad(path);          /* text: the desktop's Notepad */
}

static void terminal_at(const char* path) {
    char cmd[FS_PATH_LEN + 16];
    ksnprintf(cmd, sizeof(cmd), "cd \"%s\"\n", path);
    gui_terminal_run(cmd);
}

/* ── copy / cut / paste, rename ───────────────────────────────────── */

static char g_clip[FS_PATH_LEN];      /* a file or folder copied (or cut) in Files */
static int  g_clip_cut;
static int  g_renaming = -1;          /* item being renamed */
static char g_rename[FS_NAME_LEN];

static int copy_tree(const char* src, const char* dst, int depth) {
    if (depth > 12 || fs_mkdir(dst) < 0) return -1;
    static int idx[FS_MAX_FILES];
    int rc = 0;
    int nf = fs_list_files(src, idx, FS_MAX_FILES);
    for (int i = 0; i < nf && i < FS_MAX_FILES; i++) {
        char s[FS_PATH_LEN], d[FS_PATH_LEN];
        const char* name = fs_file_info(idx[i])->name;
        ksnprintf(s, sizeof(s), "%s/%s", src, name);
        ksnprintf(d, sizeof(d), "%s/%s", dst, name);
        if (fs_copy(s, d) < 0) rc = -1;
    }
    int dirs[32];
    int nd = fs_list_dirs(src, dirs, 32);
    char names[32][FS_NAME_LEN];
    int n = nd < 32 ? nd : 32;
    for (int i = 0; i < n; i++) kstrlcpy(names[i], fs_get_dir(dirs[i])->name, FS_NAME_LEN);
    for (int i = 0; i < n; i++) {
        char s[FS_PATH_LEN], d[FS_PATH_LEN];
        ksnprintf(s, sizeof(s), "%s/%s", src, names[i]);
        ksnprintf(d, sizeof(d), "%s/%s", dst, names[i]);
        if (copy_tree(s, d, depth + 1) < 0) rc = -1;
    }
    return rc;
}

static void clip_selected(int cut) {
    if (g_sel < 0) return;
    child_path(item_name(&g_items[g_sel]), g_clip, sizeof(g_clip));
    g_clip_cut = cut;
    char msg[112];
    ksnprintf(msg, sizeof(msg), "%s \"%s\" - paste it in another folder (Ctrl+V)", cut ? "Cut" : "Copied", item_name(&g_items[g_sel]));
    set_status(msg);
}

static void paste_here(void) {
    if (!g_clip[0]) return;
    const char* base = strrchr(g_clip, '/');
    base = base ? base + 1 : g_clip;
    char dst[FS_PATH_LEN], msg[112];
    child_path(base, dst, sizeof(dst));
    for (int n = 2; (fs_find_file(dst) >= 0 || fs_find_dir(dst) >= 0) && n < 100; n++) {
        char nm[FS_NAME_LEN];
        ksnprintf(nm, sizeof(nm), "%s (%d)", base, n);
        child_path(nm, dst, sizeof(dst));
    }
    int ok;
    set_status(g_clip_cut ? "Moving..." : "Copying...");
    if (fs_find_dir(g_clip) >= 0) ok = g_clip_cut ? fs_move(g_clip, dst) >= 0 : copy_tree(g_clip, dst, 0) == 0;
    else ok = g_clip_cut ? fs_move(g_clip, dst) >= 0 : fs_copy(g_clip, dst) >= 0;
    if (ok) ksnprintf(msg, sizeof(msg), "%s \"%s\"", g_clip_cut ? "Moved" : "Pasted", base);
    else ksnprintf(msg, sizeof(msg), "Could not %s \"%s\"%s", g_clip_cut ? "move" : "copy", base,
                   fs_io_error() ? " (the USB stick reported an error)" : "");
    if (ok && g_clip_cut) g_clip[0] = 0;
    scan();
    select_name(strrchr(dst, '/') + 1);
    set_status_c(msg, !ok);
}

static void new_text_file(void) {
    char name[FS_NAME_LEN], path[FS_PATH_LEN];
    g_search[0] = 0;
    for (int n = 1; n < 100; n++) {
        if (n == 1) kstrlcpy(name, "New Text Document.txt", sizeof(name));
        else ksnprintf(name, sizeof(name), "New Text Document (%d).txt", n);
        child_path(name, path, sizeof(path));
        if (fs_find_dir(path) >= 0 || fs_find_file(path) >= 0) continue;
        if (fs_write_path(path, "", 0) < 0) { set_status_c("Could not create the file", 1); return; }
        scan();
        select_name(name);
        extern void explorer_start_rename(void);
        explorer_start_rename();
        return;
    }
}

void explorer_start_rename(void) {
    if (g_sel < 0) return;
    g_renaming = g_sel;
    kstrlcpy(g_rename, item_name(&g_items[g_sel]), sizeof(g_rename));
    set_status("Type the new name - Enter: rename, Esc: cancel");
}

static void finish_rename(void) {
    int i = g_renaming;
    g_renaming = -1;
    if (i < 0 || i >= g_count || !g_rename[0] || strchr(g_rename, '/')) { set_status("Not renamed"); return; }
    char from[FS_PATH_LEN], to[FS_PATH_LEN], msg[112];
    child_path(item_name(&g_items[i]), from, sizeof(from));
    child_path(g_rename, to, sizeof(to));
    if (strcmp(from, to) == 0) { set_status(""); return; }
    if (fs_find_file(to) >= 0 || fs_find_dir(to) >= 0) { set_status_c("That name is taken", 1); return; }
    int bad = fs_move(from, to) < 0;
    if (bad) ksnprintf(msg, sizeof(msg), "Could not rename it%s", fs_io_error() ? " (volume error)" : "");
    else ksnprintf(msg, sizeof(msg), "Renamed to \"%s\"", g_rename);
    scan();
    select_name(g_rename);
    set_status_c(msg, bad);
}

static void eject_here(void) {
    int mnt = fs_path_mount(g_path);
    if (!mnt) return;
    char point[FS_PATH_LEN], msg[112];
    kstrlcpy(point, fs_mount_point(mnt), sizeof(point));
    go(HOME);
    if (!blockdev_eject(mnt)) fs_unmount(mnt);
    ksnprintf(msg, sizeof(msg), "%s ejected - the stick can be removed", point);
    set_status(msg);
}

static void properties(void) {
    char msg[112], size[24], type[40];
    if (g_sel < 0) {
        int nd = 0, nf = 0;
        uint32_t bytes = 0;
        for (int i = 0; i < g_count; i++) { if (g_items[i].is_dir) nd++; else { nf++; bytes += item_size(&g_items[i]); } }
        human_size(bytes, size, sizeof(size));
        ksnprintf(msg, sizeof(msg), "%s: %d folders, %d files, %s", g_path, nd, nf, size);
    } else {
        const item_t* it = &g_items[g_sel];
        type_of(it, type, sizeof(type));
        if (it->is_dir) {
            char path[FS_PATH_LEN];
            int d[1], f[1];
            child_path(item_name(it), path, sizeof(path));
            int n = fs_list_dirs(path, d, 1) + fs_list_files(path, f, 1);
            ksnprintf(msg, sizeof(msg), "%s: %s, %d item%s", item_name(it), type, n, n == 1 ? "" : "s");
        } else {
            fs_file_t* f = fs_file_info(it->idx);
            human_size(f->size, size, sizeof(size));
            ksnprintf(msg, sizeof(msg), "%s: %s, %s (%u bytes)%s", f->name, type, size, f->size, f->mnt ? ", USB stick" : "");
        }
    }
    set_status(msg);
}

/* ── menus ────────────────────────────────────────────────────────── */

enum { M_OPEN = 1, M_EDIT, M_INSTALL, M_PLAY, M_WALLPAPER, M_COPY, M_CUT, M_PASTE, M_RENAME, M_DELETE,
       M_NEWFOLDER, M_NEWFILE, M_TERMINAL, M_REFRESH, M_USB, M_EJECT, M_PROPS, M_CLOSE, M_VIEW_DETAILS, M_VIEW_ICONS,
       M_SELECT_NONE, M_BROWSER, M_FONT, M_CODE };

static void menu_cb(int id, void* arg) {
    (void)arg;
    char path[FS_PATH_LEN];
    if (g_sel >= 0 && g_sel < g_count) child_path(item_name(&g_items[g_sel]), path, sizeof(path));
    else path[0] = 0;
    g_gen++;
    if (id != M_DELETE) g_confirm_delete = 0;
    switch (id) {
    case M_OPEN: if (g_sel >= 0) open_item(g_sel); break;
    case M_EDIT: if (path[0]) gui_open_notepad(path); break;
    case M_BROWSER: if (path[0]) gui_open_browser(path); break;
    case M_INSTALL: if (path[0]) install_package(path); break;
    case M_PLAY: if (path[0] && g_sel >= 0 && !open_media(path, item_name(&g_items[g_sel]))) play_sound(path); break;
    case M_WALLPAPER: if (path[0]) set_wallpaper(path, item_name(&g_items[g_sel])); break;
    case M_FONT: if (path[0]) install_font(path); break;
    case M_CODE: if (path[0]) open_in_code(path); break;
    case M_COPY: clip_selected(0); break;
    case M_CUT: clip_selected(1); break;
    case M_PASTE: paste_here(); break;
    case M_RENAME: explorer_start_rename(); break;
    case M_DELETE: g_confirm_delete = 1; delete_selected(); break;
    case M_NEWFOLDER: new_folder(); break;
    case M_NEWFILE: new_text_file(); break;
    case M_TERMINAL: terminal_at(g_sel >= 0 && g_items[g_sel].is_dir ? path : g_path); break;
    case M_REFRESH: scan(); set_status("Refreshed"); break;
    case M_USB: go("/mnt/usb"); break;
    case M_EJECT: eject_here(); break;
    case M_PROPS: properties(); break;
    case M_CLOSE: explorer_close(); break;
    case M_VIEW_DETAILS: g_view_icons = 0; g_scroll = 0; break;
    case M_VIEW_ICONS: g_view_icons = 1; g_scroll = 0; break;
    case M_SELECT_NONE: g_sel = -1; break;
    }
}

static void item_menu(int mx, int my, int row) {
    ctx_item_t items[CTX_MAX_ITEMS];
    int n = 0;
    g_renaming = -1;
    if (row >= 0) {
        g_sel = row;
        g_confirm_delete = 0;
        const item_t* it = &g_items[row];
        const char* name = item_name(it);
        items[n++] = (ctx_item_t){ "Open", M_OPEN, 0 };
        if (!it->is_dir) {
            if (has_ext(name, ".bpk")) items[n++] = (ctx_item_t){ "Install app", M_INSTALL, 0 };
            if (has_ext(name, ".wav") || media_app(name)) items[n++] = (ctx_item_t){ "Play", M_PLAY, 0 };
            if (is_image_file(it->idx)) items[n++] = (ctx_item_t){ "Set as desktop background", M_WALLPAPER, 0 };
            if (is_font_name(name)) items[n++] = (ctx_item_t){ "Install font", M_FONT, 0 };
            if (has_ext(name, ".html") || has_ext(name, ".htm") || has_ext(name, ".svg") || is_image_file(it->idx))
                items[n++] = (ctx_item_t){ "Open in Browser", M_BROWSER, 0 };
            items[n++] = (ctx_item_t){ "Edit in Notepad", M_EDIT, 0 };
            if (has_code()) items[n++] = (ctx_item_t){ "Open in Banana Code", M_CODE, 0 };
        } else {
            items[n++] = (ctx_item_t){ "Open in a terminal", M_TERMINAL, 0 };
            if (has_code()) items[n++] = (ctx_item_t){ "Open in Banana Code", M_CODE, 0 };
        }
        items[n++] = (ctx_item_t){ CTX_SEP, 0, 0 };
        items[n++] = (ctx_item_t){ "Cut", M_CUT, 0 };
        items[n++] = (ctx_item_t){ "Copy", M_COPY, 0 };
        items[n++] = (ctx_item_t){ CTX_SEP, 0, 0 };
        items[n++] = (ctx_item_t){ "Delete", M_DELETE, 0 };
        items[n++] = (ctx_item_t){ "Rename", M_RENAME, 0 };
        items[n++] = (ctx_item_t){ CTX_SEP, 0, 0 };
        items[n++] = (ctx_item_t){ "Properties", M_PROPS, 0 };
    } else {
        g_sel = -1;
        items[n++] = (ctx_item_t){ g_view_icons ? "View: Details" : "View: Large icons", g_view_icons ? M_VIEW_DETAILS : M_VIEW_ICONS, 0 };
        items[n++] = (ctx_item_t){ "Refresh", M_REFRESH, 0 };
        items[n++] = (ctx_item_t){ CTX_SEP, 0, 0 };
        items[n++] = (ctx_item_t){ "Paste", M_PASTE, g_clip[0] == 0 };
        items[n++] = (ctx_item_t){ CTX_SEP, 0, 0 };
        items[n++] = (ctx_item_t){ "New folder", M_NEWFOLDER, 0 };
        items[n++] = (ctx_item_t){ "New text document", M_NEWFILE, 0 };
        items[n++] = (ctx_item_t){ "Open a terminal here", M_TERMINAL, 0 };
        items[n++] = (ctx_item_t){ CTX_SEP, 0, 0 };
        if (fs_path_mount(g_path)) items[n++] = (ctx_item_t){ "Eject this USB stick", M_EJECT, 0 };
        items[n++] = (ctx_item_t){ "Properties", M_PROPS, 0 };
    }
    g_gen++;
    ctxmenu_open(mx, my, items, n, menu_cb, NULL);
}

static void organize_menu(int mx, int my) {
    ctx_item_t items[CTX_MAX_ITEMS];
    int n = 0, none = g_sel < 0;
    items[n++] = (ctx_item_t){ "Cut", M_CUT, none };
    items[n++] = (ctx_item_t){ "Copy", M_COPY, none };
    items[n++] = (ctx_item_t){ "Paste", M_PASTE, g_clip[0] == 0 };
    items[n++] = (ctx_item_t){ CTX_SEP, 0, 0 };
    items[n++] = (ctx_item_t){ "Select none", M_SELECT_NONE, none };
    items[n++] = (ctx_item_t){ CTX_SEP, 0, 0 };
    items[n++] = (ctx_item_t){ "Delete", M_DELETE, none };
    items[n++] = (ctx_item_t){ "Rename", M_RENAME, none };
    items[n++] = (ctx_item_t){ "New text document", M_NEWFILE, 0 };
    items[n++] = (ctx_item_t){ CTX_SEP, 0, 0 };
    items[n++] = (ctx_item_t){ "Properties", M_PROPS, 0 };
    items[n++] = (ctx_item_t){ "Refresh", M_REFRESH, 0 };
    items[n++] = (ctx_item_t){ CTX_SEP, 0, 0 };
    items[n++] = (ctx_item_t){ "Close", M_CLOSE, 0 };
    ctxmenu_open(mx, my, items, n, menu_cb, NULL);
}

/* ── geometry ─────────────────────────────────────────────────────── */

static int body_h(void) { return WIN_H - BODY_Y - DET_H - 3; }
static int content_x(void) { return PANE_W + 4; }          /* relative to the window */
static int content_w(void) { return WIN_W - content_x() - 3; }
static int list_top(void) { return BODY_Y + (g_view_icons ? 4 : HDR_H); }
static int content_rows(void) {
    int h = body_h() - (g_view_icons ? 4 : HDR_H);
    int r = h / (g_view_icons ? CELL_H : ROW_H);
    return r < 1 ? 1 : r;
}
static int per_row(void) {
    int n = (content_w() - SB_W - 8) / CELL_W;
    return n < 1 ? 1 : n;
}

/* Details columns: name, date modified, type, size (x offsets from the
 * content's left); the date goes first when the window is narrow (-1) */
static void columns(int* date_x, int* type_x, int* size_x) {
    int w = content_w() - SB_W;
    *size_x = w - 86;
    *type_x = *size_x - 150;
    if (*type_x < 160) *type_x = 160;
    *date_x = *type_x - 144;
    if (*date_x < 170) *date_x = -1;
}

/* the item under (mx, my): index, -1 empty space in the list, -2 outside it */
static int item_at(int mx, int my) {
    int lx = mx - g_x, ly = my - g_y;
    int cx = content_x(), cw = content_w() - SB_W;
    if (lx < cx || lx >= cx + cw || ly < list_top() || ly >= BODY_Y + body_h()) return -2;
    if (!g_view_icons) {
        int row = (ly - list_top()) / ROW_H + g_scroll;
        return row < g_count ? row : -1;
    }
    int col = (lx - cx - 6) / CELL_W, r = (ly - list_top()) / CELL_H + g_scroll;
    if (col < 0 || col >= per_row()) return -1;
    int i = r * per_row() + col;
    return i < g_count ? i : -1;
}

/* the navigation pane's places */
typedef struct { const char* label; fileicon_t icon; const char* path; int head; } place_t;
static const place_t PLACES[] = {
    { "Favorites", FI_FAVORITES, NULL, 1 },
    { "Downloads", FI_DOWNLOADS, HOME "/Downloads", 0 },
    { "banana", FI_HOME, HOME, 0 },
    { "Libraries", FI_LIBRARY, NULL, 1 },
    { "Documents", FI_DOCUMENTS, HOME "/Documents", 0 },
    { "Music", FI_MUSIC, HOME "/Music", 0 },
    { "Pictures", FI_PICTURES, HOME "/Pictures", 0 },
    { "Videos", FI_VIDEOS, HOME "/Videos", 0 },
    { "Computer", FI_COMPUTER, "/", 1 },
    { "Local Disk (/)", FI_DISK, "/", 0 },
    { "USB stick", FI_USB, "/mnt/usb", 0 },
};
#define NPLACES (int)(sizeof(PLACES) / sizeof(PLACES[0]))
#define NAV_ROW 22

static int place_visible(int i) {
    return strcmp(PLACES[i].label, "USB stick") != 0 || fs_find_dir("/mnt/usb") >= 0;
}

/* the y (relative to the window) of place i, -1 if hidden */
static int place_y(int i) {
    int y = BODY_Y + 6;
    for (int k = 0; k < i; k++) {
        if (!place_visible(k)) continue;
        y += NAV_ROW + (PLACES[k + 1].head ? 8 : 0);
    }
    return place_visible(i) ? y : -1;
}

static int place_at(int mx, int my) {
    int lx = mx - g_x, ly = my - g_y;
    if (lx < 2 || lx >= PANE_W || ly < BODY_Y || ly >= BODY_Y + body_h()) return -1;
    for (int i = 0; i < NPLACES; i++) {
        int y = place_y(i);
        if (y >= 0 && ly >= y && ly < y + NAV_ROW && PLACES[i].path) return i;
    }
    return -1;
}

static void open_place(int i) {
    const place_t* p = &PLACES[i];
    if (!p->path) return;
    if (strncmp(p->path, HOME "/", strlen(HOME) + 1) == 0) go_library(p->path + strlen(HOME) + 1);
    else go(p->path);
}

/* the toolbar's controls (relative x, y, w, h) */
enum { T_BACK = 0, T_FWD, T_ADDR, T_SEARCH, T_ORGANIZE, T_NEWFOLDER, T_TERMINAL, T_UP, T_DETAILS, T_ICONS, T_COUNT };
static void tool_rect(int t, int* x, int* y, int* w, int* h) {
    int sw = WIN_W < 640 ? 150 : 190;
    switch (t) {
    case T_BACK:      *x = 8;  *y = NAVBAR_Y + 4; *w = 24; *h = 22; break;
    case T_FWD:       *x = 34; *y = NAVBAR_Y + 4; *w = 24; *h = 22; break;
    case T_ADDR:      *x = 64; *y = NAVBAR_Y + 4; *w = WIN_W - 64 - sw - 14; *h = 22; break;
    case T_SEARCH:    *x = WIN_W - sw - 8; *y = NAVBAR_Y + 4; *w = sw; *h = 22; break;
    case T_ORGANIZE:  *x = 8;   *y = CMD_Y + 2; *w = 84; *h = 22; break;
    case T_NEWFOLDER: *x = 96;  *y = CMD_Y + 2; *w = 88; *h = 22; break;
    case T_TERMINAL:  *x = 188; *y = CMD_Y + 2; *w = 136; *h = 22; break;
    case T_UP:        *x = 328; *y = CMD_Y + 2; *w = 32; *h = 22; break;
    case T_DETAILS:   *x = WIN_W - 132; *y = CMD_Y + 2; *w = 64; *h = 22; break;
    case T_ICONS:     *x = WIN_W - 66;  *y = CMD_Y + 2; *w = 58; *h = 22; break;
    }
}
static int tool_at(int mx, int my) {
    for (int t = 0; t < T_COUNT; t++) {
        int x, y, w, h;
        tool_rect(t, &x, &y, &w, &h);
        if (t == T_TERMINAL && WIN_W < 600) continue;
        if (inside(mx, my, g_x + x, g_y + y, w, h)) return t;
    }
    return -1;
}

/* breadcrumb parts of the address bar: their path prefixes and x ranges */
#define CRUMBS 12
static struct { int x0, x1; char path[FS_PATH_LEN]; char label[40]; } g_crumb[CRUMBS];
static int g_ncrumb;

static void build_crumbs(int x_start, int x_end) {
    g_ncrumb = 0;
    /* "Computer", then each folder */
    char acc[FS_PATH_LEN] = "";
    const char* p = g_path;
    kstrlcpy(g_crumb[0].path, "/", FS_PATH_LEN);
    kstrlcpy(g_crumb[0].label, "Computer", sizeof(g_crumb[0].label));
    g_ncrumb = 1;
    while (*p && g_ncrumb < CRUMBS) {
        while (*p == '/') p++;
        if (!*p) break;
        const char* e = strchr(p, '/');
        int n = e ? (int)(e - p) : (int)strlen(p);
        char part[FS_NAME_LEN];
        if (n >= (int)sizeof(part)) n = (int)sizeof(part) - 1;
        memcpy(part, p, (size_t)n);
        part[n] = 0;
        size_t l = strlen(acc);
        ksnprintf(acc + l, sizeof(acc) - l, "/%s", part);
        kstrlcpy(g_crumb[g_ncrumb].path, acc, FS_PATH_LEN);
        kstrlcpy(g_crumb[g_ncrumb].label, part, sizeof(g_crumb[0].label));
        g_ncrumb++;
        p += n;
    }
    /* widths (from the right: the start is dropped when it does not fit) */
    int first = 0;
    for (;;) {
        int w = 0;
        for (int i = first; i < g_ncrumb; i++) w += (int)strlen(g_crumb[i].label) * 8 + 22;
        if (x_start + w <= x_end || first == g_ncrumb - 1) break;
        first++;
    }
    int x = x_start;
    for (int i = 0; i < g_ncrumb; i++) {
        if (i < first) { g_crumb[i].x0 = g_crumb[i].x1 = -1; continue; }
        g_crumb[i].x0 = x;
        x += (int)strlen(g_crumb[i].label) * 8 + 8;
        g_crumb[i].x1 = x;
        x += 14;
    }
}

/* ── keys ─────────────────────────────────────────────────────────── */

static void ensure_visible(void) {
    if (g_sel < 0) return;
    int line = g_view_icons ? g_sel / per_row() : g_sel;
    if (line < g_scroll) g_scroll = line;
    if (line >= g_scroll + content_rows()) g_scroll = line - content_rows() + 1;
    clamp_scroll();
}

static void move_sel(int d) {
    if (!g_count) return;
    if (g_sel < 0) g_sel = 0;
    else g_sel += d;
    if (g_sel < 0) g_sel = 0;
    if (g_sel >= g_count) g_sel = g_count - 1;
    g_confirm_delete = 0;
    g_status[0] = 0;
    ensure_visible();
}

static void text_edit(char* buf, int cap, char c) {
    size_t n = strlen(buf);
    if (c == '\b') { if (n) u8_backspace(buf, (int)n); }
    else if ((unsigned char)c >= 32 && (int)n < cap - 1) { buf[n] = c; buf[n + 1] = 0; }
}

void explorer_key(char c) {
    static int esc;
    g_gen++;
    if (g_renaming >= 0) {
        if (c == '\n') finish_rename();
        else if (c == 27) { g_renaming = -1; set_status("Not renamed"); }
        else if (c != '/') text_edit(g_rename, FS_NAME_LEN, c);
        return;
    }
    if (g_addr_edit) {
        if (c == '\n') {
            g_addr_edit = 0;
            char p[FS_PATH_LEN];
            if (g_addr[0] == '~') ksnprintf(p, sizeof(p), "%s%s", HOME, g_addr + 1);
            else kstrlcpy(p, g_addr, sizeof(p));
            size_t n = strlen(p);
            while (n > 1 && p[n - 1] == '/') p[--n] = 0;
            if (fs_find_dir(p) >= 0 || strcmp(p, "/") == 0) go(p);
            else {
                char msg[112];
                ksnprintf(msg, sizeof(msg), "Cannot find \"%s\" - check the spelling", g_addr);
                set_status_c(msg, 1);
            }
        } else if (c == 27) g_addr_edit = 0;
        else text_edit(g_addr, sizeof(g_addr), c);
        return;
    }
    if (g_search_focus) {
        if (c == 27) { g_search[0] = 0; g_search_focus = 0; scan(); }
        else if (c == '\n') { g_search_focus = 0; if (g_count == 1) open_item(0); }
        else if (c == '\t') g_search_focus = 0;
        else { text_edit(g_search, sizeof(g_search), c); g_sel = -1; g_scroll = 0; scan(); if (g_count) g_sel = 0; }
        return;
    }
    if (esc == 1) {
        esc = c == '[' ? 2 : 0;
        if (esc) return;
        g_confirm_delete = 0;               /* a lone Esc */
        g_status[0] = 0;
    } else if (esc == 2) {
        esc = 0;
        int step = g_view_icons ? per_row() : 1;
        if (c == 'A') move_sel(-step);
        else if (c == 'B') move_sel(step);
        else if (c == 'C' && g_view_icons) move_sel(1);
        else if (c == 'D' && g_view_icons) move_sel(-1);
        else if (c == 'H') { g_sel = g_count ? 0 : -1; ensure_visible(); }
        else if (c == 'F') { g_sel = g_count - 1; ensure_visible(); }
        else if (c == 'P' && g_sel >= 0) delete_selected();          /* Delete */
        return;
    }
    switch (c) {
    case 27: esc = 1; return;
    case '\n': if (g_sel >= 0) open_item(g_sel); return;
    case '\b': if (g_nback) go_back(); else go_up(); return;
    case 3: clip_selected(0); return;            /* Ctrl+C */
    case 24: clip_selected(1); return;           /* Ctrl+X */
    case 22: paste_here(); return;               /* Ctrl+V */
    case 18: explorer_start_rename(); return;    /* Ctrl+R: rename */
    case 14: new_folder(); return;               /* Ctrl+N */
    case 23: explorer_close(); return;           /* Ctrl+W */
    case 6: g_search_focus = 1; return;          /* Ctrl+F */
    case 12: g_addr_edit = 1; kstrlcpy(g_addr, g_path, sizeof(g_addr)); return;   /* Ctrl+L */
    }
    /* a letter: the next item whose name starts with it (like Windows) */
    if ((unsigned char)c > 32 && (unsigned char)c < 127) {
        for (int k = 1; k <= g_count; k++) {
            int i = (g_sel + k + g_count) % g_count;
            char a = item_name(&g_items[i])[0], b = c;
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a == b) { g_sel = i; ensure_visible(); break; }
        }
    }
}

void explorer_fkey(int k) {
    if (k == KEYF_F1 + 1) explorer_start_rename();                       /* F2 */
    else if (k == KEYF_F1 + 2) g_search_focus = 1;                      /* F3 */
    else if (k == KEYF_F1 + 3) { g_addr_edit = 1; kstrlcpy(g_addr, g_path, sizeof(g_addr)); }   /* F4 */
    else if (k == KEYF_F1 + 4) { scan(); set_status("Refreshed"); }     /* F5 */
    g_gen++;
}

void explorer_wheel(int mx, int my, int dz) {
    (void)mx; (void)my;
    g_scroll += dz * (g_view_icons ? 1 : 3);
    clamp_scroll();
    g_gen++;
}

/* ── mouse ────────────────────────────────────────────────────────── */

/* the Menu key / Shift+F10: the selected item's menu, at the item */
void explorer_menu_key(void) {
    if (!g_open) return;
    int x = g_x + content_x() + 60, y = g_y + list_top() + 8;
    if (g_sel >= 0 && !g_view_icons) {
        int r = g_sel - g_scroll;
        if (r >= 0) y = g_y + list_top() + r * ROW_H + ROW_H / 2;
    }
    item_menu(x, y, g_sel);
}

void explorer_rclick(int mx, int my) {
    if (!explorer_contains(mx, my)) return;
    int row = item_at(mx, my);
    if (row == -2) {
        int pl = place_at(mx, my);
        if (pl >= 0) { open_place(pl); row = -1; }
        else if (my - g_y < BODY_Y) return;
        else row = g_sel;
    }
    item_menu(mx, my, row);
}

void explorer_click(int mx, int my) {
    if (!explorer_contains(mx, my)) return;
    int lx = mx - g_x, ly = my - g_y;
    g_gen++;

    if (ly < TITLE_H + 2) {
        int b = win_button_press(&g_win, 4, 12, mx, my);
        if (b == WIN_BTN_CLOSE) { explorer_close(); return; }
        if (b) return;
        win_title_press(&g_win, mx, my);
        return;
    }
    if (win_grip_press(&g_win, mx, my)) return;
    if (g_renaming >= 0) finish_rename();

    int t = tool_at(mx, my);
    if (t >= 0) {
        if (t != T_SEARCH) g_search_focus = 0;
        if (t != T_ADDR) g_addr_edit = 0;
        g_confirm_delete = 0;
        int x, y, w, h;
        tool_rect(t, &x, &y, &w, &h);
        switch (t) {
        case T_BACK: go_back(); break;
        case T_FWD: go_forward(); break;
        case T_ADDR:
            if (!g_addr_edit) {
                for (int i = 0; i < g_ncrumb; i++)
                    if (g_crumb[i].x0 >= 0 && lx >= g_crumb[i].x0 && lx < g_crumb[i].x1) { go(g_crumb[i].path); return; }
                g_addr_edit = 1;                  /* beside the parts: type a path */
                kstrlcpy(g_addr, g_path, sizeof(g_addr));
            }
            break;
        case T_SEARCH: g_search_focus = 1; break;
        case T_ORGANIZE: organize_menu(g_x + x, g_y + y + h); break;
        case T_NEWFOLDER: new_folder(); break;
        case T_TERMINAL: terminal_at(g_path); break;
        case T_UP: go_up(); break;
        case T_DETAILS: g_view_icons = 0; g_scroll = 0; ensure_visible(); break;
        case T_ICONS: g_view_icons = 1; g_scroll = 0; ensure_visible(); break;
        }
        return;
    }
    g_search_focus = 0;
    g_addr_edit = 0;

    /* navigation pane */
    int pl = place_at(mx, my);
    if (pl >= 0) { g_confirm_delete = 0; open_place(pl); return; }

    /* column headers: sort */
    int cx = content_x();
    if (!g_view_icons && ly >= BODY_Y && ly < BODY_Y + HDR_H && lx >= cx && lx < cx + content_w() - SB_W) {
        int date_x, type_x, size_x;
        columns(&date_x, &type_x, &size_x);
        int rx = lx - cx;
        int col = (date_x >= 0 && rx >= date_x && rx < type_x) ? 1 : rx < type_x ? 0 : rx < size_x ? 2 : 3;
        if (g_sort == col) g_sort_desc = !g_sort_desc;
        else { g_sort = col; g_sort_desc = 0; }
        int keep = g_sel >= 0 ? g_items[g_sel].idx * 2 + g_items[g_sel].is_dir : -1;
        scan();
        for (int i = 0; i < g_count; i++) if (g_items[i].idx * 2 + g_items[i].is_dir == keep) g_sel = i;
        return;
    }

    /* scrollbar: arrows, then the track pages */
    int sbx = cx + content_w() - SB_W;
    int top = list_top(), bot = BODY_Y + body_h();
    if (lx >= sbx && lx < sbx + SB_W && ly >= top && ly < bot) {
        if (ly < top + SB_W) g_scroll--;
        else if (ly >= bot - SB_W) g_scroll++;
        else g_scroll += (ly < (top + bot) / 2) ? -(content_rows() - 1) : content_rows() - 1;
        clamp_scroll();
        return;
    }

    /* the items */
    int row = item_at(mx, my);
    if (row == -1) { g_sel = -1; g_confirm_delete = 0; g_status[0] = 0; return; }
    if (row >= 0) {
        uint32_t now = timer_ms();
        int dbl = row == g_last_click_row && now - g_last_click_ms < 500;
        g_last_click_row = row;
        g_last_click_ms = now;
        if (row != g_sel) { g_sel = row; g_confirm_delete = 0; g_status[0] = '\0'; }
        if (dbl) { g_last_click_row = -1; open_item(row); }
    }
}

void explorer_mouse(int mx, int my, int left) {
    if (win_mouse(&g_win, mx, my, left)) { g_gen++; clamp_scroll(); return; }
    if (!g_open) return;
    int over = explorer_contains(mx, my);
    int h = over ? item_at(mx, my) : -2;
    int hn = over ? place_at(mx, my) : -1;
    int ht = over ? tool_at(mx, my) : -1;
    if (h < 0) h = -1;
    if (h != g_hover || hn != g_hover_nav || ht != g_hover_tool) {
        g_hover = h; g_hover_nav = hn; g_hover_tool = ht;
        g_gen++;
    }
}

/* ── drawing ──────────────────────────────────────────────────────── */

static void make_thumb(int fidx) {
    drop_thumb();
    g_thumb_for = fidx;
    fs_file_t* f = fs_get_file(fidx);
    image_t img;
    char err[64];
    fs_pin(fidx);
    int bad = image_decode((const uint8_t*)f->content, f->size, &img, err, sizeof(err)) != 0;
    fs_unpin(fidx);
    if (bad) return;
    g_thumb = (uint32_t*)kmalloc(THUMB * THUMB * 4);
    if (g_thumb) image_render(&img, g_thumb, THUMB, THUMB, IMAGE_FIT, C_DET);
    image_free(&img);
}

static void blit_thumb(int x, int y) {
    int stride, tw, th;
    uint32_t* dst = fb_target(&stride, &tw, &th);
    if (!dst || !g_thumb) return;
    for (int r = 0; r < THUMB; r++) {
        int yy = y + r;
        if (yy < 0 || yy >= th) continue;
        for (int c = 0; c < THUMB; c++) {
            int xx = x + c;
            if (xx >= 0 && xx < tw) dst[yy * stride + xx] = g_thumb[r * THUMB + c];
        }
    }
}

/* a little round button with an arrow (back / forward) */
static void round_button(int x, int y, int w, int h, int enabled, int hover, int dir) {
    uint32_t fill = !enabled ? 0x00DCE5F1u : hover ? 0x00B9D9FBu : 0x00E9F1FBu;
    uint32_t edge = enabled ? 0x006E8DB5u : 0x00AFC1D7u;
    gfx_fill_rect(x + 2, y, w - 4, h, edge);
    gfx_fill_rect(x, y + 2, w, h - 4, edge);
    gfx_fill_rect(x + 1, y + 1, w - 2, h - 2, edge);
    gfx_fill_rect(x + 2, y + 1, w - 4, h - 2, fill);
    gfx_fill_rect(x + 1, y + 2, w - 2, h - 4, fill);
    uint32_t a = enabled ? 0x002A4E7Eu : 0x0098AABFu;
    int cx = x + w / 2, cy = y + h / 2;
    for (int i = 0; i < 5; i++) {                           /* an arrow head and shaft */
        int ax = dir < 0 ? cx - 4 + i : cx + 4 - i;
        gfx_fill_rect(ax, cy - i, 1, 2 * i + 1, a);
    }
    if (dir < 0) gfx_fill_rect(cx, cy - 1, 5, 3, a);
    else gfx_fill_rect(cx - 4, cy - 1, 5, 3, a);
}

/* a flat command-bar button: highlighted under the pointer */
static void cmd_button(int t, const char* label, int pressed) {
    int x, y, w, h;
    tool_rect(t, &x, &y, &w, &h);
    x += g_x; y += g_y;
    if (pressed) frame(x, y, w, h, 0x00C4DDF6u, 0x007DA2CEu);
    else if (g_hover_tool == t) frame(x, y, w, h, 0x00E3EFFCu, 0x00A9C6EAu);
    /* a menu button: its label, then a small drop-down arrow */
    int arrow = t == T_ORGANIZE;
    int tw = (int)strlen(label) * 8 + (arrow ? 10 : 0);
    int tx = x + (w - tw) / 2;
    gfx_draw_text(tx, y + 7, label, C_TEXT, pressed ? 0x00C4DDF6u : g_hover_tool == t ? 0x00E3EFFCu : C_CMD);
    if (arrow) {
        int ax = tx + (int)strlen(label) * 8 + 4, ay = y + 10;
        for (int r = 0; r < 3; r++) gfx_fill_rect(ax + r, ay + r, 5 - 2 * r, 1, C_TEXT);
    }
}

static void draw_toolbars(void) {
    int x = g_x, y = g_y;
    gradient(x + 3, y + NAVBAR_Y, WIN_W - 6, NAVBAR_H, C_NAVBAR, C_NAVBAR2);
    round_button(x + 8, y + NAVBAR_Y + 4, 24, 22, g_nback > 0, g_hover_tool == T_BACK, -1);
    round_button(x + 34, y + NAVBAR_Y + 4, 24, 22, g_nfwd > 0, g_hover_tool == T_FWD, 1);

    /* the address bar: a folder icon, then the breadcrumb (or the path being typed) */
    int ax, ay, aw, ah;
    tool_rect(T_ADDR, &ax, &ay, &aw, &ah);
    ax += x; ay += y;
    frame(ax, ay, aw, ah, C_WHITE, g_addr_edit ? 0x003C7FB1u : C_BOX_B);
    fileicon_draw(strcmp(g_path, "/") == 0 ? FI_COMPUTER : FI_FOLDER, ax + 4, ay + 3, 16);
    if (g_addr_edit) {
        int cols = (aw - 34) / 8;
        const char* s = g_addr;
        int n = (int)strlen(s);
        if (n > cols - 1) s += n - (cols - 1);            /* the end shows */
        gfx_draw_text(ax + 26, ay + 7, s, C_TEXT, C_WHITE);
        if ((timer_ms() / 500) % 2 == 0) gfx_fill_rect(ax + 26 + (int)strlen(s) * 8, ay + 4, 1, 14, C_TEXT);
    } else {
        build_crumbs(ax + 26 - x, ax + aw - 8 - x);
        for (int i = 0; i < g_ncrumb; i++) {
            if (g_crumb[i].x0 < 0) continue;
            int cx0 = x + g_crumb[i].x0;
            int hov = 0;
            uint32_t bg = C_WHITE;
            (void)hov;
            gfx_draw_text(cx0 + 4, ay + 7, g_crumb[i].label, C_TEXT, bg);
            if (i < g_ncrumb - 1) {                       /* the little arrow between parts */
                int sx = x + g_crumb[i].x1 + 3;
                for (int k = 0; k < 4; k++) gfx_fill_rect(sx + k, ay + 7 + k, 1, 8 - 2 * k, C_DIM);
            }
        }
    }

    /* the search box */
    int sx, sy, sw, sh;
    tool_rect(T_SEARCH, &sx, &sy, &sw, &sh);
    sx += x; sy += y;
    frame(sx, sy, sw, sh, C_WHITE, g_search_focus ? 0x003C7FB1u : C_BOX_B);
    fileicon_draw(FI_SEARCH, sx + sw - 20, sy + 3, 16);
    int cols = (sw - 30) / 8;
    if (g_search[0] || g_search_focus) {
        draw_clip(sx + 6, sy + 7, g_search, cols, C_TEXT, C_WHITE);
        if (g_search_focus && (timer_ms() / 500) % 2 == 0) {
            int n = (int)strlen(g_search);
            if (n > cols) n = cols;
            gfx_fill_rect(sx + 6 + n * 8, sy + 4, 1, 14, C_TEXT);
        }
    } else {
        char ph[64];
        ksnprintf(ph, sizeof(ph), "Search %s", strcmp(g_path, "/") == 0 ? "Computer" : base_name(g_path));
        draw_clip(sx + 6, sy + 7, ph, cols, 0x008C99A8u, C_WHITE);
    }

    /* the command bar */
    gradient(x + 3, y + CMD_Y, WIN_W - 6, CMD_H, C_CMD, C_CMD2);
    gfx_fill_rect(x + 3, y + CMD_Y + CMD_H - 1, WIN_W - 6, 1, C_LINE);
    cmd_button(T_ORGANIZE, "Organize", 0);
    cmd_button(T_NEWFOLDER, "New folder", 0);
    if (WIN_W >= 600) cmd_button(T_TERMINAL, "Open in terminal", 0);
    cmd_button(T_UP, "Up", 0);
    cmd_button(T_DETAILS, "Details", !g_view_icons);
    cmd_button(T_ICONS, "Icons", g_view_icons);
}

static void draw_nav_pane(void) {
    int x = g_x + 3, y = g_y + BODY_Y, h = body_h();
    gfx_fill_rect(x, y, PANE_W - 3, h, C_PANE);
    gfx_fill_rect(x + PANE_W - 3, y, 1, h, C_LINE);
    for (int i = 0; i < NPLACES; i++) {
        int py = place_y(i);
        if (py < 0 || py + NAV_ROW > BODY_Y + h) continue;
        py += g_y;
        const place_t* p = &PLACES[i];
        int here = p->path && !p->head && strcmp(p->path, g_path) == 0;
        if (p->head && p->path && strcmp(p->path, g_path) == 0 && strcmp(g_path, "/") != 0) here = 1;
        uint32_t bg = C_PANE;
        int ix = p->head ? x + 6 : x + 22;
        if (here) { frame(x + 2, py, PANE_W - 8, NAV_ROW, C_SEL, C_SEL_B); bg = C_SEL; }
        else if (g_hover_nav == i) { frame(x + 2, py, PANE_W - 8, NAV_ROW, C_HOVER, C_HOVER_B); bg = C_HOVER; }
        fileicon_draw(p->icon, ix, py + 3, 16);
        draw_clip(ix + 22, py + 7, p->label, (PANE_W - (ix - x) - 30) / 8, p->head ? C_NAVTEXT : C_TEXT, bg);
    }
}

static void draw_rename_box(int x, int y, int w) {
    frame(x, y, w, 18, C_WHITE, 0x003C7FB1u);
    int cols = (w - 8) / 8;
    const char* s = g_rename;
    int n = (int)strlen(s);
    if (n > cols - 1) s += n - (cols - 1);
    gfx_draw_text(x + 4, y + 5, s, C_TEXT, C_WHITE);
    gfx_fill_rect(x + 4 + (int)strlen(s) * 8, y + 2, 1, 14, C_TEXT);
}

static void draw_details_view(void) {
    int x = g_x + content_x(), y = g_y + BODY_Y, w = content_w() - SB_W;
    int date_x, type_x, size_x;
    columns(&date_x, &type_x, &size_x);
    int name_end = date_x >= 0 ? date_x : type_x;
    /* headers */
    gfx_fill_rect(x, y, w, HDR_H, C_WHITE);
    gfx_fill_rect(x, y + HDR_H - 1, w, 1, 0x00E5E5E5u);
    const char* hdr[4] = { "Name", "Date modified", "Type", "Size" };
    int hx[4] = { 0, date_x, type_x, size_x };
    for (int c = 0; c < 4; c++) {
        if (hx[c] < 0) continue;
        gfx_draw_text(x + hx[c] + 8, y + 7, hdr[c], 0x004C607Au, C_WHITE);
        if (c) gfx_fill_rect(x + hx[c], y + 3, 1, HDR_H - 6, 0x00E5E5E5u);
        if (g_sort == c) {                                  /* the sort arrow */
            int ax = x + hx[c] + 8 + (int)strlen(hdr[c]) * 8 + 8;
            for (int k = 0; k < 4; k++) {
                int yy = g_sort_desc ? y + 9 + k : y + 12 - k;
                gfx_fill_rect(ax + k, yy, 7 - 2 * k, 1, 0x00869BB4u);
            }
        }
    }
    int top = g_y + list_top();
    if (!g_count) {
        gfx_draw_text(x + 24, top + 10, g_search[0] ? "No items match your search." : "This folder is empty.", C_DIM, C_WHITE);
        return;
    }
    for (int r = 0; r < content_rows() && g_scroll + r < g_count; r++) {
        int i = g_scroll + r;
        const item_t* it = &g_items[i];
        int ry = top + r * ROW_H;
        uint32_t bg = C_WHITE;
        if (i == g_sel) { frame(x + 2, ry, w - 4, ROW_H, C_SEL, C_SEL_B); bg = C_SEL; }
        else if (i == g_hover) { frame(x + 2, ry, w - 4, ROW_H, C_HOVER, C_HOVER_B); bg = C_HOVER; }
        fileicon_draw(icon_of(it), x + 6, ry + 2, 16);
        if (i == g_renaming) draw_rename_box(x + 26, ry + 1, name_end - 30);
        else draw_clip(x + 28, ry + 6, item_name(it), (name_end - 34) / 8, C_TEXT, bg);
        if (date_x >= 0) {
            char d[24];
            rtc_format(item_mtime(it), d, sizeof(d));
            gfx_draw_text(x + date_x + 8, ry + 6, d, 0x006D6D6Du, bg);
        }
        char t[40];
        type_of(it, t, sizeof(t));
        draw_clip(x + type_x + 8, ry + 6, t, (size_x - type_x - 12) / 8, 0x006D6D6Du, bg);
        if (!it->is_dir) {
            char s[24];
            human_size(item_size(it), s, sizeof(s));
            int sw = (int)strlen(s) * 8;
            gfx_draw_text(x + w - 10 - sw, ry + 6, s, 0x006D6D6Du, bg);
        }
    }
}

static void draw_icons_view(void) {
    int x = g_x + content_x(), top = g_y + list_top(), pr = per_row();
    if (!g_count) {
        gfx_draw_text(x + 24, top + 10, g_search[0] ? "No items match your search." : "This folder is empty.", C_DIM, C_WHITE);
        return;
    }
    for (int r = 0; r < content_rows(); r++) {
        for (int c = 0; c < pr; c++) {
            int i = (g_scroll + r) * pr + c;
            if (i >= g_count) return;
            const item_t* it = &g_items[i];
            int cx = x + 6 + c * CELL_W, cy = top + r * CELL_H;
            uint32_t bg = C_WHITE;
            if (i == g_sel) { frame(cx + 2, cy, CELL_W - 4, CELL_H - 4, C_SEL, C_SEL_B); bg = C_SEL; }
            else if (i == g_hover) { frame(cx + 2, cy, CELL_W - 4, CELL_H - 4, C_HOVER, C_HOVER_B); bg = C_HOVER; }
            fileicon_draw(icon_of(it), cx + (CELL_W - 48) / 2, cy + 6, 48);
            if (i == g_renaming) { draw_rename_box(cx + 2, cy + 58, CELL_W - 4); continue; }
            /* the name: up to two centred lines (gfx_draw_label wraps at
             * spaces; a long name without any is cut here) */
            const char* nm = item_name(it);
            int per = (CELL_W - 8) / 7, n = (int)strlen(nm);
            if (n <= per || strchr(nm, ' ')) { gfx_draw_label(cx + CELL_W / 2, cy + 58, CELL_W - 8, nm, C_TEXT, bg); continue; }
            char l1[24], l2[24];
            int cut = per;
            for (int k = per; k > per / 2; k--) if (nm[k] == '.' || nm[k] == '-' || nm[k] == '_') { cut = k; break; }
            memcpy(l1, nm, (size_t)cut); l1[cut] = 0;
            const char* rest = nm + cut;
            if ((int)strlen(rest) > per) { memcpy(l2, rest, (size_t)(per - 2)); memcpy(l2 + per - 2, "..", 3); }
            else kstrlcpy(l2, rest, sizeof(l2));
            gfx_draw_label(cx + CELL_W / 2, cy + 58, CELL_W - 8, l1, C_TEXT, bg);
            gfx_draw_label(cx + CELL_W / 2, cy + 72, CELL_W - 8, l2, C_TEXT, bg);
        }
    }
}

static void draw_scrollbar(void) {
    int x = g_x + content_x() + content_w() - SB_W, top = g_y + list_top(), h = body_h() - (list_top() - BODY_Y);
    gfx_fill_rect(x, top, SB_W, h, 0x00F0F0F0u);
    int lines = g_view_icons ? (g_count + per_row() - 1) / per_row() : g_count;
    /* arrows */
    for (int k = 0; k < 4; k++) {
        gfx_fill_rect(x + 7 - k, top + 5 + k, 2 * k + 1, 1, 0x00606060u);
        gfx_fill_rect(x + 7 - k, top + h - 6 - k, 2 * k + 1, 1, 0x00606060u);
    }
    if (lines <= content_rows()) return;
    int track = h - 2 * SB_W;
    int th = track * content_rows() / lines;
    if (th < 16) th = 16;
    int ty = top + SB_W + (track - th) * g_scroll / (max_scroll() ? max_scroll() : 1);
    frame(x + 2, ty, SB_W - 4, th, 0x00CDCDCDu, 0x00A6A6A6u);
}

static void draw_details_pane(void) {
    int x = g_x + 3, y = g_y + WIN_H - DET_H - 3, w = WIN_W - 6;
    gradient(x, y, w, DET_H, 0x00F1F5FBu, 0x00DCE6F3u);
    gfx_fill_rect(x, y, w, 1, C_LINE);
    int ix = x + 12, iy = y + 5;
    int tx = ix + THUMB + 14;
    int cols = (w - (tx - x) - 12) / 8;
    char l1[112], l2[112], size[24], type[40];
    uint32_t bg = 0x00E8EFF8u;
    if (g_sel >= 0 && g_sel < g_count) {
        const item_t* it = &g_items[g_sel];
        type_of(it, type, sizeof(type));
        if (!it->is_dir && is_image_file(it->idx)) {
            if (g_thumb_for != it->idx) make_thumb(it->idx);
            if (g_thumb) blit_thumb(ix, iy);
            else fileicon_draw(FI_IMAGE, ix + 2, iy + 2, 48);
        } else {
            fileicon_draw(icon_of(it), ix + 2, iy + 2, 48);
        }
        kstrlcpy(l1, item_name(it), sizeof(l1));
        if (it->is_dir) {
            char path[FS_PATH_LEN];
            int d[1], f[1];
            child_path(item_name(it), path, sizeof(path));
            int n = fs_list_dirs(path, d, 1) + fs_list_files(path, f, 1);
            ksnprintf(l2, sizeof(l2), "%s    %d item%s", type, n, n == 1 ? "" : "s");
        } else {
            human_size(item_size(it), size, sizeof(size));
            ksnprintf(l2, sizeof(l2), "%s    Size: %s", type, size);
        }
        char dm[24], dc[24];
        rtc_format(item_mtime(it), dm, sizeof(dm));
        rtc_format(item_ctime(it), dc, sizeof(dc));
        if (dm[0] && !g_status[0]) {
            char l3[112];
            if (dc[0]) ksnprintf(l3, sizeof(l3), "Date modified: %s    Date created: %s", dm, dc);
            else ksnprintf(l3, sizeof(l3), "Date modified: %s", dm);
            draw_clip(tx, y + 46, l3, cols, 0x005E6F86u, 0x00DFE8F4u);
        }
    } else {
        fileicon_draw(strcmp(g_path, "/") == 0 ? FI_COMPUTER : FI_FOLDER_OPEN, ix + 2, iy + 2, 48);
        uint32_t bytes = 0;
        int nf = 0;
        for (int i = 0; i < g_count; i++) if (!g_items[i].is_dir) { nf++; bytes += item_size(&g_items[i]); }
        human_size(bytes, size, sizeof(size));
        if (g_search[0]) ksnprintf(l1, sizeof(l1), "%d item%s match \"%s\"", g_count, g_count == 1 ? "" : "s", g_search);
        else ksnprintf(l1, sizeof(l1), "%d item%s", g_count, g_count == 1 ? "" : "s");
        ksnprintf(l2, sizeof(l2), "%s    %d file%s, %s", g_path, nf, nf == 1 ? "" : "s", size);
    }
    draw_clip(tx, y + 14, l1, cols, C_TEXT, bg);
    draw_clip(tx, y + 30, l2, cols, 0x005E6F86u, 0x00E3EBF6u);
    if (g_status[0]) draw_clip(tx, y + 46, g_status, cols, g_status_warn ? C_WARN : 0x002A6F2Fu, 0x00DFE8F4u);
}

void explorer_draw(const fb_info_t* fi) {
    (void)fi;
    if (!g_open) return;
    scan();
    int x = g_x, y = g_y;
    /* the frame and title bar, like the other windows */
    gfx_fill_rect(x, y, WIN_W, WIN_H, 0x00505D72u);
    gfx_fill_rect(x + 1, y + 1, WIN_W - 2, WIN_H - 2, C_FRAME);
    gfx_fill_rect(x + 3, y + 3, WIN_W - 6, TITLE_H - 1, C_TITLE);
    fileicon_draw(FI_FOLDER, x + 7, y + 4, 16);
    char title[64];
    ksnprintf(title, sizeof(title), "%s", strcmp(g_path, "/") == 0 ? "Computer" : base_name(g_path));
    draw_clip(x + 28, y + 8, title, (WIN_W - 120) / 8, 0x00FFFFFFu, C_TITLE);
    win_draw_buttons(&g_win, 4, 12);

    draw_toolbars();
    draw_nav_pane();
    /* the content */
    gfx_fill_rect(x + content_x(), y + BODY_Y, content_w(), body_h(), C_WHITE);
    if (g_view_icons) draw_icons_view();
    else draw_details_view();
    draw_scrollbar();
    draw_details_pane();
    gfx_draw_grip(x + WIN_W, y + WIN_H);
}
