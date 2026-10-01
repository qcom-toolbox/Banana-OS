#ifndef TIMER_H
#define TIMER_H
#include <time.h>
#include <stdint.h>
static inline uint32_t timer_ms(void) { return (uint32_t)(clock() * 1000 / CLOCKS_PER_SEC); }
#endif
