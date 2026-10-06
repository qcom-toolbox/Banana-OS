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
int errno;

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
char* strerror(int e) {
    switch (e) {
    case 0: return "success";
    case ENOENT: return "no such file or folder";
    case EIO: return "input/output error";
    case EBADF: return "bad file";
    case ENOMEM: return "out of memory";
    case EEXIST: return "already exists";
    case EISDIR: return "is a folder";
    case EINVAL: return "invalid argument";
    case ENOSPC: return "no space left";
    case ENOSYS: return "not supported";
    }
    return "error";
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

/* ── memory ───────────────────────────────────────────────────────── */

void* malloc(size_t n) { void* p = __banana->malloc(n ? n : 1); if (!p) errno = ENOMEM; return p; }
void  free(void* p) { if (p) __banana->free(p); }
void* realloc(void* p, size_t n) { return __banana->realloc(p, n); }
void* calloc(size_t n, size_t s) {
    if (s && n > (size_t)-1 / s) return NULL;
    void* p = malloc(n * s);
    if (p) memset(p, 0, n * s);
    return p;
}

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

void abort(void) {
    __stdio_flush_all();
    __banana->write(2, "abort()\n", 8);
    __banana->exit(134);
    for (;;) ;
}

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

char* getenv(const char* name) {
    if (strcmp(name, "HOME") == 0) return "/home/banana";
    if (strcmp(name, "USER") == 0) return "banana";
    return NULL;
}

/* ── time ─────────────────────────────────────────────────────────── */

static unsigned int g_start_ms;

void __time_init(void) { g_start_ms = __banana->ticks_ms(); }

static int leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static const int g_mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

time_t mktime(struct tm* tm) {
    long days = 0;
    int year = tm->tm_year + 1900;
    for (int y = 1970; y < year; y++) days += leap(y) ? 366 : 365;
    for (int m = 0; m < tm->tm_mon; m++) days += g_mdays[m] + (m == 1 && leap(year));
    days += tm->tm_mday - 1;
    return ((days * 24 + tm->tm_hour) * 60 + tm->tm_min) * 60 + tm->tm_sec;
}

time_t time(time_t* t) {
    banana_time_t bt;
    __banana->localtime(&bt);
    struct tm tm = { bt.second, bt.minute, bt.hour, bt.day, bt.month - 1, bt.year - 1900, bt.weekday, 0, 0 };
    time_t v = mktime(&tm);
    if (t) *t = v;
    return v;
}

clock_t clock(void) { return (clock_t)(__banana->ticks_ms() - g_start_ms); }

struct tm* gmtime(const time_t* t) {
    static struct tm tm;
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
    return &tm;
}

struct tm* localtime(const time_t* t) { return gmtime(t); }   /* the clock chip keeps local time */

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
ssize_t read(int fd, void* buf, size_t n) { return __banana->read(fd, buf, n); }
int close(int fd) { return __banana->close(fd); }
char* getcwd(char* buf, size_t size) { return __banana->getcwd(buf, (int)size) == 0 ? buf : NULL; }
int chdir(const char* path) { return __banana->chdir(path); }
int unlink(const char* path) { return __banana->remove(path); }
int rmdir(const char* path) { return __banana->remove(path); }

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
