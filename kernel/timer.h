#ifndef TIMER_H
#define TIMER_H

#include "types.h"

void     timer_init(void);          /* PIT channel 0 at 1 kHz, driven by IRQ0 */
uint32_t timer_ticks(void);         /* 100 Hz ticks since init (10 ms each) */
uint32_t timer_ms(void);            /* milliseconds since init */
void     timer_poll(void);          /* kept for callers; a no-op under IRQs */
void     timer_sleep_ms(uint32_t ms);

/* Halts the CPU until the next interrupt (at most ~1 ms away). Used by
 * every idle wait loop instead of spinning, so an idle Banana OS no
 * longer pins its host CPU core at 100%. */
void     timer_idle(void);

#endif
