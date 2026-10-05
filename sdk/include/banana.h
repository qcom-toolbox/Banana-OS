#ifndef BANANA_H
#define BANANA_H

/*
 * Banana OS SDK - the system-specific part: windows and drawing, input
 * events, time, sound, the network, the clipboard. The standard C parts
 * (stdio.h, stdlib.h, string.h, ...) are next to this header.
 *
 * Everything is integer-only: the system does not save SSE registers,
 * so apps are built without floating point.
 */

#include "banana_api.h"

/* the system's call table (set by crt0 before main() runs) */
extern const banana_api_t* __banana;
#define banana_api() (__banana)

/* ── colors ───────────────────────────────────────────────────────── */
#define BANANA_RGB(r, g, b) ((unsigned int)(((r) & 255) << 16 | ((g) & 255) << 8 | ((b) & 255)))
#define BANANA_BLACK   0x000000u
#define BANANA_WHITE   0xFFFFFFu
#define BANANA_RED     0xE04040u
#define BANANA_GREEN   0x40C060u
#define BANANA_BLUE    0x3A7BD5u
#define BANANA_YELLOW  0xF4D35Eu
#define BANANA_GRAY    0x808890u
#define BANANA_DARK    0x1D232Cu

/* ── windows ──────────────────────────────────────────────────────── */
typedef struct {
    int           id;          /* -1 if it could not be opened */
    int           w, h;        /* client area in pixels */
    unsigned int* px;          /* w*h pixels, 0x00RRGGBB, row by row */
} bwin_t;

/* opens a window on the desktop; 0, or -1 (the desktop is not running:
 * `startx`, or too many windows) */
int  bwin_open(bwin_t* win, const char* title, int w, int h);
void bwin_close(bwin_t* win);
void bwin_title(bwin_t* win, const char* title);
/* shows what was drawn since the last update */
void bwin_update(bwin_t* win);
/* the next input event: 1 if *ev got one, 0 if none is waiting */
int  bwin_event(bwin_t* win, banana_event_t* ev);
/* waits up to timeout_ms (-1: forever) for an event */
int  bwin_wait_event(bwin_t* win, banana_event_t* ev, int timeout_ms);

/* drawing (clipped to the window) */
void bwin_clear(bwin_t* win, unsigned int color);
void bwin_pixel(bwin_t* win, int x, int y, unsigned int color);
unsigned int bwin_get_pixel(bwin_t* win, int x, int y);
void bwin_fill_rect(bwin_t* win, int x, int y, int w, int h, unsigned int color);
void bwin_rect(bwin_t* win, int x, int y, int w, int h, unsigned int color);       /* outline */
void bwin_line(bwin_t* win, int x0, int y0, int x1, int y1, unsigned int color);
void bwin_circle(bwin_t* win, int cx, int cy, int r, unsigned int color);           /* outline */
void bwin_fill_circle(bwin_t* win, int cx, int cy, int r, unsigned int color);
/* 8x8 text; bg BANANA_TRANSPARENT keeps what is behind; scale >= 1 */
void bwin_text(bwin_t* win, int x, int y, const char* s, unsigned int fg, unsigned int bg);
void bwin_text_scaled(bwin_t* win, int x, int y, int scale, const char* s, unsigned int fg, unsigned int bg);
/* a bevelled button-looking box with a centered label */
void bwin_button(bwin_t* win, int x, int y, int w, int h, const char* label, int pressed);
/* copies a w*h image (0x00RRGGBB) into the window at x, y */
void bwin_blit(bwin_t* win, int x, int y, const unsigned int* img, int w, int h);

/* ── time ─────────────────────────────────────────────────────────── */
unsigned int banana_ticks(void);           /* milliseconds since boot */
void banana_sleep(unsigned int ms);        /* lets everything else run meanwhile */
void banana_yield(void);
void banana_time(banana_time_t* t);        /* local date and time */
unsigned int banana_random(void);          /* strong random numbers */

/* ── terminal ─────────────────────────────────────────────────────── */
enum {
    BANANA_C_BLACK = 0, BANANA_C_BLUE, BANANA_C_GREEN, BANANA_C_CYAN, BANANA_C_RED, BANANA_C_MAGENTA,
    BANANA_C_BROWN, BANANA_C_LIGHT_GREY, BANANA_C_DARK_GREY, BANANA_C_LIGHT_BLUE, BANANA_C_LIGHT_GREEN,
    BANANA_C_LIGHT_CYAN, BANANA_C_LIGHT_RED, BANANA_C_LIGHT_MAGENTA, BANANA_C_YELLOW, BANANA_C_WHITE
};
void banana_color(int fg, int bg);         /* terminal text color */
void banana_clear_screen(void);
void banana_cursor(int row, int col);
void banana_term_size(int* cols, int* rows);
int  banana_key(void);                     /* waits for a key (BANANA_KEY_* for arrows) */
int  banana_try_key(void);                 /* a key or 0 */
/* call it in long loops: 1 once Ctrl+C was pressed (without it, Ctrl+C
 * simply ends the app) */
int  banana_interrupted(void);

/* ── network ──────────────────────────────────────────────────────── */
/* downloads url (http:// or https://) into a malloc'd buffer (free() it);
 * 0, or -1 with err set */
int  banana_http_get(const char* url, char** data, unsigned long* len, char* err, int errcap);

/* ── sound ────────────────────────────────────────────────────────── */
/* PCM samples (8-bit unsigned or 16-bit signed, mono or stereo, any
 * rate); returns once queued. -1 if there is no sound card. */
int  banana_play(const void* pcm, unsigned long bytes, int rate, int channels, int bits);
int  banana_playing(void);                 /* queued sound still playing */
void banana_beep(int hz, int ms);
/* a square-wave tone through the sound card (beep if there is none) */
void banana_tone(int hz, int ms, int volume_percent);

/* ── clipboard ────────────────────────────────────────────────────── */
void banana_copy(const char* text);
const char* banana_paste(void);            /* "" when empty */

#endif
