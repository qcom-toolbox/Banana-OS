/* freestanding shim for third_party/stb (see ../stb_image_impl.c) */
#ifndef SHIM_LIMITS_H
#define SHIM_LIMITS_H
#define CHAR_BIT  8
#define SCHAR_MAX 127
#define UCHAR_MAX 255
#define SHRT_MAX  32767
#define SHRT_MIN  (-32768)
#define USHRT_MAX 65535
#define INT_MAX   2147483647
#define INT_MIN   (-INT_MAX - 1)
#define UINT_MAX  4294967295u
#define LONG_MAX  2147483647L
#define LONG_MIN  (-LONG_MAX - 1L)
#define ULONG_MAX 4294967295ul
#endif
