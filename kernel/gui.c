/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "vcons.h"
#include "crash.h"
#include "i18n.h"
#include "theme.h"
#include "boottime.h"
#include "acpi_sleep.h"
#include "gui.h"
#include "notification.h"
#include "serial.h"
#include "terminal.h"
#include "timer.h"
#include "sysinfo.h"
#include "keyboard.h"
#include "gfx.h"
#include "fb.h"
#include "rtc.h"
#include "usb.h"
#include "vmtools.h"
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
#include "screenshot.h"
static int g_screenshot_requested;
#include "settings.h"
#include "nettray.h"
#include "shortcut.h"
#include "launcher.h"
#include "taskmgr.h"
#include "devmgr.h"
#include "diskmgr.h"
#include "settings.h"
#include "pkg.h"
#include "image.h"
#include "gpu.h"
#include "audio.h"
#include "app.h"
#include "../net/net.h"
#include "../shell/shell.h"
#include "startmenu.h"
#include "emojipick.h"
#include "calpanel.h"
#include "font.h"
#include "fileicons.h"

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

#define TASKBAR_ROW (VGA_HEIGHT - 1)

/* The Start menu, Windows 95 style: Programs (a submenu of the installed
 * apps), the built-in programs, and Shut down (a submenu: Restart, Shut
 * down, Lock, Exit to shell). */
/* (About and Wallpaper live in Settings now: reached from the desktop's menu) */
enum { ACT_TERMINAL = 0, ACT_FILES, ACT_BROWSER, ACT_NOTEPAD, ACT_APPS, ACT_TASKMGR, ACT_SETTINGS, ACT_QUIT, ACT_ABOUT,
       ACT_WALLPAPER, ACT_LOCK, ACT_INSTALL, ACT_PROGRAMS, ACT_POWER, ACT_RESTART, ACT_SHUTDOWN, ACT_RUN_APP, ACT_SLEEP, ACT_DEVMGR, ACT_DISKMGR };
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
    int      link;                      /* a shortcut (a .link file): what it opens, SC_* (kernel/shortcut.h) */
    char     target[FS_PATH_LEN];
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
enum { APP_FILES = 0, APP_BROWSER, APP_NOTEPAD, APP_LAUNCHER, APP_TASKMGR, APP_SETTINGS, APP_INSTALLER, APP_DEVMGR, APP_DISKMGR, APP_APPWIN, APP_COUNT };
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
    { devmgr_is_open, devmgr_draw, devmgr_contains, devmgr_click, devmgr_mouse, devmgr_signature, devmgr_close, devmgr_rclick, "Device Manager" },
    { diskmgr_is_open, diskmgr_draw, diskmgr_contains, diskmgr_click, diskmgr_mouse, diskmgr_signature, diskmgr_close, diskmgr_rclick, "Disk Management" },
    { appwin_is_open, appwin_draw, appwin_contains, appwin_click, appwin_mouse, appwin_signature, appwin_close_all, appwin_rclick, "App" },
};
static int g_front_app = -1;                        /* -1: a terminal window is in front */
static int g_app_order[APP_COUNT] = { APP_FILES, APP_BROWSER, APP_NOTEPAD, APP_LAUNCHER, APP_TASKMGR, APP_SETTINGS, APP_INSTALLER, APP_DEVMGR, APP_DISKMGR, APP_APPWIN };   /* back to front */
static int g_app_min[APP_COUNT];                    /* minimized to the taskbar */

/* ── the window manager's state (see "the window manager" below) ──
 * Every window - terminals, the built-in apps, the installed apps' windows
 * as one layer - is in ONE stacking order (g_z), on one of NDESK virtual
 * desktops, and may be kept above the others (always on top). */
#define NDESK 4
static int     g_cur_desk;
static uint8_t g_app_desk[APP_COUNT], g_app_top[APP_COUNT];
static void    z_raise(int h);
static int     g_force_redraw;              /* (defined with the view below) */

/* open, not minimized, and on the desktop shown (the app windows minimize one by one) */
static int app_visible(int a) {
    if (a == APP_APPWIN) return appwin_any_visible();
    return g_apps[a].is_open() && !g_app_min[a] && g_app_desk[a] == g_cur_desk;
}

static void raise_app(int a) {
    int pos = 0;
    while (pos < APP_COUNT && g_app_order[pos] != a) pos++;
    for (int i = pos; i < APP_COUNT - 1; i++) g_app_order[i] = g_app_order[i + 1];
    g_app_order[APP_COUNT - 1] = a;
    g_front_app = a;
    z_raise(2 << 8 | a);                            /* (WK_APP) */
}

/* the topmost app window at a point, -1 if none (with_front: also the one above the terminals) */
/* the mouse wheel, per app window (NULL: it does not scroll) */
static void (*const g_app_wheel[APP_COUNT])(int mx, int my, int dz) = {
    explorer_wheel, browser_wheel, notepad_wheel, NULL, taskmgr_wheel, settings_wheel, NULL, devmgr_wheel, diskmgr_wheel, appwin_wheel,
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
    int snapped;                  /* in a half / quarter of the screen (sx.. restore it) */
    int px, py;                   /* where a drag started */
    int desk, top;                /* its virtual desktop; always on top */
} term_win_t;

/* shown: open, not minimized, on the desktop shown */
#define TERM_SHOWN(t) ((t)->open && !(t)->minimized && (t)->desk == g_cur_desk)

#define TERM_WIN_MAX 4
static term_win_t g_terms[TERM_WIN_MAX] = {
    {.x = 140, .y = 90, .w = 520, .h = 340},
    {.x = 180, .y = 120, .w = 520, .h = 340},
    {.x = 220, .y = 150, .w = 520, .h = 340},
    {.x = 260, .y = 180, .w = 520, .h = 340},
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

/* a screen with a chip in front */
static void draw_icon_devmgr(int x, int y, uint32_t bg) {
    (void)bg;
    icon_bevel(x, y, 14, 10, 0x00C8D2E0u, 0x00EEF2F8u, 0x00566477u);
    IR(2, 2, 10, 6, 0x002F7FD0u);
    IR(5, 10, 4, 2, 0x00566477u);
    IR(3, 12, 8, 2, 0x00566477u);
    IR(7, 6, 7, 7, 0x00263241u);
    IR(9, 8, 3, 3, 0x0057B65Au);
}

/* a disk drive with its partitions */
static void draw_icon_diskmgr(int x, int y, uint32_t bg) {
    (void)bg;
    icon_bevel(x, y + 2, 14, 10, 0x00C8D2E0u, 0x00EEF2F8u, 0x00566477u);
    IR(2, 4, 6, 4, 0x00203F9Au);
    IR(9, 4, 3, 4, 0x00101010u);
    IR(2, 9, 2, 1, 0x003CC35Au);
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
    icon_bevel(x, y, 12, 12, TC(0x412A2A), TC(0x764A4A), TC(0x170D0D));
    gfx_draw_text_scaled(x + 3 * g_is, y + 2 * g_is, g_is, "o", TC(0xFFFFFF), TC(0x412A2A));
}

/* Programs: a folder with app tiles in it */
static __attribute__((unused)) void draw_icon_programs(int x, int y, uint32_t bg) {
    (void)bg;
    IR(0, 1, 6, 2, TC(0xD9B44A));
    IR(0, 3, 14, 10, TC(0xD9B44A));
    IR(2, 5, 4, 3, TC(0x3A7BD5));
    IR(8, 5, 4, 3, TC(0x57B65A));
    IR(2, 9, 4, 3, TC(0xE07040));
    IR(8, 9, 4, 3, TC(0xF4F8FF));
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
    const uint32_t* chars;
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
    const uint32_t* chars;
    const uint8_t* cols;
    int tw, th, stride;
    terminal_vt_get_buffer(w->vt, &chars, &cols, &tw, &th, &stride);
    int r0 = w->s_r0, c0 = w->s_c0, r1 = w->s_r1, c1 = w->s_c1;
    if (r1 < r0 || (r1 == r0 && c1 < c0)) { int t = r0; r0 = r1; r1 = t; t = c0; c0 = c1; c1 = t; }
    static char out[100 * 76 * 4];                 /* (up to 4 UTF-8 bytes a cell) */
    uint32_t n = 0;
    for (int r = r0; r <= r1 && r < th; r++) {
        int a = r == r0 ? c0 : 0, b = r == r1 ? c1 : tw - 1;
        uint32_t line_start = n;
        for (int c = a; c <= b && c < tw; c++) {
            uint32_t ch = chars[r * stride + c];
            if (ch == TERMINAL_WIDE_CONT) continue;             /* (a wide character's second cell) */
            if (ch >= 0x80) n += (uint32_t)u8_encode(ch, out + n);
            else out[n++] = ch ? (char)ch : ' ';
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
    z_raise(1 << 8 | idx);                          /* (WK_TERM) */
}

/* the frontmost open terminal window at a point, as index + 1 (0 = none) */
static int term_at(int mx, int my) {
    for (int oi = TERM_WIN_MAX - 1; oi >= 0; oi--) {
        term_win_t* w = &g_terms[g_term_order[oi]];
        if (TERM_SHOWN(w) && mx >= w->x && mx < w->x + w->w && my >= w->y && my < w->y + w->h) return g_term_order[oi] + 1;
    }
    return 0;
}

static int bash_terminal(const char* cmd);

static void draw_terminal_window(const fb_info_t* fi, const term_win_t* win) {
    if (!win || !TERM_SHOWN(win)) return;
    if (!fi) return;

    term_win_t w = *win;
    clamp_win(fi, &w);

    int title_h = 20;
    int pad = 6;

    int active = (g_term_order[TERM_WIN_MAX - 1] >= 0) && (&g_terms[g_term_order[TERM_WIN_MAX - 1]] == win);
    uint32_t frame = active ? TC(0x1A1E24) : TC(0x13151A);
    uint32_t body = active ? TC(0x262A31) : TC(0x202429);
    uint32_t title = active ? TC(0x3C4B66) : TC(0x273140);
    uint32_t title_hi = active ? TC(0x6B7892) : TC(0x4B5568);
    uint32_t title_lo = active ? TC(0x111824) : TC(0x10151F);

    /* frame + title (stronger Fluxbox-like bevel) */
    draw_bevel_box(w.x, w.y, w.w, w.h, frame, TC(0x4A5466), TC(0x0E1116));
    gfx_fill_rect(w.x + 2, w.y + 2, w.w - 4, w.h - 4, body);
    draw_bevel_box(w.x + 3, w.y + 3, w.w - 6, title_h - 1, title, title_hi, title_lo);
    gfx_draw_text(w.x + 10, w.y + 7, _("Terminal"), active ? TC(0xF2F7FF) : TC(0xCED8E6), title);

    /* minimize, maximize / restore, close */
    win_draw_button_row(w.x + w.w, w.y + 4, 12, 1, w.maxed);

    /* client area */
    int cx = w.x + pad;
    int cy = w.y + title_h + pad;
    int cw = w.w - pad * 2;
    int ch = w.h - title_h - pad * 2;
    gfx_fill_rect(cx, cy, cw, ch, 0x00000000u);

    const uint32_t* chars;
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
            uint32_t c = chars[idx];
            if (c == TERMINAL_WIDE_CONT) continue;              /* (drawn across both cells by the first) */
            if (term_cell_selected(win, y + row_off, x)) {     /* selection: inverted */
                gfx_draw_cell_cp(cx + x * CW, cy + y * CH, c ? c : ' ', TC(0x101010), TC(0xC8D8F0));
                continue;
            }
            /* the client area is already black: skip blank black cells */
            if ((c == ' ' || c == 0) && (color & 0xF0) == 0) continue;
            gfx_draw_cell_cp(cx + x * CW, cy + y * CH, c,
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
            int cells = (int)cc + 1 < tw && chars[idx + 1] == TERMINAL_WIDE_CONT ? 2 : 1;
            gfx_fill_rect(cx + (int)cc * CW, cy + scr_row * CH + CH - 1, CW * cells, 1, vga_color_rgb(fg));
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


static void do_action(int act);

static void notification_action(int action, void* ctx) {
    (void)ctx;
    if (action >= ACT_TERMINAL && action <= ACT_RUN_APP && action != ACT_RUN_APP)
        do_action(action);
}

void gui_init(void) {
    notify_init();
    notify_set_action_callback(notification_action, NULL);
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
 * (up to 2560x1600 - 800x600 if there is not room for that), from the
 * heap: they are given back by stopx. */
#define DESK_MAX_W 2560u
#define DESK_MAX_H 1600u
static uint32_t* g_desktop_backbuf;        /* from the heap while the desktop runs */
static uint32_t* g_wallpaper_cache;
static uint32_t  g_desk_w, g_desk_h;
static int       g_backbuf_active = 0;

/* Cache of the rendered wallpaper: rendering it (bilinear upscale of a
 * preset, or copying a decoded user picture) only happens when the
 * wallpaper actually changes, never per frame. */
static uint32_t g_wallpaper_cache_gen = 0;

static int size_desktop(const fb_info_t* fi) {
    uint32_t w = fi->width > DESK_MAX_W ? DESK_MAX_W : fi->width;
    uint32_t h = fi->height > DESK_MAX_H ? DESK_MAX_H : fi->height;
    if (g_desktop_backbuf && w == g_desk_w && h == g_desk_h) return 0;
    for (int tries = 0; tries < 2; tries++) {
        uint32_t* bb = (uint32_t*)kmalloc(w * h * 4);
        uint32_t* wc = bb ? (uint32_t*)kmalloc(w * h * 4) : NULL;
        if (bb && wc) {
            kfree(g_desktop_backbuf);
            kfree(g_wallpaper_cache);
            g_desktop_backbuf = bb;
            g_wallpaper_cache = wc;
            g_desk_w = w;
            g_desk_h = h;
            g_wallpaper_cache_gen = (uint32_t)-1;          /* (a new size: redrawn at it) */
            return 0;
        }
        kfree(bb);
        /* not enough room: 800x600 (or less) at most */
        if (w <= 800 && h <= 600) break;
        if (w > 800) w = 800;
        if (h > 600) h = 600;
    }
    return g_desktop_backbuf ? 0 : -1;
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
            if (ch == 'B') fb_putpixel_direct(x + c, y + r, TC(0x000000));
            else if (ch == 'W') fb_putpixel_direct(x + c, y + r, TC(0xFFFFFF));
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
                if (resize_white(x, y)) { fb_putpixel_direct(mx - 7 + x, my - 7 + y, TC(0xFFFFFF)); continue; }
                int edge = 0;
                for (int dy = -1; dy <= 1 && !edge; dy++)
                    for (int dx = -1; dx <= 1; dx++) if (resize_white(x + dx, y + dy)) { edge = 1; break; }
                if (edge) fb_putpixel_direct(mx - 7 + x, my - 7 + y, TC(0x000000));
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
    if(appwin_capture_pending()) { hw_cursor_hide();return; }
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
    if(crash_preview_active() || crash_in_progress() || appwin_capture_pending())return;
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
        if (!TERM_SHOWN(w)) continue;
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
        if (TERM_SHOWN(&g_terms[wi])) return WK_TERM << 8 | wi;
    }
    return 0;
}

static void win_title(int h, char* out, int cap) {
    int k = h >> 8, i = h & 0xFF;
    if (k == WK_TERM) {
        int n = 0;
        for (int j = 0; j <= i; j++) if (g_terms[j].open) n++;
        ksnprintf(out, (size_t)cap, "%s %d", _("Terminal"), n);
    } else if (k == WK_APP) {
        kstrlcpy(out, _(g_apps[i].title), (size_t)cap);
    } else {
        appwin_info(i, out, cap, NULL);
    }
}

/* keeps g_tb in step with the windows that exist */
static int wm_desk_of(int h);

/* (only the windows of the desktop shown: each desktop has its own taskbar) */
static void sync_taskbar(void) {
    int n = 0;
    for (int j = 0; j < g_ntb; j++) if (win_exists(g_tb[j]) && wm_desk_of(g_tb[j]) == g_cur_desk) g_tb[n++] = g_tb[j];
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
        if (wm_desk_of(cand[c]) != g_cur_desk) continue;
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

/* ── the window manager ─────────────────────────────────────────────────
 * One stacking order for every window: the terminals, the built-in apps
 * and the installed apps' windows (those as one layer - they keep their own
 * order inside it). A click raises the window under the mouse whatever it
 * is; when the front window closes or is minimized, the next one down gets
 * the keyboard. Windows "always on top" stay above the rest. Each window is
 * on one of NDESK virtual desktops; only the shown desktop's windows are
 * drawn, listed on the taskbar and given the mouse. Windows snap to halves,
 * quarters and the whole screen (kernel/winframe.c), by dragging or with
 * Win+arrows, and a right-click on a title bar gives the window's menu. */

#define ZMAX (TERM_WIN_MAX + APP_COUNT)
static int      g_z[ZMAX], g_nz;                 /* back to front: WK_TERM / WK_APP handles */
static uint8_t  g_app_was_open[APP_COUNT];
static uint8_t  g_awin_desk[APPWIN_MAX], g_awin_seen[APPWIN_MAX];
static uint32_t g_desk_osd_until;                /* "Desktop 2" shows until then */
static int      g_desk_osd;
static char     g_osd_text[40];                   /* (another note than "Desktop 2": the keyboard layout) */

static int z_open(int h) {
    int k = h >> 8, i = h & 0xFF;
    if (k == WK_TERM) return i < TERM_WIN_MAX && g_terms[i].open;
    if (k == WK_APP) return i < APP_COUNT && (i == APP_APPWIN ? appwin_is_open() : g_apps[i].is_open());
    return 0;
}

static void z_remove(int h) {
    int n = 0;
    for (int j = 0; j < g_nz; j++) if (g_z[j] != h) g_z[n++] = g_z[j];
    g_nz = n;
}

static void z_raise(int h) {
    if (!h) return;
    z_remove(h);
    if (g_nz < ZMAX) g_z[g_nz++] = h;
}

static int z_shown(int h) {
    int k = h >> 8, i = h & 0xFF;
    if (k == WK_TERM) return TERM_SHOWN(&g_terms[i]);
    if (k == WK_APP) return app_visible(i);
    return 0;
}

static int z_ontop(int h) {
    int k = h >> 8, i = h & 0xFF;
    return k == WK_TERM ? g_terms[i].top : k == WK_APP ? g_app_top[i] : 0;
}

/* closed windows leave the order, new ones come in on top - on the desktop shown */
static void z_sync(void) {
    int n = 0;
    for (int j = 0; j < g_nz; j++) if (z_open(g_z[j])) g_z[n++] = g_z[j];
    g_nz = n;
    for (int i = 0; i < TERM_WIN_MAX; i++) {
        int h = WK_TERM << 8 | i, have = 0;
        for (int j = 0; j < g_nz; j++) if (g_z[j] == h) have = 1;
        if (g_terms[i].open && !have) z_raise(h);
    }
    for (int a = 0; a < APP_COUNT; a++) {
        int open = z_open(WK_APP << 8 | a);
        if (open && !g_app_was_open[a]) {        /* (re)opened: on this desktop, not on top */
            if (a != APP_APPWIN) g_app_desk[a] = (uint8_t)g_cur_desk;
            g_app_top[a] = 0;
            int have = 0;
            for (int j = 0; j < g_nz; j++) if (g_z[j] == (WK_APP << 8 | a)) have = 1;
            if (!have) z_raise(WK_APP << 8 | a);
        }
        g_app_was_open[a] = (uint8_t)open;
    }
    /* the installed apps' windows: a new one is on the desktop shown */
    for (int id = 0; id < APPWIN_MAX; id++) {
        int used = appwin_info(id, NULL, 0, NULL);
        if (used && !g_awin_seen[id]) g_awin_desk[id] = (uint8_t)g_cur_desk;
        g_awin_seen[id] = (uint8_t)used;
        if (used) appwin_set_hidden(id, g_awin_desk[id] != g_cur_desk);
    }
}

/* the window that should have the keyboard: the top one shown */
static int z_top(void) {
    for (int pass = 1; pass >= 0; pass--)
        for (int j = g_nz - 1; j >= 0; j--)
            if (z_ontop(g_z[j]) == pass && z_shown(g_z[j])) return g_z[j];
    return 0;
}

static void z_focus(void) {
    int h = z_top();
    if ((h >> 8) == WK_APP) {
        int a = h & 0xFF;
        if (g_front_app != a) {
            int pos = 0;
            while (pos < APP_COUNT && g_app_order[pos] != a) pos++;
            for (int i = pos; i < APP_COUNT - 1; i++) g_app_order[i] = g_app_order[i + 1];
            g_app_order[APP_COUNT - 1] = a;
            g_front_app = a;
        }
    } else {
        g_front_app = -1;
        if (h) {                                 /* that terminal is the front one */
            int idx = h & 0xFF, pos = -1;
            for (int i = 0; i < TERM_WIN_MAX; i++) if (g_term_order[i] == idx) pos = i;
            if (pos >= 0 && pos != TERM_WIN_MAX - 1) {
                for (int i = pos; i < TERM_WIN_MAX - 1; i++) g_term_order[i] = g_term_order[i + 1];
                g_term_order[TERM_WIN_MAX - 1] = idx;
            }
        }
    }
}

static int z_contains(int h, int mx, int my) {
    int k = h >> 8, i = h & 0xFF;
    if (k == WK_TERM) {
        term_win_t* w = &g_terms[i];
        return mx >= w->x && mx < w->x + w->w && my >= w->y && my < w->y + w->h;
    }
    return g_apps[i].contains(mx, my);
}

/* the window under the mouse (the ones on top first), 0 if none */
static int z_at(int mx, int my) {
    for (int pass = 1; pass >= 0; pass--)
        for (int j = g_nz - 1; j >= 0; j--) {
            int h = g_z[j];
            if (z_ontop(h) == pass && z_shown(h) && z_contains(h, mx, my)) return h;
        }
    return 0;
}

/* the built-in apps' geometry (their own modules keep it) */
static win_geom_t* app_geom(int a) {
    switch (a) {
    case APP_FILES:     return explorer_geom();
    case APP_BROWSER:   return browser_geom();
    case APP_NOTEPAD:   return notepad_geom();
    case APP_LAUNCHER:  return launcher_geom();
    case APP_TASKMGR:   return taskmgr_geom();
    case APP_SETTINGS:  return settings_geom();
    case APP_INSTALLER: return installer_geom();
    case APP_DEVMGR: return devmgr_geom();
    case APP_DISKMGR: return diskmgr_geom();
    default:            return NULL;
    }
}

/* a window as the menu and the keys know it: an installed app's own window, not its layer */
static int wm_target(int h) {
    if (h == (WK_APP << 8 | APP_APPWIN)) { int id = appwin_front_id(); return id >= 0 ? (WK_APPWIN << 8 | id) : 0; }
    return h;
}

static int fullscreen_active(void) {
    int h=wm_target(win_front()),k=h>>8,i=h&255;
    if(k==WK_APPWIN)return appwin_fullscreen(i);
    win_geom_t* g=k==WK_APP?app_geom(i):NULL;
    return g&&g->fullscreen;
}
static void toggle_max_term(const fb_info_t*, term_win_t*);
static void fullscreen_toggle(void) {
    int h=wm_target(win_front()),k=h>>8,i=h&255;
    if(k==WK_APPWIN)appwin_toggle_fullscreen(i);
    else if(k==WK_APP){win_geom_t* g=app_geom(i);if(g)win_toggle_fullscreen(g);}
    else if(k==WK_TERM)toggle_max_term(fb_info(),&g_terms[i]);
    g_menu_open=0;ctxmenu_close();cal_close();nettray_close();g_force_redraw=1;
}

/* maximized / snapped */
static void wm_state(int h, int* maxed, int* snapped) {
    int k = h >> 8, i = h & 0xFF;
    *maxed = *snapped = 0;
    if (k == WK_TERM) { *maxed = g_terms[i].maxed; *snapped = g_terms[i].snapped; }
    else if (k == WK_APP) { win_geom_t* g = app_geom(i); if (g) { *maxed = g->maxed; *snapped = g->snapped; } }
    else if (k == WK_APPWIN) appwin_state(i, maxed, snapped, NULL);
}

static void term_snap(const fb_info_t* fi, term_win_t* w, const win_rect_t* r, int where) {
    if (!w->maxed && !w->snapped) { w->sx = w->x; w->sy = w->y; w->sw = w->w; w->sh = w->h; }
    w->x = r->x; w->y = r->y; w->w = r->w; w->h = r->h;
    w->maxed = where == SNAP_MAX;
    w->snapped = where == SNAP_HALF;
    w->dragging = w->resizing = 0;
    clamp_win(fi, w);
    term_apply_size(w);
}

static void term_unsnap(const fb_info_t* fi, term_win_t* w) {
    if (!w->maxed && !w->snapped) return;
    w->x = w->sx; w->y = w->sy; w->w = w->sw; w->h = w->sh;
    w->maxed = w->snapped = 0;
    clamp_win(fi, w);
    term_apply_size(w);
}

/* puts window h at a named place ('l', 'r', '1'-'4', 'm'; 0: back to its own size) */
static void wm_place(int h, char where) {
    const fb_info_t* fi = fb_info();
    if (!fi) return;
    h = wm_target(h);
    win_rect_t r;
    int z = where == 'm' ? SNAP_MAX : SNAP_HALF;
    if (where) winframe_snap_rect(where, &r);
    int k = h >> 8, i = h & 0xFF;
    if (k == WK_TERM) {
        if (where) term_snap(fi, &g_terms[i], &r, z); else term_unsnap(fi, &g_terms[i]);
    } else if (k == WK_APP) {
        win_geom_t* g = app_geom(i);
        if (g) { if(g->fullscreen)win_toggle_fullscreen(g); if (where) win_snap(g, &r, z); else win_unsnap(g); }
    } else if (k == WK_APPWIN) {
        appwin_snap(i, &r, where ? z : SNAP_NONE);
    }
    g_force_redraw = 1;
}

/* the window's virtual desktop / always-on-top flag */
static int wm_desk_of(int h) {
    int k = h >> 8, i = h & 0xFF;
    if (k == WK_TERM) return g_terms[i].desk;
    if (k == WK_APP) return i == APP_APPWIN ? g_cur_desk : g_app_desk[i];
    if (k == WK_APPWIN) return g_awin_desk[i];
    return g_cur_desk;
}

static void wm_to_desk(int h, int d) {
    int k = h >> 8, i = h & 0xFF;
    if (k == WK_TERM) g_terms[i].desk = d;
    else if (k == WK_APP && i != APP_APPWIN) g_app_desk[i] = (uint8_t)d;
    else if (k == WK_APPWIN) { g_awin_desk[i] = (uint8_t)d; appwin_set_hidden(i, d != g_cur_desk); }
    g_tb_gen++;
    g_force_redraw = 1;
}

static void wm_set_top(int h, int on) {
    int k = h >> 8, i = h & 0xFF;
    if (k == WK_TERM) g_terms[i].top = on;
    else if (k == WK_APP || k == WK_APPWIN) g_app_top[k == WK_APP ? i : APP_APPWIN] = (uint8_t)on;
    g_force_redraw = 1;
}

static int wm_top_of(int h) {
    int k = h >> 8, i = h & 0xFF;
    if (k == WK_TERM) return g_terms[i].top;
    return g_app_top[k == WK_APP ? i : APP_APPWIN];
}

static void desk_switch(int d) {
    if (d < 0 || d >= NDESK || d == g_cur_desk) return;
    g_cur_desk = d;
    for (int id = 0; id < APPWIN_MAX; id++)
        if (appwin_info(id, NULL, 0, NULL)) appwin_set_hidden(id, g_awin_desk[id] != d);
    g_desk_osd = 1;
    g_desk_osd_until = timer_ms() + 1200;
    g_menu_open = 0;
    g_tb_gen++;
    g_force_redraw = 1;
    z_focus();
}

/* how many windows are on desktop d (the switcher's dots) */
static int desk_count(int d) {
    int n = 0;
    for (int i = 0; i < TERM_WIN_MAX; i++) if (g_terms[i].open && g_terms[i].desk == d) n++;
    for (int a = 0; a < APP_APPWIN; a++) if (g_apps[a].is_open() && g_app_desk[a] == d) n++;
    for (int id = 0; id < APPWIN_MAX; id++) if (appwin_info(id, NULL, 0, NULL) && g_awin_desk[id] == d) n++;
    return n;
}

/* the title bar under the mouse: the window's handle (an installed app's own), 0 if none */
static int wm_title_at(int mx, int my) {
    int h = z_at(mx, my);
    int k = h >> 8, i = h & 0xFF;
    if (k == WK_TERM) return my < g_terms[i].y + TERM_TITLE_H ? h : 0;
    if (k == WK_APP && i == APP_APPWIN) {
        int title, id = appwin_at(mx, my, &title);
        return id >= 0 && title ? (WK_APPWIN << 8 | id) : 0;
    }
    if (k == WK_APP) {
        win_geom_t* g = app_geom(i);
        return g && my < g->y + 24 ? h : 0;
    }
    return 0;
}

/* Alt+drag: the window under the mouse moves, wherever it was taken */
static int wm_alt_drag(int mx, int my) {
    int h = z_at(mx, my);
    int k = h >> 8, i = h & 0xFF;
    if (!h) return 0;
    if (k == WK_TERM) {
        term_win_t* w = &g_terms[i];
        bring_term_front(i);
        g_front_app = -1;
        w->dragging = 1;
        w->drag_dx = mx - w->x;
        w->drag_dy = my - w->y;
        w->px = mx;
        w->py = my;
        return 1;
    }
    if (i == APP_APPWIN) {
        int id = appwin_at(mx, my, NULL);
        if (id < 0) return 0;
        raise_app(APP_APPWIN);
        appwin_start_drag(id, mx, my);
        return 1;
    }
    win_geom_t* g = app_geom(i);
    if (!g) return 0;
    raise_app(i);
    if(g->fullscreen)return 1;
    g->dragging = 1;
    g->dx = mx - g->x;
    g->dy = my - g->y;
    g->px = mx;
    g->py = my;
    return 1;
}

/* the window menu: a right-click on a title bar */
enum { WM_RESTORE = 300, WM_MIN, WM_MAX, WM_LEFT, WM_RIGHT, WM_TOP, WM_DESK0, WM_CLOSE = WM_DESK0 + NDESK };
static int  g_wmenu_h;
static char g_wmenu_desk[NDESK][24];

static void wm_menu_cb(int id, void* arg) {
    (void)arg;
    int h = g_wmenu_h;
    if (!win_exists(h)) return;
    switch (id) {
    case WM_RESTORE: wm_place(h, 0); break;
    case WM_MIN:     win_minimize(h); break;
    case WM_MAX:     wm_place(h, 'm'); break;
    case WM_LEFT:    wm_place(h, 'l'); break;
    case WM_RIGHT:   wm_place(h, 'r'); break;
    case WM_TOP:     wm_set_top(h, !wm_top_of(h)); break;
    case WM_CLOSE:   win_close(h); break;
    default:
        if (id >= WM_DESK0 && id < WM_DESK0 + NDESK) wm_to_desk(h, id - WM_DESK0);
        break;
    }
    g_force_redraw = 1;
}

static void open_window_menu(int h, int mx, int my) {
    h = wm_target(h);
    if (!h) return;
    g_wmenu_h = h;
    int maxed, snapped;
    wm_state(h, &maxed, &snapped);
    int resizable = 1;
    if ((h >> 8) == WK_APPWIN) appwin_state(h & 0xFF, NULL, NULL, &resizable);
    ctx_item_t items[CTX_MAX_ITEMS];
    int n = 0;
    items[n++] = (ctx_item_t){ "Restore", WM_RESTORE, !(maxed || snapped) };
    items[n++] = (ctx_item_t){ "Minimize", WM_MIN, 0 };
    items[n++] = (ctx_item_t){ "Maximize", WM_MAX, maxed || !resizable };
    items[n++] = (ctx_item_t){ "Snap to the left half", WM_LEFT, !resizable };
    items[n++] = (ctx_item_t){ "Snap to the right half", WM_RIGHT, !resizable };
    items[n++] = (ctx_item_t){ CTX_SEP, 0, 0 };
    items[n++] = (ctx_item_t){ wm_top_of(h) ? "Always on top: on" : "Always on top: off", WM_TOP, 0 };
    for (int d = 0; d < NDESK && n < CTX_MAX_ITEMS - 2; d++) {
        if (d == wm_desk_of(h)) continue;
        ksnprintf(g_wmenu_desk[d], sizeof(g_wmenu_desk[d]), "Move to desktop %d", d + 1);
        items[n++] = (ctx_item_t){ g_wmenu_desk[d], WM_DESK0 + d, 0 };
    }
    items[n++] = (ctx_item_t){ CTX_SEP, 0, 0 };
    items[n++] = (ctx_item_t){ "Close", WM_CLOSE, 0 };
    ctxmenu_open(mx, my, items, n, wm_menu_cb, NULL);
    g_force_redraw = 1;
}

/* Win+Left / Win+Right / Win+Up / Win+Down on the front window, as in Windows */
static void wm_arrow(int c) {
    int h = wm_target(win_front());
    if (!h) return;
    int maxed, snapped;
    wm_state(h, &maxed, &snapped);
    win_rect_t a;
    winframe_work_area(&a);
    int left_half = 0, k = h >> 8, i = h & 0xFF;
    if (k == WK_TERM) left_half = g_terms[i].x == a.x;
    else if (k == WK_APP) { win_geom_t* g = app_geom(i); left_half = g && g->x == a.x; }
    else { int x, y, w, hh; if (appwin_get_rect(i, &x, &y, &w, &hh)) left_half = x == a.x; }
    if (c == KEYW_LEFT) {
        if (snapped && !left_half) wm_place(h, 0);       /* the right half: back to its own place */
        else if (!snapped || !left_half) wm_place(h, 'l');
    } else if (c == KEYW_RIGHT) {
        if (snapped && left_half) wm_place(h, 0);
        else if (!snapped || left_half) wm_place(h, 'r');
    } else if (c == KEYW_UP) {
        wm_place(h, 'm');
    } else if (c == KEYW_DOWN) {
        if (maxed || snapped) wm_place(h, 0);
        else win_minimize(h);
    }
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
static int   g_tb_desks = 1;              /* the virtual desktops' buttons shown */
static void  pins_load(void);
#define PINS_X    (START_X + START_W + 6)
static void pins_draw(const fb_info_t* fi);
static int tb_x(void) { if (g_npins < 0) pins_load(); return PINS_X + g_npins * PIN_W + (g_npins ? 6 : 2); }
#define TB_X      (tb_x())
#define TB_BTN_W  (g_tb_style == 1 ? 44 : 150)

/* the network icon (kernel/nettray.c), left of the clock */
#define CLK_W 80                          /* the clock: the time over the date */
static int net_icon_x(const fb_info_t* fi) { return (int)fi->width - 8 - CLK_W - 14 - NETTRAY_ICON_W; }
static int clock_at(const fb_info_t* fi, int mx, int my) {
    return my >= (int)fi->height - BAR_H && mx >= (int)fi->width - 8 - CLK_W - 6;
}
static int net_icon_at(const fb_info_t* fi, int mx, int my) {
    int x = net_icon_x(fi), bar_y = (int)fi->height - BAR_H;
    return my >= bar_y && mx >= x - 4 && mx < x + NETTRAY_ICON_W + 2;
}
static int g_net_hover;

static int tray_x(const fb_info_t* fi) {
    int badge = sysinfo_live_boot() ? 8 * 8 + 18 : 0;     /* "LIVE CD" */
    return net_icon_x(fi) - 6 - badge;
}

/* the virtual desktops' switcher, left of the tray: a box a desktop */
#define DSW_BOX 18
#define DSW_W   (g_tb_desks ? NDESK * (DSW_BOX + 2) + 10 : 0)
static int dsw_x(const fb_info_t* fi) { return tray_x(fi) - DSW_W; }

static int desk_switch_at(const fb_info_t* fi, int mx, int my) {
    int bar_y = (int)fi->height - BAR_H, x0 = dsw_x(fi);
    if (!g_tb_desks) return -1;
    if (my < bar_y + 4 || my >= bar_y + BAR_H - 4 || mx < x0) return -1;
    int d = (mx - x0) / (DSW_BOX + 2);
    return d < NDESK && (mx - x0) % (DSW_BOX + 2) < DSW_BOX ? d : -1;
}

static int desk_count(int d);

static void draw_desk_switcher(const fb_info_t* fi) {
    if (!g_tb_desks) return;                   /* (hidden: Settings > Taskbar) */
    int bar_y = (int)fi->height - BAR_H, x0 = dsw_x(fi);
    for (int d = 0; d < NDESK; d++) {
        int x = x0 + d * (DSW_BOX + 2), y = bar_y + 5, cur = d == g_cur_desk;
        uint32_t base = cur ? TC(0x405478) : TC(0x262D38);
        draw_bevel_box(x, y, DSW_BOX, BAR_H - 10, base, cur ? TC(0x222A36) : TC(0x434D5E), cur ? TC(0x6B7892) : TC(0x15191F));
        char t[2] = { (char)('1' + d), 0 };
        gfx_draw_text(x + 5, y + 5, t, cur ? TC(0xFFFFFF) : TC(0x9AA6B6), base);
        if (desk_count(d)) gfx_fill_rect(x + DSW_BOX / 2 - 2, y + BAR_H - 13, 4, 2, cur ? TC(0x68A8F0) : TC(0x808C9C));
    }
}

static int tb_btn_w(const fb_info_t* fi) {
    int avail = dsw_x(fi) - 12 - TB_X;
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
        else if (i == APP_DEVMGR) draw_icon_devmgr(x, y, bg);
        else if (i == APP_DISKMGR) draw_icon_diskmgr(x, y, bg);
        else draw_icon_taskmgr(x, y, bg);
        return;
    }
    if (k == WK_APPWIN && appwin_is_terminal(i)) {
        draw_icon_terminal(x, y, bg);
        return;
    }
    draw_bevel_box(x, y, 14, 12, 0x00D0A030u, 0x00F0D070u, 0x00604010u);
}

static void draw_taskbar_fb(const fb_info_t* fi) {
    int bar_y = (int)fi->height - BAR_H;
    draw_bevel_box(0, bar_y, (int)fi->width, BAR_H, TC(0x192026), TC(0x4F5A6E), TC(0x10141B));
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
        uint32_t base = active ? TC(0x405478) : min ? TC(0x222831) : TC(0x303740);
        uint32_t hi = active ? TC(0x222A36) : TC(0x535D6E), lo = active ? TC(0x6B7892) : TC(0x15191F);
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
        gfx_draw_text(tx, bar_y + 10, t, min ? TC(0x9AA6B6) : TC(0xE8EEF6), base);
    }
    draw_desk_switcher(fi);
    /* tray: network, clock */
    char clk[9];
    format_clock(clk);
    int clk_x = (int)fi->width - 8 - CLK_W;
    uint32_t cbg = cal_is_open() ? TC(0x303A48) : TC(0x192026);
    gfx_fill_rect(clk_x - 10, bar_y + 6, 1, BAR_H - 12, TC(0x4F5A6E));
    if (cal_is_open()) gfx_fill_rect(clk_x - 6, bar_y + 2, CLK_W + 10, BAR_H - 4, cbg);
    {   /* the time over the date (a click opens the calendar): both placed by
         * the font's own height and kept inside the taskbar, whatever the font */
        rtc_datetime_t dt;
        char d[16];
        if (rtc_read_datetime(&dt) == 0) ksnprintf(d, sizeof(d), "%02u/%02u/%04u", dt.day, dt.month, dt.year);
        else d[0] = 0;
        int stride, tw, th;
        uint32_t* t = fb_target(&stride, &tw, &th);
        int face = font_available(FONT_SANS) ? FONT_SANS : -1;
        if (t && face >= 0) {
            int px = 12, a, dd, lh;
            font_metrics(face, px, &a, &dd, &lh);
            while (px > 8 && 2 * (a + dd) > BAR_H - 2) { px--; font_metrics(face, px, &a, &dd, &lh); }
            int top = bar_y + (BAR_H - 2 * (a + dd)) / 2;
            int w1 = font_text_width(face, px, clk, (uint32_t)strlen(clk)), w2 = font_text_width(face, px, d, (uint32_t)strlen(d));
            int cx = clk_x + CLK_W / 2;
            font_draw(t, stride, clk_x - 6, bar_y + 1, clk_x + CLK_W + 4, bar_y + BAR_H - 1, face, px, cx - w1 / 2, top + a, clk, (uint32_t)strlen(clk), TC(0xE8EEF6), 0);
            font_draw(t, stride, clk_x - 6, bar_y + 1, clk_x + CLK_W + 4, bar_y + BAR_H - 1, face, px, cx - w2 / 2, top + 2 * a + dd, d, (uint32_t)strlen(d), TC(0xB8C2D0), 0);
        } else {
            gfx_draw_text(clk_x + (CLK_W - 64) / 2, bar_y + 5, clk, TC(0xE8EEF6), cbg);
            gfx_draw_text(clk_x, bar_y + 16, d, TC(0xB8C2D0), cbg);
        }
    }
    int net_x = net_icon_x(fi);
    nettray_draw_icon(net_x, bar_y + (BAR_H - 16) / 2, TC(0x192026), g_net_hover || nettray_is_open());
    if (g_net_hover && !nettray_is_open()) {          /* its tooltip: the state in words */
        char tip[64];
        nettray_summary(tip, sizeof(tip));
        int tw = (int)strlen(tip) * 8 + 12, tx = net_x + NETTRAY_ICON_W - tw;
        if (tx < 4) tx = 4;
        draw_bevel_box(tx, bar_y - 26, tw, 22, TC(0x1D232C), TC(0x505E74), TC(0x10151D));
        gfx_draw_text(tx + 6, bar_y - 22, tip, TC(0xE8EEF6), TC(0x1D232C));
    }
    /* started from the CD: say so (nothing is kept unless installed) */
    if (sysinfo_live_boot()) {
        int bx = net_x - 14 - 7 * 8 - 8;
        gfx_fill_rect(bx, bar_y + 5, 7 * 8 + 8, BAR_H - 10, TC(0xD9822B));
        gfx_draw_text(bx + 4, bar_y + 10, _("LIVE CD"), TC(0xFFFFFF), TC(0xD9822B));
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
    uint32_t desk_sig, wm_sig;
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
    v->wp_gen = wallpaper_generation() ^ theme_generation() * 2654435761u ^ i18n_generation() * 40503u;   /* (a new theme: everything again) */
    v->net_state = (net_if()->configured ? net_if()->ip : 1) ^ nettray_sig();
    v->front_app = g_front_app;
    for (int a = 0; a < APP_COUNT; a++) {
        v->app_order[a] = g_app_order[a];
        v->app_sig[a] = g_apps[a].signature();
        v->app_min[a] = g_app_min[a];
    }
    v->ctx_sig = ctxmenu_signature() ^ (g_menu_open ? startmenu_signature() : 0) ^ (uint32_t)g_start_hover * 0x9E37u;
    v->tb_gen = g_tb_gen;
    /* the window manager: the stacking order, desktops, on-top flags, the snap preview */
    uint32_t s = (uint32_t)g_nz * 31u + (uint32_t)g_cur_desk * 7919u + (uint32_t)g_desk_osd;
    for (int j = 0; j < g_nz; j++) s = s * 131u + (uint32_t)g_z[j] * 2u + (uint32_t)z_ontop(g_z[j]);
    win_rect_t pr;
    if (winframe_preview(&pr)) s ^= (uint32_t)(pr.x * 3 + pr.y * 5 + pr.w * 7 + pr.h * 11 + 1) * 2654435761u;
    for (int i = 0; i < TERM_WIN_MAX; i++) s = s * 31u + (uint32_t)g_terms[i].desk;
    v->wm_sig = s;
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
    else ksnprintf(out, (size_t)cap, "file:%s", strrchr(d->path, '/') ? strrchr(d->path, '/') + 1 : d->label);   /* (a shortcut: its file name) */
}

/* Coordinates index a fixed 64-by-32 occupancy table. Parse against the
 * limit before multiplication so long decimal strings cannot wrap. */
static int layout_coord(const char* text, unsigned limit, int* out) {
    unsigned value = 0;
    if (!text[0]) return 0;
    for (const char* p = text; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
        unsigned digit = (unsigned)(*p - '0');
        if (value > (limit - 1u - digit) / 10u) return 0;
        value = value * 10u + digit;
    }
    *out = (int)value;
    return 1;
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
        if (n >= (int)sizeof(line)) {
            if (!nl) break;
            p = nl + 1;
            continue;
        }
        memcpy(line, p, (size_t)n);
        line[n] = 0;
        if (n && line[n - 1] == '\r') line[n - 1] = 0;
        char* t1 = strchr(line, '\t');
        char* t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (t1 && t2) {
            *t1 = 0; *t2 = 0;
            int c, r;
            if (line[0] && strlen(line) < sizeof(g_lay[0].key) &&
                layout_coord(t1 + 1, 64, &c) && layout_coord(t2 + 1, 32, &r)) {
                kstrlcpy(g_lay[g_nlay].key, line, sizeof(g_lay[0].key));
                g_lay[g_nlay].col = c;
                g_lay[g_nlay].row = r;
                g_nlay++;
            }
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
            if (!strcmp(g_lay[k].key, key) && g_lay[k].col >= 0 && g_lay[k].row >= 0 &&
                g_lay[k].col < cols && g_lay[k].row < rows &&
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

/* icons taken off the desktop (programs, the Recycle Bin): their keys in
 * ~/.config/desktop-hidden, one a line. */
#define HIDDEN_FILE "/home/banana/.config/desktop-hidden"
#define HIDDEN_MAX  48
static char g_hidden[HIDDEN_MAX][56];
static int  g_nhidden = -1;

static void hidden_load(void) {
    g_nhidden = 0;
    int fi = fs_find_file(HIDDEN_FILE);
    fs_file_t* f = fi >= 0 ? fs_get_file(fi) : NULL;
    if (!f || !f->content) return;
    for (const char* p = f->content; *p && g_nhidden < HIDDEN_MAX; ) {
        const char* nl = strchr(p, '\n');
        int n = nl ? (int)(nl - p) : (int)strlen(p);
        if (n > 0 && n < 56) { memcpy(g_hidden[g_nhidden], p, (size_t)n); g_hidden[g_nhidden++][n] = 0; }
        if (!nl) break;
        p = nl + 1;
    }
}

static void hidden_save(void) {
    static char buf[HIDDEN_MAX * 57];
    int o = 0;
    for (int i = 0; i < g_nhidden; i++) o += ksnprintf(buf + o, sizeof(buf) - (size_t)o, "%s\n", g_hidden[i]);
    fs_mkdir_p("/home/banana/.config");
    fs_write_path(HIDDEN_FILE, buf, (uint32_t)o);
}

static int is_hidden(const char* key) {
    if (g_nhidden < 0) hidden_load();
    for (int i = 0; i < g_nhidden; i++) if (!strcmp(g_hidden[i], key)) return 1;
    return 0;
}

static void desk_refresh(int force);
static int  builtin_act(const char* name);

/* every icon keeps the cell it has now (an icon going away must not move
 * the others: those never dragged would fill its place) */
static void layout_pin_all(void) {
    for (int i = 0; i < g_ndesk; i++) {
        char key[56];
        desk_key(&g_desk[i], key, sizeof(key));
        layout_set(key, g_desk[i].col, g_desk[i].row);
    }
    layout_save();
}

/* "Remove from desktop" on a program or the Recycle Bin */
static void desk_hide(int i) {
    if (i < 0 || i >= g_ndesk || (g_desk[i].kind != DK_APP && g_desk[i].kind != DK_TRASH)) return;
    char key[56];
    desk_key(&g_desk[i], key, sizeof(key));
    if (is_hidden(key) || g_nhidden >= HIDDEN_MAX) return;
    layout_pin_all();
    kstrlcpy(g_hidden[g_nhidden++], key, sizeof(g_hidden[0]));
    hidden_save();
    g_desk_sel = -1;
    g_desk_marks_clear();
    desk_refresh(1);
}

/* ── the programs on the desktop are shortcuts (.link files in ~/Desktop),
 * as on Windows: deleting one deletes the shortcut, never the program.
 * ~/.config/desktop-apps lists the desktop (graphical) apps already given
 * one: an app installed later gets its shortcut once (as a Windows
 * installer makes one); an app removed takes its shortcuts along. ── */
#define DESK_APPS_FILE "/home/banana/.config/desktop-apps"

static int listed(const char* file, const char* name) {
    int fi = fs_find_file(file);
    fs_file_t* f = fi >= 0 ? fs_get_file(fi) : NULL;
    if (!f || !f->content) return 0;
    size_t n = strlen(name);
    for (const char* p = f->content; (p = strstr(p, name)); p++)
        if ((p == f->content || p[-1] == '\n') && p[n] == '\n') return 1;
    return 0;
}
static void list_add(const char* file, const char* name) {
    char line[PKG_NAME_MAX + 2];
    ksnprintf(line, sizeof(line), "%s\n", name);
    int fi = fs_find_file(file);
    if (fi < 0) { fs_mkdir_p("/home/banana/.config"); fi = fs_create(file); }
    if (fi >= 0) fs_append(fi, line, (uint32_t)strlen(line));
}

/* a shortcut on the desktop to target, named name; its icon takes the cell
 * key had (the program icons of before build 249) */
static void desk_make_link(const char* name, const char* target, const char* key) {
    char p[FS_PATH_LEN];
    if (shortcut_create(DESKTOP_DIR, name, target, p, sizeof(p)) != 0) return;
    if (!key) return;
    if (g_nlay < 0) layout_load();
    for (int k = 0; k < g_nlay; k++)
        if (!strcmp(g_lay[k].key, key)) {
            ksnprintf(g_lay[k].key, sizeof(g_lay[0].key), "file:%s", strrchr(p, '/') + 1);
            layout_save();
            break;
        }
}

/* the shortcuts the desktop's programs need: the first time (the built-in
 * programs and the desktop apps - not the ones taken off it before), then
 * each desktop app installed since; the shortcuts of apps removed go */
static void desk_sync_links(void) {
    static uint32_t last;
    if (last && timer_ms() - last < 2000) return;
    last = timer_ms();
    int first = fs_find_file(DESK_APPS_FILE) < 0;
    if (first) {
        static const char* const PROGS[] = { "Terminal", "Files", "Browser", "Notepad", "Apps", "Task Manager", "Settings" };
        for (int i = 0; i < (int)(sizeof(PROGS) / sizeof(PROGS[0])); i++) {
            char key[56], target[64];
            ksnprintf(key, sizeof(key), "app:%s", _(PROGS[i]));
            if (is_hidden(key)) continue;
            ksnprintf(target, sizeof(target), "app:%s", PROGS[i]);
            desk_make_link(_(PROGS[i]), target, key);
        }
        fs_mkdir_p("/home/banana/.config");
        if (fs_find_file(DESK_APPS_FILE) < 0) fs_create(DESK_APPS_FILE);
    }
    int n = pkg_list(g_pkgs, DESK_MAX);
    for (int i = 0; i < n && i < DESK_MAX; i++) {
        if (!pkg_is_desktop(&g_pkgs[i]) || listed(DESK_APPS_FILE, g_pkgs[i].name)) continue;
        if (!strcmp(g_pkgs[i].name, "terminal")) continue;          /* (the Terminal program is it) */
        const char* label = g_pkgs[i].title[0] ? g_pkgs[i].title : g_pkgs[i].name;
        char key[56], target[PKG_NAME_MAX + 8];
        ksnprintf(key, sizeof(key), "app:%s", label);
        ksnprintf(target, sizeof(target), "app:%s", g_pkgs[i].name);
        if (!(first && is_hidden(key))) desk_make_link(label, target, key);
        list_add(DESK_APPS_FILE, g_pkgs[i].name);
    }
    /* an app gone: its shortcuts too */
    static int fi2[DESK_MAX];
    int nf = fs_list_files(DESKTOP_DIR, fi2, DESK_MAX);
    char canonical[FS_PATH_LEN], canonical_target[FS_PATH_LEN];
    ksnprintf(canonical, sizeof(canonical), "%s/%s.link", DESKTOP_DIR, _("Terminal"));
    int keep_terminal = fs_find_file(canonical);
    if (keep_terminal >= 0 && (shortcut_read(canonical, canonical_target, sizeof(canonical_target)) != SC_APP ||
        builtin_act(canonical_target + 4) != ACT_TERMINAL)) keep_terminal = -1;
    for (int i = 0; i < nf && i < DESK_MAX; i++) {
        fs_file_t* f = fs_file_info(fi2[i]);
        if (!f || !shortcut_is(f->name)) continue;
        char p[FS_PATH_LEN], target[FS_PATH_LEN];
        ksnprintf(p, sizeof(p), "%s/%s", DESKTOP_DIR, f->name);
        if (shortcut_read(p, target, sizeof(target)) != SC_APP) continue;
        int act = builtin_act(target + 4);
        if (act == ACT_TERMINAL) {
            if (keep_terminal < 0) {
                char terminal_path[FS_PATH_LEN];
                if (shortcut_create(DESKTOP_DIR, _("Terminal"), "app:Terminal",
                                    terminal_path, sizeof(terminal_path)) == 0)
                    keep_terminal = fs_find_file(terminal_path);
                if (keep_terminal < 0) keep_terminal = fi2[i];
            }
            if (fi2[i] != keep_terminal) fs_delete(p, 0);
            continue;
        }
        if (act >= 0) continue;
        /* Shortcuts from older shell launchers may outlive their package. */
        if (!strcasecmp(target + 4, "bash") || !strcasecmp(target + 4, "zsh") ||
            !strcasecmp(target + 4, "zshell")) {
            fs_delete(p, 0);
            continue;
        }
        pkg_info_t have;
        int found = pkg_get(target + 4, &have) == 0;
        if ((found && !pkg_is_desktop(&have)) || (!found && listed(DESK_APPS_FILE, target + 4))) fs_delete(p, 0);
    }
}

/* the desktop's items: the Recycle Bin, the Desktop folder's folders and
 * files - the programs' shortcuts among them ("Install Banana OS" on the
 * live CD is the one program shown as itself) */
static void desk_refresh(int force) {
    if (!force && g_ndesk && timer_ms() - g_desk_checked < 2000) return;
    g_desk_checked = timer_ms();
    if (fs_find_dir(DESKTOP_DIR) < 0) fs_mkdir_p(DESKTOP_DIR);
    desk_sync_links();
    g_ndesk = 0;
    if (installer_available()) desk_add(ACT_INSTALL, _("Install Banana OS"), NULL);
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
        if (shortcut_is(f->name)) {                  /* a shortcut: shown by its name, without .link */
            desk_item_t* d = &g_desk[g_ndesk - 1];
            d->link = shortcut_read(p, d->target, sizeof(d->target));
            if (d->link) d->label[strlen(d->label) - (sizeof(SHORTCUT_EXT) - 1)] = 0;
        }
    }
    /* the ones taken off the desktop go */
    int k = 0;
    for (int i = 0; i < g_ndesk; i++) {
        char key[56];
        desk_key(&g_desk[i], key, sizeof(key));
        if ((g_desk[i].kind == DK_APP || g_desk[i].kind == DK_TRASH) && is_hidden(key)) continue;
        if (k != i) g_desk[k] = g_desk[i];
        k++;
    }
    g_ndesk = k;
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

/* a built-in program's ACT_* by its name ("Notepad"), or -1 */
static int builtin_act(const char* name) {
    static const struct { const char* n; int act; } B[] = {
        { "Terminal", ACT_TERMINAL }, { "Files", ACT_FILES }, { "Browser", ACT_BROWSER }, { "Notepad", ACT_NOTEPAD },
        { "Apps", ACT_APPS }, { "Task Manager", ACT_TASKMGR }, { "Settings", ACT_SETTINGS },
        { "Device Manager", ACT_DEVMGR }, { "Disk Management", ACT_DISKMGR },
    };
    for (int i = 0; i < (int)(sizeof(B) / sizeof(B[0])); i++) if (!strcasecmp(name, B[i].n)) return B[i].act;
    return -1;
}

/* a shortcut: its target's icon, and the shortcut arrow at its bottom left */
static void desk_link_icon(const desk_item_t* d, int x, int y) {
    int ix = x + (CELL_W - 32) / 2, iy = y + 3;
    if (d->link == SC_APP) {
        int act = builtin_act(d->target + 4);
        desk_icon(act >= 0 ? act : ACT_RUN_APP, x + (CELL_W - 28) / 2, y + 5, act >= 0 ? NULL : d->target + 4);
    } else if (d->link == SC_WEB) desk_icon(ACT_BROWSER, x + (CELL_W - 28) / 2, y + 5, NULL);
    else {
        const char* b = strrchr(d->target, '/');
        fileicon_draw(d->link == SC_DIR ? FI_FOLDER : fileicon_for_name(b ? b + 1 : d->target), ix, iy, 32);
    }
    int ax = ix, ay = iy + 20;                       /* the arrow: white box, a blue arrow up and right */
    gfx_fill_rect(ax, ay, 12, 12, TC(0xFFFFFF));
    gfx_fill_rect(ax, ay, 12, 1, TC(0x808080)); gfx_fill_rect(ax, ay + 11, 12, 1, TC(0x808080));
    gfx_fill_rect(ax, ay, 1, 12, TC(0x808080)); gfx_fill_rect(ax + 11, ay, 1, 12, TC(0x808080));
    for (int i = 0; i < 6; i++) gfx_fill_rect(ax + 3 + i, ay + 8 - i, 2, 2, TC(0x1A5FC8));   /* the shaft */
    gfx_fill_rect(ax + 5, ay + 2, 5, 2, TC(0x1A5FC8));                                    /* the head */
    gfx_fill_rect(ax + 8, ay + 2, 2, 5, TC(0x1A5FC8));
}

static void draw_desktop_icons(const fb_info_t* fi) {
    g_is = 2;                                   /* 28 x 28 */
    for (int i = 0; i < g_ndesk; i++) {
        int x, y;
        desk_cell(fi, i, &x, &y);
        if (x + CELL_W > (int)fi->width) continue;
        int sel = desk_marked(i) || i == g_desk_drop;
        if (sel) gfx_fill_rect(x + (CELL_W - 36) / 2, y + 2, 36, 34, TC(0x315A9C));   /* selected: tinted */
        if (g_desk[i].link) desk_link_icon(&g_desk[i], x, y);
        else if (g_desk[i].kind == DK_APP) desk_icon(g_desk[i].act, x + (CELL_W - 28) / 2, y + 5, g_desk[i].app);
        else {
            fileicon_t k = g_desk[i].kind == DK_TRASH ? (trash_count() > 0 ? FI_TRASH_FULL : FI_TRASH) :
                           g_desk[i].kind == DK_DIR ? FI_FOLDER : fileicon_for_name(g_desk[i].label);
            fileicon_draw(k, x + (CELL_W - 32) / 2, y + 3, 32);
        }
        gfx_draw_label(x + CELL_W / 2, y + 40, CELL_W - 6, g_desk[i].label, TC(0xFFFFFF), sel ? TC(0x315A9C) : 0);   /* (white on the wallpaper, whatever the theme) */
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
    case SM_DEVMGR:  act = ACT_DEVMGR; break;
    case SM_DISKMGR: act = ACT_DISKMGR; break;
    case SM_SETTINGS: act = ACT_SETTINGS; break;
    case SM_INSTALL:  act = ACT_INSTALL; break;
    case SM_SHUTDOWN: act = ACT_SHUTDOWN; break;
    case SM_RESTART:  act = ACT_RESTART; break;
    case SM_LOCK:     act = ACT_LOCK; break;
    case SM_QUIT:     act = ACT_QUIT; break;
    case SM_SLEEP:    act = ACT_SLEEP; break;
    }
    g_menu_open = 0;
    g_force_redraw = 1;
    if (act >= 0) do_action(act);
}

static void sm_run_app(const char* name) { g_force_redraw = 1; run_app(name); }

/* a right-click on a program in the Start menu: open it, a shortcut to it
 * on the desktop, pin it to the taskbar */
enum { SMM_OPEN = 1, SMM_DESKTOP, SMM_PIN, SMM_UNPIN };
static struct { int sm, act; char app[PKG_NAME_MAX], label[48]; } g_smm;
static int sm_act(int sm) {
    switch (sm) {
    case SM_TERMINAL: return ACT_TERMINAL;
    case SM_FILES:    return ACT_FILES;
    case SM_BROWSER:  return ACT_BROWSER;
    case SM_NOTEPAD:  return ACT_NOTEPAD;
    case SM_APPS:     return ACT_APPS;
    case SM_TASKMGR:  return ACT_TASKMGR;
    case SM_DEVMGR:   return ACT_DEVMGR;
    case SM_DISKMGR:  return ACT_DISKMGR;
    case SM_SETTINGS: return ACT_SETTINGS;
    }
    return ACT_RUN_APP;
}
static void sm_menu_cb(int id, void* arg) {
    (void)arg;
    if (id == SMM_OPEN) {
        g_menu_open = 0;
        if (g_smm.sm) sm_action(g_smm.sm); else sm_run_app(g_smm.app);
    } else if (id == SMM_DESKTOP) {
        char target[PKG_NAME_MAX + 24];
        static const char* const NAMES[] = { [SM_TERMINAL] = "Terminal", [SM_FILES] = "Files", [SM_BROWSER] = "Browser",
            [SM_NOTEPAD] = "Notepad", [SM_APPS] = "Apps", [SM_TASKMGR] = "Task Manager", [SM_SETTINGS] = "Settings",
            [SM_DEVMGR] = "Device Manager", [SM_DISKMGR] = "Disk Management" };
        const char* n = g_smm.sm > 0 && g_smm.sm < (int)(sizeof(NAMES) / sizeof(NAMES[0])) ? NAMES[g_smm.sm] : NULL;
        if (g_smm.sm && !n) return;
        ksnprintf(target, sizeof(target), "app:%s", g_smm.sm ? n : g_smm.app);
        gui_desktop_shortcut(g_smm.label, target);
    } else if (id == SMM_PIN) pin_add(g_smm.act, g_smm.app);
    else if (id == SMM_UNPIN) { int p = pin_find(g_smm.act, g_smm.app); if (p >= 0) pin_remove(p); }
    g_force_redraw = 1;
}
static int sm_rclick(const fb_info_t* fi, int mx, int my) {
    if (!startmenu_program_at(fi, mx, my, &g_smm.sm, g_smm.app, sizeof(g_smm.app), g_smm.label, sizeof(g_smm.label))) return 0;
    g_smm.act = sm_act(g_smm.sm);
    if (g_smm.act != ACT_RUN_APP) g_smm.app[0] = 0;
    int pinned = pin_find(g_smm.act, g_smm.app) >= 0;
    ctx_item_t items[] = {
        { _("Open"), SMM_OPEN, 0 },
        { CTX_SEP, 0, 0 },
        { _("Add to desktop"), SMM_DESKTOP, 0 },
        { pinned ? _("Unpin from taskbar") : _("Pin to taskbar"), pinned ? SMM_UNPIN : SMM_PIN, 0 },
    };
    ctxmenu_open(mx, my, items, 4, sm_menu_cb, NULL);
    return 1;
}

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
    case SM_DEVMGR:  draw_icon_devmgr(x, y, bg); break;
    case SM_DISKMGR: draw_icon_diskmgr(x, y, bg); break;
    case SM_SETTINGS: draw_icon_settings(x, y, bg); break;
    case SM_INSTALL:  draw_icon_install(x, y, bg); break;
    default:          g_app_icon_name = app ? app : ""; draw_icon_app(x, y, bg); break;
    }
    g_is = 1;
}

/* the Start menu opens (graphics mode) */
static void start_open(void) {
    appwin_release_mouse();
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
            if (!pkg_is_desktop(&g_pkgs[i]) || !strcmp(g_pkgs[i].name, "terminal")) continue;
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
        char reason[160];
        items[n++] = (ctx_item_t){ "Sleep", ACT_SLEEP, acpi_s3_status(reason,sizeof(reason))!=0 };
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
        if (TERM_SHOWN(&g_terms[i])) return i;
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
    if (w->maxed) { term_unsnap(fi, w); return; }
    win_rect_t r;
    winframe_snap_rect('m', &r);
    term_snap(fi, w, &r, SNAP_MAX);
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
static void dband_draw(const fb_info_t* fi);
static void ask_draw(const fb_info_t* fi);

/* the keyboard everywhere (defined at the end of this file) */
static int  kb_global(int k);                 /* Alt+Tab, Win+..., the Menu key: 1 if taken */
static void kb_draw_overlays(const fb_info_t* fi);
static int  kb_modal(void);                   /* the switcher, task view, taskbar focus or a menu has the keys */
static int  kb_modal_key(int code);
static int  g_sw_open, g_sw_sel;              /* Alt+Tab: the switcher */
static int  g_tv_open, g_tv_sel;              /* Win+Tab: task view */
static int  g_bar_kb, g_bar_sel = -1;         /* Win+T: the taskbar has the keys */
static int  g_cb_open, g_cb_sel, g_cb_top;       /* Win+V: the clipboard history */
static int g_np_open, g_np_sel;
static void np_toggle(void);
static int np_key(int code);
static void np_draw(const fb_info_t* fi);
static void np_click(const fb_info_t* fi, int mx, int my);
static int np_wheel(const fb_info_t* fi, int mx, int my, int dz);
static int  cb_key(int code);
static void cb_toggle(void);
static void cb_click(const fb_info_t* fi, int mx, int my);
static int  cb_wheel(const fb_info_t* fi, int mx, int my, int dz);
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
        if (c == KEYF_F1 + 10 && !(k & (KEYF_ALT|KEYF_CTRL|KEYF_SHIFT))) { fullscreen_toggle();continue; }
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
    draw_bevel_box(x, y, w, h, TC(0x1D232C), TC(0x505E74), TC(0x10151D));
    char t[32];
    if (v == 0) kstrlcpy(t, "Sound off", sizeof(t));
    else ksnprintf(t, sizeof(t), "Volume %d%%", v);
    gfx_draw_text(x + 14, y + 10, t, TC(0xE8EEF6), TC(0x1D232C));
    gfx_fill_rect(x + 14, y + 30, w - 28, 10, TC(0x303A4A));
    gfx_fill_rect(x + 14, y + 30, (w - 28) * v / 100, 10, TC(0x68A8F0));
}

static void render_desktop(const fb_info_t* fi, int mx, int my) {
    refresh_wallpaper_cache(fi);
    blit_wallpaper_cache();

    draw_desktop_icons(fi);
    dband_draw(fi);

    /* windows back to front in the one stacking order, the ones always on
     * top last; each with a soft shadow under it */
    for (int pass = 0; pass < 2; pass++) {
        for (int j = 0; j < g_nz; j++) {
            int h = g_z[j], k = h >> 8, i = h & 0xFF;
            if (z_ontop(h) != pass || !z_shown(h)) continue;
            if (k == WK_TERM) {
                term_win_t* w = &g_terms[i];
                if (!w->maxed) winframe_shadow(w->x, w->y, w->w, w->h);
                draw_terminal_window(fi, w);
            } else if (i == APP_APPWIN) {
                for (int id = 0; id < APPWIN_MAX; id++) {
                    int x, y, ww, hh, min = 0, mx2 = 0;
                    if (!appwin_info(id, NULL, 0, &min) || min || g_awin_desk[id] != g_cur_desk) continue;
                    appwin_state(id, &mx2, NULL, NULL);
                    if (!mx2 && appwin_get_rect(id, &x, &y, &ww, &hh)) winframe_shadow(x, y, ww, hh);
                }
                g_apps[i].draw(fi);
            } else {
                win_geom_t* g = app_geom(i);
                if (g && !g->maxed) winframe_shadow(g->x, g->y, g->w, g->h);
                g_apps[i].draw(fi);
            }
        }
    }

    /* a window dragged over a snap zone: where it would go */
    {
        win_rect_t r;
        if (winframe_preview(&r)) {
            winframe_tint(r.x + 4, r.y + 4, r.w - 8, r.h - 8, TC(0x3A7BD5), 70);
            gfx_fill_rect(r.x + 4, r.y + 4, r.w - 8, 2, TC(0x68A8F0));
            gfx_fill_rect(r.x + 4, r.y + r.h - 6, r.w - 8, 2, TC(0x68A8F0));
            gfx_fill_rect(r.x + 4, r.y + 4, 2, r.h - 8, TC(0x68A8F0));
            gfx_fill_rect(r.x + r.w - 6, r.y + 4, 2, r.h - 8, TC(0x68A8F0));
        }
    }

    if(!fullscreen_active())draw_taskbar_fb(fi);
    nettray_draw();                                 /* the network panel, over the windows */
    cal_draw();                                     /* the calendar (a click on the clock) */
    if (g_desk_osd) {                               /* "Desktop 2", after switching */
        char t[40];
        if (g_osd_text[0]) kstrlcpy(t, g_osd_text, sizeof(t));
        else ksnprintf(t, sizeof(t), "Desktop %d", g_cur_desk + 1);
        int w = (int)strlen(t) * 16 + 40, h = 44;
        if (w < 180) w = 180;
        int x = ((int)fi->width - w) / 2, y = ((int)fi->height - BAR_H) / 2 - h / 2;
        draw_bevel_box(x, y, w, h, TC(0x1D232C), TC(0x505E74), TC(0x10151D));
        gfx_draw_text_scaled(x + (w - (int)strlen(t) * 16) / 2, y + 14, 2, t, TC(0xE8EEF6), TC(0x1D232C));
    }

    /* the Start menu */
    if (g_menu_open) startmenu_draw(fi);

    draw_volume_osd(fi);
    notify_draw_toast(fi);
    dnd_draw(fi, mx, my);
    kb_draw_overlays(fi);
    ctxmenu_draw();
    ask_draw(fi);                                   /* a dialog (New shortcut...) over everything */

    /* The software pointer is outside the shadow image. Erase its previous
     * location even when the desktop pixels underneath did not change. */
    if (g_hw_shape < 0 && g_ptr_drawn_x >= 0)
        fb_present_rect(g_ptr_drawn_x + CURSOR_X0, g_ptr_drawn_y + CURSOR_Y0, CURSOR_W, CURSOR_H);
    fb_present_damage();
    draw_cursor(mx, my);
    static int shown;
    if (!shown) { shown = 1; boottime_mark("the desktop's first frame"); }
}

/* ── right-click menus ─────────────────────────────────────────────── */

static void do_action(int act);

/* ── a small dialog asking for a line of text (New shortcut...) ────────
 * Over everything, in the middle of the screen: a prompt, a field, an
 * example under it; Enter or OK hands the text to `done`, which closes it
 * (0) or says what is wrong (the dialog stays); Esc or Cancel closes it. */
static struct {
    int  open;
    char title[48], prompt[120], hint[120], err[120];
    char text[FS_PATH_LEN];
    int  (*done)(const char* text, char* err, int ecap);
    int  x, y, w, h;                                   /* where it was drawn */
} g_ask;

static void ask_open(const char* title, const char* prompt, const char* hint,
                     int (*done)(const char*, char*, int)) {
    memset(&g_ask, 0, sizeof(g_ask));
    g_ask.open = 1;
    kstrlcpy(g_ask.title, title, sizeof(g_ask.title));
    kstrlcpy(g_ask.prompt, prompt, sizeof(g_ask.prompt));
    kstrlcpy(g_ask.hint, hint, sizeof(g_ask.hint));
    g_ask.done = done;
    g_force_redraw = 1;
}

static void ask_ok(void) {
    if (g_ask.done && g_ask.done(g_ask.text, g_ask.err, sizeof(g_ask.err)) != 0) { g_force_redraw = 1; return; }
    g_ask.open = 0;
    g_force_redraw = 1;
}

static void ask_key(char c) {
    int n = (int)strlen(g_ask.text);
    if (c == '\n') ask_ok();
    else if (c == 27) g_ask.open = 0;
    else if (c == '\b') { if (n) u8_backspace(g_ask.text, n); g_ask.err[0] = 0; }
    else if (c == 22) {                                                 /* Ctrl+V */
        uint32_t cn;
        const char* clip = clipboard_get(&cn);
        for (uint32_t i = 0; i < cn && n < (int)sizeof(g_ask.text) - 1; i++)
            if ((unsigned char)clip[i] >= 32) g_ask.text[n++] = clip[i];
        g_ask.text[n] = 0;
    } else if ((unsigned char)c >= 32 && n < (int)sizeof(g_ask.text) - 1) { g_ask.text[n] = c; g_ask.text[n + 1] = 0; g_ask.err[0] = 0; }
    g_force_redraw = 1;
}

static void ask_draw(const fb_info_t* fi) {
    if (!g_ask.open) return;
    int w = 460, h = 178, x = ((int)fi->width - w) / 2, y = ((int)fi->height - BAR_H - h) / 2;
    g_ask.x = x; g_ask.y = y; g_ask.w = w; g_ask.h = h;
    gfx_fill_rect(x + 5, y + 5, w, h, TC(0x080A0E));
    draw_bevel_box(x, y, w, h, TC(0x1D232C), TC(0x505E74), TC(0x10151D));
    gfx_fill_rect(x + 1, y + 1, w - 2, 24, TC(0x304868));
    gfx_draw_text(x + 10, y + 5, g_ask.title, TC(0xFFFFFF), TC(0x304868));
    gfx_draw_text(x + 14, y + 36, g_ask.prompt, TC(0xE8EEF6), TC(0x1D232C));
    gfx_fill_rect(x + 14, y + 58, w - 28, 24, TC(0xFFFFFF));             /* the field */
    gfx_fill_rect(x + 14, y + 81, w - 28, 1, TC(0x1A73E8));
    int maxc = (w - 40) / 8, n = (int)strlen(g_ask.text);
    const char* shown = g_ask.text + (n > maxc - 1 ? n - (maxc - 1) : 0);  /* (its end, while typing) */
    gfx_draw_text(x + 20, y + 63, shown, TC(0x101010), TC(0xFFFFFF));
    gfx_fill_rect(x + 20 + (int)strlen(shown) * 8, y + 62, 1, 14, TC(0x202020));
    gfx_draw_text(x + 14, y + 90, g_ask.hint, TC(0x9AA6B6), TC(0x1D232C));
    if (g_ask.err[0]) gfx_draw_text(x + 14, y + 112, g_ask.err, TC(0xF08070), TC(0x1D232C));
    draw_bevel_box(x + w - 186, y + h - 36, 80, 24, TC(0x405478), TC(0x6B7892), TC(0x222A36));
    gfx_draw_text(x + w - 186 + 32, y + h - 31, _("OK"), TC(0xFFFFFF), TC(0x405478));
    draw_bevel_box(x + w - 96, y + h - 36, 80, 24, TC(0x303740), TC(0x535D6E), TC(0x15191F));
    gfx_draw_text(x + w - 96 + 16, y + h - 31, _("Cancel"), TC(0xE8EEF6), TC(0x303740));
}

/* a click while it is open (it takes every click) */
static void ask_click(int mx, int my) {
    int x = g_ask.x, y = g_ask.y, w = g_ask.w, h = g_ask.h;
    if (my >= y + h - 36 && my < y + h - 12) {
        if (mx >= x + w - 186 && mx < x + w - 106) ask_ok();
        else if (mx >= x + w - 96 && mx < x + w - 16) g_ask.open = 0;
    }
    g_force_redraw = 1;
}

/* New shortcut: what was typed, as a shortcut on the desktop */
static int new_shortcut_done(const char* text, char* err, int ecap) {
    char target[FS_PATH_LEN], name[FS_NAME_LEN];
    if (!text[0]) { kstrlcpy(err, "Type what the shortcut should open.", (size_t)ecap); return -1; }
    if (shortcut_resolve(text, target, sizeof(target), name, sizeof(name)) == SC_NONE) {
        ksnprintf(err, (size_t)ecap, "Cannot find \"%s\".", text);
        return -1;
    }
    if (gui_desktop_shortcut(name, target) != 0) { kstrlcpy(err, "The shortcut could not be made.", (size_t)ecap); return -1; }
    return 0;
}

enum { DM_NEWFOLDER = 100, DM_NEWFILE, DM_PASTE, DM_OPENDIR, DM_REFRESH, DM_ARRANGE, DM_NEWLINK };
static void desktop_menu_cb(int id, void* arg) {
    (void)arg;
    char msg[112];
    switch (id) {
    case DM_NEWFOLDER: desk_new(1); break;
    case DM_NEWFILE: desk_new(0); break;
    case DM_NEWLINK:
        ask_open(_("Create shortcut"), "What should the shortcut open?",
                 "e.g. Notepad, example.com or ~/Music", new_shortcut_done);
        break;
    case DM_PASTE: fileops_paste(DESKTOP_DIR, msg, sizeof(msg)); desk_refresh(1); explorer_refresh(); break;
    case DM_OPENDIR: explorer_open(DESKTOP_DIR); gui_raise_files(); break;
    case DM_REFRESH: desk_refresh(1); break;
    case DM_ARRANGE: g_nlay = 0; layout_save(); desk_refresh(1); break;
    default: do_action(id); break;
    }
}

static void open_desktop_menu(int mx, int my) {
    ctx_item_t items[] = {
        { _("New folder"), DM_NEWFOLDER, 0 },
        { _("New text document"), DM_NEWFILE, 0 },
        { _("New shortcut..."), DM_NEWLINK, 0 },
        { _("Paste"), DM_PASTE, fileops_clip_count() == 0 },
        { CTX_SEP, 0, 0 },
        { _("Arrange icons"), DM_ARRANGE, 0 },
        { _("Refresh"), DM_REFRESH, 0 },
        { _("Open the Desktop folder"), DM_OPENDIR, 0 },
        { CTX_SEP, 0, 0 },
        { _("Terminal"), ACT_TERMINAL, 0 },
        { _("Files"), ACT_FILES, 0 },
        { _("Task Manager"), ACT_TASKMGR, 0 },
        { _("Device Manager"), ACT_DEVMGR, 0 },
        { _("Disk Management"), ACT_DISKMGR, 0 },
        { _("Change wallpaper..."), ACT_WALLPAPER, 0 },
        { _("About Banana OS"), ACT_ABOUT, 0 },
        { CTX_SEP, 0, 0 },
        { _("Lock screen"), ACT_LOCK, 0 },
        { _("Exit to shell"), ACT_QUIT, 0 },
    };
    ctxmenu_open(mx, my, items, (int)(sizeof(items) / sizeof(items[0])), desktop_menu_cb, NULL);
}

/* a desktop icon: open it (an installed app: also remove it) */
static int g_icon_menu_i;
enum { ICM_OPEN = 1 };

/* the program an icon opens - itself, or a shortcut's (app:...): 1, its
 * ACT_* and (ACT_RUN_APP) its app's name */
static int desk_program(int i, int* act, char* app, int cap) {
    desk_item_t* d = &g_desk[i];
    app[0] = 0;
    if (d->kind == DK_APP) { *act = d->act; if (d->act == ACT_RUN_APP) kstrlcpy(app, d->app, (size_t)cap); return 1; }
    if (d->link != SC_APP) return 0;
    *act = builtin_act(d->target + 4);
    if (*act < 0) { *act = ACT_RUN_APP; kstrlcpy(app, d->target + 4, (size_t)cap); }
    return 1;
}
enum { ICM_PIN = 10, ICM_UNPIN, ICM_SHOW, ICM_COPY, ICM_CUT, ICM_DELETE, ICM_RENAME, ICM_EMPTY, ICM_HIDE, ICM_TARGET };
static void icon_menu_cb(int id, void* arg) {
    (void)arg;
    int i = g_icon_menu_i;
    if (i < 0 || i >= g_ndesk) return;
    desk_item_t* d = &g_desk[i];
    const char* fname = strrchr(d->path, '/') ? strrchr(d->path, '/') + 1 : d->label;   /* (a shortcut's .link file) */
    int act;
    char app[PKG_NAME_MAX];
    if (id == ICM_OPEN) desk_open(i);
    else if ((id == ICM_PIN || id == ICM_UNPIN) && desk_program(i, &act, app, sizeof(app))) {
        if (id == ICM_PIN) pin_add(act, app);
        else { int p = pin_find(act, app); if (p >= 0) pin_remove(p); }
    }
    else if (id == ICM_SHOW) { explorer_open(DESKTOP_DIR); explorer_select(fname); gui_raise_files(); }
    else if (id == ICM_COPY || id == ICM_CUT) desk_clip_marked(id == ICM_CUT);
    else if (id == ICM_DELETE) desk_trash_marked();
    else if (id == ICM_RENAME) { explorer_open(DESKTOP_DIR); explorer_select(fname); gui_raise_files(); explorer_start_rename(); }
    else if (id == ICM_HIDE) desk_hide(i);
    else if (id == ICM_TARGET) {                     /* a shortcut to a file or folder: where that is */
        char dir[FS_PATH_LEN];
        kstrlcpy(dir, d->target, sizeof(dir));
        char* sl = strrchr(dir, '/');
        if (d->link == SC_FILE && sl) { *sl = 0; explorer_open(dir[0] ? dir : "/"); explorer_select(sl + 1); }
        else explorer_open(d->target);
        gui_raise_files();
    }
    else if (id == ICM_EMPTY) { trash_empty(); explorer_refresh(); desk_refresh(1); }
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
        items[n++] = (ctx_item_t){ "Remove from desktop", ICM_HIDE, 0 };
    } else if (d->kind == DK_TRASH) {
        items[n++] = (ctx_item_t){ "Open", ICM_OPEN, 0 };
        items[n++] = (ctx_item_t){ CTX_SEP, 0, 0 };
        items[n++] = (ctx_item_t){ "Empty Recycle Bin", ICM_EMPTY, trash_count() == 0 };
        items[n++] = (ctx_item_t){ "Remove from desktop", ICM_HIDE, 0 };
    } else {
        items[n++] = (ctx_item_t){ "Open", ICM_OPEN, 0 };
        if (d->link == SC_FILE || d->link == SC_DIR) items[n++] = (ctx_item_t){ "Open file location", ICM_TARGET, 0 };
        int act;
        char app[PKG_NAME_MAX];
        if (d->link == SC_APP && desk_program(i, &act, app, sizeof(app))) {
            int pinned = pin_find(act, app) >= 0;
            items[n++] = (ctx_item_t){ pinned ? "Unpin from taskbar" : "Pin to taskbar", pinned ? ICM_UNPIN : ICM_PIN, 0 };
        }
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
       TBM_PIN, TBM_UNPIN, TBM_PIN_OPEN, TBM_SETTINGS, TBM_DESKS };
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
    case TBM_DESKS: gui_set_taskbar_desks(!g_tb_desks); break;
    case TBM_SETTINGS: settings_open_page(SETTINGS_PAGE_TASKBAR); g_app_min[APP_SETTINGS] = 0; raise_app(APP_SETTINGS); break;
    }
}

static void open_taskbar_menu(const fb_info_t* fi, int mx, int my) {
    int p = pin_at(mx, my);
    if (p >= 0) {
        g_tbmenu_pin = p;
        ctx_item_t items[] = {
            { _("Open"), TBM_PIN_OPEN, 0 },
            { CTX_SEP, 0, 0 },
            { _("Unpin from taskbar"), TBM_UNPIN, 0 },
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
            { _("Restore"), TBM_RESTORE, !min && win_front() == g_tbmenu_win },
            { _("Minimize"), TBM_MINIMIZE, min },
            { CTX_SEP, 0, 0 },
            { pinned ? "Unpin from taskbar" : "Pin to taskbar", pinned ? TBM_UNPIN : TBM_PIN, !can },
            { CTX_SEP, 0, 0 },
            { _("Close window"), TBM_CLOSE, 0 },
            { CTX_SEP, 0, 0 },
            { _("Task Manager"), TBM_TASKMGR, 0 },
        };
        ctxmenu_open(mx, my, items, 8, taskbar_menu_cb, NULL);
        return;
    }
    ctx_item_t items[] = {
        { _("Task Manager"), TBM_TASKMGR, 0 },
        { CTX_SEP, 0, 0 },
        { _("Show the desktop"), TBM_SHOW_DESKTOP, g_ntb == 0 },
        { _("Restore all windows"), TBM_RESTORE_ALL, g_ntb == 0 },
        { CTX_SEP, 0, 0 },
        { _("New terminal"), TBM_TERMINAL, 0 },
        { g_tb_desks ? "Hide desktop buttons" : "Show desktop buttons", TBM_DESKS, 0 },
        { _("Taskbar settings"), TBM_SETTINGS, 0 },
        { _("Lock screen"), TBM_LOCK, 0 },
        { _("Exit to shell"), TBM_QUIT, 0 },
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
        { _("Copy"), TM_COPY, !g_terms[wi].sel },
        { _("Paste"), TM_PASTE, n == 0 },
        { _("Clear"), TM_CLEAR, 0 },
        { CTX_SEP, 0, 0 },
        { _("New terminal"), TM_NEW, 0 },
        { _("Minimize"), TM_MINIMIZE, 0 },
        { _("Close"), TM_CLOSE, 0 },
    };
    ctxmenu_open(mx, my, items, 7, term_menu_cb, NULL);
}

/* Notepad: its own Ctrl keys */
static void notepad_menu_cb(int id, void* arg) { (void)arg; notepad_key((char)id); }

static void notepad_rclick_menu(int mx, int my) {
    uint32_t n;
    clipboard_get(&n);
    ctx_item_t items[] = {
        { _("Cut"), 24, 0 },
        { _("Copy"), 3, 0 },
        { _("Paste"), 22, n == 0 },
        { _("Select all"), 1, 0 },
        { CTX_SEP, 0, 0 },
        { _("Find..."), 6, 0 },
        { _("Open..."), 15, 0 },
        { _("Save"), 19, 0 },
    };
    ctxmenu_open(mx, my, items, 8, notepad_menu_cb, NULL);
}

static void explorer_rclick_menu(int mx, int my) {
    explorer_rclick(mx, my);
}

/* Network flyout owns its input, including over a terminal/app window. */
static void nettray_action(int action) {
    if(action!=NETTRAY_SETTINGS && action!=NETTRAY_WIFI_SETTINGS) return;
    settings_open_page(action==NETTRAY_WIFI_SETTINGS?SETTINGS_PAGE_WIFI:SETTINGS_PAGE_NETWORK);
    g_app_min[APP_SETTINGS]=0;raise_app(APP_SETTINGS);
}
static int nettray_arrow_code(char c) {
    return c=='A'?KB_UP:c=='B'?KB_DOWN:c=='C'?KB_RIGHT:c=='D'?KB_LEFT:
           c=='H'?KB_HOME:c=='F'?KB_END:c=='I'?KB_PGUP:c=='G'?KB_PGDN:c=='P'?KB_DEL:0;
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
    if(crash_in_progress()) { crash_stop_secondary(); return; }
    if(crash_preview_active()) { task_sleep_ms(10); return; }
    g_gui_busy++;
    gui_poll_body();
    g_gui_busy--;
}

static void gui_poll_body(void) {
    /* the lock screen: while it is up, every other caller (the terminal
     * windows' shells poll too) leaves the screen and the keyboard to it */
    if (g_in_lock || crash_preview_active() || crash_in_progress()) return;
    /* (background tasks - the servers, app windows - call this too, but read no keys) */
    if (g_lock_pending && g_gui_enabled && !task_is_background()) {
        g_lock_pending = 0;
        appwin_release_mouse();
        vmtools_capture_pointer(0);
        g_in_lock = 1;
        g_menu_open = 0;nettray_close();
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
    if (g_in_lock || crash_preview_active() || crash_in_progress()) return;

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

        if (appwin_take_new()) raise_app(APP_APPWIN);   /* a new app window comes up in front */
        z_sync();                                       /* the stacking order: new and closed windows */
        sync_taskbar();
        z_focus();                                      /* the keyboard: the top window shown */
        appwin_focus(g_front_app == APP_APPWIN);
        if (g_desk_osd && (int32_t)(timer_ms() - g_desk_osd_until) >= 0) { g_desk_osd = 0; g_osd_text[0] = 0; g_force_redraw = 1; }

        /* Notepad and app windows have no keyboard task of their own:
         * while one is in front, keys are handed to it from here (Ctrl+T
         * and the Start menu first). Apps and Task Manager read no keys
         * (Esc closes them). */
        if (nettray_is_open() || g_ask.open || gui_notepad_focused() || gui_appwin_focused() || (kb_modal() && g_front_app >= 0) ||
            ((g_front_app == APP_LAUNCHER || g_front_app == APP_TASKMGR || g_front_app == APP_FILES || g_front_app == APP_SETTINGS || g_front_app == APP_INSTALLER || g_front_app == APP_DEVMGR || g_front_app == APP_DISKMGR) &&
             app_visible(g_front_app) && tty_current() < 0)) {
            for (int k = 0; k < 64; k++) {
                char c = keyboard_try_getchar();
                if (!c) break;
                if (g_ask.open) {                       /* a dialog (New shortcut...): its field */
                    if (c == 27) {
                        char c2 = keyboard_try_getchar();
                        if (c2 == '[') { char c3 = keyboard_try_getchar(); if (c3 >= '0' && c3 <= '9') keyboard_try_getchar(); continue; }
                    }
                    ask_key(c);
                    continue;
                }
                if(nettray_is_open()) {
                    int code=(unsigned char)c;
                    if(c==27) {
                        char c2=keyboard_try_getchar();
                        if(c2=='[') {
                            char c3=keyboard_try_getchar();
                            if(c3>='0' && c3<='9') keyboard_try_getchar();
                            code=nettray_arrow_code(c3);
                        }
                    } else if(c=='\t' && (keyboard_mods()&1)) code=KB_BACKTAB;
                    if(code) nettray_action(nettray_key(code));
                    g_force_redraw=1;continue;
                }
                /* the switcher, task view, taskbar focus, a right-click menu: theirs */
                if (kb_modal() && !g_menu_open) {
                    int code = (unsigned char)c;
                    if (c == 27) {
                        char c2 = keyboard_try_getchar();
                        if (c2 == '[') {
                            char c3 = keyboard_try_getchar();
                            if (c3 >= '0' && c3 <= '9') keyboard_try_getchar();
                            code = c3 == 'A' ? KB_UP : c3 == 'B' ? KB_DOWN : c3 == 'C' ? KB_RIGHT : c3 == 'D' ? KB_LEFT :
                                   c3 == 'H' ? KB_HOME : c3 == 'F' ? KB_END : c3 == 'P' ? KB_DEL :
                                   c3 == 'I' ? KB_PGUP : c3 == 'G' ? KB_PGDN : 0;
                        } else code = KB_ESC;
                    } else if (c == '\n') code = KB_ENTER;
                    else if (c == '\t') code = (keyboard_mods() & 1) ? KB_BACKTAB : KB_TAB;
                    else if (c == ' ') code = KB_SPACE;
                    else if (g_cb_open && c > ' ' && c < 127) code = c;
                    else if (ep_is_open() && ((c > ' ' && c < 127) || c == '\b' || c == 127)) code = c;   /* (its search) */
                    if (code) kb_modal_key(code);
                    g_force_redraw = 1;
                    continue;
                }
                /* Settings, Task Manager, Apps: the focus moves between their controls */
                if (!g_menu_open && (g_front_app == APP_SETTINGS || g_front_app == APP_TASKMGR || g_front_app == APP_LAUNCHER || g_front_app == APP_DEVMGR || g_front_app == APP_DISKMGR)) {
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
                        else if (g_front_app == APP_DEVMGR) took = devmgr_navkey(code);
                        else if (g_front_app == APP_DISKMGR) took = diskmgr_navkey(code);
                        else took = launcher_navkey(code);
                    }
                    if (!took) {
                        if (code == KB_ESC) c = 27;
                        if (g_front_app == APP_SETTINGS) settings_key(c);
                        else if (g_front_app == APP_DISKMGR) diskmgr_key(c);      /* (its dialog's fields) */
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
                /* (Ctrl+T: the Start menu - but Ctrl+Shift+T goes to an app's window: Terminal's new tab) */
                if (g_menu_open || (c == 20 && !(g_front_app == APP_APPWIN && (keyboard_mods() & 1)))) { gui_handle_key(c); continue; }
                if (g_front_app == APP_NOTEPAD) notepad_key(c);
                else if (g_front_app == APP_APPWIN && c == 27) {
                    /* Esc alone, or the start of an arrow key's ESC [ X (queued
                     * together by the keyboard): Esc pressed alone reaches the
                     * app now, not with the next key */
                    char c2 = keyboard_try_getchar();
                    appwin_key(27);
                    if (c2 == '[') {
                        appwin_key('[');
                        char c3 = keyboard_try_getchar();
                        if (c3) appwin_key(c3);
                        if (c3 >= '0' && c3 <= '9') { char c4 = keyboard_try_getchar(); if (c4) appwin_key(c4); }
                    } else {
                        appwin_key_esc();
                        if (c2) appwin_key(c2);
                    }
                }
                else if (g_front_app == APP_APPWIN) appwin_key(c);
                else if (g_front_app == APP_FILES) explorer_key(c);
                else if (g_front_app == APP_SETTINGS) settings_key(c);
                else if (g_front_app == APP_INSTALLER) installer_key(c);
                else if (c == 27) g_apps[g_front_app].close();
            }
        }
        gui_fkeys(fi);
        { static uint32_t cbg; if (g_cb_open && clipboard_hist_generation() != cbg) { cbg = clipboard_hist_generation(); g_force_redraw = 1; } }
        notify_poll();
        { static int was_active; static uint32_t last;
          int active = notify_has_active_toast(); uint32_t now = timer_ms();
          if (active != was_active || (active && now - last >= 32)) { g_force_redraw = 1; last = now; }
          was_active = active;
        }
        appwin_focus(g_front_app==APP_APPWIN && !g_np_open && !g_menu_open && !ctxmenu_is_open() && !g_cb_open && !ep_is_open() && !g_tv_open && !g_sw_open && !g_bar_kb);

        if (!g_backbuf_active) {
            if (size_desktop(fi) < 0) {
                gui_set_enabled(0);
                terminal_writeln("startx: not enough free memory for the desktop's screen buffers");
                return;
            }
            fb_set_backbuffer(g_desktop_backbuf, g_desk_w, g_desk_h);
            g_backbuf_active = 1;
            g_force_redraw = 1;
            /* the saved wallpaper (/etc/wallpaper) is applied on first use */
            wallpaper_load_config();
            theme_init();                   /* the built-in and installed themes, the one chosen */
        }

        /* (the timer interrupt moves the pointer too: interrupts off while it changes here) */
        g_scr_w = (int)fi->width;
        g_scr_h = (int)fi->height;
        uintptr_t fl;
        __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
        int captured=appwin_capture_active();
        static int was_captured;
        if(captured!=was_captured) { vmtools_capture_pointer(captured);g_force_redraw=1;was_captured=captured; }
        mouse_state_t ms = captured?mouse_read_raw():mouse_read();
        if(captured && appwin_capture_input(&ms,(int)fi->width,(int)fi->height)) {
            ms.dx=ms.dy=ms.abs=ms.dz=0;
        }
        g_mouse_from_lock = 0;               /* (the lock screen moved g_cur_mx/my itself) */
        if (ms.abs) {                        /* a tablet says where the pointer is */
            g_cur_mx = (int)((int64_t)ms.ax * ((int)fi->width - 1) / 65535);
            g_cur_my = (int)((int64_t)ms.ay * ((int)fi->height - 1) / 65535);
        }
        g_cur_mx += ms.dx;
        g_cur_my -= ms.dy;
        if (g_cur_mx < 0) g_cur_mx = 0;
        if (g_cur_my < 0) g_cur_my = 0;
        if (g_cur_mx > (int)fi->width - 1) g_cur_mx = (int)fi->width - 1;
        if (g_cur_my > (int)fi->height - 1) g_cur_my = (int)fi->height - 1;
        mx = g_cur_mx;
        my = g_cur_my;
        if (fl & 0x200) __asm__ volatile("sti" ::: "memory");
        if (ms.dz && g_np_open && np_wheel(fi, mx, my, ms.dz)) {
            g_force_redraw = 1;
        } else if (ms.dz && g_cb_open && cb_wheel(fi, mx, my, ms.dz)) {
            g_force_redraw = 1;
        } else if (ms.dz && ep_is_open() && ep_wheel(fi, BAR_H, mx, my, ms.dz)) {
            g_force_redraw = 1;
        } else if (ms.dz && g_menu_open && startmenu_contains(fi, mx, my)) {
            startmenu_wheel(ms.dz);
        } else if (ms.dz) {
            /* the wheel scrolls the window under the mouse */
            int h = z_at(mx, my);
            if ((h >> 8) == WK_APP && g_app_wheel[h & 0xFF]) g_app_wheel[h & 0xFF](mx, my, ms.dz);
        }
        if (mx < 0) mx = 0;
        if (my < 0) my = 0;
        if (mx > (int)fi->width - 1) mx = (int)fi->width - 1;
        if (my > (int)fi->height - 1) my = (int)fi->height - 1;

        uint32_t sec = timer_ticks() / 100u;
        int bar_y = (int)fi->height - (fullscreen_active()?0:BAR_H);

        /* mouse click handling (rising edge) */
        int left = ms.btn_left ? 1 : 0;
        int click = (left && !prev_left);
        prev_left = left;
        static int prev_right = 0;
        int right = ms.btn_right ? 1 : 0;
        int rclick = right && !prev_right;
        prev_right = right;
        if(captured) { click=rclick=0;left=0; }
        if ((click || rclick) && g_np_open) { if (click) np_click(fi, mx, my); else g_np_open = 0; g_force_redraw = 1; click = rclick = 0; }
        if (click && notify_click(fi, mx, my)) { click = 0; g_force_redraw = 1; }
        if ((click || rclick) && g_cb_open) { if (click) cb_click(fi, mx, my); else g_cb_open = 0; g_force_redraw = 1; click = rclick = 0; }
        if ((click || rclick) && ep_is_open()) { if (click) ep_click(fi, BAR_H, mx, my); else ep_close(); g_force_redraw = 1; click = rclick = 0; }
        /* task view / switcher / taskbar focus: a click picks a window or leaves */
        if ((click || rclick) && (g_tv_open || g_sw_open || g_bar_kb)) {
            if (click && g_tv_open) { int i = tv_card_at(fi, mx, my); if (i >= 0) tv_activate(i); }
            g_tv_open = g_sw_open = g_bar_kb = 0;
            g_force_redraw = 1;
            click = rclick = 0;
        }

        ctxmenu_hover(mx, my);
        {
            int nh = net_icon_at(fi, mx, my) && !ctxmenu_is_open();
            if (nh != g_net_hover) { g_net_hover = nh; g_force_redraw = 1; }
            nettray_hover(mx, my);
        }

        /* the Start menu follows the pointer, and its submenus open by
         * themselves under it - Windows 95 style */
        if (!ctxmenu_is_open()) g_sub_slot = -1;
        if (g_menu_open && !ctxmenu_contains(mx, my)) startmenu_hover(fi, mx, my);
        {
            int sh = my >= bar_y && mx >= START_X && mx < START_X + START_W;
            if (sh != g_start_hover) g_start_hover = sh;
        }

        if ((click || rclick) && g_ask.open) {        /* the dialog takes every click */
            if (click) ask_click(mx, my);
            click = rclick = 0;
        }

        if (rclick && g_menu_open && startmenu_contains(fi, mx, my)) {
            /* in the Start menu: a program's menu (the Start menu stays open), else nothing */
            ctxmenu_close();
            sm_rclick(fi, mx, my);
            rclick = 0;
        }
        if (rclick) {
            /* right-click: the menu of whatever is under the mouse */
            ctxmenu_close();
            g_menu_open = 0;
            int h = my < bar_y ? z_at(mx, my) : 0;
            int th = my < bar_y ? wm_title_at(mx, my) : 0;
            if (my >= bar_y) {
                open_taskbar_menu(fi, mx, my);
            } else if (th) {                         /* a title bar: the window's menu */
                win_activate(th);
                open_window_menu(th, mx, my);
            } else if ((h >> 8) == WK_APP) {
                int a = h & 0xFF;
                if (a != g_front_app) raise_app(a);
                g_apps[a].rclick(mx, my);
            } else if ((h >> 8) == WK_TERM) {
                bring_term_front(h & 0xFF);
                g_front_app = -1;
                open_term_menu(h & 0xFF, mx, my);
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

        /* the network panel: its buttons, or a click elsewhere closes it */
        if ((click || rclick) && nettray_is_open() && !net_icon_at(fi, mx, my)) {
            if (click && nettray_contains(mx, my)) {
                nettray_action(nettray_click(mx,my));
                click = 0;
            } else nettray_close();
            g_force_redraw = 1;
        }

        /* the calendar: a click in it, or a click elsewhere closes it */
        if ((click || rclick) && cal_is_open() && !clock_at(fi, mx, my)) {
            if (click && cal_contains(mx, my)) { cal_click(mx, my); click = 0; }
            else cal_close();
            g_force_redraw = 1;
        }

        if (click) {
            /* the taskbar: Start, a window's button */
            if (click && my >= bar_y) {
                if (clock_at(fi, mx, my)) {
                    cal_toggle((int)fi->width - 4, bar_y);
                    nettray_close();
                    g_menu_open = 0;
                    g_force_redraw = 1;
                } else if (net_icon_at(fi, mx, my)) {
                    cal_close();
                    nettray_toggle(net_icon_x(fi) + NETTRAY_ICON_W + 4, bar_y);
                    g_menu_open = 0;
                    g_force_redraw = 1;
                } else if (mx >= START_X && mx < START_X + START_W) {
                    if (g_menu_open) g_menu_open = 0;
                    else start_open();
                } else if (desk_switch_at(fi, mx, my) >= 0) {
                    desk_switch(desk_switch_at(fi, mx, my));
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

            /* Alt+drag: the window under the mouse moves, wherever it is taken */
            if (click && (keyboard_mods() & 4) && wm_alt_drag(mx, my)) click = 0;

            /* the window under the mouse, in the one stacking order */
            int zh = click ? z_at(mx, my) : 0;
            if ((zh >> 8) == WK_APP) {
                int a = zh & 0xFF;
                if (a != g_front_app) raise_app(a);
                g_apps[a].click(mx, my);
                if (winframe_take_minimize()) win_minimize(WK_APP << 8 | a);
                click = 0;
            }

            if ((zh >> 8) == WK_TERM) {
                int wi = zh & 0xFF;
                term_win_t* w = &g_terms[wi];
                int title_h = TERM_TITLE_H;
                {
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
                            if (w->maxed) term_unsnap(fi, w);
                            else { win_rect_t r; winframe_snap_rect('m', &r); term_snap(fi, w, &r, SNAP_MAX); }
                            w->title_click_ms = 0;
                        } else {
                            w->title_click_ms = now;
                            w->dragging = 1;
                            w->drag_dx = mx - w->x;
                            w->drag_dy = my - w->y;
                            w->px = mx;
                            w->py = my;
                        }
                    } else if (mx >= w->x + w->w - GRIP && my >= w->y + w->h - GRIP) {
                        w->resizing = 1;
                        w->drag_dx = w->x + w->w - mx;
                        w->drag_dy = w->y + w->h - my;
                        w->maxed = w->snapped = 0;
                    } else {
                        /* start a text selection (a plain click clears it) */
                        w->selecting = 1;
                        w->sel = 0;
                        term_cell_at(w, mx, my, &w->s_r0, &w->s_c0);
                        w->s_r1 = w->s_r0;
                        w->s_c1 = w->s_c0;
                    }
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
                /* snapped or maximized, dragged away: back to its own size */
                if ((t->maxed || t->snapped) && (mx - t->px > 6 || t->px - mx > 6 || my - t->py > 6 || t->py - my > 6)) {
                    int ow0 = t->w > 0 ? t->w : 1;
                    t->drag_dx = t->drag_dx * t->sw / ow0;
                    t->w = t->sw;
                    t->h = t->sh;
                    t->maxed = t->snapped = 0;
                    term_apply_size(t);
                }
                if (!t->maxed && !t->snapped) {
                    t->x = mx - t->drag_dx;
                    t->y = my - t->drag_dy;
                    clamp_win(fi, t);
                }
                win_rect_t r;
                winframe_set_preview(winframe_snap_zone(mx, my, &r) ? &r : NULL);
            }
            if (!left && t->dragging) {          /* let go over a snap zone: it goes there */
                win_rect_t r;
                int z = winframe_snap_zone(mx, my, &r);
                if (z) term_snap(fi, t, &r, z);
                winframe_set_preview(NULL);
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
            if(captured)return;
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
        if (g_screenshot_requested) {
            g_screenshot_requested = 0;
            char path[FS_PATH_LEN];
            int rc = screenshot_take(NULL, path, sizeof(path));
            kstrlcpy(g_osd_text, rc == 0 ? "Screenshot saved to Pictures" : "Screenshot failed", sizeof(g_osd_text));
            g_desk_osd = 1;
            g_desk_osd_until = timer_ms() + 2000;
            g_force_redraw = 1;
        }
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
    if(!enabled) { appwin_release_mouse();vmtools_capture_pointer(0); }
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
            /* the screen buffers go back to the heap too (made again at the next startx) */
            kfree(g_desktop_backbuf);
            kfree(g_wallpaper_cache);
            g_desktop_backbuf = g_wallpaper_cache = NULL;
            g_desk_w = g_desk_h = 0;
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
    if ((g_front_app == APP_LAUNCHER || g_front_app == APP_TASKMGR || g_front_app == APP_FILES || g_front_app == APP_SETTINGS || g_front_app == APP_INSTALLER || g_front_app == APP_DEVMGR || g_front_app == APP_DISKMGR) && app_visible(g_front_app))
        return APPWIN_VT + 1;          /* keys handed out by gui_poll() */

    /* Frontmost OPEN window, if any - g_term_order always lists every
     * slot, closed or not, so this has to skip closed ones explicitly. */
    for (int oi = TERM_WIN_MAX - 1; oi >= 0; oi--) {
        int wi = g_term_order[oi];
        if (TERM_SHOWN(&g_terms[wi])) return g_terms[wi].vt;
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
    case ACT_TERMINAL: bash_terminal(NULL); break;
    case ACT_FILES: explorer_open(NULL); g_app_min[APP_FILES] = 0; raise_app(APP_FILES); break;
    case ACT_BROWSER: browser_open(NULL); g_app_min[APP_BROWSER] = 0; raise_app(APP_BROWSER); break;
    case ACT_NOTEPAD: notepad_open(NULL); g_app_min[APP_NOTEPAD] = 0; raise_app(APP_NOTEPAD); break;
    case ACT_APPS: launcher_open(); g_app_min[APP_LAUNCHER] = 0; raise_app(APP_LAUNCHER); break;
    case ACT_TASKMGR: taskmgr_open(); g_app_min[APP_TASKMGR] = 0; raise_app(APP_TASKMGR); break;
    case ACT_DEVMGR: devmgr_open(); g_app_min[APP_DEVMGR] = 0; raise_app(APP_DEVMGR); break;
    case ACT_DISKMGR: diskmgr_open(); g_app_min[APP_DISKMGR] = 0; raise_app(APP_DISKMGR); break;
    case ACT_SETTINGS: settings_open(); g_app_min[APP_SETTINGS] = 0; raise_app(APP_SETTINGS); break;
    case ACT_WALLPAPER: settings_open_page(SETTINGS_PAGE_DISPLAY); g_app_min[APP_SETTINGS] = 0; raise_app(APP_SETTINGS); break;
    case ACT_QUIT: gui_set_enabled(0); break;
    case ACT_INSTALL: installer_open(); g_app_min[APP_INSTALLER] = 0; raise_app(APP_INSTALLER); break;
    case ACT_RESTART: shell_power(1); break;          /* (saves the files first) */
    case ACT_SHUTDOWN: shell_power(0); break;
    case ACT_SLEEP: shell_sleep();g_force_redraw=1;break;
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

int gui_open_settings_app(const char* name) {
    if (!gfx_available() || !g_gui_enabled) return 0;
    settings_show_app(name);
    do_action(ACT_SETTINGS);
    return 1;
}

/* Settings at one of its pages (SETTINGS_PAGE_*) */
int gui_open_settings_page(int page) {
    if (!gfx_available() || !g_gui_enabled) return 0;
    settings_open_page(page);
    g_app_min[APP_SETTINGS] = 0;
    raise_app(APP_SETTINGS);
    return 1;
}

int gui_open_diskmgr(void) {
    if (!gfx_available() || !g_gui_enabled) return 0;
    do_action(ACT_DISKMGR);
    return 1;
}

int gui_open_devmgr(void) {
    if (!gfx_available() || !g_gui_enabled) return 0;
    do_action(ACT_DEVMGR);
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

/* 64-bit: the original Terminal entry opens the tabbed emulator with the
 * selected shell; cmd runs there first. 0 if it could not start. */
static int bash_terminal(const char* cmd) {
#ifdef __x86_64__
    char err[96], line[FS_PATH_LEN + 32];
    char* argv[6] = { "terminal", NULL };
    int argc = 1;
    if (cmd && *cmd) {
        size_t n = strlen(cmd);
        while (n && (cmd[n - 1] == '\n' || cmd[n - 1] == '\r')) n--;
        if (n > FS_PATH_LEN) n = FS_PATH_LEN;
        memcpy(line, cmd, n);
        ksnprintf(line + n, sizeof(line) - n, "; exec %s", shell_terminal_path());
        argv[1] = "-e"; argv[2] = (char*)shell_terminal_path(); argv[3] = "-c"; argv[4] = line;
        argc = 5;
    }
    if (pkg_run("terminal", argc, argv, 0, err, sizeof(err)) < 0) {
        klog("gui: terminal: %s\n", err);
        notify_post("terminal", "Terminal", err, 0);
        return 0;
    }
    return 1;
#else
    (void)cmd;
    notify_post("terminal", "Terminal", "The graphical Bash terminal requires 64-bit Banana OS.", 0);
    return 0;
#endif
}

void gui_terminal_run(const char* cmd) {
    bash_terminal(cmd);
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

int gui_open_target(const char* target) {
    switch (shortcut_kind(target)) {
    case SC_APP: {
        int act = builtin_act(target + 4);
        if (act >= 0) do_action(act);
        else run_app(target + 4);
        return 1;
    }
    case SC_WEB: return gui_open_browser(target);
    case SC_DIR: explorer_open(target); gui_raise_files(); return 1;
    case SC_FILE:
        if (fs_find_file(target) < 0 || shortcut_is(target)) return 0;   /* (moved or deleted; not a shortcut again) */
        explorer_launch(target);
        return 1;
    }
    return 0;
}

int gui_desktop_shortcut(const char* name, const char* target) {
    char p[FS_PATH_LEN];
    if (target && !strncasecmp(target, "app:", 4) && builtin_act(target + 4) < 0) {
        pkg_info_t info;
        if (pkg_get(target + 4, &info) == 0 && !pkg_is_desktop(&info)) return -1;
    }
    if (shortcut_create(DESKTOP_DIR, name, target, p, sizeof(p)) != 0) return -1;
    desk_refresh(1);
    return 0;
}

int gui_browser_focused(void) {
    return gfx_available() && g_gui_enabled && g_front_app == APP_BROWSER && browser_is_open() && tty_current() < 0;
}

static int desk_arrow(char code);

int gui_handle_arrow(char esc_code) {
    if(gfx_available() && g_gui_enabled && nettray_is_open()) {
        nettray_action(nettray_key(nettray_arrow_code(esc_code)));g_force_redraw=1;return 1;
    }
    if (g_ask.open && gfx_available() && g_gui_enabled) return 1;     /* (the dialog: its field takes no arrows) */
    if (gfx_available() && g_gui_enabled && !g_menu_open) {
        if (kb_modal()) {
            int code = esc_code == 'A' ? KB_UP : esc_code == 'B' ? KB_DOWN : esc_code == 'C' ? KB_RIGHT : esc_code == 'D' ? KB_LEFT :
                       esc_code == 'H' ? KB_HOME : esc_code == 'F' ? KB_END : esc_code == 'P' ? KB_DEL : esc_code == 'I' ? KB_PGUP : esc_code == 'G' ? KB_PGDN : 0;
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
    if (gfx_available() && g_ask.open) { ask_key(c); return 1; }     /* a dialog (New shortcut...) has the keys */
    if(gfx_available() && nettray_is_open()) {
        int code=c=='\t' && (keyboard_mods()&1)?KB_BACKTAB:(unsigned char)c;
        nettray_action(nettray_key(code));g_force_redraw=1;return 1;
    }
    if (gfx_available() && kb_modal() && !g_menu_open && c != 20) {
        int code = c == '\n' ? KB_ENTER : c == 27 ? KB_ESC : c == '\t' ? ((keyboard_mods() & 1) ? KB_BACKTAB : KB_TAB) : c == ' ' ? KB_SPACE : 0;
        if (!code && g_cb_open && c > ' ' && c < 127) code = c;
        if (!code && ep_is_open() && ((c > ' ' && c < 127) || c == '\b' || c == 127)) code = c;
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
 * minimize / bring back; Win+V the clipboard history; Win+. (or Win+;)
 * the emoji panel (kernel/emojipick.c); the Menu key
 * (Shift+F10) - a right-click menu. */

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

static int kb_modal(void) { return g_np_open || g_cb_open || ep_is_open() || cal_is_open() || g_sw_open || g_tv_open || g_bar_kb || ctxmenu_is_open(); }

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
    if (g_np_open) return np_key(code);
    if (g_cb_open) return cb_key(code);
    if (ep_is_open()) return ep_key(code);
    if (cal_is_open()) return cal_key(code);
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
    /* Ctrl+Alt+F1..F6: a console (the desktop goes, as stopx) */
    if (c >= KEYF_F1 && c < KEYF_F1 + VCONS_SCREENS && (k & KEYF_CTRL) && (k & KEYF_ALT) && vcons_running()) {
        gui_set_enabled(0);
        vcons_switch(c - KEYF_F1);
        return 1;
    }
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
    case 'n': np_toggle(); break;
    case 'v': g_np_open = 0; ep_close(); cb_toggle(); break;
    case ' ': {                                      /* Win+Space: English (US) and back */
        kstrlcpy(g_osd_text, keyboard_toggle_layout(), sizeof(g_osd_text));
        g_desk_osd = 1;
        g_desk_osd_until = timer_ms() + 1200;
        break;
    }
    case '.':
    case ';':
        g_cb_open = g_tv_open = g_sw_open = g_bar_kb = 0;
        ep_toggle();
        break;
    case 's':
        if (keyboard_mods() & 1) g_screenshot_requested = 1; /* Win+Shift+S */
        break;
    case 'm': for (int j = 0; j < g_ntb; j++) win_minimize(g_tb[j]); g_front_app = -1; break;
    case 'e': do_action(ACT_FILES); break;
    case 'i': do_action(ACT_SETTINGS); break;
    case 'l': do_action(ACT_LOCK); break;
    case 'r': do_action(ACT_TERMINAL); break;
    case 't': g_bar_kb = g_ntb > 0; g_bar_sel = 0; break;
    case KEYW_LEFT:
    case KEYW_RIGHT: {
        int mods = keyboard_mods(), d = g_cur_desk + (c == KEYW_LEFT ? -1 : 1);
        if (mods & 2) {                              /* Win+Ctrl(+Shift)+Left / Right: the desktops */
            if (d < 0 || d >= NDESK) break;
            int f = (mods & 1) ? wm_target(win_front()) : 0;
            if (f) wm_to_desk(f, d);                 /* (+Shift: the front window goes along) */
            desk_switch(d);
            if (f) win_activate(f);
        } else {
            wm_arrow(c);                             /* the left / right half, and back */
        }
        break;
    }
    case KEYW_DOWN: wm_arrow(c); break;              /* restore a snapped one, else minimize */
    case KEYW_UP: {
        if (win_front()) { wm_arrow(c); break; }     /* maximize the front window */
        mru_update();                                /* none: the last one minimized comes back */
        for (int i = 0; i < g_nmru; i++) if (win_minimized(g_mru[i])) { win_activate(g_mru[i]); break; }
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
    uint32_t bg = sel ? TC(0x405478) : TC(0x262D38);
    draw_bevel_box(x, y, w, h, bg, sel ? TC(0x90B8F0) : TC(0x505E74), TC(0x10151D));
    if (sel) {
        gfx_fill_rect(x - 3, y - 3, w + 6, 3, TC(0xFFD34E)); gfx_fill_rect(x - 3, y + h, w + 6, 3, TC(0xFFD34E));
        gfx_fill_rect(x - 3, y - 3, 3, h + 6, TC(0xFFD34E)); gfx_fill_rect(x + w, y - 3, 3, h + 6, TC(0xFFD34E));
    }
    int saved = g_is;
    g_is = big ? 2 : 1;
    draw_win_glyph(handle, x + w / 2 - (big ? 14 : 7), y + (big ? 16 : 10), bg);
    g_is = saved;
    char t[48];
    win_title(handle, t, sizeof(t));
    int maxc = (w - 12) / 8;
    if (maxc > 0 && (int)strlen(t) > maxc) { t[maxc] = 0; if (maxc > 1) t[maxc - 1] = '.'; }
    gfx_draw_text(x + (w - (int)strlen(t) * 8) / 2, y + h - (big ? 22 : 16), t, TC(0xE8EEF6), bg);
    if (win_minimized(handle)) gfx_draw_text(x + (w - 9 * 8) / 2, y + h - 10, _("minimized"), TC(0x9AA6B6), bg);
}

/* ══ Win+V: the clipboard history ════════════════════════════════════
 * A panel above the taskbar, at the right: what was copied, the newest
 * first. Enter (or a click) puts the entry on the clipboard and pastes it
 * into the window in front (Ctrl+V); Delete removes it, P pins it (pinned
 * entries stay through "Clear all" and restarts). */

#define CB_ROW_H   40
#define CB_HEAD_H  34
#define CB_FOOT_H  22
#define CB_BG      TC(0x1B212B)
#define CB_ROW     TC(0x262D38)
#define CB_ROW_SEL TC(0x405478)

static void cb_geom(const fb_info_t* fi, int* px, int* py, int* pw, int* ph, int* rows) {
    int W = (int)fi->width, H = (int)fi->height;
    int w = W - 24 < 380 ? W - 24 : 380;
    int max_rows = (H - BAR_H - 24 - CB_HEAD_H - CB_FOOT_H) / CB_ROW_H;
    if (max_rows > 8) max_rows = 8;
    if (max_rows < 1) max_rows = 1;
    int n = clipboard_hist_count();
    int r = n < max_rows ? n : max_rows;
    *rows = r;
    *pw = w;
    *ph = CB_HEAD_H + (r ? r * CB_ROW_H : 52) + CB_FOOT_H;
    *px = W - w - 12;
    *py = H - BAR_H - *ph - 10;
}

static void cb_clamp(int rows) {
    int n = clipboard_hist_count();
    if (g_cb_sel >= n) g_cb_sel = n - 1;
    if (g_cb_sel < 0) g_cb_sel = 0;
    if (g_cb_sel < g_cb_top) g_cb_top = g_cb_sel;
    if (rows > 0 && g_cb_sel >= g_cb_top + rows) g_cb_top = g_cb_sel - rows + 1;
    if (g_cb_top > n - rows) g_cb_top = n - rows;
    if (g_cb_top < 0) g_cb_top = 0;
}

static void cb_toggle(void) {
    if (g_cb_open) { g_cb_open = 0; return; }
    clipboard_hist_load();
    g_cb_open = 1;
    g_cb_sel = g_cb_top = 0;
    g_tv_open = g_sw_open = g_bar_kb = 0;
}

/* entry i onto the clipboard, and into the window in front */
static void cb_paste(int i) {
    g_cb_open = 0;
    if (clipboard_hist_use(i) == 0) keyboard_inject("\x16");     /* Ctrl+V */
}

/* up to two lines of an entry, its line breaks and tabs as spaces */
static void cb_preview(const char* s, int cols, char lines[2][96]) {
    lines[0][0] = lines[1][0] = 0;
    if (cols > 95) cols = 95;
    int l = 0, n = 0, more = 0;
    for (const char* p = s; *p; p++) {
        char c = *p == '\n' || *p == '\r' || *p == '\t' ? ' ' : *p;
        if (c == ' ' && (n == 0 || lines[l][n - 1] == ' ')) continue;     /* (runs of spaces as one) */
        int clen = (unsigned char)c < 0x80 ? 1 : (unsigned char)c >= 0xF0 ? 4 : (unsigned char)c >= 0xE0 ? 3 : 2;
        int used = u8_width(lines[l], n);
        if (used >= cols) {
            if (l == 1) { more = 1; break; }
            l = 1;
            n = 0;
            if (c == ' ') continue;                          /* (the second line starts with a word) */
        }
        if (n + clen >= 95) { more = 1; break; }
        for (int k = 0; k < clen && p[k]; k++) lines[l][n++] = p[k];
        p += clen - 1;
        lines[l][n] = 0;
    }
    if (more) {
        char* t = lines[l];
        int m = (int)strlen(t);
        while (m > 0 && u8_width(t, m) > cols - 3) { m--; while (m > 0 && ((unsigned char)t[m] & 0xC0) == 0x80) m--; }
        t[m] = 0;
        kstrlcat(t, "...", 96);
    }
}

/* a push pin, filled when pinned */
static void cb_draw_pin(int x, int y, int on, uint32_t bg) {
    uint32_t c = on ? TC(0xFFD34E) : TC(0x7A8699);
    gfx_fill_rect(x + 2, y, 8, 2, c);
    gfx_fill_rect(x + 3, y + 2, 6, 5, on ? c : bg);
    gfx_fill_rect(x + 3, y + 2, 1, 5, c);
    gfx_fill_rect(x + 8, y + 2, 1, 5, c);
    gfx_fill_rect(x, y + 7, 12, 2, c);
    gfx_fill_rect(x + 5, y + 9, 2, 5, c);
}

static void cb_draw(const fb_info_t* fi) {
    int px, py, pw, ph, rows;
    cb_geom(fi, &px, &py, &pw, &ph, &rows);
    cb_clamp(rows);
    draw_bevel_box(px, py, pw, ph, CB_BG, TC(0x505E74), TC(0x0C0F14));
    gfx_draw_text(px + 14, py + 13, _("Clipboard"), TC(0xFFFFFF), CB_BG);
    int n = clipboard_hist_count();
    int bw = 10 * 8, bx = px + pw - bw - 12;
    draw_bevel_box(bx, py + 7, bw, 20, n ? TC(0x303740) : TC(0x262B33), TC(0x535D6E), TC(0x15191F));
    gfx_draw_text(bx + (bw - 9 * 8) / 2, py + 13, _("Clear all"), n ? TC(0xE8EEF6) : TC(0x687384), n ? TC(0x303740) : TC(0x262B33));
    int y = py + CB_HEAD_H;
    if (!n) {
        gfx_draw_text(px + 14, y + 10, _("Nothing copied yet."), TC(0xE8EEF6), CB_BG);
        gfx_draw_text(px + 14, y + 28, _("Copy text (Ctrl+C) to see it here."), TC(0x9AA6B6), CB_BG);
    }
    char* buf = (char*)kmalloc(400);
    for (int r = 0; r < rows && buf; r++, y += CB_ROW_H) {
        int i = g_cb_top + r, pinned = 0;
        uint32_t len = 0;
        if (clipboard_hist_peek(i, buf, 400, &len, &pinned) < 0) break;
        int sel = i == g_cb_sel;
        uint32_t bg = sel ? CB_ROW_SEL : CB_ROW;
        draw_bevel_box(px + 8, y + 2, pw - 16, CB_ROW_H - 4, bg, sel ? TC(0x90B8F0) : TC(0x384252), TC(0x10151D));
        char lines[2][96];
        cb_preview(buf, (pw - 16 - 52) / 8, lines);
        if (!lines[0][0]) kstrlcpy(lines[0], "(spaces only)", sizeof(lines[0]));
        gfx_draw_text(px + 16, y + 9, lines[0], TC(0xE8EEF6), bg);
        if (lines[1][0]) gfx_draw_text(px + 16, y + 23, lines[1], TC(0xC2CCD8), bg);
        cb_draw_pin(px + pw - 46, y + 12, pinned, bg);
        gfx_draw_text(px + pw - 24, y + 15, "x", TC(0xC2CCD8), bg);
    }
    kfree(buf);
    if (n > rows) {
        char m[32];
        ksnprintf(m, sizeof(m), "%d-%d of %d", g_cb_top + 1, g_cb_top + rows, n);
        gfx_draw_text(px + pw - 12 - (int)strlen(m) * 8, py + ph - 15, m, TC(0x9AA6B6), CB_BG);
    }
    gfx_draw_text(px + 14, py + ph - 15, _("Enter: paste  Del: delete  P: pin"), TC(0x9AA6B6), CB_BG);
}

static int cb_key(int code) {
    int n = clipboard_hist_count();
    if (code == KB_ESC) g_cb_open = 0;
    else if (code == KB_UP) g_cb_sel--;
    else if (code == KB_DOWN || code == KB_TAB) g_cb_sel++;
    else if (code == KB_BACKTAB) g_cb_sel--;
    else if (code == KB_HOME) g_cb_sel = 0;
    else if (code == KB_END) g_cb_sel = n - 1;
    else if (code == KB_PGUP) g_cb_sel -= 8;
    else if (code == KB_PGDN) g_cb_sel += 8;
    else if ((code == KB_ENTER || code == KB_SPACE) && n) cb_paste(g_cb_sel);
    else if (code == KB_DEL && n) clipboard_hist_delete(g_cb_sel);
    else if ((code == 'p' || code == 'P') && n) {
        int pinned = 0;
        clipboard_hist_peek(g_cb_sel, NULL, 0, NULL, &pinned);
        clipboard_hist_pin(g_cb_sel, !pinned);
    }
    if (g_cb_sel >= clipboard_hist_count()) g_cb_sel = clipboard_hist_count() - 1;
    if (g_cb_sel < 0) g_cb_sel = 0;
    return 1;
}

/* a click: an entry pastes, its pin pins, its x deletes; outside, the panel closes */
static void cb_click(const fb_info_t* fi, int mx, int my) {
    int px, py, pw, ph, rows;
    cb_geom(fi, &px, &py, &pw, &ph, &rows);
    if (mx < px || mx >= px + pw || my < py || my >= py + ph) { g_cb_open = 0; return; }
    int bw = 10 * 8, bx = px + pw - bw - 12;
    if (my >= py + 7 && my < py + 27 && mx >= bx && mx < bx + bw) { clipboard_hist_clear(); g_cb_sel = g_cb_top = 0; return; }
    int r = (my - py - CB_HEAD_H) / CB_ROW_H;
    if (my < py + CB_HEAD_H || r < 0 || r >= rows) return;
    int i = g_cb_top + r;
    g_cb_sel = i;
    if (mx >= px + pw - 52 && mx < px + pw - 30) {
        int pinned = 0;
        if (clipboard_hist_peek(i, NULL, 0, NULL, &pinned) == 0) clipboard_hist_pin(i, !pinned);
    } else if (mx >= px + pw - 30) {
        clipboard_hist_delete(i);
    } else {
        cb_paste(i);
    }
}

/* the mouse wheel over the panel scrolls it */
static int cb_wheel(const fb_info_t* fi, int mx, int my, int dz) {
    int px, py, pw, ph, rows;
    cb_geom(fi, &px, &py, &pw, &ph, &rows);
    if (mx < px || mx >= px + pw || my < py || my >= py + ph) return 0;
    g_cb_sel += dz > 0 ? 1 : -1;
    cb_clamp(rows);
    return 1;
}

#define NP_ROW_H   44
#define NP_HEAD_H  34
#define NP_FOOT_H  22
#define NP_BG      TC(0x001B212Bu)
#define NP_ROW     TC(0x00262D38u)
#define NP_ROW_SEL TC(0x00405478u)

static void np_geom(const fb_info_t* fi, int* px, int* py, int* pw, int* ph, int* rows) {
    int W = (int)fi->width, H = (int)fi->height;
    int w = W - 24 < 360 ? W - 24 : 360;
    int max_rows = (H - BAR_H - 24 - NP_HEAD_H - NP_FOOT_H) / NP_ROW_H;
    if (max_rows > 6) max_rows = 6;
    if (max_rows < 1) max_rows = 1;
    int n = notify_count();
    int r = n < max_rows ? n : max_rows;
    *rows = r;
    *pw = w;
    *ph = NP_HEAD_H + (r ? r * NP_ROW_H : 52) + NP_FOOT_H;
    *px = W - w - 12;
    *py = H - BAR_H - *ph - 10;
}

static void np_toggle(void) {
    if (g_np_open) { g_np_open = 0; return; }
    g_np_open = 1;
    g_np_sel = 0;
    g_cb_open = g_tv_open = g_sw_open = g_bar_kb = 0;
    ep_close(); cal_close();
}

static void np_draw(const fb_info_t* fi) {
    int px, py, pw, ph, rows;
    np_geom(fi, &px, &py, &pw, &ph, &rows);
    int n = notify_count();
    if (g_np_sel >= n) g_np_sel = n > 0 ? n - 1 : 0;
    if (g_np_sel < 0) g_np_sel = 0;

    draw_bevel_box(px, py, pw, ph, NP_BG, TC(0x00505E74u), TC(0x0010151Du));
    gfx_draw_text(px + 14, py + 13, "Notifications", TC(0x00FFFFFFu), NP_BG);

    int bw = 10 * 8, bx = px + pw - bw - 12;
    draw_bevel_box(bx, py + 7, bw, 20, n ? TC(0x00303740u) : TC(0x00262B33u), TC(0x00535D6Eu), TC(0x0015191Fu));
    gfx_draw_text(bx + (bw - 9 * 8) / 2, py + 13, "Clear all", n ? TC(0x00E8EEF6u) : TC(0x00687384u), n ? TC(0x00303740u) : TC(0x00262B33u));

    int y = py + NP_HEAD_H;
    if (!n) {
        gfx_draw_text(px + 14, y + 10, "No notifications.", TC(0x00E8EEF6u), NP_BG);
        gfx_draw_text(px + 14, y + 28, "Recent alerts will appear here.", TC(0x009AA6B6u), NP_BG);
    }

    int first = g_np_sel >= rows && rows > 0 ? g_np_sel - rows + 1 : 0;
    for (int r = 0; r < rows; r++, y += NP_ROW_H) {
        notify_item_t item;
        if (notify_peek(first + r, &item) < 0) break;
        int sel = (first + r == g_np_sel);
        uint32_t bg = sel ? NP_ROW_SEL : NP_ROW;
        draw_bevel_box(px + 8, y + 2, pw - 16, NP_ROW_H - 4, bg, sel ? TC(0x0090B8F0u) : TC(0x00384252u), TC(0x0010151Du));

        char hdr[48];
        ksnprintf(hdr, sizeof(hdr), "%s: %s", item.app, item.title);
        if (strlen(hdr) > 36) { hdr[33] = '.'; hdr[34] = '.'; hdr[35] = '.'; hdr[36] = 0; }
        gfx_draw_text(px + 16, y + 8, hdr, TC(0x00FFFFFFu), bg);

        char body[48];
        kstrlcpy(body, item.body, sizeof(body));
        if (strlen(body) > 36) { body[33] = '.'; body[34] = '.'; body[35] = '.'; body[36] = 0; }
        gfx_draw_text(px + 16, y + 24, body, TC(0x00C2CCD8u), bg);
    }

    gfx_draw_text(px + 14, py + ph - 15, "Enter: open  Esc: close", TC(0x009AA6B6u), NP_BG);
}

static int np_key(int code) {
    int n = notify_count();
    if (code == KB_ESC) g_np_open = 0;
    else if (code == KB_UP) g_np_sel--;
    else if (code == KB_DOWN || code == KB_TAB) g_np_sel++;
    else if (code == KB_HOME) g_np_sel = 0;
    else if (code == KB_END) g_np_sel = n - 1;
    else if ((code == KB_ENTER || code == KB_SPACE) && n > 0) {
        notify_item_t item;
        if (notify_peek(g_np_sel, &item) == 0) {
            g_np_open = 0;
            notify_trigger_action(item.id);
        }
    } else if (code == KB_DEL && n > 0) {
        notify_clear_history();
        g_np_sel = 0;
    }
    if (g_np_sel >= notify_count()) g_np_sel = notify_count() - 1;
    if (g_np_sel < 0) g_np_sel = 0;
    return 1;
}

static void np_click(const fb_info_t* fi, int mx, int my) {
    int px, py, pw, ph, rows;
    np_geom(fi, &px, &py, &pw, &ph, &rows);
    if (mx < px || mx >= px + pw || my < py || my >= py + ph) { g_np_open = 0; return; }
    int bw = 10 * 8, bx = px + pw - bw - 12;
    if (my >= py + 7 && my < py + 27 && mx >= bx && mx < bx + bw) {
        notify_clear_history();
        g_np_sel = 0;
        return;
    }
    int r = (my - py - NP_HEAD_H) / NP_ROW_H;
    if (my < py + NP_HEAD_H || r < 0 || r >= rows) return;
    notify_item_t item;
    int first = g_np_sel >= rows && rows > 0 ? g_np_sel - rows + 1 : 0;
    if (notify_peek(first + r, &item) == 0) {
        g_np_open = 0;
        notify_trigger_action(item.id);
    }
}

static int np_wheel(const fb_info_t* fi, int mx, int my, int dz) {
    int px, py, pw, ph, rows;
    np_geom(fi, &px, &py, &pw, &ph, &rows);
    if (mx < px || mx >= px + pw || my < py || my >= py + ph) return 0;
    g_np_sel += dz > 0 ? 1 : -1;
    int n = notify_count();
    if (g_np_sel >= n) g_np_sel = n > 0 ? n - 1 : 0;
    if (g_np_sel < 0) g_np_sel = 0;
    return 1;
}

static void kb_draw_overlays(const fb_info_t* fi) {
    if (g_np_open) np_draw(fi);
    int W = (int)fi->width, H = (int)fi->height;
    if (g_sw_open && g_nlist) {
        int cw = 132, ch = 84, gap = 10;
        int n = g_nlist, per = (W - 80) / (cw + gap);
        if (per < 1) per = 1;
        if (n > per) n = per;
        int first = g_sw_sel >= n ? g_sw_sel - n + 1 : 0;
        int pw = n * (cw + gap) + gap + 20, ph = ch + 56;
        int px = (W - pw) / 2, py = (H - ph) / 2;
        draw_bevel_box(px, py, pw, ph, TC(0x1B212B), TC(0x505E74), TC(0x0C0F14));
        char t[64];
        win_title(g_list[g_sw_sel], t, sizeof(t));
        gfx_draw_text(px + (pw - (int)strlen(t) * 8) / 2, py + 12, t, TC(0xFFFFFF), TC(0x1B212B));
        for (int i = 0; i < n; i++)
            kb_card(px + 20 + i * (cw + gap), py + 36, cw, ch, g_list[first + i], first + i == g_sw_sel, 1);
    }
    if (g_tv_open) {
        /* everything dimmed, the windows as cards */
        uint32_t* t;
        int stride, tw, th;
        t = fb_target(&stride, &tw, &th);
        if (t) for (int y = 0; y < th - BAR_H; y++) for (int x = 0; x < tw; x++) { uint32_t p = t[y * stride + x]; t[y * stride + x] = (p >> 2) & 0x003F3F3Fu; }
        gfx_draw_text_scaled(40, 30, 2, _("Task view"), TC(0xFFFFFF), 0x00000000u);
        gfx_draw_text(40, 66, _("Arrows choose, Enter switches, Delete closes, Esc goes back"), TC(0x9AA6B6), 0x00000000u);
        if (!g_nlist) gfx_draw_text(40, 110, _("No windows are open."), TC(0xE8EEF6), 0);
        for (int i = 0; i < g_nlist; i++) {
            int x, y, cw, ch;
            tv_card(fi, i, &x, &y, &cw, &ch);
            if (y + ch > H - BAR_H - 10) break;
            kb_card(x, y, cw, ch, g_list[i], i == g_tv_sel, 1);
        }
    }
    if (g_cb_open) cb_draw(fi);
    if (ep_is_open()) ep_draw(fi, BAR_H);
    if (g_bar_kb && g_bar_sel >= 0 && g_bar_sel < g_ntb) {
        int bw = tb_btn_w(fi), x = TB_X + g_bar_sel * bw, y = H - BAR_H + 1;
        gfx_fill_rect(x - 1, y, bw - 2, 2, TC(0xFFD34E));
        gfx_fill_rect(x - 1, y + BAR_H - 4, bw - 2, 2, TC(0xFFD34E));
        gfx_fill_rect(x - 1, y, 2, BAR_H - 2, TC(0xFFD34E));
        gfx_fill_rect(x + bw - 5, y, 2, BAR_H - 2, TC(0xFFD34E));
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
    if (d->link) { gui_open_target(d->target); return; }        /* a shortcut: what it points to */
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
    /* programs among them: off the desktop (the program stays) */
    char keys[16][56];
    int nk = 0;
    for (int i = 0; i < g_ndesk && nk < 16; i++)
        if (desk_marked(i) && g_desk[i].kind == DK_APP) desk_key(&g_desk[i], keys[nk++], sizeof(keys[0]));
    int n = desk_marked_paths(p, 64);
    if (n || nk) layout_pin_all();                   /* (the other icons stay where they are, as on Windows) */
    for (int i = 0; i < n; i++) trash_put(p[i]);
    for (int k = 0; k < nk; k++) {
        if (is_hidden(keys[k]) || g_nhidden >= HIDDEN_MAX) continue;
        kstrlcpy(g_hidden[g_nhidden++], keys[k], sizeof(g_hidden[0]));
    }
    if (nk) { layout_pin_all(); hidden_save(); }
    if (n || nk) { g_desk_marks_clear(); g_desk_sel = -1; desk_refresh(1); explorer_refresh(); }
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
    if (fileops_unique(DESKTOP_DIR, folder ? "New folder" : "New text document.txt", p, sizeof(p)) < 0) {
        notify_post("desktop", "Desktop", "Cannot create an item: no available name or path too long.", 0);
        return;
    }
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

/* the window a drop at (mx, my) lands on: the app in front, else a
 * terminal (*term = its index + 1), else another app window */
static int drop_target(int mx, int my, int* term) {
    *term = 0;
    if (g_front_app >= 0 && app_visible(g_front_app) && g_apps[g_front_app].contains(mx, my)) return g_front_app;
    int wi = term_at(mx, my);
    if (wi) { *term = wi; return -1; }
    return app_at(mx, my, 0);
}

/* what a drop there does besides Files and the desktop */
enum { DROP_NONE, DROP_TYPE, DROP_OPEN };
static int drop_kind(int mx, int my) {
    if (!g_ndnd) return DROP_NONE;
    int wi, a = drop_target(mx, my, &wi);
    if (wi) return DROP_TYPE;
    if (a == APP_APPWIN) return DROP_OPEN;
    if ((a == APP_NOTEPAD || a == APP_BROWSER) && !fileops_is_dir(g_dnd[0])) return DROP_OPEN;
    return DROP_NONE;
}

/* files let go over a window other than Files: a terminal gets their
 * paths typed at its prompt (quoted when they have spaces), Notepad and
 * the Browser open the first one, an app's window gets BANANA_EV_DROP */
static int drop_on_window(int mx, int my) {
    int k = drop_kind(mx, my);
    if (k == DROP_NONE) return 0;
    int wi, a = drop_target(mx, my, &wi);
    if (k == DROP_TYPE) {
        char line[1024];
        int o = 0;
        for (int i = 0; i < g_ndnd && o < (int)sizeof(line) - FS_PATH_LEN - 4; i++)
            o += ksnprintf(line + o, sizeof(line) - (size_t)o, strchr(g_dnd[i], ' ') ? "\"%s\" " : "%s ", g_dnd[i]);
        bring_term_front(wi - 1);
        g_front_app = -1;
        keyboard_inject(line);
    } else if (a == APP_APPWIN) {
        if (appwin_drop(mx, my, (const char (*)[FS_PATH_LEN])g_dnd, g_ndnd)) raise_app(APP_APPWIN);
    } else if (a == APP_NOTEPAD) gui_open_notepad(g_dnd[0]);
    else gui_open_browser(g_dnd[0]);
    return 1;
}

static void dnd_drop(const fb_info_t* fi, int mx, int my) {
    int copy = (keyboard_mods() & 2) != 0;              /* Ctrl: a copy */
    int bar_y = (int)fi->height - BAR_H;
    g_desk_drop = -1;
    if (my < bar_y && drop_on_window(mx, my)) {
        /* (a terminal, Notepad, the Browser or an app took it) */
    } else if (over_files(mx, my)) {
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
        } else if (i >= 0 && !self && (g_desk[i].kind == DK_APP || g_desk[i].link == SC_APP) && g_ndnd) {
            /* a file dropped on a program (or its shortcut): it opens it (Banana Code, Photos...) */
            int act;
            char app[PKG_NAME_MAX];
            desk_program(i, &act, app, sizeof(app));
            if (act == ACT_RUN_APP) {
                char err[96];
                char* argv[2] = { app, g_dnd[0] };
                pkg_run(app, 2, argv, 1, err, sizeof(err));
            } else if (act == ACT_NOTEPAD) gui_open_notepad(g_dnd[0]);
            else if (act == ACT_BROWSER) gui_open_browser(g_dnd[0]);
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
        {   /* held over a window's taskbar button: that window comes up, to drop on it */
            static int tb = -1;
            static uint32_t since;
            int j = my >= (int)fi->height - BAR_H ? tb_button_at(fi, mx, my) : -1;
            if (j != tb) { tb = j; since = timer_ms(); }
            else if (j >= 0 && since && timer_ms() - since > 600) { win_activate(g_tb[j]); since = 0; }
        }
        if (over_files(mx, my)) explorer_drag_over(mx, my);
        int d = -1;
        if (!over_window(mx, my)) {
            int i = icon_at(mx, my);
            if (i >= 0 && (g_desk[i].kind == DK_TRASH || g_desk[i].kind == DK_DIR || ((g_desk[i].kind == DK_APP || g_desk[i].link == SC_APP) && g_dnd_src == GUI_DRAG_FILES))) d = i;
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

/* the desktop's selection box: on the desktop, over its icons and under
 * every window (it was drawn last, over the windows too), never on the
 * taskbar */
static void dband_draw(const fb_info_t* fi) {
    if (!g_dband) return;
    int x0 = g_dband_x0 < g_dband_x1 ? g_dband_x0 : g_dband_x1, x1 = g_dband_x0 < g_dband_x1 ? g_dband_x1 : g_dband_x0;
    int y0 = g_dband_y0 < g_dband_y1 ? g_dband_y0 : g_dband_y1, y1 = g_dband_y0 < g_dband_y1 ? g_dband_y1 : g_dband_y0;
    int bottom = (int)fi->height - BAR_H;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)fi->width) x1 = (int)fi->width;
    if (y1 > bottom) y1 = bottom;
    if (x1 <= x0 || y1 <= y0) return;
    int stride, tw, th;
    uint32_t* t = fb_target(&stride, &tw, &th);
    for (int y = y0; t && y < y1 && y < th; y++)
        for (int x = x0; x < x1 && x < tw; x++) {
            uint32_t p = t[y * stride + x];
            t[y * stride + x] = ((p & 0x00FEFEFEu) >> 1) + ((0x006FA8E8u & 0x00FEFEFEu) >> 1);
        }
    gfx_fill_rect(x0, y0, x1 - x0, 1, TC(0x99CCFF)); gfx_fill_rect(x0, y1 - 1, x1 - x0, 1, TC(0x99CCFF));
    gfx_fill_rect(x0, y0, 1, y1 - y0, TC(0x99CCFF)); gfx_fill_rect(x1 - 1, y0, 1, y1 - y0, TC(0x99CCFF));
}

static void dnd_draw(const fb_info_t* fi, int mx, int my) {
    (void)fi;
    if (!g_dnd_on) return;
    /* what is dragged, beside the pointer */
    int n = g_dnd_src == GUI_DRAG_DESKTOP ? g_ndnd_icons : g_ndnd;
    int x = mx + 14, y = my + 14;
    char t[64];
    int copy = (keyboard_mods() & 2) != 0;
    int dk = my < (int)fb_info()->height - BAR_H ? drop_kind(mx, my) : DROP_NONE;
    if (g_ndnd && (g_desk_drop >= 0 && g_desk[g_desk_drop].kind == DK_TRASH)) ksnprintf(t, sizeof(t), "Delete %d", g_ndnd);
    else if (dk == DROP_TYPE) ksnprintf(t, sizeof(t), "Type the path%s", g_ndnd > 1 ? "s" : "");
    else if (dk == DROP_OPEN) ksnprintf(t, sizeof(t), "Open %s", fileops_base(g_dnd[0]));
    else if (n == 1) ksnprintf(t, sizeof(t), "%s%s", copy ? "+ " : "", g_ndnd ? fileops_base(g_dnd[0]) : g_desk[g_dnd_icons[0]].label);
    else ksnprintf(t, sizeof(t), "%s%d items", copy ? "+ " : "", n);
    if (strlen(t) > 22) { t[20] = '.'; t[21] = '.'; t[22] = 0; }
    int w = 44 + (int)strlen(t) * 8;
    gfx_fill_rect(x, y, w, 38, TC(0x2A3A52));
    gfx_fill_rect(x, y, w, 1, TC(0x6B8AB8));
    gfx_fill_rect(x, y + 37, w, 1, TC(0x101820));
    fileicon_t k = g_ndnd ? (fileops_is_dir(g_dnd[0]) ? FI_FOLDER : fileicon_for_name(fileops_base(g_dnd[0]))) : FI_PROGRAM;
    fileicon_draw(k, x + 4, y + 3, 32);
    gfx_draw_text(x + 40, y + 15, t, TC(0xFFFFFF), TC(0x2A3A52));
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
        /* like Windows 7: the browser and Files to begin with, then the App Store */
        g_pins[0].act = ACT_BROWSER; g_pins[0].app[0] = 0;
        g_pins[1].act = ACT_FILES; g_pins[1].app[0] = 0;
        g_npins = 2;
        static pkg_info_t store;
        if (pkg_get("store", &store) == 0) {
            g_pins[2].act = ACT_RUN_APP;
            kstrlcpy(g_pins[2].app, "store", PKG_NAME_MAX);
            g_npins = 3;
        }
        char s[16];
        if (settings_get("taskbar_style", s, sizeof(s))) { uint32_t n = 0; if (k_parse_u32(s, &n) && n <= 2) g_tb_style = (int)n; }
        if (settings_get("taskbar_desks", s, sizeof(s))) g_tb_desks = strcmp(s, "0") != 0;
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
    if (settings_get("taskbar_desks", s, sizeof(s))) g_tb_desks = strcmp(s, "0") != 0;
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
        static const int MAP[APP_COUNT] = { ACT_FILES, ACT_BROWSER, ACT_NOTEPAD, ACT_APPS, ACT_TASKMGR, ACT_SETTINGS, -1, ACT_DEVMGR, ACT_DISKMGR, -1 };
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
        uint32_t bg = TC(0x192026);
        if (run) gfx_fill_rect(x + 4, bar_y + BAR_H - 4, PIN_W - 8, 2, TC(0x68A8F0));
        int saved = g_is;
        g_is = 1;
        desk_icon(g_pins[i].act, x + (PIN_W - 14) / 2 - 2, bar_y + 7, g_pins[i].app);
        g_is = saved;
        (void)bg;
    }
    if (g_npins) gfx_fill_rect(PINS_X + g_npins * PIN_W + 1, bar_y + 6, 1, BAR_H - 12, TC(0x4F5A6E));
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

/* the virtual desktops' buttons on the taskbar (Win+Ctrl+arrows still switch) */
void gui_set_taskbar_desks(int on) {
    g_tb_desks = on ? 1 : 0;
    settings_set("taskbar_desks", on ? "1" : "0");
    g_tb_gen++;
    g_force_redraw = 1;
}
int gui_taskbar_desks(void) { if (g_npins < 0) pins_load(); return g_tb_desks; }
