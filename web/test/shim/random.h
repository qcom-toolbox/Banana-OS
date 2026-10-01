#ifndef RANDOM_H
#define RANDOM_H
#include <stdlib.h>
#include <stdint.h>
static inline uint32_t random_u32(void) { return ((uint32_t)rand() << 16) ^ (uint32_t)rand(); }
#endif
