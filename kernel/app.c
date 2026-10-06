#include "app.h"
#include "appwin.h"
#include "audio.h"
#include "fs.h"
#include "kheap.h"
#include "kstring.h"
#include "task.h"
#include "timer.h"
#include "terminal.h"
#include "keyboard.h"
#include "gui.h"
#include "rtc.h"
#include "random.h"
#include "clipboard.h"
#include "serial.h"
#include "font8x8.h"
#include "paging.h"
#include "../net/http.h"
#include "../sdk/include/banana_api.h"
#include "smp.h"

#define APP_MAX     8
#define APP_FD_MAX  16
#define APP_STACK   (1u << 20)        /* 1 MiB */
#define APP_GUARD   (16u << 10)       /* unmapped pages under it (64-bit): an overflow faults */
#define STACK_CANARY 0xB4A4A5C0u      /* at the bottom of the stack: checked on every system call */
#define APP_KEYQ    32
#define APP_MAX_IMAGE (16u << 20)
#define APP_THREADS 16                /* extra threads per app (api->thread_create) */

/* ── per-app state ─────────────────────────────────────────────────── */

/* every allocation carries this header (a multiple of 16 bytes, so the
 * app's pointer keeps the heap's 16-byte alignment) */
typedef struct ablock {
    struct ablock* next;
    struct ablock* prev;
    uintptr_t      size;
    uintptr_t      magic;
} ablock_t;
#define ABLOCK_MAGIC ((uintptr_t)0xA110CA7Eu)

typedef struct {
    int      used;
    int      fidx;
    uint32_t pos;
    int      flags;
} afd_t;

/* a thread an app started: a task of its own running app code on its own stack */
typedef struct {
    int       used;
    int       pid;             /* its task, -1 once it ended */
    int     (*fn)(void*);
    void*     arg;
    uint8_t*  stack_mem;
    uintptr_t saved_sp;        /* app_enter(): where app_leave() returns to */
    int       running;         /* in app code (between app_enter() and its return) */
    int       done;
    int       ret;
} athread_t;

typedef struct {
    int       used;
    int       id;              /* slot index: the window owner id */
    int       pid;             /* the task it runs in */
    int       own_task;        /* started with app_spawn() */
    char      name[32];
    uint8_t*  image;           /* the allocation */
    uintptr_t base;            /* where the ELF was loaded (crash reports: app+offset) */
    banana_entry_t entry;
    uint8_t*  stack_mem;       /* the allocation: guard pages, then the stack */
    uint8_t*  guard;           /* APP_GUARD bytes, page-aligned */
    uint8_t*  stack;           /* APP_STACK bytes above the guard */
    int       guarded;
    ablock_t* blocks;
    afd_t     fds[APP_FD_MAX];
    uintptr_t saved_sp;        /* the kernel stack app_enter() left: exit() returns there */
    int       exit_code;
    int       vt;              /* the terminal it was started from */
    int       has_term;
    int       keyq[APP_KEYQ];
    int       kq_head, kq_tail;
    int       esc;             /* ESC [ parsing in getchar() */
    int       handles_int;     /* called interrupted(): Ctrl+C no longer stops it */
    int       interrupted;
    uint32_t  last_breath;
    int       kill_req;        /* Task Manager: End task */
    int       running;         /* between app_enter() and its return */
    char      crash_msg[192];  /* why it crashed (app_fault), printed by run() */
    int       argc;
    char**    argv;
    uintptr_t code_lo, code_hi; /* where its code is: the timer may switch away from there */
    athread_t threads[APP_THREADS];
    int       exit_req;        /* a thread called exit() / crashed: every thread stops */
    int       exit_req_code;
} app_proc_t;

static app_proc_t g_procs[APP_MAX];
static banana_api_t g_api;


static app_proc_t* cur(void) {
    int pid = task_current_pid();
    for (int i = 0; i < APP_MAX; i++) {
        app_proc_t* p = &g_procs[i];
        if (!p->used) continue;
        if (p->pid == pid) return p;
        for (int t = 0; t < APP_THREADS; t++)
            if (p->threads[t].used && p->threads[t].pid == pid) return p;
    }
    return NULL;
}

/* the calling thread of app p, NULL for its main thread */
static athread_t* cur_thread(app_proc_t* p) {
    int pid = task_current_pid();
    if (!p || p->pid == pid) return NULL;
    for (int t = 0; t < APP_THREADS; t++)
        if (p->threads[t].used && p->threads[t].pid == pid) return &p->threads[t];
    return NULL;
}

/* ends the calling app right away (exit(), Ctrl+C, closed twice) */
int  app_enter(int (*fn)(void*), void* arg, void* stack_top, uintptr_t* saved_sp);
void app_leave(uintptr_t saved_sp, int code) __attribute__((noreturn));

static void __attribute__((noreturn)) leave(app_proc_t* p, int code) {
    athread_t* t = cur_thread(p);
    if (t) {
        /* exit(), Ctrl+C, End task or a crash in a thread: the whole app
         * ends - its main thread and the other threads notice exit_req */
        if (!p->exit_req) { p->exit_req = 1; p->exit_req_code = code; }
        t->running = 0;
        app_leave(t->saved_sp, code);
    }
    p->exit_code = code;
    p->running = 0;
    app_leave(p->saved_sp, code);
}

/* a thread's return value (thread_join), or the end of the whole app */
static void __attribute__((noreturn)) leave_thread(app_proc_t* p, athread_t* t, int ret) {
    (void)p;
    t->running = 0;
    app_leave(t->saved_sp, ret);
}

static void key_push(app_proc_t* p, int c) {
    int next = (p->kq_head + 1) % APP_KEYQ;
    if (next == p->kq_tail) return;
    p->keyq[p->kq_head] = c;
    p->kq_head = next;
}

static int key_pop(app_proc_t* p) {
    if (p->kq_head == p->kq_tail) return 0;
    int c = p->keyq[p->kq_tail];
    p->kq_tail = (p->kq_tail + 1) % APP_KEYQ;
    return c;
}

static int focused(app_proc_t* p) {
    return p->has_term && gui_focused_vt() == p->vt;
}

/* Called from the API calls an app makes: lets the rest of the system
 * run now and then (tasks are cooperative), paints the terminal, and
 * notices Ctrl+C and windows closed for good. */
static void breathe(app_proc_t* p, int force) {
    if (!p) return;
    uint32_t now = timer_ms();
    if (p->exit_req) leave(p, p->exit_req_code);
    /* the 32-bit kernel has no guard pages: a smashed canary stops the app */
    if (!cur_thread(p) && *(volatile uint32_t*)p->stack != STACK_CANARY) {
        ksnprintf(p->crash_msg, sizeof(p->crash_msg), "%s: stack overflow - it used more than %u KiB of stack "
                  "(big local arrays? malloc them)", p->name, APP_STACK >> 10);
        *(volatile uint32_t*)p->stack = STACK_CANARY;
        leave(p, 139);
    }
    if (!force && now - p->last_breath < 20) return;
    p->last_breath = now;
    if (appwin_kill_requested(p->id) || p->kill_req) leave(p, 137);
    if (focused(p)) {
        char c = keyboard_try_getchar();
        if (c == 3) {
            p->interrupted = 1;
            if (!p->handles_int) { terminal_writeln("^C"); leave(p, 130); }
        } else if (c) {
            key_push(p, (unsigned char)c);
        }
    }
    gui_poll();                         /* paints, and yields to the other tasks */
    terminal_vt_set_active(p->vt);
}

/* ── memory ────────────────────────────────────────────────────────── */

static void* a_malloc(unsigned long size) {
    app_proc_t* p = cur();
    if (!p || size > (256ul << 20)) return NULL;
    ablock_t* b = (ablock_t*)kmalloc(sizeof(ablock_t) + size);
    if (!b) return NULL;
    b->size = size;
    b->magic = ABLOCK_MAGIC;
    b->prev = NULL;
    b->next = p->blocks;
    if (p->blocks) p->blocks->prev = b;
    p->blocks = b;
    return b + 1;
}

static void a_free(void* ptr) {
    app_proc_t* p = cur();
    if (!p || !ptr) return;
    ablock_t* b = (ablock_t*)ptr - 1;
    if (b->magic != ABLOCK_MAGIC) { klog("app %s: free() of a bad pointer\n", p->name); return; }
    b->magic = 0;
    if (b->prev) b->prev->next = b->next; else p->blocks = b->next;
    if (b->next) b->next->prev = b->prev;
    kfree(b);
}

static void* a_realloc(void* ptr, unsigned long size) {
    if (!ptr) return a_malloc(size);
    if (!size) { a_free(ptr); return NULL; }
    ablock_t* b = (ablock_t*)ptr - 1;
    if (b->magic != ABLOCK_MAGIC) return NULL;
    void* n = a_malloc(size);
    if (!n) return NULL;
    memcpy(n, ptr, b->size < size ? b->size : size);
    a_free(ptr);
    return n;
}

/* ── console ───────────────────────────────────────────────────────── */

static long file_write(app_proc_t* p, int fd, const void* buf, unsigned long len);

static long a_write(int fd, const void* buf, unsigned long len) {
    app_proc_t* p = cur();
    if (!p) return -1;
    if (fd == 1 || fd == 2) {
        const char* s = (const char*)buf;
        char chunk[257];
        unsigned long i = 0;
        while (i < len) {
            int n = 0;
            while (i < len && n < 256) {
                char c = s[i++];
                if (c) chunk[n++] = c;
            }
            chunk[n] = 0;
            terminal_write(chunk);
        }
        breathe(p, 0);
        return (long)len;
    }
    return file_write(p, fd, buf, len);
}

/* the next key for the app (ESC sequences become BANANA_KEY_*), 0 if none */
static int next_key(app_proc_t* p) {
    for (;;) {
        int c = key_pop(p);
        if (!c && focused(p)) c = (unsigned char)keyboard_try_getchar();
        if (!c) return 0;
        if (c == 3) {
            p->interrupted = 1;
            if (!p->handles_int) { terminal_writeln("^C"); leave(p, 130); }
            return 3;
        }
        if (p->esc == 0 && c == 27) {
            /* an arrow key sends ESC [ X right away; a lone ESC is ESC */
            uint32_t t0 = timer_ms();
            int c2 = 0;
            while (!c2 && timer_ms() - t0 < 30) { c2 = key_pop(p); if (!c2) c2 = (unsigned char)keyboard_try_getchar(); }
            if (c2 != '[') { if (c2) key_push(p, c2); return 27; }
            int c3 = 0;
            t0 = timer_ms();
            while (!c3 && timer_ms() - t0 < 30) { c3 = key_pop(p); if (!c3) c3 = (unsigned char)keyboard_try_getchar(); }
            switch (c3) {
            case 'A': return BANANA_KEY_UP;
            case 'B': return BANANA_KEY_DOWN;
            case 'C': return BANANA_KEY_RIGHT;
            case 'D': return BANANA_KEY_LEFT;
            case 'H': return BANANA_KEY_HOME;
            case 'F': return BANANA_KEY_END;
            case 'P': return BANANA_KEY_DELETE;
            case '5': keyboard_try_getchar(); return BANANA_KEY_PGUP;
            case '6': keyboard_try_getchar(); return BANANA_KEY_PGDN;
            case '3': keyboard_try_getchar(); return BANANA_KEY_DELETE;
            default: continue;
            }
        }
        return c;
    }
}

static int a_getchar(void) {
    app_proc_t* p = cur();
    if (!p || !p->has_term) return -1;
    for (;;) {
        int c = next_key(p);
        if (c) return c;
        breathe(p, 1);
        if (appwin_kill_requested(p->id)) leave(p, 137);
        task_sleep_ms(10);
        terminal_vt_set_active(p->vt);
    }
}

static int a_trygetchar(void) {
    app_proc_t* p = cur();
    if (!p || !p->has_term) return 0;
    breathe(p, 0);
    return next_key(p);
}

static int a_readline(char* buf, int max) {
    app_proc_t* p = cur();
    if (!p || max <= 0) return -1;
    int n = 0;
    buf[0] = 0;
    for (;;) {
        int c = a_getchar();
        if (c < 0) return -1;
        if (c == '\n' || c == '\r') { terminal_putchar('\n'); break; }
        if (c == '\b' || c == 127) {
            if (n > 0) { n--; terminal_cursor_left(); terminal_putchar(' '); terminal_cursor_left(); }
            continue;
        }
        if (c < 32 || c > 126) continue;
        if (n < max - 1) { buf[n++] = (char)c; terminal_putchar((char)c); }
    }
    buf[n] = 0;
    return n;
}

static void a_set_color(int fg, int bg) { terminal_setcolor((uint8_t)(fg & 15), (uint8_t)(bg & 15)); }
static void a_clear(void) { terminal_clear(); }
static void a_term_size(int* c, int* r) {
    if (c) *c = (int)terminal_get_width();
    if (r) *r = (int)terminal_get_height();
}
static void a_set_cursor(int r, int c) {
    if (r < 0) r = 0;
    if (c < 0) c = 0;
    terminal_set_cursor((size_t)r, (size_t)c);
}

static int a_interrupted(void) {
    app_proc_t* p = cur();
    if (!p) return 0;
    p->handles_int = 1;
    breathe(p, 0);
    int r = p->interrupted;
    p->interrupted = 0;
    return r;
}

/* ── files ─────────────────────────────────────────────────────────── */

static afd_t* get_fd(app_proc_t* p, int fd) {
    if (!p || fd < 3 || fd >= APP_FD_MAX + 3) return NULL;
    afd_t* f = &p->fds[fd - 3];
    if (!f->used) return NULL;
    fs_file_t* ff = fs_file_info(f->fidx);
    if (!ff || !ff->used) return NULL;         /* deleted meanwhile */
    return f;
}

static int a_open(const char* path, int flags) {
    app_proc_t* p = cur();
    if (!p || !path || !*path) return -1;
    if (fs_find_dir(path) >= 0) return -1;
    int idx = fs_find_file(path);
    if (idx < 0) {
        if (!(flags & BANANA_O_CREATE)) return -1;
        idx = fs_create(path);
        if (idx < 0) return -1;
    } else if ((flags & BANANA_O_TRUNC) && (flags & BANANA_O_WRITE)) {
        if (fs_write(idx, "", 0) != 0) return -1;
    }
    for (int i = 0; i < APP_FD_MAX; i++) {
        if (p->fds[i].used) continue;
        p->fds[i].used = 1;
        p->fds[i].fidx = idx;
        p->fds[i].flags = flags ? flags : BANANA_O_READ;
        p->fds[i].pos = (flags & BANANA_O_APPEND) ? fs_file_info(idx)->size : 0;
        return i + 3;
    }
    return -1;
}

static long a_read(int fd, void* buf, unsigned long len) {
    app_proc_t* p = cur();
    if (fd == 0) {                          /* stdin: a line from the terminal */
        if (!p || !len) return 0;
        char* b = (char*)buf;
        int n = a_readline(b, (int)(len > 1024 ? 1024 : len));
        if (n < 0) return 0;
        if ((unsigned long)n < len) b[n++] = '\n';
        return n;
    }
    afd_t* f = get_fd(p, fd);
    if (!f) return -1;
    fs_file_t* ff = fs_get_file(f->fidx);
    if (!ff) return -1;
    if (f->pos >= ff->size) return 0;
    uint32_t n = ff->size - f->pos;
    if (n > len) n = (uint32_t)len;
    memcpy(buf, ff->content + f->pos, n);
    f->pos += n;
    breathe(p, 0);
    return (long)n;
}

static long file_write(app_proc_t* p, int fd, const void* buf, unsigned long len) {
    afd_t* f = get_fd(p, fd);
    if (!f || !(f->flags & (BANANA_O_WRITE | BANANA_O_APPEND))) return -1;
    fs_file_t* ff = fs_get_file(f->fidx);
    if (!ff) return -1;
    if (f->flags & BANANA_O_APPEND) f->pos = ff->size;
    if (f->pos == ff->size) {
        if (fs_append(f->fidx, buf, (uint32_t)len) != 0) return -1;
    } else {
        /* writing inside (or past) the data: build the new contents */
        uint32_t end = f->pos + (uint32_t)len;
        uint32_t size = end > ff->size ? end : ff->size;
        char* nb = (char*)kmalloc(size + 1);
        if (!nb) return -1;
        memset(nb, 0, size);
        memcpy(nb, ff->content, ff->size);
        memcpy(nb + f->pos, buf, len);
        int rc = fs_write(f->fidx, nb, size);
        kfree(nb);
        if (rc != 0) return -1;
    }
    f->pos += (uint32_t)len;
    breathe(p, 0);
    return (long)len;
}

static long a_seek(int fd, long off, int whence) {
    afd_t* f = get_fd(cur(), fd);
    if (!f) return -1;
    long base = whence == BANANA_SEEK_CUR ? (long)f->pos : whence == BANANA_SEEK_END ? (long)fs_file_info(f->fidx)->size : 0;
    long np = base + off;
    if (np < 0) return -1;
    f->pos = (uint32_t)np;
    return np;
}

static int a_close(int fd) {
    afd_t* f = get_fd(cur(), fd);
    app_proc_t* p = cur();
    if (!f) {
        if (p && fd >= 3 && fd < APP_FD_MAX + 3) p->fds[fd - 3].used = 0;
        return -1;
    }
    f->used = 0;
    return 0;
}

static int a_remove(const char* path) {
    if (!path || !*path) return -1;
    if (fs_find_file(path) >= 0) { fs_delete(path, 0); return fs_find_file(path) >= 0 ? -1 : 0; }
    if (fs_find_dir(path) < 0) return -1;
    int d[1], f[1];
    if (fs_list_dirs(path, d, 1) > 0 || fs_list_files(path, f, 1) > 0) return -1;   /* not empty */
    fs_delete(path, 1);
    return fs_find_dir(path) >= 0 ? -1 : 0;
}

static int a_mkdir(const char* path) { return path && fs_mkdir(path) >= 0 ? 0 : -1; }
static int a_rename(const char* a, const char* b) { return a && b && fs_move(a, b) >= 0 ? 0 : -1; }

static int a_stat(const char* path, banana_stat_t* st) {
    if (!path || !st) return -1;
    int f = fs_find_file(path);
    if (f >= 0) { st->size = fs_file_info(f)->size; st->is_dir = 0; return 0; }
    if (fs_find_dir(path) >= 0) { st->size = 0; st->is_dir = 1; return 0; }
    return -1;
}

static int a_readdir(const char* path, int index, banana_dirent_t* ent) {
    static int idx[FS_MAX_FILES];
    if (!path || !ent || index < 0) return -1;
    int nd = fs_list_dirs(path, idx, FS_MAX_DIRS);
    if (nd < 0) return -1;
    if (index < nd && index < FS_MAX_DIRS) {
        const fs_dir_t* d = fs_get_dir(idx[index]);
        if (!d) return -1;
        kstrlcpy(ent->name, d->name, sizeof(ent->name));
        ent->size = 0;
        ent->is_dir = 1;
        return 0;
    }
    index -= nd;
    int nf = fs_list_files(path, idx, FS_MAX_FILES);
    if (index >= nf || index >= FS_MAX_FILES) return -1;
    fs_file_t* f = fs_file_info(idx[index]);
    kstrlcpy(ent->name, f->name, sizeof(ent->name));
    ent->size = f->size;
    ent->is_dir = 0;
    return 0;
}

static int a_getcwd(char* buf, int size) {
    if (!buf || size <= 0) return -1;
    fs_cwd_path(buf, size);
    return 0;
}

static int a_chdir(const char* path) {
    if (!path || fs_find_dir(path) < 0) return -1;
    fs_cd(path);
    return 0;
}

/* ── time ──────────────────────────────────────────────────────────── */

static unsigned int a_ticks(void) { return timer_ms(); }

static void a_sleep(unsigned int ms) {
    app_proc_t* p = cur();
    uint32_t end = timer_ms() + ms;
    do {
        breathe(p, 1);
        int32_t left = (int32_t)(end - timer_ms());
        if (left <= 0) break;
        task_sleep_ms(left > 10 ? 10 : (uint32_t)left);
        if (p) terminal_vt_set_active(p->vt);
    } while (1);
}

static void a_yield(void) { breathe(cur(), 1); }

static void a_localtime(banana_time_t* t) {
    rtc_datetime_t dt;
    memset(t, 0, sizeof(*t));
    if (rtc_read_datetime(&dt) != 0) { t->year = 2026; t->month = 1; t->day = 1; return; }
    t->year = dt.year; t->month = dt.month; t->day = dt.day;
    t->hour = dt.hour; t->minute = dt.minute; t->second = dt.second;
    int y = dt.year, m = dt.month;
    if (m < 3) { m += 12; y--; }
    int k = y % 100, j = y / 100;
    int h = (dt.day + 13 * (m + 1) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;   /* 0 = Saturday */
    t->weekday = (h + 6) % 7;
}

static unsigned int a_random(void) { return random_u32(); }

/* ── windows ───────────────────────────────────────────────────────── */

static int a_win_open(const char* title, int w, int h) {
    app_proc_t* p = cur();
    if (!p || !gui_is_enabled()) return -1;
    return appwin_open(p->id, title, w, h);
}
static unsigned int* a_win_pixels(int win) { app_proc_t* p = cur(); return p ? appwin_pixels(win, p->id) : NULL; }
static void a_win_update(int win) {
    app_proc_t* p = cur();
    if (!p) return;
    appwin_update(win, p->id);
    breathe(p, 0);
}
static int a_win_event(int win, banana_event_t* ev) {
    app_proc_t* p = cur();
    if (!p || !ev) return 0;
    breathe(p, 0);
    return appwin_event(win, p->id, ev);
}
static void a_win_close(int win) { app_proc_t* p = cur(); if (p) appwin_close(win, p->id); }
static void a_win_set_title(int win, const char* t) { app_proc_t* p = cur(); if (p) appwin_set_title(win, p->id, t); }
static void a_win_size(int win, int* w, int* h) { app_proc_t* p = cur(); appwin_size(win, p ? p->id : -1, w, h); }
static void a_win_set_resizable(int win, int mw, int mh) { app_proc_t* p = cur(); if (p) appwin_set_resizable(win, p->id, mw, mh); }

static void a_draw_text(unsigned int* px, int stride, int w, int h, int x, int y,
                        const char* s, unsigned int fg, unsigned int bg) {
    if (!px || !s) return;
    for (; *s; s++, x += 8) {
        uint8_t c = (uint8_t)*s;
        if (c >= 128) c = '?';
        const uint8_t* g = font8x8_basic[c];
        for (int gy = 0; gy < 8; gy++) {
            int yy = y + gy;
            if (yy < 0 || yy >= h) continue;
            for (int gx = 0; gx < 8; gx++) {
                int xx = x + gx;
                if (xx < 0 || xx >= w) continue;
                if (g[gy] & (1u << gx)) px[yy * stride + xx] = fg;
                else if (bg != BANANA_TRANSPARENT) px[yy * stride + xx] = bg;
            }
        }
    }
}

/* ── network ───────────────────────────────────────────────────────── */

typedef struct { char* buf; uint32_t n, cap; int oom; } hbody_t;

static int h_body(void* ctx, const uint8_t* d, uint32_t n) {
    hbody_t* b = (hbody_t*)ctx;
    if (b->n + n + 1 > (16u << 20)) { b->oom = 1; return -1; }
    if (b->n + n + 1 > b->cap) {
        uint32_t cap = (b->n + n + 1) * 2;
        char* nb = (char*)a_realloc(b->buf, cap);
        if (!nb) { b->oom = 1; return -1; }
        b->buf = nb;
        b->cap = cap;
    }
    memcpy(b->buf + b->n, d, n);
    b->n += n;
    b->buf[b->n] = 0;
    return 0;
}

static int a_http_get(const char* url, char** data, unsigned long* len, char* ctype, int ccap, char* err, int ecap) {
    char e[128];
    hbody_t b = { 0, 0, 0, 0 };
    http_request_t req;
    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.follow_redirects = 1;
    req.max_redirects = 10;
    req.timeout_ms = 15000;
    req.user_agent = "BananaOS-App/1.0";
    req.ctx = &b;
    req.on_body = h_body;
    http_response_t* resp = (http_response_t*)kmalloc(sizeof(http_response_t));
    if (!resp) return -1;
    int rc = http_fetch(url, &req, resp, e, sizeof(e));
    if (rc != NET_OK || b.oom) {
        if (err && ecap > 0) kstrlcpy(err, b.oom ? "too big (16 MiB max)" : e, (size_t)ecap);
        if (b.buf) a_free(b.buf);
        kfree(resp);
        return -1;
    }
    if (ctype && ccap > 0) kstrlcpy(ctype, resp->content_type, (size_t)ccap);
    if (resp->status >= 400) {
        if (err && ecap > 0) ksnprintf(err, (size_t)ecap, "HTTP %d %s", resp->status, resp->reason);
        kfree(resp);
        if (b.buf) a_free(b.buf);
        return -1;
    }
    kfree(resp);
    if (!b.buf) { b.buf = (char*)a_malloc(1); if (!b.buf) return -1; b.buf[0] = 0; }
    *data = b.buf;
    if (len) *len = b.n;
    return 0;
}

/* ── sound / clipboard ─────────────────────────────────────────────── */

static int a_audio_play(const void* pcm, unsigned long bytes, int rate, int ch, int bits) {
    return audio_play(pcm, (uint32_t)bytes, rate, ch, bits);
}
static void a_audio_beep(int hz, int ms) { audio_beep(hz, ms); }
static int a_audio_busy(void) { return audio_busy(); }
static void a_clip_set(const char* t, unsigned long n) { if (t) clipboard_set(t, (uint32_t)n); }
static const char* a_clip_get(unsigned long* n) {
    uint32_t l;
    const char* s = clipboard_get(&l);
    if (n) *n = l;
    return s;
}

/* ── threads ───────────────────────────────────────────────────────── */

static int thread_tramp(void* arg) {
    athread_t* t = (athread_t*)arg;
    return t->fn(t->arg);
}

static void thread_entry(void) {
    app_proc_t* p = cur();
    athread_t* t = cur_thread(p);
    if (!p || !t) return;
    if (p->has_term) terminal_vt_set_active(p->vt);
    else task_set_background();
    t->running = 1;
    t->ret = app_enter(thread_tramp, t, t->stack_mem + APP_STACK, &t->saved_sp);
    t->running = 0;
    t->done = 1;
}

static int a_thread_create(int (*fn)(void*), void* arg) {
    app_proc_t* p = cur();
    if (!p || !fn || p->exit_req) return -1;
    athread_t* t = NULL;
    int id = -1;
    for (int i = 0; i < APP_THREADS; i++) if (!p->threads[i].used) { t = &p->threads[i]; id = i; break; }
    if (!t) return -1;
    memset(t, 0, sizeof(*t));
    t->stack_mem = (uint8_t*)kmalloc(APP_STACK + 16);
    if (!t->stack_mem) return -1;
    t->fn = fn;
    t->arg = arg;
    t->pid = -1;
    t->used = 1;
    char name[24];
    ksnprintf(name, sizeof(name), "%.14s/t%d", p->name, id + 1);
    int pid = task_create(name, thread_entry);
    if (pid < 0) { kfree(t->stack_mem); t->used = 0; return -1; }
    t->pid = pid;                           /* it runs once we yield (no preemption in here) */
    return id + 1;
}

static int a_thread_join(int id) {
    app_proc_t* p = cur();
    if (!p || id < 1 || id > APP_THREADS) return -1;
    athread_t* t = &p->threads[id - 1];
    if (!t->used || t == cur_thread(p)) return -1;
    while (!t->done) {
        breathe(p, 1);
        task_sleep_ms(1);
    }
    int r = t->ret;
    kfree(t->stack_mem);
    t->used = 0;
    return r;
}

static int a_thread_id(void) {
    app_proc_t* p = cur();
    athread_t* t = cur_thread(p);
    return t ? (int)(t - p->threads) + 1 : 0;
}

static void a_thread_exit(int ret) {
    app_proc_t* p = cur();
    athread_t* t = cur_thread(p);
    if (t) leave_thread(p, t, ret);
    if (p) leave(p, ret);                   /* the main thread: the app ends */
    for (;;) task_sleep_ms(1000);
}

static int a_cpu_count(void) { return cpu_count(); }

/* wait / wake: a thread sleeps while *addr == expected, until another one
 * changes it and calls wake (or the timeout passes) - what the SDK builds
 * condition variables and semaphores on */
#define WAITERS 64
static struct { volatile int* addr; int pid; } g_waiters[WAITERS];

static int waiter_add(volatile int* addr) {
    for (int i = 0; i < WAITERS; i++)
        if (!g_waiters[i].addr) { g_waiters[i].addr = addr; g_waiters[i].pid = task_current_pid(); return i; }
    return -1;
}

static int a_wait(volatile int* addr, int expected, int timeout_ms) {
    app_proc_t* p = cur();
    if (!p || !addr) return -1;
    uint32_t end = timer_ms() + (uint32_t)(timeout_ms > 0 ? timeout_ms : 0);
    for (;;) {
        breathe(p, 1);                      /* paints, notices Ctrl+C / End task (may not return) */
        if (*addr != expected) return 0;
        if (timeout_ms >= 0 && (int32_t)(timer_ms() - end) >= 0) return 1;
        /* registered only while asleep: if the app is stopped in breathe()
         * no stale entry is left behind; a wake in between costs <= 10 ms */
        int slot = waiter_add(addr);
        if (*addr == expected) task_sleep_ms(slot >= 0 ? 10 : 1);
        if (slot >= 0) g_waiters[slot].addr = NULL;
    }
}

static int a_wake(volatile int* addr, int count) {
    int n = 0;
    for (int i = 0; i < WAITERS; i++) {
        if (g_waiters[i].addr != addr) continue;
        task_wake(g_waiters[i].pid);
        if (++n >= count && count > 0) break;
    }
    return n;
}

/* kernel/idt.c, on every hardware interrupt: if it came while an app's
 * own code was running (not inside the kernel), switching to another task
 * is safe - so a busy app or thread cannot freeze the desktop, and the
 * threads of an app share the CPU. An app asked to stop is stopped here. */
void app_preempt(uintptr_t ip) {
    app_proc_t* p = cur();
    if (!p || ip < p->code_lo || ip >= p->code_hi) return;
    athread_t* t = cur_thread(p);
    if (t ? !t->running : !p->running) return;
    __asm__ volatile("sti");
    if (p->exit_req || p->kill_req) leave(p, p->exit_req ? p->exit_req_code : 137);
    /* what an app call does: paint the desktop (the app may run in the task
     * that draws it), notice Ctrl+C / End task, let the other tasks run */
    breathe(p, 0);
    task_maybe_yield();
    if (p->has_term) terminal_vt_set_active(p->vt);
    __asm__ volatile("cli");
}

static void a_exit(int code) {
    app_proc_t* p = cur();
    if (p) leave(p, code);
    for (;;) task_sleep_ms(1000);    /* not an app: cannot happen */
}

static void api_init(void) {
    if (g_api.magic) return;
    g_api.magic = BANANA_API_MAGIC;
    g_api.version = BANANA_API_VERSION;
    g_api.size = sizeof(banana_api_t);
    g_api.arch = BANANA_ARCH;
    g_api.os_version = "0.5";
    g_api.exit = a_exit;
    g_api.write = a_write;
    g_api.getchar = a_getchar;
    g_api.trygetchar = a_trygetchar;
    g_api.readline = a_readline;
    g_api.set_color = a_set_color;
    g_api.clear_screen = a_clear;
    g_api.term_size = a_term_size;
    g_api.set_cursor = a_set_cursor;
    g_api.malloc = a_malloc;
    g_api.free = a_free;
    g_api.realloc = a_realloc;
    g_api.open = a_open;
    g_api.read = a_read;
    g_api.seek = a_seek;
    g_api.close = a_close;
    g_api.remove = a_remove;
    g_api.mkdir = a_mkdir;
    g_api.rename = a_rename;
    g_api.stat = a_stat;
    g_api.readdir = a_readdir;
    g_api.getcwd = a_getcwd;
    g_api.chdir = a_chdir;
    g_api.ticks_ms = a_ticks;
    g_api.sleep_ms = a_sleep;
    g_api.yield = a_yield;
    g_api.localtime = a_localtime;
    g_api.random = a_random;
    g_api.win_open = a_win_open;
    g_api.win_pixels = a_win_pixels;
    g_api.win_update = a_win_update;
    g_api.win_event = a_win_event;
    g_api.win_close = a_win_close;
    g_api.win_set_title = a_win_set_title;
    g_api.win_size = a_win_size;
    g_api.draw_text = a_draw_text;
    g_api.font8x8 = &font8x8_basic[0][0];
    g_api.http_get = a_http_get;
    g_api.audio_play = a_audio_play;
    g_api.audio_beep = a_audio_beep;
    g_api.audio_busy = a_audio_busy;
    g_api.clipboard_set = a_clip_set;
    g_api.clipboard_get = a_clip_get;
    g_api.interrupted = a_interrupted;
    g_api.win_set_resizable = a_win_set_resizable;
    g_api.thread_create = a_thread_create;
    g_api.thread_join = a_thread_join;
    g_api.thread_id = a_thread_id;
    g_api.thread_exit = a_thread_exit;
    g_api.cpu_count = a_cpu_count;
    g_api.wait = a_wait;
    g_api.wake = a_wake;
}

/* ── the ELF loader ────────────────────────────────────────────────── */

#define PT_LOAD    1
#define PT_DYNAMIC 2
#define DT_NULL    0
#define DT_RELA    7
#define DT_RELASZ  8
#define DT_RELAENT 9
#define DT_REL     17
#define DT_RELSZ   18
#define DT_RELENT  19
#define R_X86_64_NONE     0
#define R_X86_64_RELATIVE 8
#define R_386_NONE        0
#define R_386_RELATIVE    8

#ifdef __x86_64__
#define EM_HOST 62            /* EM_X86_64 */
#define ELF_CLASS 2
typedef struct { uint8_t ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff;
                 uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } ehdr_t;
typedef struct { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } phdr_t;
typedef struct { int64_t tag; uint64_t val; } dyn_t;
#else
#define EM_HOST 3             /* EM_386 */
#define ELF_CLASS 1
typedef struct { uint8_t ident[16]; uint16_t type, machine; uint32_t version, entry, phoff, shoff;
                 uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } ehdr_t;
typedef struct { uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align; } phdr_t;
typedef struct { int32_t tag; uint32_t val; } dyn_t;
#endif


static int load_elf(const uint8_t* f, uint32_t size, uint8_t** image_out, banana_entry_t* entry, uintptr_t* base_out,
                    uintptr_t* lo_out, uintptr_t* hi_out, char* err, int ecap) {
    const ehdr_t* eh = (const ehdr_t*)f;
    if (size < sizeof(ehdr_t) || memcmp(eh->ident, "\x7f" "ELF", 4) != 0) { kstrlcpy(err, "not an ELF program", (size_t)ecap); return -1; }
    if (eh->ident[4] != ELF_CLASS || eh->machine != EM_HOST) {
        ksnprintf(err, (size_t)ecap, "built for another CPU (this is the %s kernel)", BANANA_ARCH);
        return -1;
    }
    if (eh->type != 3) { kstrlcpy(err, "not position-independent (build it with the Banana OS SDK)", (size_t)ecap); return -1; }
    if (eh->phentsize != sizeof(phdr_t) || eh->phoff + (uint32_t)eh->phnum * sizeof(phdr_t) > size) {
        kstrlcpy(err, "damaged ELF headers", (size_t)ecap);
        return -1;
    }
    const phdr_t* ph = (const phdr_t*)(f + eh->phoff);
    uintptr_t lo = ~(uintptr_t)0, hi = 0;
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type != PT_LOAD) continue;
        if (ph[i].offset + ph[i].filesz > size || ph[i].filesz > ph[i].memsz) { kstrlcpy(err, "damaged ELF segments", (size_t)ecap); return -1; }
        if (ph[i].vaddr < lo) lo = (uintptr_t)ph[i].vaddr;
        if (ph[i].vaddr + ph[i].memsz > hi) hi = (uintptr_t)(ph[i].vaddr + ph[i].memsz);
    }
    if (hi <= lo || hi - lo > APP_MAX_IMAGE) { kstrlcpy(err, "no loadable code (or too big)", (size_t)ecap); return -1; }
    lo &= ~(uintptr_t)4095;
    uint32_t span = (uint32_t)(hi - lo);
    uint8_t* raw = (uint8_t*)kmalloc(span + 4096);
    if (!raw) { kstrlcpy(err, "out of memory", (size_t)ecap); return -1; }
    uint8_t* mem = (uint8_t*)(((uintptr_t)raw + 4095) & ~(uintptr_t)4095);
    memset(mem, 0, span);
    uintptr_t base = (uintptr_t)mem - lo;
    const dyn_t* dyn = NULL;
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type == PT_LOAD) memcpy((void*)(base + (uintptr_t)ph[i].vaddr), f + ph[i].offset, (size_t)ph[i].filesz);
        if (ph[i].type == PT_DYNAMIC) dyn = (const dyn_t*)(base + (uintptr_t)ph[i].vaddr);
    }
    /* relocations: a static PIE only has "base + addend" ones */
    if (dyn) {
        uintptr_t rel = 0, relsz = 0, relent = 0, rela = 0, relasz = 0, relaent = 0;
        for (const dyn_t* d = dyn; d->tag != DT_NULL && (const uint8_t*)d < mem + span; d++) {
            switch (d->tag) {
            case DT_RELA: rela = (uintptr_t)d->val; break;
            case DT_RELASZ: relasz = (uintptr_t)d->val; break;
            case DT_RELAENT: relaent = (uintptr_t)d->val; break;
            case DT_REL: rel = (uintptr_t)d->val; break;
            case DT_RELSZ: relsz = (uintptr_t)d->val; break;
            case DT_RELENT: relent = (uintptr_t)d->val; break;
            }
        }
#ifdef __x86_64__
        (void)rel; (void)relsz; (void)relent;
        if (rela && relaent == 24) {
            for (uintptr_t o = 0; o + 24 <= relasz; o += 24) {
                const uint64_t* r = (const uint64_t*)(base + rela + o);
                uint32_t type = (uint32_t)r[1];
                if (type == R_X86_64_NONE) continue;
                if (type != R_X86_64_RELATIVE || r[0] - lo + 8 > span) {
                    ksnprintf(err, (size_t)ecap, "unsupported relocation type %u (link it with the SDK)", type);
                    kfree(raw);
                    return -1;
                }
                *(uint64_t*)(base + r[0]) = base + r[2];
            }
        }
#else
        (void)rela; (void)relasz; (void)relaent;
        if (rel && relent == 8) {
            for (uintptr_t o = 0; o + 8 <= relsz; o += 8) {
                const uint32_t* r = (const uint32_t*)(base + rel + o);
                uint32_t type = r[1] & 0xFF;
                if (type == R_386_NONE) continue;
                if (type != R_386_RELATIVE || r[0] - lo + 4 > span) {
                    ksnprintf(err, (size_t)ecap, "unsupported relocation type %u (link it with the SDK)", type);
                    kfree(raw);
                    return -1;
                }
                *(uint32_t*)(base + r[0]) += base;
            }
        }
#endif
    }
    *image_out = raw;
    *entry = (banana_entry_t)(base + (uintptr_t)eh->entry);
    *base_out = base;
    *lo_out = (uintptr_t)mem;
    *hi_out = (uintptr_t)mem + span;
    return 0;
}

/* ── running ───────────────────────────────────────────────────────── */

static int trampoline(void* arg) {
    app_proc_t* p = (app_proc_t*)arg;
    return p->entry(&g_api, p->argc, p->argv);
}

/* before an app's memory goes: every thread has to be out of its code */
static void stop_threads(app_proc_t* p) {
    p->exit_req = 1;
    for (;;) {
        int live = 0;
        for (int t = 0; t < APP_THREADS; t++) if (p->threads[t].used && !p->threads[t].done) live++;
        if (!live) break;
        task_sleep_ms(2);                   /* a busy one is stopped by the timer (app_preempt) */
    }
    for (int t = 0; t < APP_THREADS; t++) {
        if (p->threads[t].used) kfree(p->threads[t].stack_mem);
        p->threads[t].used = 0;
    }
}

static void release(app_proc_t* p) {
    stop_threads(p);
    appwin_close_owner(p->id);
    while (p->blocks) {
        ablock_t* b = p->blocks;
        p->blocks = b->next;
        b->magic = 0;
        kfree(b);
    }
    kfree(p->image);
    if (p->guarded) paging_guard((uintptr_t)p->guard, APP_GUARD, 0);
    kfree(p->stack_mem);
    kfree(p->argv);
    p->used = 0;
}


static int run(app_proc_t* p) {
    *(volatile uint32_t*)p->stack = STACK_CANARY;      /* the bottom of the stack: overflow check */
    p->guarded = paging_guard((uintptr_t)p->guard, APP_GUARD, 1);
    p->running = 1;
    p->exit_code = app_enter(trampoline, p, p->stack + APP_STACK, &p->saved_sp);
    p->running = 0;
    if (p->crash_msg[0]) {                              /* it crashed (app_fault) */
        klog("app crashed: %s\n", p->crash_msg);
        if (p->has_term) {
            terminal_vt_set_active(p->vt);
            terminal_write_color(p->crash_msg, VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            terminal_putchar('\n');
        }
    }
    if (*(volatile uint32_t*)p->stack != STACK_CANARY)
        klog("app %s: its stack overflowed (256 KiB) - memory may be damaged\n", p->name);
    return p->exit_code;
}

/* A CPU exception (kernel/idt.c) while an app runs - in its own code, or
 * in a system call it made with a bad pointer: the app is stopped, the
 * rest of the system goes on. Returns only if no app is running here. */
void app_fault(uint32_t vector, uint32_t err, uintptr_t ip, uintptr_t addr) {
    app_proc_t* p = cur();
    if (!p) return;
    athread_t* ft = cur_thread(p);
    if (ft ? !ft->running : !p->running) return;
    if (ft) ft->running = 0;
    else p->running = 0;
    /* We came in through an interrupt gate, so interrupts are off - and the
     * terminal output below may yield to other tasks, which must not run
     * with interrupts off (timers, sleeps and hlt would all stop): turn
     * them on first. This exception handler is never returned from. */
    __asm__ volatile("sti");
    static const char* const what[20] = {
        "division by zero", "debug trap", "NMI", "breakpoint", "overflow", "bound range",
        "invalid instruction", "no FPU", "double fault", "", "invalid TSS", "segment not present",
        "stack fault", "general protection fault", "segmentation fault (bad memory access)", "",
        "floating point error", "alignment check", "machine check", "SIMD floating point error",
    };
    const char* w = vector < 20 && what[vector][0] ? what[vector] : "CPU exception";
    uintptr_t base = p->base;
    char line[160];
    if (vector == 14 && p->guarded && addr >= (uintptr_t)p->guard && addr < (uintptr_t)p->guard + APP_GUARD)
        ksnprintf(line, sizeof(line), "%s: stack overflow - it used more than %u KiB of stack (big local arrays? malloc them)",
                  p->name, APP_STACK >> 10);
    else if (vector == 14)
        ksnprintf(line, sizeof(line), "%s: %s at address %p (ip %p, app+%x, error %x)", p->name, w, (void*)addr,
                  (void*)ip, (uint32_t)(ip - base), err);
    else
        ksnprintf(line, sizeof(line), "%s: %s (ip %p, app+%x)", p->name, w, (void*)ip, (uint32_t)(ip - base));
    /* reported by run(), back on the task's own stack: this may be the
     * dedicated fault stack, which must not be in use when we yield */
    kstrlcpy(p->crash_msg, line, sizeof(p->crash_msg));
    athread_t* t = cur_thread(p);
    if (t) {                                /* a crash in a thread ends the whole app */
        if (!p->exit_req) { p->exit_req = 1; p->exit_req_code = 139; }
        t->running = 0;
        app_leave(t->saved_sp, 139);
    }
    app_leave(p->saved_sp, 139);
}

/* reads, loads and prepares an app (slot claimed, not started) */
static app_proc_t* prepare(const char* path, int argc, char** argv, char* err, int ecap) {
    api_init();
    int fi = fs_find_file(path);
    if (fi < 0) { kstrlcpy(err, "no such program", (size_t)ecap); return NULL; }
    app_proc_t* p = NULL;
    for (int i = 0; i < APP_MAX; i++) if (!g_procs[i].used) { p = &g_procs[i]; p->id = i; break; }
    if (!p) { kstrlcpy(err, "too many apps running", (size_t)ecap); return NULL; }
    int id = p->id;
    memset(p, 0, sizeof(*p));
    p->id = id;
    fs_file_t* f = fs_get_file(fi);
    if (!f) { kstrlcpy(err, "cannot read it", (size_t)ecap); return NULL; }
    if (load_elf((const uint8_t*)f->content, f->size, &p->image, &p->entry, &p->base, &p->code_lo, &p->code_hi, err, ecap) != 0)
        return NULL;
    p->stack_mem = (uint8_t*)kmalloc(APP_GUARD + APP_STACK + 4096);
    if (p->stack_mem) {
        p->guard = (uint8_t*)(((uintptr_t)p->stack_mem + 4095) & ~(uintptr_t)4095);
        p->stack = p->guard + APP_GUARD;
    }
    /* argv: the pointers and the strings in one block */
    uint32_t need = (uint32_t)(argc + 1) * sizeof(char*);
    for (int i = 0; i < argc; i++) need += (uint32_t)strlen(argv[i]) + 1;
    p->argv = (char**)kmalloc(need);
    if (!p->stack_mem || !p->argv) {
        kfree(p->image); kfree(p->stack_mem); kfree(p->argv);
        kstrlcpy(err, "out of memory", (size_t)ecap);
        return NULL;
    }
    char* s = (char*)(p->argv + argc + 1);
    for (int i = 0; i < argc; i++) {
        p->argv[i] = s;
        size_t l = strlen(argv[i]) + 1;
        memcpy(s, argv[i], l);
        s += l;
    }
    p->argv[argc] = NULL;
    p->argc = argc;
    const char* base = strrchr(argc ? argv[0] : path, '/');
    kstrlcpy(p->name, base ? base + 1 : (argc ? argv[0] : path), sizeof(p->name));
    p->used = 1;
    return p;
}


int app_exec(const char* path, int argc, char** argv, char* err, int ecap) {
    app_proc_t* p = prepare(path, argc, argv, err, ecap);
    if (!p) return -1;
    p->pid = task_current_pid();
    p->vt = terminal_vt_get_active();
    p->has_term = !task_is_background();
    p->last_breath = timer_ms();
    int rc = run(p);
    terminal_vt_set_active(p->vt);
    terminal_setcolor(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    release(p);
    return rc;
}

static void spawned_entry(void) {
    app_proc_t* p = cur();
    if (!p) return;
    task_set_background();             /* no terminal: it talks through its windows */
    run(p);
    klog("app %s exited (%d)\n", p->name, p->exit_code);
    release(p);
}

int app_spawn(const char* path, int argc, char** argv, char* err, int ecap) {
    app_proc_t* p = prepare(path, argc, argv, err, ecap);
    if (!p) return -1;
    p->pid = -1;
    p->own_task = 1;
    p->has_term = 0;
    p->vt = 0;
    int pid = task_create(p->name, spawned_entry);
    if (pid < 0) { release(p); kstrlcpy(err, "too many tasks", (size_t)ecap); return -1; }
    p->pid = pid;                      /* the new task runs once we yield */
    return 0;
}

int app_count(void) {
    int n = 0;
    for (int i = 0; i < APP_MAX; i++) if (g_procs[i].used) n++;
    return n;
}

void app_list(void) {
    char line[96];
    int any = 0;
    for (int i = 0; i < APP_MAX; i++) {
        app_proc_t* p = &g_procs[i];
        if (!p->used) continue;
        uint32_t mem = 0;
        for (ablock_t* b = p->blocks; b; b = b->next) mem += (uint32_t)b->size;
        ksnprintf(line, sizeof(line), "  %-16s pid %-3d %s  %u KiB allocated", p->name, p->pid,
                  p->own_task ? "desktop " : "terminal", mem / 1024);
        terminal_writeln(line);
        any = 1;
    }
    if (!any) terminal_writeln("  (no app running)");
}

int app_snapshot(app_info_t* out, int max) {
    int n = 0;
    for (int i = 0; i < APP_MAX && n < max; i++) {
        app_proc_t* p = &g_procs[i];
        if (!p->used) continue;
        out[n].id = p->id;
        kstrlcpy(out[n].name, p->name, sizeof(out[n].name));
        out[n].pid = p->pid;
        out[n].desktop = p->own_task;
        out[n].mem = 0;
        out[n].threads = 1;
        for (int t = 0; t < APP_THREADS; t++) if (p->threads[t].used && !p->threads[t].done) out[n].threads++;
        for (ablock_t* b = p->blocks; b; b = b->next) out[n].mem += (uint32_t)b->size;
        n++;
    }
    return n;
}

int app_kill(int id) {
    if (id < 0 || id >= APP_MAX || !g_procs[id].used) return -1;
    g_procs[id].kill_req = 1;
    return 0;
}
