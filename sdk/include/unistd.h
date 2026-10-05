#ifndef _UNISTD_H
#define _UNISTD_H
#include <stddef.h>
typedef long ssize_t;
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
