#include "task.h"
#include "timer.h"

/*
 * Real cooperative kernel threads.
 *
 * Each task owns its own stack. task_switch() (kernel/task_switch.asm) saves
 * the outgoing task's callee-saved registers on its own stack, records the
 * resulting esp, then loads esp for the incoming task and restores its
 * registers with a plain `ret`. There is no timer IRQ in this kernel (the
 * PIT is polled, see timer.c), so switches only happen where a task calls
 * task_yield()/task_sleep_ms() itself - this is cooperative, not preemptive,
 * multitasking. CPU% and per-task tick counts are real: they are measured
 * from timer_ticks() across each task's actual run span, not invented.
 */

typedef struct {
    uint32_t*    sp;                      /* saved esp when not running */
    uint32_t     stack[TASK_STACK_WORDS]; /* only used for created tasks */
    uint32_t     pid;
    char         name[TASK_NAME_MAX];
    task_state_t state;
    void         (*entry)(void);
    uint32_t     sleep_until_tick;
    uint32_t     run_start_tick;
    uint32_t     window_ticks;   /* real ticks since last cpu_pct sample */
    uint32_t     life_ticks;     /* real ticks accumulated for the task's life */
    uint32_t     cpu_pct;
} task_t;

static task_t  g_tasks[TASK_MAX];
static int     g_count = 0;
static task_t* g_current = NULL;

extern void task_switch(uint32_t** old_sp_store, uint32_t* new_sp);

static void set_name(char* dst, const char* src, int maxlen) {
    int i = 0;
    while (i < maxlen - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int task_index(const task_t* t) {
    return (int)(t - g_tasks);
}

/* Reached only if a task's entry function ever returns (it shouldn't). */
static void task_exit_stub(void) {
    for (;;) __asm__ volatile("hlt");
}

/* Landing pad for a freshly created task's first switch-in. task_switch()
 * reaches this via `ret`, so on entry esp already looks exactly like a
 * normal 0-argument call - no arguments need passing here. */
static void task_trampoline(void) {
    g_current->entry();
    g_current->state = TASK_UNUSED;
    for (;;) task_yield();
}

void task_init(const char* main_task_name) {
    for (int i = 0; i < TASK_MAX; i++) g_tasks[i].state = TASK_UNUSED;

    task_t* t = &g_tasks[0];
    t->pid = 0;
    set_name(t->name, main_task_name, TASK_NAME_MAX);
    t->state = TASK_RUNNING;
    t->entry = NULL;
    t->sp = NULL;
    t->sleep_until_tick = 0;
    t->window_ticks = 0;
    t->life_ticks = 0;
    t->cpu_pct = 0;
    t->run_start_tick = timer_ticks();

    g_count = 1;
    g_current = t;
}

void task_create(const char* name, void (*entry)(void)) {
    if (g_count >= TASK_MAX) return;

    task_t* t = &g_tasks[g_count];
    t->pid = (uint32_t)g_count;
    set_name(t->name, name, TASK_NAME_MAX);
    t->state = TASK_READY;
    t->entry = entry;
    t->sleep_until_tick = 0;
    t->window_ticks = 0;
    t->life_ticks = 0;
    t->cpu_pct = 0;
    t->run_start_tick = timer_ticks();

    /* Build a fake call frame so task_switch()'s epilogue (pop edi/esi/ebx/
     * ebp; ret) lands straight in task_trampoline() as if it had just been
     * called with zero arguments. */
    uint32_t* sp = &t->stack[TASK_STACK_WORDS];
    sp -= 6;
    sp[0] = 0;                              /* edi */
    sp[1] = 0;                              /* esi */
    sp[2] = 0;                              /* ebx */
    sp[3] = 0;                              /* ebp */
    sp[4] = (uint32_t)task_trampoline;      /* task_switch's `ret` target */
    sp[5] = (uint32_t)task_exit_stub;       /* trampoline's own `ret` target */
    t->sp = sp;

    g_count++;
}

void task_yield(void) {
    timer_poll();
    uint32_t now = timer_ticks();
    task_t* cur = g_current;

    uint32_t elapsed = now - cur->run_start_tick;
    cur->window_ticks += elapsed;
    cur->life_ticks += elapsed;

    if (cur->state == TASK_RUNNING) cur->state = TASK_READY;

    for (int i = 0; i < g_count; i++) {
        if (g_tasks[i].state == TASK_SLEEPING &&
            (int32_t)(now - g_tasks[i].sleep_until_tick) >= 0) {
            g_tasks[i].state = TASK_READY;
        }
    }

    int cur_idx = task_index(cur);
    int next_idx = -1;
    for (int step = 1; step <= g_count; step++) {
        int cand = (cur_idx + step) % g_count;
        if (g_tasks[cand].state == TASK_READY) { next_idx = cand; break; }
    }

    if (next_idx < 0) {
        /* Nothing else is READY. If we were the one asking to sleep, there
         * is genuinely nothing else to run, so wait for real time to pass
         * instead of returning early. */
        while (cur->state == TASK_SLEEPING &&
               (int32_t)(timer_ticks() - cur->sleep_until_tick) < 0) {
            timer_poll();
        }
        cur->state = TASK_RUNNING;
        cur->run_start_tick = timer_ticks();
        return;
    }

    task_t* nxt = &g_tasks[next_idx];
    nxt->state = TASK_RUNNING;
    nxt->run_start_tick = now;
    g_current = nxt;

    task_switch(&cur->sp, nxt->sp);

    /* We only get here once some other task switches back into `cur`.
     * g_current was set to `cur` by whoever scheduled us back in. */
    g_current->run_start_tick = timer_ticks();
}

void task_sleep_ms(uint32_t ms) {
    timer_poll();
    uint32_t now = timer_ticks();
    uint32_t wraps = (ms * 100u + 999u) / 1000u; /* PIT runs at 100 Hz */
    if (wraps == 0) wraps = 1;
    uint32_t target = now + wraps;

    g_current->sleep_until_tick = target;
    g_current->state = TASK_SLEEPING;
    task_yield();

    /* Guard against being woken slightly early by scheduling latency. */
    while ((int32_t)(timer_ticks() - target) < 0) {
        task_yield();
    }
}

int task_count(void) {
    return g_count;
}

void task_snapshot(task_info_t* out, int max_count) {
    int n = (g_count < max_count) ? g_count : max_count;
    for (int i = 0; i < n; i++) {
        out[i].pid = g_tasks[i].pid;
        set_name(out[i].name, g_tasks[i].name, TASK_NAME_MAX);
        out[i].state = g_tasks[i].state;
        out[i].cpu_pct = g_tasks[i].cpu_pct;
        out[i].ticks_total = g_tasks[i].life_ticks;
    }
}

const char* task_state_str(task_state_t s) {
    if (s == TASK_RUNNING)  return "running";
    if (s == TASK_READY)    return "ready";
    if (s == TASK_SLEEPING) return "sleep";
    return "unused";
}

/* ── sysmon: a real background thread ───────────────────────────────
 * Wakes on its own schedule (task_sleep_ms, not driven by top/shell) and
 * samples real per-task run time into cpu_pct. This is the same kind of
 * work a userspace `top`-feeding daemon does on a real OS: periodic
 * sampling, decoupled from whether anyone is currently looking at it. */
static void recompute_cpu_window(void) {
    uint32_t total = 0;
    for (int i = 0; i < g_count; i++) total += g_tasks[i].window_ticks;
    if (total == 0) total = 1;
    for (int i = 0; i < g_count; i++) {
        g_tasks[i].cpu_pct = (g_tasks[i].window_ticks * 100u) / total;
        g_tasks[i].window_ticks = 0;
    }
}

static void sysmon_entry(void) {
    for (;;) {
        recompute_cpu_window();
        task_sleep_ms(200);
    }
}

void task_start_sysmon(void) {
    task_create("sysmon", sysmon_entry);
}
