#ifndef _UNISTD_H
#define _UNISTD_H
#include <stddef.h>
typedef long ssize_t;
typedef long off_t;
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
int     unlink(const char* path);
int     rmdir(const char* path);
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#endif
