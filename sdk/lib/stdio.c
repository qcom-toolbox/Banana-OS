/* Banana OS SDK stdio: printf/scanf family, FILE streams over the
 * system's file calls, and the terminal as stdin/stdout/stderr. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <math.h>
#include "banana_api.h"

extern const banana_api_t* __banana;
void __time_init(void);

struct _bfile {
    int  fd;
    int  eof, err;
    int  ungot;               /* -1: none */
    int  is_term;
    char buf[BUFSIZ];         /* terminal output: line buffered */
    int  n;
};

static struct _bfile g_std[3];
FILE* stdin  = &g_std[0];
FILE* stdout = &g_std[1];
FILE* stderr = &g_std[2];

/* stdin line buffer */
static char g_line[1024];
static int  g_line_pos, g_line_len;

void __libc_init(void) {
    for (int i = 0; i < 3; i++) {
        g_std[i].fd = i;
        g_std[i].ungot = -1;
        g_std[i].is_term = 1;
    }
    __time_init();
}

int fflush(FILE* f) {
    if (!f) { fflush(stdout); fflush(stderr); return 0; }
    if (f->is_term && f->n) {
        __banana->write(f->fd, f->buf, (unsigned long)f->n);
        f->n = 0;
    }
    return 0;
}

void __stdio_flush_all(void) { fflush(NULL); }

ssize_t write(int fd, const void* buf, size_t n) {
    if (fd == 1) fflush(stdout);
    if (fd == 2) fflush(stderr);
    return __banana->write(fd, buf, n);
}

static int put_bytes(FILE* f, const char* s, size_t n) {
    if (!f) return -1;
    if (!f->is_term) {
        long w = __banana->write(f->fd, s, n);
        if (w != (long)n) { f->err = 1; return -1; }
        return (int)n;
    }
    for (size_t i = 0; i < n; i++) {
        f->buf[f->n++] = s[i];
        if (f->n == BUFSIZ || s[i] == '\n' || f == stderr) fflush(f);
    }
    if (f == stderr) fflush(f);
    return (int)n;
}

/* ── formatting ───────────────────────────────────────────────────── */

typedef struct { char* buf; size_t cap, n; FILE* f; } out_t;

static void out_c(out_t* o, char c) {
    if (o->f) { put_bytes(o->f, &c, 1); o->n++; return; }
    if (o->n + 1 < o->cap) o->buf[o->n] = c;
    o->n++;
}

static void out_pad(out_t* o, char c, int n) { while (n-- > 0) out_c(o, c); }

/* ── floating point: %f %e %g ─────────────────────────────────────── */

/* the decimal digits of the whole number w (w >= 0, < 1e308) into d[], most significant first */
static int whole_digits(double w, char* d, int cap) {
    char tmp[320];
    int n = 0;
    if (w < 1) { d[0] = '0'; return 1; }
    while (w >= 1 && n < (int)sizeof(tmp)) {
        double q = floor(w / 10);
        int digit = (int)(w - q * 10);
        if (digit < 0) digit = 0;
        if (digit > 9) digit = 9;
        tmp[n++] = (char)('0' + digit);
        w = q;
    }
    int k = 0;
    while (n && k < cap) d[k++] = tmp[--n];
    return k;
}

/* v with `prec` digits after the point into s (no exponent) */
static int fixed(char* s, int cap, double v, int prec, int alt) {
    double scale = pow(10, prec);
    double r = floor(v * scale + 0.5);             /* rounded, in units of 10^-prec */
    double w = floor(r / scale);
    double f = r - w * scale;
    int n = whole_digits(w, s, cap - prec - 2);
    if (prec > 0 || alt) s[n++] = '.';
    char fd[64];
    int fn = whole_digits(f, fd, sizeof(fd));
    for (int i = fn; i < prec; i++) s[n++] = '0';  /* leading zeros of the fraction */
    for (int i = 0; i < fn && fn <= prec; i++) s[n++] = fd[i];
    return n;
}

static int fmt_double(char* s, int cap, double v, int prec, char conv, int alt, int plus, int space) {
    int n = 0;
    if (v < 0 || (v == 0 && 1 / v < 0)) { s[n++] = '-'; v = -v; }
    else if (plus) s[n++] = '+';
    else if (space) s[n++] = ' ';
    int upper = conv == 'F' || conv == 'E' || conv == 'G';
    if (isnan(v) || isinf(v)) {
        const char* t = isnan(v) ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
        while (*t) s[n++] = *t++;
        return n;
    }
    if (prec < 0) prec = 6;
    if (prec > 60) prec = 60;
    char c = conv | 32;                            /* f, e or g */
    int exp10 = 0;
    if (v != 0) {
        exp10 = (int)floor(log10(v));
        /* log10 can be one off near powers of ten */
        if (v / pow(10, exp10) >= 10) exp10++;
        if (v / pow(10, exp10) < 1) exp10--;
    }
    int strip = 0;
    if (c == 'g') {
        int p = prec ? prec : 1;
        /* rounding can carry into the next power of ten */
        double rounded = floor(v / pow(10, exp10 - p + 1) + 0.5) * pow(10, exp10 - p + 1);
        if (rounded != 0 && rounded >= pow(10, exp10 + 1)) exp10++;
        if (exp10 < -4 || exp10 >= p) { c = 'e'; prec = p - 1; }
        else { c = 'f'; prec = p - 1 - exp10; }
        strip = !alt;
    }
    if (c == 'f') {
        n += fixed(s + n, cap - n, v, prec, alt);
    } else {
        double m = v == 0 ? 0 : v / pow(10, exp10);
        if (floor(m * pow(10, prec) + 0.5) >= 10 * pow(10, prec)) { m /= 10; exp10++; }
        n += fixed(s + n, cap - n - 6, m, prec, alt);
    }
    if (strip && memchr(s, '.', (size_t)n)) {
        while (n > 0 && s[n - 1] == '0') n--;
        if (n > 0 && s[n - 1] == '.') n--;
    }
    if (c == 'e') {
        s[n++] = upper ? 'E' : 'e';
        s[n++] = exp10 < 0 ? '-' : '+';
        int ex = exp10 < 0 ? -exp10 : exp10;
        if (ex >= 100) s[n++] = (char)('0' + ex / 100);
        s[n++] = (char)('0' + ex / 10 % 10);
        s[n++] = (char)('0' + ex % 10);
    }
    return n;
}

static int vfmt(out_t* o, const char* fmt, va_list ap) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') { out_c(o, *fmt); continue; }
        fmt++;
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') zero = 1;
            else if (*fmt == '+') plus = 1;
            else if (*fmt == ' ') space = 1;
            else if (*fmt == '#') alt = 1;
            else break;
        }
        int width = 0, prec = -1;
        if (*fmt == '*') { width = va_arg(ap, int); if (width < 0) { left = 1; width = -width; } fmt++; }
        else while (isdigit((unsigned char)*fmt)) width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (isdigit((unsigned char)*fmt)) prec = prec * 10 + (*fmt++ - '0');
        }
        int lng = 0;                      /* 1 long, 2 long long, 3 size_t */
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z' || *fmt == 'j' || *fmt == 't') {
            if (*fmt == 'l') lng++;
            else if (*fmt == 'z' || *fmt == 't') lng = 3;
            else if (*fmt == 'j') lng = 2;
            fmt++;
        }
        char c = *fmt;
        if (!c) break;
        if (c == '%') { out_c(o, '%'); continue; }
        if (c == 'c') {
            if (!left) out_pad(o, ' ', width - 1);
            out_c(o, (char)va_arg(ap, int));
            if (left) out_pad(o, ' ', width - 1);
            continue;
        }
        if (c == 's') {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            int l = (int)strlen(s);
            if (prec >= 0 && l > prec) l = prec;
            if (!left) out_pad(o, ' ', width - l);
            for (int i = 0; i < l; i++) out_c(o, s[i]);
            if (left) out_pad(o, ' ', width - l);
            continue;
        }
        if (c == 'n') { int* p = va_arg(ap, int*); if (p) *p = (int)o->n; continue; }
        if (c == 'f' || c == 'F' || c == 'e' || c == 'E' || c == 'g' || c == 'G') {
            char num[400];
            int n = fmt_double(num, sizeof(num), va_arg(ap, double), prec, c, alt, plus, space);
            int pad = width - n;
            if (zero && !left && pad > 0 && (num[0] == '-' || num[0] == '+' || num[0] == ' ')) {
                out_c(o, num[0]);
                out_pad(o, '0', pad);
                for (int i = 1; i < n; i++) out_c(o, num[i]);
                continue;
            }
            if (!left) out_pad(o, zero ? '0' : ' ', pad);
            for (int i = 0; i < n; i++) out_c(o, num[i]);
            if (left) out_pad(o, ' ', pad);
            continue;
        }
        /* integers */
        unsigned long long v;
        int neg = 0, base = 10, upper = 0;
        if (c == 'd' || c == 'i') {
            long long s;
            if (lng == 2) s = va_arg(ap, long long);
            else if (lng == 1) s = va_arg(ap, long);
            else if (lng == 3) s = (long long)va_arg(ap, ptrdiff_t);
            else s = va_arg(ap, int);
            neg = s < 0;
            v = neg ? (unsigned long long)(-(s + 1)) + 1 : (unsigned long long)s;
        } else {
            if (c == 'p') { v = (uintptr_t)va_arg(ap, void*); base = 16; alt = 1; }
            else if (lng == 2) v = va_arg(ap, unsigned long long);
            else if (lng == 1) v = va_arg(ap, unsigned long);
            else if (lng == 3) v = va_arg(ap, size_t);
            else v = va_arg(ap, unsigned int);
            if (c == 'x') base = 16;
            else if (c == 'X') { base = 16; upper = 1; }
            else if (c == 'o') base = 8;
            else if (c == 'b') base = 2;
            else if (c != 'u' && c != 'p') { out_c(o, '%'); out_c(o, c); continue; }
        }
        char digits[72];
        int nd = 0;
        do {
            int d = (int)(v % (unsigned)base);
            digits[nd++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
            v /= (unsigned)base;
        } while (v);
        if (prec == 0 && nd == 1 && digits[0] == '0') nd = 0;
        int zeros = prec > nd ? prec - nd : 0;
        char sign = neg ? '-' : plus ? '+' : space ? ' ' : 0;
        const char* prefix = (alt && base == 16) ? (upper ? "0X" : "0x") : (alt && base == 8) ? "0" : "";
        int total = nd + zeros + (sign ? 1 : 0) + (int)strlen(prefix);
        if (zero && !left && prec < 0) { zeros += width - total; total = width; }
        if (!left) out_pad(o, ' ', width - total);
        if (sign) out_c(o, sign);
        for (const char* p = prefix; *p; p++) out_c(o, *p);
        out_pad(o, '0', zeros);
        while (nd) out_c(o, digits[--nd]);
        if (left) out_pad(o, ' ', width - total);
    }
    if (!o->f && o->cap) o->buf[o->n < o->cap ? o->n : o->cap - 1] = 0;
    return (int)o->n;
}

int vsnprintf(char* buf, size_t n, const char* fmt, va_list ap) { out_t o = { buf, n, 0, NULL }; return vfmt(&o, fmt, ap); }
int vsprintf(char* buf, const char* fmt, va_list ap) { return vsnprintf(buf, (size_t)-1 / 2, fmt, ap); }
int vfprintf(FILE* f, const char* fmt, va_list ap) { out_t o = { NULL, 0, 0, f }; return vfmt(&o, fmt, ap); }
int vprintf(const char* fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }
int snprintf(char* b, size_t n, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsnprintf(b, n, fmt, ap); va_end(ap); return r; }
int sprintf(char* b, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsprintf(b, fmt, ap); va_end(ap); return r; }
int fprintf(FILE* f, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfprintf(f, fmt, ap); va_end(ap); return r; }
int printf(const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfprintf(stdout, fmt, ap); va_end(ap); return r; }

/* ── characters and lines ─────────────────────────────────────────── */

int fputc(int c, FILE* f) { char ch = (char)c; return put_bytes(f, &ch, 1) == 1 ? (unsigned char)c : EOF; }
int putc(int c, FILE* f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, stdout); }
int fputs(const char* s, FILE* f) { return put_bytes(f, s, strlen(s)) < 0 ? EOF : 0; }
int puts(const char* s) { if (fputs(s, stdout) < 0) return EOF; return putchar('\n') == EOF ? EOF : 0; }

/* stdin: the terminal gives whole lines (with echo and backspace) */
static int stdin_getc(void) {
    if (g_line_pos >= g_line_len) {
        fflush(stdout);
        int n = __banana->readline(g_line, (int)sizeof(g_line) - 1);
        if (n < 0) { stdin->eof = 1; return EOF; }
        g_line[n] = '\n';
        g_line_len = n + 1;
        g_line_pos = 0;
    }
    return (unsigned char)g_line[g_line_pos++];
}

int fgetc(FILE* f) {
    if (!f) return EOF;
    if (f->ungot >= 0) { int c = f->ungot; f->ungot = -1; return c; }
    if (f == stdin) return stdin_getc();
    if (f->is_term) return EOF;
    unsigned char c;
    long n = __banana->read(f->fd, &c, 1);
    if (n <= 0) { f->eof = 1; return EOF; }
    return c;
}
int getc(FILE* f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }
int ungetc(int c, FILE* f) { if (c == EOF || !f) return EOF; f->ungot = c & 255; f->eof = 0; return c; }

char* fgets(char* buf, int n, FILE* f) {
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) break;
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return NULL;
    buf[i] = 0;
    return buf;
}

/* ── files ────────────────────────────────────────────────────────── */

FILE* fopen(const char* path, const char* mode) {
    int flags = 0;
    if (mode[0] == 'r') flags = BANANA_O_READ;
    else if (mode[0] == 'w') flags = BANANA_O_WRITE | BANANA_O_CREATE | BANANA_O_TRUNC;
    else if (mode[0] == 'a') flags = BANANA_O_WRITE | BANANA_O_CREATE | BANANA_O_APPEND;
    else { errno = EINVAL; return NULL; }
    if (strchr(mode, '+')) flags |= BANANA_O_READ | BANANA_O_WRITE;
    int fd = __banana->open(path, flags);
    if (fd < 0) { errno = ENOENT; return NULL; }
    FILE* f = calloc(1, sizeof(FILE));
    if (!f) { __banana->close(fd); return NULL; }
    f->fd = fd;
    f->ungot = -1;
    return f;
}

int fclose(FILE* f) {
    if (!f) return EOF;
    if (f->is_term) { fflush(f); return 0; }
    __banana->close(f->fd);
    free(f);
    return 0;
}

size_t fread(void* buf, size_t size, size_t n, FILE* f) {
    if (!f || !size) return 0;
    size_t want = size * n, got = 0;
    unsigned char* b = buf;
    if (f->ungot >= 0 && want) { b[got++] = (unsigned char)f->ungot; f->ungot = -1; }
    if (f == stdin) {
        while (got < want) { int c = stdin_getc(); if (c == EOF) break; b[got++] = (unsigned char)c; }
    } else if (!f->is_term) {
        while (got < want) {
            long r = __banana->read(f->fd, b + got, want - got);
            if (r <= 0) { f->eof = 1; break; }
            got += (size_t)r;
        }
    }
    return got / size;
}

size_t fwrite(const void* buf, size_t size, size_t n, FILE* f) {
    if (!size || !n) return 0;
    return put_bytes(f, buf, size * n) < 0 ? 0 : n;
}

int fseek(FILE* f, long off, int whence) {
    if (!f || f->is_term) return -1;
    f->ungot = -1;
    f->eof = 0;
    return __banana->seek(f->fd, off, whence) < 0 ? -1 : 0;
}
long ftell(FILE* f) { return (!f || f->is_term) ? -1 : __banana->seek(f->fd, 0, SEEK_CUR); }
void rewind(FILE* f) { fseek(f, 0, SEEK_SET); if (f) f->err = 0; }
int feof(FILE* f) { return f ? f->eof : 1; }
int ferror(FILE* f) { return f ? f->err : 1; }
void clearerr(FILE* f) { if (f) f->eof = f->err = 0; }
int remove(const char* path) { return __banana->remove(path); }
int rename(const char* a, const char* b) { return __banana->rename(a, b); }
void perror(const char* s) {
    if (s && *s) fprintf(stderr, "%s: %s\n", s, strerror(errno));
    else fprintf(stderr, "%s\n", strerror(errno));
}

/* ── scanf (integers, strings, characters, %[...] not included) ───── */

typedef struct { const char* s; FILE* f; int peek; } in_t;

static int in_get(in_t* in) {
    if (in->peek != -2) { int c = in->peek; in->peek = -2; return c; }
    if (in->s) return *in->s ? (unsigned char)*in->s++ : EOF;
    return fgetc(in->f);
}
static void in_unget(in_t* in, int c) { in->peek = c; }

static int vscan(in_t* in, const char* fmt, va_list ap) {
    int count = 0;
    for (; *fmt; fmt++) {
        if (isspace((unsigned char)*fmt)) {
            int c;
            while ((c = in_get(in)) != EOF && isspace(c)) ;
            in_unget(in, c);
            continue;
        }
        if (*fmt != '%') {
            int c = in_get(in);
            if (c != (unsigned char)*fmt) { in_unget(in, c); break; }
            continue;
        }
        fmt++;
        int skip = 0, width = 0, lng = 0;
        if (*fmt == '*') { skip = 1; fmt++; }
        while (isdigit((unsigned char)*fmt)) width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') { if (*fmt == 'l') lng++; fmt++; }
        char conv = *fmt;
        if (conv == '%') { int c = in_get(in); if (c != '%') break; continue; }
        int c;
        if (conv != 'c') { while ((c = in_get(in)) != EOF && isspace(c)) ; in_unget(in, c); }
        if (conv == 'c') {
            int n = width ? width : 1;
            char* out = skip ? NULL : va_arg(ap, char*);
            for (int i = 0; i < n; i++) {
                c = in_get(in);
                if (c == EOF) return count ? count : EOF;
                if (out) out[i] = (char)c;
            }
            if (!skip) count++;
        } else if (conv == 's') {
            char* out = skip ? NULL : va_arg(ap, char*);
            int n = 0;
            while ((c = in_get(in)) != EOF && !isspace(c) && (!width || n < width)) { if (out) out[n] = (char)c; n++; }
            in_unget(in, c);
            if (!n) return count ? count : EOF;
            if (out) { out[n] = 0; count++; }
        } else if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
            char num[40];
            int n = 0;
            int base = conv == 'x' || conv == 'X' ? 16 : conv == 'o' ? 8 : conv == 'i' ? 0 : 10;
            c = in_get(in);
            if (c == '-' || c == '+') { num[n++] = (char)c; c = in_get(in); }
            while (c != EOF && n < 38 && (!width || n < width) &&
                   (isdigit(c) || (base != 10 && isxdigit(c)) || ((c == 'x' || c == 'X') && n <= 2))) {
                num[n++] = (char)c;
                c = in_get(in);
            }
            in_unget(in, c);
            num[n] = 0;
            char* end;
            long long v = strtoll(num, &end, base);
            if (end == num) return count ? count : (c == EOF ? EOF : 0);
            if (!skip) {
                if (lng >= 2) *va_arg(ap, long long*) = v;
                else if (lng == 1) *va_arg(ap, long*) = (long)v;
                else *va_arg(ap, int*) = (int)v;
                count++;
            }
        } else if (conv == 'f' || conv == 'e' || conv == 'g' || conv == 'E' || conv == 'G') {
            char num[64];
            int n = 0;
            c = in_get(in);
            while (c != EOF && n < 62 && (!width || n < width) &&
                   (isdigit(c) || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E')) {
                num[n++] = (char)c;
                c = in_get(in);
            }
            in_unget(in, c);
            num[n] = 0;
            char* end;
            double v = strtod(num, &end);
            if (end == num) return count ? count : (c == EOF ? EOF : 0);
            if (!skip) {
                if (lng) *va_arg(ap, double*) = v;
                else *va_arg(ap, float*) = (float)v;
                count++;
            }
        } else {
            break;
        }
    }
    return count;
}

int sscanf(const char* s, const char* fmt, ...) {
    in_t in = { s, NULL, -2 };
    va_list ap;
    va_start(ap, fmt);
    int r = vscan(&in, fmt, ap);
    va_end(ap);
    return r;
}

int scanf(const char* fmt, ...) {
    in_t in = { NULL, stdin, -2 };
    va_list ap;
    va_start(ap, fmt);
    int r = vscan(&in, fmt, ap);
    va_end(ap);
    if (in.peek >= 0) ungetc(in.peek, stdin);
    return r;
}
