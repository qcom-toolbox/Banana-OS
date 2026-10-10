/* Banana OS Driver Kit: the few C library functions a driver may need
 * (gcc also calls memset / memcpy / memmove / memcmp by itself) */
#include "banana_driver.h"

void* bdrv_memset(void* d, int c, unsigned long n) {
    unsigned char* p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
void* bdrv_memcpy(void* d, const void* s, unsigned long n) {
    unsigned char* a = d;
    const unsigned char* b = s;
    while (n--) *a++ = *b++;
    return d;
}
int bdrv_memcmp(const void* a, const void* b, unsigned long n) {
    const unsigned char* x = a;
    const unsigned char* y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
    return 0;
}
unsigned long bdrv_strlen(const char* s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

void* memset(void* d, int c, unsigned long n) { return bdrv_memset(d, c, n); }
void* memcpy(void* d, const void* s, unsigned long n) { return bdrv_memcpy(d, s, n); }
int memcmp(const void* a, const void* b, unsigned long n) { return bdrv_memcmp(a, b, n); }
void* memmove(void* d, const void* s, unsigned long n) {
    unsigned char* a = d;
    const unsigned char* b = s;
    if (a < b) while (n--) *a++ = *b++;
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
