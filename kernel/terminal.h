#ifndef TERMINAL_H
#define TERMINAL_H

#include "types.h"

/* Virtual terminal count: vt0 is the boot/console shell, plus one per GUI
 * terminal window (TERM_WIN_MAX in kernel/gui.c). Shared with modules
 * (e.g. shell/editor.c) that need one instance of some per-window state
 * per vt, so every window can run independently. */
#define TERMINAL_VT_MAX 7   /* console + 4 GUI windows + 2 SSH sessions */

/* VGA colors */
enum vga_color {
    VGA_COLOR_BLACK         = 0,
    VGA_COLOR_BLUE          = 1,
    VGA_COLOR_GREEN         = 2,
    VGA_COLOR_CYAN          = 3,
    VGA_COLOR_RED           = 4,
    VGA_COLOR_MAGENTA       = 5,
    VGA_COLOR_BROWN         = 6,
    VGA_COLOR_LIGHT_GREY    = 7,
    VGA_COLOR_DARK_GREY     = 8,
    VGA_COLOR_LIGHT_BLUE    = 9,
    VGA_COLOR_LIGHT_GREEN   = 10,
    VGA_COLOR_LIGHT_CYAN    = 11,
    VGA_COLOR_LIGHT_RED     = 12,
    VGA_COLOR_LIGHT_MAGENTA = 13,
    VGA_COLOR_YELLOW        = 14,
    VGA_COLOR_WHITE         = 15,
};

void terminal_init(void);
void terminal_clear(void);
void terminal_setcolor(uint8_t fg, uint8_t bg);
void terminal_putchar(char c);
void terminal_write(const char* str);
void terminal_writeln(const char* str);
void terminal_write_color(const char* str, uint8_t fg, uint8_t bg);
void terminal_cursor_left(void);
void terminal_cursor_right(void);
void terminal_set_cursor(size_t row, size_t col);

typedef enum {
    TERMINAL_MODE_VGA_TEXT = 0,
    TERMINAL_MODE_FRAMEBUFFER = 1,
    TERMINAL_MODE_SUSPENDED = 2,
} terminal_mode_t;

void terminal_set_mode(terminal_mode_t mode);
terminal_mode_t terminal_get_mode(void);

/* Framebuffer-console scaling (bigger scale = fewer columns/rows on screen) */
void terminal_fb_set_scale(int scale);
int  terminal_fb_get_scale(void);

/* Access framebuffer backing store (80x25) for GUI windows */
void terminal_fb_get_buffer(const char** chars, const uint8_t** colors, int* width, int* height, int* stride);

/* Virtual terminals (for multiple GUI terminal windows) */
int  terminal_vt_alloc(void);          /* returns vt id or -1 */
void terminal_vt_free(int vt);
void terminal_vt_set_active(int vt);   /* selects vt for subsequent output */
int  terminal_vt_get_active(void);
void terminal_vt_get_buffer(int vt, const char** chars, const uint8_t** colors, int* width, int* height, int* stride);
void terminal_vt_get_cursor(int vt, size_t* row, size_t* col);

/* Text grid of the active terminal (columns / usable rows). */
size_t terminal_get_width(void);
size_t terminal_get_height(void);

/* vt0 output is mirrored to the serial console; the shell's line editor
 * turns that off while it repaints and sends its own ANSI redraw. */
void terminal_serial_mirror(int on);

/* changes whenever any terminal content or cursor changes */
uint32_t terminal_generation(void);

/* The framebuffer console paints lazily (see terminal.c): this brings
 * the screen up to date now. Cheap when nothing changed; called from
 * idle/wait loops so output never lingers unpainted. */
void terminal_flush(void);

/* Redirection (`cmd > file`): the calling task's output is collected
 * instead of shown. start: 0 if a capture is already running; stop: the
 * heap buffer (caller kfree()s it) and its length. */
int   terminal_capture_start(void);
char* terminal_capture_stop(uint32_t* len);
int   terminal_is_capturing(void);        /* the calling task's output is redirected */

/* GUI helpers */
void terminal_get_cursor(size_t* row, size_t* col);
void terminal_set_reserved_bottom(size_t rows);
void terminal_putentryat(char c, uint8_t fg, uint8_t bg, size_t row, size_t col);
void terminal_write_at(const char* str, uint8_t fg, uint8_t bg, size_t row, size_t col);

#endif
