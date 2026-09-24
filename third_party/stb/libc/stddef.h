/* freestanding shim for third_party/stb (see ../stb_image_impl.c) */
#ifndef SHIM_STDDEF_H
#define SHIM_STDDEF_H
typedef unsigned int size_t;
typedef signed int   ptrdiff_t;
#ifndef NULL
#define NULL ((void*)0)
#endif
#define offsetof(t, m) __builtin_offsetof(t, m)
#endif
