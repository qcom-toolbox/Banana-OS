#ifndef TASK_H
#define TASK_H

#include "types.h"

#define TASK_MAX         4
#define TASK_NAME_MAX    24
#define TASK_STACK_WORDS 1024   /* 4 KiB stack per created task */

typedef enum {
    TASK_UNUSED = 0,
    TASK_RUNNING,
    TASK_READY,
    TASK_SLEEPING
} task_state_t;

typedef struct {
    uint32_t     pid;
    char         name[TASK_NAME_MAX];
    task_state_t state;
    uint32_t     cpu_pct;
    uint32_t     ticks_total;
} task_info_t;

/* Turns the currently executing context (the boot stack) into task 0. */
void task_init(const char* main_task_name);

/* Spawns the real background stats-sampling thread ("sysmon"). */
void task_start_sysmon(void);

/* Creates a new cooperative kernel thread with its own 4 KiB stack.
 * `entry` takes no arguments and is expected to run forever, calling
 * task_yield()/task_sleep_ms() so other tasks get the CPU. */
void task_create(const char* name, void (*entry)(void));

/* Voluntarily gives up the CPU to the next READY task (round robin).
 * If nothing else is READY, returns to the caller immediately. */
void task_yield(void);

/* Real sleep: marks the calling task SLEEPING and does not resume it
 * until at least `ms` milliseconds of real PIT-tick time have passed. */
void task_sleep_ms(uint32_t ms);

int  task_count(void);
void task_snapshot(task_info_t* out, int max_count);
const char* task_state_str(task_state_t s);

#endif
