/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef _UNISTD_H
#define _UNISTD_H
#ifdef __cplusplus
extern "C" {
#endif
#include <stddef.h>
#include <sys/types.h>
/* what the system has of POSIX.1-2008 (Unix programs test these) */
#define _POSIX_VERSION      200809L
#define _POSIX2_VERSION     200809L
#define _POSIX_JOB_CONTROL  1
#define _POSIX_SAVED_IDS    1
#define _POSIX_NO_TRUNC     1
#define _POSIX_THREADS      200809L
#define _POSIX_TIMERS       200809L
#define _POSIX_MONOTONIC_CLOCK 200809L
off_t   lseek(int fd, off_t off, int whence);
int     isatty(int fd);
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif
unsigned int sleep(unsigned int seconds);
int     usleep(unsigned long usec);
ssize_t write(int fd, const void* buf, size_t n);
ssize_t read(int fd, void* buf, size_t n);
int     close(int fd);
char*   getcwd(char* buf, size_t size);
int     chdir(const char* path);
int     fchdir(int fd);
size_t  confstr(int name, char* buf, size_t len);
#define _CS_PATH 0
#define _CS_GNU_LIBC_VERSION 2
#define _CS_GNU_LIBPTHREAD_VERSION 3
int     unlink(const char* path);
int     rmdir(const char* path);
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* the process: an app (one user, banana: uid 1000) */
pid_t   getpid(void);                   /* unique among the apps since boot */
pid_t   getppid(void);
pid_t   gettid(void);                   /* the calling thread's */
uid_t   getuid(void);
uid_t   geteuid(void);
gid_t   getgid(void);
gid_t   getegid(void);
int     setuid(uid_t uid);         /* Unix's rules (root: any; others: their real / saved id) */
int     setgid(gid_t gid);
int     seteuid(uid_t uid);
int     setegid(gid_t gid);
int     setreuid(uid_t r, uid_t e);
int     setregid(gid_t r, gid_t e);
int     getgroups(int n, gid_t* list);
int     setresuid(uid_t r, uid_t e, uid_t s);
int     setresgid(gid_t r, gid_t e, gid_t s);
int     getresuid(uid_t* r, uid_t* e, uid_t* s);
int     getresgid(gid_t* r, gid_t* e, gid_t* s);
int     getpagesize(void);
int     gethostname(char* name, size_t len);
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
extern char** environ;
/* (getopt_long: <getopt.h>) */
extern char* optarg;
extern int optind, opterr, optopt;
int     getopt(int argc, char* const argv[], const char* opts);
int     access(const char* path, int mode);
ssize_t readlink(const char* path, char* buf, size_t len);
int     symlink(const char* target, const char* path);
int     link(const char* from, const char* to);
int     chown(const char* path, uid_t u, gid_t g);
int     fchown(int fd, uid_t u, gid_t g);
int     lchown(const char* path, uid_t u, gid_t g);
int     fchownat(int dirfd, const char* path, uid_t u, gid_t g, int flags);
int     faccessat(int dirfd, const char* path, int mode, int flags);
int     eaccess(const char* path, int mode);
int     euidaccess(const char* path, int mode);
ssize_t readlinkat(int dirfd, const char* path, char* buf, size_t len);
int     symlinkat(const char* target, int dirfd, const char* path);
int     linkat(int fromfd, const char* from, int tofd, const char* to, int flags);
int     unlinkat(int dirfd, const char* path, int flags);
int     chroot(const char* path);            /* (EPERM: one root) */
long    gethostid(void);
int     lchown(const char* path, uid_t u, gid_t g);
int     fsync(int fd);
int     fdatasync(int fd);
void    sync(void);
int     ftruncate(int fd, off_t len);
int     truncate(const char* path, off_t len);
ssize_t pread(int fd, void* buf, size_t n, off_t off);
ssize_t pwrite(int fd, const void* buf, size_t n, off_t off);
/* (off_t is 64-bit already: the *64 names are the same calls) */
#define lseek64 lseek
#define pread64 pread
#define pwrite64 pwrite
#define ftruncate64 ftruncate
int     dup(int fd);
int     dup2(int fd, int to);
int     dup3(int fd, int to, int flags);
int     pipe(int fds[2]);
int     pipe2(int fds[2], int flags);
pid_t   fork(void);
pid_t   vfork(void);
int     execve(const char* path, char* const argv[], char* const envp[]);
int     execv(const char* path, char* const argv[]);
int     execvp(const char* file, char* const argv[]);
int     execvpe(const char* file, char* const argv[], char* const envp[]);
int     execl(const char* path, const char* arg, ...);
int     execlp(const char* file, const char* arg, ...);
int     execle(const char* path, const char* arg, ...);
int     fexecve(int fd, char* const argv[], char* const envp[]);
pid_t   getpgid(pid_t pid);
pid_t   getsid(pid_t pid);
int     setpgrp(void);
pid_t   setsid(void);
pid_t   getpgrp(void);
int     setpgid(pid_t pid, pid_t pgid);
unsigned alarm(unsigned seconds);
int     pause(void);
unsigned ualarm(unsigned usecs, unsigned interval);
char*   getlogin(void);
char*   ttyname(int fd);
int     ttyname_r(int fd, char* buf, size_t len);
pid_t   tcgetpgrp(int fd);
int     tcsetpgrp(int fd, pid_t pgrp);
long    pathconf(const char* path, int name);
long    fpathconf(int fd, int name);
int     getdtablesize(void);
#define _PC_LINK_MAX         0       /* (Linux's numbers) */
#define _PC_MAX_CANON        1
#define _PC_MAX_INPUT        2
#define _PC_NAME_MAX         3
#define _PC_PATH_MAX         4
#define _PC_PIPE_BUF         5
#define _PC_CHOWN_RESTRICTED 6
#define _PC_NO_TRUNC         7
#define _PC_VDISABLE         8
void    _exit(int code) __attribute__((noreturn));      /* at once: no stdio flush */

/* sysconf() names (Linux's values) */
#define _SC_CLK_TCK            2
#define _SC_OPEN_MAX           4
#define _SC_PAGESIZE           30
#define _SC_PAGE_SIZE          _SC_PAGESIZE
#define _SC_IOV_MAX            60
#define _SC_GETGR_R_SIZE_MAX   69
#define _SC_GETPW_R_SIZE_MAX   70
#define _SC_THREAD_KEYS_MAX    74
#define _SC_THREAD_STACK_MIN   75
#define _SC_THREAD_THREADS_MAX 76
#define _SC_NPROCESSORS_CONF   83
#define _SC_NPROCESSORS_ONLN   84
#define _SC_PHYS_PAGES         85
#define _SC_AVPHYS_PAGES       86
#define _SC_LINE_MAX           43
#define _SC_HOST_NAME_MAX      180
long    sysconf(int name);
#ifdef __cplusplus
}
#endif
#endif
