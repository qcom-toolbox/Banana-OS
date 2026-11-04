/* SPDX-License-Identifier: BSD-2-Clause */
/* Banana OS SDK: POSIX bits over the system calls - file descriptors
 * (open, lseek, fstat, stat), aligned memory, fdopen - for programs
 * ported from elsewhere. */
#include <banana.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/time.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern const banana_api_t* __banana;
#define KFD BANANA_HAS(__banana, fd_info)        /* the system has Unix descriptors (version 25) */

int __ufd_drm_open(void);

int open(const char* path, int flags, ...) {
    /* the GPU (an Intel one's 3D: ufd.c) */
    if (path && (!strcmp(path, "/dev/dri/card0") || !strcmp(path, "/dev/dri/renderD128"))) return __ufd_drm_open();
    int f = 0;
    int acc = flags & O_ACCMODE;
    if (acc == O_RDONLY || acc == O_RDWR) f |= BANANA_O_READ;
    if (acc == O_WRONLY || acc == O_RDWR) f |= BANANA_O_WRITE;
    if (flags & O_CREAT)  f |= BANANA_O_CREATE;
    if (flags & O_TRUNC)  f |= BANANA_O_TRUNC;
    if (flags & O_APPEND) f |= BANANA_O_APPEND;
    if (flags & O_NOCTTY) f |= BANANA_O_NOCTTY;
    if (flags & O_DIRECTORY) {                     /* a folder or nothing (mv, find: is it one?) */
        banana_stat_t st;
        if (__banana->stat(path, &st) != 0) { errno = ENOENT; return -1; }
        if (!st.is_dir) { errno = ENOTDIR; return -1; }
    }
    if ((flags & O_EXCL) && (flags & O_CREAT)) {
        banana_stat_t st;
        if (__banana->stat(path, &st) == 0) { errno = EEXIST; return -1; }
    }
    int fd = __banana->open(path, f);
    if (fd < -1) { errno = -fd; return -1; }           /* (/dev: the system says why) */
    if (fd < 0) {
        /* why: it is not there - or it is a folder, or every descriptor is
         * taken (EMFILE: Java then knows, instead of "no such file") */
        banana_stat_t st;
        if (__banana->stat(path, &st) != 0) errno = ENOENT;
        else errno = st.is_dir ? EISDIR : EMFILE;
        return -1;
    }
    if ((flags & O_CLOEXEC) && KFD) __banana->fd_fcntl(fd, F_SETFD, FD_CLOEXEC);
    if ((flags & O_NONBLOCK) && KFD) __banana->fd_fcntl(fd, F_SETFL, O_NONBLOCK);
    return fd;
}

/* (socket.c: a socket's O_NONBLOCK) */
int __sock_getfl(int fd);
int __sock_setfl(int fd, int fl);
/* (ufd.c: the C library's own descriptors) */
int __ufd_is(int fd);
int __ufd_fcntl(int fd, int cmd, long arg);
int __ufd_fstat(int fd, struct stat* st);

int fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    long arg = va_arg(ap, long);
    va_end(ap);
    if (__ufd_is(fd)) return __ufd_fcntl(fd, cmd, arg);
    if (KFD) {                                   /* the system's descriptors (version 25) */
        int r = cmd == F_DUPFD ? __banana->fd_dup(fd, (int)arg, 0)
              : cmd == F_DUPFD_CLOEXEC ? __banana->fd_dup(fd, (int)arg, BANANA_DUP_CLOEXEC)
              : (cmd >= F_GETFD && cmd <= F_SETFL) ? __banana->fd_fcntl(fd, cmd, arg) : 0;
        if (r < 0) { errno = -r; return -1; }
        return r;
    }
    if (cmd == F_GETFL) { int f = fd > 2 ? __sock_getfl(fd) : -1; return O_RDWR | (f > 0 ? f : 0); }
    if (cmd == F_SETFL) { if (fd > 2) __sock_setfl(fd, (int)arg); return 0; }
    return 0;
}

off_t lseek(int fd, off_t off, int whence) {
    if (__ufd_is(fd)) { errno = ESPIPE; return -1; }
    /* (the system refuses to seek what is not a file: the kind of
     *  descriptor is asked only then, for the error - one call, not two) */
    long r = __banana->seek(fd, off, whence);
    if (r >= 0) return r;
    banana_fdinfo_t fi;
    if (KFD && __banana->fd_info(fd, &fi) == 0 && fi.type != BANANA_FD_FILE) { errno = ESPIPE; return -1; }
    errno = fd <= 2 ? ESPIPE : EINVAL;
    return -1;
}

/* (isatty: tty.c; stat, fstat, mkdir, realpath: ufs.c) */

/* ── aligned memory ───────────────────────────────────────────────── */
/* An alignment over 16: a bigger block, the pointer moved up into it. free
 * and realloc must give back the block itself, so the aligned pointers are
 * kept in a table (pointer -> block, size) - nothing is probed below a
 * pointer passed to free: an ordinary allocation can start at a user
 * mapping boundary, with kernel-only memory immediately before it.
 * Every free asks the table, so asking is cheap: a pointer not aligned to
 * 32 cannot be in it (no lock), the rest is one hash lookup. The lock spins
 * a little, then yields: its holder may have been preempted, and pure
 * spinning kept every core busy waiting for it (the JVM and Mesa free from
 * many threads at once - the whole system froze). No allocation, syscall
 * or copy takes place while the lock is held. */
typedef struct { void* ptr; void* raw; size_t size; } aligned_slot_t;
#define SLOT_GONE ((void*)1)                     /* (a removed entry: lookups go on past it) */

static aligned_slot_t* g_atab;
static size_t g_acap, g_aused, g_alive;          /* slots (a power of 2), taken (live + gone), live */
static volatile int g_alock;

static void aligned_lock(void) {
    int spins = 0;
    while (__sync_lock_test_and_set(&g_alock, 1)) {
        while (g_alock) {
            if (++spins > 100) { __banana->yield(); spins = 0; }   /* the holder needs the CPU */
            __asm__ volatile("pause");
        }
    }
}

static void aligned_unlock(void) { __sync_lock_release(&g_alock); }

static size_t aligned_hash(const void* p, size_t cap) {
    uintptr_t x = (uintptr_t)p >> 5;
    x ^= x >> 15;
    x *= (uintptr_t)0x9E3779B97F4A7C15ull;
    return (size_t)(x ^ (x >> 16)) & (cap - 1);
}

/* the live entries into tab (cap slots, zeroed) */
static void aligned_rehash(aligned_slot_t* tab, size_t cap) {
    for (size_t i = 0; i < g_acap; i++) {
        void* p = g_atab[i].ptr;
        if (!p || p == SLOT_GONE) continue;
        size_t k = aligned_hash(p, cap);
        while (tab[k].ptr) k = (k + 1) & (cap - 1);
        tab[k] = g_atab[i];
    }
}

static int aligned_add(void* p, void* raw, size_t size) {
    aligned_lock();
    while ((g_aused + 1) * 4 > g_acap * 3) {             /* over 3/4 full: a bigger table */
        size_t cap = g_acap, want = 64;
        while (want < (g_alive + 1) * 4) want *= 2;
        aligned_unlock();
        aligned_slot_t* tab = calloc(want, sizeof(*tab));
        if (!tab) return -1;
        aligned_lock();
        aligned_slot_t* old = NULL;
        if (g_acap == cap) {                             /* (nobody grew it meanwhile) */
            aligned_rehash(tab, want);
            old = g_atab;
            g_atab = tab;
            g_acap = want;
            g_aused = g_alive;
        } else {
            old = tab;
        }
        aligned_unlock();
        free(old);
        aligned_lock();
    }
    size_t k = aligned_hash(p, g_acap);
    while (g_atab[k].ptr && g_atab[k].ptr != SLOT_GONE) k = (k + 1) & (g_acap - 1);
    if (!g_atab[k].ptr) g_aused++;
    g_atab[k].ptr = p;
    g_atab[k].raw = raw;
    g_atab[k].size = size;
    g_alive++;
    aligned_unlock();
    return 0;
}

int __malloc_heap(void);                           /* malloc.c */
void* __malloc_aligned(size_t align, size_t n);

int posix_memalign(void** out, size_t align, size_t size) {
    if (!out || align < sizeof(void*) || (align & (align - 1))) return EINVAL;
    if (__malloc_heap()) {                              /* (the app's heap aligns blocks itself) */
        void* p = __malloc_aligned(align, size);
        if (!p) return ENOMEM;
        *out = p;
        return 0;
    }
    /* the kernel's allocator: a bigger block, and where the aligned part is noted */
    if (align <= 16) {
        void* p = malloc(size);
        if (!p) return ENOMEM;
        *out = p;
        return 0;
    }
    if (size > (size_t)-1 - align) return ENOMEM;
    void* raw = malloc(size + align - 1);
    if (!raw) return ENOMEM;
    void* p = (void*)(((uintptr_t)raw + align - 1) & ~(uintptr_t)(align - 1));
    if (aligned_add(p, raw, size)) { free(raw); return ENOMEM; }
    *out = p;
    return 0;
}
void* aligned_alloc(size_t align, size_t size) { void* p; return posix_memalign(&p, align, size) ? NULL : p; }
void* memalign(size_t align, size_t size) { return aligned_alloc(align, size); }

/* (libc.c: free and realloc know these blocks) */
int __aligned_block(void* p, void** raw, size_t* size, int remove) {
    /* (only pointers aligned to 32 or more are ever added; g_alive is read
     * unlocked: p's own entry, if any, was added before p was handed out) */
    if (((uintptr_t)p & 31) || !g_alive) return 0;
    int found = 0;
    aligned_lock();
    if (g_acap) {
        for (size_t k = aligned_hash(p, g_acap); g_atab[k].ptr; k = (k + 1) & (g_acap - 1)) {
            if (g_atab[k].ptr != p) continue;
            if (raw) *raw = g_atab[k].raw;
            if (size) *size = g_atab[k].size;
            if (remove) { g_atab[k].ptr = SLOT_GONE; g_alive--; }
            found = 1;
            break;
        }
    }
    aligned_unlock();
    return found;
}

intmax_t strtoimax(const char* s, char** end, int base) { return strtoll(s, end, base); }
uintmax_t strtoumax(const char* s, char** end, int base) { return strtoull(s, end, base); }

/* the process's timer (version 30): alarm(), setitimer(ITIMER_REAL) and
 * one POSIX timer (timer_create) - the system sends the signal. Before
 * version 30: none (getitimer says none is armed, setitimer only disarms). */
#define TIMERS BANANA_HAS(__banana, proc_alarm)
#include <signal.h>

static long long tv_us(const struct timeval* t) { return (long long)t->tv_sec * 1000000 + t->tv_usec; }
static void us_tv(long long us, struct timeval* t) { t->tv_sec = (long)(us / 1000000); t->tv_usec = (long)(us % 1000000); }

int getitimer(int which, struct itimerval* cur) {
    if (cur) memset(cur, 0, sizeof(*cur));
    if (which != ITIMER_REAL || !TIMERS || !cur) return 0;
    long long old[2] = { 0, 0 };
    __banana->proc_alarm(SIGALRM, -1, 0, old);
    us_tv(old[0], &cur->it_value);
    us_tv(old[1], &cur->it_interval);
    return 0;
}

int setitimer(int which, const struct itimerval* val, struct itimerval* old) {
    if (old) memset(old, 0, sizeof(*old));
    if (which != ITIMER_REAL || !TIMERS) {
        if (val && (val->it_value.tv_sec || val->it_value.tv_usec)) { errno = ENOSYS; return -1; }
        return 0;
    }
    long long o[2];
    int r = __banana->proc_alarm(SIGALRM, val ? tv_us(&val->it_value) : 0, val ? tv_us(&val->it_interval) : 0, o);
    if (r < 0) { errno = -r; return -1; }
    if (old) { us_tv(o[0], &old->it_value); us_tv(o[1], &old->it_interval); }
    return 0;
}

unsigned alarm(unsigned s) {
    if (!TIMERS) return 0;
    long long o[2];
    if (__banana->proc_alarm(SIGALRM, (long long)s * 1000000, 0, o) < 0) return 0;
    return (unsigned)((o[0] + 999999) / 1000000);
}

unsigned ualarm(unsigned us, unsigned every) {
    if (!TIMERS) return 0;
    long long o[2];
    if (__banana->proc_alarm(SIGALRM, us, every, o) < 0) return 0;
    return (unsigned)o[0];
}

/* one POSIX timer (the same timer as alarm's, as on systems that have one) */
static int g_timer_sig = SIGALRM, g_timer_made;
int timer_create(clockid_t clk, struct sigevent* ev, timer_t* id) {
    (void)clk;
    if (!TIMERS || g_timer_made) { errno = EAGAIN; return -1; }
    if (ev && ev->sigev_notify == SIGEV_THREAD) { errno = ENOTSUP; return -1; }
    g_timer_sig = ev && ev->sigev_notify == SIGEV_SIGNAL ? ev->sigev_signo : ev ? 0 : SIGALRM;
    g_timer_made = 1;
    *id = (timer_t)(uintptr_t)1;
    return 0;
}
int timer_delete(timer_t id) {
    (void)id;
    if (g_timer_made && TIMERS) __banana->proc_alarm(SIGALRM, 0, 0, NULL);
    g_timer_made = 0;
    return 0;
}
static long long ts_us(const struct timespec* t) { return (long long)t->tv_sec * 1000000 + (t->tv_nsec + 999) / 1000; }
static void us_ts(long long us, struct timespec* t) { t->tv_sec = (time_t)(us / 1000000); t->tv_nsec = (long)(us % 1000000) * 1000; }
int timer_settime(timer_t id, int flags, const struct itimerspec* val, struct itimerspec* old) {
    (void)id;
    if (!g_timer_made || !val) { errno = EINVAL; return -1; }
    long long v = ts_us(&val->it_value);
    if ((flags & TIMER_ABSTIME) && v) {
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        v -= ts_us(&now);
        if (v <= 0) v = 1;
    }
    long long o[2];
    /* (SIGEV_NONE: nothing to send - the timer still runs, for timer_gettime) */
    int r = __banana->proc_alarm(g_timer_sig ? g_timer_sig : SIGALRM, g_timer_sig ? v : 0, ts_us(&val->it_interval), o);
    if (r < 0) { errno = -r; return -1; }
    if (old) { us_ts(o[0], &old->it_value); us_ts(o[1], &old->it_interval); }
    return 0;
}
int timer_gettime(timer_t id, struct itimerspec* cur) {
    (void)id;
    struct itimerval v;
    getitimer(ITIMER_REAL, &v);
    us_ts(tv_us(&v.it_value), &cur->it_value);
    us_ts(tv_us(&v.it_interval), &cur->it_interval);
    return 0;
}
int timer_getoverrun(timer_t id) { (void)id; return 0; }

/* the wall clock (libc.c: __realtime_ms - it never runs backwards) */
long long __realtime_ms(void);                  /* (libc.c: never runs backwards) */
long long __realtime_ns(void);                  /* (the same, to the nanosecond) */
int gettimeofday(struct timeval* tv, void* tz) {
    (void)tz;
    if (tv) { long long now = __realtime_ns() / 1000; tv->tv_sec = (long)(now / 1000000); tv->tv_usec = (long)(now % 1000000); }
    return 0;
}

