/* SPDX-License-Identifier: MIT */
/* What Linux's AtomBIOS interpreter (atom.c, from drivers/gpu/drm/radeon)
 * takes from the Linux kernel, on Banana OS's: logging, memory, delays,
 * unaligned little-endian reads, and the mutexes (the driver already runs
 * one table at a time: kernel/radeon_atom.c holds its own lock). */
#ifndef RADEON_LINUX_COMPAT_H
#define RADEON_LINUX_COMPAT_H

#include "../../kernel/types.h"
#include "../../kernel/kheap.h"
#include "../../kernel/kstring.h"
#include "../../kernel/serial.h"
#include "../../kernel/timer.h"

#ifndef __cplusplus
typedef _Bool bool;
#define true 1
#define false 0
#endif
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef uint16_t __le16;
typedef uint32_t __le32;

#define GFP_KERNEL 0
#define kzalloc(n, ...) kzalloc(n)
#define kmalloc_array(n, s, gfp) kmalloc((size_t)(n) * (size_t)(s))
#define kcalloc(n, s, gfp) kzalloc((size_t)(n) * (size_t)(s))
#define ENOMEM 12
#define EINVAL 22

#define KERN_DEBUG ""
#define KERN_INFO ""
#define printk(...) klog(__VA_ARGS__)
#define pr_info(...) klog("atom: " __VA_ARGS__)
#define DRM_ERROR(...) klog("atom: " __VA_ARGS__)
#define DRM_INFO(...) klog("atom: " __VA_ARGS__)
#define DRM_DEBUG(...) do { } while (0)
#define DRM_DEBUG_KMS(...) do { } while (0)

/* (busy-waits: tables wait microseconds to a few milliseconds) */
static inline void udelay(unsigned us) {
    uint64_t end = timer_counter() + (uint64_t)us * 1000u;
    while (timer_counter() < end) __asm__ volatile("pause");
}
static inline void mdelay(unsigned ms) { udelay(ms * 1000u); }
static inline void msleep(unsigned ms) { if (ms >= 2) timer_sleep_ms(ms); else mdelay(ms); }

#define jiffies ((unsigned long)timer_ms())
#define time_after(a, b) ((long)((b) - (a)) < 0)
#define jiffies_to_msecs(j) ((unsigned)(j))
static inline const char* str_yes_no(int v) { return v ? "yes" : "no"; }

static inline uint16_t get_unaligned_le16(const void* p) { const uint8_t* b = p; return (uint16_t)(b[0] | b[1] << 8); }
static inline uint32_t get_unaligned_le32(const void* p) {
    const uint8_t* b = p;
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static inline void put_unaligned_le32(uint32_t v, void* p) {
    uint8_t* b = p;
    b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
}
#define cpu_to_le16(x) ((uint16_t)(x))
#define cpu_to_le32(x) ((uint32_t)(x))
#define le16_to_cpu(x) ((uint16_t)(x))
#define le32_to_cpu(x) ((uint32_t)(x))

struct mutex { int unused; };
#define mutex_init(m) ((void)(m))
#define mutex_lock(m) ((void)(m))
#define mutex_unlock(m) ((void)(m))

#define offsetof(t, m) __builtin_offsetof(t, m)
#define __counted_by(m)
/* (the tables' waits: busy, wherever they are run from) */
#define drm_can_sleep() 0
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define __packed __attribute__((packed))

#endif
