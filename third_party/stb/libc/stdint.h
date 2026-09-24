/* freestanding shim for third_party/stb (see ../stb_image_impl.c) */
#ifndef SHIM_STDINT_H
#define SHIM_STDINT_H
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef signed char        int8_t;
typedef signed short       int16_t;
typedef signed int         int32_t;
typedef signed long long   int64_t;
typedef unsigned int       uintptr_t;
#endif
