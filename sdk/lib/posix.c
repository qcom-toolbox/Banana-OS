/* Banana OS SDK: POSIX bits over the system calls - file descriptors
 * (open, lseek, fstat, stat), aligned memory, fdopen - for programs
 * ported from elsewhere. */
#include <banana.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern const banana_api_t* __banana;

int open(const char* path, int flags, ...) {
    int f = 0;
    int acc = flags & O_ACCMODE;
    if (acc == O_RDONLY || acc == O_RDWR) f |= BANANA_O_READ;
    if (acc == O_WRONLY || acc == O_RDWR) f |= BANANA_O_WRITE;
    if (flags & O_CREAT)  f |= BANANA_O_CREATE;
    if (flags & O_TRUNC)  f |= BANANA_O_TRUNC;
    if (flags & O_APPEND) f |= BANANA_O_APPEND;
    if ((flags & O_EXCL) && (flags & O_CREAT)) {
        banana_stat_t st;
        if (__banana->stat(path, &st) == 0) { errno = EEXIST; return -1; }
    }
    int fd = __banana->open(path, f);
    if (fd < 0) { errno = ENOENT; return -1; }
    return fd;
}

int fcntl(int fd, int cmd, ...) { (void)fd; (void)cmd; return 0; }

off_t lseek(int fd, off_t off, int whence) {
    long r = __banana->seek(fd, off, whence);
    if (r < 0) { errno = fd <= 2 ? ESPIPE : EINVAL; return -1; }
    return r;
}

int isatty(int fd) { return fd >= 0 && fd <= 2; }

int fstat(int fd, struct stat* st) {
    memset(st, 0, sizeof(*st));
    st->st_blksize = 4096;
    if (fd <= 2) { st->st_mode = 0020000; return 0; }     /* a character device */
    long cur = __banana->seek(fd, 0, SEEK_CUR);
    if (cur < 0) { errno = EBADF; return -1; }
    long end = __banana->seek(fd, 0, SEEK_END);
    __banana->seek(fd, cur, SEEK_SET);
    st->st_mode = S_IFREG | 0644;
    st->st_size = end;
    return 0;
}

int stat(const char* path, struct stat* st) {
    banana_stat_t b;
    if (__banana->stat(path, &b) != 0) { errno = ENOENT; return -1; }
    memset(st, 0, sizeof(*st));
    st->st_mode = b.is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    st->st_size = b.size;
    st->st_blksize = 4096;
    return 0;
}

int mkdir(const char* path, mode_t mode) { (void)mode; return __banana->mkdir(path) == 0 ? 0 : -1; }

/* ── aligned memory ───────────────────────────────────────────────── */
/* malloc gives 16-byte alignment; a larger one takes a bigger block and a
 * header just under the pointer handed out: [size, start, magic] */
#define ALIGNED_MAGIC ((uintptr_t)0xA119ED5Eu)

int posix_memalign(void** out, size_t align, size_t size) {
    if (align < sizeof(void*) || (align & (align - 1))) return EINVAL;
    if (align <= 16) { *out = malloc(size); return *out ? 0 : ENOMEM; }
    char* raw = malloc(size + align + 3 * sizeof(uintptr_t));
    if (!raw) return ENOMEM;
    uintptr_t p = ((uintptr_t)raw + 3 * sizeof(uintptr_t) + align - 1) & ~(uintptr_t)(align - 1);
    uintptr_t* h = (uintptr_t*)p;
    h[-3] = size;
    h[-2] = (uintptr_t)raw;
    h[-1] = ALIGNED_MAGIC ^ (uintptr_t)raw;
    *out = (void*)p;
    return 0;
}
void* aligned_alloc(size_t align, size_t size) { void* p; return posix_memalign(&p, align, size) ? NULL : p; }
void* memalign(size_t align, size_t size) { return aligned_alloc(align, size); }

/* (libc.c: free and realloc know these blocks) */
int __aligned_block(void* p, void** raw, size_t* size) {
    uintptr_t* h = p;
    if (((uintptr_t)p & 31) || h[-1] != (ALIGNED_MAGIC ^ h[-2])) return 0;
    if (raw) *raw = (void*)h[-2];
    if (size) *size = h[-3];
    return 1;
}

intmax_t strtoimax(const char* s, char** end, int base) { return strtoll(s, end, base); }
uintmax_t strtoumax(const char* s, char** end, int base) { return strtoull(s, end, base); }
