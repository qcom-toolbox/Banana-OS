/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Mesa's r600, built for Linux (the reference build of ports/mesa with
 * R600=1), driven on the hosted kernel radeon interface (kernel/radeon_drm.c
 * with Linux's command checker, kernel/radeon_gfx.c's ring and fences) and
 * the model card of tools/tests/radeon_gfx.c - as an LD_PRELOAD library:
 * /dev/dri/card0, its ioctls and the buffers' maps are answered here. Every
 * command stream the driver builds goes through the checker - its
 * relocations patched, its registers checked - and is run by the model's
 * command processor (which keeps the fences and memory writes, and skips the
 * drawing): the whole path from glClear to the read-back, but not the pixels.
 *   see tools/tests/radeon_host/run.sh
 * Built with hidden visibility: only the interposed calls below are seen
 * by the rest of the process (not the kernel's own memcpy and others). */
#define RADEON_PRELOAD 1
#define MODEL_ARENA (768u << 20)
#include "../radeon_gfx.c"

#define EXPORT __attribute__((visibility("default")))
typedef unsigned long ulong;
extern char* getenv(const char*);
extern void* dlsym(void*, const char*);
#define RTLD_NEXT ((void*)-1l)
extern int* __errno_location(void);
extern int pthread_mutex_lock(void*);
extern int pthread_mutex_unlock(void*);
static uint64_t g_mx[8];                    /* (a pthread_mutex_t: 40 bytes on x86_64) */

static int g_ready;
static char g_drm[1024];                     /* fds that are /dev/dri/card0 */
static int (*real_open)(const char*, int, ...);
static int (*real_close)(int);
static int (*real_ioctl)(int, ulong, ...);
static int (*real_fcntl)(int, int, ...);
static void* (*real_mmap)(void*, size_t, int, int, int, long);
static int (*real_munmap)(void*, size_t);
static ulong g_ioctls, g_cs, g_cs_refused;

static void boot(void) {
    if (g_ready) return;
    g_ready = 1;
    real_open = dlsym(RTLD_NEXT, "open");
    real_close = dlsym(RTLD_NEXT, "close");
    real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    real_fcntl = dlsym(RTLD_NEXT, "fcntl");
    real_mmap = dlsym(RTLD_NEXT, "mmap");
    real_munmap = dlsym(RTLD_NEXT, "munmap");
    g_quiet = getenv("MODEL_QUIET") != NULL;
    char why[160] = "";
    const char* chip = getenv("MODEL_CHIP");                 /* (turks, barts: those cards' models; else the HD 7450) */
    int r = model_boot(chip ? chip : "ok", why, sizeof(why));
    printf("preload: a Radeon model (%s, device %04x, 1 GiB) on /dev/dri/card0: %s\n", rad_family_name(rt.family), rt.pci.device, why);
    if (r) exit(2);
}

static int is_drm(int fd) { return fd >= 0 && fd < 1024 && g_drm[fd]; }

EXPORT int open(const char* path, int flags, ...) {
    boot();
    __builtin_va_list ap; __builtin_va_start(ap, flags); int mode = __builtin_va_arg(ap, int); __builtin_va_end(ap);
    if (path && (!strcmp(path, "/dev/dri/card0") || !strcmp(path, "/dev/dri/renderD128"))) {
        int fd = real_open("/dev/null", 2);
        if (fd >= 0 && fd < 1024) g_drm[fd] = 1;
        return fd;
    }
    return real_open(path, flags, mode);
}
EXPORT int open64(const char* path, int flags, ...) {
    __builtin_va_list ap; __builtin_va_start(ap, flags); int mode = __builtin_va_arg(ap, int); __builtin_va_end(ap);
    return open(path, flags, mode);
}
EXPORT int close(int fd) {
    boot();
    if (is_drm(fd)) g_drm[fd] = 0;
    return real_close(fd);
}
EXPORT int fcntl(int fd, int cmd, ...) {
    boot();
    __builtin_va_list ap; __builtin_va_start(ap, cmd); long arg = __builtin_va_arg(ap, long); __builtin_va_end(ap);
    int r = real_fcntl(fd, cmd, arg);
    if ((cmd == 0 || cmd == 1030) && r >= 0 && r < 1024 && is_drm(fd)) g_drm[r] = 1;   /* F_DUPFD(_CLOEXEC) */
    return r;
}
EXPORT int fcntl64(int fd, int cmd, ...) {
    __builtin_va_list ap; __builtin_va_start(ap, cmd); long arg = __builtin_va_arg(ap, long); __builtin_va_end(ap);
    return fcntl(fd, cmd, arg);
}
EXPORT int ioctl(int fd, ulong req, ...) {
    boot();
    __builtin_va_list ap; __builtin_va_start(ap, req); void* arg = __builtin_va_arg(ap, void*); __builtin_va_end(ap);
    if (!is_drm(fd)) return real_ioctl(fd, req, arg);
    pthread_mutex_lock(g_mx);
    g_ioctls++;
    int cs = ((req >> 8) & 0xFF) == 'd' && (req & 0xFF) == 0x40 + RD_CS;
    int r = rad_drm_ioctl(1, req, arg);
    if (cs) { g_cs++; if (r < 0) g_cs_refused++; }
    pthread_mutex_unlock(g_mx);
    if (r < 0) { *__errno_location() = -r; return -1; }
    return r;
}
/* a buffer's map: GEM_MMAP gave its address (as on Banana OS), the "offset" Linux's winsys passes on */
EXPORT void* mmap(void* a, size_t n, int prot, int flags, int fd, long off) {
    boot();
    if (is_drm(fd)) return (void*)off;
    return real_mmap(a, n, prot, flags, fd, off);
}
EXPORT void* mmap64(void* a, size_t n, int prot, int flags, int fd, long off) { return mmap(a, n, prot, flags, fd, off); }
EXPORT int munmap(void* p, size_t n) {
    boot();
    if (((uint8_t*)p >= g_arena && (uint8_t*)p < g_arena + sizeof(g_arena)) || ((uint8_t*)p >= VR && (uint8_t*)p < VR + VRAM_HOST)) return 0;
    return real_munmap(p, n);
}

__attribute__((destructor)) static void bye(void) {
    if (!g_ready) return;
    char b[200];
    rad_drm_stats(b, sizeof b);
    printf("preload: %lu requests, %lu command streams (%lu refused); %s; packets: %u memory writes, %u fences, %u IBs; "
           "%u bad addresses, %u bad packets\n",
           g_ioctls, g_cs, g_cs_refused, b, g_pkt_mem, g_pkt_eop, g_pkt_ib, g_bad_addr, g_bad_pkt);
}
