#ifndef _FCNTL_H
#define _FCNTL_H
/* Banana OS SDK (ports): POSIX file descriptors over the SDK stdio */
#include <sys/types.h>
#define O_RDONLY   0
#define O_WRONLY   1
#define O_RDWR     2
#define O_ACCMODE  3
#define O_CREAT    0100
#define O_EXCL     0200
#define O_TRUNC    01000
#define O_APPEND   02000
#define O_NONBLOCK 04000
#define O_BINARY   0
#define O_CLOEXEC  02000000
int open(const char* path, int flags, ...);
int fcntl(int fd, int cmd, ...);
#define F_GETFD 1
#define F_SETFD 2
#define FD_CLOEXEC 1
#endif
