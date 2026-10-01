/* host-test stand-in for kernel/kstring.h */
#ifndef KSTRING_H
#define KSTRING_H
#include <string.h>
#include <strings.h>
#include <stdio.h>
#define ksnprintf snprintf
#define kvsnprintf vsnprintf
static inline size_t kstrlcpy(char* d, const char* s, size_t n) {
    size_t l = strlen(s);
    if (n) { size_t c = l < n - 1 ? l : n - 1; memcpy(d, s, c); d[c] = 0; }
    return l;
}
static inline size_t kstrlcat(char* d, const char* s, size_t n) {
    size_t dl = strlen(d);
    if (dl >= n) return dl + strlen(s);
    return dl + kstrlcpy(d + dl, s, n - dl);
}
#endif
