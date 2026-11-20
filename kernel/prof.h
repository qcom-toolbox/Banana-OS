/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PROF_H
#define PROF_H
#include "types.h"

/* a sampling profiler (kernel/prof.c, the `prof` command) */
typedef void (*prof_out_t)(void* ctx, const char* line);
/* looks at every core each millisecond for seconds (1..60), then writes the
 * report a line at a time; 0, or -1 (already running, no memory) */
int  prof_run(uint32_t seconds, prof_out_t out, void* ctx);
/* the interrupts (kernel/idt.c): where the core was when it came */
void prof_hit(uintptr_t ip, int user);
/* the boot core's timer (kernel/timer.c): the other cores looked at too */
void prof_tick(uint32_t ms);
int  prof_running(void);
#endif
