/* freestanding shim for third_party/stb (see ../stb_image_impl.c) */
#ifndef SHIM_STDLIB_H
#define SHIM_STDLIB_H
#include <stddef.h>
static inline int abs(int v) { return v < 0 ? -v : v; }
#endif
