#ifndef _STDDEF_H
#define _STDDEF_H
typedef __SIZE_TYPE__    size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;
typedef __WCHAR_TYPE__   wchar_t;
#ifndef NULL
#define NULL ((void*)0)
#endif
#define offsetof(t, m) __builtin_offsetof(t, m)
typedef struct { long long __ll __attribute__((aligned(__alignof__(long long)))); long double __ld __attribute__((aligned(__alignof__(long double)))); } max_align_t;
#endif
