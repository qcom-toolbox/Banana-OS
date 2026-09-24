/* freestanding shim for third_party/stb (see ../stb_image_impl.c) */
#ifndef SHIM_STDARG_H
#define SHIM_STDARG_H
typedef __builtin_va_list va_list;
#define va_start(v, l) __builtin_va_start(v, l)
#define va_end(v)      __builtin_va_end(v)
#define va_arg(v, t)   __builtin_va_arg(v, t)
#endif
