#include "gui.h"
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
#include "../net/net.h"
#include "../shell/shell.h"

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

#define TASKBAR_ROW (VGA_HEIGHT - 1)

static int g_menu_open = 0;
static int g_menu_sel = 0; /* 0=About, 1=Terminal, 2=Files, 3=Wallpaper, 4=Quit GUI */
#define MENU_ITEMS 5
static int g_files_front = 0; /* the Files window is above the terminal windows */
static uint32_t g_last_clock_sec = (uint32_t)-1;
static int g_gui_enabled = 0; /* like startx: default off */
static int g_about_open = 0;
static int g_wallpaper_open = 0;

/* Wallpaper app: user pictures found in ~/Pictures (file indexes), a
 * picture waiting to be decoded on the next frame (so "Loading..." gets
 * painted first), and a one-line status/error message. */
#define WP_PICS_MAX 6
static int  g_wp_pics[WP_PICS_MAX];
static int  g_wp_pic_count = 0;       /* shown (<= WP_PICS_MAX) */
static int  g_wp_pic_total = 0;       /* found */
static int  g_wp_pending = -1;        /* file index to load next frame */
static char g_wp_status[96];

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
} term_win_t;

#define TERM_WIN_MAX 4
static term_win_t g_terms[TERM_WIN_MAX] = {
    {0, 0, 140,  90, 520, 340, 0, 0, 0},
    {0, 0, 180, 120, 520, 340, 0, 0, 0},
    {0, 0, 220, 150, 520, 340, 0, 0, 0},
    {0, 0, 260, 180, 520, 340, 0, 0, 0},
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

static void draw_flux_toolbar_button(int x, int y, int w, int h, const char* label, int pressed) {
    uint32_t base = pressed ? 0x00252B33u : 0x00303740u;
    uint32_t hi = pressed ? 0x002E3640u : 0x00535D6Eu;
    uint32_t lo = pressed ? 0x00141920u : 0x0015191Fu;
    draw_bevel_box(x, y, w, h, base, hi, lo);
    gfx_draw_text(x + 8, y + 8, label, 0x00E6EDF5u, base);
}

static void draw_icon_terminal(int x, int y, uint32_t bg) {
    (void)bg;
    draw_bevel_box(x, y, 14, 12, 0x00161D28u, 0x00475A78u, 0x000E1118u);
    gfx_draw_text(x + 2, y + 2, ">", 0x00E8EEF6u, 0x00161D28u);
}

static void draw_icon_info(int x, int y, uint32_t bg) {
    (void)bg;
    draw_bevel_box(x, y, 12, 12, 0x00323C52u, 0x0060708Bu, 0x00101520u);
    gfx_draw_text(x + 4, y + 2, "i", 0x00FFFFFFu, 0x00323C52u);
}

static void draw_icon_wallpaper(int x, int y, uint32_t bg) {
    (void)bg;
    draw_bevel_box(x, y, 14, 12, 0x00342F25u, 0x0061573Fu, 0x0018110Au);
    gfx_fill_rect(x + 2, y + 7, 10, 3, 0x00384F70u);
}

static void draw_icon_files(int x, int y, uint32_t bg) {
    (void)bg;
    gfx_fill_rect(x, y + 1, 6, 2, 0x00F4D35Eu);
    gfx_fill_rect(x, y + 3, 14, 9, 0x00F4D35Eu);
    gfx_fill_rect(x + 1, y + 4, 12, 1, 0x00FFF1A8u);
}

static void draw_icon_power(int x, int y, uint32_t bg) {
    (void)bg;
    draw_bevel_box(x, y, 12, 12, 0x00412A2Au, 0x00764A4Au, 0x00170D0Du);
    gfx_draw_text(x + 3, y + 2, "o", 0x00FFFFFFu, 0x00412A2Au);
}

/* a tiny "photo" glyph: sky, sun, hill */
static void draw_icon_picture(int x, int y) {
    draw_bevel_box(x, y, 16, 16, 0x003A6EA5u, 0x006F9BCCu, 0x00182C44u);
    gfx_fill_rect(x + 10, y + 3, 3, 3, 0x00F4D35Eu);
    gfx_fill_rect(x + 2, y + 10, 12, 4, 0x003C8D4Fu);
    gfx_fill_rect(x + 5, y + 8, 5, 2, 0x003C8D4Fu);
}

static void clamp_win(const fb_info_t* fi, term_win_t* w) {
    if (!fi) return;
    if (!w) return;
    if (w->w < 220) w->w = 220;
    if (w->h < 160) w->h = 160;
    if (w->x < 0) w->x = 0;
    if (w->y < 0) w->y = 0;
    if (w->x + w->w > (int)fi->width)  w->x = (int)fi->width - w->w;
    if (w->y + w->h > (int)fi->height) w->y = (int)fi->height - w->h;
    if (w->x < 0) w->x = 0;
    if (w->y < 0) w->y = 0;
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
        g_terms[i].dragging = 0;
        bring_term_front(i);
        return i;
    }
    /* none free: focus the frontmost */
    bring_term_front(g_term_order[TERM_WIN_MAX - 1]);
    return g_term_order[TERM_WIN_MAX - 1];
}

static void draw_terminal_window(const fb_info_t* fi, const term_win_t* win) {
    if (!win || !win->open) return;
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

    /* close button */
    int bx = w.x + w.w - 28;
    draw_bevel_box(bx, w.y + 4, 20, 12, 0x006D2F2Fu, 0x00A14747u, 0x00301717u);
    gfx_draw_text(bx + 6, w.y + 6, "x", 0x00FFFFFFu, 0x006D2F2Fu);

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

    int row_off = (int)cr - max_rows + 1;
    if (row_off < 0) row_off = 0;
    if (row_off + max_rows > th) row_off = th - max_rows;
    if (row_off < 0) row_off = 0;

    for (int y = 0; y < max_rows; y++) {
        for (int x = 0; x < max_cols; x++) {
            int idx = (y + row_off) * stride + x;
            uint8_t color = cols[idx];
            char c = chars[idx];
            /* the client area is already black: skip blank black cells */
            if ((c == ' ' || c == 0) && (color & 0xF0) == 0) continue;
            gfx_draw_char(cx + x * 8, cy + y * 8, c,
                          vga_color_rgb(color & 0x0F), vga_color_rgb((color >> 4) & 0x0F));
        }
    }

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

static void k_memset(char* p, char v, int n) {
    for (int i = 0; i < n; i++) p[i] = v;
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

        static const char* const items[MENU_ITEMS] = { " About app", " Terminal", " Files", " Wallpaper", " Quit GUI" };
        const char* item = items[i];
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

/* ── Wallpaper app ─────────────────────────────────────────────────── */

#define WP_W 560
#define WP_H 430

static void wp_scan_pictures(void) {
    int idx[64];
    int n = fs_list_files(WALLPAPER_DIR, idx, 64);
    g_wp_pic_count = 0;
    g_wp_pic_total = 0;
    for (int i = 0; i < n && i < 64; i++) {
        fs_file_t* f = fs_get_file(idx[i]);
        if (!wallpaper_is_image_name(f->name)) continue;
        if (g_wp_pic_count < WP_PICS_MAX) g_wp_pics[g_wp_pic_count++] = idx[i];
        g_wp_pic_total++;
    }
}

static void open_wallpaper_app(void) {
    g_menu_open = 0;
    g_about_open = 0;
    g_wallpaper_open = 1;
    g_wp_pending = -1;
    g_wp_status[0] = '\0';
    wp_scan_pictures();
}

/* geometry shared by drawing and hit testing: kind 0 = preset, 1 = picture */
static void wp_item_rect(const fb_info_t* fi, int kind, int i, int* x, int* y, int* w, int* h) {
    int wx0 = ((int)fi->width - WP_W) / 2;
    int wy0 = ((int)fi->height - WP_H) / 2;
    int col_w = (WP_W - 36) / 2;
    int top = (kind == 0) ? wy0 + 50 : wy0 + 248;
    *x = wx0 + 12 + (i & 1) * col_w;
    *y = top + (i >> 1) * 32;
    *w = col_w - 12;
    *h = 28;
}

static void draw_wallpaper_app(const fb_info_t* fi) {
    int wx0 = ((int)fi->width - WP_W) / 2;
    int wy0 = ((int)fi->height - WP_H) / 2;
    const uint32_t panel = 0x001D232Cu;
    draw_bevel_box(wx0, wy0, WP_W, WP_H, panel, 0x00505D72u, 0x0010141Cu);
    draw_bevel_box(wx0 + 3, wy0 + 3, WP_W - 6, 19, 0x00384562u, 0x00647692u, 0x00111923u);
    gfx_draw_text(wx0 + 10, wy0 + 7, "Wallpaper", 0x00FFFFFFu, 0x00384562u);

    gfx_draw_text(wx0 + 14, wy0 + 32, "Built-in:", 0x00E8EEF6u, panel);
    int cur = wallpaper_current_preset();
    for (int i = 0; i < wallpaper_preset_count(); i++) {
        int x, y, w, h;
        wp_item_rect(fi, 0, i, &x, &y, &w, &h);
        uint32_t bg = (i == cur) ? 0x003A4A66u : panel;
        draw_bevel_box(x, y, w, h, bg, 0x0056667Fu, 0x0010151Fu);
        gfx_fill_rect(x + 6, y + 6, 16, 16, wallpaper_preset(i)->base);
        gfx_draw_text(x + 28, y + 10, wallpaper_preset(i)->name, 0x00E8EEF6u, bg);
    }

    gfx_draw_text(wx0 + 14, wy0 + 230, "Your pictures (~/Pictures):", 0x00E8EEF6u, panel);
    if (g_wp_pic_count == 0) {
        gfx_draw_text(wx0 + 24, wy0 + 256, "No pictures yet. In a terminal, download one:", 0x00AAB6C6u, panel);
        gfx_draw_text(wx0 + 24, wy0 + 272, "wget -O ~/Pictures/wall.jpg https://...", 0x00F4D35Eu, panel);
        gfx_draw_text(wx0 + 24, wy0 + 288, "then pick it here, or run: wallpaper ~/Pictures/wall.jpg",
                      0x00AAB6C6u, panel);
    }
    const char* current_file = wallpaper_current_file();
    for (int i = 0; i < g_wp_pic_count; i++) {
        int x, y, w, h;
        wp_item_rect(fi, 1, i, &x, &y, &w, &h);
        char path[FS_PATH_LEN];
        fs_file_path(g_wp_pics[i], path, sizeof(path));
        uint32_t bg = (current_file[0] && strcmp(path, current_file) == 0) ? 0x003A4A66u : panel;
        draw_bevel_box(x, y, w, h, bg, 0x0056667Fu, 0x0010151Fu);
        draw_icon_picture(x + 6, y + 6);
        char name[29];
        kstrlcpy(name, fs_get_file(g_wp_pics[i])->name, sizeof(name));
        gfx_draw_text(x + 28, y + 10, name, 0x00E8EEF6u, bg);
    }
    if (g_wp_pic_total > g_wp_pic_count) {
        char more[64];
        ksnprintf(more, sizeof(more), "+%d more - use the `wallpaper` command", g_wp_pic_total - g_wp_pic_count);
        gfx_draw_text(wx0 + 24, wy0 + 348, more, 0x00AAB6C6u, panel);
    }

    /* status: what's happening, or what's in use */
    char line[96];
    if (g_wp_status[0]) kstrlcpy(line, g_wp_status, sizeof(line));
    else if (current_file[0]) ksnprintf(line, sizeof(line), "Current: %s (%s)", current_file,
                                        image_mode_name(wallpaper_current_mode()));
    else ksnprintf(line, sizeof(line), "Current: %s (built-in)", wallpaper_preset(cur)->name);
    line[(WP_W - 28) / 8] = '\0';
    gfx_draw_text(wx0 + 14, wy0 + WP_H - 42, line, 0x00A9B7C8u, panel);
    gfx_draw_text(wx0 + 14, wy0 + WP_H - 22, "Click an item to apply, click outside to close",
                  0x00AAAAAAu, panel);
}

/* returns 1 if the click landed inside the app window */
static int wallpaper_app_click(const fb_info_t* fi, int mx, int my) {
    int wx0 = ((int)fi->width - WP_W) / 2;
    int wy0 = ((int)fi->height - WP_H) / 2;
    if (!(mx >= wx0 && mx < wx0 + WP_W && my >= wy0 && my < wy0 + WP_H)) {
        g_wallpaper_open = 0;
        return 1;   /* consumed: clicking outside just closes the app */
    }
    for (int i = 0; i < wallpaper_preset_count(); i++) {
        int x, y, w, h;
        wp_item_rect(fi, 0, i, &x, &y, &w, &h);
        if (mx >= x && mx < x + w && my >= y && my < y + h) {
            wallpaper_set_preset(i);
            g_wp_status[0] = '\0';
            return 1;
        }
    }
    for (int i = 0; i < g_wp_pic_count; i++) {
        int x, y, w, h;
        wp_item_rect(fi, 1, i, &x, &y, &w, &h);
        if (mx >= x && mx < x + w && my >= y && my < y + h) {
            /* decode on the next frame, after "Loading..." is on screen */
            g_wp_pending = g_wp_pics[i];
            ksnprintf(g_wp_status, sizeof(g_wp_status), "Loading %s...", fs_get_file(g_wp_pics[i])->name);
            return 1;
        }
    }
    return 1;
}

static void wallpaper_app_do_pending(void) {
    if (g_wp_pending < 0) return;
    char path[FS_PATH_LEN], err[96];
    fs_file_path(g_wp_pending, path, sizeof(path));
    g_wp_pending = -1;
    if (wallpaper_set_file(path, IMAGE_FILL, err, sizeof(err)) == 0) g_wp_status[0] = '\0';
    else ksnprintf(g_wp_status, sizeof(g_wp_status), "Error: %s", err);
}

/* ── About ─────────────────────────────────────────────────────────── */

static void show_about(void) {
    size_t saved_r, saved_c;
    terminal_get_cursor(&saved_r, &saved_c);

    terminal_clear();

    const sysinfo_t* si = sysinfo_get();
    terminal_write_color("Banana OS 0.5 - About app\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    terminal_writeln("----------------------------------------");
    terminal_write_color("Version: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    terminal_writeln("0.5");
    terminal_write_color("Display: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    terminal_writeln("VGA text 80x25 + basic GUI taskbar");
    terminal_write_color("CPU: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    terminal_writeln(si->cpu_brand[0] ? si->cpu_brand : "Unknown");
    terminal_write_color("Network: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    {
        char net[40];
        format_net_status(net, sizeof(net));
        terminal_writeln(net);
    }

    terminal_write_color("Uptime: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    {
        uint32_t sec = timer_ticks() / 100u;
        uint32_t h = sec / 3600u;
        uint32_t m = (sec % 3600u) / 60u;
        uint32_t s = sec % 60u;
        char b[32];
        k_memset(b, 0, (int)sizeof(b));
        /* simple formatting without stdlib */
        b[0] = (char)('0' + ((h / 10u) % 10u));
        b[1] = (char)('0' + (h % 10u));
        b[2] = 'h';
        b[3] = ' ';
        b[4] = (char)('0' + ((m / 10u) % 10u));
        b[5] = (char)('0' + (m % 10u));
        b[6] = 'm';
        b[7] = ' ';
        b[8] = (char)('0' + ((s / 10u) % 10u));
        b[9] = (char)('0' + (s % 10u));
        b[10] = 's';
        b[11] = '\0';
        terminal_writeln(b);
    }

    terminal_writeln("");
    terminal_write_color("Press any key to return...\n", VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);

    /* wait for a key, keep clock updating */
    while (1) {
        gui_poll();
        char c = keyboard_try_getchar();
        if (c) break;
        timer_sleep_ms(10);
    }

    terminal_clear();
    terminal_set_cursor(saved_r, saved_c);
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
static uint32_t g_desktop_backbuf[800u * 600u];
static int      g_backbuf_active = 0;

/* Cache of the rendered wallpaper: rendering it (bilinear upscale of a
 * preset, or copying a decoded user picture) only happens when the
 * wallpaper actually changes, never per frame. */
static uint32_t g_wallpaper_cache[800u * 600u];
static uint32_t g_wallpaper_cache_gen = 0;

static void refresh_wallpaper_cache(const fb_info_t* fi) {
    if (g_wallpaper_cache_gen == wallpaper_generation()) return;
    int w = (int)fi->width < 800 ? (int)fi->width : 800;
    int h = (int)fi->height < 600 ? (int)fi->height : 600;
    wallpaper_render(g_wallpaper_cache, w, h, 800);
    g_wallpaper_cache_gen = wallpaper_generation();
}

static void blit_wallpaper_cache(void) {
    memcpy(g_desktop_backbuf, g_wallpaper_cache, sizeof(g_desktop_backbuf));
}

/* ── mouse cursor ──────────────────────────────────────────────────
 * Drawn straight onto the framebuffer over the presented frame, so when
 * only the mouse moves, restoring the old spot from the backbuffer and
 * drawing the arrow elsewhere is all it takes - no full repaint. */
#define CURSOR_W 6
#define CURSOR_H 10

static void draw_cursor(int mx, int my) {
    for (int cy = 0; cy < CURSOR_H; cy++)
        for (int cx = 0; cx < CURSOR_W; cx++)
            if (cx == 0 || cy == 0 || cx == cy / 2)
                fb_putpixel_direct(mx + cx, my + cy, 0x00FFFFFFu);
}

/* Everything that affects what the desktop looks like (besides
 * wallpaper/terminal content, which have their own change counters).
 * A frame is only rendered when this, those counters, or the clock's
 * second changed - the desktop used to be fully repainted ~33 times a
 * second even when nothing at all was happening. */
typedef struct {
    int menu_open, menu_sel, about_open, wallpaper_open;
    int term_open[TERM_WIN_MAX], term_x[TERM_WIN_MAX], term_y[TERM_WIN_MAX], term_order[TERM_WIN_MAX];
    int pic_count, pending, files_front;
    uint32_t sec, term_gen, wp_gen, net_state, files_sig;
    char status[96];
} gui_view_t;

static void capture_view(gui_view_t* v, uint32_t sec) {
    memset(v, 0, sizeof(*v));
    v->menu_open = g_menu_open;
    v->menu_sel = g_menu_sel;
    v->about_open = g_about_open;
    v->wallpaper_open = g_wallpaper_open;
    for (int i = 0; i < TERM_WIN_MAX; i++) {
        v->term_open[i] = g_terms[i].open;
        v->term_x[i] = g_terms[i].x;
        v->term_y[i] = g_terms[i].y;
        v->term_order[i] = g_term_order[i];
    }
    v->pic_count = g_wp_pic_count;
    v->pending = g_wp_pending;
    v->sec = sec;
    v->term_gen = terminal_generation();
    v->wp_gen = wallpaper_generation();
    v->net_state = net_if()->configured ? net_if()->ip : 1;
    v->files_front = g_files_front;
    v->files_sig = explorer_signature();
    kstrlcpy(v->status, g_wp_status, sizeof(v->status));
}

static gui_view_t g_last_view;
static int        g_force_redraw = 1;

static void render_desktop(const fb_info_t* fi, int mx, int my) {
    int bar_h = 28;
    int bar_y = (int)fi->height - bar_h;
    int start_x = 8;
    int clk_w = 8 * 8;
    int clk_x = (int)fi->width - clk_w - 8; /* clock stays at far right */
    int quit_x = clk_x - 76 - 16;           /* quit before clock */
    if (quit_x < (start_x + 88 + 12)) quit_x = start_x + 88 + 12;

    refresh_wallpaper_cache(fi);
    blit_wallpaper_cache();

    /* desktop shortcuts */
    {
        int icon_w = 132, icon_h = 38;
        int sx = 18, sy = 22;

        draw_bevel_box(sx, sy, icon_w, icon_h, 0x0029313Du, 0x00586678u, 0x0010151Eu);
        draw_icon_info(sx + 8, sy + 12, 0x0029313Du);
        gfx_draw_text(sx + 30, sy + 13, "About", 0x00F0F6FFu, 0x0029313Du);

        draw_bevel_box(sx, sy + icon_h + 10, icon_w, icon_h, 0x0029313Du, 0x00586678u, 0x0010151Eu);
        draw_icon_terminal(sx + 8, sy + icon_h + 22, 0x0029313Du);
        gfx_draw_text(sx + 30, sy + icon_h + 10 + 13, "Terminal", 0x00F0F6FFu, 0x0029313Du);
        draw_bevel_box(sx, sy + (icon_h + 10) * 2, icon_w, icon_h, 0x0029313Du, 0x00586678u, 0x0010151Eu);
        draw_icon_files(sx + 8, sy + (icon_h + 10) * 2 + 12, 0x0029313Du);
        gfx_draw_text(sx + 30, sy + (icon_h + 10) * 2 + 13, "Files", 0x00F0F6FFu, 0x0029313Du);
        draw_bevel_box(sx, sy + (icon_h + 10) * 3, icon_w, icon_h, 0x0029313Du, 0x00586678u, 0x0010151Eu);
        draw_icon_wallpaper(sx + 8, sy + (icon_h + 10) * 3 + 12, 0x0029313Du);
        gfx_draw_text(sx + 30, sy + (icon_h + 10) * 3 + 13, "Wallpaper", 0x00F0F6FFu, 0x0029313Du);
        draw_bevel_box(sx, sy + (icon_h + 10) * 4, icon_w, icon_h, 0x0029313Du, 0x00586678u, 0x0010151Eu);
        draw_icon_power(sx + 8, sy + (icon_h + 10) * 4 + 12, 0x0029313Du);
        gfx_draw_text(sx + 30, sy + (icon_h + 10) * 4 + 13, "Quit GUI", 0x00F0F6FFu, 0x0029313Du);
    }

    char clk[9];
    format_clock(clk);

    /* toolbar */
    draw_bevel_box(0, bar_y, (int)fi->width, bar_h, 0x00192026u, 0x004F5A6Eu, 0x0010141Bu);
    draw_flux_toolbar_button(start_x - 4, bar_y + 5, 88, 18, "banana", g_menu_open);
    draw_flux_toolbar_button(quit_x - 4, bar_y + 5, 76, 18, "exit", 0);
    gfx_draw_text(clk_x, bar_y + 10, clk, 0x00E8EEF6u, 0x00192026u);
    {
        char net[40];
        format_net_status(net, sizeof(net));
        int nx = quit_x - 16 - (int)strlen(net) * 8;
        uint32_t col = net_if()->configured ? 0x008FE3A1u : 0x00C9A45Cu;
        gfx_draw_text(nx, bar_y + 10, net, col, 0x00192026u);
    }

    /* root menu */
    if (g_menu_open) {
        int menu_w = 236;
        int menu_h = 168;
        int menu_x = 8;
        int menu_y = (int)fi->height - bar_h - menu_h - 8;
        draw_bevel_box(menu_x, menu_y, menu_w, menu_h, 0x001D232Cu, 0x00505E74u, 0x0010151Du);
        gfx_fill_rect(menu_x + 2, menu_y + 2, 18, menu_h - 4, 0x00354463u);
        gfx_draw_text(menu_x + 5, menu_y + 8, "B", 0x00F4F8FFu, 0x00354463u);

        static void (*const icons[MENU_ITEMS])(int, int, uint32_t) = {
            draw_icon_info, draw_icon_terminal, draw_icon_files, draw_icon_wallpaper, draw_icon_power,
        };
        static const char* const labels[MENU_ITEMS] = { "About", "Terminal", "Files", "Wallpaper", "Exit to shell" };
        for (int i = 0; i < MENU_ITEMS; i++) {
            uint32_t bg = (g_menu_sel == i) ? 0x003A4A66u : 0x001D232Cu;
            gfx_fill_rect(menu_x + 24, menu_y + 8 + i * 28, menu_w - 32, 24, bg);
            icons[i](menu_x + 28, menu_y + 14 + i * 28, bg);
            gfx_draw_text(menu_x + 48, menu_y + 16 + i * 28, labels[i], 0x00E8EEF6u, bg);
        }
    }

    if (g_about_open) {
        int mw = 420, mh = 160;
        int mx0 = ((int)fi->width - mw) / 2;
        int my0 = ((int)fi->height - mh) / 2;
        char net[48], line[64];
        format_net_status(net, sizeof(net));
        ksnprintf(line, sizeof(line), "Network: %s", net);
        draw_bevel_box(mx0, my0, mw, mh, 0x001D232Cu, 0x00505D72u, 0x0010141Cu);
        draw_bevel_box(mx0 + 3, my0 + 3, mw - 6, 19, 0x00384562u, 0x00647692u, 0x00111923u);
        gfx_draw_text(mx0 + 10, my0 + 7, "About Banana OS 0.5", 0x00FFFFFFu, 0x00384562u);
        gfx_draw_text(mx0 + 16, my0 + 40, "Banana OS 0.5", 0x00FFFFFFu, 0x001D232Cu);
        gfx_draw_text(mx0 + 16, my0 + 56, "Theme: Fluxbox-inspired toolbar/menu", 0x00FFFFFFu, 0x001D232Cu);
        gfx_draw_text(mx0 + 16, my0 + 72, line, 0x00FFFFFFu, 0x001D232Cu);
        gfx_draw_text(mx0 + 16, my0 + 96, "Click anywhere to close", 0x00AAAAAAu, 0x001D232Cu);
    }

    if (g_wallpaper_open) draw_wallpaper_app(fi);

    /* windows back-to-front: Files below or above the terminals */
    if (!g_files_front) explorer_draw(fi);
    for (int oi = 0; oi < TERM_WIN_MAX; oi++) {
        term_win_t* w = &g_terms[g_term_order[oi]];
        draw_terminal_window(fi, w);
    }
    if (g_files_front) explorer_draw(fi);

    /* push backbuffer to framebuffer once per frame, then the cursor */
    fb_present();
    draw_cursor(mx, my);
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

        if (!g_backbuf_active) {
            fb_set_backbuffer(g_desktop_backbuf, 800u, 600u);
            g_backbuf_active = 1;
            g_force_redraw = 1;
            /* the saved wallpaper (/etc/wallpaper) is applied on first use */
            wallpaper_load_config();
        }

        mouse_state_t ms = mouse_read();
        mx += ms.dx;
        my -= ms.dy;
        if (mx < 0) mx = 0;
        if (my < 0) my = 0;
        if (mx > (int)fi->width - 1) mx = (int)fi->width - 1;
        if (my > (int)fi->height - 1) my = (int)fi->height - 1;

        uint32_t sec = timer_ticks() / 100u;

        /* mouse click handling (rising edge) */
        int left = ms.btn_left ? 1 : 0;
        int click = (left && !prev_left);
        prev_left = left;

        int bar_h = 28;
        int bar_y = (int)fi->height - bar_h;

        /* Start button hitbox around "[Start]" text */
        int start_x = 8;
        int start_w = 88;
        int quit_w = 76;
        int clk_w_for_hit = 8 * 8;
        int clk_x_for_hit = (int)fi->width - clk_w_for_hit - 8;
        int quit_x = clk_x_for_hit - quit_w - 16; /* quit before clock */
        if (quit_x < (start_x + start_w + 12)) quit_x = start_x + start_w + 12;

        /* the picture picked earlier is decoded once a frame showing its
         * "Loading..." status has actually been painted */
        if (g_wp_pending >= 0 && g_last_view.pending == g_wp_pending) wallpaper_app_do_pending();

        if (click) {
            if (g_about_open) {
                g_about_open = 0;
            }
            if (g_wallpaper_open) {
                if (wallpaper_app_click(fi, mx, my)) click = 0;
            }

            /* click on taskbar Start */
            if (click && my >= bar_y + 5 && my < bar_y + 23 && mx >= start_x - 4 && mx < (start_x - 4 + start_w)) {
                g_menu_open = !g_menu_open;
                if (g_menu_open) g_menu_sel = 0;
            }

            /* click on Quit GUI button */
            if (click && my >= bar_y + 5 && my < bar_y + 23 && mx >= quit_x - 4 && mx < (quit_x - 4 + quit_w)) {
                gui_set_enabled(0);
                return;
            }

            /* click in menu items (the open menu is above every window) */
            if (click && g_menu_open) {
                int menu_w = 236;
                int menu_h = 168;
                int menu_x = 8;
                int menu_y = (int)fi->height - bar_h - menu_h - 8;

                int item_x0 = menu_x + 24;
                int item_x1 = menu_x + menu_w - 8;

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
            }

            /* the Files window, when it is in front of the terminals */
            if (click && g_files_front && explorer_contains(mx, my)) {
                explorer_click(mx, my);
                click = 0;
            }

            /* terminal windows hit testing (front-to-back) */
            for (int oi = TERM_WIN_MAX - 1; click && oi >= 0; oi--) {
                int wi = g_term_order[oi];
                term_win_t* w = &g_terms[wi];
                if (!w->open) continue;

                int title_h = 20;
                if (mx >= w->x && mx < w->x + w->w && my >= w->y && my < w->y + w->h) {
                    /* Bringing a window to front changes keyboard focus
                     * (gui_focused_vt() below), but must NOT retarget
                     * where terminal output is written - that's owned by
                     * this window's own shell task now, not by whichever
                     * window was last clicked. (That retargeting used to
                     * be exactly this line, and was the root cause of
                     * every window showing the same running command.) */
                    bring_term_front(wi);
                    g_files_front = 0;

                    int close_x = w->x + w->w - 28;
                    if (mx >= close_x && mx < close_x + 24 && my >= w->y + 2 && my < w->y + 18) {
                        /* Hide only - the vt and its shell task are kept
                         * running so a later reopen picks up right where
                         * this session left off. */
                        w->open = 0;
                    } else if (my < w->y + title_h) {
                        w->dragging = 1;
                        w->drag_dx = mx - w->x;
                        w->drag_dy = my - w->y;
                    }
                    click = 0;
                    break;
                }
            }

            /* the Files window behind the terminals: clicking raises it */
            if (click && explorer_contains(mx, my)) {
                g_files_front = 1;
                explorer_click(mx, my);
                click = 0;
            }

            /* desktop shortcuts */
            if (click && !g_about_open && !g_menu_open && !g_wallpaper_open) {
                int icon_w = 132, icon_h = 38;
                int sx = 18, sy = 22;
                int slot = -1;
                for (int i = 0; i < MENU_ITEMS; i++)
                    if (mx >= sx && mx < sx + icon_w && my >= sy + (icon_h + 10) * i && my < sy + (icon_h + 10) * i + icon_h)
                        slot = i;
                if (slot == 0) g_about_open = 1;
                else if (slot == 1) open_new_terminal();
                else if (slot == 2) { explorer_open(NULL); g_files_front = 1; }
                else if (slot == 3) open_wallpaper_app();
                else if (slot == 4) { gui_set_enabled(0); return; }
            }
        }

        /* a menu entry ("Exit to shell") may have closed the desktop */
        if (!g_gui_enabled) return;

        /* drag any terminal windows while holding left */
        for (int i = 0; i < TERM_WIN_MAX; i++) {
            if (!g_terms[i].open) continue;
            if (left && g_terms[i].dragging) {
                g_terms[i].x = mx - g_terms[i].drag_dx;
                g_terms[i].y = my - g_terms[i].drag_dy;
                clamp_win(fi, &g_terms[i]);
            }
            if (!left) g_terms[i].dragging = 0;
        }
        explorer_mouse(mx, my, left);

        gui_view_t view;
        capture_view(&view, sec);
        int changed = g_force_redraw || memcmp(&view, &g_last_view, sizeof(view)) != 0;

        if (!changed) {
            /* only the mouse moved: restore its old spot, draw it anew */
            if (mx != drawn_mx || my != drawn_my) {
                fb_present_rect(drawn_mx, drawn_my, CURSOR_W, CURSOR_H);
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
    g_about_open = 0;
    g_wallpaper_open = 0;
    g_force_redraw = 1;
    if (!enabled) explorer_close();
    /* Hide (don't tear down) any open windows: their vts and shell tasks
     * are permanent for the OS's lifetime (see term_win_t.vt), so a later
     * startx can bring them straight back instead of every window losing
     * its running state on every stopx. */
    for (int i = 0; i < TERM_WIN_MAX; i++) g_terms[i].open = 0;

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

    /* Frontmost OPEN window, if any - g_term_order always lists every
     * slot, closed or not, so this has to skip closed ones explicitly. */
    for (int oi = TERM_WIN_MAX - 1; oi >= 0; oi--) {
        int wi = g_term_order[oi];
        if (g_terms[wi].open) return g_terms[wi].vt;
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

static void menu_activate(void) {
    if (g_menu_sel == 0) {
        if (gfx_available()) {
            g_menu_open = 0;
            g_about_open = 1;
            return;
        } else {
            menu_close_redraw();
            show_about();
        }
        return;
    }
    if (g_menu_sel == 1) {
        if (gfx_available()) {
            g_menu_open = 0;
            g_about_open = 0;
            open_new_terminal();
        } else {
            menu_close_redraw();
        }
        return; /* "Terminal" just closes the menu */
    }
    if (g_menu_sel == 2) {
        if (gfx_available()) {
            g_menu_open = 0;
            explorer_open(NULL);
            g_files_front = 1;
        } else {
            menu_close_redraw();          /* desktop only */
        }
        return;
    }
    if (g_menu_sel == 3) {
        if (gfx_available()) {
            open_wallpaper_app();
        } else {
            menu_close_redraw();
        }
        return;
    }
    if (g_menu_sel == 4) {
        gui_set_enabled(0);
        return;
    }
}

void gui_terminal_run(const char* cmd) {
    open_new_terminal();
    g_files_front = 0;              /* the new terminal comes up in front */
    /* typed into the new window: it has the keyboard focus now */
    keyboard_inject(cmd);
}

int gui_open_files(const char* path) {
    if (!gfx_available() || !g_gui_enabled) return 0;
    explorer_open(path);
    g_files_front = 1;
    return 1;
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
