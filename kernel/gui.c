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
#include "wallpaper_data.h"

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

#define TASKBAR_ROW (VGA_HEIGHT - 1)

static int g_menu_open = 0;
static int g_menu_sel = 0; /* 0=About, 1=Terminal, 2=Wallpaper, 3=Quit GUI */
static uint32_t g_last_clock_sec = (uint32_t)-1;
static int g_gui_enabled = 0; /* like startx: default off */
static int g_about_open = 0;
static int g_wallpaper_open = 0;
static int g_wallpaper_sel = 0;

typedef struct {
    const char* name;
    const char* png_path;
    uint32_t base;
    uint32_t accent;
    int pattern;
    const uint8_t* pixels; /* real RGB888 image data, see wallpaper_data.h */
} wallpaper_t;

static const wallpaper_t g_wallpapers[] = {
    {"Midnight Blue", "assets/wallpapers/midnight-blue.png", 0x00151F30u, 0x001B2A40u, 0, wallpaper_midnight_blue},
    {"Forest Night",  "assets/wallpapers/forest-night.png",  0x00162A1Eu, 0x001D3A29u, 1, wallpaper_forest_night},
    {"Graphite",      "assets/wallpapers/graphite.png",      0x00252528u, 0x00323236u, 2, wallpaper_graphite},
    {"Royal Purple",  "assets/wallpapers/royal-purple.png",  0x00261D3Au, 0x00362A52u, 0, wallpaper_royal_purple},
    {"Sunset Grid",   "assets/wallpapers/sunset-grid.png",   0x0030292Au, 0x004F3A34u, 2, wallpaper_sunset_grid},
    {"Ocean Wave",    "assets/wallpapers/ocean-wave.png",    0x00131F2Du, 0x001B3A58u, 1, wallpaper_ocean_wave},
    {"Cyber Mint",    "assets/wallpapers/cyber-mint.png",    0x00152A2Au, 0x001E4A4Au, 0, wallpaper_cyber_mint},
    {"Amber Mesh",    "assets/wallpapers/amber-mesh.png",    0x0033261Bu, 0x00513C22u, 2, wallpaper_amber_mesh},
    {"Azure Flow",    "assets/wallpapers/azure-flow.jpg",    0x00142E5Cu, 0x002E5CA0u, 1, wallpaper_azure_flow},
};

#define WALLPAPER_COUNT ((int)(sizeof(g_wallpapers) / sizeof(g_wallpapers[0])))
static uint32_t g_wallpaper_color = 0x00151F30u;
static uint32_t g_wallpaper_accent = 0x001B2A40u;
static int g_wallpaper_pattern = 0;
static const uint8_t* g_wallpaper_pixels = wallpaper_midnight_blue;
typedef struct {
    int open;
    int vt;
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

/* Bilinear upscale of a WALLPAPER_IMG_W x WALLPAPER_IMG_H RGB888 image to
 * fill the framebuffer. Integer-only fixed-point (16.16) math throughout -
 * deliberately no float/double, matching -mno-sse/-mno-mmx in the Makefile:
 * this kernel doesn't assume the target CPU has any FPU/SIMD support
 * beyond plain integer instructions, so this doesn't either. Smooth
 * enough for real photos (thin lines, gradients) instead of the blocky
 * look plain nearest-neighbor gave the fine detail in e.g. azure-flow. */
static void draw_wallpaper_bitmap(const fb_info_t* fi, const uint8_t* pixels) {
    int fb_w = (int)fi->width;
    int fb_h = (int)fi->height;
    if (fb_w <= 1 || fb_h <= 1) return;

    int32_t step_x = (int32_t)(((WALLPAPER_IMG_W - 1) << 16) / (fb_w - 1));
    int32_t step_y = (int32_t)(((WALLPAPER_IMG_H - 1) << 16) / (fb_h - 1));

    for (int y = 0; y < fb_h; y++) {
        int32_t sy_fixed = (int32_t)y * step_y;
        int sy0 = (int)(sy_fixed >> 16);
        int fy = (int)(sy_fixed & 0xFFFF);
        int sy1 = (sy0 + 1 < WALLPAPER_IMG_H) ? sy0 + 1 : sy0;

        for (int x = 0; x < fb_w; x++) {
            int32_t sx_fixed = (int32_t)x * step_x;
            int sx0 = (int)(sx_fixed >> 16);
            int fx = (int)(sx_fixed & 0xFFFF);
            int sx1 = (sx0 + 1 < WALLPAPER_IMG_W) ? sx0 + 1 : sx0;

            const uint8_t* p00 = &pixels[(sy0 * WALLPAPER_IMG_W + sx0) * 3];
            const uint8_t* p10 = &pixels[(sy0 * WALLPAPER_IMG_W + sx1) * 3];
            const uint8_t* p01 = &pixels[(sy1 * WALLPAPER_IMG_W + sx0) * 3];
            const uint8_t* p11 = &pixels[(sy1 * WALLPAPER_IMG_W + sx1) * 3];

            uint32_t rgb = 0;
            for (int c = 0; c < 3; c++) {
                int top    = p00[c] + (((p10[c] - p00[c]) * fx) >> 16);
                int bottom = p01[c] + (((p11[c] - p01[c]) * fx) >> 16);
                int val    = top + (((bottom - top) * fy) >> 16);
                rgb = (rgb << 8) | (uint32_t)(val & 0xFF);
            }
            fb_putpixel(x, y, rgb);
        }
    }
}

static void draw_desktop_texture(const fb_info_t* fi, uint32_t c0, uint32_t c1, const uint8_t* pixels) {
    if (!fi) return;
    if (pixels) {
        draw_wallpaper_bitmap(fi, pixels);
        return;
    }
    gfx_clear(c0);
    if (g_wallpaper_pattern == 1) {
        for (int y = 0; y < (int)fi->height; y += 4) {
            int off = (y / 4) & 7;
            for (int x = off; x < (int)fi->width; x += 16) {
                gfx_fill_rect(x, y, 8, 2, c1);
            }
        }
        return;
    }
    if (g_wallpaper_pattern == 2) {
        for (int y = 0; y < (int)fi->height; y += 24) gfx_fill_rect(0, y, (int)fi->width, 1, c1);
        for (int x = 0; x < (int)fi->width; x += 24) gfx_fill_rect(x, 0, 1, (int)fi->height, c1);
        return;
    }
    for (int y = 0; y < (int)fi->height; y += 3) {
        for (int x = ((y >> 1) & 1); x < (int)fi->width; x += 3) {
            gfx_fill_rect(x, y, 1, 1, c1);
        }
    }
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

static void draw_icon_power(int x, int y, uint32_t bg) {
    (void)bg;
    draw_bevel_box(x, y, 12, 12, 0x00412A2Au, 0x00764A4Au, 0x00170D0Du);
    gfx_draw_text(x + 3, y + 2, "o", 0x00FFFFFFu, 0x00412A2Au);
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
        if (!g_terms[i].open) {
            int vt = terminal_vt_alloc();
            if (vt < 0) return -1;
            g_terms[i].open = 1;
            g_terms[i].vt = vt;
            g_terms[i].dragging = 0;
            bring_term_front(i);
            terminal_vt_set_active(vt);
            return i;
        }
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

    for (int y = 0; y < max_rows; y++) {
        for (int x = 0; x < max_cols; x++) {
            int idx = y * stride + x;
            uint8_t color = cols[idx];
            uint8_t fg = color & 0x0F;
            uint8_t bg = (color >> 4) & 0x0F;
            gfx_draw_char(cx + x * 8, cy + y * 8, chars[idx],
                          vga_color_rgb(fg), vga_color_rgb(bg));
        }
    }

    {
        size_t cr = 0, cc = 0;
        terminal_vt_get_cursor(win->vt, &cr, &cc);
        if ((int)cr < max_rows && (int)cc < max_cols) {
            int idx = (int)cr * stride + (int)cc;
            uint8_t color = cols[idx];
            uint8_t fg = color & 0x0F;
            gfx_fill_rect(cx + (int)cc * 8, cy + (int)cr * 8 + 7, 8, 1, vga_color_rgb(fg));
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
    const size_t menu_y = TASKBAR_ROW - 4;
    const size_t menu_w = 20;
    const size_t menu_h = 4;

    /* box background */
    for (size_t y = 0; y < menu_h; y++) {
        for (size_t x = 0; x < menu_w; x++) {
            terminal_putentryat(' ', VGA_COLOR_WHITE, VGA_COLOR_DARK_GREY, menu_y + y, menu_x + x);
        }
    }

    /* items */
    for (int i = 0; i < 4; i++) {
        uint8_t fg = VGA_COLOR_WHITE;
        uint8_t bg = VGA_COLOR_DARK_GREY;
        if (g_menu_sel == i) { fg = VGA_COLOR_WHITE; bg = VGA_COLOR_BLUE; }

        const char* item = (i == 0) ? " About app" : (i == 1) ? " Terminal" : (i == 2) ? " Wallpaper" : " Quit GUI";
        /* fill the rest of the row in highlight color for clean look */
        for (size_t x = 0; x < menu_w - 2; x++) {
            terminal_putentryat(' ', fg, bg, menu_y + (size_t)i, menu_x + 1 + x);
        }
        terminal_write_at(item,
                          fg, bg, menu_y + (size_t)i, menu_x + 1);
    }

    /* hint row */
    terminal_write_at(" Enter = open", VGA_COLOR_LIGHT_GREY, VGA_COLOR_DARK_GREY, menu_y + 4, menu_x + 1);
}

static void menu_close_redraw(void) {
    g_menu_open = 0;
    draw_taskbar();
    /* clear menu area */
    for (size_t y = 0; y < 4; y++) {
        for (size_t x = 0; x < 20; x++) {
            terminal_putentryat(' ', VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK, (TASKBAR_ROW - 4) + y, x);
        }
    }
    draw_taskbar();
}

static void open_wallpaper_app(void) {
    g_menu_open = 0;
    g_about_open = 0;
    g_wallpaper_open = 1;
}

static void show_about(void) {
    size_t saved_r, saved_c;
    terminal_get_cursor(&saved_r, &saved_c);

    terminal_clear();

    const sysinfo_t* si = sysinfo_get();
    terminal_write_color("Banana OS 0.3 - About app\n", VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    terminal_writeln("----------------------------------------");
    terminal_write_color("Version: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    terminal_writeln("0.3");
    terminal_write_color("Display: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    terminal_writeln("VGA text 80x25 + basic GUI taskbar");
    terminal_write_color("CPU: ", VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    terminal_writeln(si->cpu_brand[0] ? si->cpu_brand : "Unknown");

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

void gui_poll(void) {
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
        /* framebuffer desktop loop (throttled + backbuffer to avoid flicker) */
        const fb_info_t* fi = fb_info();
        if (!fi || fi->width == 0 || fi->height == 0) return;

        /* backbuffer sized for requested mode (800x600) */
        static int mx = 100, my = 100;
        static int last_mx = -1, last_my = -1;
        static uint32_t last_sec = (uint32_t)-1;
        static int last_open = -1, last_sel = -1;
        static uint32_t last_frame_tick = 0;
        static int prev_left = 0;

        if (!g_backbuf_active) {
            fb_set_backbuffer(g_desktop_backbuf, 800u, 600u);
            g_backbuf_active = 1;
            last_mx = -1; last_my = -1;
            last_sec = (uint32_t)-1;
            last_open = -1; last_sel = -1;
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

        if (click) {
            if (g_about_open) {
                g_about_open = 0;
            }
            if (g_wallpaper_open) {
                int ww = 560, wh = 330;
                int wx0 = ((int)fi->width - ww) / 2;
                int wy0 = ((int)fi->height - wh) / 2;
                int consumed = 0;

                if (mx >= wx0 && mx < wx0 + ww && my >= wy0 && my < wy0 + wh) {
                    int list_y = wy0 + 56;
                    int col_w = (ww - 36) / 2;
                    int item_h = 28;
                    for (int i = 0; i < WALLPAPER_COUNT; i++) {
                        int col = i & 1;
                        int row = i >> 1;
                        int ix = wx0 + 12 + col * col_w;
                        int iy = list_y + row * 32;
                        if (mx >= ix && mx < ix + col_w - 12 &&
                            my >= iy && my < iy + item_h) {
                            g_wallpaper_sel = i;
                            g_wallpaper_color = g_wallpapers[i].base;
                            g_wallpaper_accent = g_wallpapers[i].accent;
                            g_wallpaper_pattern = g_wallpapers[i].pattern;
                            g_wallpaper_pixels = g_wallpapers[i].pixels;
                            consumed = 1;
                            break;
                        }
                    }
                } else {
                    g_wallpaper_open = 0;
                    consumed = 1;
                }
                if (consumed) click = 0;
            }

            /* click on taskbar Start */
            if (my >= bar_y + 5 && my < bar_y + 23 && mx >= start_x - 4 && mx < (start_x - 4 + start_w)) {
                g_menu_open = !g_menu_open;
                if (g_menu_open) g_menu_sel = 0;
            }

            /* click on Quit GUI button */
            if (my >= bar_y + 5 && my < bar_y + 23 && mx >= quit_x - 4 && mx < (quit_x - 4 + quit_w)) {
                gui_set_enabled(0);
            }

            /* terminal windows hit testing (front-to-back) */
            for (int oi = TERM_WIN_MAX - 1; oi >= 0; oi--) {
                int wi = g_term_order[oi];
                term_win_t* w = &g_terms[wi];
                if (!w->open) continue;

                int title_h = 20;
                if (mx >= w->x && mx < w->x + w->w && my >= w->y && my < w->y + w->h) {
                    bring_term_front(wi);
                    terminal_vt_set_active(w->vt);

                    int close_x = w->x + w->w - 28;
                    if (mx >= close_x && mx < close_x + 24 && my >= w->y + 2 && my < w->y + 18) {
                        w->open = 0;
                        terminal_vt_free(w->vt);
                        w->vt = 0;
                    } else if (my < w->y + title_h) {
                        w->dragging = 1;
                        w->drag_dx = mx - w->x;
                        w->drag_dy = my - w->y;
                    }
                    break;
                }
            }

            /* click in menu items */
            if (g_menu_open) {
                int menu_w = 220;
                int menu_h = 140;
                int menu_x = 8;
                int menu_y = (int)fi->height - bar_h - menu_h - 8;

                int item_x0 = menu_x + 24;
                int item_x1 = menu_x + menu_w - 8;

                int item0_y0 = menu_y + 8;
                int item0_y1 = item0_y0 + 24;
                int item1_y0 = menu_y + 36;
                int item1_y1 = item1_y0 + 24;
                int item2_y0 = menu_y + 64;
                int item2_y1 = item2_y0 + 24;
                int item3_y0 = menu_y + 92;
                int item3_y1 = item3_y0 + 24;

                if (mx >= item_x0 && mx < item_x1) {
                    if (my >= item0_y0 && my < item0_y1) {
                        g_menu_sel = 0;
                        menu_activate();
                    } else if (my >= item1_y0 && my < item1_y1) {
                        g_menu_sel = 1;
                        menu_activate();
                    } else if (my >= item2_y0 && my < item2_y1) {
                        g_menu_sel = 2;
                        menu_activate();
                    } else if (my >= item3_y0 && my < item3_y1) {
                        g_menu_sel = 3;
                        menu_activate();
                    }
                }
            }
        }

        /* throttle to ~30 fps max */
        uint32_t now_tick = timer_ticks();
        if ((int32_t)(now_tick - last_frame_tick) < 3 && /* <30ms */
            mx == last_mx && my == last_my &&
            sec == last_sec &&
            g_menu_open == last_open && g_menu_sel == last_sel) {
            return;
        }
        last_frame_tick = now_tick;

        /* redraw desktop (without cursor) into backbuffer */
        draw_desktop_texture(fi, g_wallpaper_color, g_wallpaper_accent, g_wallpaper_pixels);

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
            draw_icon_wallpaper(sx + 8, sy + (icon_h + 10) * 2 + 12, 0x0029313Du);
            gfx_draw_text(sx + 30, sy + (icon_h + 10) * 2 + 13, "Wallpaper", 0x00F0F6FFu, 0x0029313Du);
            draw_bevel_box(sx, sy + (icon_h + 10) * 3, icon_w, icon_h, 0x0029313Du, 0x00586678u, 0x0010151Eu);
            draw_icon_power(sx + 8, sy + (icon_h + 10) * 3 + 12, 0x0029313Du);
            gfx_draw_text(sx + 30, sy + (icon_h + 10) * 3 + 13, "Quit GUI", 0x00F0F6FFu, 0x0029313Du);

            /* click shortcuts */
            if (click && !g_about_open && !g_menu_open) {
                if (mx >= sx && mx < (sx + icon_w) && my >= sy && my < (sy + icon_h)) {
                    g_about_open = 1;
                } else if (mx >= sx && mx < (sx + icon_w) && my >= (sy + icon_h + 10) && my < (sy + icon_h + 10 + icon_h)) {
                    open_new_terminal();
                } else if (mx >= sx && mx < (sx + icon_w) && my >= (sy + (icon_h + 10) * 2) && my < (sy + (icon_h + 10) * 2 + icon_h)) {
                    open_wallpaper_app();
                } else if (mx >= sx && mx < (sx + icon_w) && my >= (sy + (icon_h + 10) * 3) && my < (sy + (icon_h + 10) * 3 + icon_h)) {
                    gui_set_enabled(0);
                }
            }
        }

        char clk[9];
        format_clock(clk);

        int clk_w = 8 * 8;
        int clk_x = (int)fi->width - clk_w - 8; /* clock stays at far right */

        /* toolbar */
        draw_bevel_box(0, bar_y, (int)fi->width, bar_h, 0x00192026u, 0x004F5A6Eu, 0x0010141Bu);
        draw_flux_toolbar_button(start_x - 4, bar_y + 5, 88, 18, "banana", g_menu_open);
        draw_flux_toolbar_button(quit_x - 4, bar_y + 5, 76, 18, "exit", 0);
        gfx_draw_text(clk_x, bar_y + 10, clk, 0x00E8EEF6u, 0x00192026u);

        /* root menu */
        if (g_menu_open) {
            int menu_w = 236;
            int menu_h = 140;
            int menu_x = 8;
            int menu_y = (int)fi->height - bar_h - menu_h - 8;
            draw_bevel_box(menu_x, menu_y, menu_w, menu_h, 0x001D232Cu, 0x00505E74u, 0x0010151Du);
            gfx_fill_rect(menu_x + 2, menu_y + 2, 18, menu_h - 4, 0x00354463u);
            gfx_draw_text(menu_x + 5, menu_y + 8, "B", 0x00F4F8FFu, 0x00354463u);

            uint32_t sel_bg0 = (g_menu_sel == 0) ? 0x003A4A66u : 0x001D232Cu;
            uint32_t sel_bg1 = (g_menu_sel == 1) ? 0x003A4A66u : 0x001D232Cu;
            uint32_t sel_bg2 = (g_menu_sel == 2) ? 0x003A4A66u : 0x001D232Cu;
            uint32_t sel_bg3 = (g_menu_sel == 3) ? 0x003A4A66u : 0x001D232Cu;
            gfx_fill_rect(menu_x + 24, menu_y + 8, menu_w - 32, 24, sel_bg0);
            gfx_fill_rect(menu_x + 24, menu_y + 36, menu_w - 32, 24, sel_bg1);
            gfx_fill_rect(menu_x + 24, menu_y + 64, menu_w - 32, 24, sel_bg2);
            gfx_fill_rect(menu_x + 24, menu_y + 92, menu_w - 32, 24, sel_bg3);
            draw_icon_info(menu_x + 28, menu_y + 14, sel_bg0);
            draw_icon_terminal(menu_x + 28, menu_y + 42, sel_bg1);
            draw_icon_wallpaper(menu_x + 28, menu_y + 70, sel_bg2);
            draw_icon_power(menu_x + 28, menu_y + 98, sel_bg3);
            gfx_draw_text(menu_x + 36, menu_y + 14, "About", 0x00E8EEF6u, sel_bg0);
            gfx_draw_text(menu_x + 36, menu_y + 42, "Terminal", 0x00E8EEF6u, sel_bg1);
            gfx_draw_text(menu_x + 36, menu_y + 70, "Wallpaper", 0x00E8EEF6u, sel_bg2);
            gfx_draw_text(menu_x + 36, menu_y + 98, "Exit to shell", 0x00E8EEF6u, sel_bg3);
        }

        if (g_about_open) {
            int mw = 420, mh = 160;
            int mx0 = ((int)fi->width - mw) / 2;
            int my0 = ((int)fi->height - mh) / 2;
            draw_bevel_box(mx0, my0, mw, mh, 0x001D232Cu, 0x00505D72u, 0x0010141Cu);
            draw_bevel_box(mx0 + 3, my0 + 3, mw - 6, 19, 0x00384562u, 0x00647692u, 0x00111923u);
            gfx_draw_text(mx0 + 10, my0 + 7, "About Banana OS 0.3", 0x00FFFFFFu, 0x00384562u);
            gfx_draw_text(mx0 + 16, my0 + 40, "Banana OS 0.3", 0x00FFFFFFu, 0x001D232Cu);
            gfx_draw_text(mx0 + 16, my0 + 56, "Theme: Fluxbox-inspired toolbar/menu", 0x00FFFFFFu, 0x001D232Cu);
            gfx_draw_text(mx0 + 16, my0 + 80, "Click anywhere to close", 0x00AAAAAAu, 0x001D232Cu);
        }

        if (g_wallpaper_open) {
            int ww = 560, wh = 330;
            int wx0 = ((int)fi->width - ww) / 2;
            int wy0 = ((int)fi->height - wh) / 2;
            draw_bevel_box(wx0, wy0, ww, wh, 0x001D232Cu, 0x00505D72u, 0x0010141Cu);
            draw_bevel_box(wx0 + 3, wy0 + 3, ww - 6, 19, 0x00384562u, 0x00647692u, 0x00111923u);
            gfx_draw_text(wx0 + 10, wy0 + 7, "Wallpaper App", 0x00FFFFFFu, 0x00384562u);
            gfx_draw_text(wx0 + 14, wy0 + 30, "Pick a PNG wallpaper:", 0x00E8EEF6u, 0x001D232Cu);

            int list_y = wy0 + 56;
            int col_w = (ww - 36) / 2;
            int item_h = 28;
            for (int i = 0; i < WALLPAPER_COUNT; i++) {
                int col = i & 1;
                int row = i >> 1;
                int ix = wx0 + 12 + col * col_w;
                int iy = list_y + row * 32;
                uint32_t bg = (i == g_wallpaper_sel) ? 0x003A4A66u : 0x001D232Cu;
                draw_bevel_box(ix, iy, col_w - 12, item_h, bg, 0x0056667Fu, 0x0010151Fu);
                gfx_fill_rect(ix + 6, iy + 6, 16, 16, g_wallpapers[i].base);
                gfx_draw_text(ix + 28, iy + 10, g_wallpapers[i].name, 0x00E8EEF6u, bg);
            }
            gfx_draw_text(wx0 + 14, wy0 + wh - 42, g_wallpapers[g_wallpaper_sel].png_path, 0x00A9B7C8u, 0x001D232Cu);
            gfx_draw_text(wx0 + 14, wy0 + wh - 26, "Click item to apply, click outside to close", 0x00AAAAAAu, 0x001D232Cu);
        }

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

        /* draw terminal windows back-to-front */
        for (int oi = 0; oi < TERM_WIN_MAX; oi++) {
            term_win_t* w = &g_terms[g_term_order[oi]];
            draw_terminal_window(fi, w);
        }

        /* push backbuffer to framebuffer once per frame */
        fb_present();

        /* mouse cursor */
        for (int cy = 0; cy < 10; cy++) {
            for (int cx = 0; cx < 6; cx++) {
                if (cx == 0 || cy == 0 || cx == cy / 2) {
                    fb_putpixel_direct(mx + cx, my + cy, 0x00FFFFFFu);
                }
            }
        }

        last_mx = mx; last_my = my;
        last_sec = sec;
        last_open = g_menu_open;
        last_sel = g_menu_sel;

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
    for (int i = 0; i < TERM_WIN_MAX; i++) {
        if (g_terms[i].open) terminal_vt_free(g_terms[i].vt);
        g_terms[i].open = 0;
        g_terms[i].vt = 0;
    }

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
            open_wallpaper_app();
        } else {
            menu_close_redraw();
        }
        return;
    }
    if (g_menu_sel == 3) {
        gui_set_enabled(0);
        return;
    }
}

int gui_handle_arrow(char esc_code) {
    if (!g_menu_open) return 0;
    if (esc_code == 'A') { /* up */
        if (g_menu_sel > 0) g_menu_sel--;
        draw_menu();
        return 1;
    }
    if (esc_code == 'B') { /* down */
        if (g_menu_sel < 3) g_menu_sel++;
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

