#include "terminal.h"
#include "types.h"
#include "fb.h"
#include "gfx.h"
#include "serial.h"
#include "tty.h"
#include "kstring.h"
#include "timer.h"
#include "task.h"
#include "kheap.h"

#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_MEMORY ((uint16_t*)0xB8000)

/* Framebuffer console max grid (for 800x600 @ 8x8 => 100x75) */
#define FB_MAX_COLS 100
#define FB_MAX_ROWS 75

static size_t   term_row;
static size_t   term_col;
static uint8_t  term_color;
static uint16_t *term_buf;
static size_t   term_reserved_bottom = 0;
static terminal_mode_t term_mode = TERMINAL_MODE_VGA_TEXT;
static int term_fb_scale = 1; /* smaller font by default */
static size_t term_fb_cols = VGA_WIDTH;
static size_t term_fb_rows = VGA_HEIGHT;

/* framebuffer virtual terminals (count shared via terminal.h so other
 * modules can size their own per-window state to match) */
#define VT_MAX TERMINAL_VT_MAX
static char    vt_chars[VT_MAX][FB_MAX_ROWS][FB_MAX_COLS];
static uint8_t vt_colors[VT_MAX][FB_MAX_ROWS][FB_MAX_COLS];
static size_t  vt_row[VT_MAX];
static size_t  vt_col[VT_MAX];
static uint8_t vt_color[VT_MAX];
static uint8_t vt_used[VT_MAX] = {1, 0, 0, 0}; /* vt0 reserved */
static int     vt_active = 0;

/* bumped on every change to any vt (text, colours, cursor): the GUI
 * compares it to decide whether terminal windows need repainting */
static volatile uint32_t g_term_gen = 1;

uint32_t terminal_generation(void) { return g_term_gen; }

static uint32_t vga_color_rgb(uint8_t c) {
    /* simple VGA palette */
    static const uint32_t pal[16] = {
        0x00000000u, 0x000000AAu, 0x0000AA00u, 0x0000AAAAu,
        0x00AA0000u, 0x00AA00AAu, 0x00AA5500u, 0x00AAAAAAu,
        0x00555555u, 0x005555FFu, 0x0055FF55u, 0x0055FFFFu,
        0x00FF5555u, 0x00FF55FFu, 0x00FFFF55u, 0x00FFFFFFu,
    };
    return pal[c & 0x0F];
}

static size_t terminal_fb_cols(void) {
    return term_fb_cols;
}

static size_t terminal_fb_rows(void) {
    return term_fb_rows;
}

static size_t terminal_width(void) {
    if (term_mode == TERMINAL_MODE_FRAMEBUFFER || term_mode == TERMINAL_MODE_SUSPENDED) return terminal_fb_cols();
    return VGA_WIDTH;
}

static size_t terminal_height(void) {
    if (term_mode == TERMINAL_MODE_FRAMEBUFFER || term_mode == TERMINAL_MODE_SUSPENDED) return terminal_fb_rows();
    return VGA_HEIGHT;
}

static void fb_recompute_grid(void) {
    if (!fb_available()) {
        term_fb_cols = VGA_WIDTH;
        term_fb_rows = VGA_HEIGHT;
        return;
    }

    const fb_info_t* fi = fb_info();
    if (!fi || fi->width == 0 || fi->height == 0) {
        term_fb_cols = VGA_WIDTH;
        term_fb_rows = VGA_HEIGHT;
        return;
    }

    int scale = term_fb_scale;
    if (scale < 1) scale = 1;

    uint32_t cols = fi->width  / (uint32_t)(8u * (uint32_t)scale);
    uint32_t rows = fi->height / (uint32_t)(8u * (uint32_t)scale);
    if (cols < 10) cols = 10;
    if (rows < 5)  rows = 5;
    if (cols > FB_MAX_COLS) cols = FB_MAX_COLS;
    if (rows > FB_MAX_ROWS) rows = FB_MAX_ROWS;

    term_fb_cols = (size_t)cols;
    term_fb_rows = (size_t)rows;

    if (term_col >= term_fb_cols) term_col = term_fb_cols - 1;
    if (term_row >= term_fb_rows) term_row = term_fb_rows - 1;
}

/* ── framebuffer console: deferred rendering ─────────────────────────
 * The framebuffer console (the shell before `startx`) always shows vt0.
 * Output only updates the text grid and marks rows dirty; the pixels are
 * brought up to date by terminal_flush(), which runs at most every ~25 ms
 * while output streams, whenever the system goes idle (gui_poll() and
 * the blocking waits call it), and immediately for interactive cursor
 * moves. Consecutive scrolls are coalesced into one block move of the
 * pixels. Painting every character (and moving the whole screen for
 * every line) as it was printed made long output crawl - `cat` of an
 * 8000-line file took ~28 s. */

#define CONSOLE_VT 0
#define FLUSH_INTERVAL_MS 25

static uint8_t  g_dirty[FB_MAX_ROWS];
static int      g_any_dirty = 1;
static uint32_t g_pending_scroll = 0;
static uint32_t g_last_flush_ms = 0;

/* where the underline cursor is currently painted */
static int    g_ov_valid = 0;
static size_t g_ov_row, g_ov_col;

static int fb_console_on(void) {
    return term_mode == TERMINAL_MODE_FRAMEBUFFER && gfx_available();
}

/* is the text being written right now what the console shows? */
static int fb_console_target(void) {
    return fb_console_on() && vt_active == CONSOLE_VT;
}

static void mark_dirty(size_t row) {
    if (row < FB_MAX_ROWS) {
        g_dirty[row] = 1;
        g_any_dirty = 1;
    }
}

static void mark_all_dirty(void) {
    for (size_t r = 0; r < FB_MAX_ROWS; r++) g_dirty[r] = 1;
    g_any_dirty = 1;
}

static void fb_draw_cell(size_t row, size_t col) {
    if (row >= terminal_fb_rows() || col >= terminal_fb_cols()) return;
    uint8_t color = vt_colors[CONSOLE_VT][row][col];
    int scale = term_fb_scale < 1 ? 1 : term_fb_scale;
    gfx_draw_char_scaled((int)col * 8 * scale, (int)row * 8 * scale, scale, vt_chars[CONSOLE_VT][row][col],
                         vga_color_rgb(color & 0x0F), vga_color_rgb((color >> 4) & 0x0F));
}

static void fb_draw_cursor_overlay(size_t row, size_t col) {
    if (row >= terminal_fb_rows() || col >= terminal_fb_cols()) return;
    g_ov_valid = 1;
    g_ov_row = row;
    g_ov_col = col;

    uint8_t fg = vt_colors[CONSOLE_VT][row][col] & 0x0F;
    int scale = term_fb_scale < 1 ? 1 : term_fb_scale;
    int h = (scale > 1) ? 2 : 1;
    gfx_fill_rect((int)col * 8 * scale, (int)row * 8 * scale + (8 * scale - h), 8 * scale, h, vga_color_rgb(fg));
}

static void console_cursor(size_t* row, size_t* col) {
    /* the live copy is term_row/col while vt0 is the active vt */
    *row = (vt_active == CONSOLE_VT) ? term_row : vt_row[CONSOLE_VT];
    *col = (vt_active == CONSOLE_VT) ? term_col : vt_col[CONSOLE_VT];
}

void terminal_flush(void) {
    if (!fb_console_on()) return;
    size_t cr, cc;
    console_cursor(&cr, &cc);
    int cursor_moved = !g_ov_valid || g_ov_row != cr || g_ov_col != cc;
    if (!g_any_dirty && !g_pending_scroll && !cursor_moved) return;

    size_t rows = terminal_fb_rows();
    int cell = 8 * (term_fb_scale < 1 ? 1 : term_fb_scale);

    if (g_pending_scroll) {
        if (g_pending_scroll >= rows) {
            mark_all_dirty();
        } else {
            /* all the scrolls since the last flush as one block move; the
             * underline moved up with the pixels, so repaint where it went */
            fb_scroll_up(0, (int)rows * cell, (int)g_pending_scroll * cell, 0);
            if (g_ov_valid && g_ov_row >= g_pending_scroll) mark_dirty(g_ov_row - g_pending_scroll);
        }
        g_pending_scroll = 0;
        g_ov_valid = 0;
    }
    if (g_ov_valid && (g_ov_row != cr || g_ov_col != cc)) mark_dirty(g_ov_row);   /* erase old underline */

    size_t cols = terminal_fb_cols();
    for (size_t r = 0; r < rows; r++) {
        if (!g_dirty[r]) continue;
        g_dirty[r] = 0;
        for (size_t c = 0; c < cols; c++) fb_draw_cell(r, c);
    }
    g_any_dirty = 0;
    fb_draw_cursor_overlay(cr, cc);
    fb_present();
    g_last_flush_ms = timer_ms();
}

static void maybe_flush(void) {
    if ((uint32_t)(timer_ms() - g_last_flush_ms) >= FLUSH_INTERVAL_MS) terminal_flush();
}

static void fb_refresh_cursor(size_t old_row, size_t old_col) {
    /* interactive cursor movement: paint right away */
    (void)old_row; (void)old_col;
    if (fb_console_target()) terminal_flush();
}

static void fb_redraw_all(void) {
    if (!fb_console_on()) return;
    g_ov_valid = 0;
    g_pending_scroll = 0;
    mark_all_dirty();
    terminal_flush();
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0,%1"::"a"(val),"Nd"(port));
}

static void terminal_update_cursor(void) {
    uint16_t pos = (uint16_t)(term_row * VGA_WIDTH + term_col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

static void terminal_cursor_enable(void) {
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x0E); /* start scanline */
    outb(0x3D4, 0x0B);
    outb(0x3D5, 0x0F); /* end scanline */
}

static inline uint8_t vga_entry_color(uint8_t fg, uint8_t bg) {
    return fg | bg << 4;
}

static inline uint16_t vga_entry(char c, uint8_t color) {
    return (uint16_t)c | (uint16_t)color << 8;
}

static size_t terminal_usable_height(void) {
    size_t h = terminal_height();
    if (term_reserved_bottom >= h) return 0;
    return h - term_reserved_bottom;
}

void terminal_init(void) {
    term_row   = 0;
    term_col   = 0;
    term_color = vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    term_buf   = VGA_MEMORY;
    term_reserved_bottom = 0;
    terminal_cursor_enable();

    /* If framebuffer exists, default shell output to framebuffer (pre-startx). */
    if (fb_available()) {
        term_mode = TERMINAL_MODE_FRAMEBUFFER;
        term_fb_scale = 1; /* smaller font */
    }
    vt_active = 0;
    vt_row[0] = 0;
    vt_col[0] = 0;
    vt_color[0] = term_color;
    fb_recompute_grid();
    terminal_clear();
}

static int capturing(void);

void terminal_clear(void) {
    if (capturing()) return;
    tty_clear_screen((int)vt_active);
    g_term_gen++;
    size_t h = terminal_usable_height();
    size_t w = terminal_width();

    if (term_mode == TERMINAL_MODE_SUSPENDED) {
        for (size_t y = 0; y < h; y++) {
            for (size_t x = 0; x < w; x++) {
                vt_chars[vt_active][y][x] = ' ';
                vt_colors[vt_active][y][x] = vt_color[vt_active];
            }
        }
        vt_row[vt_active] = 0;
        vt_col[vt_active] = 0;
        term_row = 0;
        term_col = 0;
        return;
    }

    if (term_mode == TERMINAL_MODE_FRAMEBUFFER && gfx_available()) {
        for (size_t y = 0; y < h; y++) {
            for (size_t x = 0; x < w; x++) {
                vt_chars[vt_active][y][x] = ' ';
                vt_colors[vt_active][y][x] = vt_color[vt_active];
            }
        }
        term_row = 0;
        term_col = 0;
        vt_row[vt_active] = 0;
        vt_col[vt_active] = 0;
        fb_redraw_all();
        return;
    }

    for (size_t y = 0; y < h; y++) {
        for (size_t x = 0; x < w; x++) {
            term_buf[y * VGA_WIDTH + x] = vga_entry(' ', term_color);
        }
    }
    term_row = 0;
    term_col = 0;
    terminal_update_cursor();
}

void terminal_setcolor(uint8_t fg, uint8_t bg) {
    term_color = vga_entry_color(fg, bg);
    vt_color[vt_active] = term_color;
}

static void terminal_scroll(void) {
    size_t h = terminal_usable_height();
    if (h == 0) return;
    size_t w = terminal_width();

    if ((term_mode == TERMINAL_MODE_FRAMEBUFFER && gfx_available()) || term_mode == TERMINAL_MODE_SUSPENDED) {
        memmove(&vt_chars[vt_active][0][0], &vt_chars[vt_active][1][0], (h - 1) * FB_MAX_COLS);
        memmove(&vt_colors[vt_active][0][0], &vt_colors[vt_active][1][0], (h - 1) * FB_MAX_COLS);
        for (size_t x = 0; x < w; x++) {
            vt_chars[vt_active][h - 1][x] = ' ';
            vt_colors[vt_active][h - 1][x] = vt_color[vt_active];
        }
        term_row = h - 1;
        term_col = 0;
        vt_row[vt_active] = term_row;
        vt_col[vt_active] = term_col;
        if (fb_console_target()) {
            /* dirty flags follow their text up; the pixels move at the
             * next flush, together with any further scrolls */
            memmove(g_dirty, g_dirty + 1, h - 1);
            g_dirty[h - 1] = 1;
            g_any_dirty = 1;
            g_pending_scroll++;
        }
        return;
    }

    for (size_t y = 1; y < h; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            term_buf[(y-1) * VGA_WIDTH + x] = term_buf[y * VGA_WIDTH + x];
    for (size_t x = 0; x < VGA_WIDTH; x++)
        term_buf[(h-1) * VGA_WIDTH + x] = vga_entry(' ', term_color);
    term_row = h - 1;
    term_col = 0;
}

static int g_serial_mirror = 1;

void terminal_serial_mirror(int on) {
    g_serial_mirror = on;
}

size_t terminal_get_width(void) {
    return terminal_width();
}

size_t terminal_get_height(void) {
    return terminal_usable_height();
}

/* Output capture for shell redirection (`cmd > file`): while a task
 * captures, what it prints goes into a buffer instead of the screen. */
static struct { int active, pid; char* buf; uint32_t len, cap; } g_cap;

int terminal_capture_start(void) {
    if (g_cap.active) return 0;
    g_cap.cap = 4096;
    g_cap.buf = (char*)kmalloc(g_cap.cap);
    if (!g_cap.buf) return 0;
    g_cap.len = 0;
    g_cap.pid = task_current_pid();
    g_cap.active = 1;
    return 1;
}

char* terminal_capture_stop(uint32_t* len) {
    char* b = g_cap.buf;
    *len = g_cap.len;
    g_cap.active = 0;
    g_cap.buf = NULL;
    return b;
}

static int capturing(void) {
    return g_cap.active && g_cap.pid == task_current_pid();
}

static void capture_putc(char c) {
    if (g_cap.len + 1 >= g_cap.cap) {
        if (g_cap.cap >= (32u << 20)) return;     /* the file size limit */
        char* nb = (char*)kmalloc(g_cap.cap * 2);
        if (!nb) return;
        memcpy(nb, g_cap.buf, g_cap.len);
        kfree(g_cap.buf);
        g_cap.buf = nb;
        g_cap.cap *= 2;
    }
    g_cap.buf[g_cap.len++] = c;
}

void terminal_putchar(char c) {
    if (capturing()) { capture_putc(c); return; }
    g_term_gen++;
    /* the console shell (vt0) is mirrored to the serial port */
    if (vt_active == 0 && g_serial_mirror) serial_putc(c);
    /* ...and an SSH session's vt to its client */
    if (g_serial_mirror && vt_active != 0) tty_mirror((int)vt_active, c);

    size_t h = terminal_usable_height();
    if (h == 0) return;
    size_t w = terminal_width();

    if (term_mode == TERMINAL_MODE_FRAMEBUFFER && gfx_available() && vt_active == CONSOLE_VT) {
        /* text grid only; the pixels follow in terminal_flush() */
        if (c == '\n') {
            term_col = 0;
            if (++term_row == h) terminal_scroll();
        } else if (c == '\r') {
            term_col = 0;
        } else if (c == '\b') {
            if (term_col > 0) {
                term_col--;
                vt_chars[CONSOLE_VT][term_row][term_col] = ' ';
                vt_colors[CONSOLE_VT][term_row][term_col] = vt_color[CONSOLE_VT];
                mark_dirty(term_row);
            }
        } else {
            vt_chars[CONSOLE_VT][term_row][term_col] = c;
            vt_colors[CONSOLE_VT][term_row][term_col] = vt_color[CONSOLE_VT];
            mark_dirty(term_row);
            if (++term_col == w) {
                term_col = 0;
                if (++term_row == h) terminal_scroll();
            }
        }
        vt_row[CONSOLE_VT] = term_row;
        vt_col[CONSOLE_VT] = term_col;
        maybe_flush();
        return;
    }

    if (term_mode == TERMINAL_MODE_SUSPENDED || (term_mode == TERMINAL_MODE_FRAMEBUFFER && gfx_available())) {
        /* Keep backing store up to date so GUI can render terminal window
         * (also: a hidden window's shell writing while the console is up) */
        if (c == '\n') {
            term_col = 0;
            if (++term_row == h) terminal_scroll();
            vt_row[vt_active] = term_row;
            vt_col[vt_active] = term_col;
            return;
        }
        if (c == '\r') { term_col = 0; return; }
        if (c == '\b') {
            if (term_col > 0) {
                term_col--;
                vt_chars[vt_active][term_row][term_col] = ' ';
                vt_colors[vt_active][term_row][term_col] = vt_color[vt_active];
                vt_row[vt_active] = term_row;
                vt_col[vt_active] = term_col;
            }
            return;
        }
        vt_chars[vt_active][term_row][term_col] = c;
        vt_colors[vt_active][term_row][term_col] = vt_color[vt_active];
        if (++term_col == w) {
            term_col = 0;
            if (++term_row == h) terminal_scroll();
        }
        vt_row[vt_active] = term_row;
        vt_col[vt_active] = term_col;
        return;
    }

    /* VGA text mode (no framebuffer) */
    if (c == '\n') {
        term_col = 0;
        if (++term_row == h)
            terminal_scroll();
        terminal_update_cursor();
        return;
    }
    if (c == '\r') {
        term_col = 0;
        terminal_update_cursor();
        return;
    }
    if (c == '\b') {
        if (term_col > 0) {
            term_col--;
            term_buf[term_row * VGA_WIDTH + term_col] = vga_entry(' ', term_color);
        }
        terminal_update_cursor();
        return;
    }

    term_buf[term_row * VGA_WIDTH + term_col] = vga_entry(c, term_color);

    if (++term_col == w) {
        term_col = 0;
        if (++term_row == h)
            terminal_scroll();
    }
    vt_row[vt_active] = term_row;
    vt_col[vt_active] = term_col;
    terminal_update_cursor();
}

void terminal_write(const char* str) {
    for (size_t i = 0; str[i]; i++)
        terminal_putchar(str[i]);
}

void terminal_writeln(const char* str) {
    terminal_write(str);
    terminal_putchar('\n');
}

void terminal_write_color(const char* str, uint8_t fg, uint8_t bg) {
    uint8_t saved = term_color;
    terminal_setcolor(fg, bg);
    terminal_write(str);
    term_color = saved;
}

void terminal_cursor_left(void) {
    g_term_gen++;
    if (term_col > 0) {
        size_t old_row = term_row, old_col = term_col;
        term_col--;
        if (term_mode != TERMINAL_MODE_FRAMEBUFFER) terminal_update_cursor();
        else fb_refresh_cursor(old_row, old_col);
    }
}

void terminal_cursor_right(void) {
    g_term_gen++;
    size_t w = terminal_width();
    if (term_col + 1 < w) {
        size_t old_row = term_row, old_col = term_col;
        term_col++;
        if (term_mode != TERMINAL_MODE_FRAMEBUFFER) terminal_update_cursor();
        else fb_refresh_cursor(old_row, old_col);
    }
}

void terminal_set_cursor(size_t row, size_t col) {
    g_term_gen++;
    size_t h = terminal_usable_height();
    size_t w = terminal_width();
    size_t old_row = term_row, old_col = term_col;
    if (h == 0) { row = 0; col = 0; }
    else if (row >= h) row = h - 1;
    if (col >= w)  col = w - 1;
    term_row = row;
    term_col = col;
    if (term_mode != TERMINAL_MODE_FRAMEBUFFER) terminal_update_cursor();
    else fb_refresh_cursor(old_row, old_col);
}

void terminal_get_cursor(size_t* row, size_t* col) {
    if (row) *row = term_row;
    if (col) *col = term_col;
}

void terminal_set_reserved_bottom(size_t rows) {
    if (rows >= VGA_HEIGHT) rows = VGA_HEIGHT - 1;
    term_reserved_bottom = rows;
    size_t h = terminal_usable_height();
    if (h == 0) {
        term_row = 0;
        term_col = 0;
    } else {
        if (term_row >= h) term_row = h - 1;
        if (term_col >= VGA_WIDTH) term_col = VGA_WIDTH - 1;
    }
    if (term_mode != TERMINAL_MODE_FRAMEBUFFER) terminal_update_cursor();
}

void terminal_putentryat(char c, uint8_t fg, uint8_t bg, size_t row, size_t col) {
    g_term_gen++;
    if (row >= VGA_HEIGHT || col >= VGA_WIDTH) return;
    if (term_mode == TERMINAL_MODE_FRAMEBUFFER && gfx_available()) {
        uint8_t color = vga_entry_color(fg, bg);
        vt_chars[vt_active][row][col] = c;
        vt_colors[vt_active][row][col] = color;
        if (vt_active == CONSOLE_VT) { mark_dirty(row); terminal_flush(); }
        return;
    }

    uint8_t color = vga_entry_color(fg, bg);
    term_buf[row * VGA_WIDTH + col] = vga_entry(c, color);
}

void terminal_write_at(const char* str, uint8_t fg, uint8_t bg, size_t row, size_t col) {
    if (!str) return;
    size_t x = col;
    for (size_t i = 0; str[i] && x < VGA_WIDTH; i++, x++) {
        terminal_putentryat(str[i], fg, bg, row, x);
    }
}

void terminal_set_mode(terminal_mode_t mode) {
    term_mode = mode;
    if (term_mode == TERMINAL_MODE_FRAMEBUFFER && gfx_available()) {
        fb_redraw_all();   /* back from the desktop: repaint everything */
    }
}

terminal_mode_t terminal_get_mode(void) {
    return term_mode;
}

void terminal_fb_set_scale(int scale) {
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    term_fb_scale = scale;
    fb_recompute_grid();
    if (term_mode == TERMINAL_MODE_FRAMEBUFFER && gfx_available()) {
        fb_redraw_all();
    }
}

int terminal_fb_get_scale(void) {
    return term_fb_scale;
}

void terminal_fb_get_buffer(const char** chars, const uint8_t** colors, int* width, int* height, int* stride) {
    if (chars)  *chars = (const char*)&vt_chars[vt_active][0][0];
    if (colors) *colors = (const uint8_t*)&vt_colors[vt_active][0][0];
    if (width)  *width = (int)terminal_width();
    if (height) *height = (int)terminal_height();
    if (stride) *stride = FB_MAX_COLS;
}

int terminal_vt_alloc(void) {
    g_term_gen++;
    for (int i = 1; i < VT_MAX; i++) {
        if (!vt_used[i]) {
            vt_used[i] = 1;
            vt_row[i] = 0;
            vt_col[i] = 0;
            vt_color[i] = vt_color[vt_active];
            for (size_t y = 0; y < FB_MAX_ROWS; y++) {
                for (size_t x = 0; x < FB_MAX_COLS; x++) {
                    vt_chars[i][y][x] = ' ';
                    vt_colors[i][y][x] = vt_color[i];
                }
            }
            return i;
        }
    }
    return -1;
}

void terminal_vt_free(int vt) {
    if (vt <= 0 || vt >= VT_MAX) return;
    vt_used[vt] = 0;
    if (vt_active == vt) terminal_vt_set_active(0);
}

void terminal_vt_set_active(int vt) {
    if (vt < 0 || vt >= VT_MAX) return;
    if (!vt_used[vt]) return;
    vt_active = vt;
    term_row = vt_row[vt_active];
    term_col = vt_col[vt_active];
    term_color = vt_color[vt_active];
}

int terminal_vt_get_active(void) {
    return vt_active;
}

void terminal_vt_get_buffer(int vt, const char** chars, const uint8_t** colors, int* width, int* height, int* stride) {
    if (vt < 0 || vt >= VT_MAX || !vt_used[vt]) {
        if (chars) *chars = NULL;
        if (colors) *colors = NULL;
        if (width) *width = 0;
        if (height) *height = 0;
        if (stride) *stride = 0;
        return;
    }
    if (chars)  *chars = (const char*)&vt_chars[vt][0][0];
    if (colors) *colors = (const uint8_t*)&vt_colors[vt][0][0];
    if (width)  *width = (int)terminal_width();
    if (height) *height = (int)terminal_height();
    if (stride) *stride = FB_MAX_COLS;
}

void terminal_vt_get_cursor(int vt, size_t* row, size_t* col) {
    if (vt < 0 || vt >= VT_MAX || !vt_used[vt]) {
        if (row) *row = 0;
        if (col) *col = 0;
        return;
    }
    if (row) *row = vt_row[vt];
    if (col) *col = vt_col[vt];
}

int terminal_is_capturing(void) {
    return capturing();
}
