#include "gui.h"
#include "serial.h"
#include "terminal.h"
#include "timer.h"
#include "sysinfo.h"
#include "keyboard.h"
#include "gfx.h"
#include "fb.h"
#include "rtc.h"
#include "usb.h"
#include "task.h"
#include "fs.h"
#include "kstring.h"
#include "wallpaper.h"
#include "tty.h"
#include "explorer.h"
#include "browser.h"
#include "notepad.h"
#include "clipboard.h"
#include "installer.h"
#include "passwd.h"
#include "login.h"
#include "utf8.h"
#include "kheap.h"
#include "appwin.h"
#include "winframe.h"
#include "ctxmenu.h"
#include "kbnav.h"
#include "fileops.h"
#include "fileicons.h"
#include "settings.h"
#include "launcher.h"
#include "taskmgr.h"
#include "settings.h"
#include "pkg.h"
#include "image.h"
#include "gpu.h"
#include "audio.h"
#include "app.h"
#include "../net/net.h"
#include "../shell/shell.h"
#include "startmenu.h"
#include "fileicons.h"

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

#define TASKBAR_ROW (VGA_HEIGHT - 1)

/* The Start menu, Windows 95 style: Programs (a submenu of the installed
 * apps), the built-in programs, and Shut down (a submenu: Restart, Shut
 * down, Lock, Exit to shell). */
/* (About and Wallpaper live in Settings now: reached from the desktop's menu) */
enum { ACT_TERMINAL = 0, ACT_FILES, ACT_BROWSER, ACT_NOTEPAD, ACT_APPS, ACT_TASKMGR, ACT_SETTINGS, ACT_QUIT, ACT_ABOUT,
       ACT_WALLPAPER, ACT_LOCK, ACT_INSTALL, ACT_PROGRAMS, ACT_POWER, ACT_RESTART, ACT_SHUTDOWN, ACT_RUN_APP };
/* slots: the installer's ("Install Banana OS") is there only on the live CD */
#define MENU_ITEMS_MAX 10
#define MENU_SLOT_INSTALL 8
static const int MENU_ACTS[MENU_ITEMS_MAX] = {
    ACT_PROGRAMS, ACT_TERMINAL, ACT_FILES, ACT_BROWSER, ACT_NOTEPAD, ACT_APPS, ACT_TASKMGR, ACT_SETTINGS, ACT_INSTALL, ACT_POWER,
};
static const char* const MENU_LABELS[MENU_ITEMS_MAX] = {
    "Programs", "Terminal", "Files", "Browser", "Notepad", "Apps", "Task Manager", "Settings", "Install Banana OS", "Shut down",
};
static int menu_count(void) { return installer_available() ? MENU_ITEMS_MAX : MENU_ITEMS_MAX - 1; }
/* a visible slot's entry in the tables above */
static int menu_idx(int slot) { return (!installer_available() && slot >= MENU_SLOT_INSTALL) ? slot + 1 : slot; }
static int menu_has_sub(int slot) { int a = MENU_ACTS[menu_idx(slot)]; return a == ACT_PROGRAMS || a == ACT_POWER; }
#define MENU_ITEMS menu_count()
#define START_MENU_W 236
#define START_MENU_H (16 + MENU_ITEMS * 28)

/* The desktop: big icons with their names under them, like Windows - the
 * built-in programs, then every installed app; in columns from the top
 * left. One click selects, a double click opens. */
#define CELL_W 80
#define CELL_H 74
#define DESK_X 4
#define DESK_Y 6
#define DESK_MAX 128
enum { DK_APP = 0, DK_TRASH, DK_FILE, DK_DIR };
typedef struct {
    int      act;                       /* ACT_*; ACT_RUN_APP: an installed app */
    char     label[48];
    char     app[PKG_NAME_MAX];
    int      kind;                      /* DK_*: a program, the Recycle Bin, a file or folder of ~/Desktop */
    char     path[FS_PATH_LEN];         /* (files and folders) */
    int      col, row;                  /* its cell on the desktop */
} desk_item_t;
static desk_item_t g_desk[DESK_MAX];
static int         g_ndesk, g_desk_sel = -1, g_last_click_i = -1;
static uint8_t     g_desk_mark[DESK_MAX];       /* several selected (Ctrl+click, a box, Ctrl+A) */
static int         g_desk_drop = -1;            /* the icon a drag would land on (drawn) */
static int  desk_marked(int i) { return i >= 0 && i < g_ndesk && (g_desk_mark[i] || i == g_desk_sel); }
static void g_desk_marks_clear(void) { memset(g_desk_mark, 0, sizeof(g_desk_mark)); }
static void desk_open(int i);
static int  pin_find(int act, const char* app);
static void pin_add(int act, const char* app);
static void pin_remove(int i);
static void desk_trash_marked(void);
static void desk_clip_marked(int cut);
static void desk_new(int folder);

static uint32_t    g_desk_sig, g_desk_checked, g_last_click_ms;
static int         g_sub_slot = -1;     /* the Start menu item whose submenu is open */

static int g_menu_open = 0;
static int g_start_hover;         /* the pointer is on the Start orb */
static int g_lock_pending = 0;   /* "Lock screen" was chosen */
static int g_in_lock = 0;        /* the lock screen is up (in some task's gui_poll) */
/* The pointer. The desktop loop moves it, and so does the timer interrupt
 * (gui_cursor_tick) while the loop is not drawing: interrupts are off
 * wherever the loop updates it. */
static int g_cur_mx = 100, g_cur_my = 100;
static volatile int g_ptr_drawn_x = -1, g_ptr_drawn_y = -1;   /* where it is on the screen (-1: not drawn) */
static volatile int g_gui_busy;      /* a task is drawing the desktop (gui_poll): hands off the screen */
static volatile int g_scr_w, g_scr_h;
static int g_mouse_from_lock = 0;           /* it moved there: take it back */
static int g_menu_sel = 0;     /* an ACT_* */
/* The app windows (Files, Browser, Notepad, Apps, Task Manager and the
 * windows of installed apps) and the terminal windows share one stacking
 * order: the app windows have their own back-to-front order, and at most
 * one of them - g_front_app - is above the terminals. */
enum { APP_FILES = 0, APP_BROWSER, APP_NOTEPAD, APP_LAUNCHER, APP_TASKMGR, APP_SETTINGS, APP_INSTALLER, APP_APPWIN, APP_COUNT };
typedef struct {
    int      (*is_open)(void);
    void     (*draw)(const fb_info_t* fi);
    int      (*contains)(int mx, int my);
    void     (*click)(int mx, int my);
    void     (*mouse)(int mx, int my, int left);
    uint32_t (*signature)(void);
    void     (*close)(void);
    void     (*rclick)(int mx, int my);
    const char* title;
} app_t;
static void explorer_rclick_menu(int mx, int my);
static void notepad_rclick_menu(int mx, int my);
static const app_t g_apps[APP_COUNT] = {
    { explorer_is_open, explorer_draw, explorer_contains, explorer_click, explorer_mouse, explorer_signature, explorer_close, explorer_rclick_menu, "Files" },
    { browser_is_open, browser_draw, browser_contains, browser_click, browser_mouse, browser_signature, browser_close, browser_rclick, "Browser" },
    { notepad_is_open, notepad_draw, notepad_contains, notepad_click, notepad_mouse, notepad_signature, notepad_close, notepad_rclick_menu, "Notepad" },
    { launcher_is_open, launcher_draw, launcher_contains, launcher_click, launcher_mouse, launcher_signature, launcher_close, launcher_rclick, "Apps" },
    { taskmgr_is_open, taskmgr_draw, taskmgr_contains, taskmgr_click, taskmgr_mouse, taskmgr_signature, taskmgr_close, taskmgr_rclick, "Task Manager" },
    { settings_is_open, settings_draw, settings_contains, settings_click, settings_mouse, settings_signature, settings_close, settings_rclick, "Settings" },
    { installer_is_open, installer_draw, installer_contains, installer_click, installer_mouse, installer_signature, installer_close, installer_rclick, "Install Banana OS" },
    { appwin_is_open, appwin_draw, appwin_contains, appwin_click, appwin_mouse, appwin_signature, appwin_close_all, appwin_rclick, "App" },
};
static int g_front_app = -1;                        /* -1: a terminal window is in front */
static int g_app_order[APP_COUNT] = { APP_FILES, APP_BROWSER, APP_NOTEPAD, APP_LAUNCHER, APP_TASKMGR, APP_SETTINGS, APP_INSTALLER, APP_APPWIN };   /* back to front */
static int g_app_min[APP_COUNT];                    /* minimized to the taskbar */

/* open and not minimized (the app windows minimize one by one) */
static int app_visible(int a) {
    if (a == APP_APPWIN) return appwin_any_visible();
    return g_apps[a].is_open() && !g_app_min[a];
}

static void raise_app(int a) {
    int pos = 0;
    while (pos < APP_COUNT && g_app_order[pos] != a) pos++;
    for (int i = pos; i < APP_COUNT - 1; i++) g_app_order[i] = g_app_order[i + 1];
    g_app_order[APP_COUNT - 1] = a;
    g_front_app = a;
}

/* the topmost app window at a point, -1 if none (with_front: also the one above the terminals) */
/* the mouse wheel, per app window (NULL: it does not scroll) */
static void (*const g_app_wheel[APP_COUNT])(int mx, int my, int dz) = {
    explorer_wheel, browser_wheel, notepad_wheel, NULL, NULL, settings_wheel, NULL, appwin_wheel,
};

static int app_at(int mx, int my, int with_front) {
    for (int i = APP_COUNT - 1; i >= 0; i--) {
        int a = g_app_order[i];
        if (!with_front && a == g_front_app) continue;
        if (app_visible(a) && g_apps[a].contains(mx, my)) return a;
    }
    return -1;
}
static uint32_t g_last_clock_sec = (uint32_t)-1;
static int g_gui_enabled = 0; /* like startx: default off */
typedef struct {
    int open;
    int vt;   /* 0 until this slot's window has been opened for the first
               * time; once allocated it (and its shell task below) is
               * kept for the OS's lifetime, so "exit"/closing a window
               * just hides it rather than tearing anything down - the
               * next open reuses the same vt and picks its shell back up
               * wherever it left off, like a detached tmux pane. */
    int x, y, w, h;
    int dragging;
    int drag_dx, drag_dy;
    int resizing;                 /* dragging the bottom-right grip */
    int maxed, sx, sy, sw, sh;    /* maximized, and the geometry to restore */
    uint32_t title_click_ms;      /* double-click on the title bar */
    int selecting, sel;           /* mouse text selection (cells, buffer rows) */
    int s_r0, s_c0, s_r1, s_c1;
    int minimized;                /* to the taskbar */
} term_win_t;

#define TERM_WIN_MAX 4
static term_win_t g_terms[TERM_WIN_MAX] = {
    {.x = 140, .y = 90, .w = 520, .h = 340},
    {.x = 180, .y = 120, .w = 520, .h = 340},
    {.x = 220, .y = 150, .w = 520, .h = 340},
    {.x = 260, .y = 180, .w = 520, .h = 340},
};

/* Each open terminal window is driven by its own cooperative shell task
 * (kernel/task.h) bound permanently to its vt, so windows make progress
 * independently instead of all sharing whichever single shell last had
 * focus (the original bug: running "top"/"edit"/"help" in one window and
 * then clicking another showed the same running command, because there
 * used to be only one shell instance and focus just retargeted its
 * output). task_create() takes a plain 0-argument entry point, so these
 * four trampolines exist one-per-slot to close over each slot's index. */
static void term_task_entry_0(void) { shell_run_window(g_terms[0].vt); }
static void term_task_entry_1(void) { shell_run_window(g_terms[1].vt); }
static void term_task_entry_2(void) { shell_run_window(g_terms[2].vt); }
static void term_task_entry_3(void) { shell_run_window(g_terms[3].vt); }
static void (*const g_term_task_entry[TERM_WIN_MAX])(void) = {
    term_task_entry_0, term_task_entry_1, term_task_entry_2, term_task_entry_3
};

/* z-order: back -> front */
static int g_term_order[TERM_WIN_MAX] = {0, 1, 2, 3};

static void menu_activate(void);

static uint32_t vga_color_rgb(uint8_t c) {
    static const uint32_t pal[16] = {
        0x00000000u, 0x000000AAu, 0x0000AA00u, 0x0000AAAAu,
        0x00AA0000u, 0x00AA00AAu, 0x00AA5500u, 0x00AAAAAAu,
        0x00555555u, 0x005555FFu, 0x0055FF55u, 0x0055FFFFu,
        0x00FF5555u, 0x00FF55FFu, 0x00FFFF55u, 0x00FFFFFFu,
    };
    return pal[c & 0x0F];
}

static void draw_bevel_box(int x, int y, int w, int h, uint32_t base, uint32_t hi, uint32_t lo) {
    if (w <= 2 || h <= 2) return;
    gfx_fill_rect(x, y, w, h, base);
    gfx_fill_rect(x, y, w, 1, hi);
    gfx_fill_rect(x, y, 1, h, hi);
    gfx_fill_rect(x, y + h - 1, w, 1, lo);
    gfx_fill_rect(x + w - 1, y, 1, h, lo);
}


/* The icons: 14x14 drawings, at g_is times their size (1 in the menus,
 * 2 on the desktop). IR: a rectangle in the icon's own coordinates. */
static int g_is = 1;
#define IR(dx, dy, w, h, c) gfx_fill_rect(x + (dx) * g_is, y + (dy) * g_is, (w) * g_is, (h) * g_is, (c))
static void icon_bevel(int x, int y, int w, int h, uint32_t base, uint32_t hi, uint32_t lo) {
    draw_bevel_box(x, y, w * g_is, h * g_is, base, hi, lo);
}

static void draw_icon_terminal(int x, int y, uint32_t bg) {
    (void)bg;
    icon_bevel(x, y, 14, 12, 0x00161D28u, 0x00475A78u, 0x000E1118u);
    gfx_draw_text_scaled(x + 2 * g_is, y + 2 * g_is, g_is, ">", 0x00E8EEF6u, 0x00161D28u);
}

static void draw_icon_files(int x, int y, uint32_t bg) {
    (void)bg;
    IR(0, 1, 6, 2, 0x00F4D35Eu);
    IR(0, 3, 14, 9, 0x00F4D35Eu);
    IR(1, 4, 12, 1, 0x00FFF1A8u);
}

/* a sheet of paper with lines */
static void draw_icon_notepad(int x, int y, uint32_t bg) {
    (void)bg;
    IR(1, 0, 11, 12, 0x00F2F2EAu);
    IR(1, 0, 11, 2, 0x005A86C8u);
    for (int i = 0; i < 3; i++) IR(3, 4 + i * 3, 7, 1, 0x00808890u);
}

/* a little globe */
static void draw_icon_browser(int x, int y, uint32_t bg) {
    (void)bg;
    IR(4, 0, 6, 12, 0x003A7BD5u);
    IR(1, 2, 12, 8, 0x003A7BD5u);
    IR(2, 1, 10, 10, 0x003A7BD5u);
    IR(3, 3, 4, 3, 0x0057B65Au);
    IR(8, 6, 3, 3, 0x0057B65Au);
    IR(1, 6, 12, 1, 0x00A9CCF5u);
    IR(6, 0, 1, 12, 0x00A9CCF5u);
}

/* four tiles */
static void draw_icon_apps(int x, int y, uint32_t bg) {
    (void)bg;
    IR(1, 0, 5, 5, 0x003A7BD5u);
    IR(8, 0, 5, 5, 0x0057B65Au);
    IR(1, 7, 5, 5, 0x00F4D35Eu);
    IR(8, 7, 5, 5, 0x00E07040u);
}

/* a little CPU graph */
static void draw_icon_taskmgr(int x, int y, uint32_t bg) {
    (void)bg;
    icon_bevel(x, y, 14, 12, 0x00161D28u, 0x00475A78u, 0x000E1118u);
    IR(2, 7, 2, 3, 0x0057B65Au);
    IR(5, 4, 2, 6, 0x0057B65Au);
    IR(8, 6, 2, 4, 0x0057B65Au);
    IR(11, 2, 2, 8, 0x0057B65Au);
}

/* a gear */
static void draw_icon_settings(int x, int y, uint32_t bg) {
    const uint32_t c = 0x00C8D2E0u;
    IR(5, 0, 4, 14, c);
    IR(0, 5, 14, 4, c);
    IR(2, 2, 10, 10, c);
    IR(1, 1, 3, 3, c);
    IR(10, 1, 3, 3, c);
    IR(1, 10, 3, 3, c);
    IR(10, 10, 3, 3, c);
    IR(5, 5, 4, 4, bg);
}

static void draw_icon_install(int x, int y, uint32_t bg) {
    (void)bg;
    IR(5, 0, 4, 6, 0x0080E080u);           /* the arrow */
    IR(2, 5, 10, 2, 0x0080E080u);
    IR(4, 7, 6, 1, 0x0080E080u);
    IR(6, 8, 2, 1, 0x0080E080u);
    IR(0, 10, 14, 4, 0x00C8D2E0u);         /* the disk */
    IR(10, 11, 2, 2, 0x0080E080u);
}

static __attribute__((unused)) void draw_icon_power(int x, int y, uint32_t bg) {
    (void)bg;
    icon_bevel(x, y, 12, 12, 0x00412A2Au, 0x00764A4Au, 0x00170D0Du);
    gfx_draw_text_scaled(x + 3 * g_is, y + 2 * g_is, g_is, "o", 0x00FFFFFFu, 0x00412A2Au);
}

/* Programs: a folder with app tiles in it */
static __attribute__((unused)) void draw_icon_programs(int x, int y, uint32_t bg) {
    (void)bg;
    IR(0, 1, 6, 2, 0x00D9B44Au);
    IR(0, 3, 14, 10, 0x00D9B44Au);
    IR(2, 5, 4, 3, 0x003A7BD5u);
    IR(8, 5, 4, 3, 0x0057B65Au);
    IR(2, 9, 4, 3, 0x00E07040u);
    IR(8, 9, 4, 3, 0x00F4F8FFu);
}

/* an installed app's own picture (/apps/<name>/icon.png), decoded once at
 * 28 x 28; the colour of its four corners (when they agree) is see-through */
#define PIC_N 28
#define PIC_CACHE 24
#define PIC_CLEAR 0xFF000000u
typedef struct { char name[PKG_NAME_MAX]; int file; uint32_t size; int ok; uint32_t px[PIC_N * PIC_N]; } app_pic_t;
static app_pic_t* g_pics;
static int        g_pic_next;

static const uint32_t* app_picture(const char* name) {
    if (!name || !name[0] || strlen(name) >= PKG_NAME_MAX) return NULL;
    char path[FS_PATH_LEN];
    ksnprintf(path, sizeof(path), "%s/%s/icon.png", PKG_DIR, name);
    int fi = fs_find_file(path);
    if (fi < 0) return NULL;
    fs_file_t* info = fs_file_info(fi);
    uint32_t size = info ? info->size : 0;
    if (!g_pics) g_pics = (app_pic_t*)kzalloc(sizeof(app_pic_t) * PIC_CACHE);
    if (!g_pics) return NULL;
    for (int i = 0; i < PIC_CACHE; i++)
        if (g_pics[i].name[0] && !strcmp(g_pics[i].name, name)) {
            if (g_pics[i].file == fi && g_pics[i].size == size) return g_pics[i].ok ? g_pics[i].px : NULL;
            g_pics[i].name[0] = 0;                 /* the app was installed again */
        }
    app_pic_t* e = &g_pics[g_pic_next];
    g_pic_next = (g_pic_next + 1) % PIC_CACHE;
    kstrlcpy(e->name, name, sizeof(e->name));
    e->file = fi;
    e->size = size;
    e->ok = 0;
    if (size == 0 || size > (512u << 10)) return NULL;
    fs_pin(fi);
    fs_file_t* f = fs_get_file(fi);
    image_t img;
    char err[64];
    if (f && f->content && image_decode((const uint8_t*)f->content, f->size, &img, err, sizeof(err)) == 0) {
        for (int yy = 0; yy < PIC_N; yy++)
            for (int xx = 0; xx < PIC_N; xx++) {
                /* the average of the pixels this one covers */
                int x0 = xx * img.w / PIC_N, x1 = (xx + 1) * img.w / PIC_N, y0 = yy * img.h / PIC_N, y1 = (yy + 1) * img.h / PIC_N;
                if (x1 <= x0) x1 = x0 + 1;
                if (y1 <= y0) y1 = y0 + 1;
                uint32_t r = 0, g = 0, b = 0, n = 0;
                for (int sy = y0; sy < y1 && sy < img.h; sy++)
                    for (int sx = x0; sx < x1 && sx < img.w; sx++) {
                        const uint8_t* q = img.rgb + ((long)sy * img.w + sx) * 3;
                        r += q[0]; g += q[1]; b += q[2]; n++;
                    }
                if (!n) n = 1;
                e->px[yy * PIC_N + xx] = (r / n) << 16 | (g / n) << 8 | (b / n);
            }
        const uint8_t* c = img.rgb;
        const uint8_t* c2 = img.rgb + (long)(img.w - 1) * 3;
        const uint8_t* c3 = img.rgb + (long)(img.h - 1) * img.w * 3;
        const uint8_t* c4 = img.rgb + ((long)img.h * img.w - 1) * 3;
        if (!memcmp(c, c2, 3) && !memcmp(c, c3, 3) && !memcmp(c, c4, 3)) {
            uint32_t key = (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
            for (int k = 0; k < PIC_N * PIC_N; k++) if (e->px[k] == key) e->px[k] = PIC_CLEAR;
        }
        image_free(&img);
        e->ok = 1;
    }
    fs_unpin(fi);
    return e->ok ? e->px : NULL;
}

/* an installed app: its picture, or a little window with its colour and
 * initial from its name */
static const char* g_app_icon_name = "";
static void draw_icon_app(int x, int y, uint32_t bg) {
    (void)bg;
    const uint32_t* pic = app_picture(g_app_icon_name);
    if (pic) {
        int n = 14 * g_is;                          /* 14 or 28 pixels */
        for (int yy = 0; yy < n; yy++) {
            const uint32_t* row = pic + (yy * PIC_N / n) * PIC_N;
            int xx = 0;
            while (xx < n) {                        /* runs of one colour: one rectangle */
                uint32_t c = row[xx * PIC_N / n];
                int run = 1;
                while (xx + run < n && row[(xx + run) * PIC_N / n] == c) run++;
                if (c != PIC_CLEAR) gfx_fill_rect(x + xx, y + yy, run, 1, c);
                xx += run;
            }
        }
        return;
    }
    static const uint32_t COLORS[6] = { 0x003A7BD5u, 0x0057B65Au, 0x00E07040u, 0x009B59B6u, 0x00D9B44Au, 0x0020A0A0u };
    uint32_t h = 0;
    for (const char* p = g_app_icon_name; *p; p++) h = h * 31u + (uint8_t)*p;
    uint32_t c = COLORS[h % 6];
    icon_bevel(x, y, 14, 13, 0x00E8ECF2u, 0x00FFFFFFu, 0x00707888u);
    IR(0, 0, 14, 3, c);
    char ini[2] = { g_app_icon_name[0] ? g_app_icon_name[0] : '?', 0 };
    if (ini[0] >= 'a' && ini[0] <= 'z') ini[0] = (char)(ini[0] - 32);
    gfx_draw_text_scaled(x + 3 * g_is, y + 4 * g_is, g_is, ini, c, 0x00E8ECF2u);
}

/* a tiny "photo" glyph: sky, sun, hill */
static void clamp_win(const fb_info_t* fi, term_win_t* w) {
    if (!fi) return;
    if (!w) return;
    if (w->w > (int)fi->width) w->w = (int)fi->width;
    if (w->h > (int)fi->height - 28) w->h = (int)fi->height - 28;
    if (w->w < 220) w->w = 220;
    if (w->h < 160) w->h = 160;
    if (w->x < 0) w->x = 0;
    if (w->y < 0) w->y = 0;
    if (w->x + w->w > (int)fi->width)  w->x = (int)fi->width - w->w;
    if (w->y + w->h > (int)fi->height) w->y = (int)fi->height - w->h;
    if (w->x < 0) w->x = 0;
    if (w->y < 0) w->y = 0;
}

#define GRIP 14                     /* the resize grip in a window's bottom-right corner */
#define TERM_TITLE_H 20
#define TERM_PAD 6

/* the window's client area decides its shell's text grid */
static void term_apply_size(term_win_t* w) {
    if (!w->vt) return;
    terminal_vt_set_size(w->vt, (w->w - TERM_PAD * 2) / gfx_cell_w(), (w->h - TERM_TITLE_H - TERM_PAD * 2) / gfx_cell_h());
}

/* first buffer row shown (the view follows the cursor) */
static int term_row_off(const term_win_t* w, int max_rows, int th) {
    size_t cr = 0, cc = 0;
    terminal_vt_get_cursor(w->vt, &cr, &cc);
    int row_off = (int)cr - max_rows + 1;
    if (row_off + max_rows > th) row_off = th - max_rows;
    if (row_off < 0) row_off = 0;
    return row_off;
}

/* the text cell under the mouse (buffer coordinates), clamped to the grid */
static void term_cell_at(const term_win_t* w, int mx, int my, int* r, int* c) {
    const char* chars;
    const uint8_t* cols;
    int tw, th, stride;
    terminal_vt_get_buffer(w->vt, &chars, &cols, &tw, &th, &stride);
    int max_cols = (w->w - TERM_PAD * 2) / gfx_cell_w(), max_rows = (w->h - TERM_TITLE_H - TERM_PAD * 2) / gfx_cell_h();
    if (max_cols > tw) max_cols = tw;
    if (max_rows > th) max_rows = th;
    int x = (mx - (w->x + TERM_PAD)) / gfx_cell_w(), y = (my - (w->y + TERM_TITLE_H + TERM_PAD)) / gfx_cell_h();
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= max_cols) x = max_cols - 1;
    if (y >= max_rows) y = max_rows - 1;
    *r = y + term_row_off(w, max_rows, th);
    *c = x;
}

static int term_cell_selected(const term_win_t* w, int r, int c) {
    if (!w->sel) return 0;
    int r0 = w->s_r0, c0 = w->s_c0, r1 = w->s_r1, c1 = w->s_c1;
    if (r1 < r0 || (r1 == r0 && c1 < c0)) { int t = r0; r0 = r1; r1 = t; t = c0; c0 = c1; c1 = t; }
    if (r < r0 || r > r1) return 0;
    if (r == r0 && c < c0) return 0;
    if (r == r1 && c > c1) return 0;
    return 1;
}

/* the selected text (lines without their trailing blanks) onto the clipboard */
static void term_copy_selection(const term_win_t* w) {
    const char* chars;
    const uint8_t* cols;
    int tw, th, stride;
    terminal_vt_get_buffer(w->vt, &chars, &cols, &tw, &th, &stride);
    int r0 = w->s_r0, c0 = w->s_c0, r1 = w->s_r1, c1 = w->s_c1;
    if (r1 < r0 || (r1 == r0 && c1 < c0)) { int t = r0; r0 = r1; r1 = t; t = c0; c0 = c1; c1 = t; }
    static char out[100 * 76 * 2];
    uint32_t n = 0;
    for (int r = r0; r <= r1 && r < th; r++) {
        int a = r == r0 ? c0 : 0, b = r == r1 ? c1 : tw - 1;
        uint32_t line_start = n;
        for (int c = a; c <= b && c < tw; c++) {
            char ch = chars[r * stride + c];
            if ((unsigned char)ch >= 0xA0) n += (uint32_t)u8_encode((unsigned char)ch, out + n);
            else out[n++] = ch ? ch : ' ';
        }
        while (n > line_start && out[n - 1] == ' ') n--;
        if (r < r1) out[n++] = '\n';
    }
    clipboard_set(out, n);
}

static void bring_term_front(int idx) {
    int pos = -1;
    for (int i = 0; i < TERM_WIN_MAX; i++) {
        if (g_term_order[i] == idx) { pos = i; break; }
    }
    if (pos < 0) return;
    for (int i = pos; i < TERM_WIN_MAX - 1; i++) g_term_order[i] = g_term_order[i + 1];
    g_term_order[TERM_WIN_MAX - 1] = idx;
}

/* the frontmost open terminal window at a point, as index + 1 (0 = none) */
static int term_at(int mx, int my) {
    for (int oi = TERM_WIN_MAX - 1; oi >= 0; oi--) {
        term_win_t* w = &g_terms[g_term_order[oi]];
        if (w->open && !w->minimized && mx >= w->x && mx < w->x + w->w && my >= w->y && my < w->y + w->h) return g_term_order[oi] + 1;
    }
    return 0;
}

static int open_new_terminal(void) {
    for (int i = 0; i < TERM_WIN_MAX; i++) {
        if (g_terms[i].open) continue;

        if (g_terms[i].vt == 0) {
            /* First time this slot has ever been opened: allocate its vt
             * and start its shell task. Both are kept forever after this
             * (see the term_win_t.vt comment) - closing the window later
             * just hides it, it does not free the vt or stop the task. */
            int vt = terminal_vt_alloc();
            if (vt < 0) continue; /* out of vts; maybe another slot is reusable */
            g_terms[i].vt = vt;
            task_create("term-sh", g_term_task_entry[i]);
        }

        g_terms[i].open = 1;
        g_terms[i].minimized = 0;
        g_terms[i].dragging = 0;
        g_terms[i].sel = 0;
        term_apply_size(&g_terms[i]);
        bring_term_front(i);
        return i;
    }
    /* none free: focus the frontmost */
    bring_term_front(g_term_order[TERM_WIN_MAX - 1]);
    return g_term_order[TERM_WIN_MAX - 1];
}

static void draw_terminal_window(const fb_info_t* fi, const term_win_t* win) {
    if (!win || !win->open || win->minimized) return;
    if (!fi) return;

    term_win_t w = *win;
    clamp_win(fi, &w);

    int title_h = 20;
    int pad = 6;

    int active = (g_term_order[TERM_WIN_MAX - 1] >= 0) && (&g_terms[g_term_order[TERM_WIN_MAX - 1]] == win);
    uint32_t frame = active ? 0x001A1E24u : 0x0013151Au;
    uint32_t body = active ? 0x00262A31u : 0x00202429u;
    uint32_t title = active ? 0x003C4B66u : 0x00273140u;
    uint32_t title_hi = active ? 0x006B7892u : 0x004B5568u;
    uint32_t title_lo = active ? 0x00111824u : 0x0010151Fu;

    /* frame + title (stronger Fluxbox-like bevel) */
    draw_bevel_box(w.x, w.y, w.w, w.h, frame, 0x004A5466u, 0x000E1116u);
    gfx_fill_rect(w.x + 2, w.y + 2, w.w - 4, w.h - 4, body);
    draw_bevel_box(w.x + 3, w.y + 3, w.w - 6, title_h - 1, title, title_hi, title_lo);
    gfx_draw_text(w.x + 10, w.y + 7, "Terminal", active ? 0x00F2F7FFu : 0x00CED8E6u, title);

    /* minimize, maximize / restore, close */
    win_draw_button_row(w.x + w.w, w.y + 4, 12, 1, w.maxed);

    /* client area */
    int cx = w.x + pad;
    int cy = w.y + title_h + pad;
    int cw = w.w - pad * 2;
    int ch = w.h - title_h - pad * 2;
    gfx_fill_rect(cx, cy, cw, ch, 0x00000000u);

    const char* chars;
    const uint8_t* cols;
    int tw, th, stride;
    terminal_vt_get_buffer(win->vt, &chars, &cols, &tw, &th, &stride);

    int CW = gfx_cell_w(), CH = gfx_cell_h();
    int max_cols = cw / CW;
    int max_rows = ch / CH;
    if (max_cols > tw) max_cols = tw;
    if (max_rows > th) max_rows = th;

    /* Every vt is sized to the full screen's text grid (kernel/terminal.c),
     * which is normally much taller than a terminal window's own client
     * area - a window this size only ever showed rows 0..max_rows of that
     * much bigger buffer, so once the cursor scrolled past the bottom of
     * the window it just kept going in buffer rows nobody drew, and
     * everything after looked "cut off" with no way to scroll to it.
     * Follow the cursor instead: keep it pinned to the window's last
     * visible row once the content grows past what the window can show,
     * the same way a real terminal emulator's viewport tracks output. */
    size_t cr = 0, cc = 0;
    terminal_vt_get_cursor(win->vt, &cr, &cc);
    int row_off = term_row_off(win, max_rows, th);

    for (int y = 0; y < max_rows; y++) {
        for (int x = 0; x < max_cols; x++) {
            int idx = (y + row_off) * stride + x;
            uint8_t color = cols[idx];
            char c = chars[idx];
            if (term_cell_selected(win, y + row_off, x)) {     /* selection: inverted */
                gfx_draw_cell_char(cx + x * CW, cy + y * CH, c ? c : ' ', 0x00101010u, 0x00C8D8F0u);
                continue;
            }
            /* the client area is already black: skip blank black cells */
            if ((c == ' ' || c == 0) && (color & 0xF0) == 0) continue;
            gfx_draw_cell_char(cx + x * CW, cy + y * CH, c,
                               vga_color_rgb(color & 0x0F), vga_color_rgb((color >> 4) & 0x0F));
        }
    }

    gfx_draw_grip(w.x + w.w, w.y + w.h);

    {
        int scr_row = (int)cr - row_off;
        if (scr_row >= 0 && scr_row < max_rows && (int)cc < max_cols) {
            int idx = (int)cr * stride + (int)cc;
            uint8_t color = cols[idx];
            uint8_t fg = color & 0x0F;
            gfx_fill_rect(cx + (int)cc * CW, cy + scr_row * CH + CH - 1, CW, 1, vga_color_rgb(fg));
        }
    }
}

static void u32_to_2dig(uint32_t v, char out[2]) {
    out[0] = (char)('0' + ((v / 10u) % 10u));
    out[1] = (char)('0' + (v % 10u));
}

static void format_clock(char out[9]) {
    static uint32_t last_read_sec = (uint32_t)-1;
    static rtc_datetime_t cached = {0, 0, 0, 0, 0, 0};
    uint32_t now_sec = timer_ticks() / 100u;

    if (now_sec != last_read_sec) {
        rtc_datetime_t dt;
        if (rtc_read_datetime(&dt) == 0) {
            cached = dt;
        } else {
            cached.hour = (uint8_t)((now_sec / 3600u) % 24u);
            cached.minute = (uint8_t)((now_sec / 60u) % 60u);
            cached.second = (uint8_t)(now_sec % 60u);
        }
        last_read_sec = now_sec;
    }

    {
        const rtc_datetime_t* dt = &cached;
        u32_to_2dig(dt->hour, &out[0]);
        out[2] = ':';
        u32_to_2dig(dt->minute, &out[3]);
        out[5] = ':';
        u32_to_2dig(dt->second, &out[6]);
        out[8] = '\0';
    }
}

/* "eth0 10.0.2.15" / "usb0 (dhcp...)" / "no network" */
static void format_net_status(char* out, uint32_t cap) {
    netif_t* nif = net_if();
    if (!nif->dev) { kstrlcpy(out, "no network", cap); return; }
    if (!nif->configured) { ksnprintf(out, cap, "%s (dhcp...)", nif->dev->ifname); return; }
    char ip[16];
    ip4_to_str(nif->ip, ip);
    ksnprintf(out, cap, "%s %s", nif->dev->ifname, ip);
}

static void draw_taskbar(void) {
    /* background bar */
    for (size_t x = 0; x < VGA_WIDTH; x++) {
        terminal_putentryat(' ', VGA_COLOR_BLACK, VGA_COLOR_LIGHT_GREY, TASKBAR_ROW, x);
    }

    terminal_write_at(" [Start] ", VGA_COLOR_BLACK, VGA_COLOR_LIGHT_GREY, TASKBAR_ROW, 0);

    char clk[9];
    format_clock(clk);
    size_t clk_len = 8;
    size_t clk_col = (VGA_WIDTH > (clk_len + 1)) ? (VGA_WIDTH - (clk_len + 1)) : 0;
    terminal_write_at(clk, VGA_COLOR_BLACK, VGA_COLOR_LIGHT_GREY, TASKBAR_ROW, clk_col);
}

static void draw_menu(void) {
    if (!g_menu_open) return;

    const size_t menu_x = 0;
    const size_t menu_y = TASKBAR_ROW - (MENU_ITEMS + 1);
    const size_t menu_w = 20;
    const size_t menu_h = MENU_ITEMS;

    /* box background */
    for (size_t y = 0; y < menu_h; y++) {
        for (size_t x = 0; x < menu_w; x++) {
            terminal_putentryat(' ', VGA_COLOR_WHITE, VGA_COLOR_DARK_GREY, menu_y + y, menu_x + x);
        }
    }

    /* items */
    for (int i = 0; i < MENU_ITEMS; i++) {
        uint8_t fg = VGA_COLOR_WHITE;
        uint8_t bg = VGA_COLOR_DARK_GREY;
        if (g_menu_sel == i) { fg = VGA_COLOR_WHITE; bg = VGA_COLOR_BLUE; }

        const char* item = MENU_LABELS[menu_idx(i)];
        /* fill the rest of the row in highlight color for clean look */
        for (size_t x = 0; x < menu_w - 2; x++) {
            terminal_putentryat(' ', fg, bg, menu_y + (size_t)i, menu_x + 1 + x);
        }
        terminal_write_at(item,
                          fg, bg, menu_y + (size_t)i, menu_x + 1);
    }

    /* hint row */
    terminal_write_at(" Enter = open", VGA_COLOR_LIGHT_GREY, VGA_COLOR_DARK_GREY, menu_y + MENU_ITEMS, menu_x + 1);
}

static void menu_close_redraw(void) {
    g_menu_open = 0;
    draw_taskbar();
    /* clear menu area */
    for (size_t y = 0; y < (size_t)MENU_ITEMS + 1; y++) {
        for (size_t x = 0; x < 20; x++) {
            terminal_putentryat(' ', VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK, (TASKBAR_ROW - (MENU_ITEMS + 1)) + y, x);
        }
    }
    draw_taskbar();
}


void gui_init(void) {
    if (gfx_available()) {
        /* framebuffer desktop is opt-in via startx */
        return;
    }

    terminal_set_reserved_bottom(1);
    draw_taskbar();
}

/* File-scope (not gui_poll()-local) so gui_set_enabled() can tear the
 * backbuffer down on exit: without this, fb_present() keeps copying the
 * full 800x600 backbuffer to the real framebuffer on every single
 * terminal_putchar() - including plain shell typing - forever after the
 * first startx, since nothing ever called fb_clear_backbuffer(). */
/* The desktop is as big as the screen: GRUB asks for 800x600, but UEFI
 * firmware often only offers the panel's native mode (1920x1080, ...),
 * so the backbuffer and the wallpaper cache are sized at the first startx
 * (up to 2560x1600; the 800x600 static ones if that allocation fails). */
#define DESK_MAX_W 2560u
#define DESK_MAX_H 1600u
static uint32_t  g_desk_static[800u * 600u];
static uint32_t  g_wall_static[800u * 600u];
static uint32_t* g_desktop_backbuf = g_desk_static;
static uint32_t* g_wallpaper_cache = g_wall_static;
static uint32_t  g_desk_w = 800, g_desk_h = 600;
static int       g_backbuf_active = 0;

/* Cache of the rendered wallpaper: rendering it (bilinear upscale of a
 * preset, or copying a decoded user picture) only happens when the
 * wallpaper actually changes, never per frame. */
static uint32_t g_wallpaper_cache_gen = 0;

static void size_desktop(const fb_info_t* fi) {
    uint32_t w = fi->width > DESK_MAX_W ? DESK_MAX_W : fi->width;
    uint32_t h = fi->height > DESK_MAX_H ? DESK_MAX_H : fi->height;
    if (w != g_desk_w || h != g_desk_h) g_wallpaper_cache_gen = (uint32_t)-1;   /* (a new size: redrawn at it) */
    if (w <= 800 && h <= 600) { g_desk_w = w; g_desk_h = h; return; }
    if (g_desktop_backbuf != g_desk_static && w == g_desk_w && h == g_desk_h) return;
    uint32_t* bb = (uint32_t*)kmalloc(w * h * 4);
    uint32_t* wc = (uint32_t*)kmalloc(w * h * 4);
    if (!bb || !wc) {
        kfree(bb); kfree(wc);
        g_desk_w = fi->width < 800 ? fi->width : 800;
        g_desk_h = fi->height < 600 ? fi->height : 600;
        return;
    }
    g_desktop_backbuf = bb;
    g_wallpaper_cache = wc;
    g_desk_w = w;
    g_desk_h = h;
    g_wallpaper_cache_gen = (uint32_t)-1;
}

static void refresh_wallpaper_cache(const fb_info_t* fi) {
    (void)fi;
    if (g_wallpaper_cache_gen == wallpaper_generation()) return;
    wallpaper_render(g_wallpaper_cache, (int)g_desk_w, (int)g_desk_h, (int)g_desk_w);
    g_wallpaper_cache_gen = wallpaper_generation();
}

static void blit_wallpaper_cache(void) {
    memcpy(g_desktop_backbuf, g_wallpaper_cache, (size_t)g_desk_w * g_desk_h * 4);
}

/* ── mouse cursor ──────────────────────────────────────────────────
 * Drawn straight onto the framebuffer over the presented frame, so when
 * only the mouse moves, restoring the old spot from the backbuffer and
 * drawing the cursor elsewhere is all it takes - no full repaint.
 * Shapes: the arrow, the arrow with an hourglass (the browser is loading)
 * and a diagonal double arrow (resizing a window). */
#define CURSOR_X0 (-8)                /* the area any shape covers, around the hot spot */
#define CURSOR_Y0 (-8)
#define CURSOR_W  30
#define CURSOR_H  32
enum { CUR_ARROW, CUR_BUSY, CUR_RESIZE };

static const char* const CUR_ARROW_BITS[] = {
    "B...........",
    "BB..........",
    "BWB.........",
    "BWWB........",
    "BWWWB.......",
    "BWWWWB......",
    "BWWWWWB.....",
    "BWWWWWWB....",
    "BWWWWWWWB...",
    "BWWWWWWWWB..",
    "BWWWWWWWWWB.",
    "BWWWWWWBBBBB",
    "BWWWBWWB....",
    "BWWBBWWB....",
    "BWB..BWWB...",
    "BB...BWWB...",
    "B.....BWWB..",
    "......BWWB..",
    ".......BB...",
};
static const char* const CUR_HOURGLASS_BITS[] = {
    "BBBBBBBBB",
    "BWWWWWWWB",
    ".BWWWWWB.",
    "..BWWWB..",
    "...BWB...",
    "...BWB...",
    "..BW.WB..",
    ".BW...WB.",
    "BWWWWWWWB",
    "BBBBBBBBB",
};

static void cursor_bits(int x, int y, const char* const* rows, int n) {
    for (int r = 0; r < n; r++)
        for (int c = 0; rows[r][c]; c++) {
            char ch = rows[r][c];
            if (ch == 'B') fb_putpixel_direct(x + c, y + r, 0x00000000u);
            else if (ch == 'W') fb_putpixel_direct(x + c, y + r, 0x00FFFFFFu);
        }
}

/* a diagonal double arrow, 15x15 around the hot spot */
static int resize_white(int x, int y) {
    if (x < 0 || y < 0 || x > 14 || y > 14) return 0;
    int d = x - y;
    return x + y <= 6 || (14 - x) + (14 - y) <= 6 || (d >= -1 && d <= 1);
}

static void draw_cursor_shape(int mx, int my, int shape) {
    if (shape == CUR_RESIZE) {
        for (int y = -1; y <= 15; y++)
            for (int x = -1; x <= 15; x++) {
                if (resize_white(x, y)) { fb_putpixel_direct(mx - 7 + x, my - 7 + y, 0x00FFFFFFu); continue; }
                int edge = 0;
                for (int dy = -1; dy <= 1 && !edge; dy++)
                    for (int dx = -1; dx <= 1; dx++) if (resize_white(x + dx, y + dy)) { edge = 1; break; }
                if (edge) fb_putpixel_direct(mx - 7 + x, my - 7 + y, 0x00000000u);
            }
        return;
    }
    cursor_bits(mx, my, CUR_ARROW_BITS, (int)(sizeof(CUR_ARROW_BITS) / sizeof(CUR_ARROW_BITS[0])));
    if (shape == CUR_BUSY)
        cursor_bits(mx + 12, my + 14, CUR_HOURGLASS_BITS, (int)(sizeof(CUR_HOURGLASS_BITS) / sizeof(CUR_HOURGLASS_BITS[0])));
}

/* the pointer in hardware (kernel/gpu.h), when the display has one: its
 * image is the shape, and the display moves it - nothing to restore */
static int g_hw_shape = -1;               /* the image the hardware has (-1: none) */
static int g_hw_failed;

static void img_bits(uint32_t* img, int x, int y, const char* const* rows, int n) {
    for (int r = 0; r < n; r++)
        for (int c = 0; rows[r][c]; c++) {
            int px = x + c, py = y + r;
            if (px < 0 || py < 0 || px >= 32 || py >= 32) continue;
            if (rows[r][c] == 'B') img[py * 32 + px] = 0xFF000000u;
            else if (rows[r][c] == 'W') img[py * 32 + px] = 0xFFFFFFFFu;
        }
}

static void shape_image(int shape, uint32_t* img, int* hx, int* hy) {
    memset(img, 0, 32 * 32 * 4);
    if (shape == CUR_RESIZE) {
        for (int y = -1; y <= 15; y++)
            for (int x = -1; x <= 15; x++) {
                uint32_t* p = &img[(y + 1) * 32 + (x + 1)];
                if (resize_white(x, y)) { *p = 0xFFFFFFFFu; continue; }
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) if (resize_white(x + dx, y + dy)) *p = 0xFF000000u;
            }
        *hx = 8;
        *hy = 8;
        return;
    }
    img_bits(img, 0, 0, CUR_ARROW_BITS, (int)(sizeof(CUR_ARROW_BITS) / sizeof(CUR_ARROW_BITS[0])));
    if (shape == CUR_BUSY) img_bits(img, 12, 14, CUR_HOURGLASS_BITS, (int)(sizeof(CUR_HOURGLASS_BITS) / sizeof(CUR_HOURGLASS_BITS[0])));
    *hx = 0;
    *hy = 0;
}

/* 1 if the hardware shows the pointer at (mx, my) */
static int hw_cursor(int mx, int my, int shape) {
    if (g_hw_failed || !gpu_has_hw_cursor()) return 0;
    if (shape != g_hw_shape) {
        static uint32_t img[32 * 32];
        int hx, hy;
        shape_image(shape, img, &hx, &hy);
        if (gpu_cursor_image(img, 32, 32, hx, hy) < 0) { g_hw_failed = 1; return 0; }
        g_hw_shape = shape;
    }
    gpu_cursor_move(mx, my, 1);
    return 1;
}

static void hw_cursor_hide(void) {
    if (g_hw_shape >= 0) gpu_cursor_move(0, 0, 0);
    g_hw_shape = -1;
}

static int cursor_shape(int mx, int my);
static int g_drawn_shape;

static void draw_cursor(int mx, int my) {
    g_drawn_shape = cursor_shape(mx, my);
    if (hw_cursor(mx, my, g_drawn_shape)) return;
    draw_cursor_shape(mx, my, g_drawn_shape);
}

/* The timer interrupt, every millisecond: when the mouse moved and no task
 * is drawing the desktop, the pointer is moved here - restored spot, drawn
 * anew, on the presented frame - so it follows the hand at full speed
 * even while another task keeps the CPU (saving files, a heavy page).
 * Clicks, the wheel and what the pointer is over are still the desktop
 * loop's, next time it runs; it finds the pointer already in place. */
void gui_cursor_tick(void) {
    if (g_gui_busy || !g_gui_enabled || !g_backbuf_active || g_in_lock || g_lock_pending) return;
    if (g_ptr_drawn_x < 0 || g_scr_w <= 0 || g_scr_h <= 0) return;
    int dx, dy;
    if (!mouse_irq_motion(&dx, &dy)) return;
    int x = g_cur_mx + dx, y = g_cur_my - dy;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > g_scr_w - 1) x = g_scr_w - 1;
    if (y > g_scr_h - 1) y = g_scr_h - 1;
    g_cur_mx = x;
    g_cur_my = y;
    if (x == g_ptr_drawn_x && y == g_ptr_drawn_y) return;
    if (g_hw_shape >= 0) {                    /* the hardware pointer: only moved */
        gpu_t* gp = gpu_active();
        if (!gp || !gp->cursor_irq_safe) return;   /* (moved by the desktop loop instead) */
        gpu_cursor_move(x, y, 1);
        g_ptr_drawn_x = x;
        g_ptr_drawn_y = y;
        return;
    }
    fb_present_rect(g_ptr_drawn_x + CURSOR_X0, g_ptr_drawn_y + CURSOR_Y0, CURSOR_W, CURSOR_H);
    draw_cursor_shape(x, y, g_drawn_shape);
    g_ptr_drawn_x = x;
    g_ptr_drawn_y = y;
}

static int cursor_shape(int mx, int my) {
    for (int i = 0; i < TERM_WIN_MAX; i++) {
        term_win_t* w = &g_terms[i];
        if (!w->open || w->minimized) continue;
        if (w->resizing || (mx >= w->x + w->w - GRIP && mx < w->x + w->w && my >= w->y + w->h - GRIP && my < w->y + w->h))
            return CUR_RESIZE;
    }
    if (appwin_resize_cursor(mx, my)) return CUR_RESIZE;
    if (browser_busy() && browser_contains(mx, my)) return CUR_BUSY;
    return CUR_ARROW;
}

/* ── windows on the taskbar ────────────────────────────────────────
 * Every open window gets a taskbar button, in the order the windows
 * were opened. A window is named by a handle: kind << 8 | index. */
#define WK_TERM   1
#define WK_APP    2
#define WK_APPWIN 3
#define TB_MAX    24
static int      g_tb[TB_MAX];
static int      g_ntb;
static uint32_t g_tb_gen;

static int win_exists(int h) {
    int k = h >> 8, i = h & 0xFF;
    if (k == WK_TERM) return i < TERM_WIN_MAX && g_terms[i].open;
    if (k == WK_APP) return i < APP_APPWIN && g_apps[i].is_open();
    if (k == WK_APPWIN) return appwin_info(i, NULL, 0, NULL);
    return 0;
}

static int win_minimized(int h) {
    int k = h >> 8, i = h & 0xFF;
    if (k == WK_TERM) return g_terms[i].minimized;
    if (k == WK_APP) return g_app_min[i];
    int m = 0;
    appwin_info(i, NULL, 0, &m);
    return m;
}

/* the window that has the focus */
static int win_front(void) {
    if (g_front_app == APP_APPWIN) { int id = appwin_front_id(); return id >= 0 ? (WK_APPWIN << 8 | id) : 0; }
    if (g_front_app >= 0) return app_visible(g_front_app) ? (WK_APP << 8 | g_front_app) : 0;
    for (int oi = TERM_WIN_MAX - 1; oi >= 0; oi--) {
        int wi = g_term_order[oi];
        if (g_terms[wi].open && !g_terms[wi].minimized) return WK_TERM << 8 | wi;
    }
    return 0;
}

static void win_title(int h, char* out, int cap) {
    int k = h >> 8, i = h & 0xFF;
    if (k == WK_TERM) {
        int n = 0;
        for (int j = 0; j <= i; j++) if (g_terms[j].open) n++;
        ksnprintf(out, (size_t)cap, "Terminal %d", n);
    } else if (k == WK_APP) {
        kstrlcpy(out, g_apps[i].title, (size_t)cap);
    } else {
        appwin_info(i, out, cap, NULL);
    }
}

/* keeps g_tb in step with the windows that exist */
static void sync_taskbar(void) {
    int n = 0;
    for (int j = 0; j < g_ntb; j++) if (win_exists(g_tb[j])) g_tb[n++] = g_tb[j];
    if (n != g_ntb) g_tb_gen++;
    g_ntb = n;
    int cand[TB_MAX], nc = 0;
    for (int i = 0; i < TERM_WIN_MAX; i++) cand[nc++] = WK_TERM << 8 | i;
    for (int a = 0; a < APP_APPWIN; a++) cand[nc++] = WK_APP << 8 | a;
    for (int i = 0; i < APPWIN_MAX && nc < TB_MAX; i++) cand[nc++] = WK_APPWIN << 8 | i;
    for (int c = 0; c < nc; c++) {
        if (!win_exists(cand[c])) {
            /* a closed window forgets it was minimized */
            int k = cand[c] >> 8, i = cand[c] & 0xFF;
            if (k == WK_TERM) g_terms[i].minimized = 0;
            if (k == WK_APP) g_app_min[i] = 0;
            continue;
        }
        int have = 0;
        for (int j = 0; j < g_ntb; j++) if (g_tb[j] == cand[c]) have = 1;
        if (!have && g_ntb < TB_MAX) { g_tb[g_ntb++] = cand[c]; g_tb_gen++; }
    }
}

static void win_activate(int h) {
    int k = h >> 8, i = h & 0xFF;
    if (k == WK_TERM) { g_terms[i].minimized = 0; bring_term_front(i); g_front_app = -1; }
    else if (k == WK_APP) { g_app_min[i] = 0; raise_app(i); }
    else if (k == WK_APPWIN) { appwin_activate(i); raise_app(APP_APPWIN); }
    g_tb_gen++;
}

static void win_minimize(int h) {
    int k = h >> 8, i = h & 0xFF;
    if (k == WK_TERM) g_terms[i].minimized = 1;
    else if (k == WK_APP) { g_app_min[i] = 1; if (g_front_app == i) g_front_app = -1; }
    else if (k == WK_APPWIN) { appwin_minimize(i, 1); if (!appwin_any_visible() && g_front_app == APP_APPWIN) g_front_app = -1; }
    g_tb_gen++;
}

static void win_close(int h) {
    int k = h >> 8, i = h & 0xFF;
    if (k == WK_TERM) { g_terms[i].open = 0; g_terms[i].minimized = 0; }
    else if (k == WK_APP) { g_apps[i].close(); g_app_min[i] = 0; if (g_front_app == i) g_front_app = -1; }
    else if (k == WK_APPWIN) appwin_request_close(i);
    g_tb_gen++;
}

/* taskbar geometry: Start button, window buttons, then the tray */
#define BAR_H     28
#define START_X   4
#define START_W   STARTMENU_ORB_W
/* apps pinned to the taskbar (right of Start): a program (ACT_*) or an
 * installed app; kept in /etc/settings.conf as taskbar_pins */
#define PIN_MAX   12
#define PIN_W     38
typedef struct { int act; char app[PKG_NAME_MAX]; } pin_t;
static pin_t g_pins[PIN_MAX];
static int   g_npins = -1;                /* -1: not read yet */
static int   g_tb_style;                  /* 0 icons and titles, 1 icons, 2 titles */
static void  pins_load(void);
#define PINS_X    (START_X + START_W + 6)
static void pins_draw(const fb_info_t* fi);
static int tb_x(void) { if (g_npins < 0) pins_load(); return PINS_X + g_npins * PIN_W + (g_npins ? 6 : 2); }
#define TB_X      (tb_x())
#define TB_BTN_W  (g_tb_style == 1 ? 44 : 150)

static int tray_x(const fb_info_t* fi) {
    char net[40];
    netif_t* nif = net_if();
    if (!nif->dev) kstrlcpy(net, "no network", sizeof(net));
    else ksnprintf(net, sizeof(net), "%s 255.255.255.255", nif->dev->ifname);
    int badge = sysinfo_live_boot() ? 8 * 8 + 18 : 0;     /* "LIVE CD" */
    return (int)fi->width - 8 - 64 - 16 - (int)strlen(net) * 8 - badge;
}

static int tb_btn_w(const fb_info_t* fi) {
    int avail = tray_x(fi) - 12 - TB_X;
    if (!g_ntb) return TB_BTN_W;
    int w = avail / g_ntb;
    return w > TB_BTN_W ? TB_BTN_W : w;
}

/* the taskbar button under the mouse, -1 if none */
static int tb_button_at(const fb_info_t* fi, int mx, int my) {
    int bar_y = (int)fi->height - BAR_H;
    if (my < bar_y + 3 || my >= bar_y + BAR_H - 3) return -1;
    int bw = tb_btn_w(fi);
    if (mx < TB_X || bw < 8) return -1;
    int j = (mx - TB_X) / bw;
    return j < g_ntb ? j : -1;
}

/* a little glyph per kind of window */
static void draw_win_glyph(int h, int x, int y, uint32_t bg) {
    int k = h >> 8, i = h & 0xFF;
    if (k == WK_TERM) { draw_icon_terminal(x, y, bg); return; }
    if (k == WK_APP) {
        if (i == APP_FILES) draw_icon_files(x, y, bg);
        else if (i == APP_BROWSER) draw_icon_browser(x, y, bg);
        else if (i == APP_NOTEPAD) draw_icon_notepad(x, y, bg);
        else if (i == APP_LAUNCHER) draw_icon_apps(x, y, bg);
        else if (i == APP_SETTINGS) draw_icon_settings(x, y, bg);
        else if (i == APP_INSTALLER) draw_icon_install(x, y, bg);
        else draw_icon_taskmgr(x, y, bg);
        return;
    }
    draw_bevel_box(x, y, 14, 12, 0x00D0A030u, 0x00F0D070u, 0x00604010u);
}

static void draw_taskbar_fb(const fb_info_t* fi) {
    int bar_y = (int)fi->height - BAR_H;
    draw_bevel_box(0, bar_y, (int)fi->width, BAR_H, 0x00192026u, 0x004F5A6Eu, 0x0010141Bu);
    /* Start: the orb */
    startmenu_draw_orb(START_X, bar_y, BAR_H, g_menu_open, g_start_hover);
    /* the pinned apps */
    pins_draw(fi);
    /* one button per window */
    int bw = tb_btn_w(fi), front = win_front();
    for (int j = 0; j < g_ntb; j++) {
        int h = g_tb[j];
        int x = TB_X + j * bw;
        int active = h == front, min = win_minimized(h);
        uint32_t base = active ? 0x00405478u : min ? 0x00222831u : 0x00303740u;
        uint32_t hi = active ? 0x00222A36u : 0x00535D6Eu, lo = active ? 0x006B7892u : 0x0015191Fu;
        draw_bevel_box(x, bar_y + 3, bw - 4, BAR_H - 6, base, hi, lo);
        if (g_tb_style == 1) {                         /* icons only */
            draw_win_glyph(h, x + (bw - 4 - 14) / 2, bar_y + 8, base);
            continue;
        }
        int tx = x + 26;
        if (g_tb_style == 2) tx = x + 8;               /* titles only */
        else if (bw >= 40) draw_win_glyph(h, x + 6, bar_y + 8, base);
        char t[48];
        win_title(h, t, sizeof(t));
        int maxc = (x + bw - 8 - tx) / 8;
        if (maxc < 1) continue;
        if ((int)strlen(t) > maxc) { t[maxc] = 0; if (maxc > 1) t[maxc - 1] = '.'; }
        gfx_draw_text(tx, bar_y + 10, t, min ? 0x009AA6B6u : 0x00E8EEF6u, base);
    }
    /* tray: network, clock */
    char clk[9], net[40];
    format_clock(clk);
    format_net_status(net, sizeof(net));
    int clk_x = (int)fi->width - 8 - 64;
    gfx_fill_rect(clk_x - 10, bar_y + 6, 1, BAR_H - 12, 0x004F5A6Eu);
    gfx_draw_text(clk_x, bar_y + 10, clk, 0x00E8EEF6u, 0x00192026u);
    uint32_t col = net_if()->configured ? 0x008FE3A1u : 0x00C9A45Cu;
    int net_x = clk_x - 18 - (int)strlen(net) * 8;
    gfx_draw_text(net_x, bar_y + 10, net, col, 0x00192026u);
    /* started from the CD: say so (nothing is kept unless installed) */
    if (sysinfo_live_boot()) {
        int bx = net_x - 14 - 7 * 8 - 8;
        gfx_fill_rect(bx, bar_y + 5, 7 * 8 + 8, BAR_H - 10, 0x00D9822Bu);
        gfx_draw_text(bx + 4, bar_y + 10, "LIVE CD", 0x00FFFFFFu, 0x00D9822Bu);
    }
}

/* Everything that affects what the desktop looks like (besides
 * wallpaper/terminal content, which have their own change counters).
 * A frame is only rendered when this, those counters, or the clock's
 * second changed - the desktop used to be fully repainted ~33 times a
 * second even when nothing at all was happening. */
typedef struct {
    int menu_open, menu_sel;
    int term_open[TERM_WIN_MAX], term_x[TERM_WIN_MAX], term_y[TERM_WIN_MAX], term_order[TERM_WIN_MAX];
    int term_w[TERM_WIN_MAX], term_h[TERM_WIN_MAX], term_sel[TERM_WIN_MAX], term_min[TERM_WIN_MAX];
    int front_app, app_order[APP_COUNT], app_min[APP_COUNT];
    uint32_t sec, term_gen, wp_gen, net_state, app_sig[APP_COUNT], ctx_sig, tb_gen;
    uint32_t desk_sig;
    int desk_sel, sub_slot;
} gui_view_t;

static void capture_view(gui_view_t* v, uint32_t sec) {
    memset(v, 0, sizeof(*v));
    v->menu_open = g_menu_open;
    v->menu_sel = g_menu_sel;
    v->desk_sig = g_desk_sig;
    v->desk_sel = g_desk_sel;
    v->sub_slot = g_sub_slot;
    for (int i = 0; i < TERM_WIN_MAX; i++) {
        v->term_open[i] = g_terms[i].open;
        v->term_x[i] = g_terms[i].x;
        v->term_y[i] = g_terms[i].y;
        v->term_order[i] = g_term_order[i];
        v->term_w[i] = g_terms[i].w;
        v->term_h[i] = g_terms[i].h;
        v->term_min[i] = g_terms[i].minimized;
        v->term_sel[i] = g_terms[i].sel ? g_terms[i].s_r0 * 7919 + g_terms[i].s_c0 * 31 + g_terms[i].s_r1 * 131 + g_terms[i].s_c1 + 1 : 0;
    }
    v->sec = sec;
    v->term_gen = terminal_generation();
    v->wp_gen = wallpaper_generation();
    v->net_state = net_if()->configured ? net_if()->ip : 1;
    v->front_app = g_front_app;
    for (int a = 0; a < APP_COUNT; a++) {
        v->app_order[a] = g_app_order[a];
        v->app_sig[a] = g_apps[a].signature();
        v->app_min[a] = g_app_min[a];
    }
    v->ctx_sig = ctxmenu_signature() ^ (g_menu_open ? startmenu_signature() : 0) ^ (uint32_t)g_start_hover * 0x9E37u;
    v->tb_gen = g_tb_gen;
}

static gui_view_t g_last_view;
static int        g_force_redraw = 1;


static int start_menu_y(const fb_info_t* fi) { return (int)fi->height - BAR_H - START_MENU_H - 4; }

/* ── the desktop's icons ─────────────────────────────────────────────── */

static void desk_add(int act, const char* label, const char* app) {
    if (g_ndesk >= DESK_MAX) return;
    desk_item_t* d = &g_desk[g_ndesk++];
    memset(d, 0, sizeof(*d));
    d->act = act;
    kstrlcpy(d->label, label, sizeof(d->label));
    kstrlcpy(d->app, app ? app : "", sizeof(d->app));
    d->kind = DK_APP;
}

static void desk_add_path(int kind, const char* label, const char* path) {
    if (g_ndesk >= DESK_MAX) return;
    desk_item_t* d = &g_desk[g_ndesk++];
    memset(d, 0, sizeof(*d));
    d->act = -1;
    d->kind = kind;
    kstrlcpy(d->label, label, sizeof(d->label));
    kstrlcpy(d->path, path ? path : "", sizeof(d->path));
}

/* ── where the icons are: ~/.config/desktop-icons ("key<TAB>col<TAB>row"),
 *    the rest fill the free cells in columns from the top left ── */
#define LAYOUT_FILE "/home/banana/.config/desktop-icons"
#define LAYOUT_MAX  128
static struct { char key[56]; int col, row; } g_lay[LAYOUT_MAX];
static int g_nlay = -1;

static void desk_key(const desk_item_t* d, char* out, int cap) {
    if (d->kind == DK_TRASH) kstrlcpy(out, "trash", (size_t)cap);
    else if (d->kind == DK_APP) ksnprintf(out, (size_t)cap, "app:%s", d->label);
    else ksnprintf(out, (size_t)cap, "file:%s", d->label);
}

static void layout_load(void) {
    g_nlay = 0;
    int fi = fs_find_file(LAYOUT_FILE);
    fs_file_t* f = fi >= 0 ? fs_get_file(fi) : NULL;
    if (!f || !f->content) return;
    const char* p = f->content;
    while (*p && g_nlay < LAYOUT_MAX) {
        const char* nl = strchr(p, '\n');
        int n = nl ? (int)(nl - p) : (int)strlen(p);
        char line[96];
        if (n >= (int)sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, p, (size_t)n);
        line[n] = 0;
        char* t1 = strchr(line, '\t');
        char* t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (t1 && t2) {
            *t1 = 0; *t2 = 0;
            uint32_t c = 0, r = 0;
            k_parse_u32(t1 + 1, &c);
            k_parse_u32(t2 + 1, &r);
            kstrlcpy(g_lay[g_nlay].key, line, sizeof(g_lay[0].key));
            g_lay[g_nlay].col = (int)c;
            g_lay[g_nlay].row = (int)r;
            g_nlay++;
        }
        if (!nl) break;
        p = nl + 1;
    }
}

static void layout_save(void) {
    static char buf[LAYOUT_MAX * 72];
    int o = 0;
    for (int i = 0; i < g_nlay && o < (int)sizeof(buf) - 72; i++)
        o += ksnprintf(buf + o, sizeof(buf) - (size_t)o, "%s\t%d\t%d\n", g_lay[i].key, g_lay[i].col, g_lay[i].row);
    fs_mkdir_p("/home/banana/.config");
    fs_write_path(LAYOUT_FILE, buf, (uint32_t)o);
}

static void layout_set(const char* key, int col, int row) {
    for (int i = 0; i < g_nlay; i++)
        if (!strcmp(g_lay[i].key, key)) { g_lay[i].col = col; g_lay[i].row = row; return; }
    if (g_nlay < LAYOUT_MAX) {
        kstrlcpy(g_lay[g_nlay].key, key, sizeof(g_lay[0].key));
        g_lay[g_nlay].col = col;
        g_lay[g_nlay].row = row;
        g_nlay++;
    }
}

static int desk_rows(const fb_info_t* fi);
static int desk_cols(const fb_info_t* fi) {
    int c = ((int)fi->width - DESK_X) / CELL_W;
    return c < 1 ? 1 : c;
}

/* each item gets a cell: its saved one if free, else the next free one */
static void desk_place(void) {
    const fb_info_t* fi = fb_info();
    if (!fi || !fi->width) return;
    if (g_nlay < 0) layout_load();
    int rows = desk_rows(fi), cols = desk_cols(fi);
    static uint8_t used[64][32];
    memset(used, 0, sizeof(used));
    for (int i = 0; i < g_ndesk; i++) g_desk[i].col = -1;
    for (int i = 0; i < g_ndesk; i++) {
        char key[56];
        desk_key(&g_desk[i], key, sizeof(key));
        for (int k = 0; k < g_nlay; k++)
            if (!strcmp(g_lay[k].key, key) && g_lay[k].col < cols && g_lay[k].row < rows &&
                g_lay[k].col < 64 && g_lay[k].row < 32 && !used[g_lay[k].col][g_lay[k].row]) {
                g_desk[i].col = g_lay[k].col;
                g_desk[i].row = g_lay[k].row;
                used[g_desk[i].col][g_desk[i].row] = 1;
                break;
            }
    }
    int c = 0, r = 0;
    for (int i = 0; i < g_ndesk; i++) {
        if (g_desk[i].col >= 0) continue;
        while (c < cols && c < 64 && used[c][r]) { if (++r >= rows || r >= 32) { r = 0; c++; } }
        if (c >= cols || c >= 64) { g_desk[i].col = cols - 1; g_desk[i].row = rows - 1; continue; }
        g_desk[i].col = c;
        g_desk[i].row = r;
        used[c][r] = 1;
    }
}

/* the built-in programs, then the installed apps (looked at again every
 * 2 s: an app installed or removed shows up by itself) */
static pkg_info_t g_pkgs[DESK_MAX];
static void desk_refresh(int force) {
    if (!force && g_ndesk && timer_ms() - g_desk_checked < 2000) return;
    g_desk_checked = timer_ms();
    g_ndesk = 0;
    desk_add(ACT_TERMINAL, "Terminal", NULL);
    desk_add(ACT_FILES, "Files", NULL);
    desk_add(ACT_BROWSER, "Browser", NULL);
    desk_add(ACT_NOTEPAD, "Notepad", NULL);
    desk_add(ACT_APPS, "Apps", NULL);
    desk_add(ACT_TASKMGR, "Task Manager", NULL);
    desk_add(ACT_SETTINGS, "Settings", NULL);
    if (installer_available()) desk_add(ACT_INSTALL, "Install Banana OS", NULL);
    int n = pkg_list(g_pkgs, DESK_MAX);
    for (int i = 0; i < n && i < DESK_MAX; i++)
        desk_add(ACT_RUN_APP, g_pkgs[i].title[0] ? g_pkgs[i].title : g_pkgs[i].name, g_pkgs[i].name);
    desk_add_path(DK_TRASH, "Recycle Bin", TRASH_FILES);
    /* the Desktop folder: its folders, then its files */
    if (fs_find_dir(DESKTOP_DIR) < 0) fs_mkdir_p(DESKTOP_DIR);
    static int di[DESK_MAX], fi2[DESK_MAX];
    int nd = fs_list_dirs(DESKTOP_DIR, di, DESK_MAX), nf = fs_list_files(DESKTOP_DIR, fi2, DESK_MAX);
    for (int i = 0; i < nd && i < DESK_MAX; i++) {
        const fs_dir_t* d = fs_get_dir(di[i]);
        if (!d || d->name[0] == '.') continue;
        char p[FS_PATH_LEN];
        ksnprintf(p, sizeof(p), "%s/%s", DESKTOP_DIR, d->name);
        desk_add_path(DK_DIR, d->name, p);
    }
    for (int i = 0; i < nf && i < DESK_MAX; i++) {
        fs_file_t* f = fs_file_info(fi2[i]);
        if (!f || f->name[0] == '.') continue;
        char p[FS_PATH_LEN];
        ksnprintf(p, sizeof(p), "%s/%s", DESKTOP_DIR, f->name);
        desk_add_path(DK_FILE, f->name, p);
    }
    desk_place();
    uint32_t sig = (uint32_t)g_ndesk ^ (uint32_t)(trash_count() > 0) << 30;
    for (int i = 0; i < g_ndesk; i++) {
        for (const char* p = g_desk[i].label; *p; p++) sig = sig * 31u + (uint8_t)*p;
        sig = sig * 31u + (uint32_t)(g_desk[i].col * 64 + g_desk[i].row);
    }
    if (sig != g_desk_sig) { g_desk_sig = sig; if (g_desk_sel >= g_ndesk) g_desk_sel = -1; g_desk_marks_clear(); g_force_redraw = 1; }
}

static int desk_rows(const fb_info_t* fi) {
    int r = ((int)fi->height - BAR_H - DESK_Y) / CELL_H;
    return r < 1 ? 1 : r;
}
static void desk_cell(const fb_info_t* fi, int i, int* x, int* y) {
    (void)fi;
    *x = DESK_X + g_desk[i].col * CELL_W;
    *y = DESK_Y + g_desk[i].row * CELL_H;
}

static void desk_icon(int act, int x, int y, const char* app) {
    switch (act) {
    case ACT_TERMINAL: draw_icon_terminal(x, y, 0); break;
    case ACT_FILES:    draw_icon_files(x, y, 0); break;
    case ACT_BROWSER:  draw_icon_browser(x, y, 0); break;
    case ACT_NOTEPAD:  draw_icon_notepad(x, y, 0); break;
    case ACT_APPS:     draw_icon_apps(x, y, 0); break;
    case ACT_TASKMGR:  draw_icon_taskmgr(x, y, 0); break;
    case ACT_SETTINGS: draw_icon_settings(x, y, 0x00303A48u); break;
    case ACT_INSTALL:  draw_icon_install(x, y, 0); break;
    default:           g_app_icon_name = app; draw_icon_app(x, y, 0); break;
    }
}

static void draw_desktop_icons(const fb_info_t* fi) {
    g_is = 2;                                   /* 28 x 28 */
    for (int i = 0; i < g_ndesk; i++) {
        int x, y;
        desk_cell(fi, i, &x, &y);
        if (x + CELL_W > (int)fi->width) continue;
        int sel = desk_marked(i) || i == g_desk_drop;
        if (sel) gfx_fill_rect(x + (CELL_W - 36) / 2, y + 2, 36, 34, 0x00315A9Cu);   /* selected: tinted */
        if (g_desk[i].kind == DK_APP) desk_icon(g_desk[i].act, x + (CELL_W - 28) / 2, y + 5, g_desk[i].app);
        else {
            fileicon_t k = g_desk[i].kind == DK_TRASH ? (trash_count() > 0 ? FI_TRASH_FULL : FI_TRASH) :
                           g_desk[i].kind == DK_DIR ? FI_FOLDER : fileicon_for_name(g_desk[i].label);
            fileicon_draw(k, x + (CELL_W - 32) / 2, y + 3, 32);
        }
        gfx_draw_label(x + CELL_W / 2, y + 40, CELL_W - 6, g_desk[i].label, 0x00FFFFFFu, sel ? 0x00315A9Cu : 0);
    }
    g_is = 1;
}

/* the icon under (mx, my), or -1 */
static int icon_at(int mx, int my) {
    const fb_info_t* fi = fb_info();
    if (!fi) return -1;
    for (int i = 0; i < g_ndesk; i++) {
        int x, y;
        desk_cell(fi, i, &x, &y);
        if (mx >= x + 4 && mx < x + CELL_W - 4 && my >= y + 2 && my < y + CELL_H - 4) return i;
    }
    return -1;
}

/* ── the Start menu's submenus (drawn by the right-click menu code) ──── */

static void do_action(int act);
static void run_app(const char* name);
static char g_prog_names[CTX_MAX_ITEMS][PKG_NAME_MAX];

/* ── the Start menu (kernel/startmenu.c) asks for these ──────────── */

static void sm_action(int sm) {
    int act = -1;
    switch (sm) {
    case SM_TERMINAL: act = ACT_TERMINAL; break;
    case SM_FILES:    act = ACT_FILES; break;
    case SM_BROWSER:  act = ACT_BROWSER; break;
    case SM_NOTEPAD:  act = ACT_NOTEPAD; break;
    case SM_APPS:     act = ACT_APPS; break;
    case SM_TASKMGR:  act = ACT_TASKMGR; break;
    case SM_SETTINGS: act = ACT_SETTINGS; break;
    case SM_INSTALL:  act = ACT_INSTALL; break;
    case SM_SHUTDOWN: act = ACT_SHUTDOWN; break;
    case SM_RESTART:  act = ACT_RESTART; break;
    case SM_LOCK:     act = ACT_LOCK; break;
    case SM_QUIT:     act = ACT_QUIT; break;
    }
    g_menu_open = 0;
    g_force_redraw = 1;
    if (act >= 0) do_action(act);
}

static void sm_run_app(const char* name) { g_force_redraw = 1; run_app(name); }

/* a folder opens in Files; a file: its folder, with it selected */
static void sm_open_path(const char* path) {
    g_menu_open = 0;
    g_force_redraw = 1;
    if (strcmp(path, "/") == 0 || fs_find_dir(path) >= 0) {
        explorer_open(path);
    } else {
        char dir[FS_PATH_LEN];
        kstrlcpy(dir, path, sizeof(dir));
        char* sl = strrchr(dir, '/');
        if (!sl) return;
        if (sl == dir) sl[1] = 0; else *sl = 0;
        explorer_open(dir);
        explorer_select(strrchr(path, '/') + 1);
    }
    g_app_min[APP_FILES] = 0;
    raise_app(APP_FILES);
}

static void sm_close(void) { g_menu_open = 0; g_force_redraw = 1; }

static void sm_program_icon(int sm, const char* app, int x, int y, int big, uint32_t bg) {
    g_is = big ? 2 : 1;
    switch (sm) {
    case SM_TERMINAL: draw_icon_terminal(x, y, bg); break;
    case SM_FILES:    draw_icon_files(x, y, bg); break;
    case SM_BROWSER:  draw_icon_browser(x, y, bg); break;
    case SM_NOTEPAD:  draw_icon_notepad(x, y, bg); break;
    case SM_APPS:     draw_icon_apps(x, y, bg); break;
    case SM_TASKMGR:  draw_icon_taskmgr(x, y, bg); break;
    case SM_SETTINGS: draw_icon_settings(x, y, bg); break;
    case SM_INSTALL:  draw_icon_install(x, y, bg); break;
    default:          g_app_icon_name = app ? app : ""; draw_icon_app(x, y, bg); break;
    }
    g_is = 1;
}

/* the Start menu opens (graphics mode) */
static void start_open(void) {
    static int ready;
    if (!ready) {
        startmenu_host_t h = { sm_action, sm_run_app, sm_open_path, sm_close, sm_program_icon, installer_available };
        startmenu_init(&h);
        ready = 1;
    }
    ctxmenu_close();
    g_sub_slot = -1;
    startmenu_reset();
    g_menu_open = 1;
    g_force_redraw = 1;
}

static void programs_cb(int id, void* arg) {
    (void)arg;
    g_sub_slot = -1;
    g_menu_open = 0;
    if (id == 1000) { do_action(ACT_APPS); return; }
    if (id >= 0 && id < CTX_MAX_ITEMS) run_app(g_prog_names[id]);
}
static void power_cb(int id, void* arg) { (void)arg; g_sub_slot = -1; do_action(id); }

static void open_submenu(const fb_info_t* fi, int slot) {
    ctx_item_t items[CTX_MAX_ITEMS];
    int n = 0;
    int x = START_X + START_MENU_W - 6, y = start_menu_y(fi) + 6 + slot * 28;
    if (MENU_ACTS[menu_idx(slot)] == ACT_PROGRAMS) {
        int k = pkg_list(g_pkgs, DESK_MAX);
        for (int i = 0; i < k && n < CTX_MAX_ITEMS - 2; i++) {
            kstrlcpy(g_prog_names[n], g_pkgs[i].name, PKG_NAME_MAX);
            items[n].label = g_pkgs[i].title[0] ? g_pkgs[i].title : g_pkgs[i].name;
            items[n].id = n;
            items[n].disabled = 0;
            n++;
        }
        if (!n) items[n++] = (ctx_item_t){ "(no apps installed)", -1, 1 };
        items[n++] = (ctx_item_t){ CTX_SEP, 0, 0 };
        items[n++] = (ctx_item_t){ "All apps...", 1000, 0 };
        ctxmenu_open(x, y, items, n, programs_cb, NULL);
    } else {
        items[n++] = (ctx_item_t){ "Restart", ACT_RESTART, 0 };
        items[n++] = (ctx_item_t){ "Shut down", ACT_SHUTDOWN, 0 };
        items[n++] = (ctx_item_t){ CTX_SEP, 0, 0 };
        items[n++] = (ctx_item_t){ "Lock the screen", ACT_LOCK, 0 };
        items[n++] = (ctx_item_t){ "Exit to the shell", ACT_QUIT, 0 };
        ctxmenu_open(x, y, items, n, power_cb, NULL);
    }
    g_sub_slot = slot;
}

/* ── function and media keys (keyboard_take_fkey) ──────────────────── */
static uint32_t g_osd_until;                /* the volume level shows until then */
static int      g_osd_shown;

/* the terminal window in front, -1 if none is open */
static int front_term(void) {
    for (int oi = TERM_WIN_MAX - 1; oi >= 0; oi--) {
        int i = g_term_order[oi];
        if (g_terms[i].open && !g_terms[i].minimized) return i;
    }
    return -1;
}

/* Alt+F4: the window in front closes; with none, the Shut down menu opens */
static void close_front_window(const fb_info_t* fi) {
    if (ctxmenu_is_open()) {                    /* a menu: it closes (the Start menu with its submenu) */
        if (g_sub_slot >= 0) g_menu_open = 0;
        ctxmenu_close();
        g_sub_slot = -1;
        g_force_redraw = 1;
        return;
    }
    if (g_menu_open) { g_menu_open = 0; g_force_redraw = 1; return; }
    if (g_front_app >= 0 && app_visible(g_front_app)) {
        if (g_front_app == APP_APPWIN) { int id = appwin_front_id(); if (id >= 0) appwin_request_close(id); }
        else win_close(WK_APP << 8 | g_front_app);
        return;
    }
    int t = front_term();
    if (t >= 0) { g_terms[t].open = 0; g_terms[t].minimized = 0; g_tb_gen++; return; }
    (void)fi;
    start_open();
}

static void toggle_max_term(const fb_info_t* fi, term_win_t* w) {
    if (!w->maxed) {
        w->sx = w->x; w->sy = w->y; w->sw = w->w; w->sh = w->h;
        w->x = 0; w->y = 0; w->w = (int)fi->width; w->h = (int)fi->height - BAR_H;
        w->maxed = 1;
    } else {
        w->x = w->sx; w->y = w->sy; w->w = w->sw; w->h = w->sh;
        w->maxed = 0;
    }
    clamp_win(fi, w);
    term_apply_size(w);
}

/* play / pause, stop, next, previous: the music player has them first (1 if taken) */
static int gui_media_key(int c) {
    return appwin_media_key(c);
}

/* the desktop's mouse, drag and drop (defined at the end of this file) */
static int  g_dpress = -1, g_dpress_x, g_dpress_y, g_dpress_collapse;
static int  g_dband, g_dband_x0, g_dband_y0, g_dband_x1, g_dband_y1;
static int  desk_nmarked(void);
static void desk_mouse(const fb_info_t* fi, int mx, int my, int left);
static void dnd_draw(const fb_info_t* fi, int mx, int my);

/* the keyboard everywhere (defined at the end of this file) */
static int  kb_global(int k);                 /* Alt+Tab, Win+..., the Menu key: 1 if taken */
static void kb_draw_overlays(const fb_info_t* fi);
static int  kb_modal(void);                   /* the switcher, task view, taskbar focus or a menu has the keys */
static int  kb_modal_key(int code);
static int  g_sw_open, g_sw_sel;              /* Alt+Tab: the switcher */
static int  g_tv_open, g_tv_sel;              /* Win+Tab: task view */
static int  g_bar_kb, g_bar_sel = -1;         /* Win+T: the taskbar has the keys */
static int  tv_card_at(const fb_info_t* fi, int mx, int my);
static void tv_activate(int sel);

static void gui_fkeys(const fb_info_t* fi) {
    if (g_osd_shown && (int32_t)(timer_ms() - g_osd_until) >= 0) { g_osd_shown = 0; g_force_redraw = 1; }
    for (int n = 0; n < 16; n++) {
        /* a console app in the front terminal takes them itself (not the desktop's own shortcuts) */
        if (g_front_app < 0 && front_term() >= 0 && app_console_focused()) {
            int k = keyboard_take_fkey();
            if (!k) return;
            if (!kb_global(k)) return;
            continue;
        }
        int k = keyboard_take_fkey();
        if (!k) return;
        if (kb_global(k)) { g_force_redraw = 1; continue; }
        int c = KEYF_CODE(k);
        if (c == KEYF_WIN) { gui_handle_key(20); g_force_redraw = 1; continue; }
        if (c == KEYF_MUTE || c == KEYF_VOLDOWN || c == KEYF_VOLUP) {   /* (the keyboard driver set it) */
            g_osd_until = timer_ms() + 1500;
            g_osd_shown = 1;
            g_force_redraw = 1;
            continue;
        }
        if (c == KEYF_F1 + 3 && (k & KEYF_ALT)) { close_front_window(fi); continue; }   /* Alt+F4 */
        if (c >= KEYF_PLAY && c <= KEYF_PREV) {
            if (gui_media_key(c)) continue;          /* the music player, wherever it is */
        }
        if (g_menu_open || ctxmenu_is_open()) continue;
        if (g_front_app >= 0 && app_visible(g_front_app)) {
            switch (g_front_app) {
            case APP_FILES:   explorer_fkey(k); break;
            case APP_BROWSER: browser_fkey(k); break;
            case APP_NOTEPAD: notepad_fkey(k); break;
            case APP_APPWIN:  appwin_fkey(k); break;
            default: break;
            }
        } else if (c == KEYF_F1 + 10) {             /* F11: the front terminal fills the screen */
            int t = front_term();
            if (t >= 0) toggle_max_term(fi, &g_terms[t]);
        }
        g_force_redraw = 1;
    }
}

static void draw_volume_osd(const fb_info_t* fi) {
    if (!g_osd_shown) return;
    int w = 220, h = 54, x = ((int)fi->width - w) / 2, y = (int)fi->height - BAR_H - h - 40;
    int v = audio_get_volume();
    draw_bevel_box(x, y, w, h, 0x001D232Cu, 0x00505E74u, 0x0010151Du);
    char t[32];
    if (v == 0) kstrlcpy(t, "Sound off", sizeof(t));
    else ksnprintf(t, sizeof(t), "Volume %d%%", v);
    gfx_draw_text(x + 14, y + 10, t, 0x00E8EEF6u, 0x001D232Cu);
    gfx_fill_rect(x + 14, y + 30, w - 28, 10, 0x00303A4Au);
    gfx_fill_rect(x + 14, y + 30, (w - 28) * v / 100, 10, 0x0068A8F0u);
}

static void render_desktop(const fb_info_t* fi, int mx, int my) {
    refresh_wallpaper_cache(fi);
    blit_wallpaper_cache();

    draw_desktop_icons(fi);

    /* windows back to front: the app windows behind the terminals, the
     * terminals, then the app window in front of them (if any) */
    for (int i = 0; i < APP_COUNT; i++)
        if (g_app_order[i] != g_front_app && app_visible(g_app_order[i])) g_apps[g_app_order[i]].draw(fi);
    for (int oi = 0; oi < TERM_WIN_MAX; oi++) {
        term_win_t* w = &g_terms[g_term_order[oi]];
        draw_terminal_window(fi, w);
    }
    if (g_front_app >= 0 && app_visible(g_front_app)) g_apps[g_front_app].draw(fi);

    draw_taskbar_fb(fi);

    /* the Start menu */
    if (g_menu_open) startmenu_draw(fi);

    draw_volume_osd(fi);
    dnd_draw(fi, mx, my);
    kb_draw_overlays(fi);
    ctxmenu_draw();

    /* push backbuffer to framebuffer once per frame, then the cursor */
    fb_present();
    draw_cursor(mx, my);
}

/* ── right-click menus ─────────────────────────────────────────────── */

static void do_action(int act);

enum { DM_NEWFOLDER = 100, DM_NEWFILE, DM_PASTE, DM_OPENDIR, DM_REFRESH, DM_ARRANGE };
static void desktop_menu_cb(int id, void* arg) {
    (void)arg;
    char msg[112];
    switch (id) {
    case DM_NEWFOLDER: desk_new(1); break;
    case DM_NEWFILE: desk_new(0); break;
    case DM_PASTE: fileops_paste(DESKTOP_DIR, msg, sizeof(msg)); desk_refresh(1); explorer_refresh(); break;
    case DM_OPENDIR: explorer_open(DESKTOP_DIR); gui_raise_files(); break;
    case DM_REFRESH: desk_refresh(1); break;
    case DM_ARRANGE: g_nlay = 0; layout_save(); desk_refresh(1); break;
    default: do_action(id); break;
    }
}

static void open_desktop_menu(int mx, int my) {
    ctx_item_t items[] = {
        { "New folder", DM_NEWFOLDER, 0 },
        { "New text document", DM_NEWFILE, 0 },
        { "Paste", DM_PASTE, fileops_clip_count() == 0 },
        { CTX_SEP, 0, 0 },
        { "Arrange icons", DM_ARRANGE, 0 },
        { "Refresh", DM_REFRESH, 0 },
        { "Open the Desktop folder", DM_OPENDIR, 0 },
        { CTX_SEP, 0, 0 },
        { "Terminal", ACT_TERMINAL, 0 },
        { "Files", ACT_FILES, 0 },
        { "Task Manager", ACT_TASKMGR, 0 },
        { "Change wallpaper...", ACT_WALLPAPER, 0 },
        { "About Banana OS", ACT_ABOUT, 0 },
        { CTX_SEP, 0, 0 },
        { "Lock screen", ACT_LOCK, 0 },
        { "Exit to shell", ACT_QUIT, 0 },
    };
    ctxmenu_open(mx, my, items, (int)(sizeof(items) / sizeof(items[0])), desktop_menu_cb, NULL);
}

/* a desktop icon: open it (an installed app: also remove it) */
static int g_icon_menu_i;
enum { ICM_OPEN = 1, ICM_REMOVE };
enum { ICM_PIN = 10, ICM_UNPIN, ICM_SHOW, ICM_COPY, ICM_CUT, ICM_DELETE, ICM_RENAME, ICM_EMPTY };
static void icon_menu_cb(int id, void* arg) {
    (void)arg;
    int i = g_icon_menu_i;
    if (i < 0 || i >= g_ndesk) return;
    desk_item_t* d = &g_desk[i];
    if (id == ICM_OPEN) desk_open(i);
    else if (id == ICM_PIN) pin_add(d->act, d->act == ACT_RUN_APP ? d->app : "");
    else if (id == ICM_UNPIN) { int p = pin_find(d->act, d->app); if (p >= 0) pin_remove(p); }
    else if (id == ICM_SHOW) { explorer_open(DESKTOP_DIR); explorer_select(d->label); gui_raise_files(); }
    else if (id == ICM_COPY || id == ICM_CUT) desk_clip_marked(id == ICM_CUT);
    else if (id == ICM_DELETE) desk_trash_marked();
    else if (id == ICM_RENAME) { explorer_open(DESKTOP_DIR); explorer_select(d->label); gui_raise_files(); explorer_start_rename(); }
    else if (id == ICM_EMPTY) { trash_empty(); explorer_refresh(); desk_refresh(1); }
    else if (id == ICM_REMOVE && d->act == ACT_RUN_APP) {
        char msg[96];
        pkg_remove(g_desk[i].app, msg, sizeof(msg));
        g_desk_sel = -1;
        desk_refresh(1);
    }
}

static void open_icon_menu(int i, int mx, int my) {
    char open[64];
    ksnprintf(open, sizeof(open), "Open %s", g_desk[i].label);
    ctx_item_t items[10];
    int n = 0;
    g_icon_menu_i = i;
    if (!desk_marked(i)) { g_desk_marks_clear(); g_desk_sel = i; }
    desk_item_t* d = &g_desk[i];
    if (d->kind == DK_APP) {
        items[n++] = (ctx_item_t){ open, ICM_OPEN, 0 };
        items[n++] = (ctx_item_t){ CTX_SEP, 0, 0 };
        if (d->act != ACT_INSTALL) {
            int pinned = pin_find(d->act, d->app) >= 0;
            items[n++] = (ctx_item_t){ pinned ? "Unpin from taskbar" : "Pin to taskbar", pinned ? ICM_UNPIN : ICM_PIN, 0 };
        }
        if (d->act == ACT_RUN_APP) items[n++] = (ctx_item_t){ "Remove this app", ICM_REMOVE, 0 };
    } else if (d->kind == DK_TRASH) {
        items[n++] = (ctx_item_t){ "Open", ICM_OPEN, 0 };
        items[n++] = (ctx_item_t){ CTX_SEP, 0, 0 };
        items[n++] = (ctx_item_t){ "Empty Recycle Bin", ICM_EMPTY, trash_count() == 0 };
    } else {
        items[n++] = (ctx_item_t){ "Open", ICM_OPEN, 0 };
        items[n++] = (ctx_item_t){ "Show in Files", ICM_SHOW, 0 };
        items[n++] = (ctx_item_t){ CTX_SEP, 0, 0 };
        items[n++] = (ctx_item_t){ "Cut", ICM_CUT, 0 };
        items[n++] = (ctx_item_t){ "Copy", ICM_COPY, 0 };
        items[n++] = (ctx_item_t){ CTX_SEP, 0, 0 };
        items[n++] = (ctx_item_t){ "Delete", ICM_DELETE, 0 };
        items[n++] = (ctx_item_t){ "Rename", ICM_RENAME, 0 };
    }
    if (n && !strcmp(items[n - 1].label, CTX_SEP)) n--;
    ctxmenu_open(mx, my, items, n, icon_menu_cb, NULL);
}

/* taskbar: Task Manager & co, or one window's button */
enum { TBM_TASKMGR = 1, TBM_SHOW_DESKTOP, TBM_RESTORE_ALL, TBM_RESTORE, TBM_MINIMIZE, TBM_CLOSE, TBM_QUIT, TBM_TERMINAL, TBM_LOCK,
       TBM_PIN, TBM_UNPIN, TBM_PIN_OPEN, TBM_SETTINGS };
static int g_tbmenu_win, g_tbmenu_pin = -1;
static int  win_pin_target(int h, int* act, char* app, int cap);
static int  pin_at(int mx, int my);
static void pin_open(int i);

static void taskbar_menu_cb(int id, void* arg) {
    (void)arg;
    switch (id) {
    case TBM_TASKMGR: do_action(ACT_TASKMGR); break;
    case TBM_SHOW_DESKTOP: for (int j = 0; j < g_ntb; j++) win_minimize(g_tb[j]); g_front_app = -1; break;
    case TBM_RESTORE_ALL: for (int j = 0; j < g_ntb; j++) if (win_minimized(g_tb[j])) win_activate(g_tb[j]); break;
    case TBM_RESTORE: if (win_exists(g_tbmenu_win)) win_activate(g_tbmenu_win); break;
    case TBM_MINIMIZE: if (win_exists(g_tbmenu_win)) win_minimize(g_tbmenu_win); break;
    case TBM_CLOSE: if (win_exists(g_tbmenu_win)) win_close(g_tbmenu_win); break;
    case TBM_TERMINAL: do_action(ACT_TERMINAL); break;
    case TBM_QUIT: do_action(ACT_QUIT); break;
    case TBM_LOCK: do_action(ACT_LOCK); break;
    case TBM_PIN: {
        int act;
        char app[PKG_NAME_MAX];
        if (win_exists(g_tbmenu_win) && win_pin_target(g_tbmenu_win, &act, app, sizeof(app))) pin_add(act, app);
        break;
    }
    case TBM_UNPIN: if (g_tbmenu_pin >= 0) pin_remove(g_tbmenu_pin); break;
    case TBM_PIN_OPEN: if (g_tbmenu_pin >= 0) pin_open(g_tbmenu_pin); break;
    case TBM_SETTINGS: settings_open_page(SETTINGS_PAGE_TASKBAR); g_app_min[APP_SETTINGS] = 0; raise_app(APP_SETTINGS); break;
    }
}

static void open_taskbar_menu(const fb_info_t* fi, int mx, int my) {
    int p = pin_at(mx, my);
    if (p >= 0) {
        g_tbmenu_pin = p;
        ctx_item_t items[] = {
            { "Open", TBM_PIN_OPEN, 0 },
            { CTX_SEP, 0, 0 },
            { "Unpin from taskbar", TBM_UNPIN, 0 },
        };
        ctxmenu_open(mx, my, items, 3, taskbar_menu_cb, NULL);
        return;
    }
    int j = tb_button_at(fi, mx, my);
    if (j >= 0) {
        g_tbmenu_win = g_tb[j];
        int min = win_minimized(g_tbmenu_win);
        int act;
        char app[PKG_NAME_MAX];
        int can = win_pin_target(g_tbmenu_win, &act, app, sizeof(app));
        int pinned = can && pin_find(act, app) >= 0;
        g_tbmenu_pin = pinned ? pin_find(act, app) : -1;
        ctx_item_t items[] = {
            { "Restore", TBM_RESTORE, !min && win_front() == g_tbmenu_win },
            { "Minimize", TBM_MINIMIZE, min },
            { CTX_SEP, 0, 0 },
            { pinned ? "Unpin from taskbar" : "Pin to taskbar", pinned ? TBM_UNPIN : TBM_PIN, !can },
            { CTX_SEP, 0, 0 },
            { "Close window", TBM_CLOSE, 0 },
            { CTX_SEP, 0, 0 },
            { "Task Manager", TBM_TASKMGR, 0 },
        };
        ctxmenu_open(mx, my, items, 8, taskbar_menu_cb, NULL);
        return;
    }
    ctx_item_t items[] = {
        { "Task Manager", TBM_TASKMGR, 0 },
        { CTX_SEP, 0, 0 },
        { "Show the desktop", TBM_SHOW_DESKTOP, g_ntb == 0 },
        { "Restore all windows", TBM_RESTORE_ALL, g_ntb == 0 },
        { CTX_SEP, 0, 0 },
        { "New terminal", TBM_TERMINAL, 0 },
        { "Taskbar settings", TBM_SETTINGS, 0 },
        { "Lock screen", TBM_LOCK, 0 },
        { "Exit to shell", TBM_QUIT, 0 },
    };
    ctxmenu_open(mx, my, items, (int)(sizeof(items) / sizeof(items[0])), taskbar_menu_cb, NULL);
}

/* a terminal window */
enum { TM_COPY = 1, TM_PASTE, TM_CLEAR, TM_MINIMIZE, TM_CLOSE, TM_NEW };
static int g_tmenu_term;

static void term_menu_cb(int id, void* arg) {
    (void)arg;
    term_win_t* w = &g_terms[g_tmenu_term];
    if (!w->open) return;
    switch (id) {
    case TM_COPY: if (w->sel) term_copy_selection(w); break;
    case TM_PASTE: bring_term_front(g_tmenu_term); g_front_app = -1; keyboard_inject("\x16"); break;
    case TM_CLEAR: bring_term_front(g_tmenu_term); g_front_app = -1; keyboard_inject("clear\n"); break;
    case TM_MINIMIZE: win_minimize(WK_TERM << 8 | g_tmenu_term); break;
    case TM_CLOSE: w->open = 0; g_tb_gen++; break;
    case TM_NEW: do_action(ACT_TERMINAL); break;
    }
}

static void open_term_menu(int wi, int mx, int my) {
    g_tmenu_term = wi;
    uint32_t n;
    clipboard_get(&n);
    ctx_item_t items[] = {
        { "Copy", TM_COPY, !g_terms[wi].sel },
        { "Paste", TM_PASTE, n == 0 },
        { "Clear", TM_CLEAR, 0 },
        { CTX_SEP, 0, 0 },
        { "New terminal", TM_NEW, 0 },
        { "Minimize", TM_MINIMIZE, 0 },
        { "Close", TM_CLOSE, 0 },
    };
    ctxmenu_open(mx, my, items, 7, term_menu_cb, NULL);
}

/* Notepad: its own Ctrl keys */
static void notepad_menu_cb(int id, void* arg) { (void)arg; notepad_key((char)id); }

static void notepad_rclick_menu(int mx, int my) {
    uint32_t n;
    clipboard_get(&n);
    ctx_item_t items[] = {
        { "Cut", 24, 0 },
        { "Copy", 3, 0 },
        { "Paste", 22, n == 0 },
        { "Select all", 1, 0 },
        { CTX_SEP, 0, 0 },
        { "Find...", 6, 0 },
        { "Open...", 15, 0 },
        { "Save", 19, 0 },
    };
    ctxmenu_open(mx, my, items, 8, notepad_menu_cb, NULL);
}

static void explorer_rclick_menu(int mx, int my) {
    explorer_rclick(mx, my);
}

/* ── the desktop loop ───────────────────────────────────────────────── */

int gui_appwin_focused(void) {
    return gfx_available() && g_gui_enabled && g_front_app == APP_APPWIN && appwin_any_visible() && tty_current() < 0;
}

static void gui_poll_body(void);

/* While a task is in here (drawing the desktop), the timer interrupt
 * leaves the pointer alone (gui_cursor_tick); not while it waits in
 * task_yield. A counter: tasks can be in here at the same time. */
void gui_poll(void) {
    g_gui_busy++;
    gui_poll_body();
    g_gui_busy--;
}

static void gui_poll_body(void) {
    /* the lock screen: while it is up, every other caller (the terminal
     * windows' shells poll too) leaves the screen and the keyboard to it */
    if (g_in_lock) return;
    /* (background tasks - the servers, app windows - call this too, but read no keys) */
    if (g_lock_pending && g_gui_enabled && !task_is_background()) {
        g_lock_pending = 0;
        g_in_lock = 1;
        g_menu_open = 0;
        login_lock(&g_cur_mx, &g_cur_my);
        g_mouse_from_lock = 1;
        g_ptr_drawn_x = -1;              /* (the lock screen was on the screen) */
        g_in_lock = 0;
        keyboard_ctrl_alt_del_pending();   /* (a press while locked does nothing) */
        gui_screen_changed();              /* the desktop repaints everything */
    }
    /* every idle/wait loop passes through here: paint pending console output */
    terminal_flush();
    timer_poll();
    g_gui_busy--;
    task_yield();
    g_gui_busy++;
    /* another task may have locked the screen while this one waited here:
     * no frame, no mouse, no keys from this caller now */
    if (g_in_lock) return;

    /* Consume the Ctrl+Alt+Delete flag every cycle regardless of GUI
     * state, so a press while the GUI is off can't linger and fire the
     * next time it's started - only actually close it when running. */
    if (keyboard_ctrl_alt_del_pending() && g_gui_enabled) {
        gui_set_enabled(0);
    }

    if (gfx_available()) {
        if (!g_gui_enabled) return;
        /* framebuffer desktop loop (backbuffer to avoid flicker) */
        const fb_info_t* fi = fb_info();
        if (!fi || fi->width == 0 || fi->height == 0) return;

        int mx, my;
        static uint32_t last_frame_ms = 0;
        static int prev_left = 0;

        sync_taskbar();
        if (g_front_app >= 0 && !app_visible(g_front_app)) g_front_app = -1;
        if (appwin_take_new()) raise_app(APP_APPWIN);   /* a new app window comes up in front */
        appwin_focus(g_front_app == APP_APPWIN);

        /* Notepad and app windows have no keyboard task of their own:
         * while one is in front, keys are handed to it from here (Ctrl+T
         * and the Start menu first). Apps and Task Manager read no keys
         * (Esc closes them). */
        if (gui_notepad_focused() || gui_appwin_focused() || (kb_modal() && g_front_app >= 0) ||
            ((g_front_app == APP_LAUNCHER || g_front_app == APP_TASKMGR || g_front_app == APP_FILES || g_front_app == APP_SETTINGS || g_front_app == APP_INSTALLER) &&
             app_visible(g_front_app) && tty_current() < 0)) {
            for (int k = 0; k < 64; k++) {
                char c = keyboard_try_getchar();
                if (!c) break;
                /* the switcher, task view, taskbar focus, a right-click menu: theirs */
                if (kb_modal() && !g_menu_open) {
                    int code = (unsigned char)c;
                    if (c == 27) {
                        char c2 = keyboard_try_getchar();
                        if (c2 == '[') {
                            char c3 = keyboard_try_getchar();
                            if (c3 >= '0' && c3 <= '9') keyboard_try_getchar();
                            code = c3 == 'A' ? KB_UP : c3 == 'B' ? KB_DOWN : c3 == 'C' ? KB_RIGHT : c3 == 'D' ? KB_LEFT :
                                   c3 == 'H' ? KB_HOME : c3 == 'F' ? KB_END : 0;
                        } else code = KB_ESC;
                    } else if (c == '\n') code = KB_ENTER;
                    else if (c == '\t') code = (keyboard_mods() & 1) ? KB_BACKTAB : KB_TAB;
                    else if (c == ' ') code = KB_SPACE;
                    if (code) kb_modal_key(code);
                    g_force_redraw = 1;
                    continue;
                }
                /* Settings, Task Manager, Apps: the focus moves between their controls */
                if (!g_menu_open && (g_front_app == APP_SETTINGS || g_front_app == APP_TASKMGR || g_front_app == APP_LAUNCHER)) {
                    int code = 0;
                    if (c == 27) {
                        char c2 = keyboard_try_getchar();
                        if (c2 == '[') {
                            char c3 = keyboard_try_getchar();
                            if (c3 >= '0' && c3 <= '9') keyboard_try_getchar();
                            code = c3 == 'A' ? KB_UP : c3 == 'B' ? KB_DOWN : c3 == 'C' ? KB_RIGHT : c3 == 'D' ? KB_LEFT :
                                   c3 == 'H' ? KB_HOME : c3 == 'F' ? KB_END : c3 == 'I' ? KB_PGUP : c3 == 'G' ? KB_PGDN : c3 == 'P' ? KB_DEL : 0;
                            if (!code) continue;
                        } else {
                            code = KB_ESC;
                            if (c2) { /* (Esc, then a key typed fast: the key is lost - rare) */ }
                        }
                    } else if (c == '\t') code = (keyboard_mods() & 1) ? KB_BACKTAB : KB_TAB;
                    else if (c == '\n') code = KB_ENTER;
                    else if (c == ' ') code = KB_SPACE;
                    int took = 0;
                    if (code) {
                        if (g_front_app == APP_SETTINGS) took = settings_navkey(code);
                        else if (g_front_app == APP_TASKMGR) took = taskmgr_navkey(code);
                        else took = launcher_navkey(code);
                    }
                    if (!took) {
                        if (code == KB_ESC) c = 27;
                        if (g_front_app == APP_SETTINGS) settings_key(c);
                        else if (c == 27) g_apps[g_front_app].close();
                    }
                    g_force_redraw = 1;
                    continue;
                }
                if (g_menu_open && c == 27) {
                    char c2 = keyboard_try_getchar();
                    if (c2 == '[') {
                        char c3 = keyboard_try_getchar();
                        if (c3 >= '0' && c3 <= '9') keyboard_try_getchar();   /* '~' */
                        gui_handle_arrow(c3);
                    } else {
                        gui_handle_key(27);
                        if (c2) gui_handle_key(c2);
                    }
                    continue;
                }
                if (g_menu_open || c == 20) { gui_handle_key(c); continue; }
                if (g_front_app == APP_NOTEPAD) notepad_key(c);
                else if (g_front_app == APP_APPWIN) appwin_key(c);
                else if (g_front_app == APP_FILES) explorer_key(c);
                else if (g_front_app == APP_SETTINGS) settings_key(c);
                else if (g_front_app == APP_INSTALLER) installer_key(c);
                else if (c == 27) g_apps[g_front_app].close();
            }
        }
        gui_fkeys(fi);

        if (!g_backbuf_active) {
            size_desktop(fi);
            fb_set_backbuffer(g_desktop_backbuf, g_desk_w, g_desk_h);
            g_backbuf_active = 1;
            g_force_redraw = 1;
            /* the saved wallpaper (/etc/wallpaper) is applied on first use */
            wallpaper_load_config();
        }

        /* (the timer interrupt moves the pointer too: interrupts off while it changes here) */
        g_scr_w = (int)fi->width;
        g_scr_h = (int)fi->height;
        uintptr_t fl;
        __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
        mouse_state_t ms = mouse_read();
        g_mouse_from_lock = 0;               /* (the lock screen moved g_cur_mx/my itself) */
        g_cur_mx += ms.dx;
        g_cur_my -= ms.dy;
        if (g_cur_mx < 0) g_cur_mx = 0;
        if (g_cur_my < 0) g_cur_my = 0;
        if (g_cur_mx > (int)fi->width - 1) g_cur_mx = (int)fi->width - 1;
        if (g_cur_my > (int)fi->height - 1) g_cur_my = (int)fi->height - 1;
        mx = g_cur_mx;
        my = g_cur_my;
        if (fl & 0x200) __asm__ volatile("sti" ::: "memory");
        if (ms.dz && g_menu_open && startmenu_contains(fi, mx, my)) {
            startmenu_wheel(ms.dz);
        } else if (ms.dz) {
            /* the wheel scrolls the window under the mouse */
            int a = (g_front_app >= 0 && app_visible(g_front_app) && g_apps[g_front_app].contains(mx, my)) ? g_front_app : app_at(mx, my, 0);
            if (a >= 0 && g_app_wheel[a]) g_app_wheel[a](mx, my, ms.dz);
        }
        if (mx < 0) mx = 0;
        if (my < 0) my = 0;
        if (mx > (int)fi->width - 1) mx = (int)fi->width - 1;
        if (my > (int)fi->height - 1) my = (int)fi->height - 1;

        uint32_t sec = timer_ticks() / 100u;
        int bar_y = (int)fi->height - BAR_H;

        /* mouse click handling (rising edge) */
        int left = ms.btn_left ? 1 : 0;
        int click = (left && !prev_left);
        prev_left = left;
        static int prev_right = 0;
        int right = ms.btn_right ? 1 : 0;
        int rclick = right && !prev_right;
        prev_right = right;
        /* task view / switcher / taskbar focus: a click picks a window or leaves */
        if ((click || rclick) && (g_tv_open || g_sw_open || g_bar_kb)) {
            if (click && g_tv_open) { int i = tv_card_at(fi, mx, my); if (i >= 0) tv_activate(i); }
            g_tv_open = g_sw_open = g_bar_kb = 0;
            g_force_redraw = 1;
            click = rclick = 0;
        }

        ctxmenu_hover(mx, my);

        /* the Start menu follows the pointer, and its submenus open by
         * themselves under it - Windows 95 style */
        if (!ctxmenu_is_open()) g_sub_slot = -1;
        if (g_menu_open && !ctxmenu_contains(mx, my)) startmenu_hover(fi, mx, my);
        {
            int sh = my >= bar_y && mx >= START_X && mx < START_X + START_W;
            if (sh != g_start_hover) g_start_hover = sh;
        }

        if (rclick) {
            /* right-click: the menu of whatever is under the mouse */
            ctxmenu_close();
            g_menu_open = 0;
            int a = (g_front_app >= 0 && app_visible(g_front_app) && g_apps[g_front_app].contains(mx, my)) ? g_front_app : -1;
            int wi = a < 0 ? term_at(mx, my) : 0;
            if (a < 0 && !wi) a = app_at(mx, my, 0);
            if (my >= bar_y) {
                open_taskbar_menu(fi, mx, my);
            } else if (a >= 0) {
                if (a != g_front_app) raise_app(a);
                g_apps[a].rclick(mx, my);
            } else if (wi) {
                bring_term_front(wi - 1);
                g_front_app = -1;
                open_term_menu(wi - 1, mx, my);
            } else {
                int slot = icon_at(mx, my);
                if (slot >= 0) open_icon_menu(slot, mx, my);
                else open_desktop_menu(mx, my);
            }
        }

        if (click && ctxmenu_is_open()) {
            /* a Start submenu: a click beside it goes on to the Start menu (or closes it) */
            int beside = g_sub_slot >= 0 && !ctxmenu_contains(mx, my);
            if (beside) { ctxmenu_close(); g_sub_slot = -1; }
            else { ctxmenu_click(mx, my); click = 0; }   /* an item, or a click outside that just closes it */
            if (!g_gui_enabled) return;
        }

        if (click) {
            /* the taskbar: Start, a window's button */
            if (click && my >= bar_y) {
                if (mx >= START_X && mx < START_X + START_W) {
                    if (g_menu_open) g_menu_open = 0;
                    else start_open();
                } else if (pin_at(mx, my) >= 0) {
                    pin_open(pin_at(mx, my));
                    g_menu_open = 0;
                } else {
                    int j = tb_button_at(fi, mx, my);
                    if (j >= 0) {
                        int h = g_tb[j];
                        /* like Windows: restore, bring to front, or minimize the front one */
                        if (win_minimized(h)) win_activate(h);
                        else if (win_front() == h) win_minimize(h);
                        else win_activate(h);
                    }
                    g_menu_open = 0;
                }
                click = 0;
            }

            /* click in menu items (the open menu is above every window) */
            if (click && g_menu_open) {
                if (startmenu_contains(fi, mx, my)) { startmenu_click(fi, mx, my); click = 0; }
                if (!g_gui_enabled) return;
                if (click) g_menu_open = 0;   /* a click elsewhere closes the menu */
            }

            /* the app window in front of the terminals */
            if (click && g_front_app >= 0 && app_visible(g_front_app) && g_apps[g_front_app].contains(mx, my)) {
                g_apps[g_front_app].click(mx, my);
                if (winframe_take_minimize()) win_minimize(WK_APP << 8 | g_front_app);
                click = 0;
            }

            /* terminal windows hit testing (front-to-back) */
            for (int oi = TERM_WIN_MAX - 1; click && oi >= 0; oi--) {
                int wi = g_term_order[oi];
                term_win_t* w = &g_terms[wi];
                if (!w->open || w->minimized) continue;

                int title_h = 20;
                if (mx >= w->x && mx < w->x + w->w && my >= w->y && my < w->y + w->h) {
                    /* Bringing a window to front changes keyboard focus
                     * (gui_focused_vt() below), but must NOT retarget
                     * where terminal output is written - that's owned by
                     * this window's own shell task now, not by whichever
                     * window was last clicked. */
                    bring_term_front(wi);
                    g_front_app = -1;

                    int tbtn = win_button_hit(w->x + w->w, w->y + 4, 12, 1, mx, my);
                    if (tbtn == WIN_BTN_CLOSE) {
                        /* Hide only - the vt and its shell task are kept
                         * running so a later reopen picks up right where
                         * this session left off. */
                        w->open = 0;
                    } else if (tbtn == WIN_BTN_MIN) {
                        win_minimize(WK_TERM << 8 | wi);
                    } else if (my < w->y + title_h) {
                        uint32_t now = timer_ms();
                        if (tbtn == WIN_BTN_MAX || now - w->title_click_ms < 400) {   /* maximize / restore */
                            if (!w->maxed) {
                                w->sx = w->x; w->sy = w->y; w->sw = w->w; w->sh = w->h;
                                w->x = 0; w->y = 0; w->w = (int)fi->width; w->h = (int)fi->height - BAR_H;
                                w->maxed = 1;
                            } else {
                                w->x = w->sx; w->y = w->sy; w->w = w->sw; w->h = w->sh;
                                w->maxed = 0;
                            }
                            clamp_win(fi, w);
                            term_apply_size(w);
                            w->title_click_ms = 0;
                        } else {
                            w->title_click_ms = now;
                            w->dragging = 1;
                            w->drag_dx = mx - w->x;
                            w->drag_dy = my - w->y;
                        }
                    } else if (mx >= w->x + w->w - GRIP && my >= w->y + w->h - GRIP) {
                        w->resizing = 1;
                        w->drag_dx = w->x + w->w - mx;
                        w->drag_dy = w->y + w->h - my;
                        w->maxed = 0;
                    } else {
                        /* start a text selection (a plain click clears it) */
                        w->selecting = 1;
                        w->sel = 0;
                        term_cell_at(w, mx, my, &w->s_r0, &w->s_c0);
                        w->s_r1 = w->s_r0;
                        w->s_c1 = w->s_c0;
                    }
                    click = 0;
                    break;
                }
            }

            /* an app window behind the terminals: clicking raises it */
            if (click) {
                int a = app_at(mx, my, 0);
                if (a >= 0) {
                    raise_app(a);
                    g_apps[a].click(mx, my);
                    if (winframe_take_minimize()) win_minimize(WK_APP << 8 | a);
                    click = 0;
                }
            }

            /* the desktop's icons: a click selects, a double click opens */
            if (click && !g_menu_open) {
                int i = icon_at(mx, my);
                uint32_t now = timer_ms();
                int mods = keyboard_mods();
                if (i >= 0 && i == g_last_click_i && now - g_last_click_ms < 800 && !(mods & 3)) {
                    g_last_click_i = -1;
                    g_dpress = -1;
                    desk_open(i);
                } else {
                    g_last_click_i = i;
                    g_last_click_ms = now;
                    if (i < 0) {
                        /* the empty desktop: a selection box (Ctrl: added to the selection) */
                        if (!(mods & 2)) { g_desk_marks_clear(); g_desk_sel = -1; }
                        g_dband = 1;
                        g_dband_x0 = g_dband_x1 = mx;
                        g_dband_y0 = g_dband_y1 = my;
                    } else if (mods & 2) {
                        if (desk_marked(i)) { g_desk_mark[i] = 0; if (g_desk_sel == i) g_desk_sel = -1; }
                        else { if (g_desk_sel >= 0) g_desk_mark[g_desk_sel] = 1; g_desk_mark[i] = 1; g_desk_sel = i; }
                    } else {
                        g_dpress_collapse = desk_marked(i) && desk_nmarked() > 1;
                        if (!g_dpress_collapse) { g_desk_marks_clear(); g_desk_sel = i; }
                        g_dpress = i;
                        g_dpress_x = mx;
                        g_dpress_y = my;
                    }
                }
                if (!g_gui_enabled) return;
            }
        }

        /* a menu entry ("Exit to shell") may have closed the desktop */
        if (!g_gui_enabled) return;

        /* drag any terminal windows while holding left */
        for (int i = 0; i < TERM_WIN_MAX; i++) {
            if (!g_terms[i].open) continue;
            term_win_t* t = &g_terms[i];
            if (left && t->dragging) {
                t->x = mx - t->drag_dx;
                t->y = my - t->drag_dy;
                t->maxed = 0;
                clamp_win(fi, t);
            }
            if (left && t->resizing) {
                int nw = mx + t->drag_dx - t->x, nh = my + t->drag_dy - t->y;
                if (nw != t->w || nh != t->h) {
                    t->w = nw;
                    t->h = nh;
                    clamp_win(fi, t);
                    term_apply_size(t);
                }
            }
            if (left && t->selecting) {
                int r, c;
                term_cell_at(t, mx, my, &r, &c);
                if (r != t->s_r1 || c != t->s_c1) { t->s_r1 = r; t->s_c1 = c; t->sel = 1; }
            }
            if (!left) {
                if (t->selecting && t->sel) term_copy_selection(t);   /* like X11: selecting copies */
                t->dragging = 0;
                t->resizing = 0;
                t->selecting = 0;
            }
        }
        for (int a = 0; a < APP_COUNT; a++) if (g_apps[a].is_open()) g_apps[a].mouse(mx, my, left);
        desk_mouse(fi, mx, my, left);

        desk_refresh(0);
        gui_view_t view;
        capture_view(&view, sec);
        int changed = g_force_redraw || memcmp(&view, &g_last_view, sizeof(view)) != 0;

        if (!changed) {
            /* only the mouse moved (or its shape changed): restore its old spot, draw it anew */
            if (mx != g_ptr_drawn_x || my != g_ptr_drawn_y || cursor_shape(mx, my) != g_drawn_shape) {
                fb_present_rect(g_ptr_drawn_x + CURSOR_X0, g_ptr_drawn_y + CURSOR_Y0, CURSOR_W, CURSOR_H);
                draw_cursor(mx, my);
                g_ptr_drawn_x = mx;
                g_ptr_drawn_y = my;
            }
            return;
        }

        /* at most ~60 frames per second; the change is kept for next time */
        uint32_t now = timer_ms();
        if (!g_force_redraw && (uint32_t)(now - last_frame_ms) < 16) return;
        last_frame_ms = now;

        render_desktop(fi, mx, my);
        g_ptr_drawn_x = mx;
        g_ptr_drawn_y = my;
        g_last_view = view;
        g_force_redraw = 0;
        return;
    }

    uint32_t sec = timer_ticks() / 100u;
    if (sec != g_last_clock_sec) {
        g_last_clock_sec = sec;
        draw_taskbar();
        draw_menu();
    } else if (g_menu_open) {
        /* keep menu visible even if other code redraws */
        draw_menu();
    }
}

void gui_set_enabled(int enabled) {
    if (!enabled) hw_cursor_hide();
    while (keyboard_take_fkey()) {}          /* (pressed while the desktop was off) */
    g_gui_enabled = enabled ? 1 : 0;
    g_ptr_drawn_x = -1;              /* no pointer on the screen until the desktop draws one */
    g_menu_open = 0;
    g_force_redraw = 1;
    ctxmenu_close();
    if (!enabled) for (int a = 0; a < APP_COUNT; a++) { g_apps[a].close(); g_app_min[a] = 0; }
    g_front_app = -1;
    /* Hide (don't tear down) any open windows: their vts and shell tasks
     * are permanent for the OS's lifetime (see term_win_t.vt), so a later
     * startx can bring them straight back instead of every window losing
     * its running state on every stopx. */
    for (int i = 0; i < TERM_WIN_MAX; i++) { g_terms[i].open = 0; g_terms[i].minimized = 0; }
    g_ntb = 0;

    if (gfx_available()) {
        if (g_gui_enabled) terminal_set_mode(TERMINAL_MODE_SUSPENDED);
        else {
            terminal_vt_set_active(0);
            terminal_set_mode(TERMINAL_MODE_FRAMEBUFFER);
            terminal_fb_set_scale(1); /* console stays small */
            terminal_clear();

            /* Tear the desktop backbuffer down: leaving it active makes
             * every future terminal_putchar() (plain shell typing
             * included) pay for a full 800x600 fb_present() copy, forever.
             * g_backbuf_active=0 also makes the next startx correctly
             * re-run gui_poll()'s one-time backbuffer setup. */
            fb_clear_backbuffer();
            g_backbuf_active = 0;
        }
    }
}

int gui_is_enabled(void) {
    return g_gui_enabled;
}

int gui_focused_vt(void) {
    if (login_is_locked()) return -100;    /* the lock screen has the keyboard */
    /* an SSH session's shell always has the focus of its own terminal */
    int tt = tty_current();
    if (tt >= 0) return tty_vt(tt);

    if (!gfx_available() || !g_gui_enabled) return 0; /* plain console owns input */

    /* the browser in front reads the keyboard itself (its own task) */
    if (g_front_app == APP_BROWSER && browser_is_open() && !g_app_min[APP_BROWSER]) return BROWSER_VT;
    if (g_front_app == APP_NOTEPAD && notepad_is_open() && !g_app_min[APP_NOTEPAD]) return NOTEPAD_VT;
    if (g_front_app == APP_APPWIN && appwin_any_visible()) return APPWIN_VT;
    if ((g_front_app == APP_LAUNCHER || g_front_app == APP_TASKMGR || g_front_app == APP_FILES || g_front_app == APP_SETTINGS || g_front_app == APP_INSTALLER) && app_visible(g_front_app))
        return APPWIN_VT + 1;          /* keys handed out by gui_poll() */

    /* Frontmost OPEN window, if any - g_term_order always lists every
     * slot, closed or not, so this has to skip closed ones explicitly. */
    for (int oi = TERM_WIN_MAX - 1; oi >= 0; oi--) {
        int wi = g_term_order[oi];
        if (g_terms[wi].open && !g_terms[wi].minimized) return g_terms[wi].vt;
    }

    /* No window open yet: fall back to the (hidden) console's vt0 rather
     * than nobody, so its shell task keeps reading keys and Ctrl+T / the
     * Start menu's arrow-key navigation still work with the mouse alone
     * unavailable, exactly like before any window existed. */
    return 0;
}

int gui_close_terminal_by_vt(int vt) {
    for (int i = 0; i < TERM_WIN_MAX; i++) {
        if (g_terms[i].open && g_terms[i].vt == vt) {
            g_terms[i].open = 0;
            return 1;
        }
    }
    return 0;
}

/* a Start menu entry / desktop shortcut / desktop menu item */
static void do_action(int act) {
    g_menu_open = 0;
    switch (act) {
    case ACT_ABOUT: settings_open_page(SETTINGS_PAGE_ABOUT); g_app_min[APP_SETTINGS] = 0; raise_app(APP_SETTINGS); break;
    case ACT_TERMINAL: open_new_terminal(); g_front_app = -1; break;
    case ACT_FILES: explorer_open(NULL); g_app_min[APP_FILES] = 0; raise_app(APP_FILES); break;
    case ACT_BROWSER: browser_open(NULL); g_app_min[APP_BROWSER] = 0; raise_app(APP_BROWSER); break;
    case ACT_NOTEPAD: notepad_open(NULL); g_app_min[APP_NOTEPAD] = 0; raise_app(APP_NOTEPAD); break;
    case ACT_APPS: launcher_open(); g_app_min[APP_LAUNCHER] = 0; raise_app(APP_LAUNCHER); break;
    case ACT_TASKMGR: taskmgr_open(); g_app_min[APP_TASKMGR] = 0; raise_app(APP_TASKMGR); break;
    case ACT_SETTINGS: settings_open(); g_app_min[APP_SETTINGS] = 0; raise_app(APP_SETTINGS); break;
    case ACT_WALLPAPER: settings_open_page(SETTINGS_PAGE_DISPLAY); g_app_min[APP_SETTINGS] = 0; raise_app(APP_SETTINGS); break;
    case ACT_QUIT: gui_set_enabled(0); break;
    case ACT_INSTALL: installer_open(); g_app_min[APP_INSTALLER] = 0; raise_app(APP_INSTALLER); break;
    case ACT_RESTART: shell_power(1); break;          /* (saves the files first) */
    case ACT_SHUTDOWN: shell_power(0); break;
    case ACT_LOCK:
        if (passwd_is_set(PASSWD_USER)) {
            g_lock_pending = 1;            /* gui_poll() shows it, outside any menu */
        } else {
            settings_open_page(SETTINGS_PAGE_STARTUP);
            settings_show_status("Set a password first to lock the screen");
            g_app_min[APP_SETTINGS] = 0;
            raise_app(APP_SETTINGS);
        }
        break;
    }
}

/* an installed app, from the desktop or Programs (as the Apps window does) */
static void run_app(const char* name) {
    char err[96];
    g_menu_open = 0;
    if (pkg_run(name, 0, NULL, 1, err, sizeof(err)) < 0) klog("gui: %s: %s\n", name, err);
}

static void menu_activate(void) {
    int act = MENU_ACTS[menu_idx(g_menu_sel)];
    if (gfx_available()) {
        if (menu_has_sub(g_menu_sel)) {               /* Programs, Shut down: their submenu */
            const fb_info_t* fi = fb_info();
            if (fi) open_submenu(fi, g_menu_sel);
            return;
        }
        do_action(act);
        return;
    }
    /* text mode: the desktop's apps need the framebuffer */
    menu_close_redraw();
    if (act == ACT_QUIT || act == ACT_POWER) gui_set_enabled(0);
}

/* ── windows for the Task Manager ──────────────────────────────────── */

int gui_windows(gui_win_info_t* out, int max) {
    if (!g_gui_enabled) return 0;
    sync_taskbar();
    int front = win_front(), n = 0;
    for (int j = 0; j < g_ntb && n < max; j++) {
        out[n].handle = g_tb[j];
        win_title(g_tb[j], out[n].title, sizeof(out[n].title));
        out[n].minimized = win_minimized(g_tb[j]);
        out[n].focused = g_tb[j] == front;
        n++;
    }
    return n;
}

void gui_window_close(int handle) { if (win_exists(handle)) win_close(handle); }
void gui_window_activate(int handle) { if (win_exists(handle)) win_activate(handle); }

void gui_raise_files(void) { g_app_min[APP_FILES] = 0; raise_app(APP_FILES); }

/* `installer`: only on the live CD, with the desktop running */
int gui_open_installer(void) {
    if (!g_gui_enabled || !gfx_available() || !installer_available()) return 0;
    do_action(ACT_INSTALL);
    return 1;
}

/* a new resolution: the desktop's buffers and every window follow at the next frame */
void gui_text_changed(void) {
    for (int i = 0; i < TERM_WIN_MAX; i++) if (g_terms[i].open) term_apply_size(&g_terms[i]);
}

void gui_screen_changed(void) {
    if (g_backbuf_active) { fb_clear_backbuffer(); g_backbuf_active = 0; }
    g_force_redraw = 1;
}

int gui_open_apps(void) {
    if (!gfx_available() || !g_gui_enabled) return 0;
    do_action(ACT_APPS);
    return 1;
}

int gui_open_settings(void) {
    if (!gfx_available() || !g_gui_enabled) return 0;
    do_action(ACT_SETTINGS);
    return 1;
}

int gui_open_taskmgr(void) {
    if (!gfx_available() || !g_gui_enabled) return 0;
    do_action(ACT_TASKMGR);
    return 1;
}

int gui_open_notepad(const char* path) {
    if (!gfx_available() || !g_gui_enabled) return 0;
    notepad_open(path);
    raise_app(APP_NOTEPAD);
    return 1;
}

int gui_notepad_focused(void) {
    return gfx_available() && g_gui_enabled && g_front_app == APP_NOTEPAD && notepad_is_open() && tty_current() < 0;
}

void gui_terminal_run(const char* cmd) {
    open_new_terminal();
    g_front_app = -1;               /* the new terminal comes up in front */
    /* typed into the new window: it has the keyboard focus now */
    keyboard_inject(cmd);
}

int gui_open_files(const char* path) {
    if (!gfx_available() || !g_gui_enabled) return 0;
    explorer_open(path);
    raise_app(APP_FILES);
    return 1;
}

int gui_open_browser(const char* url) {
    if (!gfx_available() || !g_gui_enabled) return 0;
    browser_open(url);
    raise_app(APP_BROWSER);
    return 1;
}

int gui_browser_focused(void) {
    return gfx_available() && g_gui_enabled && g_front_app == APP_BROWSER && browser_is_open() && tty_current() < 0;
}

static int desk_arrow(char code);

int gui_handle_arrow(char esc_code) {
    if (gfx_available() && g_gui_enabled && !g_menu_open) {
        if (kb_modal()) {
            int code = esc_code == 'A' ? KB_UP : esc_code == 'B' ? KB_DOWN : esc_code == 'C' ? KB_RIGHT : esc_code == 'D' ? KB_LEFT :
                       esc_code == 'H' ? KB_HOME : esc_code == 'F' ? KB_END : 0;
            if (code) kb_modal_key(code);
            g_force_redraw = 1;
            return 1;
        }
        if (desk_arrow(esc_code)) return 1;
    }
    if (!g_menu_open) return 0;
    if (gfx_available() && g_gui_enabled) { startmenu_arrow(esc_code); return 1; }
    if (esc_code == 'A') { /* up */
        if (g_menu_sel > 0) g_menu_sel--;
        draw_menu();
        return 1;
    }
    if (esc_code == 'B') { /* down */
        if (g_menu_sel < MENU_ITEMS - 1) g_menu_sel++;
        draw_menu();
        return 1;
    }
    return 1; /* swallow other arrows while menu is open */
}

int gui_handle_key(char c) {
    if (gfx_available() && !g_gui_enabled) return 0;
    if (gfx_available() && kb_modal() && !g_menu_open && c != 20) {
        int code = c == '\n' ? KB_ENTER : c == 27 ? KB_ESC : c == '\t' ? ((keyboard_mods() & 1) ? KB_BACKTAB : KB_TAB) : c == ' ' ? KB_SPACE : 0;
        if (code) kb_modal_key(code);
        g_force_redraw = 1;
        return 1;
    }
    /* Ctrl+T toggles Start menu */
    if (c == 20) {
        if (gfx_available()) {
            if (g_menu_open) g_menu_open = 0;
            else start_open();
            g_force_redraw = 1;
            return 1;
        }
        g_menu_open = !g_menu_open;
        if (g_menu_open) {
            g_menu_sel = 0;
            draw_taskbar();
            draw_menu();
        } else {
            menu_close_redraw();
        }
        return 1;
    }

    /* Enter on the desktop (no window in front): the selected icon opens */
    if (!g_menu_open && c == '\n' && g_desk_sel >= 0 && g_desk_sel < g_ndesk && g_front_app < 0 && front_term() < 0) {
        desk_open(g_desk_sel);
        return 1;
    }
    if (!g_menu_open && g_front_app < 0 && front_term() < 0 && gfx_available()) {
        if (c == 1) { for (int i = 0; i < g_ndesk; i++) g_desk_mark[i] = 1; if (g_desk_sel < 0 && g_ndesk) g_desk_sel = 0; g_force_redraw = 1; return 1; }   /* Ctrl+A */
        if (c == 3 || c == 24) { desk_clip_marked(c == 24); return 1; }                              /* Ctrl+C / X */
        if (c == 22) { char m[112]; fileops_paste(DESKTOP_DIR, m, sizeof(m)); desk_refresh(1); explorer_refresh(); return 1; }   /* Ctrl+V */
    }
    if (!g_menu_open) return 0;
    if (gfx_available()) {                      /* the Windows 7 menu: typing searches */
        if (c == 27) startmenu_escape();
        else startmenu_key(c);
        g_force_redraw = 1;
        return 1;
    }

    if (c == '\n') {
        menu_activate();
        return 1;
    }
    if (c == 27) { /* ESC */
        menu_close_redraw();
        return 1;
    }

    return 1; /* swallow typing while menu is open */
}

/* ══ the keyboard everywhere ══════════════════════════════════════════
 * Alt+Tab (Shift: backwards) - the window switcher, let Alt go to switch;
 * Win+Tab - task view (arrows, Enter, Esc); Win+D - the desktop (then the
 * arrows go through its icons, Enter opens); Win+T - the taskbar (arrows,
 * Enter); Win+E Files, Win+I Settings, Win+L lock, Win+R a terminal,
 * Win+M everything down, Win+1..9 the taskbar's windows, Win+Down / Up
 * minimize / bring back; the Menu key (Shift+F10) - a right-click menu. */

/* the windows in the order they were used, the front one first */
static int g_mru[TB_MAX], g_nmru;
static void mru_update(void) {
    int f = win_front();
    int n = 0, tmp[TB_MAX];
    if (f) tmp[n++] = f;
    for (int i = 0; i < g_nmru; i++) if (g_mru[i] != f && win_exists(g_mru[i]) && n < TB_MAX) tmp[n++] = g_mru[i];
    for (int j = 0; j < g_ntb; j++) {
        int have = 0;
        for (int i = 0; i < n; i++) if (tmp[i] == g_tb[j]) have = 1;
        if (!have && n < TB_MAX) tmp[n++] = g_tb[j];
    }
    memcpy(g_mru, tmp, sizeof(int) * (size_t)n);
    g_nmru = n;
}

static int g_list[TB_MAX], g_nlist;      /* the windows the switcher / task view show */

static void tv_card(const fb_info_t* fi, int i, int* x, int* y, int* w, int* h);
static int tv_card_at(const fb_info_t* fi, int mx, int my);
static void tv_activate(int sel);

static int kb_modal(void) { return g_sw_open || g_tv_open || g_bar_kb || ctxmenu_is_open(); }

static void list_windows(void) {
    sync_taskbar();
    mru_update();
    g_nlist = g_nmru;
    memcpy(g_list, g_mru, sizeof(int) * (size_t)g_nlist);
}

static void show_desktop(int on) {
    static int downed[TB_MAX], ndowned;
    if (on) {
        ndowned = 0;
        for (int j = 0; j < g_ntb; j++) if (!win_minimized(g_tb[j])) { downed[ndowned++] = g_tb[j]; win_minimize(g_tb[j]); }
        g_front_app = -1;
        if (g_desk_sel < 0 && g_ndesk) g_desk_sel = 0;
    } else {
        for (int i = 0; i < ndowned; i++) if (win_exists(downed[i])) win_activate(downed[i]);
        ndowned = 0;
    }
}

static int desk_focused(void) { return !g_menu_open && g_front_app < 0 && front_term() < 0; }

/* the arrows on the desktop: through its icons (columns, top to bottom) */
static int desk_arrow(char code) {
    if (!desk_focused() || !g_ndesk) return 0;
    if (code == 'P') { desk_trash_marked(); return 1; }        /* Delete: to the Recycle Bin */
    const fb_info_t* fi = fb_info();
    if (!fi) return 0;
    int rows = desk_rows(fi);
    int i = g_desk_sel < 0 ? 0 : g_desk_sel;
    if (g_desk_sel >= 0) {
        if (code == 'A' && i % rows > 0) i--;
        else if (code == 'B' && i + 1 < g_ndesk && (i + 1) % rows) i++;
        else if (code == 'C' && i + rows < g_ndesk) i += rows;
        else if (code == 'D' && i - rows >= 0) i -= rows;
        else if (code == 'H') i = 0;
        else if (code == 'F') i = g_ndesk - 1;
    }
    g_desk_sel = i;
    g_force_redraw = 1;
    return 1;
}

static void tv_activate(int sel) {
    if (sel >= 0 && sel < g_nlist && win_exists(g_list[sel])) win_activate(g_list[sel]);
}

static int kb_modal_key(int code) {
    if (ctxmenu_is_open()) return ctxmenu_key(code);
    if (g_sw_open) {
        if (code == KB_ESC) g_sw_open = 0;
        else if (code == KB_ENTER) { tv_activate(g_sw_sel); g_sw_open = 0; }
        else if (code == KB_RIGHT || code == KB_TAB) g_sw_sel = (g_sw_sel + 1) % (g_nlist ? g_nlist : 1);
        else if (code == KB_LEFT || code == KB_BACKTAB) g_sw_sel = (g_sw_sel + g_nlist - 1) % (g_nlist ? g_nlist : 1);
        return 1;
    }
    if (g_tv_open) {
        int cols = g_nlist <= 4 ? (g_nlist ? g_nlist : 1) : 4;
        if (code == KB_ESC) g_tv_open = 0;
        else if (code == KB_ENTER || code == KB_SPACE) { tv_activate(g_tv_sel); g_tv_open = 0; }
        else if (code == KB_RIGHT || code == KB_TAB) g_tv_sel++;
        else if (code == KB_LEFT || code == KB_BACKTAB) g_tv_sel--;
        else if (code == KB_DOWN) g_tv_sel += cols;
        else if (code == KB_UP) g_tv_sel -= cols;
        else if (code == KB_DEL && g_tv_sel >= 0 && g_tv_sel < g_nlist) { win_close(g_list[g_tv_sel]); list_windows(); }
        if (g_tv_sel >= g_nlist) g_tv_sel = g_nlist - 1;
        if (g_tv_sel < 0) g_tv_sel = 0;
        return 1;
    }
    if (g_bar_kb) {
        sync_taskbar();
        if (code == KB_ESC || code == KB_UP) { g_bar_kb = 0; return 1; }
        if (!g_ntb) { g_bar_kb = 0; return 1; }
        if (code == KB_RIGHT || code == KB_TAB) g_bar_sel = (g_bar_sel + 1) % g_ntb;
        else if (code == KB_LEFT || code == KB_BACKTAB) g_bar_sel = (g_bar_sel + g_ntb - 1) % g_ntb;
        else if (code == KB_HOME) g_bar_sel = 0;
        else if (code == KB_END) g_bar_sel = g_ntb - 1;
        else if (code == KB_ENTER || code == KB_SPACE) {
            int h = g_tb[g_bar_sel];
            if (win_front() == h && !win_minimized(h)) win_minimize(h); else win_activate(h);
            g_bar_kb = 0;
        }
        if (g_bar_sel >= g_ntb) g_bar_sel = g_ntb - 1;
        return 1;
    }
    return 0;
}

static int kb_global(int k) {
    int c = KEYF_CODE(k);
    if (c == KEYF_TAB && (k & KEYF_ALT)) {
        if (g_menu_open) { g_menu_open = 0; }
        if (!g_sw_open) {
            list_windows();
            if (!g_nlist) return 1;
            g_sw_open = 1;
            g_tv_open = 0;
            g_sw_sel = g_nlist > 1 ? ((k & KEYF_SHIFT) ? g_nlist - 1 : 1) : 0;
        } else if (g_nlist) {
            g_sw_sel = (g_sw_sel + ((k & KEYF_SHIFT) ? g_nlist - 1 : 1)) % g_nlist;
        }
        g_force_redraw = 1;
        return 1;
    }
    if (c == KEYF_ALTUP) {
        if (g_sw_open) { tv_activate(g_sw_sel); g_sw_open = 0; g_force_redraw = 1; }
        return 1;
    }
    if (c == KEYF_MENU || (c == KEYF_F1 + 9 && (k & KEYF_SHIFT))) {
        const fb_info_t* fi = fb_info();
        if (!fi || g_menu_open) return 1;
        if (desk_focused()) {
            if (g_desk_sel >= 0 && g_desk_sel < g_ndesk) {
                int x, y;
                desk_cell(fi, g_desk_sel, &x, &y);
                open_icon_menu(g_desk_sel, x + CELL_W / 2, y + CELL_H / 2);
            } else open_desktop_menu((int)fi->width / 3, (int)fi->height / 3);
        } else if (g_front_app == APP_FILES) {
            explorer_menu_key();
        }
        if (ctxmenu_is_open()) ctxmenu_select_first();
        g_force_redraw = 1;
        return 1;
    }
    if (!(k & KEYF_WINKEY)) return 0;
    g_force_redraw = 1;
    g_menu_open = 0;
    sync_taskbar();
    switch (c) {
    case '\t':
        if (g_tv_open) { g_tv_open = 0; break; }
        list_windows();
        g_tv_open = 1;
        g_sw_open = 0;
        g_tv_sel = g_nlist > 1 ? 1 : 0;
        break;
    case 'd': {
        static int shown;
        int any = 0;
        for (int j = 0; j < g_ntb; j++) if (!win_minimized(g_tb[j])) any = 1;
        if (any) { show_desktop(1); shown = 1; }
        else if (shown) { show_desktop(0); shown = 0; }
        else if (g_desk_sel < 0 && g_ndesk) g_desk_sel = 0;
        break;
    }
    case 'm': for (int j = 0; j < g_ntb; j++) win_minimize(g_tb[j]); g_front_app = -1; break;
    case 'e': do_action(ACT_FILES); break;
    case 'i': do_action(ACT_SETTINGS); break;
    case 'l': do_action(ACT_LOCK); break;
    case 'r': do_action(ACT_TERMINAL); break;
    case 't': g_bar_kb = g_ntb > 0; g_bar_sel = 0; break;
    case KEYW_DOWN: { int f = win_front(); if (f) win_minimize(f); break; }
    case KEYW_UP: {
        mru_update();
        for (int i = 0; i < g_nmru; i++) if (win_minimized(g_mru[i])) { win_activate(g_mru[i]); break; }
        int t = front_term();
        const fb_info_t* fi = fb_info();
        if (t >= 0 && g_front_app < 0 && fi && !g_terms[t].maxed) toggle_max_term(fi, &g_terms[t]);
        break;
    }
    default:
        if (c >= '1' && c <= '9' && c - '1' < g_ntb) {
            int h = g_tb[c - '1'];
            if (win_front() == h && !win_minimized(h)) win_minimize(h); else win_activate(h);
        }
        break;
    }
    return 1;
}

static void kb_card(int x, int y, int w, int h, int handle, int sel, int big) {
    uint32_t bg = sel ? 0x00405478u : 0x00262D38u;
    draw_bevel_box(x, y, w, h, bg, sel ? 0x0090B8F0u : 0x00505E74u, 0x0010151Du);
    if (sel) {
        gfx_fill_rect(x - 3, y - 3, w + 6, 3, 0x00FFD34Eu); gfx_fill_rect(x - 3, y + h, w + 6, 3, 0x00FFD34Eu);
        gfx_fill_rect(x - 3, y - 3, 3, h + 6, 0x00FFD34Eu); gfx_fill_rect(x + w, y - 3, 3, h + 6, 0x00FFD34Eu);
    }
    int saved = g_is;
    g_is = big ? 2 : 1;
    draw_win_glyph(handle, x + w / 2 - (big ? 14 : 7), y + (big ? 16 : 10), bg);
    g_is = saved;
    char t[48];
    win_title(handle, t, sizeof(t));
    int maxc = (w - 12) / 8;
    if (maxc > 0 && (int)strlen(t) > maxc) { t[maxc] = 0; if (maxc > 1) t[maxc - 1] = '.'; }
    gfx_draw_text(x + (w - (int)strlen(t) * 8) / 2, y + h - (big ? 22 : 16), t, 0x00E8EEF6u, bg);
    if (win_minimized(handle)) gfx_draw_text(x + (w - 9 * 8) / 2, y + h - 10, "minimized", 0x009AA6B6u, bg);
}

static void kb_draw_overlays(const fb_info_t* fi) {
    int W = (int)fi->width, H = (int)fi->height;
    if (g_sw_open && g_nlist) {
        int cw = 132, ch = 84, gap = 10;
        int n = g_nlist, per = (W - 80) / (cw + gap);
        if (per < 1) per = 1;
        if (n > per) n = per;
        int first = g_sw_sel >= n ? g_sw_sel - n + 1 : 0;
        int pw = n * (cw + gap) + gap + 20, ph = ch + 56;
        int px = (W - pw) / 2, py = (H - ph) / 2;
        draw_bevel_box(px, py, pw, ph, 0x001B212Bu, 0x00505E74u, 0x000C0F14u);
        char t[64];
        win_title(g_list[g_sw_sel], t, sizeof(t));
        gfx_draw_text(px + (pw - (int)strlen(t) * 8) / 2, py + 12, t, 0x00FFFFFFu, 0x001B212Bu);
        for (int i = 0; i < n; i++)
            kb_card(px + 20 + i * (cw + gap), py + 36, cw, ch, g_list[first + i], first + i == g_sw_sel, 1);
    }
    if (g_tv_open) {
        /* everything dimmed, the windows as cards */
        uint32_t* t;
        int stride, tw, th;
        t = fb_target(&stride, &tw, &th);
        if (t) for (int y = 0; y < th - BAR_H; y++) for (int x = 0; x < tw; x++) { uint32_t p = t[y * stride + x]; t[y * stride + x] = (p >> 2) & 0x003F3F3Fu; }
        gfx_draw_text_scaled(40, 30, 2, "Task view", 0x00FFFFFFu, 0x00000000u);
        gfx_draw_text(40, 66, "Arrows choose, Enter switches, Delete closes, Esc goes back", 0x009AA6B6u, 0x00000000u);
        if (!g_nlist) gfx_draw_text(40, 110, "No windows are open.", 0x00E8EEF6u, 0);
        for (int i = 0; i < g_nlist; i++) {
            int x, y, cw, ch;
            tv_card(fi, i, &x, &y, &cw, &ch);
            if (y + ch > H - BAR_H - 10) break;
            kb_card(x, y, cw, ch, g_list[i], i == g_tv_sel, 1);
        }
    }
    if (g_bar_kb && g_bar_sel >= 0 && g_bar_sel < g_ntb) {
        int bw = tb_btn_w(fi), x = TB_X + g_bar_sel * bw, y = H - BAR_H + 1;
        gfx_fill_rect(x - 1, y, bw - 2, 2, 0x00FFD34Eu);
        gfx_fill_rect(x - 1, y + BAR_H - 4, bw - 2, 2, 0x00FFD34Eu);
        gfx_fill_rect(x - 1, y, 2, BAR_H - 2, 0x00FFD34Eu);
        gfx_fill_rect(x + bw - 5, y, 2, BAR_H - 2, 0x00FFD34Eu);
    }
}

static void tv_card(const fb_info_t* fi, int i, int* x, int* y, int* w, int* h) {
    int W = (int)fi->width;
    int cols = g_nlist <= 4 ? (g_nlist ? g_nlist : 1) : 4;
    int cw = (W - 80 - (cols - 1) * 24) / cols, ch = cw * 9 / 16;
    if (cw > 300) { cw = 300; ch = 170; }
    int gx = (W - (cols * cw + (cols - 1) * 24)) / 2;
    *x = gx + (i % cols) * (cw + 24);
    *y = 110 + (i / cols) * (ch + 30);
    *w = cw;
    *h = ch;
}

static int tv_card_at(const fb_info_t* fi, int mx, int my) {
    for (int i = 0; i < g_nlist; i++) {
        int x, y, w, h;
        tv_card(fi, i, &x, &y, &w, &h);
        if (mx >= x && mx < x + w && my >= y && my < y + h) return i;
    }
    return -1;
}

/* ══ the desktop: files, the Recycle Bin, selection, drag and drop ═════ */

static int desk_nmarked(void) { int n = 0; for (int i = 0; i < g_ndesk; i++) n += desk_marked(i); return n; }

static void desk_open(int i) {
    if (i < 0 || i >= g_ndesk) return;
    desk_item_t* d = &g_desk[i];
    if (d->kind == DK_TRASH) { fs_mkdir_p(TRASH_FILES); fs_mkdir_p(TRASH_INFO); explorer_open(TRASH_FILES); gui_raise_files(); return; }
    if (d->kind == DK_FILE || d->kind == DK_DIR) { explorer_launch(d->path); return; }
    if (d->act == ACT_RUN_APP) run_app(d->app);
    else do_action(d->act);
}

/* the selected files and folders of the desktop (not the programs) */
static int desk_marked_paths(char (*out)[FS_PATH_LEN], int max) {
    int n = 0;
    for (int i = 0; i < g_ndesk && n < max; i++)
        if (desk_marked(i) && (g_desk[i].kind == DK_FILE || g_desk[i].kind == DK_DIR)) kstrlcpy(out[n++], g_desk[i].path, FS_PATH_LEN);
    return n;
}

static void desk_trash_marked(void) {
    static char p[64][FS_PATH_LEN];
    int n = desk_marked_paths(p, 64);
    for (int i = 0; i < n; i++) trash_put(p[i]);
    if (n) { g_desk_marks_clear(); g_desk_sel = -1; desk_refresh(1); explorer_refresh(); }
}

static void desk_clip_marked(int cut) {
    static char p[CLIP_MAX][FS_PATH_LEN];
    int n = desk_marked_paths(p, CLIP_MAX);
    const char* ptrs[CLIP_MAX];
    for (int i = 0; i < n; i++) ptrs[i] = p[i];
    if (n) fileops_clip_set(ptrs, n, cut);
}

static void desk_new(int folder) {
    char p[FS_PATH_LEN];
    fs_mkdir_p(DESKTOP_DIR);
    fileops_unique(DESKTOP_DIR, folder ? "New folder" : "New text document.txt", p, sizeof(p));
    if (folder) fs_mkdir(p);
    else fs_write_path(p, "", 0);
    desk_refresh(1);
    for (int i = 0; i < g_ndesk; i++) if (!strcmp(g_desk[i].path, p)) { g_desk_marks_clear(); g_desk_sel = i; }
}

/* the cell under (mx, my) */
static void desk_cell_at(const fb_info_t* fi, int mx, int my, int* col, int* row) {
    int c = (mx - DESK_X) / CELL_W, r = (my - DESK_Y) / CELL_H;
    int cols = desk_cols(fi), rows = desk_rows(fi);
    if (c < 0) c = 0;
    if (r < 0) r = 0;
    if (c >= cols) c = cols - 1;
    if (r >= rows) r = rows - 1;
    *col = c;
    *row = r;
}

/* ── what is being dragged ── */
static char g_dnd[64][FS_PATH_LEN];
static int  g_ndnd, g_dnd_src, g_dnd_on;
static int  g_dnd_icons[DESK_MAX], g_ndnd_icons;     /* (from the desktop: the icons, to move them) */
static int  g_dnd_from_col, g_dnd_from_row;

void gui_drag_begin(const char (*paths)[FS_PATH_LEN], int n, int source) {
    if (n > 64) n = 64;
    for (int i = 0; i < n; i++) kstrlcpy(g_dnd[i], paths[i], FS_PATH_LEN);
    g_ndnd = n;
    g_dnd_src = source;
    g_dnd_on = 1;
    g_ndnd_icons = 0;
    g_force_redraw = 1;
}

int gui_drag_has(const char* dir, const char* name) {
    if (!g_dnd_on) return 0;
    char p[FS_PATH_LEN];
    if (!strcmp(dir, "/")) ksnprintf(p, sizeof(p), "/%s", name);
    else ksnprintf(p, sizeof(p), "%s/%s", dir, name);
    for (int i = 0; i < g_ndnd; i++) if (!strcmp(g_dnd[i], p)) return 1;
    return 0;
}

/* a window under (mx, my)? (a drop there: only Files takes it) */
static int over_window(int mx, int my) {
    if (g_front_app >= 0 && app_visible(g_front_app) && g_apps[g_front_app].contains(mx, my)) return 1;
    for (int oi = 0; oi < TERM_WIN_MAX; oi++) {
        term_win_t* w = &g_terms[oi];
        if (w->open && !w->minimized && mx >= w->x && mx < w->x + w->w && my >= w->y && my < w->y + w->h) return 1;
    }
    return app_at(mx, my, 0) >= 0;
}

static int over_files(int mx, int my) {
    if (!explorer_is_open() || !app_visible(APP_FILES) || !explorer_contains(mx, my)) return 0;
    /* Files in front there (not covered by another window) */
    if (g_front_app == APP_FILES) return 1;
    if (g_front_app >= 0 && app_visible(g_front_app) && g_apps[g_front_app].contains(mx, my)) return 0;
    for (int oi = 0; oi < TERM_WIN_MAX; oi++) {
        term_win_t* w = &g_terms[oi];
        if (w->open && !w->minimized && mx >= w->x && mx < w->x + w->w && my >= w->y && my < w->y + w->h) return 0;
    }
    return app_at(mx, my, 0) == APP_FILES;
}

static void dnd_drop(const fb_info_t* fi, int mx, int my) {
    int copy = (keyboard_mods() & 2) != 0;              /* Ctrl: a copy */
    int bar_y = (int)fi->height - BAR_H;
    g_desk_drop = -1;
    if (over_files(mx, my)) {
        if (g_ndnd) explorer_drop((const char (*)[FS_PATH_LEN])g_dnd, g_ndnd, mx, my, copy);
        raise_app(APP_FILES);
    } else if (my < bar_y && !over_window(mx, my)) {
        int i = icon_at(mx, my);
        int self = 0;
        for (int k = 0; k < g_ndnd_icons; k++) if (g_dnd_icons[k] == i) self = 1;
        if (i >= 0 && !self && g_desk[i].kind == DK_TRASH) {
            for (int k = 0; k < g_ndnd; k++) trash_put(g_dnd[k]);
        } else if (i >= 0 && !self && g_desk[i].kind == DK_DIR) {
            char err[64];
            for (int k = 0; k < g_ndnd; k++) fileops_transfer(g_dnd[k], g_desk[i].path, copy, NULL, 0, err, sizeof(err));
        } else if (i >= 0 && !self && g_desk[i].kind == DK_APP && g_ndnd) {
            /* a file dropped on a program: it opens it (Banana Code, Photos...) */
            if (g_desk[i].act == ACT_RUN_APP) {
                char err[96];
                char* argv[2] = { g_desk[i].app, g_dnd[0] };
                pkg_run(g_desk[i].app, 2, argv, 1, err, sizeof(err));
            } else if (g_desk[i].act == ACT_NOTEPAD) gui_open_notepad(g_dnd[0]);
            else if (g_desk[i].act == ACT_BROWSER) gui_open_browser(g_dnd[0]);
        } else {
            int col, row;
            desk_cell_at(fi, mx, my, &col, &row);
            if (g_dnd_src == GUI_DRAG_DESKTOP) {
                /* icons moved: each keeps its place relative to the one held */
                int dc = col - g_dnd_from_col, dr = row - g_dnd_from_row;
                int cols = desk_cols(fi), rows = desk_rows(fi);
                for (int k = 0; k < g_ndnd_icons; k++) {
                    desk_item_t* d = &g_desk[g_dnd_icons[k]];
                    int c = d->col + dc, r = d->row + dr;
                    if (c < 0) c = 0;
                    if (r < 0) r = 0;
                    if (c >= cols) c = cols - 1;
                    if (r >= rows) r = rows - 1;
                    /* a cell another icon holds: that icon moves to where this one was */
                    for (int o = 0; o < g_ndesk; o++)
                        if (g_desk[o].col == c && g_desk[o].row == r && o != g_dnd_icons[k]) {
                            int moving = 0;
                            for (int q = 0; q < g_ndnd_icons; q++) if (g_dnd_icons[q] == o) moving = 1;
                            if (!moving) {
                                char ko[56];
                                desk_key(&g_desk[o], ko, sizeof(ko));
                                layout_set(ko, d->col, d->row);
                                g_desk[o].col = d->col;
                                g_desk[o].row = d->row;
                            }
                        }
                    char key[56];
                    desk_key(d, key, sizeof(key));
                    layout_set(key, c, r);
                    d->col = c;
                    d->row = r;
                }
                layout_save();
            } else {
                /* from Files: onto the desktop (the Desktop folder), where it was let go */
                char err[64], dst[FS_PATH_LEN];
                fs_mkdir_p(DESKTOP_DIR);
                for (int k = 0; k < g_ndnd; k++)
                    if (fileops_transfer(g_dnd[k], DESKTOP_DIR, copy, dst, sizeof(dst), err, sizeof(err)) == 0) {
                        char key[56];
                        ksnprintf(key, sizeof(key), "file:%s", fileops_base(dst));
                        layout_set(key, col, row + k < desk_rows(fi) ? row + k : row);
                    }
                layout_save();
            }
        }
    }
    g_ndnd = 0;
    g_dnd_on = 0;
    g_ndnd_icons = 0;
    desk_refresh(1);
    explorer_refresh();
    g_force_redraw = 1;
}

static void desk_mouse(const fb_info_t* fi, int mx, int my, int left) {
    if (g_dnd_on) {
        if (!left) { dnd_drop(fi, mx, my); return; }
        if (over_files(mx, my)) explorer_drag_over(mx, my);
        int d = -1;
        if (!over_window(mx, my)) {
            int i = icon_at(mx, my);
            if (i >= 0 && (g_desk[i].kind == DK_TRASH || g_desk[i].kind == DK_DIR || (g_desk[i].kind == DK_APP && g_dnd_src == GUI_DRAG_FILES))) d = i;
            for (int k = 0; k < g_ndnd_icons; k++) if (g_dnd_icons[k] == d) d = -1;
        }
        if (d != g_desk_drop) g_desk_drop = d;
        g_force_redraw = 1;
        return;
    }
    if (g_dband) {
        if (!left) { g_dband = 0; g_force_redraw = 1; return; }
        if (mx != g_dband_x1 || my != g_dband_y1) {
            g_dband_x1 = mx;
            g_dband_y1 = my;
            int x0 = g_dband_x0 < mx ? g_dband_x0 : mx, x1 = g_dband_x0 < mx ? mx : g_dband_x0;
            int y0 = g_dband_y0 < my ? g_dband_y0 : my, y1 = g_dband_y0 < my ? my : g_dband_y0;
            g_desk_sel = -1;
            for (int i = 0; i < g_ndesk; i++) {
                int x, y;
                desk_cell(fi, i, &x, &y);
                int hit = x + 8 < x1 && x + CELL_W - 8 > x0 && y + 2 < y1 && y + CELL_H - 6 > y0;
                if (hit) { g_desk_mark[i] = 1; if (g_desk_sel < 0) g_desk_sel = i; }
                else if (!(keyboard_mods() & 2)) g_desk_mark[i] = 0;
            }
            g_force_redraw = 1;
        }
        return;
    }
    if (g_dpress >= 0) {
        if (!left) {
            if (g_dpress_collapse) { g_desk_marks_clear(); g_desk_sel = g_dpress; g_force_redraw = 1; }
            g_dpress = -1;
            return;
        }
        if ((mx - g_dpress_x) * (mx - g_dpress_x) + (my - g_dpress_y) * (my - g_dpress_y) > 36) {
            /* a drag of the selected icons (their files go along to Files / folders / the bin) */
            static char p[64][FS_PATH_LEN];
            int n = desk_marked_paths(p, 64);
            gui_drag_begin((const char (*)[FS_PATH_LEN])p, n, GUI_DRAG_DESKTOP);
            g_ndnd_icons = 0;
            for (int i = 0; i < g_ndesk; i++) if (desk_marked(i)) g_dnd_icons[g_ndnd_icons++] = i;
            g_dnd_from_col = g_desk[g_dpress].col;
            g_dnd_from_row = g_desk[g_dpress].row;
            g_dpress = -1;
        }
    }
}

static void dnd_draw(const fb_info_t* fi, int mx, int my) {
    (void)fi;
    /* the desktop's selection box */
    if (g_dband) {
        int x0 = g_dband_x0 < g_dband_x1 ? g_dband_x0 : g_dband_x1, x1 = g_dband_x0 < g_dband_x1 ? g_dband_x1 : g_dband_x0;
        int y0 = g_dband_y0 < g_dband_y1 ? g_dband_y0 : g_dband_y1, y1 = g_dband_y0 < g_dband_y1 ? g_dband_y1 : g_dband_y0;
        int stride, tw, th;
        uint32_t* t = fb_target(&stride, &tw, &th);
        for (int y = y0; t && y < y1 && y < th; y++)
            for (int x = x0 < 0 ? 0 : x0; x < x1 && x < tw; x++) {
                uint32_t p = t[y * stride + x];
                t[y * stride + x] = ((p & 0x00FEFEFEu) >> 1) + ((0x006FA8E8u & 0x00FEFEFEu) >> 1);
            }
        gfx_fill_rect(x0, y0, x1 - x0, 1, 0x0099CCFFu); gfx_fill_rect(x0, y1 - 1, x1 - x0, 1, 0x0099CCFFu);
        gfx_fill_rect(x0, y0, 1, y1 - y0, 0x0099CCFFu); gfx_fill_rect(x1 - 1, y0, 1, y1 - y0, 0x0099CCFFu);
    }
    if (!g_dnd_on) return;
    /* what is dragged, beside the pointer */
    int n = g_dnd_src == GUI_DRAG_DESKTOP ? g_ndnd_icons : g_ndnd;
    int x = mx + 14, y = my + 14;
    char t[64];
    int copy = (keyboard_mods() & 2) != 0;
    if (g_ndnd && (g_desk_drop >= 0 && g_desk[g_desk_drop].kind == DK_TRASH)) ksnprintf(t, sizeof(t), "Delete %d", g_ndnd);
    else if (n == 1) ksnprintf(t, sizeof(t), "%s%s", copy ? "+ " : "", g_ndnd ? fileops_base(g_dnd[0]) : g_desk[g_dnd_icons[0]].label);
    else ksnprintf(t, sizeof(t), "%s%d items", copy ? "+ " : "", n);
    if (strlen(t) > 22) { t[20] = '.'; t[21] = '.'; t[22] = 0; }
    int w = 44 + (int)strlen(t) * 8;
    gfx_fill_rect(x, y, w, 38, 0x002A3A52u);
    gfx_fill_rect(x, y, w, 1, 0x006B8AB8u);
    gfx_fill_rect(x, y + 37, w, 1, 0x00101820u);
    fileicon_t k = g_ndnd ? (fileops_is_dir(g_dnd[0]) ? FI_FOLDER : fileicon_for_name(fileops_base(g_dnd[0]))) : FI_PROGRAM;
    fileicon_draw(k, x + 4, y + 3, 32);
    gfx_draw_text(x + 40, y + 15, t, 0x00FFFFFFu, 0x002A3A52u);
}

/* ══ apps pinned to the taskbar ═══════════════════════════════════════ */

static void pins_save(void) {
    char v[PIN_MAX * (PKG_NAME_MAX + 4)];
    int o = 0;
    v[0] = 0;
    for (int i = 0; i < g_npins; i++) {
        if (g_pins[i].act == ACT_RUN_APP) o += ksnprintf(v + o, sizeof(v) - (size_t)o, "%sp%s", i ? "," : "", g_pins[i].app);
        else o += ksnprintf(v + o, sizeof(v) - (size_t)o, "%sa%d", i ? "," : "", g_pins[i].act);
    }
    settings_set("taskbar_pins", g_npins ? v : "none");
}

static void pins_load(void) {
    g_npins = 0;
    char v[PIN_MAX * (PKG_NAME_MAX + 4)];
    if (!settings_get("taskbar_pins", v, sizeof(v))) {
        /* like Windows 7: the browser and Files to begin with */
        g_pins[0].act = ACT_BROWSER; g_pins[0].app[0] = 0;
        g_pins[1].act = ACT_FILES; g_pins[1].app[0] = 0;
        g_npins = 2;
        char s[16];
        if (settings_get("taskbar_style", s, sizeof(s))) { uint32_t n = 0; if (k_parse_u32(s, &n) && n <= 2) g_tb_style = (int)n; }
        return;
    }
    char* p = v;
    while (*p && g_npins < PIN_MAX) {
        char* e = strchr(p, ',');
        if (e) *e = 0;
        if (p[0] == 'a') { uint32_t a = 0; if (k_parse_u32(p + 1, &a)) { g_pins[g_npins].act = (int)a; g_pins[g_npins].app[0] = 0; g_npins++; } }
        else if (p[0] == 'p' && p[1]) { g_pins[g_npins].act = ACT_RUN_APP; kstrlcpy(g_pins[g_npins].app, p + 1, PKG_NAME_MAX); g_npins++; }
        if (!e) break;
        p = e + 1;
    }
    char s[16];
    if (settings_get("taskbar_style", s, sizeof(s))) { uint32_t n = 0; if (k_parse_u32(s, &n) && n <= 2) g_tb_style = (int)n; }
}

static int pin_find(int act, const char* app) {
    if (g_npins < 0) pins_load();
    for (int i = 0; i < g_npins; i++)
        if (g_pins[i].act == act && (act != ACT_RUN_APP || !strcmp(g_pins[i].app, app ? app : ""))) return i;
    return -1;
}

static void pin_add(int act, const char* app) {
    if (pin_find(act, app) >= 0 || g_npins >= PIN_MAX) return;
    g_pins[g_npins].act = act;
    kstrlcpy(g_pins[g_npins].app, act == ACT_RUN_APP && app ? app : "", PKG_NAME_MAX);
    g_npins++;
    pins_save();
    g_tb_gen++;
    g_force_redraw = 1;
}

static void pin_remove(int i) {
    if (i < 0 || i >= g_npins) return;
    for (int k = i; k < g_npins - 1; k++) g_pins[k] = g_pins[k + 1];
    g_npins--;
    pins_save();
    g_tb_gen++;
    g_force_redraw = 1;
}

static int pin_at(int mx, int my) {
    const fb_info_t* fi = fb_info();
    if (!fi || g_npins <= 0) return -1;
    int bar_y = (int)fi->height - BAR_H;
    if (my < bar_y + 2 || mx < PINS_X) return -1;
    int i = (mx - PINS_X) / PIN_W;
    return i < g_npins ? i : -1;
}

static void pin_open(int i) {
    if (i < 0 || i >= g_npins) return;
    if (g_pins[i].act == ACT_RUN_APP) run_app(g_pins[i].app);
    else do_action(g_pins[i].act);
}

/* the program a window belongs to (what "Pin to taskbar" pins) */
static int win_pin_target(int h, int* act, char* app, int cap) {
    int k = h >> 8, i = h & 0xFF;
    app[0] = 0;
    if (k == WK_TERM) { *act = ACT_TERMINAL; return 1; }
    if (k == WK_APP) {
        static const int MAP[APP_COUNT] = { ACT_FILES, ACT_BROWSER, ACT_NOTEPAD, ACT_APPS, ACT_TASKMGR, ACT_SETTINGS, -1, -1 };
        *act = i < APP_COUNT ? MAP[i] : -1;
        return *act >= 0;
    }
    if (k == WK_APPWIN) {
        /* an installed app's window: the app whose title it starts with */
        char t[64];
        win_title(h, t, sizeof(t));
        int n = pkg_list(g_pkgs, DESK_MAX);
        for (int j = 0; j < n && j < DESK_MAX; j++) {
            const char* tt = g_pkgs[j].title[0] ? g_pkgs[j].title : g_pkgs[j].name;
            size_t l = strlen(tt);
            if (l && (!strncmp(t, tt, l) || strstr(t, tt))) { *act = ACT_RUN_APP; kstrlcpy(app, g_pkgs[j].name, (size_t)cap); return 1; }
        }
    }
    return 0;
}

/* is a pinned program's window open? (it gets a line under its icon) */
static int pin_running(int i) {
    sync_taskbar();
    for (int j = 0; j < g_ntb; j++) {
        int act;
        char app[PKG_NAME_MAX];
        if (win_pin_target(g_tb[j], &act, app, sizeof(app)) && act == g_pins[i].act &&
            (act != ACT_RUN_APP || !strcmp(app, g_pins[i].app))) return 1;
    }
    return 0;
}

static void pins_draw(const fb_info_t* fi) {
    if (g_npins < 0) pins_load();
    int bar_y = (int)fi->height - BAR_H;
    for (int i = 0; i < g_npins; i++) {
        int x = PINS_X + i * PIN_W;
        int run = pin_running(i);
        uint32_t bg = 0x00192026u;
        if (run) gfx_fill_rect(x + 4, bar_y + BAR_H - 4, PIN_W - 8, 2, 0x0068A8F0u);
        int saved = g_is;
        g_is = 1;
        desk_icon(g_pins[i].act, x + (PIN_W - 14) / 2 - 2, bar_y + 7, g_pins[i].app);
        g_is = saved;
        (void)bg;
    }
    if (g_npins) gfx_fill_rect(PINS_X + g_npins * PIN_W + 1, bar_y + 6, 1, BAR_H - 12, 0x004F5A6Eu);
}

void gui_set_taskbar_style(int style) {
    if (style < 0 || style > 2) style = 0;
    g_tb_style = style;
    char v[4];
    ksnprintf(v, sizeof(v), "%d", style);
    settings_set("taskbar_style", v);
    g_tb_gen++;
    g_force_redraw = 1;
}
int gui_taskbar_style(void) { if (g_npins < 0) pins_load(); return g_tb_style; }
