/* SPDX-License-Identifier: GPL-3.0-or-later */
/* kernel/gpu_vblank.c on a simulated 60 Hz display: the wait must end right
 * after each vertical blank, sleep through most of the frame, and spin only
 * the last few milliseconds (it spun through the whole frame before). */
#include "../../kernel/types.h"
#define GPU_H
typedef struct { uint32_t frame; uint64_t at_ns, period_ns; int known; } gpu_vblank_clock_t;
uint64_t timer_counter(void);
void task_sleep_ms(uint32_t ms);
void gpu_note_vblank(int timeout);
#include "../../kernel/gpu_vblank.c"
extern int printf(const char*, ...);
extern void exit(int);

#define PERIOD 16666667ull                     /* 60 Hz */
static uint64_t now = 5000000;                 /* (the simulated clock, ns) */
static uint64_t spun_ns, slept_ns;
static int timeouts, in_sleep;
uint64_t timer_counter(void) { if (!in_sleep) { now += 2000; spun_ns += 2000; } return now; }   /* (each read: 2 us of spinning) */
void task_sleep_ms(uint32_t ms) { uint64_t d = (uint64_t)ms * 1000000u + 600000u; now += d; slept_ns += d; }   /* (wakes up to a tick late) */
void gpu_note_vblank(int t) { timeouts += t; }
static uint32_t frame(void) { return (uint32_t)(now / PERIOD); }

static int fails;
#define CHECK(c, ...) do { if (c) printf("ok   " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

int main(void) {
    gpu_vblank_clock_t c = {0};
    uint32_t late_max = 0;
    for (int i = 0; i < 300; i++) {
        now += 3000000;                        /* (the frame's drawing: 3 ms) */
        uint64_t before = spun_ns;
        (void)before;
        gpu_vblank_wait(&c, frame);
        uint64_t into = now % PERIOD;          /* how far past the vertical blank it returned */
        if (i >= 5 && into > late_max) late_max = (uint32_t)into;
    }
    double busy = (double)spun_ns / (double)(spun_ns + slept_ns);
    printf("spun %.1f ms, slept %.1f ms over 300 frames: %.0f %% of the waiting spent spinning\n",
           spun_ns / 1e6, slept_ns / 1e6, busy * 100);
    CHECK(timeouts == 0, "no wait timed out");
    CHECK(c.known && c.period_ns > 16000000u && c.period_ns < 17400000u, "the period learned: %.2f ms", c.period_ns / 1e6);
    CHECK(late_max < 100000u, "every wait ends within 0.1 ms of the blank (worst %.3f ms)", late_max / 1e6);
    CHECK(busy < 0.35, "most of the waiting asleep, not spinning");
    /* a frame missed (the task held up for 40 ms): it re-anchors and goes on */
    now += 40000000;
    for (int i = 0; i < 20; i++) { now += 3000000; gpu_vblank_wait(&c, frame); }
    CHECK(timeouts == 0 && now % PERIOD < 100000u, "after a stall: still in step");
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
