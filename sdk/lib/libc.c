/* SPDX-License-Identifier: BSD-2-Clause */
/* Banana OS SDK libc: strings, memory, conversions, sorting, time. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include "banana_api.h"

extern const banana_api_t* __banana;

/* errno, one per thread (by the system's thread id: every kind of thread
 * has its own, however it was started). The main thread's is the global
 * `errno` that code built before build 163 (the FFmpeg libraries) uses. */
#undef errno
int errno;
static int g_errno[130];
int* __errno_location(void) {
    unsigned id = __banana && __banana->version >= 3 ? (unsigned)__banana->thread_id() : 0;
    return id ? &g_errno[id < 129 ? id : 129] : &errno;
}
/* (from here on errno is per thread again, in this file too) */
#define errno (*__errno_location())

/* ── memory / strings ─────────────────────────────────────────────── */

void* memcpy(void* dst, const void* src, size_t n) {
    unsigned char* d = dst;
    const unsigned char* s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
    unsigned char* d = dst;
    const unsigned char* s = src;
    if (d < s) while (n--) *d++ = *s++;
    else { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}

void* memset(void* dst, int c, size_t n) {
    unsigned char* d = dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
    const unsigned char* x = a;
    const unsigned char* y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
    return 0;
}

void* memchr(const void* s, int c, size_t n) {
    const unsigned char* p = s;
    for (; n; n--, p++) if (*p == (unsigned char)c) return (void*)p;
    return NULL;
}

size_t strlen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
size_t strnlen(const char* s, size_t max) { size_t n = 0; while (n < max && s[n]) n++; return n; }
char* strcpy(char* d, const char* s) { char* r = d; while ((*d++ = *s++)) ; return r; }
char* strncpy(char* d, const char* s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
char* strcat(char* d, const char* s) { strcpy(d + strlen(d), s); return d; }
char* strncat(char* d, const char* s, size_t n) {
    char* e = d + strlen(d);
    while (n-- && *s) *e++ = *s++;
    *e = 0;
    return d;
}
int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char* a, const char* b, size_t n) {
    for (; n; n--, a++, b++) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        if (!*a) return 0;
    }
    return 0;
}
int strcasecmp(const char* a, const char* b) {
    while (*a && tolower(*a) == tolower(*b)) { a++; b++; }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}
int strncasecmp(const char* a, const char* b, size_t n) {
    for (; n; n--, a++, b++) {
        int d = tolower((unsigned char)*a) - tolower((unsigned char)*b);
        if (d || !*a) return d;
    }
    return 0;
}
char* strchr(const char* s, int c) {
    for (;; s++) { if (*s == (char)c) return (char*)s; if (!*s) return NULL; }
}
char* strrchr(const char* s, int c) {
    const char* r = NULL;
    for (;; s++) { if (*s == (char)c) r = s; if (!*s) return (char*)r; }
}
char* strstr(const char* h, const char* n) {
    size_t l = strlen(n);
    if (!l) return (char*)h;
    for (; *h; h++) if (*h == *n && strncmp(h, n, l) == 0) return (char*)h;
    return NULL;
}
char* strdup(const char* s) {
    size_t l = strlen(s) + 1;
    char* d = malloc(l);
    if (d) memcpy(d, s, l);
    return d;
}
char* strndup(const char* s, size_t n) {
    size_t l = strnlen(s, n);
    char* d = malloc(l + 1);
    if (d) { memcpy(d, s, l); d[l] = 0; }
    return d;
}
size_t strspn(const char* s, const char* a) { size_t n = 0; while (s[n] && strchr(a, s[n])) n++; return n; }
size_t strcspn(const char* s, const char* r) { size_t n = 0; while (s[n] && !strchr(r, s[n])) n++; return n; }
char* strpbrk(const char* s, const char* a) { s += strcspn(s, a); return *s ? (char*)s : NULL; }
char* strtok_r(char* s, const char* d, char** save) {
    if (!s) s = *save;
    s += strspn(s, d);
    if (!*s) { *save = s; return NULL; }
    char* e = s + strcspn(s, d);
    if (*e) *e++ = 0;
    *save = e;
    return s;
}
char* strtok(char* s, const char* d) { static char* save; return strtok_r(s, d, &save); }
size_t strlcpy(char* d, const char* s, size_t n) {
    size_t l = strlen(s);
    if (n) { size_t c = l < n - 1 ? l : n - 1; memcpy(d, s, c); d[c] = 0; }
    return l;
}
size_t strlcat(char* d, const char* s, size_t n) {
    size_t dl = strnlen(d, n);
    if (dl == n) return n + strlen(s);
    return dl + strlcpy(d + dl, s, n - dl);
}
int snprintf(char* buf, size_t n, const char* fmt, ...);

/* glibc's messages, by Linux's numbers (programs and scripts know them) */
static const char* const g_errmsg[] = {
    "Success", "Operation not permitted", "No such file or directory", "No such process",
    "Interrupted system call", "Input/output error", "No such device or address", "Argument list too long",
    "Exec format error", "Bad file descriptor", "No child processes", "Resource temporarily unavailable",
    "Cannot allocate memory", "Permission denied", "Bad address", "Block device required",
    "Device or resource busy", "File exists", "Invalid cross-device link", "No such device",
    "Not a directory", "Is a directory", "Invalid argument", "Too many open files in system",
    "Too many open files", "Inappropriate ioctl for device", "Text file busy", "File too large",
    "No space left on device", "Illegal seek", "Read-only file system", "Too many links", "Broken pipe",
    "Numerical argument out of domain", "Numerical result out of range", "Resource deadlock avoided",
    "File name too long", "No locks available", "Function not implemented", "Directory not empty",
    "Too many levels of symbolic links", "", "No message of desired type", "Identifier removed",
    "Channel number out of range", "Level 2 not synchronized", "Level 3 halted", "Level 3 reset",
    "Link number out of range", "Protocol driver not attached", "No CSI structure available",
    "Level 2 halted", "Invalid exchange", "Invalid request descriptor", "Exchange full", "No anode",
    "Invalid request code", "Invalid slot", "", "Bad font file format", "Device not a stream",
    "No data available", "Timer expired", "Out of streams resources", "Machine is not on the network",
    "Package not installed", "Object is remote", "Link has been severed", "Advertise error", "Srmount error",
    "Communication error on send", "Protocol error", "Multihop attempted", "RFS specific error",
    "Bad message", "Value too large for defined data type", "Name not unique on network",
    "File descriptor in bad state", "Remote address changed", "Can not access a needed shared library",
    "Accessing a corrupted shared library", ".lib section in a.out corrupted",
    "Attempting to link in too many shared libraries", "Cannot exec a shared library directly",
    "Invalid or incomplete multibyte or wide character", "Interrupted system call should be restarted",
    "Streams pipe error", "Too many users", "Socket operation on non-socket", "Destination address required",
    "Message too long", "Protocol wrong type for socket", "Protocol not available", "Protocol not supported",
    "Socket type not supported", "Operation not supported", "Protocol family not supported",
    "Address family not supported by protocol", "Address already in use", "Cannot assign requested address",
    "Network is down", "Network is unreachable", "Network dropped connection on reset",
    "Software caused connection abort", "Connection reset by peer", "No buffer space available",
    "Transport endpoint is already connected", "Transport endpoint is not connected",
    "Cannot send after transport endpoint shutdown", "Too many references: cannot splice",
    "Connection timed out", "Connection refused", "Host is down", "No route to host",
    "Operation already in progress", "Operation now in progress", "Stale file handle",
    "Structure needs cleaning", "Not a XENIX named type file", "No XENIX semaphores available",
    "Is a named type file", "Remote I/O error", "Disk quota exceeded", "No medium found",
    "Wrong medium type", "Operation canceled", "Required key not available", "Key has expired",
    "Key has been revoked", "Key was rejected by service", "Owner died", "State not recoverable",
    "Operation not possible due to RF-kill", "Memory page has hardware error",
};

char* strerror(int e) {
    static char unknown[32];
    if (e >= 0 && e < (int)(sizeof(g_errmsg) / sizeof(g_errmsg[0])) && g_errmsg[e][0]) return (char*)g_errmsg[e];
    snprintf(unknown, sizeof(unknown), "Unknown error %d", e);
    return unknown;
}

/* ── ctype ────────────────────────────────────────────────────────── */

int isdigit(int c) { return c >= '0' && c <= '9'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int isalpha(int c) { return islower(c) || isupper(c); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isprint(int c) { return c >= 32 && c < 127; }
int isgraph(int c) { return c > 32 && c < 127; }
int iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
int ispunct(int c) { return isgraph(c) && !isalnum(c); }
int tolower(int c) { return isupper(c) ? c + 32 : c; }
int toupper(int c) { return islower(c) ? c - 32 : c; }
int isascii(int c) { return (unsigned)c < 128; }
int toascii(int c) { return c & 0x7f; }
int isblank(int c) { return c == ' ' || c == '\t'; }

/* ── memory ───────────────────────────────────────────────────────── */

/* malloc, free, realloc, calloc: malloc.c (the app's own heap, 64-bit) */

/* ── exit ─────────────────────────────────────────────────────────── */

void __stdio_flush_all(void);
static void (*g_atexit[16])(void);
static int g_natexit;

int atexit(void (*fn)(void)) {
    if (g_natexit == 16) return -1;
    g_atexit[g_natexit++] = fn;
    return 0;
}

void exit(int code) {
    while (g_natexit) g_atexit[--g_natexit]();
    __stdio_flush_all();
    __banana->exit(code);
    for (;;) ;
}

int raise(int sig);

void abort(void) {
    __stdio_flush_all();
    __banana->write(2, "abort()\n", 8);
    raise(6);                               /* SIGABRT: its handler first, if there is one */
    __banana->exit(134);
    for (;;) ;
}

/* ends the app at once (no stdio flush, no atexit) */
void _exit(int code) { __banana->exit(code); for (;;) ; }
void _Exit(int code) { __banana->exit(code); for (;;) ; }

void __assert_fail(const char* expr, const char* file, int line) {
    char buf[200];
    int n = 0;
    const char* parts[] = { "assertion failed: ", expr, " (", file, ")\n" };
    for (int i = 0; i < 5; i++) for (const char* s = parts[i]; *s && n < 190; s++) buf[n++] = *s;
    (void)line;
    __stdio_flush_all();
    __banana->write(2, buf, (unsigned long)n);
    __banana->exit(134);
    for (;;) ;
}

/* ── numbers ──────────────────────────────────────────────────────── */

unsigned long long strtoull(const char* s, char** end, int base) {
    const char* p = s;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '+') p++;
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) { p += 2; base = 16; }
    else if (base == 0 && p[0] == '0') base = 8;
    else if (base == 0) base = 10;
    unsigned long long v = 0;
    const char* start = p;
    for (;; p++) {
        int d;
        if (isdigit((unsigned char)*p)) d = *p - '0';
        else if (isalpha((unsigned char)*p)) d = tolower((unsigned char)*p) - 'a' + 10;
        else break;
        if (d >= base) break;
        v = v * (unsigned)base + (unsigned)d;
    }
    if (end) *end = (char*)(p == start ? s : p);
    return v;
}

long long strtoll(const char* s, char** end, int base) {
    const char* p = s;
    while (isspace((unsigned char)*p)) p++;
    int neg = *p == '-';
    if (neg) p++;
    char* e;
    unsigned long long v = strtoull(p, &e, base);
    if (e == p) { if (end) *end = (char*)s; return 0; }
    if (end) *end = e;
    return neg ? -(long long)v : (long long)v;
}

long strtol(const char* s, char** e, int b) { return (long)strtoll(s, e, b); }
unsigned long strtoul(const char* s, char** e, int b) { return (unsigned long)strtoull(s, e, b); }
int atoi(const char* s) { return (int)strtol(s, NULL, 10); }
long atol(const char* s) { return strtol(s, NULL, 10); }
long long atoll(const char* s) { return strtoll(s, NULL, 10); }
int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }
long long llabs(long long x) { return x < 0 ? -x : x; }
div_t div(int a, int b) { div_t d = { a / b, a % b }; return d; }

static unsigned int g_seed = 1;
void srand(unsigned int s) { g_seed = s; }
int rand(void) {
    g_seed = g_seed * 1103515245u + 12345u;
    return (int)((g_seed >> 1) & RAND_MAX);
}

static void swap_bytes(char* a, char* b, size_t n) {
    while (n--) { char t = *a; *a++ = *b; *b++ = t; }
}

/* shell sort: no recursion, fine for app-sized arrays */
void qsort(void* base, size_t n, size_t size, int (*cmp)(const void*, const void*)) {
    char* b = base;
    for (size_t gap = n / 2; gap > 0; gap /= 2)
        for (size_t i = gap; i < n; i++)
            for (size_t j = i; j >= gap && cmp(b + (j - gap) * size, b + j * size) > 0; j -= gap)
                swap_bytes(b + (j - gap) * size, b + j * size, size);
}

void qsort_r(void* base, size_t n, size_t size, int (*cmp)(const void*, const void*, void*), void* arg) {
    char* b = base;
    for (size_t gap = n / 2; gap > 0; gap /= 2)
        for (size_t i = gap; i < n; i++)
            for (size_t j = i; j >= gap && cmp(b + (j - gap) * size, b + j * size, arg) > 0; j -= gap)
                swap_bytes(b + (j - gap) * size, b + j * size, size);
}

void* bsearch(const void* key, const void* base, size_t n, size_t size, int (*cmp)(const void*, const void*)) {
    const char* b = base;
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = cmp(key, b + mid * size);
        if (c == 0) return (void*)(b + mid * size);
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}


/* ── time ─────────────────────────────────────────────────────────── */

static unsigned int g_start_ms;

void __time_init(void) { g_start_ms = __banana->ticks_ms(); }

static int leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static const int g_mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

/* seconds since 1970 of tm read as UTC (timegm) */
time_t __tm_to_epoch(const struct tm* tm) {
    long days = 0;
    int year = tm->tm_year + 1900;
    for (int y = 1970; y < year; y++) days += leap(y) ? 366 : 365;
    for (int m = 0; m < tm->tm_mon; m++) days += g_mdays[m] + (m == 1 && leap(year));
    days += tm->tm_mday - 1;
    return ((days * 24 + tm->tm_hour) * 60 + tm->tm_min) * 60 + tm->tm_sec;
}

/* the system's time zone (Settings > Date & time): seconds east of UTC at
 * the instant utc - 0 on a system older than API version 21 (its clock:
 * the local time) */
static int has_zone(void) {
    return __banana->size >= offsetof(banana_api_t, utc_offset) + sizeof(void*);
}
long __utc_offset(time_t utc) {
    return has_zone() ? __banana->utc_offset(utc > 0 ? (unsigned int)utc : 1u) : 0;
}

time_t mktime(struct tm* tm) {
    time_t local = __tm_to_epoch(tm);
    long off = __utc_offset(local);                     /* (a guess: the offset differs by an hour at most) */
    time_t utc = local - off;
    long off2 = __utc_offset(utc);
    if (off2 != off) utc = local - off2;
    return utc;
}

time_t time(time_t* t) {
    time_t v = 0;
    if (has_zone()) v = (time_t)__banana->time_utc();
    if (!v) {
        banana_time_t bt;
        __banana->localtime(&bt);
        struct tm tm = { bt.second, bt.minute, bt.hour, bt.day, bt.month - 1, bt.year - 1900, bt.weekday, 0, 0, 0, "UTC" };
        v = __tm_to_epoch(&tm);
    }
    if (t) *t = v;
    return v;
}

clock_t clock(void) { return (clock_t)(__banana->ticks_ms() - g_start_ms); }

struct tm* gmtime_r(const time_t* t, struct tm* out) {
    struct tm tm;
    long s = (long)*t, days = s / 86400, rem = s % 86400;
    if (rem < 0) { rem += 86400; days--; }
    tm.tm_hour = (int)(rem / 3600);
    tm.tm_min = (int)(rem % 3600 / 60);
    tm.tm_sec = (int)(rem % 60);
    tm.tm_wday = (int)((days + 4) % 7);
    int y = 1970;
    for (;;) { int yd = leap(y) ? 366 : 365; if (days < yd) break; days -= yd; y++; }
    tm.tm_year = y - 1900;
    tm.tm_yday = (int)days;
    int m = 0;
    for (;; m++) { int md = g_mdays[m] + (m == 1 && leap(y)); if (days < md) break; days -= md; }
    tm.tm_mon = m;
    tm.tm_mday = (int)days + 1;
    tm.tm_isdst = 0;
    tm.tm_gmtoff = 0;
    tm.tm_zone = "UTC";
    *out = tm;
    return out;
}

struct tm* gmtime(const time_t* t) { static struct tm tm; return gmtime_r(t, &tm); }

int snprintf(char* buf, size_t n, const char* fmt, ...);

/* "UTC+01:00" for an offset (4 kept: a few localtime() results at once) */
static const char* zone_name(long off) {
    static char names[4][12];
    static int next;
    if (!off) return "UTC";
    char* s = names[next++ & 3];
    long a = off < 0 ? -off : off;
    snprintf(s, 12, "UTC%c%02ld:%02ld", off < 0 ? '-' : '+', a / 3600, a / 60 % 60);
    return s;
}

struct tm* localtime_r(const time_t* t, struct tm* out) {
    long off = __utc_offset(*t);
    time_t l = *t + off;
    gmtime_r(&l, out);
    /* summer time: more than the year's smaller offset (January's or July's) */
    struct tm y;
    gmtime_r(t, &y);
    struct tm jan = { 0, 0, 12, 1, 0, y.tm_year, 0, 0, 0, 0, 0 }, jul = jan;
    jul.tm_mon = 6;
    long a = __utc_offset(__tm_to_epoch(&jan)), b = __utc_offset(__tm_to_epoch(&jul));
    out->tm_isdst = off > (a < b ? a : b);
    out->tm_gmtoff = off;
    out->tm_zone = zone_name(off);
    return out;
}
struct tm* localtime(const time_t* t) { static struct tm tm; return localtime_r(t, &tm); }

/* ── POSIX clocks (the system timer: milliseconds) ────────────────── */

/* The wall clock in milliseconds since 1970. The clock chip only counts
 * seconds; its seconds plus the uptime counter's milliseconds went back by
 * up to a second whenever the two did not tick together (Minecraft: "Time
 * ran backwards!"). So it is one line: an anchor taken from the clock chip
 * once, and the uptime counter from there - anchored again only when the
 * clock chip is more than 2 s away from it (set by hand, a new time zone). */
/* nanoseconds since boot: the system's counter (API 33), else its milliseconds */
unsigned long long __uptime_ns(void) {
    unsigned long long ns;
    if (BANANA_HAS(__banana, uptime_ns) && __banana->uptime_ns(&ns) == 0) return ns;
    return (unsigned long long)__banana->ticks_ms() * 1000000ull;
}

/* The wall clock in nanoseconds since 1970. The clock chip only counts
 * seconds; its seconds plus the uptime counter went back by up to a second
 * whenever the two did not tick together (Minecraft: "Time ran
 * backwards!"). So it is one line: an anchor taken from the clock chip
 * once, and the uptime counter from there - anchored again only when the
 * clock chip is more than 2 s away from it (set by hand, a new time zone).
 * One read of the counter: milliseconds and nanoseconds always agree. */
long long __realtime_ns(void) {
    static volatile long long base;               /* (ns since 1970 at uptime 0) */
    static volatile int anchored;
    long long ns = (long long)__uptime_ns();
    long long chip = (long long)time(0) * 1000000000ll;
    long long now = base + ns;
    if (!anchored || now - chip > 2000000000ll || chip - now > 2000000000ll) {
        base = chip + 500000000ll - ns;           /* (the middle of the chip's second) */
        anchored = 1;
        now = base + ns;
    }
    return now;
}

long long __realtime_ms(void) { return __realtime_ns() / 1000000ll; }

int clock_gettime(clockid_t clk, struct timespec* ts) {
    if (!ts) { errno = EFAULT; return -1; }
    unsigned ms = __banana->ticks_ms();
    switch (clk) {
    case CLOCK_REALTIME: case CLOCK_REALTIME_COARSE: {
        long long now = __realtime_ns();
        ts->tv_sec = (time_t)(now / 1000000000ll);
        ts->tv_nsec = (long)(now % 1000000000ll);
        return 0;
    }
    case CLOCK_MONOTONIC: case CLOCK_MONOTONIC_RAW: case CLOCK_MONOTONIC_COARSE: case CLOCK_BOOTTIME: {
        /* (nanoseconds: Java's System.nanoTime, frame timers - and a whole
         *  millisecond between two reads is no longer 0 or 1 ms) */
        unsigned long long ns = __uptime_ns();
        ts->tv_sec = (time_t)(ns / 1000000000ull);
        ts->tv_nsec = (long)(ns % 1000000000ull);
        return 0;
    }
    case CLOCK_PROCESS_CPUTIME_ID: case CLOCK_THREAD_CPUTIME_ID:
        ms -= g_start_ms;
        break;
    default:
        errno = EINVAL;
        return -1;
    }
    ts->tv_sec = (time_t)(ms / 1000u);
    ts->tv_nsec = (long)(ms % 1000u) * 1000000L;
    return 0;
}

int clock_getres(clockid_t clk, struct timespec* ts) {
    if (clk < 0 || clk > CLOCK_BOOTTIME) { errno = EINVAL; return -1; }
    /* (the system's counter: well under a microsecond; older systems: ms) */
    if (ts) { ts->tv_sec = 0; ts->tv_nsec = BANANA_HAS(__banana, uptime_ns) ? 1000L : 1000000L; }
    return 0;
}

extern volatile unsigned long __sig_handled;

/* the system's sleep ends early for a signal: when a handler ran for it,
 * EINTR and what was left (as Linux); else (a ^Z, then fg) it sleeps on */
int nanosleep(const struct timespec* req, struct timespec* rem) {
    if (!req || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L || req->tv_sec < 0) { errno = EINVAL; return -1; }
    unsigned long long ms = (unsigned long long)req->tv_sec * 1000ull + (unsigned long long)((req->tv_nsec + 999999L) / 1000000L);
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    if (!ms) { __banana->yield(); return 0; }
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    unsigned long long start = (unsigned long long)t0.tv_sec * 1000ull + (unsigned long long)(t0.tv_nsec / 1000000L);
    for (;;) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        unsigned long long now = (unsigned long long)t.tv_sec * 1000ull + (unsigned long long)(t.tv_nsec / 1000000L);
        unsigned long long done = now - start;
        if (done >= ms) return 0;
        unsigned long long left = ms - done;
        unsigned long h = __sig_handled;
        __banana->sleep_ms(left > 0xFFFFFFFFull ? 0xFFFFFFFFu : (unsigned)left);
        if (__sig_handled != h) {
            clock_gettime(CLOCK_MONOTONIC, &t);
            now = (unsigned long long)t.tv_sec * 1000ull + (unsigned long long)(t.tv_nsec / 1000000L);
            if (now - start >= ms) return 0;
            left = ms - (now - start);
            if (rem) { rem->tv_sec = (time_t)(left / 1000ull); rem->tv_nsec = (long)(left % 1000ull) * 1000000L; }
            errno = EINTR;
            return -1;
        }
    }
}

int clock_nanosleep(clockid_t clk, int flags, const struct timespec* req, struct timespec* rem) {
    if (!req) return EINVAL;
    struct timespec d = *req;
    if (flags & TIMER_ABSTIME) {
        struct timespec now;
        if (clock_gettime(clk, &now) != 0) return EINVAL;
        time_t s = req->tv_sec - now.tv_sec;
        long ns = req->tv_nsec - now.tv_nsec;
        if (ns < 0) { ns += 1000000000L; s--; }
        if (s < 0 || (s == 0 && ns == 0)) return 0;
        d.tv_sec = s;
        d.tv_nsec = ns;
    }
    return nanosleep(&d, rem) == 0 ? 0 : errno;
}

int snprintf(char* buf, size_t n, const char* fmt, ...);

size_t strftime(char* buf, size_t max, const char* fmt, const struct tm* tm) {
    static const char* const wd[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char* const mo[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    size_t n = 0;
    char tmp[32];
    for (; *fmt && n + 1 < max; fmt++) {
        if (*fmt != '%') { buf[n++] = *fmt; continue; }
        fmt++;
        tmp[0] = 0;
        switch (*fmt) {
        case 'Y': snprintf(tmp, sizeof(tmp), "%d", tm->tm_year + 1900); break;
        case 'm': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mon + 1); break;
        case 'd': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mday); break;
        case 'H': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_hour); break;
        case 'M': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_min); break;
        case 'S': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_sec); break;
        case 'a': snprintf(tmp, sizeof(tmp), "%s", wd[tm->tm_wday % 7]); break;
        case 'b': snprintf(tmp, sizeof(tmp), "%s", mo[tm->tm_mon % 12]); break;
        case 'z': {                                /* +0100 */
            long o = tm->tm_gmtoff, a = o < 0 ? -o : o;
            snprintf(tmp, sizeof(tmp), "%c%02ld%02ld", o < 0 ? '-' : '+', a / 3600, a / 60 % 60);
            break;
        }
        case 'Z': snprintf(tmp, sizeof(tmp), "%s", tm->tm_zone ? tm->tm_zone : "UTC"); break;
        case '%': tmp[0] = '%'; tmp[1] = 0; break;
        case 0: fmt--; break;
        default: tmp[0] = '%'; tmp[1] = *fmt; tmp[2] = 0; break;
        }
        for (char* s = tmp; *s && n + 1 < max; s++) buf[n++] = *s;
    }
    if (max) buf[n] = 0;
    return n;
}

/* ── unistd ───────────────────────────────────────────────────────── */

unsigned int sleep(unsigned int s) { __banana->sleep_ms(s * 1000); return 0; }
int usleep(unsigned long us) { __banana->sleep_ms((unsigned int)((us + 999) / 1000)); return 0; }
int __ufd_is(int fd);
ssize_t __ufd_read(int fd, void* buf, size_t n, int dontwait);
int __ufd_close(int fd);
void __ufd_forget(int fd);
/* (a socket's errors come as -errno; a file's as -1) */
ssize_t read(int fd, void* buf, size_t n) {
    if (__ufd_is(fd)) return __ufd_read(fd, buf, n, 0);
    long r = __banana->read(fd, buf, n);
    if (r < 0) { errno = r < -1 ? (int)-r : EIO; return -1; }
    return r;
}
int close(int fd) {
    if (__ufd_is(fd)) return __ufd_close(fd);
    __ufd_forget(fd);                                /* (out of epoll sets) */
    return __banana->close(fd);
}
/* getcwd(NULL, size) allocates the buffer (size 0: as long as needed), as glibc */
char* getcwd(char* buf, size_t size) {
    if (buf) {
        if (!size) { errno = EINVAL; return NULL; }
        if (__banana->getcwd(buf, (int)size) == 0) return buf;
        errno = ERANGE;
        return NULL;
    }
    char tmp[4096];
    if (__banana->getcwd(tmp, sizeof(tmp)) != 0) { errno = ERANGE; return NULL; }
    size_t n = strlen(tmp) + 1;
    if (size && size < n) { errno = ERANGE; return NULL; }
    char* r = malloc(size ? size : n);
    if (r) memcpy(r, tmp, n);
    return r;
}
int chdir(const char* path) { return __banana->chdir(path); }

static int has_v17(void) {
    return __banana->version >= 17 && BANANA_HAS(__banana, app_id);
}

pid_t getpid(void) { return has_v17() ? __banana->app_id() : 1; }
/* (getppid: proc.c) */
/* a thread's id: the app's and the thread's, so no two threads share one */
pid_t gettid(void) { return getpid() * 1000 + (__banana->version >= 3 ? __banana->thread_id() : 0); }
int getpagesize(void) { return 4096; }
uid_t getuid(void) { return 1000; }
uid_t geteuid(void) { return 1000; }
gid_t getgid(void) { return 1000; }
gid_t getegid(void) { return 1000; }

long sysconf(int name) {
    unsigned long long total = 512ull << 20, avail = 256ull << 20;
    switch (name) {
    case _SC_PAGESIZE: return 4096;
    case _SC_CLK_TCK: return 1000;
    case _SC_OPEN_MAX: return 64;
    case _SC_NPROCESSORS_CONF: case _SC_NPROCESSORS_ONLN:
        return __banana->version >= 3 ? __banana->cpu_count() : 1;
    case _SC_PHYS_PAGES: case _SC_AVPHYS_PAGES:
        if (has_v17()) __banana->mem_info(&total, &avail);
        return (long)((name == _SC_PHYS_PAGES ? total : avail) / 4096);
    case _SC_THREAD_STACK_MIN: return 16384;
    case _SC_HOST_NAME_MAX: return 64;
    case _SC_LINE_MAX: return 2048;
    case _SC_GETPW_R_SIZE_MAX: case _SC_GETGR_R_SIZE_MAX: return 1024;
    case _SC_IOV_MAX: return 1024;
    case _SC_THREAD_KEYS_MAX: return 128;
    case _SC_THREAD_THREADS_MAX: return 64;
    default: errno = EINVAL; return -1;
    }
}

/* ── floating point ───────────────────────────────────────────────── */

#include <math.h>

double strtod(const char* s, char** end) {
    const char* p = s;
    while (isspace((unsigned char)*p)) p++;
    int neg = 0;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    if (strncasecmp(p, "inf", 3) == 0) { if (end) *end = (char*)p + (strncasecmp(p, "infinity", 8) == 0 ? 8 : 3); return neg ? -INFINITY : INFINITY; }
    if (strncasecmp(p, "nan", 3) == 0) { if (end) *end = (char*)p + 3; return NAN; }
    double m = 0;
    int digits = 0, scale = 0;
    while (isdigit((unsigned char)*p)) { m = m * 10 + (*p++ - '0'); digits++; }
    if (*p == '.') {
        p++;
        while (isdigit((unsigned char)*p)) { m = m * 10 + (*p++ - '0'); scale--; digits++; }
    }
    if (!digits) { if (end) *end = (char*)s; return 0; }
    if (*p == 'e' || *p == 'E') {
        const char* q = p + 1;
        int eneg = 0, ex = 0;
        if (*q == '+' || *q == '-') eneg = *q++ == '-';
        if (isdigit((unsigned char)*q)) {
            while (isdigit((unsigned char)*q)) { if (ex < 10000) ex = ex * 10 + (*q - '0'); q++; }
            scale += eneg ? -ex : ex;
            p = q;
        }
    }
    if (end) *end = (char*)p;
    double v = scale < 0 ? m / pow(10, -scale) : m * pow(10, scale);
    return neg ? -v : v;
}

double atof(const char* s) { return strtod(s, NULL); }
float  strtof(const char* s, char** end) { return (float)strtod(s, end); }
long double strtold(const char* s, char** end) { return (long double)strtod(s, end); }
