#ifndef BANANA_H
#define BANANA_H

/*
 * Banana OS SDK - the system-specific part: windows and drawing, input
 * events, time, sound, the network, the clipboard. The standard C parts
 * (stdio.h, stdlib.h, string.h, ...) are next to this header.
 *
 * float and double work (math.h has the usual functions); the system
 * keeps every app's FPU/SSE registers apart.
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
/* lets the user resize (grip in the corner) and maximize (double-click
 * the title) the window, not below min_w x min_h. On BANANA_EV_RESIZE,
 * bwin_event() has already updated win->w, win->h and win->px: redraw
 * everything. 0, or -1 on a Banana OS too old for it. */
int  bwin_resizable(bwin_t* win, int min_w, int min_h);

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


/* ── threads (Banana OS with API version 3) ─────────────────────────
 * banana_thread() runs fn(arg) alongside the rest of the app; the timer
 * shares the processor cores between the threads. A banana_mutex_t
 * (start it at BANANA_MUTEX_INIT) guards data several threads change. */
typedef volatile int banana_mutex_t;
#define BANANA_MUTEX_INIT 0
int  banana_thread(int (*fn)(void* arg), void* arg);   /* thread id (> 0), or -1 */
int  banana_join(int id);                  /* waits for it: what fn returned */
int  banana_thread_id(void);               /* 0 in the main thread */
void banana_thread_exit(int ret);
int  banana_cpus(void);                    /* processor cores in use */
void banana_lock(banana_mutex_t* m);
int  banana_trylock(banana_mutex_t* m);    /* 1 if it got the lock */
void banana_unlock(banana_mutex_t* m);

/* waiting for another thread (API version 4; older systems poll):
 * a condition variable is used with a mutex held -
 *     banana_lock(&m); while (!ready) banana_cond_wait(&c, &m); ... banana_unlock(&m);
 * and a semaphore counts free resources (wait takes one, post gives one) */
typedef struct { volatile int seq; } banana_cond_t;
#define BANANA_COND_INIT { 0 }
void banana_cond_wait(banana_cond_t* c, banana_mutex_t* m);
int  banana_cond_timedwait(banana_cond_t* c, banana_mutex_t* m, int ms);   /* 1 on timeout */
void banana_cond_signal(banana_cond_t* c);      /* wakes one waiter */
void banana_cond_broadcast(banana_cond_t* c);   /* wakes all of them */

typedef struct { volatile int count; } banana_sem_t;
#define BANANA_SEM_INIT(n) { (n) }
void banana_sem_init(banana_sem_t* s, int count);
void banana_sem_wait(banana_sem_t* s);
int  banana_sem_trywait(banana_sem_t* s);       /* 1 if it got one */
int  banana_sem_timedwait(banana_sem_t* s, int ms);   /* 1 on timeout */
void banana_sem_post(banana_sem_t* s);

/* the building block: sleep while *addr == expected (-1: no timeout);
 * 1 on timeout. banana_wake() wakes up to count waiters (0: all). */
int  banana_wait_value(volatile int* addr, int expected, int timeout_ms);
int  banana_wake(volatile int* addr, int count);

#endif
