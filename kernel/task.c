#include "task.h"
#include "timer.h"
#include "terminal.h"
#include "kheap.h"
#include "smp.h"
#include "app.h"
#include "serial.h"

/*
 * Real cooperative kernel threads, scheduled fairly.
 *
 * Each task owns its own stack. task_switch() (kernel/task_switch.asm) saves
 * the outgoing task's callee-saved registers on its own stack, records the
 * resulting esp, then loads esp for the incoming task and restores its
 * registers with a plain `ret`. Kernel code switches only where a task
 * calls task_yield()/task_sleep_ms() itself (app code is also switched
 * away from by the timer - kernel/app.c, app_preempt). When every task is
 * asleep the scheduler halts the CPU until the next interrupt.
 *
 * Which task runs next: the READY one that has had the least processor
 * time so far ("virtual run time"), counted in TSC cycles and weighted by
 * its priority - the desktop's task counts its time at half rate, the
 * daemons (autosave, sshd...) at four times, so under load the desktop
 * gets twice a normal task's share and a daemon a quarter. A task that
 * wakes from a sleep (a key press, a timer, a packet) starts just ahead of
 * the others instead of at the back of a queue, so it runs at the next
 * switch: interactive tasks answer quickly even while others compute. A
 * task that slept is not credited the whole sleep, though - only a few
 * ms - so it cannot then hog the CPU to "catch up".
 *
 * CPU% and run times are real: measured with the TSC across each task's
 * actual run spans; halted (idle) time is charged to no task.
 */

typedef struct task {
    uintptr_t*   sp;                      /* saved stack pointer when not running */
    uintptr_t    stack[TASK_STACK_WORDS] __attribute__((aligned(16))); /* created tasks only */
    uint32_t     pid;
    char         name[TASK_NAME_MAX];
    volatile task_state_t state;
    void         (*entry)(void);
    uint32_t     sleep_until_tick;        /* timer_ms() */
    uint64_t     run_start;               /* TSC when it got the boot core */
    uint64_t     window_cyc;              /* boot-core time since the last cpu_pct sample */
    uint64_t     life_cyc;                /* ... for the task's life */
    volatile uint32_t away_window_ms;     /* time on other cores (added by them) */
    uint32_t     away_life_ms;
    uint32_t     cpu_pct;
    uint64_t     vruntime;                /* weighted cycles: who runs next */
    uint32_t     weight;                  /* 2048 high, 1024 normal, 256 background */
    int          prio;
    int          pinned;                  /* never runs on another core */
    volatile int cpu;                     /* 0 boot core, n another core, -1 waiting for one */
    volatile int came_home;               /* back from another core: vruntime caught up first */
    int          vt;                      /* terminal output target, restored on switch-in */
    int          background;              /* daemon: never reads the keyboard (task_set_background) */
    void*        stack_mem;               /* kmalloc'd stack (task_create_stack), or NULL */
    uint32_t     fault_vec, fault_err;    /* a fault on another core, reported on the boot core */
    uintptr_t    fault_ip, fault_addr;
    uint8_t      fpu[512] __attribute__((aligned(16)));   /* FPU/SSE registers while switched out */
} task_t;

static task_t           g_tasks[TASK_MAX];
static int              g_count = 0;
static task_t* volatile g_current = NULL;   /* on the boot core */
static task_t           g_idle;             /* the boot core's idle loop (see task_offload) */
static task_t*          g_prev;             /* switched away from: finish_switch() */
static uint64_t         g_min_vr;           /* the smallest vruntime of the runnable tasks */
static uint64_t         g_tsc_per_ms = 1000000;

extern void task_switch(uintptr_t** old_sp_store, uintptr_t* new_sp);

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline uintptr_t irq_save(void) {
    uintptr_t f;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uintptr_t f) {
    if (f & 0x200) __asm__ volatile("sti" ::: "memory");
}

static void set_name(char* dst, const char* src, int maxlen) {
    int i = 0;
    while (i < maxlen - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int task_index(const task_t* t) {
    return (int)(t - g_tasks);
}

static uint32_t weight_of(int prio) {
    return prio == TASK_PRIO_HIGH ? 2048u : prio == TASK_PRIO_BACKGROUND ? 256u : 1024u;
}

/* Reached only if a task's entry function ever returns (it shouldn't). */
static void task_exit_stub(void) {
    for (;;) __asm__ volatile("hlt");
}

/* Every task has its own FPU/SSE registers (apps compute with float and
 * double; the browser's JavaScript uses the x87): saved when a task is
 * switched out, restored when it comes back. fxsave when the CPU has it
 * (it also covers SSE), the older fnsave otherwise. */
static int g_fxsr = -1;

static int have_fxsr(void) {
    if (g_fxsr < 0) {
        uint32_t a = 1, b, c, d;
        __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
        g_fxsr = (d >> 24) & 1;
    }
    return g_fxsr;
}

static void fpu_save(uint8_t* area) {
    if (have_fxsr()) __asm__ volatile("fxsave (%0)" :: "r"(area) : "memory");
    else __asm__ volatile("fnsave (%0)" :: "r"(area) : "memory");
}

static void fpu_restore(uint8_t* area) {
    if (have_fxsr()) __asm__ volatile("fxrstor (%0)" :: "r"(area) : "memory");
    else __asm__ volatile("frstor (%0)" :: "r"(area) : "memory");
}

/* ── tasks waiting for another core ──────────────────────────────── */

static volatile int g_qlock;
static task_t*      g_q[TASK_MAX];
static volatile int g_qhead, g_qlen;

static void q_lock(void)   { while (__sync_lock_test_and_set(&g_qlock, 1)) while (g_qlock) __asm__ volatile("pause"); }
static void q_unlock(void) { __sync_lock_release(&g_qlock); }

typedef struct {
    uintptr_t*       idle_sp;     /* its loop's stack while a task runs */
    task_t* volatile task;
    volatile int     idle;        /* halted, waiting for work */
    volatile int     reason;      /* why the task stopped */
    volatile uint32_t since;      /* timer_ms() when the task got it */
    volatile int     used;        /* has run app code: 1, logged: 2 */
    struct task*     first;       /* the first task it ran */
} ap_t;

#define AP_REQUEUE 1              /* others are waiting: back into the queue */
#define AP_HOME    2              /* back to the boot core */
#define AP_SLICE_MS 20            /* a core's turn when more tasks than cores want one */
#define AP_HOME_MS  100           /* the boot core sees each task this often (Ctrl+C, End task, painting) */

static ap_t g_ap[SMP_MAX_CPUS];

/* an idle core is woken for it (interrupts off) */
static void q_push(task_t* t) {
    uintptr_t f = irq_save();
    q_lock();
    g_q[(g_qhead + g_qlen) % TASK_MAX] = t;
    g_qlen++;
    q_unlock();
    for (int c = 1; c < cpu_count(); c++)
        if (g_ap[c].idle) { smp_send_ipi(c, SMP_VEC_KICK); break; }
    irq_restore(f);
}

static task_t* q_pop(void) {
    if (!g_qlen) return NULL;
    uintptr_t f = irq_save();
    q_lock();
    task_t* t = NULL;
    if (g_qlen) {
        t = g_q[g_qhead];
        g_qhead = (g_qhead + 1) % TASK_MAX;
        g_qlen--;
    }
    q_unlock();
    irq_restore(f);
    return t;
}

static int idle_cores(void) {
    int n = 0;
    for (int c = 1; c < cpu_count(); c++) if (g_ap[c].idle) n++;
    return n;
}

/* ── the boot core's scheduler ───────────────────────────────────── */

/* the time since it got the CPU: its statistics and its virtual run time */
static void account(task_t* t) {
    uint64_t now = rdtsc();
    uint64_t d = now - t->run_start;
    t->run_start = now;
    if (t == &g_idle) return;
    t->window_cyc += d;
    t->life_cyc += d;
    t->vruntime += d * 1024u / t->weight;
}

/* a task that slept (or came back from another core) starts just ahead of
 * the others - a few ms of credit, not the whole time it was away */
static void catch_up(task_t* t) {
    uint64_t credit = 6u * g_tsc_per_ms;
    uint64_t floor = g_min_vr > credit ? g_min_vr - credit : 0;
    if (t->vruntime < floor) t->vruntime = floor;
}

static void wake_expired(uint32_t now_ms) {
    for (int i = 0; i < g_count; i++) {
        task_t* t = &g_tasks[i];
        if (t->state == TASK_SLEEPING && (int32_t)(now_ms - t->sleep_until_tick) >= 0) {
            catch_up(t);
            t->state = TASK_READY;
        }
    }
}

/* The READY task with the least virtual run time other than cur (ties go
 * round robin after cur); cur itself only if nothing else is READY. */
static task_t* pick_next(task_t* cur) {
    task_t* best = NULL;
    uint64_t min_vr = ~0ull;
    int start = (cur == &g_idle || !cur) ? 0 : task_index(cur) + 1;
    for (int k = 0; k < g_count; k++) {
        task_t* t = &g_tasks[(start + k) % g_count];
        if (t->state != TASK_READY && t->state != TASK_RUNNING) continue;
        if (t->came_home) { t->came_home = 0; catch_up(t); }
        if (t->vruntime < min_vr) min_vr = t->vruntime;
        if (t->state != TASK_READY || t == cur) continue;
        if (!best || t->vruntime < best->vruntime) best = t;
    }
    if (min_vr != ~0ull && min_vr > g_min_vr) g_min_vr = min_vr;
    if (!best && cur && cur != &g_idle && cur->state == TASK_READY) best = cur;
    return best;
}

/* runs in the task switched to, right after the switch: the task just left
 * is now off its stack - one going to another core can be queued now */
static void finish_switch(void) {
    task_t* p = g_prev;
    g_prev = NULL;
    if (p && p->state == TASK_AWAY && p->cpu < 0) q_push(p);
}

/* back in a task after a switch - on the boot core, or on another one */
static void after_switch(task_t* self) {
    fpu_restore(self->fpu);
    if (cpu_id() != 0) return;
    finish_switch();
    self->run_start = rdtsc();
    /* every task has its own idea of which terminal it writes to */
    terminal_vt_set_active(self->vt);
}

static void switch_to(task_t* cur, task_t* nxt) {
    nxt->state = TASK_RUNNING;
    nxt->cpu = 0;
    nxt->run_start = rdtsc();
    if (nxt == cur) return;
    /* a background command (or another window) running in between must
     * not leave its terminal selected for us */
    cur->vt = terminal_vt_get_active();
    g_current = nxt;
    g_prev = cur;
    fpu_save(cur->fpu);
    task_switch(&cur->sp, nxt->sp);
    after_switch(cur);
}

/* Landing pad for a freshly created task's first switch-in. task_switch()
 * reaches this via `ret`, so on entry esp already looks exactly like a
 * normal 0-argument call - no arguments need passing here. */
static void task_trampoline(void) {
    finish_switch();
    /* a fresh task starts with clean FPU/SSE registers */
    __asm__ volatile("fninit");
    if (have_fxsr()) {
        uint32_t mxcsr = 0x1F80;
        __asm__ volatile("ldmxcsr %0" :: "m"(mxcsr));
    }
    g_current->run_start = rdtsc();
    terminal_vt_set_active(g_current->vt);
    __asm__ volatile("sti");
    g_current->entry();
    g_current->state = TASK_UNUSED;
    for (;;) task_yield();
}

/* a fake call frame so task_switch()'s epilogue (pop the callee-saved
 * registers; ret) lands in `to` as if it had just been called with zero
 * arguments, on the stack below top */
static uintptr_t* fake_frame(uintptr_t* sp, void (*to)(void)) {
#ifdef __x86_64__
    /* r15 r14 r13 r12 rbx rbp, then the return addresses; the slot
     * after `to`'s address sits at 8 mod 16, like after a call */
    sp -= 8;
    for (int i = 0; i < 6; i++) sp[i] = 0;
    sp[6] = (uintptr_t)to;                  /* task_switch's `ret` target */
    sp[7] = (uintptr_t)task_exit_stub;      /* `to`'s own `ret` target */
#else
    sp -= 6;
    for (int i = 0; i < 4; i++) sp[i] = 0;  /* edi esi ebx ebp */
    sp[4] = (uintptr_t)to;
    sp[5] = (uintptr_t)task_exit_stub;
#endif
    return sp;
}

static void idle_entry(void) {
    for (;;) task_yield();
}

/* the TSC's rate, against the 1 kHz timer: run times in cycles */
static void calibrate_tsc(void) {
    if (timer_tsc_per_ms()) { g_tsc_per_ms = timer_tsc_per_ms(); return; }   /* measured by timer_init */
    uint32_t t0 = timer_ms();
    while (timer_ms() == t0) timer_idle();
    uint64_t c0 = rdtsc();
    t0 = timer_ms();
    while (timer_ms() - t0 < 20) timer_idle();
    uint64_t per = (rdtsc() - c0) / (uint64_t)(timer_ms() - t0);
    if (per > 1000) g_tsc_per_ms = per;
}

void task_init(const char* main_task_name) {
    for (int i = 0; i < TASK_MAX; i++) g_tasks[i].state = TASK_UNUSED;
    calibrate_tsc();

    task_t* t = &g_tasks[0];
    t->pid = 0;
    set_name(t->name, main_task_name, TASK_NAME_MAX);
    t->state = TASK_RUNNING;
    t->entry = NULL;
    t->sp = NULL;
    t->prio = TASK_PRIO_HIGH;               /* the desktop and the main shell */
    t->weight = weight_of(t->prio);
    t->pinned = 1;
    t->run_start = rdtsc();

    set_name(g_idle.name, "idle", TASK_NAME_MAX);
    g_idle.entry = idle_entry;
    g_idle.weight = 1024;
    g_idle.pinned = 1;
    g_idle.sp = fake_frame(&g_idle.stack[TASK_STACK_WORDS], task_trampoline);

    g_count = 1;
    g_current = t;
}

int task_create(const char* name, void (*entry)(void)) {
    return task_create_stack(name, entry, 0);
}

int task_create_stack(const char* name, void (*entry)(void), uint32_t stack_bytes) {
    /* a finished task's slot (apps run as tasks that end) is reused */
    task_t* t = NULL;
    for (int i = 1; i < g_count; i++) {
        if (g_tasks[i].state == TASK_UNUSED && &g_tasks[i] != g_current) { t = &g_tasks[i]; break; }
    }
    if (!t) {
        if (g_count >= TASK_MAX) return -1;
        t = &g_tasks[g_count];
        t->pid = (uint32_t)g_count;
        t->stack_mem = NULL;
        g_count++;
    }
    /* the old stack of a reused slot is free now: nothing runs on it */
    if (t->stack_mem) { kfree(t->stack_mem); t->stack_mem = NULL; }
    set_name(t->name, name, TASK_NAME_MAX);
    t->entry = entry;
    t->sleep_until_tick = 0;
    t->window_cyc = 0;
    t->life_cyc = 0;
    t->away_window_ms = 0;
    t->away_life_ms = 0;
    t->cpu_pct = 0;
    t->vt = 0;
    t->background = 0;
    t->prio = TASK_PRIO_NORMAL;
    t->weight = weight_of(t->prio);
    t->pinned = 0;
    t->cpu = 0;
    t->came_home = 0;
    t->vruntime = g_min_vr;                 /* a new task starts level with the others */

    uintptr_t* sp = &t->stack[TASK_STACK_WORDS];
    if (stack_bytes > sizeof(t->stack)) {
        uint8_t* mem = (uint8_t*)kmalloc(stack_bytes + 16);
        if (!mem) { t->state = TASK_UNUSED; return -1; }
        t->stack_mem = mem;
        sp = (uintptr_t*)(((uintptr_t)mem + stack_bytes) & ~(uintptr_t)15);
    }
    t->sp = fake_frame(sp, task_trampoline);

    t->state = TASK_READY;
    return (int)t->pid;
}

void task_yield(void) {
    task_t* cur = g_current;
    account(cur);
    if (cur->state == TASK_RUNNING) cur->state = TASK_READY;

    task_t* nxt;
    for (;;) {
        wake_expired(timer_ms());
        nxt = pick_next(cur);
        if (nxt) break;
        /* every core busy and app code waiting for one: run it here meanwhile */
        if (g_qlen && !idle_cores()) {
            task_t* s = q_pop();
            if (s) { s->cpu = 0; s->came_home = 1; s->state = TASK_READY; continue; }
        }
        /* Every task is asleep: halt until the next interrupt (timer, a
         * device that task_wake()s a sleeper, or a core sending a task
         * back) instead of spinning. Halted time is charged to no task. */
        timer_idle();
    }
    switch_to(cur, nxt);
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
    uint64_t per_tick = g_tsc_per_ms * 10u;  /* 100 Hz ticks */
    for (int i = 0; i < n; i++) {
        task_t* t = &g_tasks[i];
        out[i].pid = t->pid;
        set_name(out[i].name, t->name, TASK_NAME_MAX);
        out[i].state = t->state;
        out[i].cpu_pct = t->cpu_pct;
        out[i].ticks_total = (uint32_t)(t->life_cyc / per_tick) + t->away_life_ms / 10u;
        out[i].cpu = t->state == TASK_AWAY ? t->cpu : 0;
        out[i].prio = t->prio;
    }
}

const char* task_state_str(task_state_t s) {
    if (s == TASK_RUNNING)  return "running";
    if (s == TASK_READY)    return "ready";
    if (s == TASK_SLEEPING) return "sleep";
    if (s == TASK_AWAY)     return "core";
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
     * to 100%. Time on other cores counts too (a task can show up to
     * 100% of one core). */
    static uint32_t last_sample_ms = 0;
    uint32_t now = timer_ms();
    uint32_t total = now - last_sample_ms;
    last_sample_ms = now;
    if (total == 0) total = 1;
    for (int i = 0; i < g_count; i++) {
        task_t* t = &g_tasks[i];
        uint32_t away = __sync_fetch_and_and(&t->away_window_ms, 0);
        t->away_life_ms += away;
        uint32_t ms = (uint32_t)(t->window_cyc / g_tsc_per_ms) + away;
        t->window_cyc = 0;
        uint32_t pct = ms * 100u / total;
        t->cpu_pct = pct > 100 ? 100 : pct;
    }
}

static void sysmon_entry(void) {
    task_set_background();
    for (;;) {
        recompute_cpu_window();
        for (int c = 1; c < cpu_count(); c++)
            if (g_ap[c].used == 1) {
                g_ap[c].used = 2;
                klog("smp: core %d runs app code (first: %s)\n", c, g_ap[c].first->name);
            }
        task_sleep_ms(200);
    }
}

void task_start_sysmon(void) {
    task_create("sysmon", sysmon_entry);
}

void task_set_background(void) {
    g_current->background = 1;
    task_set_priority(TASK_PRIO_BACKGROUND);
}

int task_is_background(void) {
    return g_current ? g_current->background : 0;
}

void task_set_priority(int prio) {
    task_t* t = g_current;
    if (!t || t == &g_idle || prio < TASK_PRIO_HIGH || prio > TASK_PRIO_BACKGROUND) return;
    account(t);
    t->prio = prio;
    t->weight = weight_of(prio);
}

void task_maybe_yield(void) {
    task_t* t = g_current;
    if (t && rdtsc() - t->run_start >= (uint64_t)TASK_SLICE_MS * g_tsc_per_ms) task_yield();
}

/* ── other cores ─────────────────────────────────────────────────── */

void task_pin(void) {
    if (g_current) g_current->pinned = 1;
}

int task_offload(void) {
    task_t* cur = g_current;
    if (cpu_count() < 2 || cur == &g_idle || cur->pinned || cur->state != TASK_RUNNING) return 0;
    if (idle_cores() <= g_qlen) return 0;   /* no core free for it */
    /* (interrupts stay on, as in task_yield: the task switched to expects them) */
    account(cur);
    cur->state = TASK_AWAY;
    cur->cpu = -1;                          /* queued by finish_switch(), once off its stack */
    wake_expired(timer_ms());
    task_t* nxt = pick_next(cur);
    switch_to(cur, nxt ? nxt : &g_idle);
    /* running again - on another core, or back on this one */
    return 1;
}

/* the task running on this core stops here (interrupts off) */
static void ap_switch_out(ap_t* a, task_t* t, int reason) {
    a->reason = reason;
    fpu_save(t->fpu);
    task_switch(&t->sp, a->idle_sp);
    after_switch(t);
}

void task_ap_loop(int cpu) {
    ap_t* a = &g_ap[cpu];
    for (;;) {
        __asm__ volatile("cli");
        task_t* t = q_pop();
        if (!t) {
            a->idle = 1;
            /* "sti; hlt" is atomic: a kick sent after the queue check still wakes us */
            if (!g_qlen) __asm__ volatile("sti; hlt; cli" ::: "memory");
            continue;
        }
        a->idle = 0;
        a->since = timer_ms();
        t->cpu = cpu;
        a->task = t;
        if (!a->used) { a->first = t; a->used = 1; }   /* sysmon logs it (the kernel is not ours here) */
#ifdef __x86_64__
        /* the boot core may have changed page tables (guard pages) since */
        __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
#endif
        task_switch(&a->idle_sp, t->sp);
        /* t stopped: a system call, its turn ended, or a fault */
        a->task = NULL;
        __sync_fetch_and_add(&t->away_window_ms, timer_ms() - a->since);
        if (a->reason == AP_REQUEUE) {
            t->cpu = -1;
            q_push(t);
        } else {
            t->cpu = 0;
            t->came_home = 1;
            __sync_synchronize();
            t->state = TASK_READY;          /* its context is saved: the boot core may run it */
            smp_send_ipi(0, SMP_VEC_KICK);
        }
    }
}

void task_ap_go_home(void) {
    int c = cpu_id();
    if (c <= 0 || c >= SMP_MAX_CPUS || !g_ap[c].task) return;
    ap_switch_out(&g_ap[c], g_ap[c].task, AP_HOME);
}

void task_ipi(void) {
    int c = cpu_id();
    if (c <= 0 || c >= SMP_MAX_CPUS) return;   /* the boot core: woken from hlt, nothing else */
    ap_t* a = &g_ap[c];
    task_t* t = a->task;
    if (!t) return;                         /* the idle loop looks at the queue itself */
    uint32_t ran = timer_ms() - a->since;
    if (ran >= AP_HOME_MS) ap_switch_out(a, t, AP_HOME);
    else if (g_qlen && ran >= AP_SLICE_MS) ap_switch_out(a, t, AP_REQUEUE);
}

void task_smp_tick(void) {
    static uint32_t n;
    if (cpu_count() < 2 || ++n % 5) return;
    uint32_t now = timer_ms();
    int kicked = 0;
    for (int c = 1; c < cpu_count(); c++) {
        ap_t* a = &g_ap[c];
        if (!a->task) {
            if (g_qlen && a->idle && !kicked) { smp_send_ipi(c, SMP_VEC_KICK); kicked = 1; }
            continue;
        }
        uint32_t ran = now - a->since;
        if (ran >= AP_HOME_MS || (g_qlen && ran >= AP_SLICE_MS)) smp_send_ipi(c, SMP_VEC_KICK);
    }
}

/* the boot core takes over a task that faulted elsewhere: app_fault() stops the app */
static void ap_fault_landing(void) {
    finish_switch();
    __asm__ volatile("fninit");
    task_t* self = g_current;
    self->run_start = rdtsc();
    terminal_vt_set_active(self->vt);
    app_fault(self->fault_vec, self->fault_err, self->fault_ip, self->fault_addr);
    for (;;) task_yield();                  /* (app_fault does not return for an app) */
}

void task_ap_fault(uint32_t vector, uint32_t err, uintptr_t ip, uintptr_t addr) {
    int c = cpu_id();
    if (c <= 0 || c >= SMP_MAX_CPUS || vector == 2 || vector == 8 || vector == 18) return;
    ap_t* a = &g_ap[c];
    task_t* t = a->task;
    if (!t) return;
    /* where the task entered its app code: the kernel stack below that is free */
    uintptr_t ksp = app_fault_stack((int)t->pid);
    if (!ksp) return;
    t->fault_vec = vector;
    t->fault_err = err;
    t->fault_ip = ip;
    t->fault_addr = addr;
    t->sp = fake_frame((uintptr_t*)((ksp - 512) & ~(uintptr_t)15), ap_fault_landing);
    a->reason = AP_HOME;
    /* the faulting context is left behind: back to this core's loop */
    uintptr_t* dropped;
    task_switch(&dropped, a->idle_sp);
}
