/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef BANANA_API_H
#define BANANA_API_H

/*
 * The Banana OS application interface (ABI), version 22.
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
#define BANANA_API_VERSION 34u

/* open() flags */
#define BANANA_O_READ    0x01
#define BANANA_O_WRITE   0x02
#define BANANA_O_CREATE  0x04
#define BANANA_O_TRUNC   0x08
#define BANANA_O_APPEND  0x10
#define BANANA_O_NOCTTY  0x20          /* (version 27) a terminal opened is not made the controlling one */

/* seek() whence */
#define BANANA_SEEK_SET 0
#define BANANA_SEEK_CUR 1
#define BANANA_SEEK_END 2

/* a package (repo_list) */
#define BANANA_PKG_INSTALLED  1    /* installed (installed: its version) */
#define BANANA_PKG_UPGRADABLE 2    /* a newer version is in a repository */
#define BANANA_PKG_AVAILABLE  4    /* in a repository (version: the newest) */
#define BANANA_PKG_AUTO       8    /* installed only because another one needs it */
typedef struct {
    char         name[25];
    char         title[48];
    char         version[24];      /* the repository's */
    char         installed[24];    /* "" if not installed */
    char         type[8];          /* console, gui, driver */
    char         category[24];
    char         description[160];
    char         author[48];
    char         depends[128];
    unsigned int size;             /* download size */
    int          flags;            /* BANANA_PKG_* */
} banana_pkg_t;

/* a package repository (repo_sources, version 15) */
typedef struct {
    char url[200];
    int  has_key;                  /* its package list must be signed with its key */
    int  trusted;                  /* taken without a signature */
} banana_repo_source_t;

/* a game controller (version 12): the Xbox layout whatever the device */
#define BANANA_PAD_A      0x0001u
#define BANANA_PAD_B      0x0002u
#define BANANA_PAD_X      0x0004u
#define BANANA_PAD_Y      0x0008u
#define BANANA_PAD_LB     0x0010u
#define BANANA_PAD_RB     0x0020u
#define BANANA_PAD_BACK   0x0040u
#define BANANA_PAD_START  0x0080u
#define BANANA_PAD_LS     0x0100u    /* the sticks pressed in */
#define BANANA_PAD_RS     0x0200u
#define BANANA_PAD_UP     0x0400u    /* the d-pad */
#define BANANA_PAD_DOWN   0x0800u
#define BANANA_PAD_LEFT   0x1000u
#define BANANA_PAD_RIGHT  0x2000u
#define BANANA_PAD_GUIDE  0x4000u    /* the Xbox / PS / Home button */
#define BANANA_PAD_LT     0x8000u    /* the triggers pressed past half */
#define BANANA_PAD_RT     0x10000u

typedef struct {
    int          connected;
    char         name[40];
    int          lx, ly, rx, ry;     /* sticks: -32767..32767, + = right / down */
    int          lt, rt;             /* triggers: 0..32767 */
    unsigned int buttons;            /* BANANA_PAD_* */
} banana_gamepad_t;

/* window events */
#define BANANA_EV_NONE        0
#define BANANA_EV_KEY         1    /* key: ASCII, or one of BANANA_KEY_* */
#define BANANA_EV_MOUSE_DOWN  2    /* x,y client coordinates; button 1 left / 2 right / 3 middle */
#define BANANA_EV_MOUSE_UP    3
#define BANANA_EV_MOUSE_MOVE  4    /* buttons: bit 0 left, 1 right, 2 middle (capture) */
#define BANANA_EV_CLOSE       5    /* the close button: clean up and exit */
#define BANANA_EV_FOCUS       6    /* x = 1 gained / 0 lost */
#define BANANA_EV_RESIZE      7    /* (resizable windows) x, y = the new size: fetch
                                      win_pixels() again and redraw */
#define BANANA_EV_WHEEL       8    /* (after win_wheel) y = notches, + = toward the user */
#define BANANA_EV_DROP        9    /* (version 13) files dropped on the window: x, y where,
                                      key = how many - win_dropped() gives their paths */
#define BANANA_EV_MOUSE_REL   10   /* v20: captured motion, x/y = dx/dy, right/down positive */
#define BANANA_EV_CAPTURE     11   /* v20: x = 1 captured / 0 released; buttons = held mask */

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
#define BANANA_KEY_SHIFT  0x130    /* key_down() only: either Shift, Ctrl, Alt */
#define BANANA_KEY_CTRL   0x131
#define BANANA_KEY_ALT    0x132

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

/* fs_ustat (version 28): mode as Linux's st_mode (S_IFREG, S_IFDIR,
 * S_IFLNK | permissions) */
typedef struct {
    unsigned int       mode, uid, gid, nlink;
    unsigned long long size, ino, dev;
    long long          atime, mtime, ctime;
} banana_ustat_t;

typedef struct {
    char         name[64];
    unsigned int size;
    int          is_dir;
} banana_dirent_t;

/* proc_list (version 30): a process as ps shows it */
typedef struct {
    int  pid, ppid, pgid, sid;
    int  tty;                    /* its terminal: /dev/pts/N (N), BANANA_TTY_CONSOLE | minor a console, -2 the kernel's, -1 none */
    int  tpgid;                  /* that terminal's foreground group */
    char state;                  /* R running, S in a system call, T stopped, Z a zombie */
    char __pad[3];
    unsigned int threads;
    unsigned long long mem;      /* bytes of memory it holds */
    unsigned long long start_ms; /* when it started (milliseconds since boot) */
    unsigned long long sigpending;
    unsigned long long cpu_ms;   /* processor time used */
    unsigned long long uip;      /* where in its program it last ran (an offset) */
    unsigned int cpu_pct;        /* its share of a processor now */
    char sched;                  /* the scheduler's view: R running, r ready, S sleeping, A on another core */
    char __pad2[3];
    char name[32];
    char wchan[32];              /* the system call it is in */
    char cmdline[160];           /* its arguments */
} banana_procinfo_t;

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

    /* ── version 11: the package manager (apt, the App Store) ── */
    /* fetches the repositories' package lists: how many packages, or -1 (msg) */
    int   (*repo_update)(char* msg, int mcap);
    /* every package: the repositories' (newest for this computer) and the
     * installed ones; how many there are (fills out up to max) */
    int   (*repo_list)(banana_pkg_t* out, int max);
    /* installs a package and what it depends on, or upgrades it: 0 / -1 (msg) */
    int   (*repo_install)(const char* name, char* msg, int mcap);
    int   (*repo_remove)(const char* name, char* msg, int mcap);
    /* what an update / install is doing: percent, or -1 when nothing runs */
    int   (*repo_status)(char* msg, int mcap);
    /* a picture file for the package (downloaded once): 0 with its path, -1 */
    int   (*repo_icon)(const char* name, char* path, int pcap);
    /* upgrades every package that has a newer version: how many, or -1 */
    int   (*repo_upgrade)(char* msg, int mcap);

    /* ── version 12: game controllers ──────────────────────── */
    /* controller `index` (0-3, in the order they were plugged in): 1 and
     * its state now, or 0 if there is none */
    int   (*gamepad)(int index, banana_gamepad_t* out);

    /* ── version 13: drag and drop ─────────────────────────── */
    /* after BANANA_EV_DROP: path `index` (0..key-1) of the last drop on
     * the window; 0, or -1 if there is no such path */
    int   (*win_dropped)(int win, int index, char* path, int cap);

    /* ── version 14: keys held down, several at once (games) ── */
    /* 1 while `key` is held and this app has the keyboard, 0 otherwise:
     * a character by its place on a US keyboard ('w' is the key above
     * 's' on AZERTY too) - letters, digits, ' ', '\n', '\t', '\b', 27 for
     * Esc - or BANANA_KEY_UP..DELETE, BANANA_KEY_F1..F12,
     * BANANA_KEY_SHIFT / CTRL / ALT. Ask for every key a frame needs. */
    int   (*key_down)(int key);

    /* ── version 15: the repositories (apt sources, the App Store's servers) ── */
    /* the sources: how many (fills out up to max) */
    int   (*repo_sources)(banana_repo_source_t* out, int max);
    /* adds one: url https://, http:// or file://; key_hex its Ed25519
     * key (64 hex digits) or NULL / "" with trusted = 1 to take it
     * unsigned. 0, or -1 (msg). Then repo_update() fetches its list. */
    int   (*repo_add_source)(const char* url, const char* key_hex, int trusted, char* msg, int mcap);
    /* removes the one with this URL: 0, or -1 (msg) */
    int   (*repo_remove_source)(const char* url, char* msg, int mcap);

    /* ── version 16: mapped memory (mmap, <sys/mman.h>) ────────── */
    /* 64-bit: len bytes of addresses of the app's own (rounded up to 4 KiB
     * pages) that get memory - zeroed - when first touched, so a big range
     * can be reserved and used little by little. prot: BANANA_PROT_*;
     * flags BANANA_MAP_FIXED: at addr exactly, replacing what was mapped
     * there (otherwise addr is a hint). The address, or NULL. 32-bit:
     * zeroed memory right away, prot not enforced. */
    void* (*vm_map)(void* addr, unsigned long len, int prot, int flags);
    int   (*vm_unmap)(void* addr, unsigned long len);           /* 0, or -1 */
    /* touching a page against its protection is a bad access: 0, or -1
     * if some of the range is not mapped */
    int   (*vm_protect)(void* addr, unsigned long len, int prot);

    /* ── version 17: threads for <pthread.h> ───────────────────── */
    /* like thread_create, on the stack [stack, stack + size) - the app's
     * memory (mapped, with its own guard pages), kept by it - or, with
     * stack NULL, on size bytes of new stack (0: 1 MiB). Id, or -1 */
    int   (*thread_start)(int (*fn)(void* arg), void* arg, void* stack, unsigned long size);
    /* nobody will thread_join it: it is forgotten as soon as it ends; 0, -1 */
    int   (*thread_detach)(int id);
    /* the calling thread's thread pointer - FS on x86_64, what __thread
     * variables are found from; -1 on the 32-bit system (no TLS there) */
    int   (*tls_set)(void* tp);
    /* the memory there is for apps and what is free now, in bytes */
    void  (*mem_info)(unsigned long long* total, unsigned long long* avail);
    /* this app's process id (getpid): no two apps since boot share one */
    int   (*app_id)(void);

    /* ── version 18: BSD sockets (<sys/socket.h>), TCP and UDP over IPv4 ── */
    /* Linux's constants and struct sockaddr_in; a socket is a file
     * descriptor (read, write, close work on it); errors are negative
     * errno numbers. Waiting calls let the app be stopped meanwhile. */
    int   (*sock_open)(int family, int type, int proto);      /* fd */
    int   (*sock_bind)(int fd, const void* sa, int len);
    int   (*sock_listen)(int fd, int backlog);
    int   (*sock_accept)(int fd, void* sa, int* len);          /* a new fd */
    int   (*sock_connect)(int fd, const void* sa, int len);
    /* sa NULL: to the connected peer; bytes sent */
    long  (*sock_send)(int fd, const void* buf, unsigned long len, int flags, const void* sa, int salen);
    /* bytes received (0: the peer closed); the sender in sa if given */
    long  (*sock_recv)(int fd, void* buf, unsigned long len, int flags, void* sa, int* salen);
    int   (*sock_shutdown)(int fd, int how);
    /* get (set 0) or set (1) an option: val holds *len bytes */
    int   (*sock_opt)(int fd, int set, int level, int name, void* val, int* len);
    int   (*sock_name)(int fd, int peer, void* sa, int* len);  /* getsockname / getpeername */
    /* op 1: non-blocking (arg 1 on, 0 off); 2: non-blocking?; 3: bytes to read */
    int   (*sock_ctl)(int fd, int op, int arg);
    /* struct pollfd { int fd; short events, revents; } fds[n]: how many are
     * ready (sockets, files, the terminal), 0 after timeout_ms (-1: none) */
    int   (*poll)(void* fds, int n, int timeout_ms);
    /* a host name's IPv4 address (DNS; dotted ones and localhost too) into
     * ip[4], network order: 0, or -1 */
    int   (*resolve)(const char* name, unsigned char* ip);

    /* ── version 19: signals (<signal.h>) ──────────────────────────── */
    /* Linux's numbers. The SDK gives the system one entry point, entry(sig,
     * info, uc), run on the thread's stack (or its sigaltstack) with a
     * banana_siginfo_t and a banana_ucontext_t: the registers it was
     * stopped with, which the handler may change - returning resumes from
     * them (sigreturn). 64-bit: a CPU fault in app code (bad access,
     * division by zero, invalid instruction...) becomes SIGSEGV / SIGFPE /
     * SIGILL / SIGBUS / SIGTRAP for a handler; other signals come when the
     * thread next leaves a system call or is interrupted. 32-bit: only
     * raise() reaches a handler (the SDK calls it itself). */
    int   (*sig_entry)(void (*entry)(int sig, void* info, void* uc));
    /* what sig does: BANANA_SIG_DEFAULT / IGNORE / CATCH; onstack: on the
     * alternate stack (SA_ONSTACK) */
    int   (*sig_action)(int sig, int disposition, int onstack);
    /* the calling thread's blocked signals (bit sig - 1): how 0 block, 1
     * unblock, 2 set; old gets the previous mask (either may be NULL) */
    int   (*sig_mask)(int how, const unsigned long long* set, unsigned long long* old);
    /* sends sig to one of the app's threads (thread id, as thread_id()), or
     * to the app (-1: whichever thread does not block it): 0, -1 */
    int   (*sig_send)(int thread, int sig);
    /* the calling thread's alternate signal stack: ss {sp, flags, size},
     * old the previous one (either may be NULL); 0, -1 */
    int   (*sig_altstack)(const void* ss, void* old);
    /* the signals pending for the calling thread (or the app), bit sig - 1 */
    unsigned long long (*sig_pending)(void);
    /* v20: only a foreground window may capture. Escape and focus loss,
     * minimize, desktop switch, close or app exit release it. 0 / -1. */
    int   (*win_capture_mouse)(int win,int capture);
    int   (*win_mouse_captured)(int win); /* 1 active, 0 released, -1 invalid */
    /* v21: the time in UTC, seconds since 1970 (0: no clock); and how far
     * the local time is from it - seconds east of UTC - at the instant utc
     * (0: now), with the summer time of the zone set in Settings */
    unsigned int (*time_utc)(void);
    int   (*utc_offset)(unsigned int utc);
    /* v22: optional GPU contexts and owned resources (banana_gpu.h). */
    int (*gpu_call)(unsigned op, const void* request, unsigned request_size, void* data, unsigned size);
    /* v23: the desktop's language - "en", "fr", "de" (Settings > Language) - into out: 0, or -1 */
    int (*language)(char* out, int cap);
    /* v24: an Intel GPU's 3D (gen 5 - 7.5, for Mesa's crocus): one of Linux's
     * DRM / i915 requests - req its ioctl number, arg its structure - what
     * the SDK's ioctl() on a /dev/dri/card0 it opened sends. 0, or -errno */
    int (*drm_ioctl)(unsigned long req, void* arg);

    /* ── version 25: Unix file descriptors ─────────────────────────
     * Descriptors point at open file descriptions, shared by dup (and
     * fork): the position, the status flags. 0, 1 and 2 start as the
     * terminal; any of them may be closed or replaced. Errors: -errno. */
    /* a pipe: fds[0] reads what fds[1] writes; flags Linux's O_NONBLOCK /
     * O_CLOEXEC */
    int (*fd_pipe)(int* fds, int flags);
    /* a connected pair of AF_UNIX stream sockets, flags as fd_pipe */
    int (*fd_socketpair)(int* fds, int flags);
    /* another descriptor for fd's description: exactly to (BANANA_DUP_TO:
     * what was there closes first) or the lowest free one >= to; with
     * BANANA_DUP_CLOEXEC close-on-exec. The new number. */
    int (*fd_dup)(int fd, int to, int flags);
    /* Linux's F_GETFD / F_SETFD (FD_CLOEXEC) and F_GETFL / F_SETFL (O_ACCMODE,
     * O_APPEND, O_NONBLOCK: Linux's values) */
    int (*fd_fcntl)(int fd, int cmd, long arg);
    /* what fd is: out a banana_fdinfo_t */
    int (*fd_info)(int fd, void* out);

    /* ── version 26: Unix processes ──────────────────────────────────
     * A process's pid is getpid()'s. 64-bit only (the 32-bit system: -ENOSYS).
     * fork: a copy of the calling process (its memory copy-on-write, the same
     * descriptors), 0 in the child, the child's pid in the parent.
     * exec: the program at path replaces the calling one (argv, envp: NULL-
     * ended; a #! script runs its interpreter) - it returns only if it
     * cannot (-errno). The new program finds envp after argv's NULL.
     * wait: Linux's wait4 - pid (-1 any child, 0 its group, < -1 the group
     * -pid), status (or NULL), options (WNOHANG 1), rusage (or NULL: zeroed);
     * the pid of a child that ended, 0 (WNOHANG: none yet), -errno.
     * ids: process pid's (0: the caller) pid, ppid, pgid, sid into out[4].
     * setpgid, setsid, kill: as POSIX's; errors -errno. */
    int (*proc_fork)(void);
    int (*proc_exec)(const char* path, char* const* argv, char* const* envp);
    int (*proc_wait)(int pid, int* status, int options, void* rusage);
    int (*proc_ids)(int pid, int* out);
    int (*proc_setpgid)(int pid, int pgid);
    int (*proc_setsid)(void);
    int (*proc_kill)(int pid, int sig);

    /* ── version 27: terminals ───────────────────────────────────────
     * open() knows /dev: null, zero, full, random, urandom, tty, console,
     * ptmx (a new pseudo-terminal's master), pts/N (its slave), stdin,
     * stdout, stderr, fd/N. A session leader opening a terminal without
     * BANANA_O_NOCTTY makes it its controlling terminal. ioctl on a
     * terminal: Linux's request numbers (TCGETS / TCSETS* with the SDK's
     * struct termios, TIOCGWINSZ, TIOCSWINSZ, TIOCSCTTY, TIOCNOTTY,
     * TIOCGPGRP, TIOCSPGRP, TIOCGSID, TIOCGPTN, TIOCSPTLCK, FIONREAD,
     * TIOCOUTQ, TCFLSH, TCSBRK, TCXONC): 0 or -errno (-ENOTTY: not one).
     * Job control: SIGSTOP / SIGTSTP / SIGTTIN / SIGTTOU stop a process,
     * SIGCONT continues it; proc_wait's WUNTRACED 2, WCONTINUED 8. */
    int (*tty_ioctl)(int fd, unsigned long req, void* arg);

    /* ── version 28: Unix files ───────────────────────────────────────
     * What the files' own records say: the mode (type and permissions),
     * owner, times, symbolic links. Errors are -errno. fs_ustat: follow 0
     * is lstat (the link itself); fs_fdstat: an open file's (-EBADF for a
     * descriptor that is not a file); fs_utime: its last change, seconds
     * since 1970 (-1: now); fs_unlink: dir 1 removes an empty folder
     * (rmdir), 0 anything else (unlink: -EISDIR for a folder); fs_fdattr:
     * what 1 chmod(a), 2 chown(a, b; -1 keeps one), 3 the time (a). */
    int (*fs_ustat)(const char* path, int follow, banana_ustat_t* st);
    int (*fs_fdstat)(int fd, banana_ustat_t* st);
    int (*fs_chmod)(const char* path, unsigned int mode);
    int (*fs_chown)(const char* path, int uid, int gid);
    int (*fs_symlink)(const char* target, const char* linkpath);
    int (*fs_readlink)(const char* path, char* buf, int cap);      /* its length */
    int (*fs_utime)(const char* path, int follow, long long mtime);
    int (*fs_unlink)(const char* path, int dir);
    int (*fs_fdattr)(int fd, int what, long long a, long long b);
    /* open() of a folder (read only) gives a descriptor of type
     * BANANA_FD_DIR; fs_fdpath: its absolute path (a file's too) - its length */
    int (*fs_fdpath)(int fd, char* buf, int cap);

    /* ── version 29: hard links, both times ───────────────────────────
     * fs_link: newpath another name of the file oldpath names (a link
     * there is not followed; -EPERM for a folder or on FAT / exFAT,
     * -EXDEV across volumes, -EEXIST); fs_utimes: the last access's and
     * last change's times, seconds since 1970, -1 now, -2 as it is.
     * fs_fdattr also takes what 4: the two times (a, b - the same way);
     * and (build 247) what 5: the file cut to - or grown to - a bytes
     * (ftruncate; -EINVAL from older kernels). */
    int (*fs_link)(const char* oldpath, const char* newpath);
    int (*fs_utimes)(const char* path, int follow, long long atime, long long mtime);

    /* ── version 30: a timer ──────────────────────────────────────────
     * proc_alarm: signal sig to the process value_us microseconds from now
     * (0: none; -1: only old is filled in), then every interval_us (0: once) - alarm(), setitimer(),
     * timer_create(); old (optional): what was left and its interval */
    int (*proc_alarm)(int sig, long long value_us, long long interval_us, long long* old);
    /* proc_list: the processes (as many as fit in max): how many */
    int (*proc_list)(banana_procinfo_t* out, int max);
    /* sig_suspend: the calling thread's mask becomes *mask and it waits
     * until a signal it does not block comes (at once: none can slip in
     * between); -EINTR - the signal's handler runs as it returns */
    int (*sig_suspend)(const unsigned long long* mask);
    /* kcmd: one of the system's own commands (startx, install, pkg...: what
     * the old built-in shell had) run with this program's stdin and stdout
     * - 0, 127 if there is no such command (apps/kcmd: their /bin links) */
    int (*kcmd)(const char* line);
    /* poll_mask: poll with the calling thread's mask *mask while it waits
     * (set at once: pselect, ppoll); the caller sets its mask back after */
    int (*poll_mask)(void* fds, int n, int timeout_ms, const unsigned long long* mask);
    /* version 31: completed file/directory synchronization. 0 or -errno;
     * live RAM root succeeds without disk I/O; installed root flushes;
     * other mounts: ENOSYS; backup freeze: EBUSY. */
    int (*fd_sync)(int fd);
    /* version 32: bounded browser-worker IPC. Only a kernel-created,
     * restricted worker may use its own channel; no caller-supplied IDs. */
    int (*web_ipc)(int op, const void* in, unsigned int len, void* out, unsigned int cap);
    /* version 33: the time since boot in nanoseconds (the shared hardware
     * counter ticks_ms is made from: HPET or ACPI PM timer) into *out; 0.
     * The SDK's clock_gettime and gettimeofday use it: before, they had
     * whole milliseconds only */
    int (*uptime_ns)(unsigned long long* out);
    /* version 34: read / write a file at an offset, its own position left
     * as it is (pread, pwrite): one call - the SDK took six (lseek, its
     * fd_info, the read, two lseeks back). -29 (ESPIPE): not a file */
    long (*pread)(int fd, void* buf, unsigned long len, unsigned long off);
    long (*pwrite)(int fd, const void* buf, unsigned long len, unsigned long off);
} banana_api_t;

#define BANANA_SIG_DEFAULT 0
#define BANANA_SIG_IGNORE  1
#define BANANA_SIG_CATCH   2

/* what a signal handler gets (the SDK's siginfo_t and ucontext_t are these) */
typedef struct {
    int   si_signo, si_errno, si_code, __pad0;
    void* si_addr;                     /* the address a bad access touched */
    int   si_pid, si_uid;
    long  si_value;
    long  __pad1[11];
} banana_siginfo_t;

typedef struct { void* ss_sp; int ss_flags; unsigned long ss_size; } banana_stack_t;

/* the x87 / SSE registers as FXSAVE stores them (Linux's struct _libc_fpstate) */
struct _libc_fpxreg { unsigned short significand[4], exponent, __glibc_reserved1[3]; };
struct _libc_xmmreg { unsigned int element[4]; };
struct _libc_fpstate {
    unsigned short cwd, swd, ftw, fop;
    unsigned long long rip, rdp;
    unsigned int mxcsr, mxcr_mask;
    struct _libc_fpxreg _st[8];
    struct _libc_xmmreg _xmm[16];
    unsigned int __glibc_reserved1[24];
};

/* the registers, Linux's order for x86_64 (REG_R8 = 0 ... REG_RIP = 16) */
typedef struct {
    long long gregs[23];
    struct _libc_fpstate* fpregs;      /* -> fpregs_mem (FXSAVE format) */
    unsigned long long __reserved[8];
} banana_mcontext_t;

typedef struct banana_ucontext {
    unsigned long           uc_flags;
    struct banana_ucontext* uc_link;
    banana_stack_t          uc_stack;
    banana_mcontext_t       uc_mcontext;
    unsigned long long      uc_sigmask[16];    /* (the first word: signals 1-64) */
    unsigned char           fpregs_mem[512] __attribute__((aligned(16)));
} banana_ucontext_t;

/* 1 if the running system's table has the call `field` (newer than the app?) */
#define BANANA_HAS(api, field) \
    ((api)->size >= (unsigned long)((const char*)&(api)->field - (const char*)(api)) + sizeof(void*))

/* 64-bit: vm_map hands out addresses from here on (kernel/uvm.h's
 * UVM_BASE); the memory the kernel allocates for an app (malloc, http_get's
 * data...) is below it - the SDK's heap tells the two apart so */
#define BANANA_VM_BASE 0x0000640000000000ull

/* vm_map / vm_protect (the same values as kernel/uvm.h) */
#define BANANA_PROT_NONE  0
#define BANANA_PROT_READ  1
#define BANANA_PROT_WRITE 2
#define BANANA_PROT_EXEC  4
#define BANANA_MAP_FIXED  1

/* fd_dup flags; fd_info's answer */
#define BANANA_DUP_TO      1
#define BANANA_DUP_CLOEXEC 2
#define BANANA_FD_FILE 1                /* type: kernel/kfile.h's KF_* */
#define BANANA_FD_SOCK 2
#define BANANA_FD_PIPE 3                /* a pipe's end, or a socket pair's (is_sock) */
#define BANANA_FD_TERM 4
#define BANANA_FD_PTM  5                /* a pseudo-terminal's master: size its number (pts/N) */
#define BANANA_FD_PTS  6                /* ... its slave */
/* a pty that is one of the consoles (fd_info's size, procinfo's tty):
 * this | 1..6 /dev/tty1..6, | 64 /dev/ttyS0 (Linux's minors) - else the N of /dev/pts/N */
#define BANANA_TTY_CONSOLE 0x10000
#define BANANA_FD_DEV  7                /* /dev/null, zero, full, random: size which (1-4) */
#define BANANA_FD_DIR  8                /* a folder (version 28) */
typedef struct {
    int type, flags;                    /* flags: as F_GETFL */
    int is_sock, cloexec;
    unsigned long long size;            /* a file's */
    unsigned long long id;              /* the description (same id: the same one) */
} banana_fdinfo_t;

/* the app's entry point (the SDK's crt0 provides it) */
typedef int (*banana_entry_t)(const banana_api_t* api, int argc, char** argv);

#endif
