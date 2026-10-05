#ifndef _STDIO_H
#define _STDIO_H
#include <stddef.h>
#include <stdarg.h>

/* Banana OS stdio: stdout and stderr go to the terminal the app runs in,
 * stdin reads lines from it; files are Banana OS files (paths like the
 * shell uses, /mnt/usb included). No floating point in printf/scanf. */

#define EOF      (-1)
#define BUFSIZ   512
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef struct _bfile FILE;
extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

int    printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
int    fprintf(FILE* f, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
int    sprintf(char* buf, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
int    snprintf(char* buf, size_t n, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
int    vprintf(const char* fmt, va_list ap);
int    vfprintf(FILE* f, const char* fmt, va_list ap);
int    vsprintf(char* buf, const char* fmt, va_list ap);
int    vsnprintf(char* buf, size_t n, const char* fmt, va_list ap);
int    scanf(const char* fmt, ...);
int    sscanf(const char* s, const char* fmt, ...);
int    putchar(int c);
int    puts(const char* s);
int    getchar(void);
char*  fgets(char* buf, int n, FILE* f);
int    fputs(const char* s, FILE* f);
int    fputc(int c, FILE* f);
int    putc(int c, FILE* f);
int    fgetc(FILE* f);
int    getc(FILE* f);
int    ungetc(int c, FILE* f);
FILE*  fopen(const char* path, const char* mode);
int    fclose(FILE* f);
size_t fread(void* buf, size_t size, size_t n, FILE* f);
size_t fwrite(const void* buf, size_t size, size_t n, FILE* f);
int    fseek(FILE* f, long off, int whence);
long   ftell(FILE* f);
void   rewind(FILE* f);
int    feof(FILE* f);
int    ferror(FILE* f);
void   clearerr(FILE* f);
int    fflush(FILE* f);
int    remove(const char* path);
int    rename(const char* from, const char* to);
void   perror(const char* s);
#endif
