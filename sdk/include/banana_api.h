#ifndef BANANA_API_H
#define BANANA_API_H

/*
 * The Banana OS application interface (ABI), version 6.
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
#define BANANA_API_VERSION 10u

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
#define BANANA_EV_RESIZE      7    /* (resizable windows) x, y = the new size: fetch
                                      win_pixels() again and redraw */
#define BANANA_EV_WHEEL       8    /* (after win_wheel) y = notches, + = toward the user */

/* key_mods() */
#define BANANA_MOD_SHIFT 1
#define BANANA_MOD_CTRL  2
#define BANANA_MOD_ALT   4

#define BANANA_KEY_UP     0x101
#define BANANA_KEY_DOWN   0x102
#define BANANA_KEY_LEFT   0x103
#define BANANA_KEY_RIGHT  0x104
#define BANANA_KEY_HOME   0x105
#define BANANA_KEY_END    0x106
#define BANANA_KEY_PGUP   0x107
#define BANANA_KEY_PGDN   0x108
#define BANANA_KEY_DELETE 0x109
#define BANANA_KEY_F1     0x111    /* F1..F12: BANANA_KEY_F1 + 0 .. 11 */
#define BANANA_KEY_F12    0x11C
#define BANANA_KEY_PLAY   0x120    /* the media keys (laptops: Fn + F-key) */
#define BANANA_KEY_STOP   0x121
#define BANANA_KEY_NEXT   0x122
#define BANANA_KEY_PREV   0x123

/* web_poll() flags */
#define BANANA_WEB_DIRTY    1    /* a new picture: web_draw() it */
#define BANANA_WEB_LOADING  2    /* a page is loading */
#define BANANA_WEB_TITLE    4    /* the title or the address changed: web_info() */
#define BANANA_WEB_MESSAGE  8    /* the page called banana.postMessage(): web_message() */

/* the system fonts (font_draw / font_width / font_metrics) */
#define BANANA_FONT_SANS       0    /* DejaVu Sans */
#define BANANA_FONT_SANS_BOLD  1
#define BANANA_FONT_MONO       2    /* DejaVu Sans Mono */
#define BANANA_FONT_MONO_BOLD  3

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

/* http_fetch(): any request, any answer (API version 10) */
typedef struct {
    const char*   method;        /* "GET", "POST", ... (NULL: GET) */
    const char*   url;           /* http:// or https:// */
    const char*   headers;       /* extra header lines, each "Name: value\r\n" (NULL: none) */
    const char*   body;          /* NULL: none */
    unsigned long body_len;
    const char*   content_type;  /* of the body (NULL: application/json) */
    int           timeout_ms;    /* per wait for the server (0: 20 s) */
} banana_http_req_t;

typedef struct {
    int           status;        /* 200, 401, ... */
    char*         data;          /* the body, NUL-terminated (free() it) */
    unsigned long len;
    char          content_type[96];
    char          err[128];      /* why it failed (-1) */
} banana_http_resp_t;

typedef struct banana_api {
    unsigned int magic;       /* BANANA_API_MAGIC */
    unsigned int version;     /* BANANA_API_VERSION */
    unsigned int size;        /* sizeof(banana_api_t) on the running system */
    const char*  arch;        /* "i686" or "x86_64" */
    const char*  os_version;  /* "0.6" */

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

    /* ── version 2 ────────────────────────────────────────── */
    /* lets the user resize and maximize the window (not smaller than
     * min_w x min_h); the app gets BANANA_EV_RESIZE and must take the
     * new buffer from win_pixels() - the old one is gone after that event */
    void  (*win_set_resizable)(int win, int min_w, int min_h);

    /* ── version 3: threads ───────────────────────────────── */
    /* fn(arg) runs in a new thread (its own 1 MiB stack) alongside the
     * others; the timer shares the CPUs between them. Returns the thread
     * id (> 0), or -1. All of an app's threads end when it exits. */
    int   (*thread_create)(int (*fn)(void* arg), void* arg);
    int   (*thread_join)(int id);       /* waits for it to end: what fn returned */
    int   (*thread_id)(void);           /* 0 in the main thread */
    void  (*thread_exit)(int ret);      /* ends the calling thread (the main one: the app) */
    int   (*cpu_count)(void);           /* processor cores Banana OS uses */

    /* ── version 4: waiting between threads ───────────────── */
    /* sleeps while *addr == expected (until wake() or timeout_ms; -1:
     * no timeout): 0 when the value changed / it was woken, 1 on timeout */
    int   (*wait)(volatile int* addr, int expected, int timeout_ms);
    /* wakes up to count threads waiting on addr (0: all); how many */
    int   (*wake)(volatile int* addr, int count);

    /* ── version 5: web views (the system browser's engine) ── */
    /* A web view loads and runs a page (HTML, CSS, JavaScript) in the
     * background and keeps a w x h picture of it; the app shows it with
     * web_draw() wherever it likes in its window and passes it the mouse
     * and keys. Pages talk to the app with banana.postMessage(text) and
     * get the app's web_post() as a "message" event (event.data). */
    int   (*web_open)(int w, int h);                    /* view id, or -1 */
    void  (*web_close)(int view);
    int   (*web_load)(int view, const char* url);       /* http(s)://, file://, a path, about:; 0 or -1 */
    int   (*web_load_html)(int view, const char* html, const char* base_url);
    void  (*web_resize)(int view, int w, int h);
    int   (*web_poll)(int view);                        /* BANANA_WEB_* since the last poll */
    /* copies the picture to (x, y) of a buf_w x buf_h buffer (clipped) */
    void  (*web_draw)(int view, unsigned int* px, int stride, int x, int y, int buf_w, int buf_h);
    /* a window event in view coordinates: clicks, keys (typing, arrows/PgUp/PgDn scroll) */
    void  (*web_event)(int view, const banana_event_t* ev);
    void  (*web_scroll)(int view, int dy);
    void  (*web_go)(int view, int delta);               /* -1 back, 1 forward, 0 reload */
    int   (*web_info)(int view, char* title, int tcap, char* url, int ucap);   /* 1 while loading */
    /* runs JavaScript in the page, waits; the value as text (objects as JSON); 0 or -1 */
    int   (*web_eval)(int view, const char* js, char* out, int cap);
    int   (*web_message)(int view, char* out, int cap); /* next banana.postMessage() text: its length, -1 none */
    int   (*web_post)(int view, const char* msg);       /* a "message" event in the page */

    /* ── version 6: fonts (TrueType, anti-aliased, UTF-8) ─── */
    /* text in one of the BANANA_FONT_* faces, size pixels high (per em),
     * into any w x h pixel buffer; (x, y) is the top-left of the line.
     * Returns the x just after the text. */
    int   (*font_draw)(unsigned int* px, int stride, int w, int h, int x, int y,
                       int font, int size, const char* text, unsigned int color);
    int   (*font_width)(int font, int size, const char* text);     /* pixels */
    /* ascent above the baseline, descent below it, and the line height */
    void  (*font_metrics)(int font, int size, int* ascent, int* descent, int* line_h);

    /* ── version 7: the sound queue, for players ──────────── */
    unsigned int (*audio_queued_ms)(void);    /* queued sound not played yet */
    void  (*audio_stop)(void);                /* drops what is queued (pause, seek) */
    int   (*audio_volume)(int percent);       /* sets it (0..100; -1: only reads it); the volume */
    /* this window gets the media keys (BANANA_KEY_PLAY...) even when
     * another one has the focus - for music players */
    void  (*win_media_keys)(int win);

    /* ── version 8: any HTTP request (POST forms, JSON APIs) ── */
    /* method "GET" / "POST" / ...; body (NULL: none) with its Content-Type
     * (NULL: application/x-www-form-urlencoded); the answer like http_get */
    int   (*http_request)(const char* method, const char* url, const char* body, unsigned long body_len,
                          const char* content_type, char** data, unsigned long* len,
                          char* ctype, int ccap, char* err, int ecap);

    /* ── version 9: pictures ──────────────────────────────── */
    /* decodes a picture file (PNG, JPEG, BMP, GIF): *w x *h pixels
     * 0x00RRGGBB, row by row (free() them); NULL with err set */
    unsigned int* (*image_load)(const char* path, int* w, int* h, char* err, int ecap);
    /* makes it the desktop's wallpaper - mode 0 fill, 1 fit, 2 stretch,
     * 3 center - and adds it to Settings' pictures; 0, or -1 with err */
    int   (*set_wallpaper)(const char* path, int mode, char* err, int ecap);

    /* ── version 10: for tools (Banana Code) ──────────────── */
    /* a request with any method and headers; the answer whatever its
     * status (resp->data even for 4xx / 5xx): 0, or -1 if the server could
     * not be reached (resp->err) */
    int   (*http_fetch)(const banana_http_req_t* req, banana_http_resp_t* resp);
    int   (*key_mods)(void);           /* BANANA_MOD_* held now */
    /* the mouse wheel comes as BANANA_EV_WHEEL instead of Up / Down keys */
    void  (*win_wheel)(int win);
    /* installs a .bpk (like `pkg install`): 0, or -1 - msg says what happened */
    int   (*pkg_install)(const char* path, char* msg, int mcap);
    /* starts an installed app (a console app in a terminal window of its own) */
    int   (*app_run)(const char* name, int argc, char** argv, char* err, int ecap);
} banana_api_t;

/* the app's entry point (the SDK's crt0 provides it) */
typedef int (*banana_entry_t)(const banana_api_t* api, int argc, char** argv);

#endif
