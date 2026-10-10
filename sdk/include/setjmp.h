#ifndef _SETJMP_H
#define _SETJMP_H
/* Banana OS SDK: non-local jumps (lib/setjmp.c) */
#ifdef __x86_64__
typedef long jmp_buf[8];
#else
typedef long jmp_buf[6];
#endif
int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));
#define _setjmp setjmp
#define _longjmp longjmp
#endif
