#ifndef _STDLIB_H
#define _STDLIB_H
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
int    rand(void);
void   srand(unsigned int seed);
void   qsort(void* base, size_t n, size_t size, int (*cmp)(const void*, const void*));
void*  bsearch(const void* key, const void* base, size_t n, size_t size, int (*cmp)(const void*, const void*));
char*  (getenv)(const char* name);    /* (in parentheses: a program may define getenv as a macro) */
double strtod(const char* s, char** end);
float  strtof(const char* s, char** end);
long double strtold(const char* s, char** end);   /* (double precision) */
/* the absolute path, without . and .. (resolved may be NULL: malloc'd) */
char*  realpath(const char* path, char* resolved);
double atof(const char* s);
#endif
