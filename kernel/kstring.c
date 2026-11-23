/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "kstring.h"

/* ── memory ─────────────────────────────────────────────────────── */

void* memcpy(void* dst, const void* src, size_t n) {
    void* ret = dst;
    /* dword copy, then the 0-3 byte tail: several times faster than a
     * byte loop for framebuffer-sized blits */
    size_t dwords = n >> 2;
    __asm__ volatile("rep movsl"
                     : "+D"(dst), "+S"(src), "+c"(dwords) : : "memory");
    size_t tail = n & 3u;
    __asm__ volatile("rep movsb"
                     : "+D"(dst), "+S"(src), "+c"(tail) : : "memory");
    return ret;
}

void* memmove(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (d == s || n == 0) return dst;
    if (d < s || d >= s + n) return memcpy(dst, src, n);
    /* overlapping, copy backwards */
    d += n - 1;
    s += n - 1;
    __asm__ volatile("std; rep movsb; cld"
                     : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return dst;
}

void* memset(void* dst, int c, size_t n) {
    void* ret = dst;
    uint32_t v = (uint8_t)c;
    v |= v << 8;
    v |= v << 16;
    size_t dwords = n >> 2;
    __asm__ volatile("rep stosl"
                     : "+D"(dst), "+c"(dwords) : "a"(v) : "memory");
    size_t tail = n & 3u;
    __asm__ volatile("rep stosb"
                     : "+D"(dst), "+c"(tail) : "a"(v) : "memory");
    return ret;
}

void memset32(uint32_t* dst, uint32_t v, size_t count) {
    __asm__ volatile("rep stosl"
                     : "+D"(dst), "+c"(count) : "a"(v) : "memory");
}

int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

/* ── strings ────────────────────────────────────────────────────── */

size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

int k_tolower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

int strcasecmp(const char* a, const char* b) {
    while (*a && k_tolower((unsigned char)*a) == k_tolower((unsigned char)*b)) { a++; b++; }
    return k_tolower((unsigned char)*a) - k_tolower((unsigned char)*b);
}

int strncasecmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int ca = k_tolower((unsigned char)a[i]), cb = k_tolower((unsigned char)b[i]);
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
    }
    return 0;
}

char* strchr(const char* s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char*)s;
        if (!*s) return NULL;
    }
}

char* strrchr(const char* s, int c) {
    const char* last = NULL;
    for (;; s++) {
        if (*s == (char)c) last = s;
        if (!*s) return (char*)last;
    }
}

char* strstr(const char* hay, const char* needle) {
    if (!*needle) return (char*)hay;
    for (; *hay; hay++) {
        size_t j = 0;
        while (needle[j] && hay[j] == needle[j]) j++;
        if (!needle[j]) return (char*)hay;
    }
    return NULL;
}

size_t kstrlcpy(char* dst, const char* src, size_t n) {
    size_t len = strlen(src);
    if (n) {
        size_t c = (len >= n) ? n - 1 : len;
        memcpy(dst, src, c);
        dst[c] = '\0';
    }
    return len;
}

size_t kstrlcat(char* dst, const char* src, size_t n) {
    size_t dl = 0;
    while (dl < n && dst[dl]) dl++;
    if (dl == n) return n + strlen(src);
    return dl + kstrlcpy(dst + dl, src, n - dl);
}

int k_isdigit(int c) { return c >= '0' && c <= '9'; }
int k_isspace(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

int k_parse_u32(const char* s, uint32_t* out) {
    int i = 0;
    uint32_t v = 0;
    while (k_isdigit((unsigned char)s[i])) {
        v = v * 10u + (uint32_t)(s[i] - '0');
        i++;
    }
    if (out) *out = v;
    return i;
}

/* ── ksnprintf ──────────────────────────────────────────────────── */

typedef struct {
    char*  buf;
    size_t cap;
    size_t len;  /* total that would have been written */
} outbuf_t;

static void ob_putc(outbuf_t* o, char c) {
    if (o->len + 1 < o->cap) o->buf[o->len] = c;
    o->len++;
}

static void ob_pad(outbuf_t* o, char c, int count) {
    while (count-- > 0) ob_putc(o, c);
}

static void ob_num(outbuf_t* o, uint64_t v, int base, int upper, int neg,
                   int width, int zero, int left) {
    char tmp[24];
    int n = 0;
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0) tmp[n++] = '0';
    /* base is 8, 10 or 16: shifts for 8 and 16, no 64-bit divide */
    while (v) {
        if (base == 16) { tmp[n++] = digits[v & 0xF]; v >>= 4; }
        else if (base == 8) { tmp[n++] = digits[v & 7]; v >>= 3; }
        else {
            /* 64-bit /10 without libgcc when the value fits in 32 bits */
            if (v <= 0xFFFFFFFFull) {
                uint32_t w = (uint32_t)v;
                tmp[n++] = digits[w % 10u];
                v = w / 10u;
            } else {
                tmp[n++] = digits[v % 10u];
                v /= 10u;
            }
        }
    }
    int total = n + (neg ? 1 : 0);
    if (!left && !zero) ob_pad(o, ' ', width - total);
    if (neg) ob_putc(o, '-');
    if (!left && zero) ob_pad(o, '0', width - total);
    while (n) ob_putc(o, tmp[--n]);
    if (left) ob_pad(o, ' ', width - total);
}

/* A conversion's argument size from its length modifier: none / h / hh
 * (int), l (long: 32 bits on i386, 64 on x86_64), ll / j (64 bits),
 * z / t (size_t, ptrdiff_t: the width of a pointer). */
enum { ARG_INT, ARG_LONG, ARG_64, ARG_PTR };

static uint64_t arg_unsigned(__builtin_va_list* ap, int size) {
    switch (size) {
    case ARG_LONG: return (uint64_t)__builtin_va_arg(*ap, unsigned long);
    case ARG_64:   return (uint64_t)__builtin_va_arg(*ap, unsigned long long);
    case ARG_PTR:  return (uint64_t)__builtin_va_arg(*ap, size_t);
    default:       return (uint64_t)__builtin_va_arg(*ap, unsigned int);
    }
}

static int64_t arg_signed(__builtin_va_list* ap, int size) {
    switch (size) {
    case ARG_LONG: return (int64_t)__builtin_va_arg(*ap, long);
    case ARG_64:   return (int64_t)__builtin_va_arg(*ap, long long);
    case ARG_PTR:  return (int64_t)__builtin_va_arg(*ap, intptr_t);
    default:       return (int64_t)__builtin_va_arg(*ap, int);
    }
}

int kvsnprintf(char* buf, size_t n, const char* fmt, __builtin_va_list ap_in) {
    outbuf_t o = { buf, n, 0 };
    /* (a copy: va_list is an array on x86_64, and the helpers take its address) */
    __builtin_va_list ap;
    __builtin_va_copy(ap, ap_in);
    for (const char* p = fmt; *p; p++) {
        if (*p != '%') { ob_putc(&o, *p); continue; }
        p++;
        int left = 0, zero = 0, width = 0, prec = -1, size = ARG_INT;
        for (;; p++) {
            if (*p == '-') left = 1;
            else if (*p == '0') zero = 1;
            else if (*p == ' ' || *p == '+' || *p == '#') { /* (accepted, not used) */ }
            else break;
        }
        if (*p == '*') {                             /* width from an argument */
            width = __builtin_va_arg(ap, int);
            if (width < 0) { left = 1; width = -width; }
            p++;
        } else {
            while (k_isdigit((unsigned char)*p)) width = width * 10 + (*p++ - '0');
        }
        if (*p == '.') {                             /* precision: %.3s, %.*s */
            p++;
            prec = 0;
            if (*p == '*') {
                prec = __builtin_va_arg(ap, int);
                if (prec < 0) prec = -1;
                p++;
            } else {
                while (k_isdigit((unsigned char)*p)) prec = prec * 10 + (*p++ - '0');
            }
        }
        for (;; p++) {                               /* length modifiers */
            if (*p == 'l') size = size == ARG_LONG ? ARG_64 : ARG_LONG;
            else if (*p == 'j' || *p == 'q' || *p == 'L') size = ARG_64;
            else if (*p == 'z' || *p == 't') size = ARG_PTR;
            else if (*p != 'h') break;
        }
        switch (*p) {
        case 'd': case 'i': {
            int64_t v = arg_signed(&ap, size);
            int neg = v < 0;
            ob_num(&o, neg ? 0 - (uint64_t)v : (uint64_t)v, 10, 0, neg, width, zero, left);
            break;
        }
        case 'u':
            ob_num(&o, arg_unsigned(&ap, size), 10, 0, 0, width, zero, left);
            break;
        case 'x': case 'X':
            ob_num(&o, arg_unsigned(&ap, size), 16, *p == 'X', 0, width, zero, left);
            break;
        case 'o':                                    /* (modes: chmod, stat) */
            ob_num(&o, arg_unsigned(&ap, size), 8, 0, 0, width, zero, left);
            break;
        case 'p': {
            uint64_t v = (uint64_t)(uintptr_t)__builtin_va_arg(ap, void*);   /* (all of it: app memory lies past 4 GiB) */
            ob_putc(&o, '0'); ob_putc(&o, 'x');
            ob_num(&o, v, 16, 0, 0, 8, 1, 0);
            break;
        }
        case 's': {
            const char* s = __builtin_va_arg(ap, const char*);
            if (!s) s = "(null)";
            int l = 0;                               /* (never reads past the precision) */
            while (s[l] && (prec < 0 || l < prec)) l++;
            if (!left) ob_pad(&o, ' ', width - l);
            for (int i = 0; i < l; i++) ob_putc(&o, s[i]);
            if (left) ob_pad(&o, ' ', width - l);
            break;
        }
        case 'c':
            if (!left) ob_pad(&o, ' ', width - 1);
            ob_putc(&o, (char)__builtin_va_arg(ap, int));
            if (left) ob_pad(&o, ' ', width - 1);
            break;
        case '%':
            ob_putc(&o, '%');
            break;
        case '\0':
            p--;
            break;
        default:
            ob_putc(&o, '%');
            ob_putc(&o, *p);
            break;
        }
    }
    __builtin_va_end(ap);
    if (n) buf[(o.len < n) ? o.len : n - 1] = '\0';
    return (int)o.len;
}

int ksnprintf(char* buf, size_t n, const char* fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int r = kvsnprintf(buf, n, fmt, ap);
    __builtin_va_end(ap);
    return r;
}
