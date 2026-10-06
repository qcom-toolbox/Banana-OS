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
#include "kheap.h"
#include "appwin.h"
#include "winframe.h"
#include "ctxmenu.h"
#include "launcher.h"
#include "taskmgr.h"
#include "settings.h"
#include "../net/net.h"
#include "../shell/shell.h"

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

#define TASKBAR_ROW (VGA_HEIGHT - 1)

/* the Start menu and the desktop shortcuts: the same entries */
/* (About and Wallpaper live in Settings now: past MENU_ITEMS, reached from the desktop's menu) */
enum { ACT_TERMINAL = 0, ACT_FILES, ACT_BROWSER, ACT_NOTEPAD, ACT_APPS, ACT_TASKMGR, ACT_SETTINGS, ACT_QUIT, ACT_ABOUT, ACT_WALLPAPER };
#define MENU_ITEMS 8
static const char* const MENU_LABELS[MENU_ITEMS] = {
    "Terminal", "Files", "Browser", "Notepad", "Apps", "Task Manager", "Settings", "Exit to shell",
};
static const char* const ICON_LABELS[MENU_ITEMS] = {
    "Terminal", "Files", "Browser", "Notepad", "Apps", "Task Manager", "Settings", "Quit GUI",
};
#define START_MENU_W 236
#define START_MENU_H (16 + MENU_ITEMS * 28)
#define ICON_W 132
#define ICON_H 38
#define ICON_X 18
#define ICON_Y 22
#define ICON_STEP (ICON_H + 10)

static int g_menu_open = 0;
static int g_menu_sel = 0;     /* an ACT_* */
/* The app windows (Files, Browser, Notepad, Apps, Task Manager and the
 * windows of installed apps) and the terminal windows share one stacking
 * order: the app windows have their own back-to-front order, and at most
 * one of them - g_front_app - is above the terminals. */
enum { APP_FILES = 0, APP_BROWSER, APP_NOTEPAD, APP_LAUNCHER, APP_TASKMGR, APP_SETTINGS, APP_APPWIN, APP_COUNT };
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
    { appwin_is_open, appwin_draw, appwin_contains, appwin_click, appwin_mouse, appwin_signature, appwin_close_all, appwin_rclick, "App" },
};
static int g_front_app = -1;                        /* -1: a terminal window is in front */
static int g_app_order[APP_COUNT] = { APP_FILES, APP_BROWSER, APP_NOTEPAD, APP_LAUNCHER, APP_TASKMGR, APP_SETTINGS, APP_APPWIN };   /* back to front */
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
    explorer_wheel, browser_wheel, notepad_wheel, NULL, NULL, NULL, appwin_wheel,
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


static void draw_icon_terminal(int x, int y, uint32_t bg) {
    (void)bg;
    draw_bevel_box(x, y, 14, 12, 0x00161D28u, 0x00475A78u, 0x000E1118u);
    gfx_draw_text(x + 2, y + 2, ">", 0x00E8EEF6u, 0x00161D28u);
}

static void draw_icon_files(int x, int y, uint32_t bg) {
    (void)bg;
    gfx_fill_rect(x, y + 1, 6, 2, 0x00F4D35Eu);
    gfx_fill_rect(x, y + 3, 14, 9, 0x00F4D35Eu);
    gfx_fill_rect(x + 1, y + 4, 12, 1, 0x00FFF1A8u);
}

/* a sheet of paper with lines */
static void draw_icon_notepad(int x, int y, uint32_t bg) {
    (void)bg;
    gfx_fill_rect(x + 1, y, 11, 12, 0x00F2F2EAu);
    gfx_fill_rect(x + 1, y, 11, 2, 0x005A86C8u);
    for (int i = 0; i < 3; i++) gfx_fill_rect(x + 3, y + 4 + i * 3, 7, 1, 0x00808890u);
}

/* a little globe */
static void draw_icon_browser(int x, int y, uint32_t bg) {
    (void)bg;
    gfx_fill_rect(x + 4, y, 6, 12, 0x003A7BD5u);
    gfx_fill_rect(x + 1, y + 2, 12, 8, 0x003A7BD5u);
    gfx_fill_rect(x + 2, y + 1, 10, 10, 0x003A7BD5u);
    gfx_fill_rect(x + 3, y + 3, 4, 3, 0x0057B65Au);
    gfx_fill_rect(x + 8, y + 6, 3, 3, 0x0057B65Au);
    gfx_fill_rect(x + 1, y + 6, 12, 1, 0x00A9CCF5u);
    gfx_fill_rect(x + 6, y, 1, 12, 0x00A9CCF5u);
}

/* four tiles */
static void draw_icon_apps(int x, int y, uint32_t bg) {
    (void)bg;
    gfx_fill_rect(x + 1, y, 5, 5, 0x003A7BD5u);
    gfx_fill_rect(x + 8, y, 5, 5, 0x0057B65Au);
    gfx_fill_rect(x + 1, y + 7, 5, 5, 0x00F4D35Eu);
    gfx_fill_rect(x + 8, y + 7, 5, 5, 0x00E07040u);
}

/* a little CPU graph */
static void draw_icon_taskmgr(int x, int y, uint32_t bg) {
    (void)bg;
    draw_bevel_box(x, y, 14, 12, 0x00161D28u, 0x00475A78u, 0x000E1118u);
    gfx_fill_rect(x + 2, y + 7, 2, 3, 0x0057B65Au);
    gfx_fill_rect(x + 5, y + 4, 2, 6, 0x0057B65Au);
    gfx_fill_rect(x + 8, y + 6, 2, 4, 0x0057B65Au);
    gfx_fill_rect(x + 11, y + 2, 2, 8, 0x0057B65Au);
}

/* a gear */
static void draw_icon_settings(int x, int y, uint32_t bg) {
    const uint32_t c = 0x00C8D2E0u;
    gfx_fill_rect(x + 5, y, 4, 14, c);
    gfx_fill_rect(x, y + 5, 14, 4, c);
    gfx_fill_rect(x + 2, y + 2, 10, 10, c);
    gfx_fill_rect(x + 1, y + 1, 3, 3, c);
    gfx_fill_rect(x + 10, y + 1, 3, 3, c);
    gfx_fill_rect(x + 1, y + 10, 3, 3, c);
    gfx_fill_rect(x + 10, y + 10, 3, 3, c);
    gfx_fill_rect(x + 5, y + 5, 4, 4, bg);
}

static void draw_icon_power(int x, int y, uint32_t bg) {
    (void)bg;
    draw_bevel_box(x, y, 12, 12, 0x00412A2Au, 0x00764A4Au, 0x00170D0Du);
    gfx_draw_text(x + 3, y + 2, "o", 0x00FFFFFFu, 0x00412A2Au);
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
    static char out[100 * 76];
    uint32_t n = 0;
    for (int r = r0; r <= r1 && r < th; r++) {
        int a = r == r0 ? c0 : 0, b = r == r1 ? c1 : tw - 1;
        uint32_t line_start = n;
        for (int c = a; c <= b && c < tw; c++) {
            char ch = chars[r * stride + c];
            out[n++] = ch ? ch : ' ';
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

        const char* item = MENU_LABELS[i];
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
    for (size_t y = 0; y < MENU_ITEMS + 1; y++) {
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

static int cursor_shape(int mx, int my);
static int g_drawn_shape;

static void draw_cursor(int mx, int my) {
    g_drawn_shape = cursor_shape(mx, my);
    draw_cursor_shape(mx, my, g_drawn_shape);
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
    return (int)fi->width - 8 - 64 - 16 - (int)strlen(net) * 8;
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
    gfx_draw_text(clk_x - 18 - (int)strlen(net) * 8, bar_y + 10, net, col, 0x00192026u);
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
} gui_view_t;

static void capture_view(gui_view_t* v, uint32_t sec) {
    memset(v, 0, sizeof(*v));
    v->menu_open = g_menu_open;
    v->menu_sel = g_menu_sel;
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

static void (*const g_menu_icons[MENU_ITEMS])(int, int, uint32_t) = {
    draw_icon_terminal, draw_icon_files, draw_icon_browser, draw_icon_notepad,
    draw_icon_apps, draw_icon_taskmgr, draw_icon_settings, draw_icon_power,
};

static int start_menu_y(const fb_info_t* fi) { return (int)fi->height - BAR_H - START_MENU_H - 4; }

static void render_desktop(const fb_info_t* fi, int mx, int my) {
    refresh_wallpaper_cache(fi);
    blit_wallpaper_cache();

    /* desktop shortcuts */
    for (int i = 0; i < MENU_ITEMS; i++) {
        int iy = ICON_Y + ICON_STEP * i;
        draw_bevel_box(ICON_X, iy, ICON_W, ICON_H, 0x0029313Du, 0x00586678u, 0x0010151Eu);
        g_menu_icons[i](ICON_X + 8, iy + 12, 0x0029313Du);
        gfx_draw_text(ICON_X + 30, iy + 13, ICON_LABELS[i], 0x00F0F6FFu, 0x0029313Du);
    }

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
            uint32_t bg = (g_menu_sel == i) ? 0x003A4A66u : 0x001D232Cu;
            gfx_fill_rect(menu_x + 24, menu_y + 8 + i * 28, START_MENU_W - 32, 24, bg);
            g_menu_icons[i](menu_x + 28, menu_y + 14 + i * 28, bg);
            gfx_draw_text(menu_x + 48, menu_y + 16 + i * 28, MENU_LABELS[i], 0x00E8EEF6u, bg);
        }
    }

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
        { "Exit to shell", ACT_QUIT, 0 },
    };
    ctxmenu_open(mx, my, items, (int)(sizeof(items) / sizeof(items[0])), desktop_menu_cb, NULL);
}

static void icon_menu_cb(int id, void* arg) { (void)arg; do_action(id); }

static void open_icon_menu(int slot, int mx, int my) {
    char open[40];
    ksnprintf(open, sizeof(open), "Open %s", ICON_LABELS[slot]);
    ctx_item_t items[] = { { open, slot, 0 } };
    ctxmenu_open(mx, my, items, 1, icon_menu_cb, NULL);
}

/* taskbar: Task Manager & co, or one window's button */
enum { TBM_TASKMGR = 1, TBM_SHOW_DESKTOP, TBM_RESTORE_ALL, TBM_RESTORE, TBM_MINIMIZE, TBM_CLOSE, TBM_QUIT, TBM_TERMINAL };
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
        { "Exit to shell", TBM_QUIT, 0 },
    };
    ctxmenu_open(mx, my, items, 7, taskbar_menu_cb, NULL);
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

static int icon_at(int mx, int my) {
    for (int i = 0; i < MENU_ITEMS; i++)
        if (mx >= ICON_X && mx < ICON_X + ICON_W && my >= ICON_Y + ICON_STEP * i && my < ICON_Y + ICON_STEP * i + ICON_H)
            return i;
    return -1;
}

int gui_appwin_focused(void) {
    return gfx_available() && g_gui_enabled && g_front_app == APP_APPWIN && appwin_any_visible() && tty_current() < 0;
}

void gui_poll(void) {
    /* every idle/wait loop passes through here: paint pending console output */
    terminal_flush();
    timer_poll();
    task_yield();

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

        static int mx = 100, my = 100;
        static int drawn_mx = -1, drawn_my = -1;
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
            ((g_front_app == APP_LAUNCHER || g_front_app == APP_TASKMGR || g_front_app == APP_FILES) &&
             app_visible(g_front_app) && tty_current() < 0)) {
            for (int k = 0; k < 64; k++) {
                char c = keyboard_try_getchar();
                if (!c) break;
                if (g_menu_open || c == 20) { gui_handle_key(c); continue; }
                if (g_front_app == APP_NOTEPAD) notepad_key(c);
                else if (g_front_app == APP_APPWIN) appwin_key(c);
                else if (g_front_app == APP_FILES) explorer_key(c);
                else if (c == 27) g_apps[g_front_app].close();
            }
        }

        if (!g_backbuf_active) {
            size_desktop(fi);
            fb_set_backbuffer(g_desktop_backbuf, g_desk_w, g_desk_h);
            g_backbuf_active = 1;
            g_force_redraw = 1;
            /* the saved wallpaper (/etc/wallpaper) is applied on first use */
            wallpaper_load_config();
        }

        mouse_state_t ms = mouse_read();
        mx += ms.dx;
        my -= ms.dy;
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
            ctxmenu_click(mx, my);        /* an item, or a click outside that just closes it */
            click = 0;
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
                int menu_y = start_menu_y(fi);
                int item_x0 = START_X + 24;
                int item_x1 = START_X + START_MENU_W - 8;
                if (mx >= item_x0 && mx < item_x1) {
                    for (int i = 0; i < MENU_ITEMS; i++) {
                        int y0 = menu_y + 8 + i * 28;
                        if (my >= y0 && my < y0 + 24) {
                            g_menu_sel = i;
                            menu_activate();
                            click = 0;
                            break;
                        }
                    }
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

            /* desktop shortcuts */
            if (click && !g_menu_open) {
                int slot = icon_at(mx, my);
                if (slot >= 0) do_action(slot);
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

        gui_view_t view;
        capture_view(&view, sec);
        int changed = g_force_redraw || memcmp(&view, &g_last_view, sizeof(view)) != 0;

        if (!changed) {
            /* only the mouse moved (or its shape changed): restore its old spot, draw it anew */
            if (mx != drawn_mx || my != drawn_my || cursor_shape(mx, my) != g_drawn_shape) {
                fb_present_rect(drawn_mx + CURSOR_X0, drawn_my + CURSOR_Y0, CURSOR_W, CURSOR_H);
                draw_cursor(mx, my);
                drawn_mx = mx;
                drawn_my = my;
            }
            return;
        }

        /* at most ~60 frames per second; the change is kept for next time */
        uint32_t now = timer_ms();
        if (!g_force_redraw && (uint32_t)(now - last_frame_ms) < 16) return;
        last_frame_ms = now;

        render_desktop(fi, mx, my);
        drawn_mx = mx;
        drawn_my = my;
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
    g_gui_enabled = enabled ? 1 : 0;
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
    /* an SSH session's shell always has the focus of its own terminal */
    int tt = tty_current();
    if (tt >= 0) return tty_vt(tt);

    if (!gfx_available() || !g_gui_enabled) return 0; /* plain console owns input */

    /* the browser in front reads the keyboard itself (its own task) */
    if (g_front_app == APP_BROWSER && browser_is_open() && !g_app_min[APP_BROWSER]) return BROWSER_VT;
    if (g_front_app == APP_NOTEPAD && notepad_is_open() && !g_app_min[APP_NOTEPAD]) return NOTEPAD_VT;
    if (g_front_app == APP_APPWIN && appwin_any_visible()) return APPWIN_VT;
    if ((g_front_app == APP_LAUNCHER || g_front_app == APP_TASKMGR || g_front_app == APP_FILES) && app_visible(g_front_app))
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
    }
}

static void menu_activate(void) {
    if (gfx_available()) { do_action(g_menu_sel); return; }
    /* text mode: the desktop's apps need the framebuffer */
    menu_close_redraw();
    if (g_menu_sel == ACT_QUIT) gui_set_enabled(0);
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
