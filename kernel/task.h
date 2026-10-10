#ifndef TASK_H
#define TASK_H

#include "types.h"

/* main shell + sysmon + the daemons + up to TERM_WIN_MAX (kernel/gui.c)
 * per-window terminal shells + apps and their threads (kernel/app.c). */
#define TASK_MAX         48
#define TASK_NAME_MAX    24
/* 16 KiB stack per created task, matching the boot stack (boot/boot.asm)
 * that task 0 already runs on: created tasks can now run a full shell
 * instance (kernel/gui.c spawns one per GUI terminal window), which nests
 * shell_readline()/dispatch()/command call frames just as deep as the
 * main console shell does. The old 4 KiB was fine only while the sole
 * created task was sysmon's tiny sampling loop. */
#define TASK_STACK_WORDS 4096

typedef enum {
    TASK_UNUSED = 0,
    TASK_RUNNING,
    TASK_READY,
    TASK_SLEEPING,
    TASK_AWAY            /* running app code on another processor core (or waiting for one) */
} task_state_t;

typedef struct {
    uint32_t     pid;
    char         name[TASK_NAME_MAX];
    task_state_t state;
    uint32_t     cpu_pct;
    uint32_t     ticks_total;
    int          cpu;       /* the core it runs on (0 = the boot core), -1 waiting for one */
    int          prio;      /* TASK_PRIO_* (from nice: < 0 high, >= 5 background) */
    int          nice;      /* -20 (most CPU) .. 19 (least) */
    uint32_t     weight;    /* its share: 1024 at nice 0, x1.25 per step */
    uint32_t     vcsw;      /* switches: it gave the processor up (sleep, wait, yield) */
    uint32_t     icsw;      /* ... its time slice ended / a woken task took over */
    uint32_t     lat_avg_us, lat_max_us;   /* from ready (woken) to running */
    uint32_t     longest_ms;               /* its longest run without letting others in */
} task_info_t;

/* How big a share of the boot core a task gets when several want it:
 * the desktop's task twice a normal one, daemons a quarter. These are
 * nice values -3, 0 and 6 (task_set_nice: any of -20 .. 19). */
#define TASK_PRIO_HIGH       0
#define TASK_PRIO_NORMAL     1
#define TASK_PRIO_BACKGROUND 2

#define TASK_NICE_MIN (-20)
#define TASK_NICE_MAX 19
/* nice value of task pid (`renice`, Task Manager): 0, or -1 (no such task) */
int  task_set_nice(int pid, int nice);
int  task_get_nice(int pid);
/* load averages over 1, 5 and 15 minutes (tasks wanting a processor), x100 */
void task_loadavg(uint32_t out[3]);

/* Turns the currently executing context (the boot stack) into task 0. */
void task_init(const char* main_task_name);

/* Spawns the real background stats-sampling thread ("sysmon"). */
void task_start_sysmon(void);

/* Creates a new cooperative kernel thread with its own 16 KiB stack.
 * `entry` takes no arguments and is expected to run forever, calling
 * task_yield()/task_sleep_ms() so other tasks get the CPU. */
int  task_create(const char* name, void (*entry)(void));  /* pid, or -1 if full */
/* Same, on a kmalloc'd stack of stack_bytes (the web browser and the PHP
 * pages of httpd run deeply recursive interpreters). */
int  task_create_stack(const char* name, void (*entry)(void), uint32_t stack_bytes);

/* Voluntarily gives up the CPU to the READY task that has had the least
 * of it (weighted by priority). If nothing else is READY, returns to the
 * caller immediately. */
void task_yield(void);

/* Long CPU work (page layout, scripts, TLS) calls this often: it gives up
 * the CPU only once the task has run for TASK_SLICE_MS, so it is cheap to
 * call in loops and keeps the desktop responsive. */
#define TASK_SLICE_MS 8
void task_maybe_yield(void);
/* 1 when the current task should let another run now: its time slice is
 * over (8 ms, less when many tasks want the processor), or - after at
 * least 1 ms - a sleeping task woke up (a key, a timer, a packet): the
 * woken one takes over at once instead of waiting for the slice. */
int  task_should_yield(void);

/* Real sleep: marks the calling task SLEEPING and does not resume it
 * until at least `ms` milliseconds have passed (or task_wake()). */
void task_sleep_ms(uint32_t ms);

/* Ends a task's sleep early (next scheduling pass). IRQ-safe: drivers
 * use it to wake whoever is waiting on their device. */
void task_wake(int pid);
int  task_current_pid(void);

/* Marks the calling task as a daemon (sshd, httpd, ...): it has no
 * terminal of its own, so it never reads the keyboard and Ctrl+C never
 * interrupts it (net_interrupted() is 0) - that input belongs to the
 * shells. Daemons also get the background priority. */
void task_set_background(void);
int  task_is_background(void);
/* TASK_PRIO_* of the calling task */
void task_set_priority(int prio);

int  task_count(void);
void task_snapshot(task_info_t* out, int max_count);
const char* task_state_str(task_state_t s);
/* how busy processor core `cpu` was over the last sysmon sample (0-100) */
uint32_t task_core_pct(int cpu);

/* ── other processor cores (kernel/smp.c starts them) ───────────────
 * Kernel code only ever runs on the boot core: the kernel is written for
 * one processor. What runs on the others is app code - an app's main
 * thread or its threads (kernel/app.c), whose code touches only the app's
 * own memory. When such a task is interrupted in its own code on the boot
 * core, task_offload() hands it to an idle core; every system call it
 * makes there (and every fault) first brings it back to the boot core. */

/* app_preempt(): the current task is in app code - continue it on
 * another core if one is free. Returns 1 once it runs again (anywhere). */
int  task_offload(void);
/* the calling task never runs on another core (the desktop's own task) */
void task_pin(void);
/* on another core: the current task goes back to the boot core (system calls) */
void task_ap_go_home(void);
/* an IPI arrived (kernel/idt.c) */
void task_ipi(void);
/* a CPU exception on another core: the app is stopped on the boot core
 * (returns only if no task was running there - a kernel bug, then) */
void task_ap_fault(uint32_t vector, uint32_t err, uintptr_t ip, uintptr_t addr);
/* the boot core's timer, every millisecond */
void task_smp_tick(void);
/* another core's loop (kernel/smp.c), interrupts off: never returns */
void task_ap_loop(int cpu) __attribute__((noreturn));

#endif
