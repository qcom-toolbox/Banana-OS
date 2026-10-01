#ifndef TYPES_H
#define TYPES_H

/* Freestanding types - no system headers needed */

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;

typedef signed char        int8_t;
typedef signed short       int16_t;
typedef signed int         int32_t;
typedef signed long long   int64_t;

/* pointer-sized types: 32-bit on i386, 64-bit on x86_64 */
typedef __SIZE_TYPE__      size_t;
typedef __PTRDIFF_TYPE__   ptrdiff_t;
typedef __UINTPTR_TYPE__   uintptr_t;
typedef __INTPTR_TYPE__    intptr_t;

/* the architecture this kernel was built for (uname, neofetch) */
#ifdef __x86_64__
#define BANANA_ARCH      "x86_64"
#define BANANA_ARCH_DESC "x86_64 (64-bit long mode)"
#else
#define BANANA_ARCH      "i686"
#define BANANA_ARCH_DESC "x86 (i686, 32-bit)"
#endif

#ifndef NULL
#define NULL ((void*)0)
#endif

/* uint32 → decimal string, returns pointer into buf */
static inline char* u32_to_str(uint32_t n, char* buf, int buflen) {
    buf[buflen-1] = '\0';
    int i = buflen - 2;
    if (n == 0) {
        buf[i--] = '0';
    } else {
        while (n > 0 && i >= 0) {
            buf[i--] = '0' + (n % 10);
            n /= 10;
        }
    }
    return &buf[i+1];
}

#endif
