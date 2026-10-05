#ifndef BANANA_API_H
#define BANANA_API_H

/*
 * The Banana OS application interface (ABI), version 1.
 *
 * An app is a position-independent ELF executable (built by the SDK for
 * i686 and x86_64, both packed in one .bpk). Banana OS loads it, applies
 * its relocations and calls its entry point with a pointer to the table
 * below - every service the system offers an app goes through it. The
 * SDK's crt0 keeps that pointer; its libc (printf, fopen, malloc, ...)
 * and banana.h are thin wrappers over these calls.
 *
 * This header is shared by the kernel and the SDK, so it only uses the
 * compiler's built-in types. New calls are only ever added at the end;
 * `size` tells an app how much of the table the running system has.
 */

#define BANANA_API_MAGIC   0x414E4142u     /* "BANA" */
#define BANANA_API_VERSION 1u

/* open() flags */
#define BANANA_O_READ    0x01
#define BANANA_O_WRITE   0x02
#define BANANA_O_CREATE  0x04
#define BANANA_O_TRUNC   0x08
#define BANANA_O_APPEND  0x10

/* seek() whence */
#define BANANA_SEEK_SET 0
#define BANANA_SEEK_CUR 1
#define BANANA_SEEK_END 2

/* window events */
#define BANANA_EV_NONE        0
#define BANANA_EV_KEY         1    /* key: ASCII, or one of BANANA_KEY_* */
#define BANANA_EV_MOUSE_DOWN  2    /* x, y (window client coordinates), button 1 left / 2 right */
#define BANANA_EV_MOUSE_UP    3
#define BANANA_EV_MOUSE_MOVE  4    /* buttons: bit 0 left held */
#define BANANA_EV_CLOSE       5    /* the close button: clean up and exit */
#define BANANA_EV_FOCUS       6    /* x = 1 gained / 0 lost */

#define BANANA_KEY_UP     0x101
#define BANANA_KEY_DOWN   0x102
#define BANANA_KEY_LEFT   0x103
#define BANANA_KEY_RIGHT  0x104
#define BANANA_KEY_HOME   0x105
#define BANANA_KEY_END    0x106
#define BANANA_KEY_PGUP   0x107
#define BANANA_KEY_PGDN   0x108
#define BANANA_KEY_DELETE 0x109

/* draw_text(): a background of BANANA_TRANSPARENT leaves the pixels alone */
#define BANANA_TRANSPARENT 0xFF000000u

typedef struct {
    int type;
    int x, y;
    int button;      /* MOUSE_DOWN / MOUSE_UP: 1 = left, 2 = right */
    int buttons;     /* MOUSE_MOVE: buttons held */
    int key;         /* KEY */
} banana_event_t;

typedef struct {
    unsigned int size;        /* bytes (files) */
    int          is_dir;
} banana_stat_t;

typedef struct {
    char         name[64];
    unsigned int size;
    int          is_dir;
} banana_dirent_t;

typedef struct {
    int year, month, day;     /* 2026, 1..12, 1..31 */
    int hour, minute, second;
    int weekday;              /* 0 = Sunday */
} banana_time_t;

typedef struct banana_api {
    unsigned int magic;       /* BANANA_API_MAGIC */
    unsigned int version;     /* BANANA_API_VERSION */
    unsigned int size;        /* sizeof(banana_api_t) on the running system */
    const char*  arch;        /* "i686" or "x86_64" */
    const char*  os_version;  /* "0.5" */

    /* ── process ─────────────────────────────────────────── */
    void  (*exit)(int code);                            /* never returns */

    /* ── console (the terminal the app was started from) ─── */
    /* fd 1 / 2: the terminal; fd >= 3: an open file */
    long  (*write)(int fd, const void* buf, unsigned long len);
    int   (*getchar)(void);         /* waits for a key; -1 if the app has no terminal */
    int   (*trygetchar)(void);      /* a key, or 0 */
    int   (*readline)(char* buf, int max);   /* a line with echo + backspace, no '\n'; its length */
    void  (*set_color)(int fg, int bg);      /* VGA colors 0..15 */
    void  (*clear_screen)(void);
    void  (*term_size)(int* cols, int* rows);
    void  (*set_cursor)(int row, int col);

    /* ── memory (everything is freed when the app exits) ─── */
    void* (*malloc)(unsigned long size);
    void  (*free)(void* p);
    void* (*realloc)(void* p, unsigned long size);

    /* ── files: paths like the shell's (/home/banana, ~, relative) ── */
    int   (*open)(const char* path, int flags);         /* fd >= 3, or -1 */
    long  (*read)(int fd, void* buf, unsigned long len);
    long  (*seek)(int fd, long off, int whence);         /* new position */
    int   (*close)(int fd);
    int   (*remove)(const char* path);                  /* file or empty folder */
    int   (*mkdir)(const char* path);
    int   (*rename)(const char* from, const char* to);
    int   (*stat)(const char* path, banana_stat_t* st);  /* 0, or -1 if missing */
    int   (*readdir)(const char* path, int index, banana_dirent_t* ent);   /* 0, or -1 past the end */
    int   (*getcwd)(char* buf, int size);
    int   (*chdir)(const char* path);

    /* ── time ─────────────────────────────────────────────── */
    unsigned int (*ticks_ms)(void);                     /* since boot */
    void  (*sleep_ms)(unsigned int ms);
    void  (*yield)(void);
    void  (*localtime)(banana_time_t* t);
    unsigned int (*random)(void);                       /* from the kernel's CSPRNG */

    /* ── windows (on the desktop: `startx`) ───────────────── */
    int   (*win_open)(const char* title, int w, int h); /* window id, or -1 (no desktop / too many) */
    unsigned int* (*win_pixels)(int win);               /* w*h pixels 0x00RRGGBB, row by row */
    void  (*win_update)(int win);                       /* show what was drawn */
    int   (*win_event)(int win, banana_event_t* ev);    /* 1 if *ev got an event, 0 if none */
    void  (*win_close)(int win);
    void  (*win_set_title)(int win, const char* title);
    void  (*win_size)(int win, int* w, int* h);
    /* text with the system's 8x8 font into any pixel buffer */
    void  (*draw_text)(unsigned int* px, int stride, int w, int h, int x, int y,
                       const char* s, unsigned int fg, unsigned int bg);
    const unsigned char* font8x8;                       /* 128 glyphs of 8 rows, bit 0 = leftmost */

    /* ── network ──────────────────────────────────────────── */
    /* GET http:// or https://: *data (free() it) and *len; 0, or -1 with
     * err (if given) set; ctype gets the Content-Type if given */
    int   (*http_get)(const char* url, char** data, unsigned long* len,
                      char* ctype, int ccap, char* err, int ecap);

    /* ── sound ────────────────────────────────────────────── */
    /* queues PCM (8/16-bit, 1/2 channels, any rate - it is resampled);
     * waits while the queue is full; -1 without a sound card */
    int   (*audio_play)(const void* pcm, unsigned long bytes, int rate, int channels, int bits);
    void  (*audio_beep)(int hz, int ms);                /* PC speaker (or the card) */
    int   (*audio_busy)(void);                          /* 1 while queued sound plays */

    /* ── clipboard ────────────────────────────────────────── */
    void  (*clipboard_set)(const char* text, unsigned long len);
    const char* (*clipboard_get)(unsigned long* len);

    /* ── more console ─────────────────────────────────────── */
    int   (*interrupted)(void);     /* 1 once Ctrl+C was pressed in the app's terminal */
} banana_api_t;

/* the app's entry point (the SDK's crt0 provides it) */
typedef int (*banana_entry_t)(const banana_api_t* api, int argc, char** argv);

#endif
