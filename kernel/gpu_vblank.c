/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Vsync without burning the frame (kernel/gpu.h: gpu_vblank_wait). The
 * drivers waited for the next vertical blank by spinning on the display's
 * frame counter: up to a whole frame (16.7 ms at 60 Hz) at full speed for
 * every frame shown - dragging a window kept a core at 100 %. The refresh
 * period is learned from the counter's steps; the task sleeps through most
 * of the frame and spins only the last ~2.5 ms. */
#include "gpu.h"
#include "task.h"
#include "timer.h"

int gpu_vblank_wait(gpu_vblank_clock_t* c, uint32_t (*frame_count)(void)) {
    uint32_t f = frame_count();
    uintptr_t fl;
    __asm__ volatile("pushf; pop %0" : "=r"(fl));
    /* most of the frame asleep: the counter stepped at c->at_ns, and steps
     * every period_ns since (when that was seen, not guessed) */
    if ((fl & 0x200) && c->known && c->period_ns) {
        uint64_t now = timer_counter();
        uint64_t next = c->at_ns + c->period_ns * ((now - c->at_ns) / c->period_ns + 1);
        uint64_t left = next - now;
        if (left > 3500000u && left < 60000000u) task_sleep_ms((uint32_t)((left - 2500000u) / 1000000u));
    }
    /* the last stretch spun, as before (bounded) */
    uint64_t start = timer_counter();
    uint32_t spins = 0;
    while (frame_count() == f) {
        if (!(++spins & 255) && timer_counter() - start >= 20000000u) break;
        __asm__ volatile("pause");
    }
    uint32_t f2 = frame_count();
    int timeout = f2 == f;
    if (!timeout && spins) {
        /* the step was seen as it happened: the clock re-anchored, the period
         * learned (a step that came during the sleep says nothing precise) */
        uint64_t t = timer_counter();
        uint32_t frames = f2 - c->frame;
        if (c->known && frames >= 1 && frames <= 8) {
            uint64_t p = (t - c->at_ns) / frames;
            if (p >= 4000000u && p <= 50000000u) c->period_ns = c->period_ns ? (c->period_ns * 3 + p) / 4 : p;
        }
        c->frame = f2;
        c->at_ns = t;
        c->known = 1;
    } else if (!spins) c->known = 0;          /* (slept past it: spin the next one whole, to re-anchor) */
    gpu_note_vblank(timeout);
    return timeout;
}

