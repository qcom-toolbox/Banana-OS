/* SPDX-License-Identifier: BSD-2-Clause */
/* Banana OS SDK: Unix files - what a file's own record says (stat: the
 * mode, owner, times, links), changing it (chmod, chown, utimensat),
 * symbolic links, unlink / rmdir / rename with their errors, folder
 * descriptors and the *at calls over them (openat, fstatat, unlinkat...),
 * fchdir, fdopendir, realpath. With the system's calls of API version 28;
 * on an older system what it can do (a mode by kind, no links). */
#include <banana.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <utime.h>

#define V28 BANANA_HAS(__banana, fs_fdpath)

int __ufd_is(int fd);
int __ufd_fstat(int fd, struct stat* st);
int __dev_stat(const char* path, struct stat* st);      /* tty.c */

static int fail(int e) { errno = e; return -1; }
static int ret(int r) { return r < 0 ? fail(-r) : r; }

/* ── stat ─────────────────────────────────────────────────────────── */

static void from_ustat(const banana_ustat_t* u, struct stat* st) {
    memset(st, 0, sizeof(*st));
    st->st_mode = u->mode;
    st->st_uid = u->uid;
    st->st_gid = u->gid;
    st->st_nlink = u->nlink;
    st->st_size = (off_t)u->size;
    st->st_ino = (ino_t)u->ino;
    st->st_dev = (dev_t)u->dev;
    st->st_atim.tv_sec = (time_t)u->atime;
    st->st_mtim.tv_sec = (time_t)u->mtime;
    st->st_ctim.tv_sec = (time_t)u->ctime;
    st->st_blksize = 4096;
    st->st_blocks = (blkcnt_t)((u->size + 4095) / 4096 * 8);
}

/* (an older system: the kind of thing, its size) */
static int stat_old(const char* path, struct stat* st) {
    banana_stat_t b;
    if (__banana->stat(path, &b) != 0) return fail(ENOENT);
    memset(st, 0, sizeof(*st));
    st->st_mode = b.is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    st->st_size = b.size;
    st->st_nlink = 1;
    st->st_uid = st->st_gid = 1000;
    st->st_dev = 1;
    for (const char* c = path; *c; c++) st->st_ino = st->st_ino * 31 + (unsigned char)*c;   /* (a stable number per path) */
    st->st_blocks = (b.size + 511) / 512;
    st->st_blksize = 4096;
    return 0;
}

static int stat_any(const char* path, struct stat* st, int follow) {
    if (!path) return fail(EFAULT);
    if (!*path) return fail(ENOENT);
    if (!strncmp(path, "/dev/", 5) && __dev_stat(path, st) == 0) return 0;
    if (!V28) return stat_old(path, st);
    banana_ustat_t u;
    int r = __banana->fs_ustat(path, follow, &u);
    if (r < 0) return fail(-r);
    from_ustat(&u, st);
    return 0;
}

int stat(const char* path, struct stat* st) { return stat_any(path, st, 1); }
int lstat(const char* path, struct stat* st) { return stat_any(path, st, 0); }

int fstat(int fd, struct stat* st) {
    if (__ufd_is(fd)) return __ufd_fstat(fd, st);
    memset(st, 0, sizeof(*st));
    st->st_blksize = 4096;
    if (V28) {
        banana_ustat_t u;
        if (__banana->fs_fdstat(fd, &u) == 0) { from_ustat(&u, st); return 0; }
    }
    banana_fdinfo_t fi;
    if (BANANA_HAS(__banana, fd_info)) {
        if (__banana->fd_info(fd, &fi) != 0) return fail(EBADF);
        if (fi.type != BANANA_FD_FILE) {
            static const unsigned dev_minor[] = { 0, 3, 5, 7, 8 };   /* (/dev/null, zero, full, random) */
            int chr = fi.type == BANANA_FD_TERM || fi.type == BANANA_FD_PTM || fi.type == BANANA_FD_PTS || fi.type == BANANA_FD_DEV;
            st->st_mode = fi.type == BANANA_FD_DIR ? S_IFDIR | 0755 : chr ? S_IFCHR | (fi.type == BANANA_FD_DEV ? 0666 : 0620)
                        : fi.is_sock || fi.type == BANANA_FD_SOCK ? S_IFSOCK | 0777 : S_IFIFO | 0600;
            st->st_rdev = fi.type == BANANA_FD_PTS ? ((fi.size & BANANA_TTY_CONSOLE) ? (4u << 8) | (fi.size & 0xFF) : (136u << 8) | (unsigned)fi.size)
                        : fi.type == BANANA_FD_PTM ? (5u << 8) | 2u
                        : fi.type == BANANA_FD_DEV && fi.size < 5 ? (1u << 8) | dev_minor[fi.size] : 0x8800;
            st->st_nlink = 1;
            st->st_uid = st->st_gid = 1000;
            st->st_ino = (ino_t)(fi.id >> 4);
            st->st_dev = chr ? 0x16 : 0xF;
            /* a device: the same as stat() of its /dev name says (tty.c) -
             * bash's noclobber compares the two for 2>/dev/null */
            if (fi.type == BANANA_FD_DEV || fi.type == BANANA_FD_PTS) { st->st_dev = 5; st->st_ino = (ino_t)(1000 + st->st_rdev); }
            return 0;
        }
    }
    if (fd <= 2) { st->st_mode = S_IFCHR | 0620; return 0; }
    long cur = __banana->seek(fd, 0, SEEK_CUR);
    if (cur < 0) return fail(EBADF);
    long end = __banana->seek(fd, 0, SEEK_END);
    __banana->seek(fd, cur, SEEK_SET);
    st->st_mode = S_IFREG | 0644;
    st->st_size = end;
    st->st_nlink = 1;
    st->st_uid = st->st_gid = 1000;
    st->st_ino = (ino_t)fd;
    st->st_blocks = (end + 511) / 512;
    return 0;
}

/* ── folder descriptors and *at ────────────────────────────────────── */

/* path, relative to the folder dirfd (AT_FDCWD: the current one), as a path: 0 or -1 */
static int at(int dirfd, const char* path, char* out, size_t cap, int empty_ok) {
    if (!path) return fail(EFAULT);
    if (!*path && !empty_ok) return fail(ENOENT);
    if (path[0] == '/' || dirfd == AT_FDCWD) {
        if (strlen(path) >= cap) return fail(ENAMETOOLONG);
        strcpy(out, path);
        return 0;
    }
    if (!V28) return fail(ENOTSUP);
    char dir[PATH_MAX];
    int n = __banana->fs_fdpath(dirfd, dir, sizeof(dir));
    if (n < 0) return fail(n == -9 ? EBADF : -n);
    if (!*path) { snprintf(out, cap, "%s", dir); return 0; }
    if ((size_t)snprintf(out, cap, "%s%s%s", dir, n && dir[n - 1] == '/' ? "" : "/", path) >= cap) return fail(ENAMETOOLONG);
    return 0;
}

int fstatat(int dirfd, const char* path, struct stat* st, int flags) {
    if ((flags & AT_EMPTY_PATH) && path && !*path) return fstat(dirfd, st);
    char p[PATH_MAX];
    if (at(dirfd, path, p, sizeof(p), 0)) return -1;
    return stat_any(p, st, !(flags & AT_SYMLINK_NOFOLLOW));
}

int openat(int dirfd, const char* path, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    int mode = (flags & O_CREAT) ? va_arg(ap, int) : 0;
    va_end(ap);
    char p[PATH_MAX];
    if (at(dirfd, path, p, sizeof(p), 0)) return -1;
    return open(p, flags, mode);
}

int fchdir(int fd) {
    if (!V28) return fail(ENOTSUP);
    char dir[PATH_MAX];
    int n = __banana->fs_fdpath(fd, dir, sizeof(dir));
    if (n < 0) return fail(n == -9 ? EBADF : -n);
    return chdir(dir);
}

int mkdirat(int dirfd, const char* path, mode_t mode) {
    char p[PATH_MAX];
    if (at(dirfd, path, p, sizeof(p), 0)) return -1;
    return mkdir(p, mode);
}

int faccessat(int dirfd, const char* path, int mode, int flags) {
    char p[PATH_MAX];
    if (at(dirfd, path, p, sizeof(p), 0)) return -1;
    struct stat st;
    if (stat_any(p, &st, !(flags & AT_SYMLINK_NOFOLLOW))) return -1;
    if (mode == F_OK) return 0;
    /* Unix's rule, for the real user (AT_EACCESS: the effective one): root
     * may read and write anything and run what anyone may run; others the
     * owner's, the group's or the others' bits */
    uid_t u = (flags & AT_EACCESS) ? geteuid() : getuid();
    gid_t g = (flags & AT_EACCESS) ? getegid() : getgid();
    if (u == 0) return (mode & X_OK) && !(st.st_mode & 0111) && !S_ISDIR(st.st_mode) ? fail(EACCES) : 0;
    unsigned bits;
    if (st.st_uid == u) bits = (st.st_mode >> 6) & 7;
    else {
        int in = st.st_gid == g;
        if (!in) {
            gid_t list[16];
            int n = getgroups(16, list);
            for (int i = 0; i < n && !in; i++) in = list[i] == st.st_gid;
        }
        bits = in ? (st.st_mode >> 3) & 7 : st.st_mode & 7;
    }
    if (((mode & R_OK) && !(bits & 4)) || ((mode & W_OK) && !(bits & 2)) || ((mode & X_OK) && !(bits & 1))) return fail(EACCES);
    return 0;
}
int access(const char* path, int mode) { return faccessat(AT_FDCWD, path, mode, 0); }
int eaccess(const char* path, int mode) { return faccessat(AT_FDCWD, path, mode, AT_EACCESS); }
int euidaccess(const char* path, int mode) { return faccessat(AT_FDCWD, path, mode, AT_EACCESS); }

/* ── changing a file's record ──────────────────────────────────────── */

int fchmodat(int dirfd, const char* path, mode_t mode, int flags) {
    char p[PATH_MAX];
    if (at(dirfd, path, p, sizeof(p), 0)) return -1;
    if (flags & AT_SYMLINK_NOFOLLOW) {               /* (a link's own mode: Linux has none either) */
        struct stat st;
        if (lstat(p, &st)) return -1;
        if (S_ISLNK(st.st_mode)) return fail(EOPNOTSUPP);
    }
    if (!V28) { struct stat st; return stat(p, &st); }
    return ret(__banana->fs_chmod(p, mode));
}
int chmod(const char* path, mode_t mode) { return fchmodat(AT_FDCWD, path, mode, 0); }
int fchmod(int fd, mode_t mode) {
    if (!V28) return 0;
    int r = __banana->fs_fdattr(fd, 1, mode, 0);
    if (r == -9) {                                   /* (a folder's descriptor: by its path) */
        char dir[PATH_MAX];
        if (__banana->fs_fdpath(fd, dir, sizeof(dir)) >= 0) return chmod(dir, mode);
    }
    return ret(r);
}

int fchownat(int dirfd, const char* path, uid_t u, gid_t g, int flags) {
    char p[PATH_MAX];
    (void)flags;                                     /* (the system changes the link's target) */
    if (at(dirfd, path, p, sizeof(p), 0)) return -1;
    if (!V28) { struct stat st; return stat(p, &st); }
    return ret(__banana->fs_chown(p, u == (uid_t)-1 ? -1 : (int)u, g == (gid_t)-1 ? -1 : (int)g));
}
int chown(const char* path, uid_t u, gid_t g) { return fchownat(AT_FDCWD, path, u, g, 0); }
int lchown(const char* path, uid_t u, gid_t g) { return fchownat(AT_FDCWD, path, u, g, AT_SYMLINK_NOFOLLOW); }
int fchown(int fd, uid_t u, gid_t g) {
    if (!V28) return 0;
    return ret(__banana->fs_fdattr(fd, 2, u == (uid_t)-1 ? -1 : (long long)u, g == (gid_t)-1 ? -1 : (long long)g));
}

/* the time of the last change (the system keeps no access time of ours):
 * NULL or UTIME_NOW: now; UTIME_OMIT: kept */
#define V29 BANANA_HAS(__banana, fs_utimes)

/* a time for the system: seconds, -1 now, -2 as it is */
static long long when1(const struct timespec* t, int k) {
    if (!t || t[k].tv_nsec == UTIME_NOW) return -1;
    if (t[k].tv_nsec == UTIME_OMIT) return -2;
    return (long long)t[k].tv_sec;
}
static long long when(const struct timespec* t) { return when1(t, 1); }

int utimensat(int dirfd, const char* path, const struct timespec t[2], int flags) {
    if (!path) return futimens(dirfd, t);
    char p[PATH_MAX];
    if (at(dirfd, path, p, sizeof(p), 0)) return -1;
    if (!V28) { struct stat st; return stat(p, &st); }
    int follow = !(flags & AT_SYMLINK_NOFOLLOW);
    if (t && t[0].tv_nsec == UTIME_OMIT && t[1].tv_nsec == UTIME_OMIT) { struct stat st; return stat_any(p, &st, follow); }
    if (V29) return ret(__banana->fs_utimes(p, follow, when1(t, 0), when1(t, 1)));
    if (t && t[1].tv_nsec == UTIME_OMIT) { struct stat st; return stat_any(p, &st, follow); }
    return ret(__banana->fs_utime(p, follow, when(t)));
}

int futimens(int fd, const struct timespec t[2]) {
    if (!V28) return 0;
    if (t && t[0].tv_nsec == UTIME_OMIT && t[1].tv_nsec == UTIME_OMIT) return 0;
    int r = V29 ? __banana->fs_fdattr(fd, 4, when1(t, 0), when1(t, 1))
          : t && t[1].tv_nsec == UTIME_OMIT ? 0 : __banana->fs_fdattr(fd, 3, when(t), 0);
    if (r == -9) {
        char dir[PATH_MAX];
        if (__banana->fs_fdpath(fd, dir, sizeof(dir)) >= 0) return utimensat(AT_FDCWD, dir, t, 0);
    }
    return ret(r);
}

static int tv_to_ts(const struct timeval* tv, struct timespec* ts) {
    if (!tv) return 0;
    for (int i = 0; i < 2; i++) { ts[i].tv_sec = tv[i].tv_sec; ts[i].tv_nsec = tv[i].tv_usec * 1000; }
    return 1;
}
int utimes(const char* path, const struct timeval t[2]) {
    struct timespec ts[2];
    return utimensat(AT_FDCWD, path, tv_to_ts(t, ts) ? ts : NULL, 0);
}
int lutimes(const char* path, const struct timeval t[2]) {
    struct timespec ts[2];
    return utimensat(AT_FDCWD, path, tv_to_ts(t, ts) ? ts : NULL, AT_SYMLINK_NOFOLLOW);
}
int futimes(int fd, const struct timeval t[2]) {
    struct timespec ts[2];
    return futimens(fd, tv_to_ts(t, ts) ? ts : NULL);
}
int futimesat(int dirfd, const char* path, const struct timeval t[2]) {
    struct timespec ts[2];
    return utimensat(dirfd, path, tv_to_ts(t, ts) ? ts : NULL, 0);
}
int utime(const char* path, const struct utimbuf* b) {
    struct timespec ts[2];
    if (b) { ts[0].tv_sec = b->actime; ts[1].tv_sec = b->modtime; ts[0].tv_nsec = ts[1].tv_nsec = 0; }
    return utimensat(AT_FDCWD, path, b ? ts : NULL, 0);
}

/* ── links ─────────────────────────────────────────────────────────── */

int symlinkat(const char* target, int dirfd, const char* linkpath) {
    char p[PATH_MAX];
    if (!target || !*target) return fail(ENOENT);
    if (at(dirfd, linkpath, p, sizeof(p), 0)) return -1;
    if (!V28) return fail(EPERM);
    return ret(__banana->fs_symlink(target, p));
}
int symlink(const char* target, const char* linkpath) { return symlinkat(target, AT_FDCWD, linkpath); }

ssize_t readlinkat(int dirfd, const char* path, char* buf, size_t len) {
    char p[PATH_MAX];
    if (at(dirfd, path, p, sizeof(p), 0)) return -1;
    if (!V28) { struct stat st; return stat(p, &st) ? -1 : fail(EINVAL); }
    return ret(__banana->fs_readlink(p, buf, len > INT_MAX ? INT_MAX : (int)len));
}
ssize_t readlink(const char* path, char* buf, size_t len) { return readlinkat(AT_FDCWD, path, buf, len); }

/* hard links (version 29): another name of the same file, on the system's
 * own disk (not on FAT / exFAT sticks: EPERM) */
int linkat(int a, const char* from, int b, const char* to, int flags) {
    char p[PATH_MAX], q[PATH_MAX];
    if (flags & ~AT_SYMLINK_FOLLOW) return fail(EINVAL);
    if (at(a, from, p, sizeof(p), 0) || at(b, to, q, sizeof(q), 0)) return -1;
    if (!V29) return fail(EPERM);
    if (flags & AT_SYMLINK_FOLLOW) {
        char r[PATH_MAX];
        if (!realpath(p, r)) return -1;
        strcpy(p, r);
    }
    return ret(__banana->fs_link(p, q));
}
int link(const char* from, const char* to) { return linkat(AT_FDCWD, from, AT_FDCWD, to, 0); }

/* ── removing, renaming, making folders ────────────────────────────── */

int unlinkat(int dirfd, const char* path, int flags) {
    char p[PATH_MAX];
    if (at(dirfd, path, p, sizeof(p), 0)) return -1;
    if (!V28) return __banana->remove(p) == 0 ? 0 : fail(ENOENT);
    return ret(__banana->fs_unlink(p, (flags & AT_REMOVEDIR) != 0));
}
int unlink(const char* path) { return unlinkat(AT_FDCWD, path, 0); }
int rmdir(const char* path) { return unlinkat(AT_FDCWD, path, AT_REMOVEDIR); }
int remove(const char* path) {
    struct stat st;
    if (lstat(path, &st)) return -1;
    return S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path);
}

int renameat(int da, const char* from, int db, const char* to) {
    char a[PATH_MAX], b[PATH_MAX];
    if (at(da, from, a, sizeof(a), 0) || at(db, to, b, sizeof(b), 0)) return -1;
    struct stat sa, sb;
    if (lstat(a, &sa)) return -1;
    int have_b = lstat(b, &sb) == 0;
    if (have_b) {
        if (sa.st_ino == sb.st_ino && sa.st_dev == sb.st_dev) return 0;      /* (the same file) */
        /* POSIX: a folder replaces only an empty folder, a file only a file */
        if (S_ISDIR(sa.st_mode) && !S_ISDIR(sb.st_mode)) return fail(ENOTDIR);
        if (!S_ISDIR(sa.st_mode) && S_ISDIR(sb.st_mode)) return fail(EISDIR);
        if (S_ISDIR(sb.st_mode) && rmdir(b)) return -1;
    }
    if (__banana->rename(a, b) != 0) return fail(EACCES);
    return 0;
}
int rename(const char* from, const char* to) { return renameat(AT_FDCWD, from, AT_FDCWD, to); }
int renameat2(int da, const char* from, int db, const char* to, unsigned flags) {
    if (flags & 1) {                                  /* RENAME_NOREPLACE */
        char b[PATH_MAX];
        struct stat st;
        if (at(db, to, b, sizeof(b), 0)) return -1;
        if (lstat(b, &st) == 0) return fail(EEXIST);
    } else if (flags) return fail(EINVAL);
    return renameat(da, from, db, to);
}

/* errno as on Linux, EEXIST above all: Java's Files.createDirectories takes
 * a folder that is there by it (it saw a stale ENOENT and gave up) */
int mkdir(const char* path, mode_t mode) {
    if (!path || !*path) return fail(ENOENT);
    struct stat st;
    if (lstat(path, &st) == 0) return fail(EEXIST);
    if (__banana->mkdir(path) == 0) {
        if (V28) {
            mode_t m = umask(0);
            umask(m);
            if ((mode & ~m & 07777) != 0755) __banana->fs_chmod(path, mode & ~m & 07777);
        }
        return 0;
    }
    /* why not: the folder it would go in */
    char parent[PATH_MAX];
    size_t n = strlen(path);
    if (n >= sizeof(parent)) return fail(ENAMETOOLONG);
    memcpy(parent, path, n + 1);
    while (n > 1 && parent[n - 1] == '/') parent[--n] = 0;
    char* slash = strrchr(parent, '/');
    if (slash == parent) slash[1] = 0;
    else if (slash) *slash = 0;
    else strcpy(parent, ".");
    if (stat(parent, &st)) return fail(ENOENT);
    return fail(S_ISDIR(st.st_mode) ? EACCES : ENOTDIR);
}

/* ── folders ──────────────────────────────────────────────────────── */

struct __dir {
    char          path[PATH_MAX];
    int           index;
    int           fd;                 /* dirfd()'s, -1 until asked for */
    int           synth;              /* /dev (1), /dev/fd (2), /dev/pts (3): made up here */
    int           next;               /* (/dev/fd, /dev/pts: the number to try next) */
    struct dirent ent;
    /* the folder's names, read at the first readdir: entries removed or added
     * while it is read do not make others skipped or repeated (rm -r, Rust's
     * remove_dir_all delete as they go) - names NUL-separated, a type each */
    char*         names;
    unsigned char* types;
    size_t*       offs;               /* where each name starts */
    int           count, loaded;
};

static void load_names(DIR* d) {
    size_t cap = 4096, used = 0;
    int n = 0, tcap = 64;
    char* names = malloc(cap);
    unsigned char* types = malloc((size_t)tcap);
    size_t* offs = malloc(sizeof(size_t) * (size_t)tcap);
    banana_dirent_t e;
    while (names && types && offs && __banana->readdir(d->path, n, &e) == 0) {
        size_t len = strlen(e.name) + 1;
        if (used + len > cap) {
            char* m = realloc(names, cap * 2 + len);
            if (!m) break;
            names = m;
            cap = cap * 2 + len;
        }
        if (n == tcap) {
            unsigned char* t = realloc(types, (size_t)tcap * 2);
            if (!t) break;
            types = t;
            size_t* o = realloc(offs, sizeof(size_t) * (size_t)tcap * 2);
            if (!o) break;
            offs = o;
            tcap *= 2;
        }
        offs[n] = used;
        memcpy(names + used, e.name, len);
        used += len;
        types[n++] = e.is_dir ? DT_DIR : V28 ? DT_UNKNOWN : DT_REG;
    }
    free(d->names);
    free(d->types);
    free(d->offs);
    d->names = names;
    d->types = types;
    d->offs = offs;
    d->count = names && types && offs ? n : 0;
    d->loaded = 1;
}

/* /dev's devices (kernel/app.c: dev_open), as ls /dev shows them */
static const char* const g_devnames[] = {
    "console", "fd", "full", "null", "ptmx", "pts", "random", "stderr", "stdin", "stdout", "tty",
    "tty1", "tty2", "tty3", "tty4", "tty5", "tty6", "ttyS0", "urandom", "zero",
};

static int dev_dir(const char* path) {
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s", path);
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/') p[--n] = 0;
    return !strcmp(p, "/dev") ? 1 : !strcmp(p, "/dev/fd") ? 2 : !strcmp(p, "/dev/pts") ? 3 : 0;
}

/* the next of the made-up folders' entries: 0 at the end (errno as it was:
 * readdir's end must not look like an error) */
static int synth_next1(DIR* d);
static int synth_next(DIR* d) {
    int e = errno;
    int r = synth_next1(d);
    errno = e;
    return r;
}
static int synth_next1(DIR* d) {
    if (d->synth == 1) {
        int i = d->index - 2;
        /* (the consoles: only where there are some) */
        while (i < (int)(sizeof(g_devnames) / sizeof(g_devnames[0])) && !strncmp(g_devnames[i], "tty", 3) && g_devnames[i][3]) {
            struct stat st;
            char p[24];
            snprintf(p, sizeof(p), "/dev/%s", g_devnames[i]);
            if (!__dev_stat(p, &st)) break;
            i = ++d->index - 2;
        }
        if (i >= (int)(sizeof(g_devnames) / sizeof(g_devnames[0]))) return 0;
        snprintf(d->ent.d_name, sizeof(d->ent.d_name), "%s", g_devnames[i]);
        d->ent.d_type = !strcmp(g_devnames[i], "fd") || !strcmp(g_devnames[i], "pts") ? DT_DIR :
                        !strncmp(g_devnames[i], "std", 3) ? DT_LNK : DT_CHR;
        return 1;
    }
    char path[32];
    for (; d->next < 1024; d->next++) {
        if (d->synth == 2) {
            if (fcntl(d->next, F_GETFD) < 0 || d->next == d->fd) continue;
        } else {
            struct stat st;
            snprintf(path, sizeof(path), "/dev/pts/%d", d->next);
            if (d->next >= 256 || __dev_stat(path, &st)) { if (d->next >= 256) return 0; continue; }
        }
        snprintf(d->ent.d_name, sizeof(d->ent.d_name), "%d", d->next);
        d->ent.d_type = d->synth == 2 ? DT_LNK : DT_CHR;
        d->next++;
        return 1;
    }
    return 0;
}

DIR* opendir(const char* path) {
    struct stat st;
    if (!path || stat(path, &st)) return NULL;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return NULL; }
    /* (a folder one may not read: EACCES now, as Unix's opendir - not an empty listing) */
    if (__banana->version >= 35) {
        banana_dirent_t probe;
        if (__banana->readdir(path, 0, &probe) == -13) { errno = EACCES; return NULL; }
    }
    DIR* d = calloc(1, sizeof(DIR));
    if (!d) { errno = ENOMEM; return NULL; }
    snprintf(d->path, sizeof(d->path), "%s", path);
    d->fd = -1;
    d->synth = dev_dir(path);
    return d;
}

DIR* fdopendir(int fd) {
    if (!V28) { errno = ENOTSUP; return NULL; }
    char dir[PATH_MAX];
    int n = __banana->fs_fdpath(fd, dir, sizeof(dir));
    if (n < 0) { errno = n == -9 ? EBADF : -n; return NULL; }
    struct stat st;
    if (fstat(fd, &st) || !S_ISDIR(st.st_mode)) { errno = ENOTDIR; return NULL; }
    DIR* d = opendir(dir);
    if (d) d->fd = fd;                       /* (closedir closes it) */
    return d;
}

/* "." and ".." first, as everywhere, then what the folder holds */
struct dirent* readdir(DIR* d) {
    memset(&d->ent, 0, sizeof(d->ent));
    if (d->index < 2) {
        strcpy(d->ent.d_name, d->index ? ".." : ".");
        d->ent.d_type = DT_DIR;
    } else if (d->synth) {
        if (!synth_next(d)) return NULL;
    } else {
        if (!d->loaded) load_names(d);
        int k = d->index - 2;
        if (k >= d->count) return NULL;
        const char* name = d->names + d->offs[k];
        snprintf(d->ent.d_name, sizeof(d->ent.d_name), "%s", name);
        /* (a file might be a link: the caller asks lstat when it matters) */
        d->ent.d_type = d->types[k];
    }
    d->index++;
    d->ent.d_ino = (ino_t)d->index;
    d->ent.d_off = d->index;
    d->ent.d_reclen = sizeof(struct dirent);
    return &d->ent;
}

int readdir_r(DIR* d, struct dirent* e, struct dirent** out) {
    struct dirent* r = readdir(d);
    if (r) { *e = *r; *out = e; } else *out = NULL;
    return 0;
}

int closedir(DIR* d) {
    if (!d) return fail(EBADF);
    if (d->fd >= 0) close(d->fd);
    free(d->names);
    free(d->types);
    free(d->offs);
    free(d);
    return 0;
}
void rewinddir(DIR* d) { d->index = 0; d->next = 0; d->loaded = 0; }   /* (read afresh) */
long telldir(DIR* d) { return d->index; }
void seekdir(DIR* d, long pos) { d->index = (int)pos; }
int dirfd(DIR* d) {
    if (d->fd < 0) d->fd = open(d->path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    return d->fd;
}

/* ── realpath ──────────────────────────────────────────────────────── */

/* the absolute path, with ".", ".." and symbolic links gone; it must exist */
char* realpath(const char* path, char* resolved) {
    char buf[PATH_MAX], rest[PATH_MAX], link[PATH_MAX];
    if (!path) { errno = EINVAL; return NULL; }
    if (!*path) { errno = ENOENT; return NULL; }
    if (strlen(path) >= sizeof(rest)) { errno = ENAMETOOLONG; return NULL; }
    strcpy(rest, path);
    if (rest[0] == '/') buf[0] = 0;
    else if (!getcwd(buf, sizeof(buf))) return NULL;
    if (!strcmp(buf, "/")) buf[0] = 0;
    size_t n = strlen(buf);
    int links = 0;
    char* p = rest;
    while (*p) {
        while (*p == '/') p++;
        char* e = p;
        while (*e && *e != '/') e++;
        size_t l = (size_t)(e - p);
        if (l == 0) break;
        if (l == 1 && p[0] == '.') { p = e; continue; }
        if (l == 2 && p[0] == '.' && p[1] == '.') {
            while (n > 0 && buf[n - 1] != '/') n--;
            if (n > 0) n--;
            buf[n] = 0;
            p = e;
            continue;
        }
        if (n + 1 + l + 1 > sizeof(buf)) { errno = ENAMETOOLONG; return NULL; }
        buf[n++] = '/';
        memcpy(buf + n, p, l);
        buf[n + l] = 0;
        struct stat st;
        if (lstat(buf, &st)) return NULL;
        if (S_ISLNK(st.st_mode)) {
            if (++links > 40) { errno = ELOOP; return NULL; }
            ssize_t k = readlink(buf, link, sizeof(link) - 1);
            if (k < 0) return NULL;
            link[k] = 0;
            /* the link's target, then what was left of the path */
            if ((size_t)k + 1 + strlen(e) + 1 > sizeof(link)) { errno = ENAMETOOLONG; return NULL; }
            strcat(link, "/");
            strcat(link, e);
            strcpy(rest, link);
            p = rest;
            if (rest[0] == '/') n = 0;
            else n--;                                /* (relative: from the link's folder) */
            buf[n] = 0;
            continue;
        }
        if (*e && !S_ISDIR(st.st_mode)) { errno = ENOTDIR; return NULL; }
        n += l;
        p = e;
    }
    if (!n) { buf[0] = '/'; buf[1] = 0; }
    if (!resolved) return strdup(buf);
    strcpy(resolved, buf);
    return resolved;
}

int creat(const char* path, mode_t mode) { return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode); }

int alphasort(const struct dirent** a, const struct dirent** b) { return strcoll((*a)->d_name, (*b)->d_name); }

int scandir(const char* path, struct dirent*** list, int (*sel)(const struct dirent*),
            int (*cmp)(const struct dirent**, const struct dirent**)) {
    DIR* d = opendir(path);
    if (!d) return -1;
    struct dirent** v = NULL;
    size_t n = 0, cap = 0;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (sel && !sel(e)) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            struct dirent** k = realloc(v, cap * sizeof(*v));
            if (!k) goto fail;
            v = k;
        }
        if (!(v[n] = malloc(sizeof(struct dirent)))) goto fail;
        *v[n++] = *e;
    }
    closedir(d);
    if (cmp && n > 1) qsort(v, n, sizeof(*v), (int (*)(const void*, const void*))cmp);
    *list = v;
    return (int)n;
fail:
    while (n) free(v[--n]);
    free(v);
    closedir(d);
    errno = ENOMEM;
    return -1;
}

int chroot(const char* path) { (void)path; return fail(EPERM); }
long gethostid(void) { return 0x0a00020f; }
