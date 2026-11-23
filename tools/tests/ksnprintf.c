/* SPDX-License-Identifier: GPL-3.0-or-later */
/* kernel/kstring.c's ksnprintf against the C library's snprintf: every
 * conversion the kernel uses, with widths, precisions (%.3s, %.*s) and
 * length modifiers (%lx with 64-bit values, %zu), and that arguments after
 * one of them are still read from the right place. kstring.o's symbols
 * carry a kt_ prefix (objcopy) so they do not replace the C library's. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int kt_ksnprintf(char* buf, size_t n, const char* fmt, ...);
void* kt_memcpy(void* d, const void* s, size_t n);
void* kt_memmove(void* d, const void* s, size_t n);
void* kt_memset(void* d, int c, size_t n);
int kt_memcmp(const void* a, const void* b, size_t n);

static int failed, passed;

#define CHECK(...) do { \
    char want[256], got[256]; \
    memset(got, 'Z', sizeof(got)); \
    int wn = snprintf(want, sizeof(want), __VA_ARGS__); \
    int gn = kt_ksnprintf(got, sizeof(got), __VA_ARGS__); \
    if (wn != gn || strcmp(want, got)) { \
        printf("FAIL  %s: \"%s\" (%d), want \"%s\" (%d)\n", #__VA_ARGS__, got, gn, want, wn); failed++; \
    } else passed++; \
} while (0)

static void truncation(void) {
    char b[8];
    memset(b, 'Z', sizeof(b));
    int n = kt_ksnprintf(b, 5, "%s-%d", "abcdef", 42);
    if (n != 9 || strcmp(b, "abcd") || b[5] != 'Z') { printf("FAIL  truncation: \"%s\" %d\n", b, n); failed++; }
    else passed++;
    /* the precision bounds the read: a buffer with no terminator */
    char raw[3] = { 'x', 'y', 'z' };
    n = kt_ksnprintf(b, sizeof(b), "%.3s|", raw);
    if (n != 4 || strcmp(b, "xyz|")) { printf("FAIL  unterminated %%.3s: \"%s\"\n", b); failed++; }
    else passed++;
}

static void memory(void) {
    unsigned char a[300], b[300], c[300];
    for (int len = 0; len < 260; len++)
        for (int off = 0; off < 9; off++) {
            for (int i = 0; i < 300; i++) { a[i] = (unsigned char)(i * 7 + 3); b[i] = c[i] = 0xEE; }
            kt_memcpy(b + off, a + 1, (size_t)len);
            memcpy(c + off, a + 1, (size_t)len);
            if (memcmp(b, c, 300)) { printf("FAIL  memcpy len %d off %d\n", len, off); failed++; return; }
            kt_memset(b + off, 0x5A, (size_t)len);
            memset(c + off, 0x5A, (size_t)len);
            if (memcmp(b, c, 300)) { printf("FAIL  memset len %d off %d\n", len, off); failed++; return; }
            for (int i = 0; i < 300; i++) b[i] = c[i] = (unsigned char)i;
            kt_memmove(b + off, b + 4, (size_t)len);       /* overlapping both ways */
            memmove(c + off, c + 4, (size_t)len);
            kt_memmove(b + 4, b + off, (size_t)len);
            memmove(c + 4, c + off, (size_t)len);
            if (memcmp(b, c, 300)) { printf("FAIL  memmove len %d off %d\n", len, off); failed++; return; }
            memcpy(b, a, 300); memcpy(c, a, 300);
            if (len) c[off + len - 1] ^= 0x80;
            int w = memcmp(b + off, c + off, (size_t)len), g = kt_memcmp(b + off, c + off, (size_t)len);
            if ((w < 0) != (g < 0) || (w > 0) != (g > 0)) { printf("FAIL  memcmp len %d off %d\n", len, off); failed++; return; }
        }
    passed++;
}

int main(void) {
    CHECK("plain");
    CHECK("%d %i %u %x %X %o %%", -42, 7, 3000000000u, 0xbeefu, 0xbeefu, 0755);
    CHECK("[%5d] [%-5d] [%05d] [%-05d]", 42, 42, -42, 7);
    CHECK("%lld %llu %llx", -1234567890123LL, 18446744073709551615ULL, 0x123456789abcULL);
    CHECK("%lx %lu %ld", (unsigned long)0x7fff12345678UL, (unsigned long)~0UL, -5L);
    CHECK("%zu %zx %jd", (size_t)123456789, (size_t)0xabcdef, (intmax_t)-99);
    CHECK("%hu %hhu %d", (unsigned)7, (unsigned)8, 9);
    CHECK("[%s] [%8s] [%-8s]", "abc", "abc", "abc");
    CHECK("%.3s %.3s %02d %d %02d:%02d:%02d", "Monday", "November", 3, 2026, 9, 5, 7);
    CHECK("%.3s, %02d %.3s %d GMT", "Tue", 4, "Dec", 2026);
    CHECK("[%-17.17s] [%-4u] [%-10.10s] %3u%%", "a-very-long-task-name-here", 12u, "Sleeping-long", 99u);
    CHECK("[%8.2s] [%-8.2s] [%.0s] [%.10s]", "abcdef", "abcdef", "abc", "ab");
    CHECK("/%.*s/: %s", 4, "abcdefgh", "after");
    CHECK("[%*d] [%-*d] [%*s]", 6, 12, 6, 12, -6, "ab");
    CHECK("%s+0x%lx (no symbols) %d", "lib.so", (unsigned long)0xfffff7801000UL, 5);
    CHECK("%c%c%c [%3c] [%-3c]", 'a', 'b', 'c', 'd', 'e');
    CHECK("%d %d", -2147483647 - 1, 2147483647);
    CHECK("%lld", (long long)(-9223372036854775807LL - 1));
    truncation();
    memory();
    printf("ksnprintf: %d passed, %d failed\n", passed, failed);
    return failed != 0;
}
