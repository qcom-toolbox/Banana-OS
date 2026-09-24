#ifndef KSTRING_H
#define KSTRING_H

#include "types.h"

/* Freestanding libc-style memory/string routines. These keep their
 * standard names on purpose: GCC is allowed to emit calls to memcpy/
 * memset/memmove/memcmp on its own (struct copies, zeroing loops), and
 * vendored code (third_party/) expects them too. */
void*  memcpy(void* dst, const void* src, size_t n);
void*  memmove(void* dst, const void* src, size_t n);
void*  memset(void* dst, int c, size_t n);
int    memcmp(const void* a, const void* b, size_t n);
size_t strlen(const char* s);
int    strcmp(const char* a, const char* b);
int    strncmp(const char* a, const char* b, size_t n);
int    strcasecmp(const char* a, const char* b);
int    strncasecmp(const char* a, const char* b, size_t n);
char*  strchr(const char* s, int c);
char*  strrchr(const char* s, int c);
char*  strstr(const char* hay, const char* needle);

/* strlcpy semantics: always NUL-terminates (if n > 0), returns strlen(src). */
size_t kstrlcpy(char* dst, const char* src, size_t n);
size_t kstrlcat(char* dst, const char* src, size_t n);

/* 32-bit fill (framebuffer rows etc.) */
void   memset32(uint32_t* dst, uint32_t v, size_t count);

int    k_isdigit(int c);
int    k_isspace(int c);
int    k_tolower(int c);
/* Parses an unsigned decimal; returns chars consumed (0 = no digits). */
int    k_parse_u32(const char* s, uint32_t* out);

/* Minimal printf family: %d %i %u %x %X %s %c %p %% with optional
 * '-', '0', width and 'l'/'ll' length (64-bit %llu/%llx supported). */
int    ksnprintf(char* buf, size_t n, const char* fmt, ...);
int    kvsnprintf(char* buf, size_t n, const char* fmt, __builtin_va_list ap);

#endif
