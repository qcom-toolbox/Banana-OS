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
#include "launcher.h"
#include "taskmgr.h"
#include "settings.h"
#include "pkg.h"
#include "gpu.h"
#include "audio.h"
#include "app.h"
#include "../net/net.h"
#include "../shell/shell.h"

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
#define DESK_MAX 48
typedef struct {
    int      act;                       /* ACT_*; ACT_RUN_APP: an installed app */
    char     label[48];
    char     app[PKG_NAME_MAX];
} desk_item_t;
static desk_item_t g_desk[DESK_MAX];
static int         g_ndesk, g_desk_sel = -1, g_last_click_i = -1;
static uint32_t    g_desk_sig, g_desk_checked, g_last_click_ms;
static int         g_sub_slot = -1;     /* the Start menu item whose submenu is open */

static int g_menu_open = 0;
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
    explorer_wheel, browser_wheel, notepad_wheel, NULL, NULL, NULL, NULL, appwin_wheel,
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

static void draw_icon_power(int x, int y, uint32_t bg) {
    (void)bg;
    icon_bevel(x, y, 12, 12, 0x00412A2Au, 0x00764A4Au, 0x00170D0Du);
    gfx_draw_text_scaled(x + 3 * g_is, y + 2 * g_is, g_is, "o", 0x00FFFFFFu, 0x00412A2Au);
}

/* Programs: a folder with app tiles in it */
static void draw_icon_programs(int x, int y, uint32_t bg) {
    (void)bg;
    IR(0, 1, 6, 2, 0x00D9B44Au);
    IR(0, 3, 14, 10, 0x00D9B44Au);
    IR(2, 5, 4, 3, 0x003A7BD5u);
    IR(8, 5, 4, 3, 0x0057B65Au);
    IR(2, 9, 4, 3, 0x00E07040u);
    IR(8, 9, 4, 3, 0x00F4F8FFu);
}

/* an installed app: a little window, its colour and initial from its name */
static const char* g_app_icon_name = "";
static void draw_icon_app(int x, int y, uint32_t bg) {
    (void)bg;
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
    terminal_vt_set_size(w->vt, (w->w - TERM_PAD * 2) / 8, (w->h - TERM_TITLE_H - TERM_PAD * 2) / 8);
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
    int max_cols = (w->w - TERM_PAD * 2) / 8, max_rows = (w->h - TERM_TITLE_H - TERM_PAD * 2) / 8;
    if (max_cols > tw) max_cols = tw;
    if (max_rows > th) max_rows = th;
    int x = (mx - (w->x + TERM_PAD)) / 8, y = (my - (w->y + TERM_TITLE_H + TERM_PAD)) / 8;
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

    int max_cols = cw / 8;
    int max_rows = ch / 8;
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
                gfx_draw_char(cx + x * 8, cy + y * 8, c ? c : ' ', 0x00101010u, 0x00C8D8F0u);
                continue;
            }
            /* the client area is already black: skip blank black cells */
            if ((c == ' ' || c == 0) && (color & 0xF0) == 0) continue;
            gfx_draw_char(cx + x * 8, cy + y * 8, c,
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
            gfx_fill_rect(cx + (int)cc * 8, cy + scr_row * 8 + 7, 8, 1, vga_color_rgb(fg));
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
#define START_W   88
#define TB_X      (START_X + START_W + 8)
#define TB_BTN_W  150

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
    /* Start */
    uint32_t sbg = g_menu_open ? 0x002E4A70u : 0x00354463u;
    draw_bevel_box(START_X, bar_y + 3, START_W, BAR_H - 6, sbg, 0x006B7892u, 0x00111824u);
    gfx_fill_rect(START_X + 8, bar_y + 9, 10, 10, 0x00F4D35Eu);
    gfx_fill_rect(START_X + 10, bar_y + 11, 6, 6, 0x00C9A227u);
    gfx_draw_text(START_X + 26, bar_y + 10, "banana", 0x00FFFFFFu, sbg);
    /* one button per window */
    int bw = tb_btn_w(fi), front = win_front();
    for (int j = 0; j < g_ntb; j++) {
        int h = g_tb[j];
        int x = TB_X + j * bw;
        int active = h == front, min = win_minimized(h);
        uint32_t base = active ? 0x00405478u : min ? 0x00222831u : 0x00303740u;
        uint32_t hi = active ? 0x00222A36u : 0x00535D6Eu, lo = active ? 0x006B7892u : 0x0015191Fu;
        draw_bevel_box(x, bar_y + 3, bw - 4, BAR_H - 6, base, hi, lo);
        if (bw >= 40) draw_win_glyph(h, x + 6, bar_y + 8, base);
        char t[48];
        win_title(h, t, sizeof(t));
        int maxc = (bw - 34) / 8;
        if (maxc < 1) continue;
        if ((int)strlen(t) > maxc) { t[maxc] = 0; if (maxc > 1) t[maxc - 1] = '.'; }
        gfx_draw_text(x + 26, bar_y + 10, t, min ? 0x009AA6B6u : 0x00E8EEF6u, base);
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
    v->ctx_sig = ctxmenu_signature();
    v->tb_gen = g_tb_gen;
}

static gui_view_t g_last_view;
static int        g_force_redraw = 1;

static void (*const g_menu_icons[MENU_ITEMS_MAX])(int, int, uint32_t) = {
    draw_icon_programs, draw_icon_terminal, draw_icon_files, draw_icon_browser, draw_icon_notepad,
    draw_icon_apps, draw_icon_taskmgr, draw_icon_settings, draw_icon_install, draw_icon_power,
};

static int start_menu_y(const fb_info_t* fi) { return (int)fi->height - BAR_H - START_MENU_H - 4; }

/* the Start menu item under (mx, my), or -1 */
static int start_item_at(const fb_info_t* fi, int mx, int my) {
    if (!g_menu_open || mx < START_X + 24 || mx >= START_X + START_MENU_W - 8) return -1;
    int menu_y = start_menu_y(fi);
    for (int i = 0; i < MENU_ITEMS; i++) {
        int y0 = menu_y + 8 + i * 28;
        if (my >= y0 && my < y0 + 24) return i;
    }
    return -1;
}

/* ── the desktop's icons ─────────────────────────────────────────────── */

static void desk_add(int act, const char* label, const char* app) {
    if (g_ndesk >= DESK_MAX) return;
    desk_item_t* d = &g_desk[g_ndesk++];
    d->act = act;
    kstrlcpy(d->label, label, sizeof(d->label));
    kstrlcpy(d->app, app ? app : "", sizeof(d->app));
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
    uint32_t sig = (uint32_t)g_ndesk;
    for (int i = 0; i < g_ndesk; i++)
        for (const char* p = g_desk[i].label; *p; p++) sig = sig * 31u + (uint8_t)*p;
    if (sig != g_desk_sig) { g_desk_sig = sig; if (g_desk_sel >= g_ndesk) g_desk_sel = -1; }
}

static int desk_rows(const fb_info_t* fi) {
    int r = ((int)fi->height - BAR_H - DESK_Y) / CELL_H;
    return r < 1 ? 1 : r;
}
static void desk_cell(const fb_info_t* fi, int i, int* x, int* y) {
    int rows = desk_rows(fi);
    *x = DESK_X + (i / rows) * CELL_W;
    *y = DESK_Y + (i % rows) * CELL_H;
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
        if (x + CELL_W > (int)fi->width) break;
        int sel = i == g_desk_sel;
        if (sel) gfx_fill_rect(x + (CELL_W - 36) / 2, y + 2, 36, 34, 0x00315A9Cu);   /* selected: tinted */
        desk_icon(g_desk[i].act, x + (CELL_W - 28) / 2, y + 5, g_desk[i].app);
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
    for (int i = 0; i < MENU_ITEMS; i++)
        if (MENU_ACTS[menu_idx(i)] == ACT_POWER) {
            g_menu_open = 1;
            g_menu_sel = i;
            open_submenu(fi, i);
            g_force_redraw = 1;
        }
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

static void gui_fkeys(const fb_info_t* fi) {
    if (g_osd_shown && (int32_t)(timer_ms() - g_osd_until) >= 0) { g_osd_shown = 0; g_force_redraw = 1; }
    for (int n = 0; n < 16; n++) {
        /* a console app in the front terminal takes them itself */
        if (g_front_app < 0 && front_term() >= 0 && app_console_focused()) return;
        int k = keyboard_take_fkey();
        if (!k) return;
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
    if (g_menu_open) {
        int menu_x = START_X, menu_y = start_menu_y(fi);
        draw_bevel_box(menu_x, menu_y, START_MENU_W, START_MENU_H, 0x001D232Cu, 0x00505E74u, 0x0010151Du);
        gfx_fill_rect(menu_x + 2, menu_y + 2, 18, START_MENU_H - 4, 0x00354463u);
        gfx_draw_text(menu_x + 5, menu_y + 8, "B", 0x00F4F8FFu, 0x00354463u);
        for (int i = 0; i < MENU_ITEMS; i++) {
            uint32_t bg = (g_menu_sel == i || g_sub_slot == i) ? 0x003A4A66u : 0x001D232Cu;
            if (MENU_ACTS[menu_idx(i)] == ACT_POWER)                 /* a line above Shut down */
                gfx_fill_rect(menu_x + 24, menu_y + 5 + i * 28, START_MENU_W - 32, 1, 0x00404B5Cu);
            gfx_fill_rect(menu_x + 24, menu_y + 8 + i * 28, START_MENU_W - 32, 24, bg);
            g_menu_icons[menu_idx(i)](menu_x + 28, menu_y + 14 + i * 28, bg);
            gfx_draw_text(menu_x + 48, menu_y + 16 + i * 28, MENU_LABELS[menu_idx(i)], 0x00E8EEF6u, bg);
            if (menu_has_sub(i)) gfx_draw_text(menu_x + START_MENU_W - 22, menu_y + 16 + i * 28, ">", 0x00B8C4D6u, bg);
        }
    }

    draw_volume_osd(fi);
    ctxmenu_draw();

    /* push backbuffer to framebuffer once per frame, then the cursor */
    fb_present();
    draw_cursor(mx, my);
}

/* ── right-click menus ─────────────────────────────────────────────── */

static void do_action(int act);

static void desktop_menu_cb(int id, void* arg) { (void)arg; do_action(id); }

static void open_desktop_menu(int mx, int my) {
    ctx_item_t items[] = {
        { "Terminal", ACT_TERMINAL, 0 },
        { "Files", ACT_FILES, 0 },
        { "Browser", ACT_BROWSER, 0 },
        { "Notepad", ACT_NOTEPAD, 0 },
        { "Apps", ACT_APPS, 0 },
        { CTX_SEP, 0, 0 },
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
static void icon_menu_cb(int id, void* arg) {
    (void)arg;
    int i = g_icon_menu_i;
    if (i < 0 || i >= g_ndesk) return;
    if (id == ICM_OPEN) {
        if (g_desk[i].act == ACT_RUN_APP) run_app(g_desk[i].app);
        else do_action(g_desk[i].act);
    } else if (id == ICM_REMOVE && g_desk[i].act == ACT_RUN_APP) {
        char msg[96];
        pkg_remove(g_desk[i].app, msg, sizeof(msg));
        g_desk_sel = -1;
        desk_refresh(1);
    }
}

static void open_icon_menu(int i, int mx, int my) {
    char open[64];
    ksnprintf(open, sizeof(open), "Open %s", g_desk[i].label);
    ctx_item_t items[3] = { { open, ICM_OPEN, 0 }, { CTX_SEP, 0, 0 }, { "Remove this app", ICM_REMOVE, 0 } };
    g_icon_menu_i = i;
    g_desk_sel = i;
    ctxmenu_open(mx, my, items, g_desk[i].act == ACT_RUN_APP ? 3 : 1, icon_menu_cb, NULL);
}

/* taskbar: Task Manager & co, or one window's button */
enum { TBM_TASKMGR = 1, TBM_SHOW_DESKTOP, TBM_RESTORE_ALL, TBM_RESTORE, TBM_MINIMIZE, TBM_CLOSE, TBM_QUIT, TBM_TERMINAL, TBM_LOCK };
static int g_tbmenu_win;

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
    }
}

static void open_taskbar_menu(const fb_info_t* fi, int mx, int my) {
    int j = tb_button_at(fi, mx, my);
    if (j >= 0) {
        g_tbmenu_win = g_tb[j];
        int min = win_minimized(g_tbmenu_win);
        ctx_item_t items[] = {
            { "Restore", TBM_RESTORE, !min && win_front() == g_tbmenu_win },
            { "Minimize", TBM_MINIMIZE, min },
            { CTX_SEP, 0, 0 },
            { "Close window", TBM_CLOSE, 0 },
            { CTX_SEP, 0, 0 },
            { "Task Manager", TBM_TASKMGR, 0 },
        };
        ctxmenu_open(mx, my, items, 6, taskbar_menu_cb, NULL);
        return;
    }
    ctx_item_t items[] = {
        { "Task Manager", TBM_TASKMGR, 0 },
        { CTX_SEP, 0, 0 },
        { "Show the desktop", TBM_SHOW_DESKTOP, g_ntb == 0 },
        { "Restore all windows", TBM_RESTORE_ALL, g_ntb == 0 },
        { CTX_SEP, 0, 0 },
        { "New terminal", TBM_TERMINAL, 0 },
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
        if (gui_notepad_focused() || gui_appwin_focused() ||
            ((g_front_app == APP_LAUNCHER || g_front_app == APP_TASKMGR || g_front_app == APP_FILES || g_front_app == APP_SETTINGS || g_front_app == APP_INSTALLER) &&
             app_visible(g_front_app) && tty_current() < 0)) {
            for (int k = 0; k < 64; k++) {
                char c = keyboard_try_getchar();
                if (!c) break;
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
        if (ms.dz) {
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

        ctxmenu_hover(mx, my);

        /* the Start menu follows the pointer, and its submenus open by
         * themselves under it - Windows 95 style */
        if (!ctxmenu_is_open()) g_sub_slot = -1;
        if (g_menu_open && !ctxmenu_contains(mx, my)) {
            static int hover_slot = -1;
            static uint32_t hover_since;
            int si = start_item_at(fi, mx, my);
            if (si != hover_slot) { hover_slot = si; hover_since = timer_ms(); }
            /* with a submenu open, another item takes over only once the
             * pointer rests on it (0.3 s): on the way to the submenu it may
             * cross the items below or above */
            if (si >= 0 && (g_sub_slot < 0 || timer_ms() - hover_since >= 300)) {
                g_menu_sel = si;
                if (menu_has_sub(si)) { if (g_sub_slot != si) open_submenu(fi, si); }
                else if (g_sub_slot >= 0) { ctxmenu_close(); g_sub_slot = -1; }
            }
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
                    g_menu_open = !g_menu_open;
                    if (g_menu_open) g_menu_sel = 0;
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
                int si = start_item_at(fi, mx, my);
                if (si >= 0) {
                    g_menu_sel = si;
                    menu_activate();
                    click = 0;
                } else if (mx >= START_X && mx < START_X + START_MENU_W && my >= start_menu_y(fi) &&
                           my < start_menu_y(fi) + START_MENU_H) {
                    click = 0;                /* the menu's own margin */
                }
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
                if (i >= 0 && i == g_last_click_i && now - g_last_click_ms < 500) {
                    g_last_click_i = -1;
                    if (g_desk[i].act == ACT_RUN_APP) run_app(g_desk[i].app);
                    else do_action(g_desk[i].act);
                } else {
                    g_last_click_i = i;
                    g_last_click_ms = now;
                }
                g_desk_sel = i;
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

int gui_handle_arrow(char esc_code) {
    if (!g_menu_open) return 0;
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
    /* Ctrl+T toggles Start menu */
    if (c == 20) {
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

    if (!g_menu_open) return 0;

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
