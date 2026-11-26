/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef _STDLIB_H
#define _STDLIB_H
#ifdef __cplusplus
extern "C" {
#endif
#include <stddef.h>
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX     0x7FFFFFFF
void*  malloc(size_t size);
void*  calloc(size_t n, size_t size);
void*  realloc(void* p, size_t size);
void   free(void* p);
/* memory aligned on align bytes (a power of two); free() it as usual */
int    posix_memalign(void** out, size_t align, size_t size);
void*  aligned_alloc(size_t align, size_t size);
void*  memalign(size_t align, size_t size);
void   exit(int code) __attribute__((noreturn));
void   abort(void) __attribute__((noreturn));
#include <alloca.h>
int    getloadavg(double loads[], int n);
void   _Exit(int code) __attribute__((noreturn));
int    atexit(void (*fn)(void));
int    atoi(const char* s);
long   atol(const char* s);
long long atoll(const char* s);
long   strtol(const char* s, char** end, int base);
unsigned long strtoul(const char* s, char** end, int base);
long long strtoll(const char* s, char** end, int base);
unsigned long long strtoull(const char* s, char** end, int base);
int    abs(int x);
long   labs(long x);
long long llabs(long long x);
typedef struct { int quot, rem; } div_t;
div_t  div(int a, int b);
typedef struct { long quot, rem; } ldiv_t;
typedef struct { long long quot, rem; } lldiv_t;
ldiv_t  ldiv(long a, long b);
lldiv_t lldiv(long long a, long long b);
int    system(const char* cmd);              /* (no shell for apps: -1) */
char*  secure_getenv(const char* name);
size_t mbstowcs(wchar_t* d, const char* s, size_t n);
size_t wcstombs(char* d, const wchar_t* s, size_t n);
int    mbtowc(wchar_t* pwc, const char* s, size_t n);
int    wctomb(char* s, wchar_t wc);
#define MB_CUR_MAX 4
int    rand(void);
void   srand(unsigned int seed);
void   qsort(void* base, size_t n, size_t size, int (*cmp)(const void*, const void*));
void   qsort_r(void* base, size_t n, size_t size, int (*cmp)(const void*, const void*, void*), void* arg);
void*  bsearch(const void* key, const void* base, size_t n, size_t size, int (*cmp)(const void*, const void*));
char*  (getenv)(const char* name);    /* (in parentheses: a program may define getenv as a macro) */
int    setenv(const char* name, const char* value, int overwrite);
int    unsetenv(const char* name);
/* pseudo-terminals (API version 27) */
int    posix_openpt(int flags);
int    grantpt(int fd);
int    unlockpt(int fd);
char*  ptsname(int fd);
int    ptsname_r(int fd, char* buf, size_t len);
int    putenv(char* s);
int    clearenv(void);
int    mkstemp(char* tmpl);
int    mkostemp(char* tmpl, int flags);
char*  mkdtemp(char* tmpl);
char*  mktemp(char* tmpl);
extern char** environ;
double strtod(const char* s, char** end);
float  strtof(const char* s, char** end);
struct __locale_struct;
double strtod_l(const char* s, char** end, struct __locale_struct* l);
float  strtof_l(const char* s, char** end, struct __locale_struct* l);
int    mblen(const char* s, size_t n);
void   quick_exit(int code) __attribute__((noreturn));
int    at_quick_exit(void (*fn)(void));
long double strtold(const char* s, char** end);   /* (double precision) */
/* the absolute path, without . and .. (resolved may be NULL: malloc'd) */
char*  realpath(const char* path, char* resolved);
double atof(const char* s);
#ifdef __cplusplus
}
#endif
#endif
