/* host-test stand-in for kernel/kheap.h */
#ifndef KHEAP_H
#define KHEAP_H
#include <stdlib.h>
#define kmalloc(n) malloc(n)
#define kfree(p) free(p)
#define kzalloc(n) calloc(1, (n))
#define krealloc(p, n) realloc((p), (n))
#endif
