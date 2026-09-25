#include "task.h"
#include "timer.h"
#include "terminal.h"

/*
 * Real cooperative kernel threads.
 *
 * Each task owns its own stack. task_switch() (kernel/task_switch.asm) saves
 * the outgoing task's callee-saved registers on its own stack, records the
 * resulting esp, then loads esp for the incoming task and restores its
 * registers with a plain `ret`. The timer IRQ only counts time - it never
 * switches tasks - so switches only happen where a task calls
 * task_yield()/task_sleep_ms() itself: cooperative, not preemptive,
 * multitasking. When every task is asleep the scheduler halts the CPU
 * until the next interrupt. CPU% and per-task run times are real: they
 * are measured with timer_ms() across each task's actual run span, and
 * halted (idle) time is charged to no task.
 */

typedef struct {
    uint32_t*    sp;                      /* saved esp when not running */
    uint32_t     stack[TASK_STACK_WORDS]; /* only used for created tasks */
    uint32_t     pid;
    char         name[TASK_NAME_MAX];
    task_state_t state;
    void         (*entry)(void);
    /* all times below are in milliseconds (timer_ms()) */
    uint32_t     sleep_until_tick;
    uint32_t     run_start_tick;
    uint32_t     window_ticks;   /* real ms run since last cpu_pct sample */
    uint32_t     life_ticks;     /* real ms accumulated for the task's life */
    uint32_t     cpu_pct;
    int          vt;             /* terminal output target, restored on switch-in */
    int          background;     /* daemon: never reads the keyboard (task_set_background) */
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
    terminal_vt_set_active(g_current->vt);
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
    t->run_start_tick = timer_ms();

    g_count = 1;
    g_current = t;
}

int task_create(const char* name, void (*entry)(void)) {
    if (g_count >= TASK_MAX) return -1;

    task_t* t = &g_tasks[g_count];
    t->pid = (uint32_t)g_count;
    set_name(t->name, name, TASK_NAME_MAX);
    t->state = TASK_READY;
    t->entry = entry;
    t->sleep_until_tick = 0;
    t->window_ticks = 0;
    t->life_ticks = 0;
    t->cpu_pct = 0;
    t->run_start_tick = timer_ms();
    t->vt = 0;
    t->background = 0;

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
    return (int)t->pid;
}

static void wake_expired(uint32_t now_ms) {
    for (int i = 0; i < g_count; i++) {
        if (g_tasks[i].state == TASK_SLEEPING &&
            (int32_t)(now_ms - g_tasks[i].sleep_until_tick) >= 0) {
            g_tasks[i].state = TASK_READY;
        }
    }
}

void task_yield(void) {
    uint32_t now = timer_ms();
    task_t* cur = g_current;

    uint32_t elapsed = now - cur->run_start_tick;
    cur->window_ticks += elapsed;
    cur->life_ticks += elapsed;

    if (cur->state == TASK_RUNNING) cur->state = TASK_READY;

    int cur_idx = task_index(cur);
    int next_idx = -1;
    for (;;) {
        wake_expired(timer_ms());
        /* Round robin starting after cur; cur itself is the last choice
         * (step == g_count), so a lone READY task keeps running. */
        for (int step = 1; step <= g_count; step++) {
            int cand = (cur_idx + step) % g_count;
            if (g_tasks[cand].state == TASK_READY) { next_idx = cand; break; }
        }
        if (next_idx >= 0) break;
        /* Every task is asleep: halt until the next interrupt (timer, or
         * a device that task_wake()s a sleeper) instead of spinning. The
         * halted time is idle time, charged to no task. */
        timer_idle();
    }

    task_t* nxt = &g_tasks[next_idx];
    nxt->state = TASK_RUNNING;
    nxt->run_start_tick = timer_ms();
    if (nxt == cur) return;

    /* every task has its own idea of which terminal it writes to: a
     * background command (or another window) running in between must
     * not leave its terminal selected for us */
    cur->vt = terminal_vt_get_active();
    g_current = nxt;
    task_switch(&cur->sp, nxt->sp);

    /* We only get here once some other task switches back into `cur`.
     * g_current was set to `cur` by whoever scheduled us back in. */
    g_current->run_start_tick = timer_ms();
    terminal_vt_set_active(g_current->vt);
}

void task_sleep_ms(uint32_t ms) {
    if (ms == 0) ms = 1;
    g_current->sleep_until_tick = timer_ms() + ms;
    g_current->state = TASK_SLEEPING;
    /* task_yield() only picks READY tasks, and a sleeper only becomes
     * READY once its deadline passes (or task_wake()), so one call is
     * enough - no early-wakeup guard loop needed. */
    task_yield();
}

int task_current_pid(void) {
    return g_current ? (int)g_current->pid : 0;
}

void task_wake(int pid) {
    /* Safe from IRQ context: only moves the deadline, and the scheduler
     * re-checks deadlines after every hlt. */
    if (pid < 0 || pid >= g_count) return;
    if (g_tasks[pid].state == TASK_SLEEPING)
        g_tasks[pid].sleep_until_tick = timer_ms();
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
        out[i].ticks_total = g_tasks[i].life_ticks / 10u; /* 100 Hz ticks */
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
    /* Percent of wall-clock time since the last sample, so an idle
     * system shows mostly-0% tasks instead of shares that always sum
     * to 100%. */
    static uint32_t last_sample_ms = 0;
    uint32_t now = timer_ms();
    uint32_t total = now - last_sample_ms;
    last_sample_ms = now;
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

void task_set_background(void) {
    g_current->background = 1;
}

int task_is_background(void) {
    return g_current ? g_current->background : 0;
}
