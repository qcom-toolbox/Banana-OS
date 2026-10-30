/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "app.h"
#include "web_worker.h"
#include "i18n.h"
#include "gpu_virgl.h"
#include "gpu_i915.h"
#include "appwin.h"
#include "audio.h"
#include "fs.h"
#include "fsdisk.h"
#include "kheap.h"
#include "kstring.h"
#include "task.h"
#include "timer.h"
#include "terminal.h"
#include "keyboard.h"
#include "gui.h"
#include "rtc.h"
#include "tz.h"
#include "random.h"
#include "clipboard.h"
#include "serial.h"
#include "font8x8.h"
#include "paging.h"
#include "../net/http.h"
#include "../sdk/include/banana_api.h"
#include "smp.h"
#include "zswap.h"
#include "webview.h"
#include "font.h"
#include "pkg.h"
#include "repo.h"
#include "wallpaper.h"
#include "image.h"
#include "gamepad.h"
#include "umem.h"
#include "uspace.h"
#include "idt.h"
#include "uvm.h"

#ifdef __x86_64__
/* the SDK's heap tells kernel memory from mapped memory by this address */
_Static_assert(BANANA_VM_BASE == UVM_BASE, "BANANA_VM_BASE (banana_api.h) must be kernel/uvm.h's UVM_BASE");
#endif
#include "meminfo.h"
#include "hmem.h"
#include "upage.h"
#include "usock.h"
#include "../shell/shell.h"
#include "kfile.h"
#include "pty.h"
#include "vcons.h"
#include "random.h"
#include "../net/net.h"

/* file descriptors: 0..255 - files, sockets, pipes, the terminal (the SDK's
 * own - eventfd, epoll - come after them, from 256: sdk/lib/ufd.c).
 * Java keeps every jar of its class path open: Minecraft ran out at 64 */
#define APP_FDS     256
#define APP_STACK   (1u << 20)        /* 1 MiB */
#define APP_GUARD   (16u << 10)       /* unmapped pages under it (64-bit): an overflow faults */
#define STACK_CANARY 0xB4A4A5C0u      /* at the bottom of the stack: checked on every system call */
#define APP_KEYQ    32
#define APP_MAX_IMAGE (64u << 20)
#define APP_THREADS 64                /* extra threads per app (api->thread_create): a thread per core on big machines */
/* 64-bit: the kernel stack of each app thread - its system calls run there
 * (the app's own stack is in ring 3's reach), with a guard page under it */
#define APP_KSTACK  (128u << 10)

/* ── per-app state ─────────────────────────────────────────────────── */

/* a thread's signals: blocked and pending ones (bit sig - 1), its sigaltstack */
typedef struct {
    uint64_t  blocked, pending;
    uintptr_t alt_sp;
    uint64_t  alt_size;
    int       alt_flags;
} sigstate_t;

/* a thread an app started: a task of its own running app code on its own stack */
typedef struct {
    int       used;
    int       pid;             /* its task, -1 once it ended */
    int     (*fn)(void*);
    void*     arg;
    uint8_t*  stack;           /* stack_size bytes (the app's memory), APP_GUARD under them */
    size_t    stack_size;
    int       own_stack;       /* the app gave it (thread_start): not freed here */
    int       detached;        /* its record goes as soon as it ends (no thread_join) */
    uint8_t*  kstack;          /* 64-bit: its system calls' stack (kstack_new) */
    uintptr_t saved_sp;        /* app_enter(): where app_leave() returns to */
    int       running;         /* in app code (between app_enter() and its return) */
    int       done;
    int       ret;
    sigstate_t sig;            /* its mask, signals sent to it, its alternate stack */
} athread_t;

typedef struct {
    int       used;
    int       id;              /* slot index: the window owner id */
    int       pid;             /* the task it runs in */
    int       own_task;        /* started with app_spawn() */
    char      name[32];
    uspace_t* us;              /* 64-bit: its page tables (kernel/uspace.c) */
    umem_t*   mem;             /* everything it has: code, stacks, malloc() (kernel/umem.c) */
    uintptr_t base;            /* where the ELF was loaded (crash reports: app+offset) */
    banana_entry_t entry;
    uint8_t*  guard;           /* APP_GUARD bytes, page-aligned */
    uint8_t*  stack;           /* APP_STACK bytes above the guard */
    int       guarded;
    uint8_t*  kstack;          /* 64-bit: the main thread's system calls' stack */
    banana_api_t* uapi;        /* 64-bit: the call table in its memory (system call stubs) */
    uintptr_t ret_stub;        /* ... and where its entry (or a thread) returns to */
    char*     clip;            /* clipboard_get()'s copy, in its memory */
    /* its file descriptors: open file descriptions (kernel/kfile.c),
     * shared with the other descriptors dup made for them */
    kfile_t*  fdt[APP_FDS];
    uint8_t   cloexec[APP_FDS];
    uintptr_t saved_sp;        /* the kernel stack app_enter() left: exit() returns there */
    int       exit_code;
    int       vt;              /* the terminal it was started from */
    int       has_term;
    int       keyq[APP_KEYQ];
    int       kq_head, kq_tail;
    int       esc;             /* ESC [ parsing in getchar() */
    int       handles_int;     /* called interrupted(): Ctrl+C no longer stops it */
    int       interrupted;
    uint32_t  last_breath;
    int       kill_req;        /* Task Manager: End task */
    int       running;         /* between app_enter() and its return */
    char      crash_msg[192];  /* why it crashed (app_fault), printed by run() */
    int       argc;
    char**    argv;
    uintptr_t code_lo, code_hi; /* where its code is: the timer may switch away from there */
    athread_t threads[APP_THREADS];
    int       exit_req;        /* a thread called exit() / crashed: every thread stops */
    int       exit_req_code;
    int       seq;             /* its process id (getpid): counts the apps started since boot */
    /* signals (version 19): what each does, where handlers start, the main
     * thread's mask and alternate stack, signals sent to the app itself */
    sigstate_t msig;
    uint8_t   sig_disp[65];    /* BANANA_SIG_* */
    uint64_t  sig_onstack;     /* bit sig - 1: on the alternate stack */
    uint64_t  sig_pending;     /* sent to the app: the first thread not blocking one takes it */
    uintptr_t sig_entry;       /* the SDK's entry(sig, info, uc) */
    uintptr_t sigret_stub;     /* ... returns there: sigreturn */
    /* Unix processes (version 26): its pid is seq; its parent's (0: started by
     * the system, nobody waits for it), its process group and session. Ended
     * while its parent lives: a zombie - nothing left but its status, until
     * the parent waits for it. */
    int       ppid, pgid, sid;
    int       zombie;
    int       wstatus;         /* as waitpid gives it */
    int       term_sig;        /* the signal that ended it (0: exit) */
    int       waiting;         /* in waitpid: the terminal's keys are not for it */
    void*     fork_frame;      /* a fork's child: the registers it goes on with (sysframe_t) */
    uintptr_t fork_fs;         /* ... and its thread pointer */
    int       exec_next;       /* execve: the new program is ready - run() starts it */
    uspace_t* old_us;          /* ... the old one's space and memory, freed then */
    umem_t*   old_mem;
    /* terminals and job control (version 27): its controlling terminal (a
     * pty; NULL: none - or the console it was started on), stopped by
     * SIGSTOP / SIGTSTP / SIGTTIN / SIGTTOU (that signal; 0: running), what
     * its parent's waitpid has yet to hear (WUNTRACED, WCONTINUED) */
    struct pty* ctty;
    int       stopped;
    int       stop_reported;
    int       cont_report;
    /* its timer (version 30: alarm, setitimer, timer_create): signal alarm_sig
     * at timer_ms() alarm_at (0: none), then every alarm_every ms */
    uint32_t  alarm_at, alarm_every;
    int       alarm_sig;
    /* for ps (version 30: proc_list): its command line, when it started */
    char      cmdline[160];
    uint32_t  start_ms;
    int       web_restricted;
    uint32_t  web_deadline, web_cpu_start, web_cpu_limit;
    uint32_t  web_stop_reason, web_stop_cpu;
    uint32_t  web_window, web_window_cpu;
} app_proc_t;

/* pending signals change on several cores at once (a stop seen by the
 * child's, the parent taking its own): no update lost */
#ifdef __x86_64__
#define SIG_OR(x, v)  __atomic_or_fetch(&(x), (v), __ATOMIC_SEQ_CST)
#define SIG_AND(x, v) __atomic_and_fetch(&(x), (v), __ATOMIC_SEQ_CST)
#else
#define SIG_OR(x, v)  ((x) |= (v))
#define SIG_AND(x, v) ((x) &= (v))
#endif

static app_proc_t g_procs[APP_MAX];
/* the system call each task is in (its slot + 1; 0: none) - ps's WCHAN */
#ifdef __x86_64__
static uint16_t g_insys[TASK_MAX];
static uintptr_t g_lastip[TASK_MAX];      /* where in user code it last was (ps -o uip) */
#endif

/* argv as one line (ps shows it) */
static void cmdline_set(app_proc_t* p, int argc, char** argv) {
    size_t n = 0;
    for (int i = 0; i < argc && argv && argv[i] && n + 2 < sizeof(p->cmdline); i++) {
        if (i) p->cmdline[n++] = ' ';
        for (const char* c = argv[i]; *c && n + 1 < sizeof(p->cmdline); c++) p->cmdline[n++] = *c;
    }
    p->cmdline[n] = 0;
    p->start_ms = timer_ms();
}
/* the environment a program the system starts gets (execve passes its own) */
static const char* const g_default_env[] = {
    "HOME=/home/banana", "USER=banana", "LOGNAME=banana", "PATH=/bin:/usr/bin:/apps",
#ifdef __x86_64__
    "LANG=C.UTF-8", "SHELL=/bin/bash", "TMPDIR=/tmp", "TERM=banana", NULL,     /* (GNU bash: ports/bash) */
#else
    "LANG=C.UTF-8", "SHELL=/bin/sh", "TMPDIR=/tmp", "TERM=banana", NULL,
#endif
};
static int g_seq = 100;                 /* the last pid given (an app slot's seq) */
#ifdef __x86_64__
static void* g_sysframe[TASK_MAX];      /* the registers each task's system call came in with (fork) */
#endif
#ifdef __x86_64__
static uint8_t* stack_map(app_proc_t* p, size_t size, size_t guard);
static void stack_unmap(app_proc_t* p, uint8_t* stack, size_t size, size_t guard);
#endif
/* (terminals and job control: further down) */
struct kfile;
static long pts_read(app_proc_t* p, kfile_t* f, void* buf, unsigned long len);
static long pts_write(app_proc_t* p, kfile_t* f, const void* buf, unsigned long len);
static long ptm_read(app_proc_t* p, kfile_t* f, void* buf, unsigned long len);
static long ptm_write(app_proc_t* p, kfile_t* f, const void* buf, unsigned long len);
static long dev_read(kfile_t* f, void* buf, unsigned long len);
static long dev_write(kfile_t* f, unsigned long len);
static int  dev_open(app_proc_t* p, const char* path, int flags);
#ifdef __x86_64__
static void proc_stop(app_proc_t* p, int sig);
static void stop_wait(app_proc_t* p);
#endif
static void ctty_set(app_proc_t* p, struct pty* t);
static int  pgrp_signal(int pgrp, int sig);
static banana_api_t g_api;
static sigstate_t* sig_state(app_proc_t* p);     /* (the calling thread's signals) */


/* which app - and which of its threads - each task runs (by pid): every
 * system call asks (cur), on any core, so it is a lookup, not a search */
static int16_t g_pid_app[TASK_MAX];     /* app id + 1; 0: none */
static int8_t  g_pid_thr[TASK_MAX];     /* thread index + 1; 0: the app's main thread */

static void pid_set(int pid, int app, int thr) {
    if (pid < 0 || pid >= TASK_MAX) return;
    g_pid_thr[pid] = (int8_t)(thr + 1);
    g_pid_app[pid] = (int16_t)(app + 1);
}

static void pid_clear(int pid, int app) {
    if (pid < 0 || pid >= TASK_MAX || g_pid_app[pid] != app + 1) return;
    g_pid_app[pid] = 0;
    g_pid_thr[pid] = 0;
}

static app_proc_t* cur(void) {
    int pid = task_current_pid();
    if (pid < 0 || pid >= TASK_MAX || !g_pid_app[pid]) return NULL;
    app_proc_t* p = &g_procs[g_pid_app[pid] - 1];
    return p->used ? p : NULL;
}

/* the calling thread of app p, NULL for its main thread */
static athread_t* cur_thread(app_proc_t* p) {
    int pid = task_current_pid();
    if (!p || pid < 0 || pid >= TASK_MAX || !g_pid_thr[pid] || g_pid_app[pid] != p->id + 1) return NULL;
    athread_t* t = &p->threads[g_pid_thr[pid] - 1];
    return t->used && t->pid == pid ? t : NULL;
}

/* ends the calling app right away (exit(), Ctrl+C, closed twice) */
int  app_enter(int (*fn)(void*), void* arg, void* stack_top, uintptr_t* saved_sp);
void app_leave(uintptr_t saved_sp, int code) __attribute__((noreturn));
static void held_drop(void);           /* (the descriptions the calling task holds: kfile references) */

static void __attribute__((noreturn)) leave(app_proc_t* p, int code) {
    athread_t* t = cur_thread(p);
    if (t) {
        /* exit(), Ctrl+C, End task or a crash in a thread: the whole app
         * ends - its main thread and the other threads notice exit_req */
        if (!p->exit_req) { p->exit_req = 1; p->exit_req_code = code; }
        t->running = 0;
        held_drop(); app_leave(t->saved_sp, code);
    }
    p->exit_code = code;
    p->running = 0;
    held_drop(); app_leave(p->saved_sp, code);
}

/* a thread's return value (thread_join), or the end of the whole app */
static void __attribute__((noreturn)) leave_thread(app_proc_t* p, athread_t* t, int ret) {
    (void)p;
    t->running = 0;
    held_drop(); app_leave(t->saved_sp, ret);
}

static void key_push(app_proc_t* p, int c) {
    int next = (p->kq_head + 1) % APP_KEYQ;
    if (next == p->kq_tail) return;
    p->keyq[p->kq_head] = c;
    p->kq_head = next;
}

static int key_pop(app_proc_t* p) {
    if (p->kq_head == p->kq_tail) return 0;
    int c = p->keyq[p->kq_tail];
    p->kq_tail = (p->kq_tail + 1) % APP_KEYQ;
    return c;
}

static int focused(app_proc_t* p) {
    return p->has_term && gui_focused_vt() == p->vt;
}

/* Called from the API calls an app makes: lets the rest of the system
 * run now and then (tasks are cooperative), paints the terminal, and
 * notices Ctrl+C and windows closed for good. */
static void breathe(app_proc_t* p, int force) {
    if (!p) return;
    uint32_t now = timer_ms();
    if (p->exit_req) leave(p, p->exit_req_code);
    /* the 32-bit kernel has no guard pages: a smashed canary stops the app */
#ifndef __x86_64__
    if (!cur_thread(p) && *(volatile uint32_t*)p->stack != STACK_CANARY) {
        ksnprintf(p->crash_msg, sizeof(p->crash_msg), "%s: stack overflow - it used more than %u KiB of stack "
                  "(big local arrays? malloc them)", p->name, APP_STACK >> 10);
        p->term_sig = 11;
        *(volatile uint32_t*)p->stack = STACK_CANARY;
        leave(p, 139);
    }
#endif
    if (!force && now - p->last_breath < 20) return;
    p->last_breath = now;
    if (appwin_kill_requested(p->id) || p->kill_req) { p->term_sig = 9; leave(p, 137); }
    if (focused(p) && !p->waiting) {                /* (a shell waiting for its child: the keys are the child's) */
        char c = keyboard_try_getchar();
        if (c == 3) {
            p->interrupted = 1;
#ifdef __x86_64__
            /* a SIGINT handler (or SIG_IGN) takes Ctrl+C instead */
            if (p->sig_disp[2] == BANANA_SIG_CATCH) SIG_OR(p->sig_pending, 1ull << 1);
            else if (p->sig_disp[2] == BANANA_SIG_IGNORE) {}
            else
#endif
            if (!p->handles_int) { terminal_writeln("^C"); p->term_sig = 2; leave(p, 130); }
        } else if (c) {
            key_push(p, (unsigned char)c);
        }
    }
    gui_poll();                         /* paints, and yields to the other tasks */
    terminal_vt_set_active(p->vt);
}

/* ── memory ────────────────────────────────────────────────────────── */

static void* a_malloc(unsigned long size) {
    app_proc_t* p = cur();
    if (!p || size > (256ul << 20)) return NULL;
    return umem_alloc(p->mem, size);
}

static void a_free(void* ptr) {
    app_proc_t* p = cur();
    if (p && ptr) umem_free(p->mem, ptr);
}

static void* a_realloc(void* ptr, unsigned long size) {
    app_proc_t* p = cur();
    if (!p || size > (256ul << 20)) return NULL;
    return umem_realloc(p->mem, ptr, size);
}

/* memory of app `owner` (its window pictures: kernel/appwin.c) */
void* app_mem_alloc(int owner, size_t size) {
    if (owner < 0 || owner >= APP_MAX || !g_procs[owner].used || g_procs[owner].zombie) return NULL;
    return umem_alloc(g_procs[owner].mem, size);
}

void app_mem_free(int owner, void* ptr) {
    if (owner < 0 || owner >= APP_MAX || !g_procs[owner].used || g_procs[owner].zombie) return;
    umem_free(g_procs[owner].mem, ptr);
}

#ifdef __x86_64__
/* 1 if [ptr, ptr + n) is p's: its blocks, or what it mapped (kernel/uvm.h) */
static int own(app_proc_t* p, const void* ptr, size_t n) {
    return umem_owns(p->mem, ptr, n) || uvm_owns(p->us, ptr, n);
}

static int own_str(app_proc_t* p, const char* s) {
    return umem_owns_str(p->mem, s) || uvm_owns_str(p->us, s);
}
#endif

/* ── mmap (version 16) ─────────────────────────────────────────────────
 * 64-bit: reserved addresses, pages on first touch, protection (uvm.h).
 * 32-bit: no paging - zeroed app memory, protection ignored. */

static void* a_vm_map(void* addr, unsigned long len, int prot, int flags) {
    app_proc_t* p = cur();
    if (!p || !len) return NULL;
#ifdef __x86_64__
    return (void*)uvm_map(p->us, p->mem, (uintptr_t)addr, len, prot, flags);
#else
    (void)prot;
    if ((flags & UVM_FIXED) && addr) {                  /* only over memory it has: cleared */
        if (!umem_owns(p->mem, addr, len)) return NULL;
        memset(addr, 0, len);
        return addr;
    }
    if (len > (256ul << 20)) return NULL;
    void* m = umem_alloc_pages(p->mem, (len + 4095) & ~4095ul, 0);
    if (m) memset(m, 0, (len + 4095) & ~4095ul);
    return m;
#endif
}

static int a_vm_unmap(void* addr, unsigned long len) {
    app_proc_t* p = cur();
    if (!p || !addr) return -1;
#ifdef __x86_64__
    return uvm_unmap(p->us, p->mem, (uintptr_t)addr, len);
#else
    if (!umem_owns(p->mem, addr, len)) return -1;
    size_t whole = umem_pages_size(p->mem, addr);
    if (whole && len >= whole) umem_free_pages(p->mem, addr, 0);   /* a whole mapping goes */
    else memset(addr, 0, len);                          /* (part of one: it stays, cleared) */
    return 0;
#endif
}

static int a_vm_protect(void* addr, unsigned long len, int prot) {
    app_proc_t* p = cur();
    if (!p || !addr) return -1;
#ifdef __x86_64__
    /* its ordinary memory (malloc: a JIT making code runnable) is always
     * readable, writable and runnable: nothing to change */
    if (umem_owns(p->mem, addr, len ? len : 1)) return 0;
    return uvm_protect(p->us, (uintptr_t)addr, len, prot);
#else
    (void)prot;
    return umem_owns(p->mem, addr, len) ? 0 : -1;
#endif
}

/* kernel/idt.c, every page fault (any core, either mode): a page of the
 * running app's mmap region touched for the first time gets its memory */
int app_vm_fault(uintptr_t addr, uint32_t err, int from_user, int ints_on) {
    app_proc_t* p = cur();
    if (!p || !p->us) return 0;
#ifdef __x86_64__
    {   /* the kernel's tables loaded (not the app's): a page put in its tables
         * would not help - the access would fault again, forever */
        uintptr_t cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        if ((cr3 & ~(uintptr_t)0xFFF & 0x000FFFFFFFFFF000ull) != uspace_cr3(p->us)) return 0;
    }
#endif
    /* (it gives up - out of memory - only once 400 rounds in a row freed nothing) */
    for (int idle = 0; idle < 400; ) {
        uint32_t idx = 0;
        int r = uvm_fault(p->us, p->mem, addr, err, &idx);
        if(r==4 && p->web_restricted){
            if(cpu_id()!=0){
                if(!from_user)return 0;
                task_ap_go_home();
            }
            p->term_sig=9;__asm__ volatile("sti");leave(p,137);
        }
        if (r != 2 && r != 3) return r == 1;
#ifdef __x86_64__
        /* the page is in the swap area on the disk (kernel/zswap.c): read in -
         * where the fault may wait (app code, or the kernel with interrupts
         * on), on the boot core, which drives the disks */
        if (!from_user && !ints_on) return 0;
        if (cpu_id() != 0) {
            if (!from_user) return 0;
            task_ap_go_home();
        }
        __asm__ volatile("sti");
        /* 2: the page is on the disk; 3: no memory for it - some is made (compressed,
         * written out) or, if none can be yet, a moment waited */
        int rc = r == 2 ? zswap_disk_in(idx) : (int)zswap_reclaim();
        if (r == 2 && rc == -2) rc = (int)zswap_reclaim() ? 0 : -2;   /* (no memory to read it into) */
        idle = (r == 2 ? rc == 0 : rc > 0) ? 0 : idle + 1;
        __asm__ volatile("cli");
        if (r == 2 && rc == -1) { klog("vm fault: disk read of %u failed\n", idx); return 0; }
#else
        (void)from_user; (void)ints_on;
        return 0;
#endif
    }
#ifdef __x86_64__
    {
        zswap_stats_t z;
        zswap_stats(&z);
        klog("app: out of memory - no page for %p (heap %u KiB free, high %llu KiB free, pool %llu/%llu KiB, "
             "%u compressed in %llu KiB, %u on the disk in %llu KiB, %u entries)\n", (void*)addr,
             (uint32_t)((kheap_total_bytes() - kheap_used_bytes()) >> 10),
             (unsigned long long)((hmem_total_bytes() - hmem_used_bytes()) >> 10),
             (unsigned long long)(upage_used_bytes() >> 10), (unsigned long long)(upage_held_bytes() >> 10),
             (uint32_t)z.pages, (unsigned long long)(z.bytes >> 10), z.disk_pages,
             (unsigned long long)(z.disk_used >> 10), z.entries);
    }
#endif
    return 0;
}

/* 1 if [ptr, ptr + n) is the calling app's memory (64-bit: what a system
 * call may touch on its behalf; the 32-bit kernel does not check) */
static int uok(const void* ptr, size_t n) {
#ifdef __x86_64__
    app_proc_t* p = cur();
    return p && own(p, ptr, n);
#else
    (void)ptr; (void)n;
    return 1;
#endif
}

static int ustr(const char* s) {
#ifdef __x86_64__
    app_proc_t* p = cur();
    return p && own_str(p, s);
#else
    (void)s;
    return 1;
#endif
}

/* ── console ───────────────────────────────────────────────────────── */

static long fd_write(app_proc_t* p, int fd, const void* buf, unsigned long len);

/* writing to the terminal (a KF_CONS descriptor) */
static long cons_write(app_proc_t* p, const void* buf, unsigned long len) {
    const char* s = (const char*)buf;
    char chunk[257];
    unsigned long i = 0;
    while (i < len) {
        int n = 0;
        while (i < len && n < 256) {
            char c = s[i++];
            if (c) chunk[n++] = c;
        }
        chunk[n] = 0;
        terminal_write(chunk);
    }
    breathe(p, 0);
    return (long)len;
}

static long a_write(int fd, const void* buf, unsigned long len) {
    app_proc_t* p = cur();
    if (!p) return -1;
    return fd_write(p, fd, buf, len);
}

/* the next key for the app (ESC sequences become BANANA_KEY_*), 0 if none */
static int next_key(app_proc_t* p) {
    for (;;) {
        if (focused(p)) {                       /* F-keys and media keys: events of their own */
            int f = KEYF_CODE(keyboard_take_fkey());
            if (f >= KEYF_F1 && f < KEYF_F1 + 12) return BANANA_KEY_F1 + (f - KEYF_F1);
            if (f == KEYF_PLAY) return BANANA_KEY_PLAY;
            if (f == KEYF_STOP) return BANANA_KEY_STOP;
            if (f == KEYF_NEXT) return BANANA_KEY_NEXT;
            if (f == KEYF_PREV) return BANANA_KEY_PREV;
        }
        int c = key_pop(p);
        if (!c && focused(p)) c = (unsigned char)keyboard_try_getchar();
        if (!c) return 0;
        if (c == 3) {
            p->interrupted = 1;
            if (!p->handles_int) { terminal_writeln("^C"); leave(p, 130); }
            return 3;
        }
        if (p->esc == 0 && c == 27) {
            /* an arrow key sends ESC [ X right away; a lone ESC is ESC */
            uint32_t t0 = timer_ms();
            int c2 = 0;
            while (!c2 && timer_ms() - t0 < 30) { c2 = key_pop(p); if (!c2) c2 = (unsigned char)keyboard_try_getchar(); }
            if (c2 != '[') { if (c2) key_push(p, c2); return 27; }
            int c3 = 0;
            t0 = timer_ms();
            while (!c3 && timer_ms() - t0 < 30) { c3 = key_pop(p); if (!c3) c3 = (unsigned char)keyboard_try_getchar(); }
            switch (c3) {
            case 'A': return BANANA_KEY_UP;
            case 'B': return BANANA_KEY_DOWN;
            case 'C': return BANANA_KEY_RIGHT;
            case 'D': return BANANA_KEY_LEFT;
            case 'H': return BANANA_KEY_HOME;
            case 'F': return BANANA_KEY_END;
            case 'P': return BANANA_KEY_DELETE;
            case 'I': return BANANA_KEY_PGUP;
            case 'G': return BANANA_KEY_PGDN;
            case '5': keyboard_try_getchar(); return BANANA_KEY_PGUP;
            case '6': keyboard_try_getchar(); return BANANA_KEY_PGDN;
            case '3': keyboard_try_getchar(); return BANANA_KEY_DELETE;
            default: continue;
            }
        }
        return c;
    }
}

static int a_getchar(void) {
    app_proc_t* p = cur();
    if (!p || !p->has_term) return -1;
    for (;;) {
        int c = next_key(p);
        if (c) return c;
        breathe(p, 1);
        if (appwin_kill_requested(p->id)) leave(p, 137);
        task_sleep_ms(10);
        terminal_vt_set_active(p->vt);
    }
}

static int a_trygetchar(void) {
    app_proc_t* p = cur();
    if (!p || !p->has_term) return 0;
    breathe(p, 0);
    return next_key(p);
}

static int a_readline(char* buf, int max) {
    app_proc_t* p = cur();
    if (!p || max <= 0) return -1;
    int n = 0;
    buf[0] = 0;
    for (;;) {
        int c = a_getchar();
        if (c < 0) return -1;
        if (c == '\n' || c == '\r') { terminal_putchar('\n'); break; }
        if (c == '\b' || c == 127) {
            if (n > 0) { n--; terminal_cursor_left(); terminal_putchar(' '); terminal_cursor_left(); }
            continue;
        }
        if (c < 32 || c > 126) continue;
        if (n < max - 1) { buf[n++] = (char)c; terminal_putchar((char)c); }
    }
    buf[n] = 0;
    return n;
}

static void a_set_color(int fg, int bg) { terminal_setcolor((uint8_t)(fg & 15), (uint8_t)(bg & 15)); }
static void a_clear(void) { terminal_clear(); }
static void a_term_size(int* c, int* r) {
    if (c) *c = (int)terminal_get_width();
    if (r) *r = (int)terminal_get_height();
}
static void a_set_cursor(int r, int c) {
    if (r < 0) r = 0;
    if (c < 0) c = 0;
    terminal_set_cursor((size_t)r, (size_t)c);
}

static int a_interrupted(void) {
    app_proc_t* p = cur();
    if (!p) return 0;
    p->handles_int = 1;
    breathe(p, 0);
    int r = p->interrupted;
    p->interrupted = 0;
    return r;
}

/* ── files ─────────────────────────────────────────────────────────── */

/* the description behind descriptor fd, NULL if none (a file deleted
 * meanwhile: none) */
static kfile_t* get_fd(app_proc_t* p, int fd) {
    if (!p || fd < 0 || fd >= APP_FDS) return NULL;
    kfile_t* f = p->fdt[fd];
    if (!f) return NULL;
    if (f->type == KF_FILE) {
        fs_file_t* ff = fs_file_info(f->fidx);
        if (!ff || !ff->used) return NULL;
    }
    return f;
}


/* A system call working on a descriptor holds a reference to its
 * description while it runs (HOLD), so a close / dup2 by another thread
 * meanwhile cannot free it under it. If the call never returns - the app
 * stopped, or crashed in it - held_drop lets go of what it held. */
#define HELD_MAX 4
static kfile_t* g_held[TASK_MAX][HELD_MAX];
static uint8_t  g_nheld[TASK_MAX];

static kfile_t* hold(int fd) {
    kfile_t* f = kfile_ref(get_fd(cur(), fd));
    int pid = task_current_pid();
    if (f && pid >= 0 && pid < TASK_MAX && g_nheld[pid] < HELD_MAX) g_held[pid][g_nheld[pid]++] = f;
    return f;
}

static void unhold(kfile_t** pf) {
    kfile_t* f = *pf;
    if (!f) return;
    int pid = task_current_pid();
    if (pid >= 0 && pid < TASK_MAX)
        for (int i = g_nheld[pid] - 1; i >= 0; i--)
            if (g_held[pid][i] == f) {
                g_held[pid][i] = g_held[pid][--g_nheld[pid]];
                break;
            }
    kfile_put(f);
}

static void held_drop(void) {
    int pid = task_current_pid();
    if (pid < 0 || pid >= TASK_MAX) return;
    while (g_nheld[pid]) kfile_put(g_held[pid][--g_nheld[pid]]);
}

#define HOLD(var, fd) __attribute__((cleanup(unhold))) kfile_t* var = hold(fd)
/* f (a reference the caller gives) at the lowest free descriptor >= min:
 * its number, or -24 (EMFILE: f let go) */
static int fd_install(app_proc_t* p, kfile_t* f, int min, int cloexec) {
    for (int i = min < 0 ? 0 : min; i < APP_FDS; i++) {
        if (p->fdt[i]) continue;
        p->fdt[i] = f;
        p->cloexec[i] = (uint8_t)(cloexec != 0);
        return i;
    }
    kfile_put(f);
    return -24;
}

/* descriptor fd goes (its description when it was the last one): 0, -9 */
static int fd_close(app_proc_t* p, int fd) {
    if (!p || fd < 0 || fd >= APP_FDS || !p->fdt[fd]) return -9;
    kfile_t* f = p->fdt[fd];
    p->fdt[fd] = NULL;
    p->cloexec[fd] = 0;
    kfile_put(f);
    return 0;
}

/* a new app's 0, 1 and 2: the terminal it runs on */
static int fd_std(app_proc_t* p) {
    kfile_t* c = kfile_new(KF_CONS, KF_READ | KF_WRITE);
    if (!c) return -1;
    p->fdt[0] = c;
    p->fdt[1] = kfile_ref(c);
    p->fdt[2] = kfile_ref(c);
    return 0;
}

static int a_open(const char* path, int flags) {
    app_proc_t* p = cur();
    if (!p || !path || !*path) return -1;
    if (!strncmp(path, "/dev/", 5)) return dev_open(p, path, flags);   /* (-errno) */
    int didx = fs_find_dir(path);
    if (didx >= 0) {
        /* a folder: a descriptor that knows where it is (read: EISDIR) */
        if (flags & (BANANA_O_WRITE | BANANA_O_TRUNC | BANANA_O_APPEND)) return -21;
        kfile_t* d = kfile_new(KF_DIR, KF_READ);
        char* ap = (char*)kmalloc(FS_PATH_LEN);
        if (!d || !ap) { kfree(ap); kfile_put(d); return -12; }
        fs_dir_path(didx, ap, FS_PATH_LEN);
        if (!ap[0]) kstrlcpy(ap, "/", FS_PATH_LEN);
        d->path = ap;
        int fd = fd_install(p, d, 0, 0);
        return fd < 0 ? -24 : fd;
    }
    int idx = fs_find_file(path);
    if (idx < 0) {
        if (!(flags & BANANA_O_CREATE)) return -1;
        idx = fs_create(path);
        if (idx < 0) return -1;
    } else if ((flags & BANANA_O_TRUNC) && (flags & BANANA_O_WRITE)) {
        if (fs_write(idx, "", 0) != 0) return -1;
    }
    kfile_t* f = kfile_new(KF_FILE, (flags ? flags : BANANA_O_READ) & (KF_READ | KF_WRITE | KF_APPEND));
    if (!f) return -1;
    f->fidx = idx;
    f->pos = (flags & BANANA_O_APPEND) ? fs_file_info(idx)->size : 0;
    fs_pin(idx);                       /* (open: its entry stays, kernel/fs.c gives back no folder holding it) */
    int fd = fd_install(p, f, 0, 0);
    return fd < 0 ? -1 : fd;
}

/* the terminal: a line, with its newline */
static long cons_read(app_proc_t* p, void* buf, unsigned long len) {
    if (!p || !len) return 0;
    char* b = (char*)buf;
    int n = a_readline(b, (int)(len > 1024 ? 1024 : len));
    if (n < 0) return 0;
    if ((unsigned long)n < len) b[n++] = '\n';
    return n;
}

/* a pipe, waiting as a blocking descriptor does: a read until there is
 * something (or the end), a write until all of it went in */
static long pipe_io(app_proc_t* p, kfile_t* f, void* buf, unsigned long len, int wr) {
    unsigned long done = 0;
    for (;;) {
        long r = wr ? kpipe_write(f, (const uint8_t*)buf + done, len - done) : kpipe_read(f, buf, len);
        if (r == -11 && !(f->flags & KF_NONBLOCK)) {
            kpipe_wait(f, wr ? KF_POLLOUT : KF_POLLIN, 20);
            breathe(p, 0);
            continue;
        }
        if (r == -32) {                                  /* no reader: SIGPIPE (it ends the app by default) */
#ifdef __x86_64__
            SIG_OR(p->sig_pending, 1ull << 12);
#endif
            return done ? (long)done : -32;
        }
        if (r < 0) return done ? (long)done : r;
        if (!wr) return r;
        done += (unsigned long)r;
        if (done >= len) return (long)done;
    }
}

static long a_read(int fd, void* buf, unsigned long len) {
    app_proc_t* p = cur();
    HOLD(f, fd);
    if (!f) return -9;
    if (!(f->flags & KF_READ)) return -9;
    uint32_t n32 = len > 0x7FFFFFFFul ? 0x7FFFFFFFu : (uint32_t)len;
    switch (f->type) {
    case KF_CONS: return cons_read(p, buf, len);
    case KF_SOCK: return usock_recv(f->sock, buf, n32, 0, NULL, NULL);
    case KF_PIPE: return pipe_io(p, f, buf, n32, 0);
    case KF_PTS:  return pts_read(p, f, buf, n32);
    case KF_PTM:  return ptm_read(p, f, buf, n32);
    case KF_DEV:  return dev_read(f, buf, n32);
    case KF_DIR:  return -21;                           /* EISDIR */
    }
    /* the part asked for only: a big file on the disk is not read whole */
    int n = fs_read(f->fidx, f->pos, buf, n32);
    if (n < 0) return -1;
    f->pos += (uint32_t)n;
    breathe(p, 0);
    return (long)n;
}

static long fd_write(app_proc_t* p, int fd, const void* buf, unsigned long len) {
    HOLD(f, fd);
    if (!f) return -9;
    uint32_t n32 = len > 0x7FFFFFFFul ? 0x7FFFFFFFu : (uint32_t)len;
    switch (f->type) {
    case KF_CONS: return cons_write(p, buf, len);
    case KF_SOCK: return usock_send(f->sock, buf, n32, 0, NULL, 0);
    case KF_PIPE: return (f->flags & KF_WRITE) ? pipe_io(p, f, (void*)buf, n32, 1) : -9;
    case KF_PTS:  return pts_write(p, f, buf, n32);
    case KF_PTM:  return ptm_write(p, f, buf, n32);
    case KF_DEV:  return dev_write(f, n32);
    case KF_DIR:  return -9;
    }
    if (!(f->flags & (KF_WRITE | KF_APPEND))) return -9;
    fs_file_t* ff = fs_file_info(f->fidx);             /* (its data is not read in for a write) */
    if (!ff || !ff->used) return -1;
    if (f->flags & KF_APPEND) f->pos = ff->size;
    if (len > 0xFFFFFFFFul - f->pos) return -27;       /* EFBIG */
    if (fs_write_at(f->fidx, f->pos, buf, (uint32_t)len) != 0) return -1;
    f->pos += (uint32_t)len;
    breathe(p, 0);
    return (long)len;
}

static long a_seek(int fd, long off, int whence) {
    kfile_t* f = get_fd(cur(), fd);
    if (!f || f->type != KF_FILE) return -1;
    long base = whence == BANANA_SEEK_CUR ? (long)f->pos : whence == BANANA_SEEK_END ? (long)fs_file_info(f->fidx)->size : 0;
    long np = base + off;
    if (np < 0) return -1;
    f->pos = (uint32_t)np;
    return np;
}

static int a_close(int fd) {
    return fd_close(cur(), fd) == 0 ? 0 : -1;
}

/* Synchronize root-volume files: RAM is already current; an installed volume
 * also flushes its journal and device cache. Hold across a sleeping flush. */
static int a_web_ipc(int op, const void* in, unsigned int len, void* out, unsigned int cap) {
    app_proc_t* p = cur();
    if (!p || !p->web_restricted || len > 65536u || cap > 65536u) return -1;
#ifdef __x86_64__
    /* Buffer ownership alone is not enough: a hostile worker can protect
     * its own output mapping read-only. Never fault in the kernel while
     * copying a response into such a mapping. */
    if(cap && !uvm_user_writable(p->us,p->mem,(uintptr_t)out,cap))return -1;
#endif
    return web_worker_ipc(p->id, (uint32_t)p->seq, op, in, len, out, cap);
}

/* CPU time comes from the scheduler, so sleeping and network waits do not
 * consume the budget. Enforced at timer preemption and on every syscall. */
static void web_check_budget(app_proc_t* p) {
    if (!p || !p->web_restricted) return;
    uint32_t now = timer_ms(), cpu = 0;
    task_query(p->pid, NULL, &cpu, NULL);
    if(!p->kill_req&&p->web_cpu_limit&&cpu-p->web_cpu_start>p->web_cpu_limit){
        p->web_stop_reason=1;p->web_stop_cpu=cpu-p->web_cpu_start;p->kill_req=1;
    }else if(!p->kill_req&&p->web_deadline&&(int32_t)(now-p->web_deadline)>=0){
        p->web_stop_reason=2;p->kill_req=1;
    }
    if (now - p->web_window >= 1000u) {
        p->web_window = now; p->web_window_cpu = cpu;
    } else if (cpu - p->web_window_cpu >= 500u) {
        task_sleep_ms(1000u - (now - p->web_window));
    }
}

static int a_fd_sync(int fd) {
    HOLD(f, fd);
    if (!f) return -9;                                  /* EBADF */
    int mnt;
    if (f->type == KF_FILE) {
        fs_file_t* ff = fs_file_info(f->fidx);
        if (!ff || !ff->used) return -9;
        mnt = ff->mnt;
    } else if (f->type == KF_DIR) {
        int idx = f->path ? fs_find_dir(f->path) : -1;
        const fs_dir_t* d = idx >= 0 ? fs_get_dir(idx) : NULL;
        if (!d || !d->used) return -9;
        mnt = d->mnt;
    } else return -22;                                  /* EINVAL */
    /* Other mounts need their own backend. The live root is memory-backed:
     * its writes are already current, with no device cache to flush. Success
     * here adds no persistence beyond the lifetime of the live session. */
    if (mnt) return -38;                               /* ENOSYS */
    if (!fsdisk_is_installed()) return 0;
    int rc = fsdisk_sync_durable();
    if (rc == FSDISK_OK) return 0;
    return rc == FSDISK_ERR_FROZEN ? -16 : -5;            /* EBUSY / EIO */
}

/* ── Unix descriptors (version 25) ─────────────────────────────────── */

#define L_O_ACCMODE  3
#define L_O_APPEND   0x400
#define L_O_NONBLOCK 0x800
#define L_O_CLOEXEC  0x80000

static int pair_fds(int* fds, int flags, int sock) {
    app_proc_t* p = cur();
    if (!p || !uok(fds, 2 * sizeof(int))) return -14;
    if (flags & ~(L_O_NONBLOCK | L_O_CLOEXEC)) return -22;
    kfile_t* a, * b;
    int rc = sock ? kfile_socketpair(&a, &b, flags & L_O_NONBLOCK ? KF_NONBLOCK : 0)
                  : kfile_pipe(&a, &b, flags & L_O_NONBLOCK ? KF_NONBLOCK : 0);
    if (rc) return rc;
    int x = fd_install(p, a, 0, flags & L_O_CLOEXEC);
    if (x < 0) { kfile_put(b); return x; }
    int y = fd_install(p, b, 0, flags & L_O_CLOEXEC);
    if (y < 0) { fd_close(p, x); return y; }
    fds[0] = x;
    fds[1] = y;
    return 0;
}
static int a_fd_pipe(int* fds, int flags) { return pair_fds(fds, flags, 0); }
static int a_fd_socketpair(int* fds, int flags) { return pair_fds(fds, flags, 1); }

static int a_fd_dup(int fd, int to, int flags) {
    app_proc_t* p = cur();
    kfile_t* f = get_fd(p, fd);
    if (!f) return -9;
    if (to < 0 || to >= APP_FDS) return (flags & BANANA_DUP_TO) ? -9 : -22;
    int cx = (flags & BANANA_DUP_CLOEXEC) != 0;
    if (!(flags & BANANA_DUP_TO)) return fd_install(p, kfile_ref(f), to, cx);
    if (to == fd) { p->cloexec[to] = (uint8_t)cx; return to; }
    kfile_ref(f);
    fd_close(p, to);
    p->fdt[to] = f;
    p->cloexec[to] = (uint8_t)cx;
    return to;
}

static int a_fd_fcntl(int fd, int cmd, long arg) {
    app_proc_t* p = cur();
    kfile_t* f = get_fd(p, fd);
    if (!f) return -9;
    switch (cmd) {
    case 1: return p->cloexec[fd] ? 1 : 0;                      /* F_GETFD */
    case 2: p->cloexec[fd] = (uint8_t)(arg & 1); return 0;      /* F_SETFD */
    case 3: {                                                   /* F_GETFL */
        int rw = f->flags & KF_READ ? (f->flags & (KF_WRITE | KF_APPEND) ? 2 : 0) : 1;
        int nb = f->type == KF_SOCK ? usock_nonblock(f->sock) > 0 : (f->flags & KF_NONBLOCK) != 0;
        return rw | (f->flags & KF_APPEND ? L_O_APPEND : 0) | (nb ? L_O_NONBLOCK : 0);
    }
    case 4:                                                     /* F_SETFL: O_APPEND, O_NONBLOCK */
        if (f->type == KF_SOCK) usock_set_nonblock(f->sock, (arg & L_O_NONBLOCK) != 0);
        f->flags = (f->flags & ~(KF_NONBLOCK | KF_APPEND)) | (arg & L_O_NONBLOCK ? KF_NONBLOCK : 0)
                 | (f->type == KF_FILE && (arg & L_O_APPEND) ? KF_APPEND : 0);
        return 0;
    }
    return -22;
}

/* a pty's number for programs: /dev/pts/N's N, or a console's (BANANA_TTY_CONSOLE) */
static int tty_num(pty_t* t) {
    int m = vcons_minor(t);
    return m ? BANANA_TTY_CONSOLE | m : pty_index(t);
}

static int a_fd_info(int fd, void* out) {
    app_proc_t* p = cur();
    kfile_t* f = get_fd(p, fd);
    if (!f) return -9;
    banana_fdinfo_t* o = (banana_fdinfo_t*)out;
    memset(o, 0, sizeof(*o));
    o->type = f->type;
    o->flags = a_fd_fcntl(fd, 3, 0);
    o->is_sock = f->type == KF_PIPE && f->is_sock;
    o->cloexec = p->cloexec[fd];
    o->size = f->type == KF_FILE ? fs_file_info(f->fidx)->size : f->type == KF_PIPE ? (unsigned)kpipe_nread(f)
            : f->type == KF_PTM || f->type == KF_PTS ? (unsigned)tty_num(f->pty) : f->type == KF_DEV ? (unsigned)f->dev : 0;
    o->id = (uintptr_t)f;
    return 0;
}

/* ── sockets (version 18, kernel/usock.c) ──────────────────────────────
 * In the app's file descriptors, so read / write / close work on them.
 * Errors are negative errno numbers. */

#define SOCK_NONBLOCK_F 04000
#define SOCK_CLOEXEC_F  02000000

/* waiting in a socket call: the app can be stopped, the desktop paints */
static void sock_breathe(void) { breathe(cur(), 1); }

/* a descriptor for socket s: its number, or -EMFILE (s closed) */
static int sock_fd(app_proc_t* p, usock_t* s) {
    kfile_t* f = kfile_new(KF_SOCK, KF_READ | KF_WRITE);
    if (!f) { usock_close(s); return -12; }
    f->sock = s;
    return fd_install(p, f, 0, 0);
}

static int a_sock_open(int family, int type, int proto) {
    app_proc_t* p = cur();
    (void)proto;
    if (!p) return -22;
    usock_set_wait(sock_breathe);
    int err = 0;
    usock_t* s = usock_new(family, type & ~(SOCK_NONBLOCK_F | SOCK_CLOEXEC_F), &err);
    if (!s) return err;
    if (type & SOCK_NONBLOCK_F) usock_set_nonblock(s, 1);
    return sock_fd(p, s);
}

static int a_sock_bind(int fd, const void* sa, int len) {
    HOLD(h, fd);
    usock_t* s = h && h->type == KF_SOCK ? h->sock : NULL;
    return s ? usock_bind(s, sa, len) : -88;            /* ENOTSOCK */
}

static int a_sock_listen(int fd, int backlog) {
    HOLD(h, fd);
    usock_t* s = h && h->type == KF_SOCK ? h->sock : NULL;
    return s ? usock_listen(s, backlog) : -88;
}

static int a_sock_accept(int fd, void* sa, int* len) {
    app_proc_t* p = cur();
    HOLD(h, fd);
    usock_t* s = h && h->type == KF_SOCK ? h->sock : NULL;
    if (!p || !s) return -88;
    if (len && !uok(len, sizeof(int))) return -14;      /* EFAULT */
    if (sa && (!len || *len < 0 || !uok(sa, (size_t)*len))) return -14;
    int err = 0;
    usock_t* n = usock_accept(s, sa, len, &err);
    return n ? sock_fd(p, n) : err;
}

static int a_sock_connect(int fd, const void* sa, int len) {
    HOLD(h, fd);
    usock_t* s = h && h->type == KF_SOCK ? h->sock : NULL;
    return s ? usock_connect(s, sa, len) : -88;
}

static long a_sock_send(int fd, const void* buf, unsigned long len, int flags, const void* sa, int salen) {
    HOLD(h, fd);
    usock_t* s = h && h->type == KF_SOCK ? h->sock : NULL;
    if (!s) {
        kfile_t* k = h && h->type == KF_PIPE && h->is_sock ? h : NULL;
        if (!k) return -88;
        if (flags & 0x40) { long r = kpipe_write(k, buf, len); if (r == -32 && !(flags & 0x4000)) SIG_OR(cur()->sig_pending, 1ull << 12); return r; }   /* MSG_DONTWAIT; MSG_NOSIGNAL */
        return pipe_io(cur(), k, (void*)buf, len > 0x7FFFFFFFul ? 0x7FFFFFFFu : (uint32_t)len, 1);
    }
    if (sa && (salen < 0 || !uok(sa, (size_t)salen))) return -14;
    return usock_send(s, buf, len > 0x7FFFFFFFul ? 0x7FFFFFFFu : (uint32_t)len, flags, sa, salen);
}

static long a_sock_recv(int fd, void* buf, unsigned long len, int flags, void* sa, int* salen) {
    HOLD(h, fd);
    usock_t* s = h && h->type == KF_SOCK ? h->sock : NULL;
    if (!s) {
        kfile_t* k = h && h->type == KF_PIPE && h->is_sock ? h : NULL;
        if (!k) return -88;
        if (salen && uok(salen, sizeof(int))) *salen = 0;
        if (flags & 0x40) return kpipe_read(k, buf, len);                             /* MSG_DONTWAIT */
        return pipe_io(cur(), k, buf, len > 0x7FFFFFFFul ? 0x7FFFFFFFu : (uint32_t)len, 0);
    }
    if (salen && !uok(salen, sizeof(int))) return -14;
    if (sa && (!salen || *salen < 0 || !uok(sa, (size_t)*salen))) return -14;
    return usock_recv(s, buf, len > 0x7FFFFFFFul ? 0x7FFFFFFFu : (uint32_t)len, flags, sa, salen);
}

static int a_sock_shutdown(int fd, int how) {
    HOLD(h, fd);
    usock_t* s = h && h->type == KF_SOCK ? h->sock : NULL;
    if (!s && h && h->type == KF_PIPE) return kpipe_shutdown(h, how);
    return s ? usock_shutdown(s, how) : -88;
}

static int a_sock_opt(int fd, int set, int level, int name, void* val, int* len) {
    HOLD(h, fd);
    usock_t* s = h && h->type == KF_SOCK ? h->sock : NULL;
    if (!s) return -88;
    if (!len || !uok(len, sizeof(int)) || *len < 0 || (val && !uok(val, (size_t)*len))) return -14;
    return usock_opt(s, set, level, name, val, len);
}

static int a_sock_name(int fd, int peer, void* sa, int* len) {
    HOLD(h, fd);
    usock_t* s = h && h->type == KF_SOCK ? h->sock : NULL;
    if (!s) return -88;
    if (!len || !uok(len, sizeof(int)) || *len < 0 || (sa && !uok(sa, (size_t)*len))) return -14;
    return usock_name(s, peer, sa, len);
}

/* op 1: non-blocking on (arg 1) / off; 2: is it?; 3: bytes waiting (FIONREAD) */
static int a_sock_ctl(int fd, int op, int arg) {
    HOLD(h, fd);
    usock_t* s = h && h->type == KF_SOCK ? h->sock : NULL;
    if (!s) {                                           /* a pipe or socket pair: the same, on its description */
        kfile_t* k = h;
        if (!k || k->type != KF_PIPE) return -88;
        if (op == 1) { if (arg) k->flags |= KF_NONBLOCK; else k->flags &= ~KF_NONBLOCK; return 0; }
        if (op == 2) return (k->flags & KF_NONBLOCK) != 0;
        if (op == 3) return kpipe_nread(k);
        return -22;
    }
    if (op == 1) { usock_set_nonblock(s, arg); return 0; }
    if (op == 2) return usock_nonblock(s);
    if (op == 3) return usock_nread(s);
    return -22;
}

/* poll(): struct pollfd { int fd; short events, revents; } x n; how many
 * are ready, 0 after timeout_ms (-1: no limit) */
typedef struct { int fd; short events, revents; } apollfd_t;
static int sig_waiting(app_proc_t* p);
#define APOLLIN   0x1
#define APOLLOUT  0x4
#define APOLLNVAL 0x20

static int a_poll(void* fds, int n, int timeout_ms) {
    app_proc_t* p = cur();
    if (!p || n < 0 || n > 4096 || (n && !uok(fds, (size_t)n * sizeof(apollfd_t)))) return -22;
    apollfd_t* pf = (apollfd_t*)fds;
    uint32_t start = timer_ms();
    for (;;) {
        int ready = 0, socks = 0;
        uint32_t gen = kpipe_generation();
        for (int i = 0; i < n; i++) {
            int fd = pf[i].fd, ev = pf[i].events, r = 0;
            if (fd < 0) { pf[i].revents = 0; continue; }
            kfile_t* f = get_fd(p, fd);
            if (!f) r = APOLLNVAL;
            else if (f->type == KF_CONS) r = ((f->flags & KF_READ) && p->kq_head != p->kq_tail ? APOLLIN : 0) | APOLLOUT;
            else if (f->type == KF_SOCK) { r = usock_poll(f->sock, ev); socks = 1; }
            else if (f->type == KF_PIPE) r = kpipe_poll(f, ev);
            else if (f->type == KF_PTM || f->type == KF_PTS) r = pty_poll(f->pty, f->type == KF_PTM, ev);
            else r = APOLLIN | APOLLOUT;                 /* (a file is always ready) */
            r &= ev | KF_POLLERR | KF_POLLHUP | APOLLNVAL;
            pf[i].revents = (short)r;
            if (r) ready++;
        }
        if (ready || !timeout_ms) return ready;
        if (timeout_ms > 0 && timer_ms() - start >= (uint32_t)timeout_ms) return 0;
        if (sig_waiting(p)) return -4;                   /* EINTR, as Linux (even with SA_RESTART) */
        if (socks) net_wait(10);
        else kpipe_sleep(gen, 10);                       /* (a pipe's other end wakes it) */
        breathe(p, 1);
    }
}

/* a host name (or a dotted address, or localhost): its IPv4 address in
 * ip[4] (network order); 0, or -1 if it could not be found */
static int a_resolve(const char* name, unsigned char* ip) {
    if (!name || !ip || !uok(ip, 4)) return -1;
    ip4_t a = 0;
    if (dns_resolve(name, &a, 5000) != NET_OK) return -1;   /* (dotted and localhost too) */
    ip[0] = (unsigned char)(a >> 24); ip[1] = (unsigned char)(a >> 16);
    ip[2] = (unsigned char)(a >> 8);  ip[3] = (unsigned char)a;
    return 0;
}

static int a_remove(const char* path) {
    if (!path || !*path) return -1;
    if (fs_find_file(path) >= 0) { fs_delete(path, 0); return fs_find_file(path) >= 0 ? -1 : 0; }
    if (fs_find_dir(path) < 0) return -1;
    int d[1], f[1];
    if (fs_list_dirs(path, d, 1) > 0 || fs_list_files(path, f, 1) > 0) return -1;   /* not empty */
    fs_delete(path, 1);
    return fs_find_dir(path) >= 0 ? -1 : 0;
}

static int a_mkdir(const char* path) { return path && fs_mkdir(path) >= 0 ? 0 : -1; }
static int a_rename(const char* a, const char* b) { return a && b && fs_move(a, b) >= 0 ? 0 : -1; }

static int a_stat(const char* path, banana_stat_t* st) {
    if (!path || !st) return -1;
    int f = fs_find_file(path);
    if (f >= 0) { st->size = fs_file_info(f)->size; st->is_dir = 0; return 0; }
    if (fs_find_dir(path) >= 0) { st->size = 0; st->is_dir = 1; return 0; }
    return -1;
}

/* ── Unix files (version 28) ───────────────────────────────────────── */

/* why the last fs call failed: -EACCES, -EIO, or ENOENT */
static int fs_errno(void) { return fs_perm_error() ? -13 : -2; }

static void ustat_fill(banana_ustat_t* u, const fs_stat_t* st) {
    memset(u, 0, sizeof(*u));
    u->mode = st->mode;
    u->uid = st->uid;
    u->gid = st->gid;
    u->nlink = st->is_dir ? 2 : st->nlink ? st->nlink : 1;
    u->size = st->size;
    u->ino = st->is_dir ? (uint64_t)st->index * 2 + 1 : (uint64_t)st->ino * 2 + 2;
    u->dev = (uint64_t)st->mnt + 1;
    u->atime = st->atime;
    u->mtime = st->mtime;
    u->ctime = st->ctime;
}

static int a_fs_ustat(const char* path, int follow, banana_ustat_t* u) {
    if (!path || !u || !uok(u, sizeof(*u))) return -14;
    fs_stat_t st;
    if (fs_stat(path, follow, &st) != 0) return fs_errno();
    ustat_fill(u, &st);
    return 0;
}

static int a_fs_fdstat(int fd, banana_ustat_t* u) {
    if (!u || !uok(u, sizeof(*u))) return -14;
    HOLD(h, fd);
    if (!h) return -9;
    if (h->type == KF_DIR) {
        fs_stat_t ds;
        if (fs_stat(h->path, 1, &ds) != 0) return -2;
        ustat_fill(u, &ds);
        return 0;
    }
    if (h->type != KF_FILE) return -9;
    fs_file_t* ff = fs_file_info(h->fidx);
    if (!ff) return -9;
    fs_stat_t st;
    memset(&st, 0, sizeof(st));
    st.index = h->fidx;
    st.nlink = fs_nlink(h->fidx);
    st.ino = h->fidx;
    for (int j = ff->lnext; j >= 0 && j != h->fidx; j = fs_file_info(j)->lnext) if (j < st.ino) st.ino = j;
    st.mode = fs_file_mode(h->fidx);
    st.uid = ff->uid;
    st.gid = ff->gid;
    st.size = ff->size;
    st.atime = ff->atime;
    st.mtime = ff->mtime;
    st.ctime = ff->ctime;
    st.mnt = ff->mnt;
    ustat_fill(u, &st);
    return 0;
}

static int a_fs_chmod(const char* path, unsigned int mode) {
    if (!path) return -14;
    if (fs_chmod(path, mode & 07777u) == 0) return 0;
    fs_stat_t st;
    return fs_stat(path, 1, &st) != 0 ? -2 : fs_perm_error() ? -13 : -1;     /* (FAT: no modes - EPERM) */
}

static int a_fs_chown(const char* path, int uid, int gid) {
    if (!path) return -14;
    if (fs_chown(path, uid, gid) == 0) return 0;
    fs_stat_t st;
    return fs_stat(path, 1, &st) != 0 ? -2 : -1;
}

static int a_fs_symlink(const char* target, const char* linkpath) {
    if (!target || !linkpath) return -14;
    fs_stat_t st;
    if (fs_stat(linkpath, 0, &st) == 0) return -17;                       /* EEXIST */
    if (fs_symlink(target, linkpath) >= 0) return 0;
    return fs_perm_error() ? -13 : -2;
}

static int a_fs_readlink(const char* path, char* buf, int cap) {
    if (!path || !buf || cap <= 0 || !uok(buf, (size_t)cap)) return -14;
    fs_stat_t st;
    if (fs_stat(path, 0, &st) != 0) return -2;
    if (!st.is_link) return -22;                                          /* EINVAL: not a link */
    char t[FS_PATH_LEN];
    int n = fs_readlink(path, t, sizeof(t));
    if (n < 0) return -5;
    if (n > cap) n = cap;                                                 /* (cut, as readlink does) */
    memcpy(buf, t, (size_t)n);
    return n;
}

static int a_fs_utime(const char* path, int follow, long long mtime) {
    if (!path) return -14;
    uint32_t t = mtime < 0 ? rtc_now() : (uint32_t)mtime;
    return fs_utime(path, follow, ~0u, t) == 0 ? 0 : fs_errno();
}

/* ── timers (version 30): a process's alarm, a kernel task sends the signal ── */
static int g_alarm_task = -1;
static void proc_signal(app_proc_t* t, int sig);

static void alarm_entry(void) {
    for (;;) {
        uint32_t now = timer_ms();
        int armed = 0;
        for (int i = 0; i < APP_MAX; i++) {
            app_proc_t* t = &g_procs[i];
            if (!t->used || t->zombie || !t->alarm_at) continue;
            armed = 1;
            if ((int32_t)(now - t->alarm_at) < 0) continue;
            int sig = t->alarm_sig;
            if (t->alarm_every) {
                t->alarm_at += t->alarm_every;
                if ((int32_t)(now - t->alarm_at) >= 0) t->alarm_at = now + t->alarm_every;   /* (missed ones: not piled up) */
                if (!t->alarm_at) t->alarm_at = 1;
            } else t->alarm_at = 0;
            proc_signal(t, sig);
        }
        task_sleep_ms(armed ? 2 : 50);
    }
}

/* value_us: from now (0: disarm, -1: only tell), interval_us: then every (0: once); old: what
 * was left and its interval (microseconds) */
static int a_proc_alarm(int sig, long long value_us, long long interval_us, long long* old) {
    app_proc_t* p = cur();
    if (!p || sig < 1 || sig > 64 || value_us < -1 || interval_us < 0) return -22;
    uint32_t now = timer_ms();
    if (old) {
        old[0] = p->alarm_at && (int32_t)(p->alarm_at - now) > 0 ? (long long)(p->alarm_at - now) * 1000 : p->alarm_at ? 1 : 0;
        old[1] = (long long)p->alarm_every * 1000;
    }
    if (value_us == -1) return 0;                       /* (only asked) */
    if (!value_us) { p->alarm_at = 0; p->alarm_every = 0; return 0; }
    uint64_t ms = (uint64_t)(value_us + 999) / 1000, every = (uint64_t)(interval_us + 999) / 1000;
    if (ms > 0x7FFFFFFFull) ms = 0x7FFFFFFFull;
    if (every > 0x7FFFFFFFull) every = 0x7FFFFFFFull;
    p->alarm_sig = sig;
    p->alarm_every = (uint32_t)every;
    p->alarm_at = now + (uint32_t)ms;
    if (!p->alarm_at) p->alarm_at = 1;
    if (g_alarm_task < 0) g_alarm_task = task_create("alarms", alarm_entry);
    return 0;
}

/* (version 29) -1 now, -2 as it is */
static uint32_t utime_of(long long t) { return t == -2 ? ~0u : t < 0 ? rtc_now() : (uint32_t)t; }

static int a_fs_utimes(const char* path, int follow, long long atime, long long mtime) {
    if (!path) return -14;
    return fs_utime(path, follow, utime_of(atime), utime_of(mtime)) == 0 ? 0 : fs_errno();
}

static int a_fs_link(const char* oldpath, const char* newpath) {
    if (!oldpath || !newpath) return -14;
    int rc = fs_link(oldpath, newpath);
    return rc >= 0 ? 0 : rc;
}

static int a_fs_unlink(const char* path, int dir) {
    if (!path || !*path) return -2;
    fs_stat_t st;
    if (fs_stat(path, 0, &st) != 0) return -2;
    if (dir) {
        if (!st.is_dir) return -20;                                       /* ENOTDIR */
        int d[1], f[1];
        if (fs_list_dirs(path, d, 1) > 0 || fs_list_files(path, f, 1) > 0) return -39;   /* ENOTEMPTY */
        fs_delete(path, 1);
    } else {
        if (st.is_dir) return -21;                                        /* EISDIR */
        fs_delete(path, 0);
    }
    fs_stat_t again;
    if (fs_stat(path, 0, &again) == 0) return fs_perm_error() ? -13 : -16; /* (EBUSY: still there) */
    return 0;
}

/* a folder descriptor's path (a file's too): its length */
static int a_fs_fdpath(int fd, char* buf, int cap) {
    if (!buf || cap <= 0 || !uok(buf, (size_t)cap)) return -14;
    HOLD(h, fd);
    if (!h) return -9;
    char t[FS_PATH_LEN];
    if (h->type == KF_DIR) kstrlcpy(t, h->path, sizeof(t));
    else if (h->type == KF_FILE) fs_file_path(h->fidx, t, sizeof(t));
    else return -20;                                                      /* ENOTDIR */
    int n = (int)strlen(t);
    if (n >= cap) return -34;                                             /* ERANGE */
    memcpy(buf, t, (size_t)n + 1);
    return n;
}

static int a_fs_fdattr(int fd, int what, long long a, long long b) {
    HOLD(h, fd);
    if (!h || h->type != KF_FILE) return -9;
    if (what == 1) return fs_file_setattr(h->fidx, (uint32_t)a & 07777u, ~0u, ~0u) == 0 ? 0 : -1;
    if (what == 2) return fs_file_setattr(h->fidx, ~0u, a < 0 ? ~0u : (uint32_t)a, b < 0 ? ~0u : (uint32_t)b) == 0 ? 0 : -1;
    if (what == 3) { fs_file_utime(h->fidx, ~0u, a < 0 ? rtc_now() : (uint32_t)a); return 0; }
    if (what == 4) { fs_file_utime(h->fidx, utime_of(a), utime_of(b)); return 0; }
    if (what == 5) {                                                      /* ftruncate: the length a */
        if (!(h->flags & KF_WRITE)) return -22;                           /* EINVAL (Linux's) */
        if (a < 0 || a > 0xFFFFFFFFll) return a < 0 ? -22 : -27;          /* EFBIG */
        return fs_truncate(h->fidx, (uint32_t)a) == 0 ? 0 : -5;           /* EIO */
    }
    return -22;
}

static int a_readdir(const char* path, int index, banana_dirent_t* ent) {
    if (!path || !ent || index < 0) return -1;
    /* (from the heap for the call: apps on several cores may list at once) */
    int* idx = (int*)kmalloc(FS_LIST_MAX * sizeof(int));
    if (!idx) return -1;
    int rc = -1;
    int nd = fs_list_dirs(path, idx, FS_LIST_MAX);
    if (nd >= 0 && index < nd && index < FS_LIST_MAX) {
        const fs_dir_t* d = fs_get_dir(idx[index]);
        if (d) {
            kstrlcpy(ent->name, d->name, sizeof(ent->name));
            ent->size = 0;
            ent->is_dir = 1;
            rc = 0;
        }
    } else if (nd >= 0) {
        index -= nd;
        int nf = fs_list_files(path, idx, FS_LIST_MAX);
        if (index < nf && index < FS_LIST_MAX) {
            fs_file_t* f = fs_file_info(idx[index]);
            kstrlcpy(ent->name, f->name, sizeof(ent->name));
            ent->size = f->size;
            ent->is_dir = 0;
            rc = 0;
        }
    }
    kfree(idx);
    return rc;
}

static int a_getcwd(char* buf, int size) {
    if (!buf || size <= 0) return -1;
    fs_cwd_path(buf, size);
    return 0;
}

static int a_chdir(const char* path) {
    if (!path || fs_find_dir(path) < 0) return -1;
    fs_cd(path);
    return 0;
}

/* ── time ──────────────────────────────────────────────────────────── */

static unsigned int a_ticks(void) { return timer_ms(); }

static int sig_waiting(app_proc_t* p);

static void a_sleep(unsigned int ms) {
    app_proc_t* p = cur();
    uint32_t end = timer_ms() + ms;
    breathe(p, 1);
    for (;;) {
        int32_t left = (int32_t)(end - timer_ms());
        if (left <= 0) break;
        task_sleep_ms(left > 10 ? 10 : (uint32_t)left);
        if (p) terminal_vt_set_active(p->vt);
        /* awake at the time asked: back to the app now - breathing first
         * (it yields) would hand the processor to the busy tasks again */
        if ((int32_t)(end - timer_ms()) <= 0) break;
        breathe(p, 0);                      /* (every 20 ms: Ctrl+C, End task, painting) */
        if (p && sig_waiting(p)) break;     /* (a signal ends the sleep, as EINTR does) */
    }
}

static void a_yield(void) { breathe(cur(), 1); }

static void a_localtime(banana_time_t* t) {
    rtc_datetime_t dt;
    memset(t, 0, sizeof(*t));
    if (rtc_read_datetime(&dt) != 0) { t->year = 2026; t->month = 1; t->day = 1; return; }
    t->year = dt.year; t->month = dt.month; t->day = dt.day;
    t->hour = dt.hour; t->minute = dt.minute; t->second = dt.second;
    int y = dt.year, m = dt.month;
    if (m < 3) { m += 12; y--; }
    int k = y % 100, j = y / 100;
    int h = (dt.day + 13 * (m + 1) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;   /* 0 = Saturday */
    t->weekday = (h + 6) % 7;
}

static unsigned int a_time_utc(void) { return rtc_now_utc(); }
static int a_utc_offset(unsigned int utc) { return tz_offset_at(tz_current(), utc ? utc : rtc_now_utc()); }
static int a_language(char* out, int cap) { if (!out || cap < 3) return -1; kstrlcpy(out, i18n_lang(), (size_t)cap); return 0; }

static unsigned int a_random(void) { return random_u32(); }

/* ── windows ───────────────────────────────────────────────────────── */

static int a_win_open(const char* title, int w, int h) {
    app_proc_t* p = cur();
    if (!p || !gui_is_enabled()) return -1;
    return appwin_open_app(p->id, title, w, h);
}
static unsigned int* a_win_pixels(int win) { app_proc_t* p = cur(); return p ? appwin_pixels(win, p->id) : NULL; }
static void a_win_update(int win) {
    app_proc_t* p = cur();
    if (!p) return;
    appwin_update(win, p->id);
    breathe(p, 0);
}
static int a_win_event(int win, banana_event_t* ev) {
    app_proc_t* p = cur();
    if (!p || !ev) return 0;
    breathe(p, 0);
    return appwin_event(win, p->id, ev);
}
static void a_win_close(int win) { app_proc_t* p = cur(); if (p) appwin_close(win, p->id); }
static void a_win_set_title(int win, const char* t) { app_proc_t* p = cur(); if (p) appwin_set_title(win, p->id, t); }
static void a_win_size(int win, int* w, int* h) { app_proc_t* p = cur(); appwin_size(win, p ? p->id : -1, w, h); }
static void a_win_media_keys(int win) { app_proc_t* p = cur(); if (p) appwin_set_media(win, p->id); }
static int a_win_capture_mouse(int win,int capture) { app_proc_t* p=cur();return p?appwin_capture_mouse(win,p->id,capture):-1; }
static int a_win_mouse_captured(int win) { app_proc_t* p=cur();return p?appwin_mouse_captured(win,p->id):-1; }
static void a_win_wheel(int win) { app_proc_t* p = cur(); if (p) appwin_set_wheel(win, p->id); }
static void a_win_set_resizable(int win, int mw, int mh) { app_proc_t* p = cur(); if (p) appwin_set_resizable(win, p->id, mw, mh); }

/* web views (webview.c): each belongs to the app that opened it */
static int  owner_id(void) { app_proc_t* p = cur(); return p ? p->id : -1; }
static int  a_web_open(int w, int h) { int o = owner_id(); return o < 0 ? -1 : webview_open(o, w, h); }
static void a_web_close(int v) { webview_close(owner_id(), v); }
static int  a_web_load(int v, const char* url) { return webview_load(owner_id(), v, url); }
static int  a_web_load_html(int v, const char* h, const char* base) { return webview_load_html(owner_id(), v, h, base); }
static void a_web_resize(int v, int w, int h) { webview_resize(owner_id(), v, w, h); }
static int  a_web_poll(int v) { return webview_poll(owner_id(), v); }
static void a_web_draw(int v, unsigned int* px, int stride, int x, int y, int w, int h) {
    if (!px || w <= 0 || h <= 0 || stride < w || !uok(px, (size_t)stride * (size_t)h * 4)) return;
    webview_draw(owner_id(), v, px, stride, x, y, w, h);
}
static void a_web_event(int v, const banana_event_t* ev) { webview_event(owner_id(), v, ev); }
static void a_web_scroll(int v, int dy) { webview_scroll(owner_id(), v, dy); }
static void a_web_go(int v, int d) { webview_go(owner_id(), v, d); }
static int  a_web_info(int v, char* t, int tc, char* u, int uc) { return webview_info(owner_id(), v, t, tc, u, uc); }
static int  a_web_eval(int v, const char* js, char* out, int cap) { return webview_eval(owner_id(), v, js, out, cap); }
static int  a_web_message(int v, char* out, int cap) { return webview_message(owner_id(), v, out, cap); }
static int  a_web_post(int v, const char* m) { return webview_post(owner_id(), v, m); }

/* ── pictures ── */
static unsigned int* a_image_load(const char* path, int* w, int* h, char* err, int ecap) {
    char e[96];
    if (!err || ecap <= 0) { err = e; ecap = sizeof(e); }
    err[0] = 0;
    int idx = path ? fs_find_file(path) : -1;
    fs_file_t* f = idx >= 0 ? fs_get_file(idx) : NULL;
    if (!f) { ksnprintf(err, (uint32_t)ecap, "no such file"); return NULL; }
    image_t img;
    fs_pin(idx);
    int bad = image_decode((const uint8_t*)f->content, f->size, &img, err, (uint32_t)ecap) != 0;
    fs_unpin(idx);
    if (bad) return NULL;
    unsigned int* px = (unsigned int*)a_malloc((unsigned long)img.w * (unsigned long)img.h * 4ul);
    if (!px) { image_free(&img); ksnprintf(err, (uint32_t)ecap, "out of memory"); return NULL; }
    for (int i = 0; i < img.w * img.h; i++)
        px[i] = (unsigned int)img.rgb[i * 3] << 16 | (unsigned int)img.rgb[i * 3 + 1] << 8 | img.rgb[i * 3 + 2];
    if (w) *w = img.w;
    if (h) *h = img.h;
    image_free(&img);
    return px;
}

static int a_set_wallpaper(const char* path, int mode, char* err, int ecap) {
    char e[96];
    if (!err || ecap <= 0) { err = e; ecap = sizeof(e); }
    if (!path) return -1;
    if (mode < 0 || mode >= IMAGE_MODE_COUNT) mode = IMAGE_FILL;
    char abs[FS_PATH_LEN];
    int idx = fs_find_file(path);
    if (idx < 0) { ksnprintf(err, (uint32_t)ecap, "no such file"); return -1; }
    fs_file_path(idx, abs, sizeof(abs));
    return wallpaper_set_file(abs, (image_mode_t)mode, err, (uint32_t)ecap);
}

/* the system fonts */
static int clamp_size(int s) { return s < 4 ? 4 : s > 200 ? 200 : s; }
static int a_font_draw(unsigned int* px, int stride, int w, int h, int x, int y, int font, int size, const char* text, unsigned int color) {
    if (!px || !text || w <= 0 || h <= 0 || stride < w) return x;
    if (!uok(px, (size_t)stride * (size_t)h * 4)) return x;
    size = clamp_size(size);
    int asc;
    font_metrics(font, size, &asc, NULL, NULL);
    return font_draw(px, stride, 0, 0, w, h, font, size, x, y + asc, text, (uint32_t)strlen(text), color, 0);
}
static int a_font_width(int font, int size, const char* text) {
    return text ? font_text_width(font, clamp_size(size), text, (uint32_t)strlen(text)) : 0;
}
static unsigned int a_audio_queued_ms(void) { return audio_queued_ms(); }
static void a_audio_stop(void) { audio_stop(); }
static int a_audio_volume(int percent) {
    if (percent >= 0) audio_set_volume(percent > 100 ? 100 : percent);
    return audio_get_volume();
}

static void a_font_metrics(int font, int size, int* ascent, int* descent, int* line_h) {
    font_metrics(font, clamp_size(size), ascent, descent, line_h);
}

static void a_draw_text(unsigned int* px, int stride, int w, int h, int x, int y,
                        const char* s, unsigned int fg, unsigned int bg) {
    if (!px || !s || w <= 0 || h <= 0 || stride < w || !uok(px, (size_t)stride * (size_t)h * 4)) return;
    for (; *s; s++, x += 8) {
        uint8_t c = (uint8_t)*s;
        if (c >= 128) c = '?';
        const uint8_t* g = font8x8_basic[c];
        for (int gy = 0; gy < 8; gy++) {
            int yy = y + gy;
            if (yy < 0 || yy >= h) continue;
            for (int gx = 0; gx < 8; gx++) {
                int xx = x + gx;
                if (xx < 0 || xx >= w) continue;
                if (g[gy] & (1u << gx)) px[yy * stride + xx] = fg;
                else if (bg != BANANA_TRANSPARENT) px[yy * stride + xx] = bg;
            }
        }
    }
}

/* ── network ───────────────────────────────────────────────────────── */

typedef struct { char* buf; uint32_t n, cap; int oom; } hbody_t;

static int h_body(void* ctx, const uint8_t* d, uint32_t n) {
    hbody_t* b = (hbody_t*)ctx;
    if (b->n + n + 1 > (16u << 20)) { b->oom = 1; return -1; }
    if (b->n + n + 1 > b->cap) {
        uint32_t cap = (b->n + n + 1) * 2;
        char* nb = (char*)a_realloc(b->buf, cap);
        if (!nb) { b->oom = 1; return -1; }
        b->buf = nb;
        b->cap = cap;
    }
    memcpy(b->buf + b->n, d, n);
    b->n += n;
    b->buf[b->n] = 0;
    return 0;
}

static int a_http_request(const char* method, const char* url, const char* body, unsigned long blen, const char* btype,
                          char** data, unsigned long* len, char* ctype, int ccap, char* err, int ecap) {
    char e[128];
    hbody_t b = { 0, 0, 0, 0 };
    http_request_t req;
    memset(&req, 0, sizeof(req));
    req.method = method && method[0] ? method : "GET";
    if (body) {
        req.body = body;
        req.body_len = (uint32_t)blen;
        req.content_type = btype && btype[0] ? btype : "application/x-www-form-urlencoded";
    }
    req.follow_redirects = 1;
    req.max_redirects = 10;
    req.timeout_ms = 15000;
    req.user_agent = "BananaOS-App/1.0";
    req.ctx = &b;
    req.on_body = h_body;
    http_response_t* resp = (http_response_t*)kmalloc(sizeof(http_response_t));
    if (!resp) return -1;
    int rc = http_fetch(url, &req, resp, e, sizeof(e));
    if (rc != NET_OK || b.oom) {
        if (err && ecap > 0) kstrlcpy(err, b.oom ? "too big (16 MiB max)" : e, (size_t)ecap);
        if (b.buf) a_free(b.buf);
        kfree(resp);
        return -1;
    }
    if (ctype && ccap > 0) kstrlcpy(ctype, resp->content_type, (size_t)ccap);
    if (resp->status >= 400) {
        if (err && ecap > 0) ksnprintf(err, (size_t)ecap, "HTTP %d %s", resp->status, resp->reason);
        kfree(resp);
        if (b.buf) a_free(b.buf);
        return -1;
    }
    kfree(resp);
    if (!b.buf) { b.buf = (char*)a_malloc(1); if (!b.buf) return -1; b.buf[0] = 0; }
    *data = b.buf;
    if (len) *len = b.n;
    return 0;
}

/* any request, any answer (version 10) */
static int a_http_fetch(const banana_http_req_t* rq, banana_http_resp_t* out) {
    if (!rq || !out || !uok(rq, sizeof(*rq)) || !uok(out, sizeof(*out))) return -1;
    /* (what the request points at must be the app's too) */
    if (!rq->url || !ustr(rq->url) || (rq->method && !ustr(rq->method)) || (rq->headers && !ustr(rq->headers)) ||
        (rq->content_type && !ustr(rq->content_type)) || (rq->body && !uok(rq->body, rq->body_len ? rq->body_len : 1)))
        return -1;
    memset(out, 0, sizeof(*out));
    hbody_t b = { 0, 0, 0, 0 };
    http_request_t req;
    memset(&req, 0, sizeof(req));
    req.method = rq->method && rq->method[0] ? rq->method : "GET";
    if (rq->body) {
        req.body = rq->body;
        req.body_len = (uint32_t)rq->body_len;
        req.content_type = rq->content_type && rq->content_type[0] ? rq->content_type : "application/json";
    }
    req.extra_headers = rq->headers;
    req.follow_redirects = 1;
    req.max_redirects = 10;
    req.timeout_ms = rq->timeout_ms > 0 ? (uint32_t)rq->timeout_ms : 20000;
    req.user_agent = "BananaOS-App/1.0";
    req.ctx = &b;
    req.on_body = h_body;
    http_response_t* resp = (http_response_t*)kmalloc(sizeof(http_response_t));
    if (!resp) { kstrlcpy(out->err, "out of memory", sizeof(out->err)); return -1; }
    char e[128];
    int rc = http_fetch(rq->url, &req, resp, e, sizeof(e));
    if (rc != NET_OK || b.oom) {
        kstrlcpy(out->err, b.oom ? "too big (16 MiB max)" : e, sizeof(out->err));
        if (b.buf) a_free(b.buf);
        kfree(resp);
        return -1;
    }
    out->status = resp->status;
    kstrlcpy(out->content_type, resp->content_type, sizeof(out->content_type));
    kfree(resp);
    if (!b.buf) { b.buf = (char*)a_malloc(1); if (b.buf) b.buf[0] = 0; }
    out->data = b.buf;
    out->len = b.n;
    return 0;
}

static int a_key_mods(void) { return keyboard_mods(); }

static int a_pkg_install(const char* path, char* msg, int mcap) {
    char m[160];
    if (!msg || mcap <= 0) { msg = m; mcap = sizeof(m); }
    if (!path) return -1;
    return pkg_install(path, msg, mcap) == 0 ? 0 : -1;
}

/* ── version 11: the package manager ── */
static int a_repo_update(char* msg, int mcap) {
    char m[200];
    if (!msg || mcap <= 0) { msg = m; mcap = sizeof(m); }
    return repo_update(NULL, NULL, msg, mcap);
}

static void to_api(banana_pkg_t* o, const repo_pkg_t* p, const pkg_info_t* inst) {
    memset(o, 0, sizeof(*o));
    const char* name = p ? p->name : inst->name;
    kstrlcpy(o->name, name, sizeof(o->name));
    kstrlcpy(o->title, p ? p->title : inst->title, sizeof(o->title));
    kstrlcpy(o->version, p ? p->version : inst->version, sizeof(o->version));
    kstrlcpy(o->type, p ? p->type : inst->type, sizeof(o->type));
    kstrlcpy(o->category, p && p->category[0] ? p->category : inst ? inst->category : "", sizeof(o->category));
    kstrlcpy(o->description, p ? p->description : inst->description, sizeof(o->description));
    kstrlcpy(o->author, p ? p->author : inst->author, sizeof(o->author));
    kstrlcpy(o->depends, p ? p->depends : inst->depends, sizeof(o->depends));
    if (p) { o->size = p->size; o->flags |= BANANA_PKG_AVAILABLE; }
    if (inst) {
        kstrlcpy(o->installed, inst->version[0] ? inst->version : "0", sizeof(o->installed));
        o->flags |= BANANA_PKG_INSTALLED;
        if (repo_is_auto(name)) o->flags |= BANANA_PKG_AUTO;
        if (p && repo_vercmp(p->version, inst->version) > 0) o->flags |= BANANA_PKG_UPGRADABLE;
    }
}

static int a_repo_list(banana_pkg_t* out, int max) {
    if (!out || max <= 0 || !uok(out, (size_t)max * sizeof(banana_pkg_t))) return 0;
    static pkg_info_t inst[64];
    int ni = pkg_list(inst, 64);
    if (ni > 64) ni = 64;
    int n = 0, total = repo_count();
    for (int i = 0; i < total; i++) {
        const repo_pkg_t* p = repo_at(i);
        if (!p || p != repo_candidate(p->name)) continue;
        const pkg_info_t* in = NULL;
        for (int k = 0; k < ni; k++) if (!strcmp(inst[k].name, p->name)) in = &inst[k];
        if (n < max) to_api(&out[n], p, in);
        n++;
    }
    for (int k = 0; k < ni; k++) {                       /* installed, in no repository */
        if (repo_candidate(inst[k].name)) continue;
        if (n < max) to_api(&out[n], NULL, &inst[k]);
        n++;
    }
    return n < max ? n : max;
}

static int a_repo_install(const char* name, char* msg, int mcap) {
    char m[200];
    if (!msg || mcap <= 0) { msg = m; mcap = sizeof(m); }
    if (!name) return -1;
    return repo_install(name, 0, NULL, NULL, msg, mcap);
}

static int a_repo_remove(const char* name, char* msg, int mcap) {
    char m[200];
    if (!msg || mcap <= 0) { msg = m; mcap = sizeof(m); }
    if (!name) return -1;
    return repo_remove(name, msg, mcap);
}

static int a_repo_status(char* msg, int mcap) { return repo_status(msg, mcap); }

static int a_repo_icon(const char* name, char* path, int pcap) {
    if (!name || !path || pcap <= 0) return -1;
    return repo_icon(name, path, pcap);
}

static int a_repo_upgrade(char* msg, int mcap) {
    char m[200];
    if (!msg || mcap <= 0) { msg = m; mcap = sizeof(m); }
    return repo_upgrade(NULL, NULL, msg, mcap);
}

static int a_repo_sources(banana_repo_source_t* out, int max) {
    static repo_source_t src[16];
    int n = repo_sources(src, 16);
    if (!out || max <= 0) return n;
    if (!uok(out, (size_t)max * sizeof(banana_repo_source_t))) return -1;
    for (int i = 0; i < n && i < max; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        kstrlcpy(out[i].url, src[i].url, sizeof(out[i].url));
        out[i].has_key = src[i].has_key;
        out[i].trusted = src[i].trusted;
    }
    return n;
}

static int a_repo_add_source(const char* url, const char* key_hex, int trusted, char* msg, int mcap) {
    char m[200];
    if (!msg || mcap <= 0) { msg = m; mcap = sizeof(m); }
    if (!url) return -1;
    if (key_hex && !key_hex[0]) key_hex = NULL;
    if (!key_hex && !trusted && strncmp(url, "file://", 7)) {
        kstrlcpy(msg, "give the server's key, or choose to trust it without one", (size_t)mcap);
        return -1;
    }
    return repo_add_source(url, key_hex, trusted, msg, mcap);
}

static int a_repo_remove_source(const char* url, char* msg, int mcap) {
    char m[200];
    if (!msg || mcap <= 0) { msg = m; mcap = sizeof(m); }
    if (!url || !url[0] || (url[0] >= '0' && url[0] <= '9')) return -1;   /* by URL only, not by number */
    return repo_remove_source(url, msg, mcap);
}

static int a_win_dropped(int win, int index, char* path, int cap) {
    app_proc_t* p = cur();
    return appwin_dropped(win, p ? p->id : -1, index, path, cap);
}

static int a_gamepad(int index, banana_gamepad_t* out) {
    gamepad_t g;
    int r = gamepad_get(index, &g);
    if (out) {
        memset(out, 0, sizeof(*out));
        out->connected = r;
        kstrlcpy(out->name, g.name, sizeof(out->name));
        out->lx = g.lx; out->ly = g.ly; out->rx = g.rx; out->ry = g.ry;
        out->lt = g.lt; out->rt = g.rt;
        out->buttons = g.buttons;
    }
    return r;
}

/* a key held now, if the app has the keyboard (its terminal or its window in front) */
static int a_key_down(int key) {
    app_proc_t* p = cur();
    if (!p || !(focused(p) || appwin_focused_owner() == p->id)) return 0;
    switch (key) {                      /* the arrows etc.: the E0 keys, or the keypad's */
    case BANANA_KEY_UP:     return keyboard_held(0xC8) || keyboard_held(0x48);
    case BANANA_KEY_DOWN:   return keyboard_held(0xD0) || keyboard_held(0x50);
    case BANANA_KEY_LEFT:   return keyboard_held(0xCB) || keyboard_held(0x4B);
    case BANANA_KEY_RIGHT:  return keyboard_held(0xCD) || keyboard_held(0x4D);
    case BANANA_KEY_HOME:   return keyboard_held(0xC7) || keyboard_held(0x47);
    case BANANA_KEY_END:    return keyboard_held(0xCF) || keyboard_held(0x4F);
    case BANANA_KEY_PGUP:   return keyboard_held(0xC9) || keyboard_held(0x49);
    case BANANA_KEY_PGDN:   return keyboard_held(0xD1) || keyboard_held(0x51);
    case BANANA_KEY_DELETE: return keyboard_held(0xD3) || keyboard_held(0x53);
    case BANANA_KEY_SHIFT:  return keyboard_held(0x2A) || keyboard_held(0x36);
    case BANANA_KEY_CTRL:   return keyboard_held(0x1D) || keyboard_held(0x9D);
    case BANANA_KEY_ALT:    return keyboard_held(0x38) || keyboard_held(0xB8);
    }
    if (key >= BANANA_KEY_F1 && key <= BANANA_KEY_F12) {
        int i = key - BANANA_KEY_F1;
        return keyboard_held(i < 10 ? 0x3B + i : 0x57 + (i - 10));
    }
    return keyboard_held_char(key);
}

static int a_app_run(const char* name, int argc, char** argv, char* err, int ecap) {
    char e[128];
    if (!err || ecap <= 0) { err = e; ecap = sizeof(e); }
    if (!name || argc < 0 || argc > 256 || (argc && !uok(argv, (size_t)argc * sizeof(char*)))) return -1;
    for (int i = 0; i < argc; i++) if (!argv[i] || !ustr(argv[i])) return -1;
    return pkg_run(name, argc, argv, 1, err, ecap) < 0 ? -1 : 0;
}

static int a_http_get(const char* url, char** data, unsigned long* len, char* ctype, int ccap, char* err, int ecap) {
    return a_http_request("GET", url, NULL, 0, NULL, data, len, ctype, ccap, err, ecap);
}

/* ── sound / clipboard ─────────────────────────────────────────────── */

static int a_audio_play(const void* pcm, unsigned long bytes, int rate, int ch, int bits) {
    return audio_play(pcm, (uint32_t)bytes, rate, ch, bits);
}
static void a_audio_beep(int hz, int ms) { audio_beep(hz, ms); }
static int a_audio_busy(void) { return audio_busy(); }
static void a_clip_set(const char* t, unsigned long n) { if (t) clipboard_set(t, (uint32_t)n); }
static const char* a_clip_get(unsigned long* n) {
    app_proc_t* p = cur();
    uint32_t l;
    const char* s = clipboard_get(&l);
    if (n) *n = s ? l : 0;
    if (!s || !p) return NULL;
    /* the app gets its own copy (the kernel's is not its to read), kept until the next call */
    char* c = (char*)umem_realloc(p->mem, p->clip, (size_t)l + 1);
    if (!c) return NULL;
    memcpy(c, s, l);
    c[l] = 0;
    p->clip = c;
    return c;
}

/* ── threads ───────────────────────────────────────────────────────── */

#ifdef __x86_64__
int app_enter_user(uintptr_t ip, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t user_sp, uintptr_t* saved_sp);

/* the kernel stack of an app thread's system calls, a guard page under it:
 * running past its end faults (the app stops) instead of overwriting the heap */
static uint8_t* kstack_new(void) {
    uint8_t* m = (uint8_t*)kmalloc_aligned(APP_KSTACK + 4096, 4096);
    if (m) paging_guard((uintptr_t)m, 4096, 1);
    return m;
}

static void kstack_free(uint8_t* m) {
    if (!m) return;
    paging_guard((uintptr_t)m, 4096, 0);
    kfree(m);
}

static uintptr_t kstack_top(const uint8_t* m) { return (uintptr_t)m + 4096 + APP_KSTACK; }

/* runs fn(a0, a1, a2) in ring 3 on the stack [stack, stack + size) until
 * it returns or the app ends: its exit code */
static int enter_user(app_proc_t* p, uintptr_t fn, uintptr_t a0, uintptr_t a1, uintptr_t a2,
                      uint8_t* stack, size_t size, uint8_t* kstack, uintptr_t* saved_sp) {
    /* where fn returns to: a system call that ends the thread (at 8 mod 16,
     * as after a call) */
    uintptr_t usp = (((uintptr_t)stack + size) & ~(uintptr_t)15) - 8;
    task_set_user(uspace_cr3(p->us), uspace_tag(p->us), kstack_top(kstack));
    *(uintptr_t*)usp = p->ret_stub;         /* (a stack it mapped: its tables are loaded now) */
    int rc = app_enter_user(fn, a0, a1, a2, usp, saved_sp);
    task_set_user(0, NULL, 0);              /* the kernel's tables: the app's may go now */
    task_set_fs(0);
    return rc;
}
#else
static int thread_tramp(void* arg) {
    athread_t* t = (athread_t*)arg;
    return t->fn(t->arg);
}
#endif

/* a thread's stacks back to the app / the kernel */
static void thread_free(app_proc_t* p, athread_t* t) {
#ifdef __x86_64__
    if (!t->own_stack) stack_unmap(p, t->stack, t->stack_size, APP_GUARD);
#else
    if (!t->own_stack) umem_free_pages(p->mem, t->stack, APP_GUARD);
#endif
#ifdef __x86_64__
    kstack_free(t->kstack);
#endif
    t->stack = NULL;
    t->kstack = NULL;
}

static void thread_entry(void) {
    app_proc_t* p = cur();
    athread_t* t = cur_thread(p);
    if (!p || !t) return;
    if (p->has_term) terminal_vt_set_active(p->vt);
    else task_set_background();
    t->running = 1;
#ifdef __x86_64__
    t->ret = enter_user(p, (uintptr_t)t->fn, (uintptr_t)t->arg, 0, 0, t->stack, t->stack_size, t->kstack, &t->saved_sp);
#else
    t->ret = app_enter(thread_tramp, t, t->stack + t->stack_size, &t->saved_sp);
#endif
    t->running = 0;
    /* The task ends now and its slot (and pid) goes to the next task
     * created - another app's thread, a kernel task. The record stays
     * for banana_thread_join, but must no longer answer to that pid:
     * it did, and a new thread of another app was taken for this one
     * (cur() found this app first) and ran this thread's code again on
     * its old stack - both apps crashed, and then the kernel. */
    pid_clear(t->pid, p->id);
    t->pid = -1;
    uintptr_t f;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory");
    t->done = 1;
    int gone = t->detached;                 /* nobody will join it: its record goes now */
    __asm__ volatile("push %0; popf" :: "r"(f) : "memory", "cc");
    if (gone) { thread_free(p, t); t->used = 0; }
}

/* a new thread running fn(arg): on stack [stack, stack + size) if the app
 * gives one (its memory, kept by it), else on size bytes (0: APP_STACK)
 * of new memory with guard pages under it; its id, or -1 */
static int thread_new(int (*fn)(void*), void* arg, void* stack, unsigned long size) {
    app_proc_t* p = cur();
    if (!p || !fn || p->exit_req || !uok((const void*)fn, 1)) return -1;
    if (stack ? (size < 16384 || !uok(stack, size)) : size > (256ul << 20)) return -1;
    athread_t* t = NULL;
    int id = -1;
    for (int i = 0; i < APP_THREADS; i++) if (!p->threads[i].used) { t = &p->threads[i]; id = i; break; }
    if (!t) return -1;
    memset(t, 0, sizeof(*t));
    t->sig.blocked = sig_state(p)->blocked;     /* (a new thread starts with its creator's mask) */
    if (stack) {
        t->stack = (uint8_t*)stack;
        t->stack_size = size;
        t->own_stack = 1;
    } else {
        t->stack_size = size ? (size + 4095) & ~4095ul : APP_STACK;
#ifdef __x86_64__
        t->stack = stack_map(p, t->stack_size, APP_GUARD);  /* (its mmap region: a fork copies it) */
#else
        t->stack = (uint8_t*)umem_alloc_pages(p->mem, t->stack_size, APP_GUARD);
#endif
    }
#ifdef __x86_64__
    t->kstack = kstack_new();
    if (!t->kstack) { thread_free(p, t); return -1; }
#endif
    if (!t->stack) { thread_free(p, t); return -1; }
    t->fn = fn;
    t->arg = arg;
    t->pid = -1;
    t->used = 1;
    char name[24];
    char base[15];                          /* (ksnprintf has no %.14s) */
    kstrlcpy(base, p->name, sizeof(base));
    ksnprintf(name, sizeof(name), "%s/t%d", base, id + 1);
    int pid = task_create(name, thread_entry);
    if (pid < 0) { thread_free(p, t); t->used = 0; return -1; }
    /* (no record of an ended thread may still answer to this pid) */
    for (int i = 0; i < APP_MAX; i++) {
        if (!g_procs[i].used) continue;
        for (int k = 0; k < APP_THREADS; k++)
            if (&g_procs[i].threads[k] != t && g_procs[i].threads[k].used && g_procs[i].threads[k].pid == pid)
                g_procs[i].threads[k].pid = -1;
    }
    t->pid = pid;                           /* it runs once we yield (no preemption in here) */
    pid_set(pid, p->id, id);
    return id + 1;
}

static int a_thread_create(int (*fn)(void*), void* arg) { return thread_new(fn, arg, NULL, 0); }

static int a_thread_start(int (*fn)(void*), void* arg, void* stack, unsigned long size) {
    return thread_new(fn, arg, stack, size);
}

/* nobody will thread_join it: its record (and stack) go when it ends */
static int a_thread_detach(int id) {
    app_proc_t* p = cur();
    if (!p || id < 1 || id > APP_THREADS) return -1;
    athread_t* t = &p->threads[id - 1];
    if (!t->used || t->detached) return -1;
    uintptr_t f;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(f) :: "memory");
    t->detached = 1;
    int done = t->done;
    __asm__ volatile("push %0; popf" :: "r"(f) : "memory", "cc");
    if (done) { thread_free(p, t); t->used = 0; }
    return 0;
}

/* the calling thread's thread pointer: FS base (64-bit TLS, <pthread.h>) */
static int a_tls_set(void* tp) {
#ifdef __x86_64__
    if (!cur()) return -1;
    task_set_fs((uintptr_t)tp);
    return 0;
#else
    (void)tp;
    return -1;
#endif
}

/* ── signals (version 19): the calls ──────────────────────────────────── */

static sigstate_t* sig_state(app_proc_t* p) {
    athread_t* t = cur_thread(p);
    return t ? &t->sig : &p->msig;
}

static int a_sig_entry(void (*entry)(int, void*, void*)) {
    app_proc_t* p = cur();
    if (!p || (entry && !uok((const void*)entry, 1))) return -1;
    p->sig_entry = (uintptr_t)entry;
    return 0;
}

static int a_sig_action(int sig, int disp, int onstack) {
    app_proc_t* p = cur();
    if (!p || sig < 1 || sig > 64 || disp < 0 || disp > 2) return -1;
    if ((sig == 9 || sig == 19) && disp) return -1;             /* SIGKILL, SIGSTOP: never */
    p->sig_disp[sig] = (uint8_t)disp;
    if (onstack) p->sig_onstack |= 1ull << (sig - 1);
    else p->sig_onstack &= ~(1ull << (sig - 1));
    return 0;
}

static int a_sig_mask(int how, const unsigned long long* set, unsigned long long* old) {
    app_proc_t* p = cur();
    if (!p || (set && !uok(set, 8)) || (old && !uok(old, 8))) return -1;
    sigstate_t* st = sig_state(p);
    if (old) *old = st->blocked;
    if (!set) return 0;
    uint64_t s = *set & ~((1ull << 8) | (1ull << 18));         /* (SIGKILL, SIGSTOP) */
    if (how == 0) st->blocked |= s;
    else if (how == 1) st->blocked &= ~s;
    else if (how == 2) st->blocked = s;
    else return -1;
    return 0;
}

/* ── the system's commands as programs (version 30: kcmd) ─────────────
 * /bin/startx, /bin/install... (apps/kcmd) give their command line here: the
 * old built-in shell's command runs in the calling task, what it writes
 * going to the program's stdout (a terminal: with its colours and cursor
 * moves as escapes) and the keys it reads coming from its stdin */
/* the command runs in a kernel task of its own, the program waiting for it:
 * one that starts an app itself (selftest, run) gives that app its task -
 * in the program's own, the app would take the program's place. What it
 * writes and the keys it reads go through two rings here; the program's
 * task (a_kcmd) moves them to and from its stdout and stdin - only a
 * process's own task may use its descriptors */
#define KJ_OUT 4096
#define KJ_IN  256
typedef struct {
    char*        line;
    int          redirect, ansi, cols, rows;
    char         out[KJ_OUT];
    volatile uint32_t oh, ot;           /* (the command adds at oh, the program takes at ot) */
    char         in[KJ_IN];
    volatile uint32_t ih, it;
    volatile int want_in;               /* it asked for keys */
    volatile int cancel;                /* the program is gone or cannot take more: keys read as ^C, then the end */
    volatile int in_end;                /* stdin ended */
    volatile int done, rc;
} kjob_t;
static kjob_t* g_kjob[TASK_MAX];

static void kcmd_put(void* ctx, const char* s, uint32_t n) {
    kjob_t* j = (kjob_t*)ctx;
    for (uint32_t i = 0; i < n; i++) {
        while ((j->oh + 1) % KJ_OUT == j->ot) {
            if (j->cancel) return;
            task_sleep_ms(1);
        }
        j->out[j->oh] = s[i];
        j->oh = (j->oh + 1) % KJ_OUT;
    }
}

static int kcmd_get(void* ctx, int wait) {
    kjob_t* j = (kjob_t*)ctx;
    for (;;) {
        if (j->it != j->ih) {
            int c = (unsigned char)j->in[j->it];
            j->it = (j->it + 1) % KJ_IN;
            return c;
        }
        if (j->cancel == 1) { j->cancel = 2; return 3; }     /* (as a dead SSH session: ^C, so it gives up) */
        if (j->cancel || j->in_end) return -1;
        j->want_in = 1;
        if (!wait) return 0;
        task_sleep_ms(2);
    }
}

static void kcmd_entry(void) {
    int me = task_current_pid();
    kjob_t* j = me >= 0 && me < TASK_MAX ? g_kjob[me] : NULL;
    if (!j) return;
    int ok = !j->redirect || terminal_redirect_begin(kcmd_put, j, j->ansi, j->cols, j->rows) == 0;
    if (ok && j->redirect) keyboard_redirect_begin(kcmd_get, j);
    j->rc = ok ? shell_exec_line(j->line) : -12;
    if (ok && j->redirect) {
        keyboard_redirect_end();
        terminal_redirect_end();
    }
    g_kjob[me] = NULL;
    j->done = 1;
}

static long a_read(int fd, void* buf, unsigned long len);

/* the program's side: what the command wrote, to stdout (1 if there was some) */
static int kcmd_out(app_proc_t* p, kjob_t* j) {
    char tmp[512];
    uint32_t n = 0;
    while (n < sizeof(tmp) && j->ot != j->oh) {
        tmp[n++] = j->out[j->ot];
        j->ot = (j->ot + 1) % KJ_OUT;
    }
    for (uint32_t d = 0; d < n && !j->cancel; ) {
        long r = fd_write(p, 1, tmp + d, n - d);
        if (r == -11) { task_sleep_ms(2); continue; }
        if (r <= 0) { j->cancel = 1; break; }         /* (a pipe nobody reads...) */
        d += (uint32_t)r;
    }
    return n != 0;
}

/* Once the producer is done, drain every remaining chunk before freeing
 * its job. One kcmd_out call moves at most 512 of the ring's 4095 bytes. */
static void kcmd_drain(app_proc_t* p, kjob_t* j) {
    while (!j->cancel && kcmd_out(p, j)) {}
}

/* ... and keys from stdin, while it wants some */
static int kcmd_in(app_proc_t* p, kjob_t* j) {
    int got = 0;
    while (j->want_in && !j->in_end && (j->ih + 1) % KJ_IN != j->it) {
        kfile_t* f = get_fd(p, 0);
        if (!f) { j->in_end = 1; break; }
        int ready = f->type == KF_PTS ? (pty_poll(f->pty, 0, KF_POLLIN) & (KF_POLLIN | KF_POLLHUP)) != 0
                  : f->type == KF_PIPE ? (kpipe_poll(f, KF_POLLIN) & (KF_POLLIN | KF_POLLHUP)) != 0 : 1;
        if (!ready) break;
        uint8_t c;
        long r = a_read(0, &c, 1);
        if (r == -11) break;
        if (r != 1) { j->in_end = 1; break; }          /* (the end, an error) */
        j->in[j->ih] = (char)c;
        j->ih = (j->ih + 1) % KJ_IN;
        got = 1;
    }
    return got;
}

/* a signal that ends the program waits (SIGHUP, SIGINT, SIGQUIT, SIGTERM - not a SIGWINCH) */
static int kcmd_stopping(app_proc_t* p) {
#ifdef __x86_64__
    sigstate_t* st = sig_state(p);
    uint64_t pend = (st->pending | p->sig_pending) & ~st->blocked;
    return (pend & ((1ull << 0) | (1ull << 1) | (1ull << 2) | (1ull << 14))) != 0;
#else
    (void)p;
    return 0;
#endif
}

static int a_kcmd(const char* line) {
    app_proc_t* p = cur();
    if (!p || !line) return -14;
    kfile_t* o = get_fd(p, 1);
    kjob_t* j = (kjob_t*)kzalloc(sizeof(kjob_t));
    size_t len = strlen(line);
    char* l = (char*)kmalloc(len + 1);
    if (!j || !l) { kfree(j); kfree(l); return -12; }
    memcpy(l, line, len + 1);
    j->line = l;
    j->redirect = !(o && o->type == KF_CONS);                   /* (the console's own: as it was) */
    j->ansi = o && o->type == KF_PTS;
    j->cols = 80;
    j->rows = 24;
    if (o && o->type == KF_PTS) {
        pty_winsize_t ws;
        pty_get_winsize(o->pty, &ws);
        if (ws.ws_col) j->cols = ws.ws_col;
        if (ws.ws_row) j->rows = ws.ws_row;
    }
    int pid = task_create_stack("kcmd", kcmd_entry, 256u << 10);   /* (guarded; the console shell had 64 KiB: selftest needs it) */
    if (pid < 0 || pid >= TASK_MAX) { kfree(l); kfree(j); return -12; }
    g_kjob[pid] = j;                                            /* (before it runs: once we yield) */
    while (!j->done) {
        int busy = kcmd_out(p, j);
        busy |= kcmd_in(p, j);
        /* killed, interrupted: it is told (its keys end) - the program goes
         * once it has returned, never while it runs for it */
        if (!j->cancel && (p->kill_req || p->exit_req || kcmd_stopping(p))) j->cancel = 1;
        if (!busy) task_sleep_ms(2);
    }
    kcmd_drain(p, j);
    int rc = j->rc;
    kfree(l);
    kfree(j);
    return rc;
}

/* sigsuspend (version 30): the mask set and the wait begun at once - no
 * signal taken between the two (the SDK did them as two calls: a SIGCHLD
 * handled at the first one's return, then a wait for ever - zsh) */
static int a_sig_suspend(const unsigned long long* mask) {
    app_proc_t* p = cur();
    if (!p || !mask) return -14;
    sigstate_t* st = sig_state(p);
    st->blocked = *mask & ~((1ull << 8) | (1ull << 18));         /* (SIGKILL, SIGSTOP) */
    breathe(p, 1);
    while (!sig_waiting(p) && !p->exit_req && !p->kill_req) {
        task_sleep_ms(10);                                        /* (a signal sent wakes it: proc_wake) */
        breathe(p, 0);
    }
    return -4;                                                    /* EINTR: its handler runs now */
}

/* pselect / ppoll (version 30): the mask set and the wait begun at once,
 * like sigsuspend - bash's readline waits in pselect with SIGCHLD let in
 * only there: done in two calls, a child's stop or end was taken before
 * the wait began and readline never heard (jobs said Running) */
static int a_poll_mask(void* fds, int n, int timeout_ms, const unsigned long long* mask) {
    app_proc_t* p = cur();
    if (!p || !mask) return -14;
    sig_state(p)->blocked = *mask & ~((1ull << 8) | (1ull << 18));
    return a_poll(fds, n, timeout_ms);                            /* (the caller puts its mask back) */
}

/* to thread id (thread_id()'s numbering), or the app (-1); it is
 * delivered when that thread next goes back to its code */
static int a_sig_send(int thread, int sig) {
    app_proc_t* p = cur();
    if (!p || sig < 0 || sig > 64) return -1;
    if (thread < -1 || thread > APP_THREADS) return -1;
    sigstate_t* st = thread == -1 ? NULL : thread == 0 ? &p->msig : &p->threads[thread - 1].sig;
    if (thread > 0 && (!p->threads[thread - 1].used || p->threads[thread - 1].done)) return -1;
    if (!sig) return 0;                                          /* (only: does it exist?) */
#ifdef __x86_64__
    if (st) SIG_OR(st->pending, 1ull << (sig - 1));
    else SIG_OR(p->sig_pending, 1ull << (sig - 1));
    /* a thread sleeping in wait() looks at its signals (EINTR-like wake) */
    if (thread > 0 && p->threads[thread - 1].pid >= 0) task_wake(p->threads[thread - 1].pid);
    else if (thread == 0 || thread == -1) task_wake(p->pid);
    return 0;
#else
    (void)st;
    return -1;                                                   /* (32-bit: raise() only, in the SDK) */
#endif
}

static int a_sig_altstack(const void* ss, void* old) {
    app_proc_t* p = cur();
    if (!p || (ss && !uok(ss, sizeof(banana_stack_t))) || (old && !uok(old, sizeof(banana_stack_t)))) return -1;
    sigstate_t* st = sig_state(p);
    if (old) {
        banana_stack_t* o = (banana_stack_t*)old;
        o->ss_sp = (void*)st->alt_sp;
        o->ss_size = st->alt_size;
        o->ss_flags = st->alt_size ? 0 : 2;                      /* SS_DISABLE */
    }
    if (ss) {
        const banana_stack_t* n = (const banana_stack_t*)ss;
        if (n->ss_flags & 2) { st->alt_sp = 0; st->alt_size = 0; return 0; }
        if (n->ss_size < 2048 || !uok(n->ss_sp, n->ss_size)) return -1;
        st->alt_sp = (uintptr_t)n->ss_sp;
        st->alt_size = n->ss_size;
    }
    return 0;
}

static unsigned long long a_sig_pending(void) {
    app_proc_t* p = cur();
    if (!p) return 0;
    return sig_state(p)->pending | p->sig_pending;
}

/* 1 if the calling thread has a signal to take now (a wait should end) */
static int sig_waiting(app_proc_t* p) {
#ifdef __x86_64__
    sigstate_t* st = sig_state(p);
    uint64_t ready = (st->pending | p->sig_pending) & ~st->blocked;
    /* (one that would be dropped - ignored, or CHLD CONT URG WINCH left as
     * they are - wakes nothing: a sleep goes on after a ^Z and fg) */
    for (uint64_t r = ready; r; r &= r - 1) {
        int sig = __builtin_ctzll(r) + 1;
        int d = p->sig_disp[sig];
        if (d == BANANA_SIG_IGNORE || (d != BANANA_SIG_CATCH && (sig == 17 || sig == 18 || sig == 23 || sig == 28)))
            ready &= ~(1ull << (sig - 1));
    }
    return ready != 0;
#else
    (void)p;
    return 0;                                   /* (32-bit: nothing is delivered) */
#endif
}

/* the computer's memory for apps, in bytes: what there is and what is free
 * (sysconf(_SC_PHYS_PAGES) - a Java heap is sized from it) */
static void a_mem_info(unsigned long long* total, unsigned long long* avail) {
    meminfo_t mi;
    meminfo_get(&mi);
    uint64_t ht = hmem_total_bytes(), hu = hmem_used_bytes();
    if (total) *total = (uint64_t)mi.total_kb * 1024 + ht;
    if (avail) *avail = (uint64_t)mi.avail_kb * 1024 + (ht > hu ? ht - hu : 0);
}

static int a_app_id(void) { app_proc_t* p = cur(); return p ? p->seq : -1; }

static int a_thread_join(int id) {
    app_proc_t* p = cur();
    if (!p || id < 1 || id > APP_THREADS) return -1;
    athread_t* t = &p->threads[id - 1];
    if (!t->used || t == cur_thread(p)) return -1;
    while (!t->done) {
        breathe(p, 1);
        task_sleep_ms(1);
    }
    int r = t->ret;
    thread_free(p, t);
    t->used = 0;
    return r;
}

static int a_thread_id(void) {
    app_proc_t* p = cur();
    athread_t* t = cur_thread(p);
    return t ? (int)(t - p->threads) + 1 : 0;
}

static void a_thread_exit(int ret) {
    app_proc_t* p = cur();
    athread_t* t = cur_thread(p);
    if (t) leave_thread(p, t, ret);
    if (p) leave(p, ret);                   /* the main thread: the app ends */
    for (;;) task_sleep_ms(1000);
}

static int a_gpu_call(unsigned op, const void* request, unsigned request_size, void* data, unsigned size) {
    app_proc_t* p = cur();
    return p ? gpu_virgl_call(p->id + 1, op, request, request_size, data, size) : -1;
}
static int a_cpu_count(void) { return cpu_count(); }

/* v24: an Intel GPU's 3D - Linux's i915 requests (kernel/gpu_gem.c, which
 * checks every pointer in them with app_user_ok) */
static int a_drm_ioctl(unsigned long req, void* arg) {
    app_proc_t* p = cur();
    return p ? gem_ioctl(p->id + 1, req, arg) : -19;
}

#ifdef __x86_64__
int app_user_ok(const void* ptr, uint64_t n) {
    app_proc_t* p = cur();
    return p && own(p, ptr, (size_t)n);
}
uspace_t* app_user_space(void) {
    app_proc_t* p = cur();
    return p ? p->us : NULL;
}
#endif

/* wait / wake: a thread sleeps while *addr == expected, until another one
 * changes it and calls wake (or the timeout passes) - what the SDK builds
 * condition variables and semaphores on */
#define WAITERS 256
static struct { volatile int* addr; int pid; } g_waiters[WAITERS];

static int waiter_add(volatile int* addr) {
    for (int i = 0; i < WAITERS; i++)
        if (!g_waiters[i].addr) { g_waiters[i].addr = addr; g_waiters[i].pid = task_current_pid(); return i; }
    return -1;
}

static int a_wait(volatile int* addr, int expected, int timeout_ms) {
    app_proc_t* p = cur();
    if (!p || !addr) return -1;
    uint32_t end = timer_ms() + (uint32_t)(timeout_ms > 0 ? timeout_ms : 0);
    breathe(p, 1);                          /* paints, notices Ctrl+C / End task (may not return) */
    for (;;) {
        /* woken: straight back to the app (breathing first would yield) */
        if (*addr != expected) return 0;
        if (timeout_ms >= 0 && (int32_t)(timer_ms() - end) >= 0) return 1;
        breathe(p, 0);                      /* (every 20 ms) */
        if (sig_waiting(p)) return 0;       /* a signal for it: it goes to the handler first */
        if (*addr != expected) return 0;
        /* registered only while asleep: if the app is stopped in breathe()
         * no stale entry is left behind; a wake in between costs <= 10 ms */
        int slot = waiter_add(addr);
        if (*addr == expected) task_sleep_ms(slot >= 0 ? 10 : 1);
        if (slot >= 0) g_waiters[slot].addr = NULL;
    }
}

static int a_wake(volatile int* addr, int count) {
    int n = 0;
    for (int i = 0; i < WAITERS; i++) {
        if (g_waiters[i].addr != addr) continue;
        task_wake(g_waiters[i].pid);
        if (++n >= count && count > 0) break;
    }
    return n;
}

/* kernel/idt.c, on every hardware interrupt: if it came while an app's
 * own code was running (not inside the kernel), switching to another task
 * is safe - so a busy app or thread cannot freeze the desktop, and the
 * threads of an app share the CPU. An app asked to stop is stopped here. */
#ifdef __x86_64__
static void sig_preempt(app_proc_t* p, void* regs);
#endif

void app_preempt(uintptr_t ip, int from_user, void* regs) {
    app_proc_t* p = cur();
#ifdef __x86_64__
    /* (anything in ring 3 is the app's: its code, and code it made - tcc, OpenCL) */
    if (!p || !from_user) return;
    { int me = task_current_pid(); if (me >= 0 && me < TASK_MAX) g_lastip[me] = ip; }
#else
    (void)from_user;
    if (!p || ip < p->code_lo || ip >= p->code_hi) return;
#endif
    athread_t* t = cur_thread(p);
    if (t ? !t->running : !p->running) return;
    __asm__ volatile("sti");
    web_check_budget(p);
    if (p->exit_req || p->kill_req) leave(p, p->exit_req ? p->exit_req_code : 137);
    /* what an app call does: paint the desktop (the app may run in the task
     * that draws it), notice Ctrl+C / End task, let the other tasks run */
    breathe(p, 0);
    task_maybe_yield();                     /* slice over, or a woken task waits (task_should_yield) */
#ifdef __x86_64__
    sig_preempt(p, regs);                   /* signals sent to it: to their handlers */
#else
    (void)regs;
#endif
    if (p->has_term) terminal_vt_set_active(p->vt);
    /* a free processor core takes the app code from here (kernel/task.h):
     * nothing of the kernel may run after this - it may be on that core now */
    task_offload();
    __asm__ volatile("cli");
}

uintptr_t app_fault_stack(int pid) {
    for (int i = 0; i < APP_MAX; i++) {
        app_proc_t* p = &g_procs[i];
        if (!p->used) continue;
        if (p->pid == pid && p->running) return p->saved_sp;
        for (int t = 0; t < APP_THREADS; t++)
            if (p->threads[t].used && p->threads[t].pid == pid && p->threads[t].running) return p->threads[t].saved_sp;
    }
    return 0;
}

static void a_exit(int code) {
    app_proc_t* p = cur();
    if (p) leave(p, code);
    for (;;) task_sleep_ms(1000);    /* not an app: cannot happen */
}

#ifdef __x86_64__
/* ── system calls (64-bit) ─────────────────────────────────────────────
 * An app runs in ring 3 and reaches the kernel only through int 0x80:
 * every function entry of the table it gets is a stub "mov eax, slot;
 * int 0x80; ret" in its own memory (make_uapi), so apps built for the
 * table of function pointers work unchanged. Before a call runs, every
 * pointer it was given is checked to be the app's memory (the kernel
 * would otherwise read or write anywhere on its behalf); a call that
 * fails the check stops the app, like a bad memory access would. */

#define API_SLOTS  (sizeof(banana_api_t) / sizeof(void*))
#define SYS_RETURN API_SLOTS            /* the entry or a thread's function returned (ret_stub) */
#define SYS_ARGS   12                   /* the most a call takes (http_request: 11) */

/* What each argument is, one letter per argument:
 *   i  a number (or a pointer the call checks itself: x)
 *   s  a string            b / B  a buffer, its size the next argument (int / unsigned long)
 *   I  int*   L  a pointer or unsigned long out   o  a struct of the slot's osize */
typedef struct { const char* args; uint16_t osize; const char* name; } sysspec_t;
static sysspec_t g_spec[API_SLOTS];

#define SPEC(f, a, o) (g_spec[__builtin_offsetof(banana_api_t, f) / sizeof(void*)] = (sysspec_t){ a, o, #f })

/* ── ps (version 30): the processes as the system sees them ── */
static uint32_t app_mem_bytes(app_proc_t* p);
static int a_proc_list(banana_procinfo_t* out, int max) {
    if (!out || max <= 0 || max > 4096) return -22;
    if (!uok(out, (size_t)max * sizeof(*out))) return -14;
    int n = 0;
    for (int i = 0; i < APP_MAX && n < max; i++) {
        app_proc_t* t = &g_procs[i];
        if (!t->used) continue;
        banana_procinfo_t* o = &out[n++];
        memset(o, 0, sizeof(*o));
        o->pid = t->seq;
        o->ppid = t->ppid;
        o->pgid = t->pgid;
        o->sid = t->sid;
        o->tty = t->ctty ? tty_num(t->ctty) : t->has_term ? -2 : -1;
        o->tpgid = t->ctty ? pty_fg(t->ctty) : 0;
        int sys = t->pid >= 0 && t->pid < TASK_MAX ? g_insys[t->pid] : 0;
        o->state = t->zombie ? 'Z' : t->stopped ? 'T' : sys ? 'S' : 'R';
        int th = 1;
        for (int k = 0; k < APP_THREADS; k++) if (t->threads[k].used && !t->threads[k].done) th++;
        o->threads = (unsigned)th;
        o->mem = t->zombie ? 0 : app_mem_bytes(t);
        o->start_ms = t->start_ms;
        o->sigpending = t->sig_pending;
        int ts = 0;
        uint32_t cms = 0, pct = 0;
        if (t->pid >= 0 && !task_query(t->pid, &ts, &cms, &pct)) {
            o->sched = ts == TASK_RUNNING ? 'R' : ts == TASK_READY ? 'r' : ts == TASK_SLEEPING ? 'S' : ts == TASK_AWAY ? 'A' : '?';
            o->cpu_ms = cms;
            o->cpu_pct = pct;
        }
        if (t->pid >= 0 && t->pid < TASK_MAX && g_lastip[t->pid]) o->uip = g_lastip[t->pid] - t->base;
        kstrlcpy(o->name, t->name, sizeof(o->name));
        if (sys && g_spec[sys - 1].name) kstrlcpy(o->wchan, g_spec[sys - 1].name, sizeof(o->wchan));
        kstrlcpy(o->cmdline, t->cmdline[0] ? t->cmdline : t->name, sizeof(o->cmdline));
    }
    return n;
}


/* Calls that may run on whichever core the app thread makes them on - they
 * touch only the app's own state and what has locks of its own (its
 * memory: kernel/umem.c, the heap) and never wait or switch tasks. Every
 * other call first takes the thread back to the boot core, where the rest
 * of the kernel runs. wait has a quick case here and goes home only
 * when it really has to sleep (LOCAL_WAIT). */
enum { LOCAL_NO = 0, LOCAL_YES, LOCAL_WAIT };
static uint8_t g_local[API_SLOTS];
#define LOCAL(f, how) (g_local[__builtin_offsetof(banana_api_t, f) / sizeof(void*)] = (how))

static void spec_init(void) {
    SPEC(exit, "i", 0);
    SPEC(write, "iBi", 0);
    SPEC(getchar, "", 0);
    SPEC(trygetchar, "", 0);
    SPEC(readline, "bi", 0);
    SPEC(set_color, "ii", 0);
    SPEC(clear_screen, "", 0);
    SPEC(term_size, "II", 0);
    SPEC(set_cursor, "ii", 0);
    SPEC(malloc, "i", 0);
    SPEC(free, "i", 0);                 /* (umem knows its own blocks) */
    SPEC(realloc, "ii", 0);
    SPEC(open, "si", 0);
    SPEC(read, "iBi", 0);
    SPEC(seek, "iii", 0);
    SPEC(close, "i", 0);
    SPEC(remove, "s", 0);
    SPEC(mkdir, "s", 0);
    SPEC(rename, "ss", 0);
    SPEC(stat, "so", sizeof(banana_stat_t));
    SPEC(readdir, "sio", sizeof(banana_dirent_t));
    SPEC(getcwd, "bi", 0);
    SPEC(chdir, "s", 0);
    SPEC(ticks_ms, "", 0);
    SPEC(sleep_ms, "i", 0);
    SPEC(yield, "", 0);
    SPEC(localtime, "o", sizeof(banana_time_t));
    SPEC(random, "", 0);
    SPEC(win_open, "sii", 0);
    SPEC(win_pixels, "i", 0);
    SPEC(win_update, "i", 0);
    SPEC(win_event, "io", sizeof(banana_event_t));
    SPEC(win_close, "i", 0);
    SPEC(win_set_title, "is", 0);
    SPEC(win_size, "iII", 0);
    SPEC(draw_text, "iiiiiisii", 0);    /* px: checked in it */
    SPEC(http_get, "sLLbibi", 0);
    SPEC(audio_play, "Biiii", 0);
    SPEC(audio_beep, "ii", 0);
    SPEC(audio_busy, "", 0);
    SPEC(clipboard_set, "Bi", 0);
    SPEC(clipboard_get, "L", 0);
    SPEC(interrupted, "", 0);
    SPEC(win_set_resizable, "iii", 0);
    SPEC(thread_create, "ii", 0);       /* fn: checked in it; arg: the app's business */
    SPEC(thread_join, "i", 0);
    SPEC(thread_id, "", 0);
    SPEC(thread_exit, "i", 0);
    SPEC(cpu_count, "", 0);
    SPEC(gpu_call, "ibibi", 0);
    SPEC(wait, "Iii", 0);
    SPEC(wake, "ii", 0);                /* (addr is only compared) */
    SPEC(web_open, "ii", 0);
    SPEC(web_close, "i", 0);
    SPEC(web_load, "is", 0);
    SPEC(web_load_html, "iss", 0);
    SPEC(web_resize, "iii", 0);
    SPEC(web_poll, "i", 0);
    SPEC(web_draw, "iiiiiii", 0);       /* px: checked in it */
    SPEC(web_event, "io", sizeof(banana_event_t));
    SPEC(web_scroll, "ii", 0);
    SPEC(web_go, "ii", 0);
    SPEC(web_info, "ibibi", 0);
    SPEC(web_eval, "isbi", 0);
    SPEC(web_message, "ibi", 0);
    SPEC(web_post, "is", 0);
    SPEC(font_draw, "iiiiiiiisi", 0);   /* px: checked in it */
    SPEC(font_width, "iis", 0);
    SPEC(font_metrics, "iiIII", 0);
    SPEC(audio_queued_ms, "", 0);
    SPEC(audio_stop, "", 0);
    SPEC(audio_volume, "i", 0);
    SPEC(win_media_keys, "i", 0);
    SPEC(http_request, "ssBisLLbibi", 0);
    SPEC(image_load, "sIIbi", 0);
    SPEC(set_wallpaper, "sibi", 0);
    SPEC(http_fetch, "ii", 0);          /* checked in it (the request points at more) */
    SPEC(key_mods, "", 0);
    SPEC(win_wheel, "i", 0);
    SPEC(win_capture_mouse, "ii", 0);
    SPEC(win_mouse_captured, "i", 0);
    SPEC(time_utc, "", 0);
    SPEC(utc_offset, "i", 0);
    SPEC(language, "bi", 0);
    SPEC(drm_ioctl, "ii", 0);            /* (its structure: checked in kernel/gpu_gem.c, by its size) */
    SPEC(fd_pipe, "i", 0);              /* fds: checked in it */
    SPEC(fd_socketpair, "i", 0);
    SPEC(fd_dup, "iii", 0);
    SPEC(fd_fcntl, "iii", 0);
    SPEC(fd_info, "io", sizeof(banana_fdinfo_t));
    SPEC(proc_fork, "", 0);
    SPEC(proc_exec, "sii", 0);           /* argv, envp: checked in it */
    SPEC(proc_wait, "iIio", 144);        /* (struct rusage) */
    SPEC(proc_ids, "io", 16);
    SPEC(proc_setpgid, "ii", 0);
    SPEC(proc_setsid, "", 0);
    SPEC(proc_kill, "ii", 0);
    SPEC(tty_ioctl, "iii", 0);            /* arg: checked in it, by the request */
    SPEC(fs_ustat, "sio", sizeof(banana_ustat_t));
    SPEC(fs_fdstat, "io", sizeof(banana_ustat_t));
    SPEC(fs_chmod, "si", 0);
    SPEC(fs_chown, "sii", 0);
    SPEC(fs_symlink, "ss", 0);
    SPEC(fs_readlink, "sbi", 0);
    SPEC(fs_utime, "sii", 0);
    SPEC(fs_unlink, "si", 0);
    SPEC(fs_fdattr, "iiii", 0);
    SPEC(fs_fdpath, "ibi", 0);
    SPEC(fs_link, "ss", 0); SPEC(fs_utimes, "siii", 0);
    SPEC(proc_alarm, "iiio", 2 * sizeof(long long));
    SPEC(proc_list, "ii", 0);
    SPEC(sig_suspend, "L", 0);
    SPEC(kcmd, "s", 0);
    SPEC(fd_sync, "i", 0);
    SPEC(web_ipc, "ibibi", 0);
    SPEC(poll_mask, "iiiL", 0);           /* fds: checked in a_poll */
    SPEC(pkg_install, "sbi", 0);
    SPEC(app_run, "siibi", 0);          /* argv: checked in it */
    SPEC(repo_update, "bi", 0);
    SPEC(repo_list, "ii", 0);           /* out: checked in it */
    SPEC(repo_install, "sbi", 0);
    SPEC(repo_remove, "sbi", 0);
    SPEC(repo_status, "bi", 0);
    SPEC(repo_icon, "sbi", 0);
    SPEC(repo_upgrade, "bi", 0);
    SPEC(gamepad, "io", sizeof(banana_gamepad_t));
    SPEC(win_dropped, "iibi", 0);
    SPEC(key_down, "i", 0);
    SPEC(repo_sources, "ii", 0);        /* out: checked in it */
    SPEC(repo_add_source, "ssibi", 0);
    SPEC(repo_remove_source, "sbi", 0);
    SPEC(vm_map, "iiii", 0);            /* (addresses: checked in uvm) */
    SPEC(vm_unmap, "ii", 0);
    SPEC(vm_protect, "iii", 0);
    SPEC(thread_start, "iiii", 0);      /* fn, stack: checked in it */
    SPEC(thread_detach, "i", 0);
    SPEC(tls_set, "i", 0);              /* (only loaded into FS: the app's own business) */
    SPEC(mem_info, "LL", 0);
    SPEC(app_id, "", 0);
    SPEC(sock_open, "iii", 0);
    SPEC(sock_bind, "ibi", 0);
    SPEC(sock_listen, "ii", 0);
    SPEC(sock_accept, "iii", 0);          /* sa, len: checked in it */
    SPEC(sock_connect, "ibi", 0);
    SPEC(sock_send, "iBiiii", 0);         /* sa: checked in it */
    SPEC(sock_recv, "iBiiii", 0);         /* sa, salen: checked in it */
    SPEC(sock_shutdown, "ii", 0);
    SPEC(sock_opt, "iiiiii", 0);          /* val, len: checked in it */
    SPEC(sock_name, "iiii", 0);           /* sa, len: checked in it */
    SPEC(sock_ctl, "iii", 0);
    SPEC(poll, "iii", 0);                 /* fds: checked in it */
    SPEC(resolve, "si", 0);               /* ip: checked in it */
    SPEC(sig_entry, "i", 0);              /* checked in it */
    SPEC(sig_action, "iii", 0);
    SPEC(sig_mask, "iii", 0);             /* checked in it */
    SPEC(sig_send, "ii", 0);
    SPEC(sig_altstack, "ii", 0);          /* checked in it */
    SPEC(sig_pending, "", 0);

    LOCAL(ticks_ms, LOCAL_YES);
    LOCAL(cpu_count, LOCAL_YES);
    LOCAL(thread_id, LOCAL_YES);
    LOCAL(key_mods, LOCAL_YES);
    LOCAL(malloc, LOCAL_YES);
    LOCAL(free, LOCAL_YES);
    LOCAL(realloc, LOCAL_YES);
    LOCAL(draw_text, LOCAL_YES);
    LOCAL(wake, LOCAL_YES);             /* (task_wake only moves a deadline: any core may) */
    LOCAL(wait, LOCAL_WAIT);
    LOCAL(vm_map, LOCAL_YES);           /* (its own tables and memory, under their locks) */
    LOCAL(vm_unmap, LOCAL_YES);
    LOCAL(vm_protect, LOCAL_YES);
    LOCAL(app_id, LOCAL_YES);
    LOCAL(sig_mask, LOCAL_YES);           /* (the calling thread's own state) */
    LOCAL(sig_pending, LOCAL_YES);
}

/* the pointers of a call, against the app's memory (NULL is left to the call) */
static int args_ok(app_proc_t* p, const sysspec_t* sp, const uint64_t* a) {
    for (int i = 0; sp->args[i]; i++) {
        const void* ptr = (const void*)(uintptr_t)a[i];
        if (!ptr) continue;
        size_t n;
        switch (sp->args[i]) {
        case 's': if (!own_str(p, (const char*)ptr)) return 0; continue;
        case 'b': n = (int32_t)a[i + 1] > 0 ? (size_t)(int32_t)a[i + 1] : 1; break;
        case 'B': n = a[i + 1] ? (size_t)a[i + 1] : 1; if (a[i + 1] >> 32) return 0; break;
        case 'I': n = 4; break;
        case 'L': n = 8; break;
        case 'o': n = sp->osize; break;
        default: continue;
        }
        if (!own(p, ptr, n)) return 0;
    }
    return 1;
}

/* what kernel/isr64.asm saved (the layout of idt.c's registers_t) */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
} sysframe_t;

/* ── signals (64-bit delivery) ─────────────────────────────────────────
 * A signal for a handler is delivered by changing the registers a thread
 * goes back to ring 3 with: a frame (siginfo, the registers it had in a
 * ucontext, its FPU/SSE state) is put on its stack - or its alternate
 * stack - and it resumes at the SDK's entry point, which calls the handler
 * and returns to the sigreturn stub: the kernel then takes the registers
 * back from the frame (maybe changed by the handler). */

#define SYS_SIGRETURN (API_SLOTS + 1)
#define STUB 16                         /* bytes per call stub in the app's table (make_uapi) */

static void __attribute__((noreturn)) bad_call(app_proc_t* p, uint64_t slot, const char* why);

/* Linux's register order in mcontext gregs */
enum { G_R8, G_R9, G_R10, G_R11, G_R12, G_R13, G_R14, G_R15, G_RDI, G_RSI, G_RBP, G_RBX, G_RDX,
       G_RAX, G_RCX, G_RSP, G_RIP, G_EFL, G_CSGSFS, G_ERR, G_TRAPNO, G_OLDMASK, G_CR2 };

typedef struct {
    banana_siginfo_t  info;
    banana_ucontext_t uc;
} sigbody_t;

static sigstate_t* sigst(app_proc_t* p) {
    athread_t* t = cur_thread(p);
    return t ? &t->sig : &p->msig;
}

/* sig for a handler now (the thread's registers in r): 1 if delivered */
static int sig_deliver(app_proc_t* p, sysframe_t* r, int sig, int code, uintptr_t addr, uint32_t trapno) {
    sigstate_t* st = sigst(p);
    if (!p->sig_entry || p->sig_disp[sig] != BANANA_SIG_CATCH) return 0;
    uint64_t sp = r->rsp;
    int on_alt = st->alt_size && sp - st->alt_sp < st->alt_size;
    if (((p->sig_onstack >> (sig - 1)) & 1) && st->alt_size && !on_alt) sp = st->alt_sp + st->alt_size;
    else sp -= 128;                                      /* (past any red zone) */
    uint64_t body_at = (sp - sizeof(sigbody_t)) & ~15ull;
    uint64_t top = body_at - 8;                          /* the return address: as after a call */
    if (!uvm_user_writable(p->us, p->mem, (uintptr_t)top, sizeof(sigbody_t) + 8)) return 0;
    sigbody_t* b = (sigbody_t*)(uintptr_t)body_at;
    memset(b, 0, sizeof(*b));
    b->info.si_signo = sig;
    b->info.si_code = code;
    b->info.si_addr = (void*)addr;
    b->info.si_pid = p->seq;
    b->info.si_uid = 1000;
    long long* g = b->uc.uc_mcontext.gregs;
    g[G_R8] = (long long)r->r8;   g[G_R9] = (long long)r->r9;   g[G_R10] = (long long)r->r10; g[G_R11] = (long long)r->r11;
    g[G_R12] = (long long)r->r12; g[G_R13] = (long long)r->r13; g[G_R14] = (long long)r->r14; g[G_R15] = (long long)r->r15;
    g[G_RDI] = (long long)r->rdi; g[G_RSI] = (long long)r->rsi; g[G_RBP] = (long long)r->rbp; g[G_RBX] = (long long)r->rbx;
    g[G_RDX] = (long long)r->rdx; g[G_RAX] = (long long)r->rax; g[G_RCX] = (long long)r->rcx; g[G_RSP] = (long long)r->rsp;
    g[G_RIP] = (long long)r->rip; g[G_EFL] = (long long)r->rflags; g[G_CSGSFS] = (long long)r->cs;
    g[G_ERR] = (long long)r->err_code; g[G_TRAPNO] = trapno; g[G_CR2] = (long long)addr;
    b->uc.uc_mcontext.fpregs = (void*)b->uc.fpregs_mem;
    b->uc.uc_sigmask[0] = st->blocked;
    b->uc.uc_stack.ss_sp = (void*)st->alt_sp;
    b->uc.uc_stack.ss_size = st->alt_size;
    b->uc.uc_stack.ss_flags = st->alt_size ? (on_alt ? 1 : 0) : 2;     /* SS_ONSTACK / SS_DISABLE */
    __asm__ volatile("fxsave64 (%0)" :: "r"(b->uc.fpregs_mem) : "memory");   /* (the app's: the kernel has no FPU code) */
    *(uint64_t*)(uintptr_t)top = p->sigret_stub;
    st->blocked |= 1ull << (sig - 1);                    /* (the SDK's entry sets sa_mask) */
    r->rip = p->sig_entry;
    r->rsp = top;
    r->rdi = (uint64_t)sig;
    r->rsi = (uint64_t)(uintptr_t)&b->info;
    r->rdx = (uint64_t)(uintptr_t)&b->uc;
    r->rax = 0;
    r->rflags &= ~0x500ull;                              /* TF and DF off, as the ABI wants */
    return 1;
}

/* the registers back from a handler's frame (sigreturn) */
static void sig_return(app_proc_t* p, sysframe_t* r) {
    sigbody_t* b = (sigbody_t*)(uintptr_t)r->rsp;        /* (the handler's ret popped the stub's address) */
    if (!own(p, b, sizeof(*b))) bad_call(p, SYS_SIGRETURN, "a signal frame that is not its own");
    const long long* g = b->uc.uc_mcontext.gregs;
    r->r8 = (uint64_t)g[G_R8];   r->r9 = (uint64_t)g[G_R9];   r->r10 = (uint64_t)g[G_R10]; r->r11 = (uint64_t)g[G_R11];
    r->r12 = (uint64_t)g[G_R12]; r->r13 = (uint64_t)g[G_R13]; r->r14 = (uint64_t)g[G_R14]; r->r15 = (uint64_t)g[G_R15];
    r->rdi = (uint64_t)g[G_RDI]; r->rsi = (uint64_t)g[G_RSI]; r->rbp = (uint64_t)g[G_RBP]; r->rbx = (uint64_t)g[G_RBX];
    r->rdx = (uint64_t)g[G_RDX]; r->rax = (uint64_t)g[G_RAX]; r->rcx = (uint64_t)g[G_RCX]; r->rsp = (uint64_t)g[G_RSP];
    r->rip = (uint64_t)g[G_RIP];
    /* only the arithmetic flags, DF, TF and AC are the app's to set */
    const uint64_t user = 0x40DD5ull;
    r->rflags = (r->rflags & ~user) | ((uint64_t)g[G_EFL] & user);
    /* the FPU/SSE state: MXCSR's reserved bits would fault in fxrstor */
    *(uint32_t*)(b->uc.fpregs_mem + 24) &= 0xFFBFu;
    __asm__ volatile("fxrstor64 (%0)" :: "r"(b->uc.fpregs_mem) : "memory");
    sigstate_t* st = sigst(p);
    st->blocked = b->uc.uc_sigmask[0] & ~((1ull << 8) | (1ull << 18));    /* (never SIGKILL, SIGSTOP) */
}

/* a pending signal the thread does not block, taken off the list: 0 if none */
static int sig_take(app_proc_t* p, sigstate_t* st) {
    uint64_t ready = (st->pending | p->sig_pending) & ~st->blocked;
    if (!ready) return 0;
    int sig = __builtin_ctzll(ready) + 1;
    uint64_t bit = 1ull << (sig - 1);
    if (st->pending & bit) SIG_AND(st->pending, ~bit);
    else SIG_AND(p->sig_pending, ~bit);
    return sig;
}

/* the thread goes back to ring 3 with registers r: its pending signals now */
static void sig_check(app_proc_t* p, sysframe_t* r) {
    for (int guard = 0; guard < 64; guard++) {
        if (p->stopped) stop_wait(p);                  /* (stopped: not back to its code until SIGCONT) */
        sigstate_t* st = sigst(p);
        int sig = sig_take(p, st);
        if (!sig) return;
        int d = p->sig_disp[sig];
        if (d == BANANA_SIG_CATCH && sig_deliver(p, r, sig, -6 /* SI_TKILL */, 0, 0)) return;
        if (d == BANANA_SIG_IGNORE) continue;
        /* the default: these are ignored, the rest end the app */
        if (sig == 17 || sig == 18 || sig == 23 || sig == 28) continue;   /* CHLD CONT URG WINCH */
        if (sig >= 19 && sig <= 22) {                  /* STOP TSTP TTIN TTOU: it stops */
            proc_stop(p, sig);
            stop_wait(p);
            continue;
        }
        if (sig != 1 && sig != 2 && sig != 9 && sig != 13 && sig != 15)   /* (HUP INT KILL PIPE TERM: asked for, no crash - a shell says so) */
            ksnprintf(p->crash_msg, sizeof(p->crash_msg), "%s: ended by signal %d", p->name, sig);
        p->term_sig = sig;
        leave(p, 128 + sig);
    }
}

/* kernel/idt.c, a CPU exception in app code (ring 3): 1 if it went to the
 * app's handler as a signal (the CPU resumes there), 0 to stop the app */
int app_signal_fault(void* regs) {
    sysframe_t* r = (sysframe_t*)regs;
    app_proc_t* p = cur();
    if (!p || !p->sig_entry || (r->cs & 3) != 3) return 0;
    athread_t* t = cur_thread(p);
    if (t ? !t->running : !p->running) return 0;
    uint32_t v = (uint32_t)r->int_no;
    uintptr_t addr = 0;
    int sig, code;
    switch (v) {
    case 14:
        __asm__ volatile("mov %%cr2, %0" : "=r"(addr));
        sig = 11; code = (r->err_code & 1) ? 2 /* SEGV_ACCERR */ : 1 /* SEGV_MAPERR */;
        break;
    case 13: case 12: sig = 11; code = 128 /* SI_KERNEL */; break;
    case 0:  sig = 8; code = 1 /* FPE_INTDIV */; break;
    case 16: case 19: sig = 8; code = 0; break;
    case 6:  sig = 4; code = 2 /* ILL_ILLOPN */; addr = (uintptr_t)r->rip; break;
    case 1: case 3: sig = 5; code = 1 /* TRAP_BRKPT */; break;
    case 17: sig = 7; code = 1 /* BUS_ADRALN */; break;
    default: return 0;
    }
    if ((sigst(p)->blocked >> (sig - 1)) & 1) return 0;  /* a fault it blocks: it cannot go on */
    return sig_deliver(p, r, sig, code, addr, v);
}

/* the timer interrupted app code (kernel/idt.c via app_preempt) */
static void sig_preempt(app_proc_t* p, void* regs) {
    sysframe_t* r = (sysframe_t*)regs;
    if (!r || (r->cs & 3) != 3) return;
    sigstate_t* st = sigst(p);
    if ((st->pending | p->sig_pending) & ~st->blocked) sig_check(p, r);
}

typedef uint64_t (*sysfn_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                            uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

static void __attribute__((noreturn)) bad_call(app_proc_t* p, uint64_t slot, const char* why) {
    ksnprintf(p->crash_msg, sizeof(p->crash_msg), "%s: %s (system call %u) - it was stopped",
              p->name, why, (uint32_t)slot);
    leave(p, 139);
}

/* the arguments of a call: the first six from the registers, the rest
 * from the app's stack; 0 if they are not what its spec says */
static int gather_args(app_proc_t* p, const sysspec_t* sp, const sysframe_t* r, uint64_t* a) {
    a[0] = r->rdi; a[1] = r->rsi; a[2] = r->rdx; a[3] = r->rcx; a[4] = r->r8; a[5] = r->r9;
    for (int i = 6; i < SYS_ARGS; i++) a[i] = 0;
    uint32_t nargs = (uint32_t)strlen(sp->args);
    if (nargs > 6) {
        /* above the return address of the app's call */
        const uint64_t* st = (const uint64_t*)(uintptr_t)(r->rsp + 8);
        if (!own(p, st, (nargs - 6) * 8)) return 0;
        for (uint32_t i = 6; i < nargs; i++) a[i] = st[i - 6];
    }
    return args_ok(p, sp, a);
}

#define API_SLOT(f) (__builtin_offsetof(banana_api_t, f) / sizeof(void*))
static int web_syscall_allowed(uint64_t slot) {
    switch (slot) {
    case API_SLOT(exit): case API_SLOT(malloc): case API_SLOT(free):
    case API_SLOT(realloc): case API_SLOT(ticks_ms): case API_SLOT(sleep_ms):
    case API_SLOT(yield): case API_SLOT(vm_map): case API_SLOT(vm_unmap):
    case API_SLOT(vm_protect): case API_SLOT(tls_set): case API_SLOT(thread_id):
    /* Read-only UTC/timezone queries back Date/Intl in the SDK runtime. */
    case API_SLOT(time_utc): case API_SLOT(utc_offset):
    case API_SLOT(localtime): case API_SLOT(random): case API_SLOT(web_ipc):
        return 1;
    default: return 0;
    }
}
static int in_uvm(uint64_t v) { return v >= UVM_BASE && v - UVM_BASE < UVM_SIZE; }

/* a call on another core, interrupts off: 1 if it was done here, 0 if it
 * must go to the boot core (where everything is checked again) */
static int syscall_here(sysframe_t* r) {
    uint64_t slot = r->rax;
    if (slot >= API_SLOTS || !g_local[slot] || !g_spec[slot].args) return 0;
    app_proc_t* p = cur();
    if (!p || p->web_restricted || p->exit_req || p->kill_req) return 0;     /* (the boot core ends it) */
    uint64_t a[SYS_ARGS];
    /* app memory in its mmap region may be in the swap area on the disk: a
     * call that would read it goes to the boot core, where a fault may wait
     * (interrupts are off here) - the vm_* calls only take its addresses.
     * Checked before the arguments are (that reads strings, the stack) */
    int vm = slot == API_SLOT(vm_map) || slot == API_SLOT(vm_unmap) || slot == API_SLOT(vm_protect);
    if (strlen(g_spec[slot].args) > 6 && in_uvm(r->rsp)) return 0;
    uint64_t reg[6] = { r->rdi, r->rsi, r->rdx, r->rcx, r->r8, r->r9 };
    uint32_t nargs = (uint32_t)strlen(g_spec[slot].args);
    if (!vm) for (uint32_t i = 0; i < nargs && i < 6; i++) if (in_uvm(reg[i])) return 0;   /* (the others hold what was there) */
    if (!gather_args(p, &g_spec[slot], r, a)) return 0;
    if (!vm) for (uint32_t i = 6; i < nargs && i < SYS_ARGS; i++) if (in_uvm(a[i])) return 0;
    if (g_local[slot] == LOCAL_WAIT) {
        /* the value changed already: nothing to wait for */
        if (!a[0] || *(volatile int*)(uintptr_t)a[0] == (int)a[1]) return 0;
        r->rax = 0;
        return 1;
    }
    sysfn_t fn = (sysfn_t)((void* const*)&g_api)[slot];
    r->rax = fn(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11]);
    return 1;
}

/* In a virtual machine, a core that runs app code may never leave the
 * guest: system calls made there stay there, and a busy app thread gives
 * the hypervisor no exit for seconds. VirtualBox on Hyper-V (its "NEM"
 * mode - every Windows PC with WSL 2) runs the machine's timers on such a
 * busy virtual CPU when the others halt: they waited for it, the boot
 * core's tick stopped, and the whole system froze while Minecraft ran.
 * So a core running app code exits to the hypervisor at least once a
 * millisecond, by a write to port 0x80 (the classic I/O delay: no device). */
static int      g_vm_exits = -1;                 /* -1: not known yet */
static uint64_t g_vm_exit_at[SMP_MAX_CPUS];
static uint64_t g_vm_probe_at[SMP_MAX_CPUS];
static unsigned g_vm_probe_calls[SMP_MAX_CPUS];

static void vm_exit_now_and_then(int cpu) {
    if (g_vm_exits < 0) {
        uint32_t a, b, c, d;
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
        g_vm_exits = (c >> 31) & 1;              /* (a hypervisor) */
    }
    uint64_t per = timer_counter_per_ms();
    if (!g_vm_exits || !per || cpu <= 0 || cpu >= SMP_MAX_CPUS) return;
    /* Reading HPET/PM itself exits the VM. Avoid doing that on every
     * cheap syscall. Raw TSC is only a sampling hint here, never a clock
     * or deadline; backwards/jumping TSCs force an immediate resample,
     * and a call-count bound also resamples a frozen counter. */
    uint32_t lo,hi;
    __asm__ volatile("rdtsc":"=a"(lo),"=d"(hi));
    uint64_t stamp=(uint64_t)hi<<32 | lo;
    if(stamp-g_vm_probe_at[cpu]<100000 && ++g_vm_probe_calls[cpu]<32)return;
    g_vm_probe_at[cpu]=stamp;g_vm_probe_calls[cpu]=0;
    uint64_t now = timer_counter();
    if (now - g_vm_exit_at[cpu] < per) return;
    g_vm_exit_at[cpu] = now;
    __asm__ volatile("outb %%al, $0x80" : : "a"(0));
}

/* kernel/isr64.asm: int 0x80 (or syscall) from an app, on its thread's kernel stack */
void app_syscall(sysframe_t* r) {
    /* the kernel runs on the boot core: a call made elsewhere moves there,
     * unless it may run where it was made (interrupts are still off, as the
     * gate left them - and stay off for those) */
    if (cpu_id() != 0) {
        vm_exit_now_and_then(cpu_id());
        if ((r->cs & 3) == 3 && syscall_here(r)) return;
        task_ap_go_home();
    }
    __asm__ volatile("sti");
    app_proc_t* p = cur();
    uint64_t slot = r->rax;
    if (!p || (r->cs & 3) != 3) {               /* (only apps get here: a kernel bug otherwise) */
        r->rax = (uint64_t)-1;
        __asm__ volatile("cli");
        return;
    }
    web_check_budget(p);
    if (p->web_restricted && (slot != SYS_RETURN && !web_syscall_allowed(slot))) {
        r->rax = (uint64_t)-1;
        __asm__ volatile("cli");
        return;
    }
    if (p->kill_req) leave(p, 137);
    if (slot == SYS_RETURN) {                   /* returned: what exit(ret) / thread_exit(ret) do */
        athread_t* t = cur_thread(p);
        if (t) leave_thread(p, t, (int)r->rdi);
        leave(p, (int)r->rdi);
    }
    if (slot == SYS_SIGRETURN) {                /* a signal handler returned */
        sig_return(p, r);
        r->int_no = 0x81;                       /* back by iretq: rcx / r11 too (sysret would not) */
        sig_check(p, r);
        __asm__ volatile("cli");
        return;
    }
    if (slot >= API_SLOTS || !g_spec[slot].args) bad_call(p, slot, "an unknown system call");
    int me = task_current_pid();
    int mine = me >= 0 && me < TASK_MAX;
    if (mine) g_sysframe[me] = r;                   /* (fork copies it) */
    uint64_t a[SYS_ARGS];
    if (!gather_args(p, &g_spec[slot], r, a)) bad_call(p, slot, "it passed memory that is not its own to the system");
    if(p->web_restricted && slot==API_SLOT(localtime) &&
       !uvm_user_writable(p->us,p->mem,(uintptr_t)a[0],sizeof(banana_time_t))){
        r->rax=(uint64_t)-1;
        __asm__ volatile("cli");
        return;
    }
    sysfn_t fn = (sysfn_t)((void* const*)&g_api)[slot];
    if (mine) { g_insys[me] = (uint16_t)(slot + 1); g_lastip[me] = r->rip; }     /* (ps: what it is in, where) */
    r->rax = fn(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11]);
    if (mine) g_insys[me] = 0;
    sig_check(p, r);                            /* signals sent to it go to their handlers now */
    __asm__ volatile("cli");
}

/* the call table an app gets, in its memory: the system's, every function
 * a stub; the font and the strings copied; the return stub after them -
 * built here and copied into its mmap region (with the rest of its memory,
 * a fork copies it: the same address in the child) */
static int make_uapi(app_proc_t* p) {
    size_t tab = (sizeof(banana_api_t) + 15) & ~(size_t)15;
    size_t stubs = API_SLOTS * STUB + 2 * STUB;     /* (+ the return and sigreturn stubs) */
    size_t total = tab + stubs + sizeof(font8x8_basic) + 64;
    uint8_t* m = (uint8_t*)kzalloc(total);
    uintptr_t at = m ? uvm_map(p->us, p->mem, 0, total, UVM_READ | UVM_WRITE | UVM_EXEC, 0) : 0;
    if (!at) { kfree(m); return -1; }
#define RT(x) (at + (uintptr_t)((const uint8_t*)(x) - m))     /* where it is in the app */
    banana_api_t* ua = (banana_api_t*)m;
    memcpy(ua, &g_api, sizeof(g_api));
    void** slot = (void**)ua;
    uint8_t* st = m + tab;
    int sc = idt_has_syscall();
    for (uint32_t k = 0; k < API_SLOTS; k++, st += STUB) {
        memset(st, 0xCC, STUB);                         /* (int3 in the gaps) */
        if (sc) {
            /* mov r10, rcx; mov eax, k; syscall; ret - syscall keeps the
             * return address in rcx, so the 4th argument goes in r10 */
            st[0] = 0x49; st[1] = 0x89; st[2] = 0xCA;
            st[3] = 0xB8; memcpy(st + 4, &k, 4);
            st[8] = 0x0F; st[9] = 0x05;
            st[10] = 0xC3;
        } else {
            st[0] = 0xB8;                               /* mov eax, k */
            memcpy(st + 1, &k, 4);
            st[5] = 0xCD; st[6] = SYSCALL_VECTOR;       /* int 0x80 */
            st[7] = 0xC3;                               /* ret */
        }
        if (g_spec[k].args) slot[k] = (void*)RT(st);
    }
    p->ret_stub = RT(m + tab) + SYS_RETURN * STUB;
    /* A C function returns its int in eax. Preserve it as the syscall's
     * first argument BEFORE loading the syscall number into eax. Ordinary
     * call stubs leave rdi alone; it is not the function's return value. */
    memset(st, 0xCC, 2 * STUB);
    st[0] = 0x89; st[1] = 0xC7;                         /* mov edi, eax */
    st[2] = 0xB8;                                      /* mov eax, SYS_RETURN */
    uint32_t ret_slot = SYS_RETURN;
    memcpy(st + 3, &ret_slot, 4);
    if (sc) { st[7] = 0x0F; st[8] = 0x05; }            /* syscall */
    else { st[7] = 0xCD; st[8] = SYSCALL_VECTOR; }      /* int 0x80 */
    st[9] = 0x0F; st[10] = 0x0B;                        /* ud2: must not return */
    /* where a signal handler returns to (sig_deliver): the frame is under rsp
     * (rcx and r11, which syscall overwrites, come back from it: iretq) */
    st += STUB;
    st[0] = 0xB8;                                       /* mov eax, SYS_SIGRETURN */
    uint32_t sr_slot = SYS_SIGRETURN;
    memcpy(st + 1, &sr_slot, 4);
    if (sc) { st[5] = 0x0F; st[6] = 0x05; }
    else { st[5] = 0xCD; st[6] = SYSCALL_VECTOR; }
    st[7] = 0x0F; st[8] = 0x0B;                         /* ud2: does not return here */
    p->sigret_stub = RT(st);
    uint8_t* font = m + tab + stubs;
    memcpy(font, font8x8_basic, sizeof(font8x8_basic));
    ua->font8x8 = (__typeof__(ua->font8x8))RT(font);
    char* str = (char*)font + sizeof(font8x8_basic);
    kstrlcpy(str, g_api.arch, 16);
    ua->arch = (const char*)RT(str);
    kstrlcpy(str + 16, g_api.os_version, 16);
    ua->os_version = (const char*)RT(str + 16);
#undef RT
    int rc = uvm_write(p->us, at, m, total);
    kfree(m);
    if (rc) return -1;
    p->uapi = (banana_api_t*)at;
    return 0;
}
#endif

/* ── Unix processes (version 26) ───────────────────────────────────────
 * fork, execve, waitpid, process groups and sessions, kill. A process is an
 * app slot: its pid is seq. fork (64-bit) gives the child a new address
 * space sharing every page copy-on-write (kernel/uspace.c), the same
 * descriptors, a task of its own that goes back to ring 3 from the same
 * system call with 0. execve loads the new program into a new space while
 * the old one runs the call; run() then frees the old one and starts it.
 * A process that ends while its parent lives stays as a zombie - its exit
 * status - until the parent waits for it. */

#define WNOHANG_F 1


static void teardown(app_proc_t* p);
static void stop_threads(app_proc_t* p);
#ifdef __x86_64__
static int  load_program(app_proc_t* p, const uint8_t* f, uint32_t size, int argc, char** argv,
                         char** envp, char* err, int ecap);
static uint64_t elf_span(const uint8_t* f, uint32_t size);
#endif

static app_proc_t* proc_by_pid(int pid) {
    for (int i = 0; i < APP_MAX; i++)
        if (g_procs[i].used && !g_procs[i].zombie && g_procs[i].seq == pid) return &g_procs[i];
    return NULL;
}

/* every task of p looks again (a wait, a sleep it is in) */
static void proc_wake(app_proc_t* p) {
    if (p->pid >= 0) task_wake(p->pid);
    for (int t = 0; t < APP_THREADS; t++)
        if (p->threads[t].used && !p->threads[t].done && p->threads[t].pid >= 0) task_wake(p->threads[t].pid);
}

static void proc_signal(app_proc_t* t, int sig) {
    if (sig < 1 || sig > 64) return;
    if (sig == 18) {                                    /* SIGCONT: it goes on, whatever it does with the signal */
        if (t->stopped) {
            t->stopped = 0;
            t->cont_report = 1;
            app_proc_t* parent = t->ppid ? proc_by_pid(t->ppid) : NULL;
            if (parent) { SIG_OR(parent->sig_pending, 1ull << 16); proc_wake(parent); }
        }
        SIG_AND(t->sig_pending, ~(0xFull << 18));             /* (stops sent before: dropped) */
    } else if (sig >= 19 && sig <= 22) {
        SIG_AND(t->sig_pending, ~(1ull << 17));               /* (a SIGCONT not taken yet: dropped) */
    }
#ifdef __x86_64__
    SIG_OR(t->sig_pending, 1ull << (sig - 1));
#else
    if (sig == 9 || sig == 15 || sig == 2) t->kill_req = 1;   /* (no signals here: the ones that end it) */
#endif
    proc_wake(t);
}

/* the last processes that ended, with their exit codes (128 + a signal):
 * one with no parent to wait for it (an SSH session's bash) */
typedef struct { int pid, code; } ended_t;
static ended_t g_ended[16];
static int g_ended_at;

int app_exit_code(int pid) {
    for (int i = 0; i < 16; i++) if (g_ended[i].pid == pid && pid) return g_ended[i].code;
    return -1;
}

/* the process ended: what it held goes back; with a parent that may wait
 * for it, it stays as a zombie (its status) and the parent gets SIGCHLD */
static void proc_finish(app_proc_t* p) {
    int sig = p->term_sig;
    pty_t* tty = p->ctty;
    if (tty && p->sid == p->seq && pty_session(tty) == p->sid) {   /* the session's leader: it loses the terminal */
        pgrp_signal(pty_fg(tty), 1);
        pgrp_signal(pty_fg(tty), 18);
        pty_set_session(tty, 0);
        pty_set_fg(tty, 0);
        for (int i = 0; i < APP_MAX; i++)
            if (g_procs[i].used && &g_procs[i] != p && g_procs[i].sid == p->sid && g_procs[i].ctty == tty)
                ctty_set(&g_procs[i], NULL);
    }
    ctty_set(p, NULL);
    p->stopped = 0;
    p->wstatus = sig ? (sig & 0x7F) : ((p->exit_code & 0xFF) << 8);
    g_ended[g_ended_at] = (ended_t){ p->seq, sig ? 128 + sig : p->exit_code & 0xFF };
    g_ended_at = (g_ended_at + 1) % 16;
    app_proc_t* parent = p->ppid ? proc_by_pid(p->ppid) : NULL;
    for (int i = 0; i < APP_MAX; i++) {                 /* its children: nobody's now */
        app_proc_t* c = &g_procs[i];
        if (!c->used || c == p || c->ppid != p->seq) continue;
        if (c->zombie) { c->zombie = 0; c->used = 0; }
        else c->ppid = 0;
    }
    teardown(p);
    if (parent && parent->sig_disp[17] != BANANA_SIG_IGNORE) {
        p->zombie = 1;                                  /* (used stays: the slot is its status) */
        proc_signal(parent, 17);                        /* SIGCHLD - and a waitpid wakes */
    } else {
        p->used = 0;
    }
}

#ifdef __x86_64__
/* the registers of the system call a fork's child goes on from (on top of
 * its kernel stack), then ring 3 - until it ends (app_leave) */
int app_resume_user(void* frame, uintptr_t* saved_sp);
static int resume_user(app_proc_t* p) {
    sysframe_t* fr = (sysframe_t*)((kstack_top(p->kstack) - sizeof(sysframe_t)) & ~(uintptr_t)15);
    memcpy(fr, p->fork_frame, sizeof(sysframe_t));
    kfree(p->fork_frame);
    p->fork_frame = NULL;
    task_set_user(uspace_cr3(p->us), uspace_tag(p->us), kstack_top(p->kstack));
    task_set_fs(p->fork_fs);
    int rc = app_resume_user(fr, &p->saved_sp);
    task_set_user(0, NULL, 0);
    task_set_fs(0);
    return rc;
}

/* execve done: the old program's space goes (the kernel's tables are loaded) */
static void exec_switch(app_proc_t* p) {
    p->exec_next = 0;
    gpu_virgl_release(p->id + 1);
    gem_release(p->id + 1);
    appwin_close_owner(p->id);              /* (their pictures were the old program's memory) */
    webview_close_owner(p->id);
    umem_destroy(p->old_mem);
    uspace_destroy(p->old_us);
    p->old_mem = NULL;
    p->old_us = NULL;
}
#endif

static int run(app_proc_t* p);

#ifdef __x86_64__
static void fork_entry(void) {
    app_proc_t* p = cur();
    if (!p) return;
    if (p->has_term) terminal_vt_set_active(p->vt);
    else task_set_background();
    run(p);
    proc_finish(p);
}
#endif

static int a_proc_fork(void) {
#ifdef __x86_64__
    app_proc_t* p = cur();
    int me = task_current_pid();
    sysframe_t* r = me >= 0 && me < TASK_MAX ? (sysframe_t*)g_sysframe[me] : NULL;
    if (!p || !r || !p->us) return -38;
    app_proc_t* c = NULL;
    for (int i = 0; i < APP_MAX; i++) if (!g_procs[i].used) { c = &g_procs[i]; c->id = i; break; }
    if (!c) return -11;                                  /* EAGAIN: too many processes */
    int id = c->id;
    memset(c, 0, sizeof(*c));
    c->id = id;
    c->used = 1;
    c->zombie = 1;                                      /* (being made: nobody's yet - waits, lists skip it) */
    c->pid = -1;
    c->seq = ++g_seq;
    kstrlcpy(c->name, p->name, sizeof(c->name));
    c->entry = p->entry; c->base = p->base; c->code_lo = p->code_lo; c->code_hi = p->code_hi;
    c->stack = p->stack; c->guard = p->guard; c->guarded = 1;
    c->argc = p->argc; c->argv = p->argv;
    c->uapi = p->uapi; c->ret_stub = p->ret_stub; c->sigret_stub = p->sigret_stub;
    c->vt = p->vt; c->has_term = p->has_term; c->handles_int = p->handles_int;
    c->own_task = 1;
    memcpy(c->sig_disp, p->sig_disp, sizeof(c->sig_disp));
    c->sig_onstack = p->sig_onstack;
    c->sig_entry = p->sig_entry;
    sigstate_t* st = sig_state(p);
    c->msig.blocked = st->blocked;
    c->msig.alt_sp = st->alt_sp; c->msig.alt_size = st->alt_size; c->msig.alt_flags = st->alt_flags;
    int rc = -12;
    c->us = uspace_create();
    c->mem = c->us ? umem_create(c->us) : NULL;
    c->kstack = kstack_new();
    c->fork_frame = kmalloc(sizeof(sysframe_t));
    if (c->us && c->mem && c->kstack && c->fork_frame) {
        for (int tries = 0; tries < 100000; tries++) {
            uintptr_t zsw = 0;
            rc = uvm_fork(p->us, c->us, &zsw);
            if (rc != -2) break;
            if (!app_vm_fault(zsw, 0, 1, 1)) { rc = -12; break; }    /* (compressed away: back in, then again) */
        }
    }
    int pid = rc == 0 ? task_create(c->name, fork_entry) : -1;
    if (pid < 0) {
        teardown(c);
        kfree(c->fork_frame);
        c->fork_frame = NULL;
        c->used = 0;
        c->zombie = 0;
        return rc ? -12 : -11;
    }
    for (int i = 0; i < APP_FDS; i++) {
        if (!p->fdt[i]) continue;
        c->fdt[i] = kfile_ref(p->fdt[i]);
        c->cloexec[i] = p->cloexec[i];
    }
    memcpy(c->fork_frame, r, sizeof(sysframe_t));
    ((sysframe_t*)c->fork_frame)->rax = 0;             /* fork() is 0 in the child */
    ((sysframe_t*)c->fork_frame)->int_no = 0x80;
    c->fork_fs = task_get_fs();
    c->ppid = p->seq;
    c->alarm_at = c->alarm_every = 0;                  /* (a child's timer starts disarmed) */
    c->start_ms = timer_ms();
    c->pgid = p->pgid;
    c->sid = p->sid;
    if (p->ctty) ctty_set(c, p->ctty);
    c->last_breath = timer_ms();
    c->pid = pid;
    pid_set(pid, c->id, -1);
    c->zombie = 0;                                      /* it runs once we yield */
    return c->seq;
#else
    return -38;                                          /* ENOSYS: the 32-bit kernel has no address spaces */
#endif
}

#ifdef __x86_64__
/* a NULL-ended array of strings from the app (argv, envp) into kernel memory:
 * *out (one block: the pointers then the strings), *n; 0 or -errno */
static int copy_strv(char* const* v, char*** out, int* n) {
    *out = NULL;
    *n = 0;
    int count = 0;
    uint64_t bytes = 0;
    for (; v; count++) {
        if (count > 65536 || !uok(&v[count], sizeof(char*))) return -14;
        const char* s = v[count];
        if (!s) break;
        if (!ustr(s)) return -14;
        bytes += strlen(s) + 1;
        if (bytes > (2u << 20)) return -7;              /* E2BIG */
    }
    char** blk = (char**)kmalloc((size_t)(count + 1) * sizeof(char*) + (size_t)bytes + 1);
    if (!blk) return -12;
    char* s = (char*)(blk + count + 1);
    for (int i = 0; i < count; i++) {
        size_t l = strlen(v[i]) + 1;
        memcpy(s, v[i], l);
        blk[i] = s;
        s += l;
    }
    blk[count] = NULL;
    *out = blk;
    *n = count;
    return 0;
}
#endif

/* Keep the executable's bytes resident before any address-space or heap
 * allocation can reclaim the disk cache. fs_get_file returns an unloaded,
 * zero-filled entry after an I/O error: that is EIO, not a shell script. */
static fs_file_t* pin_program(int fi) {
    fs_pin(fi);
    fs_file_t* f = fi >= 0 ? fs_get_file(fi) : NULL;
    if (!f || !f->used || !f->loaded || !f->content) {
        fs_unpin(fi);
        return NULL;
    }
    return f;
}

static int a_proc_exec(const char* path, char* const* argv, char* const* envp) {
#ifdef __x86_64__
    app_proc_t* p = cur();
    if (!p || !path) return -14;
    if (cur_thread(p)) return -38;                      /* (from its main thread only, for now) */
    char** kargv = NULL;
    char** kenv = NULL;
    int argc = 0, envc = 0, rc = copy_strv(argv, &kargv, &argc);
    if (!rc) rc = copy_strv(envp, &kenv, &envc);
    char file[256];
    kstrlcpy(file, path, sizeof(file));
    /* #!: the interpreter runs, given the script (up to 4 levels) */
    for (int depth = 0; !rc; depth++) {
        int fi = fs_find_file(file);
        if (fi < 0) { rc = fs_find_dir(file) >= 0 ? -13 : -2; break; }       /* EACCES, ENOENT */
        fs_file_t* f = fs_get_file(fi);
        if (!f || !f->loaded || !f->content) { rc = -5; break; }
        const char* c = (const char*)f->content;
        if (f->size < 2 || c[0] != '#' || c[1] != '!') break;
        if (depth == 4) { rc = -40; break; }            /* ELOOP */
        char line[256];
        uint32_t k = 2, n = 0;
        while (k < f->size && (c[k] == ' ' || c[k] == '\t')) k++;
        while (k < f->size && c[k] != '\n' && n < sizeof(line) - 1) line[n++] = c[k++];
        line[n] = 0;
        while (n && (line[n - 1] == ' ' || line[n - 1] == '\t' || line[n - 1] == '\r')) line[--n] = 0;
        char* arg = line;
        while (*arg && *arg != ' ' && *arg != '\t') arg++;
        if (*arg) { *arg++ = 0; while (*arg == ' ' || *arg == '\t') arg++; }
        if (!line[0]) { rc = -8; break; }               /* ENOEXEC */
        /* argv: interpreter [arg] script argv[1..] */
        char* nv[4];
        int nn = 0;
        nv[nn++] = line;
        if (*arg) nv[nn++] = arg;
        nv[nn++] = file;
        int newc = nn + (argc > 1 ? argc - 1 : 0);
        uint64_t bytes = 0;
        for (int i = 0; i < nn; i++) bytes += strlen(nv[i]) + 1;
        for (int i = 1; i < argc; i++) bytes += strlen(kargv[i]) + 1;
        char** blk = (char**)kmalloc((size_t)(newc + 1) * sizeof(char*) + (size_t)bytes);
        if (!blk) { rc = -12; break; }
        char* s = (char*)(blk + newc + 1);
        for (int i = 0; i < newc; i++) {
            const char* src = i < nn ? nv[i] : kargv[i - nn + 1];
            size_t l = strlen(src) + 1;
            memcpy(s, src, l);
            blk[i] = s;
            s += l;
        }
        blk[newc] = NULL;
        kfree(kargv);
        kargv = blk;
        argc = newc;
        kstrlcpy(file, kargv[0], sizeof(file));
    }
    if (rc) { kfree(kargv); kfree(kenv); return rc; }
    int fi = fs_find_file(file);
    fs_file_t* f = pin_program(fi);
    if (!f) { kfree(kargv); kfree(kenv); return fi < 0 ? -2 : -5; }
    if (elf_span((const uint8_t*)f->content, f->size) == 0) {
        fs_unpin(fi);
        kfree(kargv); kfree(kenv); return -8;
    }
    /* the new program into a new space, the old one kept until it is loaded */
    struct {
        uspace_t* us; umem_t* mem; banana_entry_t entry; uintptr_t base, code_lo, code_hi;
        uint8_t* stack; uint8_t* guard; char** argv; int argc;
        banana_api_t* uapi; uintptr_t ret_stub, sigret_stub;
    } old = { p->us, p->mem, p->entry, p->base, p->code_lo, p->code_hi, p->stack, p->guard, p->argv, p->argc,
              p->uapi, p->ret_stub, p->sigret_stub };
    p->us = uspace_create();
    p->mem = p->us ? umem_create(p->us) : NULL;
    p->stack = NULL;
    char err[96];
    int load_rc = !p->mem ? -12 : load_program(p, (const uint8_t*)f->content, f->size, argc, kargv, kenv, err, sizeof(err));
    fs_unpin(fi);
    kfree(kenv);
    if (load_rc) {
        umem_destroy(p->mem);
        uspace_destroy(p->us);
        p->us = old.us; p->mem = old.mem; p->entry = old.entry; p->base = old.base;
        p->code_lo = old.code_lo; p->code_hi = old.code_hi; p->stack = old.stack; p->guard = old.guard;
        p->argv = old.argv; p->argc = old.argc; p->uapi = old.uapi; p->ret_stub = old.ret_stub;
        p->sigret_stub = old.sigret_stub;
        kfree(kargv);
        return load_rc;
    }
    /* what a new program does not keep: the other threads, descriptors
     * marked close-on-exec, signal handlers (back to the default), its
     * alternate signal stack, the clipboard's copy */
    uspace_t* nus = p->us;                              /* (their stacks are in the old space) */
    umem_t* nmem = p->mem;
    p->us = old.us;
    p->mem = old.mem;
    stop_threads(p);
    p->exit_req = 0;
    p->us = nus;
    p->mem = nmem;
    for (int i = 0; i < APP_FDS; i++) if (p->fdt[i] && p->cloexec[i]) fd_close(p, i);
    for (int s = 1; s <= 64; s++) if (p->sig_disp[s] == BANANA_SIG_CATCH) p->sig_disp[s] = BANANA_SIG_DEFAULT;
    p->sig_entry = 0;
    p->sig_onstack = 0;
    p->msig.alt_sp = 0; p->msig.alt_size = 0; p->msig.alt_flags = 0;
    p->clip = NULL;
    const char* base = strrchr(argc ? kargv[0] : file, '/');
    kstrlcpy(p->name, base ? base + 1 : (argc ? kargv[0] : file), sizeof(p->name));
    kfree(kargv);
    p->old_us = old.us;
    p->old_mem = old.mem;
    p->exec_next = 1;
    leave(p, 0);                                        /* (run() starts the new program) */
#else
    (void)path; (void)argv; (void)envp;
    return -38;
#endif
}

static int a_proc_wait(int pid, int* status, int options, void* ru) {
    app_proc_t* p = cur();
    if (!p) return -10;
    if (ru) memset(ru, 0, 144);                          /* (struct rusage: not kept) */
    for (;;) {
        int found = 0;
        for (int i = 0; i < APP_MAX; i++) {
            app_proc_t* c = &g_procs[i];
            if (!c->used || c == p || c->ppid != p->seq || c->pid == -1) continue;
            if (!(pid == -1 || (pid > 0 && c->seq == pid) || (pid == 0 && c->pgid == p->pgid) ||
                  (pid < -1 && c->pgid == -pid))) continue;
            found = 1;
            if ((options & 2) && c->stopped && !c->stop_reported && !c->zombie) {     /* WUNTRACED */
                c->stop_reported = 1;
                if (status) *status = 0x7F | (c->stopped << 8);
                return c->seq;
            }
            if ((options & 8) && c->cont_report && !c->zombie) {                       /* WCONTINUED */
                c->cont_report = 0;
                if (status) *status = 0xFFFF;
                return c->seq;
            }
            if (c->zombie) {
                int st = c->wstatus, id = c->seq;
                c->zombie = 0;
                c->used = 0;
                if (status) *status = st;
                return id;
            }
        }
        if (!found) return -10;                         /* ECHILD */
        if (options & WNOHANG_F) return 0;
        if (sig_waiting(p)) return -4;                  /* EINTR: a handler runs first */
        p->waiting = 1;
        task_sleep_ms(50);                              /* (a child that ends wakes it) */
        breathe(p, 1);
        p->waiting = 0;
    }
}

static int a_proc_ids(int pid, int* out) {
    app_proc_t* p = cur();
    app_proc_t* t = pid ? proc_by_pid(pid) : p;
    if (!t) return -3;                                  /* ESRCH */
    out[0] = t->seq; out[1] = t->ppid; out[2] = t->pgid; out[3] = t->sid;
    return 0;
}

static int a_proc_setpgid(int pid, int pgid) {
    app_proc_t* p = cur();
    if (!p || pgid < 0) return -22;
    app_proc_t* t = pid ? proc_by_pid(pid) : p;
    if (!t || (t != p && t->ppid != p->seq)) return -3;
    if (t->sid != p->sid || t->seq == t->sid) return -1;           /* EPERM: another session, or its leader */
    if (!pgid) pgid = t->seq;
    if (pgid != t->seq) {                               /* joining a group: one of its session's */
        int ok = 0;
        for (int i = 0; i < APP_MAX && !ok; i++)
            ok = g_procs[i].used && !g_procs[i].zombie && g_procs[i].pgid == pgid && g_procs[i].sid == p->sid;
        if (!ok) return -1;
    }
    t->pgid = pgid;
    return 0;
}

static int a_proc_setsid(void) {
    app_proc_t* p = cur();
    if (!p) return -1;
    for (int i = 0; i < APP_MAX; i++)                   /* a group's leader may not */
        if (g_procs[i].used && !g_procs[i].zombie && g_procs[i].pgid == p->seq) return -1;
    p->sid = p->pgid = p->seq;
    ctty_set(p, NULL);                                  /* (a new session has no terminal yet) */
    return p->seq;
}

static int a_proc_kill(int pid, int sig) {
    app_proc_t* p = cur();
    if (!p || sig < 0 || sig > 64) return -22;
    int n = 0;
    for (int i = 0; i < APP_MAX; i++) {
        app_proc_t* t = &g_procs[i];
        if (!t->used || t->zombie || t->pid < 0) continue;
        if (!(pid > 0 ? t->seq == pid : pid == 0 ? t->pgid == p->pgid : pid == -1 ? 1 : t->pgid == -pid)) continue;
        n++;
        if (sig) proc_signal(t, sig);
    }
    return n ? 0 : -3;                                  /* ESRCH */
}

/* ── terminals: /dev, ptys, the controlling terminal, job control (version 27)
 * /dev/ptmx gives a pseudo-terminal's master (kernel/pty.c), /dev/pts/N its
 * slave; a session leader opening a slave without O_NOCTTY makes it its
 * session's controlling terminal (/dev/tty then), whose foreground process
 * group gets ^C, ^\, ^Z and SIGWINCH, and alone may read it - another
 * group gets SIGTTIN (and stops) when it tries. */

/* every live process of group pgrp gets sig; how many */
static int pgrp_signal(int pgrp, int sig) {
    int n = 0;
    if (pgrp <= 0) return 0;
    for (int i = 0; i < APP_MAX; i++) {
        app_proc_t* t = &g_procs[i];
        if (!t->used || t->zombie || t->pid < 0 || t->pgid != pgrp) continue;
        proc_signal(t, sig);
        n++;
    }
    return n;
}

/* the controlling terminal: set (a hold kept on it), or let go */
static void ctty_set(app_proc_t* p, pty_t* t) {
    if (p->ctty == t) return;
    if (t) pty_hold(t, 1);
    pty_t* old = p->ctty;
    p->ctty = t;
    if (old) pty_hold(old, -1);
}

/* what a pty's line discipline raised: signals for its foreground group;
 * its master closed: SIGHUP and SIGCONT to it and its session's leader,
 * and the session loses its terminal */
static void pty_dispatch(pty_t* t) {
    int ev = pty_events(t);
    int fg = pty_fg(t);
    if (ev & PTY_EV_SIGINT) pgrp_signal(fg, 2);
    if (ev & PTY_EV_SIGQUIT) pgrp_signal(fg, 3);
    if (ev & PTY_EV_SIGTSTP) pgrp_signal(fg, 20);
    if (ev & PTY_EV_SIGWINCH) pgrp_signal(fg, 28);
    if (ev & PTY_EV_HANGUP) {
        int sid = pty_session(t);
        pgrp_signal(fg, 1);
        pgrp_signal(fg, 18);
        app_proc_t* leader = sid ? proc_by_pid(sid) : NULL;
        if (leader && leader->pgid != fg) { proc_signal(leader, 1); proc_signal(leader, 18); }
        pty_set_session(t, 0);
        pty_set_fg(t, 0);
        /* (last: the holds let go may free it) */
        for (int i = 0; i < APP_MAX; i++)
            if (g_procs[i].used && g_procs[i].ctty == t) ctty_set(&g_procs[i], NULL);
    }
}

static void pty_closed_hook(struct pty* t) { pty_dispatch(t); }

/* a signal the process can be stopped / interrupted by: not ignored nor blocked */
static int sig_takes(app_proc_t* p, int sig) {
#ifdef __x86_64__
    if (p->sig_disp[sig] == BANANA_SIG_IGNORE) return 0;
    if ((sig_state(p)->blocked >> (sig - 1)) & 1) return 0;
    return 1;
#else
    (void)p; (void)sig;
    return 0;
#endif
}

/* a background process of the terminal's session touching it (reading, or
 * writing with TOSTOP, changing its modes): its group gets sig (SIGTTIN /
 * SIGTTOU) - -4 (EINTR: it stops now); -5 (EIO) when it would never be let
 * in (the signal ignored or blocked: for SIGTTOU that means "go ahead": 0) */
static int bg_check(app_proc_t* p, pty_t* t, int sig) {
    if (!p || p->ctty != t || !pty_fg(t) || p->pgid == pty_fg(t)) return 0;
    if (!sig_takes(p, sig)) return sig == 22 ? 0 : -5;
    pgrp_signal(p->pgid, sig);
    return -4;
}

#define TOSTOP_F 0000400

/* the slave: a read (canonical: a line; raw: VMIN / VTIME) */
static long pts_read(app_proc_t* p, kfile_t* f, void* buf, unsigned long len) {
    pty_t* t = f->pty;
    int r = bg_check(p, t, 21);
    if (r) return r;
    if (!len) return 0;
    int canon, vmin, vtime;
    pty_read_mode(t, &canon, &vmin, &vtime);
    uint32_t start = timer_ms();
    for (;;) {
        long n = pty_slave_read(t, (uint8_t*)buf, len);
        if (n != -11) return n;
        if (f->flags & KF_NONBLOCK) return -11;
        if (!canon && vmin == 0 && (vtime == 0 || timer_ms() - start >= (uint32_t)vtime * 100u)) return 0;
        if (sig_waiting(p)) return -4;
        kpipe_wait(NULL, 0, 20);
        breathe(p, 0);
    }
}

static long pts_write(app_proc_t* p, kfile_t* f, const void* buf, unsigned long len) {
    pty_t* t = f->pty;
    pty_termios_t tio;
    pty_get_termios(t, &tio);
    if (tio.c_lflag & TOSTOP_F) {
        int r = bg_check(p, t, 22);
        if (r) return r;
    }
    unsigned long done = 0;
    while (done < len) {
        long n = pty_slave_write(t, (const uint8_t*)buf + done, len - done);
        if (n > 0) { done += (unsigned long)n; continue; }
        if (n != -11 || (f->flags & KF_NONBLOCK)) return done ? (long)done : n;
        if (sig_waiting(p)) return done ? (long)done : -4;
        kpipe_wait(NULL, 0, 20);
        breathe(p, 0);
    }
    return (long)done;
}

static long ptm_read(app_proc_t* p, kfile_t* f, void* buf, unsigned long len) {
    for (;;) {
        long n = pty_master_read(f->pty, (uint8_t*)buf, len);
        if (n != -11 || (f->flags & KF_NONBLOCK)) return n;
        if (sig_waiting(p)) return -4;
        kpipe_wait(NULL, 0, 20);
        breathe(p, 0);
    }
}

static long ptm_write(app_proc_t* p, kfile_t* f, const void* buf, unsigned long len) {
    unsigned long done = 0;
    while (done < len) {
        long n = pty_master_write(f->pty, (const uint8_t*)buf + done, len - done);
        pty_dispatch(f->pty);                            /* (^C typed: SIGINT now) */
        if (n > 0) { done += (unsigned long)n; continue; }
        if (n != -11 || (f->flags & KF_NONBLOCK)) return done ? (long)done : n;
        if (sig_waiting(p)) return done ? (long)done : -4;
        kpipe_wait(NULL, 0, 20);
        breathe(p, 0);
    }
    return (long)done;
}

static long dev_read(kfile_t* f, void* buf, unsigned long len) {
    switch (f->dev) {
    case KF_DEV_NULL: return 0;
    case KF_DEV_ZERO: case KF_DEV_FULL: memset(buf, 0, len); return (long)len;
    case KF_DEV_RANDOM: random_bytes(buf, len > 0x7FFFFFFFul ? 0x7FFFFFFFu : (uint32_t)len); return (long)len;
    }
    return -5;
}

static long dev_write(kfile_t* f, unsigned long len) {
    return f->dev == KF_DEV_FULL ? -28 : (long)len;       /* (/dev/full: ENOSPC) */
}

/* a pty slave description for t; a session leader with no terminal (and
 * no O_NOCTTY) takes it as its controlling terminal */
static int pts_open(app_proc_t* p, pty_t* t, int flags) {
    kfile_t* f = kfile_new(KF_PTS, KF_READ | KF_WRITE);
    if (!f) return -12;
    f->pty = t;
    pty_slave_ref(t, 1);
    if (!(flags & BANANA_O_NOCTTY) && p->sid == p->seq && !p->ctty && !pty_session(t)) {
        ctty_set(p, t);
        pty_set_session(t, p->sid);
        pty_set_fg(t, p->pgid);
    }
    return fd_install(p, f, 0, 0);
}

/* open() of a path under /dev/: the devices; -errno */
static int dev_open(app_proc_t* p, const char* path, int flags) {
    const char* n = path + 5;
    static const struct { const char* name; int dev; } devs[] = {
        { "null", KF_DEV_NULL }, { "zero", KF_DEV_ZERO }, { "full", KF_DEV_FULL },
        { "random", KF_DEV_RANDOM }, { "urandom", KF_DEV_RANDOM },
    };
    for (unsigned i = 0; i < sizeof(devs) / sizeof(devs[0]); i++) {
        if (strcmp(n, devs[i].name)) continue;
        kfile_t* f = kfile_new(KF_DEV, KF_READ | KF_WRITE);
        if (!f) return -12;
        f->dev = devs[i].dev;
        return fd_install(p, f, 0, 0);
    }
    if (!strcmp(n, "ptmx")) {
        pty_t* t = pty_new();
        if (!t) return -12;
        kfile_t* f = kfile_new(KF_PTM, KF_READ | KF_WRITE);
        if (!f) { pty_master_ref(t, -1); return -12; }
        f->pty = t;
        return fd_install(p, f, 0, 0);
    }
    if (!strncmp(n, "pts/", 4) && n[4] >= '0' && n[4] <= '9') {
        int idx = 0;
        for (const char* c = n + 4; *c; c++) {
            if (*c < '0' || *c > '9' || idx > 1000000) return -2;
            idx = idx * 10 + (*c - '0');
        }
        pty_t* t = pty_find(idx);
        if (!t) return -2;
        if (pty_locked(t)) return -5;                    /* (unlockpt first) */
        return pts_open(p, t, flags | (p->ctty ? BANANA_O_NOCTTY : 0));
    }
#ifdef __x86_64__
    if (!strncmp(n, "tty", 3) && n[3]) {                /* the consoles: tty1..6, tty0, ttyS0 (kernel/vcons.c) */
        pty_t* t = vcons_tty(n);
        if (!t) return -6;                               /* ENXIO */
        return pts_open(p, t, flags | (p->ctty ? BANANA_O_NOCTTY : 0));
    }
#endif
    if (!strcmp(n, "tty")) {
        if (p->ctty) return pts_open(p, p->ctty, flags | BANANA_O_NOCTTY);
        if (!p->has_term) return -6;                     /* ENXIO: no controlling terminal */
    }
    if (!strcmp(n, "tty") || !strcmp(n, "console")) {
        kfile_t* f = kfile_new(KF_CONS, KF_READ | KF_WRITE);
        return f ? fd_install(p, f, 0, 0) : -12;
    }
    int fd = !strcmp(n, "stdin") ? 0 : !strcmp(n, "stdout") ? 1 : !strcmp(n, "stderr") ? 2 : -1;
    if (fd < 0 && !strncmp(n, "fd/", 3) && n[3] >= '0' && n[3] <= '9') {
        fd = 0;
        for (const char* c = n + 3; *c; c++) {
            if (*c < '0' || *c > '9' || fd > 100000) return -2;
            fd = fd * 10 + (*c - '0');
        }
    }
    if (fd >= 0) {                                       /* /dev/fd/N: a new descriptor for N's */
        kfile_t* f = get_fd(p, fd);
        return f ? fd_install(p, kfile_ref(f), 0, 0) : -9;
    }
    return -2;
}

/* ioctl on a terminal (Linux's request numbers; termios: the SDK's struct) */
#define L_TCGETS     0x5401
#define L_TCSETS     0x5402
#define L_TCSETSW    0x5403
#define L_TCSETSF    0x5404
#define L_TCSBRK     0x5409
#define L_TCXONC     0x540A
#define L_TCFLSH     0x540B
#define L_TIOCSCTTY  0x540E
#define L_TIOCGPGRP  0x540F
#define L_TIOCSPGRP  0x5410
#define L_TIOCOUTQ   0x5411
#define L_TIOCGWINSZ 0x5413
#define L_TIOCSWINSZ 0x5414
#define L_FIONREAD   0x541B
#define L_TIOCNOTTY  0x5422
#define L_TIOCGSID   0x5429
#define L_TIOCGPTN   0x80045430u
#define L_TIOCSPTLCK 0x40045431u

static int a_tty_ioctl(int fd, unsigned long req, void* arg) {
    app_proc_t* p = cur();
    kfile_t* f = get_fd(p, fd);
    if (!f) return -9;
    if (f->type == KF_CONS) {                            /* the console: a terminal with fixed modes */
        if (req == L_TIOCGWINSZ) {
            if (!uok(arg, sizeof(pty_winsize_t))) return -14;
            int c = 80, r = 25;
            a_term_size(&c, &r);
            pty_winsize_t* w = (pty_winsize_t*)arg;
            w->ws_row = (uint16_t)r; w->ws_col = (uint16_t)c; w->ws_xpixel = w->ws_ypixel = 0;
            return 0;
        }
        return -25;
    }
    if (f->type != KF_PTM && f->type != KF_PTS) return -25;              /* ENOTTY */
    pty_t* t = f->pty;
    int master = f->type == KF_PTM;
    switch (req) {
    case L_TCGETS:
        if (!uok(arg, sizeof(pty_termios_t))) return -14;
        pty_get_termios(t, (pty_termios_t*)arg);
        return 0;
    case L_TCSETS: case L_TCSETSW: case L_TCSETSF: {
        if (!uok(arg, sizeof(pty_termios_t))) return -14;
        int r = master ? 0 : bg_check(p, t, 22);
        if (r) return r;
        pty_termios_t tio;
        memcpy(&tio, arg, sizeof(tio));
        pty_set_termios(t, &tio, req == L_TCSETSF);
        return 0;
    }
    case L_TIOCGWINSZ:
        if (!uok(arg, sizeof(pty_winsize_t))) return -14;
        pty_get_winsize(t, (pty_winsize_t*)arg);
        return 0;
    case L_TIOCSWINSZ: {
        if (!uok(arg, sizeof(pty_winsize_t))) return -14;
        pty_winsize_t ws;
        memcpy(&ws, arg, sizeof(ws));
        pty_set_winsize(t, &ws);
        pty_dispatch(t);                                 /* (SIGWINCH) */
        return 0;
    }
    case L_FIONREAD: case L_TIOCOUTQ:
        if (!uok(arg, sizeof(int))) return -14;
        *(int*)arg = req == L_FIONREAD ? pty_nread(t, master) : 0;
        return 0;
    case L_TCFLSH:
        if ((uintptr_t)arg > 2) return -22;
        pty_flush(t, (int)(uintptr_t)arg);
        return 0;
    case L_TCSBRK: case L_TCXONC:
        return 0;
    case L_TIOCGPTN:
        if (!master) return -25;
        if (!uok(arg, sizeof(int))) return -14;
        *(int*)arg = pty_index(t);
        return 0;
    case L_TIOCSPTLCK:
        if (!master) return -25;
        if (!uok(arg, sizeof(int))) return -14;
        pty_set_locked(t, *(int*)arg != 0);
        return 0;
    case L_TIOCSCTTY:                                    /* a session leader takes it */
        if (p->sid != p->seq) return -1;
        if (p->ctty == t) return 0;
        if (p->ctty || (pty_session(t) && pty_session(t) != p->sid && !arg)) return -1;
        ctty_set(p, t);
        pty_set_session(t, p->sid);
        pty_set_fg(t, p->pgid);
        return 0;
    case L_TIOCNOTTY:
        if (p->ctty != t) return -25;
        if (p->sid == p->seq) {                          /* the leader: the session loses it */
            pgrp_signal(pty_fg(t), 1);
            pgrp_signal(pty_fg(t), 18);
            pty_set_session(t, 0);
            pty_set_fg(t, 0);
            for (int i = 0; i < APP_MAX; i++)
                if (g_procs[i].used && g_procs[i].sid == p->sid && g_procs[i].ctty == t) ctty_set(&g_procs[i], NULL);
        } else {
            ctty_set(p, NULL);
        }
        return 0;
    case L_TIOCGPGRP: case L_TIOCGSID:
        if (!uok(arg, sizeof(int))) return -14;
        if (!master && p->ctty != t) return -25;
        *(int*)arg = req == L_TIOCGPGRP ? pty_fg(t) : pty_session(t);
        return req == L_TIOCGSID && !pty_session(t) ? -25 : 0;
    case L_TIOCSPGRP: {
        if (!uok(arg, sizeof(int))) return -14;
        if (p->ctty != t || pty_session(t) != p->sid) return -25;
        int pg = *(int*)arg, ok = 0;
        if (pg <= 0) return -22;
        int r = bg_check(p, t, 22);
        if (r) return r;
        for (int i = 0; i < APP_MAX && !ok; i++)
            ok = g_procs[i].used && !g_procs[i].zombie && g_procs[i].pgid == pg && g_procs[i].sid == p->sid;
        if (!ok) return -1;                              /* EPERM: no such group in its session */
        pty_set_fg(t, pg);
        return 0;
    }
    }
    return -22;
}

/* ── job control: stopping and continuing ───────────────────────────── */

#ifdef __x86_64__
/* SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU with their default: the process stops
 * (every thread, at its next way back to its code) until SIGCONT or
 * SIGKILL; its parent learns it (SIGCHLD, waitpid's WUNTRACED) */
static void proc_stop(app_proc_t* p, int sig) {
    if (!p->stopped) {
        p->stopped = sig;
        p->stop_reported = 0;
        p->cont_report = 0;
        app_proc_t* parent = p->ppid ? proc_by_pid(p->ppid) : NULL;
        if (parent) proc_signal(parent, 17);
    }
}

/* the calling thread waits while its process is stopped */
static void stop_wait(app_proc_t* p) {
    uintptr_t fl;
    __asm__ volatile("pushf; pop %0" : "=r"(fl));
    __asm__ volatile("sti");
    while (p->stopped && !p->exit_req && !p->kill_req && !((p->sig_pending >> 8) & 1))
        task_sleep_ms(50);
    if (!(fl & 0x200)) __asm__ volatile("cli");
}
#endif

/* (64-bit: every entry of the app's table is a system call stub instead -
 * app_syscall below; G stays for the 32-bit kernel's direct calls) */
#define G(fn) (fn)

static void api_init(void) {
    if (g_api.magic) return;
    g_api.magic = BANANA_API_MAGIC;
    g_api.version = BANANA_API_VERSION;
    g_api.size = sizeof(banana_api_t);
    g_api.arch = BANANA_ARCH;
    g_api.os_version = "0.6";
    g_api.exit = G(a_exit);
    g_api.write = G(a_write);
    g_api.getchar = G(a_getchar);
    g_api.trygetchar = G(a_trygetchar);
    g_api.readline = G(a_readline);
    g_api.set_color = G(a_set_color);
    g_api.clear_screen = G(a_clear);
    g_api.term_size = G(a_term_size);
    g_api.set_cursor = G(a_set_cursor);
    g_api.malloc = G(a_malloc);
    g_api.free = G(a_free);
    g_api.realloc = G(a_realloc);
    g_api.open = G(a_open);
    g_api.read = G(a_read);
    g_api.seek = G(a_seek);
    g_api.close = G(a_close);
    g_api.remove = G(a_remove);
    g_api.mkdir = G(a_mkdir);
    g_api.rename = G(a_rename);
    g_api.stat = G(a_stat);
    g_api.readdir = G(a_readdir);
    g_api.getcwd = G(a_getcwd);
    g_api.chdir = G(a_chdir);
    g_api.ticks_ms = a_ticks;
    g_api.sleep_ms = G(a_sleep);
    g_api.yield = G(a_yield);
    g_api.localtime = G(a_localtime);
    g_api.random = G(a_random);
    g_api.win_open = G(a_win_open);
    g_api.win_pixels = G(a_win_pixels);
    g_api.win_update = G(a_win_update);
    g_api.win_event = G(a_win_event);
    g_api.win_close = G(a_win_close);
    g_api.win_set_title = G(a_win_set_title);
    g_api.win_size = G(a_win_size);
    g_api.draw_text = a_draw_text;
    g_api.font8x8 = &font8x8_basic[0][0];
    g_api.http_get = G(a_http_get);
    g_api.audio_play = G(a_audio_play);
    g_api.audio_beep = G(a_audio_beep);
    g_api.audio_busy = G(a_audio_busy);
    g_api.clipboard_set = G(a_clip_set);
    g_api.clipboard_get = G(a_clip_get);
    g_api.interrupted = G(a_interrupted);
    g_api.win_set_resizable = G(a_win_set_resizable);
    g_api.thread_create = G(a_thread_create);
    g_api.thread_join = G(a_thread_join);
    g_api.thread_id = G(a_thread_id);
    g_api.thread_exit = G(a_thread_exit);
    g_api.cpu_count = a_cpu_count;
    g_api.gpu_call = a_gpu_call;
    g_api.wait = G(a_wait);
    g_api.wake = G(a_wake);
    g_api.web_open = G(a_web_open);
    g_api.web_close = G(a_web_close);
    g_api.web_load = G(a_web_load);
    g_api.web_load_html = G(a_web_load_html);
    g_api.web_resize = G(a_web_resize);
    g_api.web_poll = G(a_web_poll);
    g_api.web_draw = G(a_web_draw);
    g_api.web_event = G(a_web_event);
    g_api.web_scroll = G(a_web_scroll);
    g_api.web_go = G(a_web_go);
    g_api.web_info = G(a_web_info);
    g_api.web_eval = G(a_web_eval);
    g_api.web_message = G(a_web_message);
    g_api.web_post = G(a_web_post);
    g_api.font_draw = G(a_font_draw);
    g_api.font_width = G(a_font_width);
    g_api.font_metrics = G(a_font_metrics);
    g_api.audio_queued_ms = G(a_audio_queued_ms);
    g_api.audio_stop = G(a_audio_stop);
    g_api.audio_volume = G(a_audio_volume);
    g_api.win_media_keys = G(a_win_media_keys);
    g_api.http_request = G(a_http_request);
    g_api.image_load = G(a_image_load);
    g_api.set_wallpaper = G(a_set_wallpaper);
    g_api.http_fetch = G(a_http_fetch);
    g_api.key_mods = G(a_key_mods);
    g_api.win_wheel = G(a_win_wheel);
    g_api.win_capture_mouse = G(a_win_capture_mouse);
    g_api.win_mouse_captured = G(a_win_mouse_captured);
    g_api.time_utc = G(a_time_utc);
    g_api.utc_offset = G(a_utc_offset);
    g_api.language = G(a_language);
    g_api.drm_ioctl = G(a_drm_ioctl);
    g_api.fd_pipe = G(a_fd_pipe);
    g_api.fd_socketpair = G(a_fd_socketpair);
    g_api.fd_dup = G(a_fd_dup);
    g_api.fd_fcntl = G(a_fd_fcntl);
    g_api.fd_info = G(a_fd_info);
    g_api.proc_fork = G(a_proc_fork);
    g_api.proc_exec = G(a_proc_exec);
    g_api.proc_wait = G(a_proc_wait);
    g_api.proc_ids = G(a_proc_ids);
    g_api.proc_setpgid = G(a_proc_setpgid);
    g_api.proc_setsid = G(a_proc_setsid);
    g_api.proc_kill = G(a_proc_kill);
    g_api.tty_ioctl = G(a_tty_ioctl);
    g_api.fs_ustat = G(a_fs_ustat);
    g_api.fs_fdstat = G(a_fs_fdstat);
    g_api.fs_chmod = G(a_fs_chmod);
    g_api.fs_chown = G(a_fs_chown);
    g_api.fs_symlink = G(a_fs_symlink);
    g_api.fs_readlink = G(a_fs_readlink);
    g_api.fs_utime = G(a_fs_utime);
    g_api.fs_unlink = G(a_fs_unlink);
    g_api.fs_fdattr = G(a_fs_fdattr);
    g_api.fs_fdpath = G(a_fs_fdpath);
    g_api.fs_link = G(a_fs_link);
    g_api.fs_utimes = G(a_fs_utimes);
    g_api.proc_alarm = G(a_proc_alarm);
    g_api.sig_suspend = G(a_sig_suspend);
    g_api.kcmd = G(a_kcmd);
    g_api.poll_mask = G(a_poll_mask);
    g_api.fd_sync = G(a_fd_sync);
    g_api.web_ipc = G(a_web_ipc);
#ifdef __x86_64__
    g_api.proc_list = G(a_proc_list);                  /* (the system calls' table: 64-bit) */
#endif
    kfile_set_pty_hook(pty_closed_hook);
    g_api.pkg_install = G(a_pkg_install);
    g_api.app_run = G(a_app_run);
    g_api.repo_update = G(a_repo_update);
    g_api.repo_list = G(a_repo_list);
    g_api.repo_install = G(a_repo_install);
    g_api.repo_remove = G(a_repo_remove);
    g_api.repo_status = G(a_repo_status);
    g_api.repo_icon = G(a_repo_icon);
    g_api.repo_upgrade = G(a_repo_upgrade);
    g_api.gamepad = G(a_gamepad);
    g_api.win_dropped = G(a_win_dropped);
    g_api.key_down = G(a_key_down);
    g_api.repo_sources = G(a_repo_sources);
    g_api.repo_add_source = G(a_repo_add_source);
    g_api.repo_remove_source = G(a_repo_remove_source);
    g_api.vm_map = G(a_vm_map);
    g_api.vm_unmap = G(a_vm_unmap);
    g_api.vm_protect = G(a_vm_protect);
    g_api.thread_start = G(a_thread_start);
    g_api.thread_detach = G(a_thread_detach);
    g_api.tls_set = G(a_tls_set);
    g_api.mem_info = G(a_mem_info);
    g_api.app_id = G(a_app_id);
    g_api.sock_open = G(a_sock_open);
    g_api.sock_bind = G(a_sock_bind);
    g_api.sock_listen = G(a_sock_listen);
    g_api.sock_accept = G(a_sock_accept);
    g_api.sock_connect = G(a_sock_connect);
    g_api.sock_send = G(a_sock_send);
    g_api.sock_recv = G(a_sock_recv);
    g_api.sock_shutdown = G(a_sock_shutdown);
    g_api.sock_opt = G(a_sock_opt);
    g_api.sock_name = G(a_sock_name);
    g_api.sock_ctl = G(a_sock_ctl);
    g_api.poll = G(a_poll);
    g_api.resolve = G(a_resolve);
    g_api.sig_entry = G(a_sig_entry);
    g_api.sig_action = G(a_sig_action);
    g_api.sig_mask = G(a_sig_mask);
    g_api.sig_send = G(a_sig_send);
    g_api.sig_altstack = G(a_sig_altstack);
    g_api.sig_pending = G(a_sig_pending);
#ifdef __x86_64__
    spec_init();
#endif
}

/* ── the ELF loader ────────────────────────────────────────────────── */

#define PT_LOAD    1
#define PT_DYNAMIC 2
#define DT_NULL    0
#define DT_RELA    7
#define DT_RELASZ  8
#define DT_RELAENT 9
#define DT_REL     17
#define DT_RELSZ   18
#define DT_RELENT  19
#define DT_PLTRELSZ 2
#define DT_STRTAB  5
#define DT_SYMTAB  6
#define DT_SYMENT  11
#define DT_PLTREL  20
#define DT_JMPREL  23
#define R_X86_64_NONE     0
#define R_X86_64_RELATIVE 8
#define R_386_NONE        0
#define R_386_RELATIVE    8

#ifdef __x86_64__
#define EM_HOST 62            /* EM_X86_64 */
#define ELF_CLASS 2
typedef struct { uint8_t ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff;
                 uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } ehdr_t;
typedef struct { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } phdr_t;
typedef struct { int64_t tag; uint64_t val; } dyn_t;
#else
#define EM_HOST 3             /* EM_386 */
#define ELF_CLASS 1
typedef struct { uint8_t ident[16]; uint16_t type, machine; uint32_t version, entry, phoff, shoff;
                 uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } ehdr_t;
typedef struct { uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align; } phdr_t;
typedef struct { int32_t tag; uint32_t val; } dyn_t;
#endif


/* m: the app's memory to load it into (NULL: the kernel's, for a driver) */
/* run_at: where the program will run (64-bit: in its mmap region, the image
 * built in a kernel buffer and copied there), its lowest page; 0: where it
 * is loaded */

/* Validate offsets with subtraction, never by adding attacker-controlled
 * lengths. Headers are copied so malformed alignment cannot become a
 * kernel unaligned access. This is shared by exec's sizing and loading. */
static int elf_layout(const uint8_t* f, uint32_t size, uintptr_t* low, uintptr_t* high) {
    ehdr_t e;
    if (!f || size < sizeof(e)) return 0;
    memcpy(&e, f, sizeof(e));
    if (memcmp(e.ident, "\177ELF", 4) || e.ident[4] != ELF_CLASS ||
        e.ident[5] != 1 || e.ident[6] != 1 || e.version != 1 ||
        e.machine != EM_HOST || e.type != 3 || e.ehsize != sizeof(e) ||
        e.phentsize != sizeof(phdr_t) || !e.phnum ||
        e.phoff < sizeof(e) || e.phoff > size ||
        e.phnum > (size - e.phoff) / sizeof(phdr_t)) return 0;
    uintptr_t lo = ~(uintptr_t)0, hi = 0;
    int entry_ok = 0, dynamics = 0;
    for (unsigned i = 0; i < e.phnum; i++) {
        phdr_t p;
        memcpy(&p, f + e.phoff + i * sizeof(p), sizeof(p));
        if (p.type == 3) return 0; /* PT_INTERP: the SDK ABI has no ELF interpreter. */
        if (p.type != PT_LOAD || !p.memsz) {
            if (p.type == PT_LOAD && p.filesz) return 0;
            continue;
        }
        if (p.offset > size || p.filesz > size - p.offset ||
            p.filesz > p.memsz || p.vaddr > ~(uintptr_t)0 ||
            p.memsz > ~(uintptr_t)0 - p.vaddr) return 0;
        uintptr_t end = (uintptr_t)(p.vaddr + p.memsz);
        if (p.vaddr < lo) lo = (uintptr_t)p.vaddr;
        if (end > hi) hi = end;
        if ((p.flags & 1u) && e.entry >= p.vaddr && e.entry - p.vaddr < p.memsz) entry_ok = 1;
    }
    if (hi <= lo || !entry_ok) return 0;
    lo &= ~(uintptr_t)4095;
    if (hi - lo > APP_MAX_IMAGE) return 0;
    for (unsigned i = 0; i < e.phnum; i++) {
        phdr_t p;
        memcpy(&p, f + e.phoff + i * sizeof(p), sizeof(p));
        if (p.type != PT_DYNAMIC) continue;
        if (++dynamics > 1 || p.offset > size || p.filesz > size - p.offset ||
            p.filesz > p.memsz || p.memsz < sizeof(dyn_t) || p.memsz % sizeof(dyn_t) ||
            p.vaddr < lo || p.vaddr > hi || p.memsz > hi - p.vaddr) return 0;
        int covered = 0;
        for (unsigned j = 0; j < e.phnum; j++) {
            phdr_t q;
            memcpy(&q, f + e.phoff + j * sizeof(q), sizeof(q));
            if (q.type == PT_LOAD && p.vaddr >= q.vaddr &&
                p.vaddr - q.vaddr <= q.memsz && p.memsz <= q.memsz - (p.vaddr - q.vaddr))
                covered = 1;
        }
        if (!covered) return 0;
    }
    *low = lo; *high = hi;
    return 1;
}

static int elf_in_image(uintptr_t at, uintptr_t n, uintptr_t lo, uintptr_t span) {
    return at >= lo && at - lo <= span && n <= span - (at - lo);
}

/* An undefined symbol's diagnostic must never scan outside the image,
 * including a corrupt or unterminated string table. */
static void elf_symbol_name(uintptr_t base, uintptr_t lo, uintptr_t span,
                            uintptr_t strtab, uint32_t index, char* out, size_t cap) {
    if (!strtab || index > ~(uintptr_t)0 - strtab ||
        !elf_in_image(strtab + index, 1, lo, span)) {
        kstrlcpy(out, "a missing function", cap);
        return;
    }
    uintptr_t at = strtab + index;
    size_t max = (size_t)(span - (at - lo)), n = 0;
    while (n + 1 < cap && n < max && *(const char*)(base + at + n)) {
        out[n] = *(const char*)(base + at + n);
        n++;
    }
    if (cap) out[n] = 0;
}

static int load_elf(umem_t* m, const uint8_t* f, uint32_t size, uint8_t** image_out, banana_entry_t* entry, uintptr_t* base_out,
                    uintptr_t* lo_out, uintptr_t* hi_out, uintptr_t run_at, char* err, int ecap) {
    ehdr_t eh;
    if (!f || size < sizeof(eh)) { kstrlcpy(err, "not an ELF program", (size_t)ecap); return -8; }
    memcpy(&eh, f, sizeof(eh));
    if (memcmp(eh.ident, "\177ELF", 4)) { kstrlcpy(err, "not an ELF program", (size_t)ecap); return -8; }
    if (eh.ident[4] != ELF_CLASS || eh.machine != EM_HOST) {
        ksnprintf(err, (size_t)ecap, "built for another CPU (this is the %s kernel)", BANANA_ARCH);
        return -8;
    }
    if (eh.type != 3) { kstrlcpy(err, "not position-independent (build it with the Banana OS SDK)", (size_t)ecap); return -8; }
    uintptr_t lo, hi;
    if (!elf_layout(f, size, &lo, &hi)) {
        kstrlcpy(err, "damaged ELF layout (headers, segments or entry)", (size_t)ecap);
        return -8;
    }
    uint32_t span = (uint32_t)(hi - lo);
    uint8_t* raw = m ? (uint8_t*)umem_alloc_pages(m, span, 0) : (uint8_t*)kmalloc(span + 4096);
    if (!raw) { kstrlcpy(err, "out of memory", (size_t)ecap); return -12; }
    uint8_t* mem = (uint8_t*)(((uintptr_t)raw + 4095) & ~(uintptr_t)4095);
    memset(mem, 0, span);
    uintptr_t base = (uintptr_t)mem - lo;
    uintptr_t run = run_at ? run_at - lo : base;
    const uint8_t* dyn = NULL;
    uintptr_t dyn_size = 0;
    for (unsigned i = 0; i < eh.phnum; i++) {
        phdr_t p;
        memcpy(&p, f + eh.phoff + i * sizeof(p), sizeof(p));
        if (p.type == PT_LOAD && p.memsz)
            memcpy((void*)(base + (uintptr_t)p.vaddr), f + p.offset, (size_t)p.filesz);
        if (p.type == PT_DYNAMIC) {
            dyn = (const uint8_t*)(base + (uintptr_t)p.vaddr);
            dyn_size = (uintptr_t)p.memsz;
        }
    }
    if (dyn) {
        uintptr_t rel = 0, relsz = 0, relent = 0, rela = 0, relasz = 0, relaent = 0;
        uintptr_t symtab = 0, syment = 0, strtab = 0, jmprel = 0, pltrelsz = 0, pltrel = 0;
        int terminated = 0;
        for (uintptr_t off = 0; off < dyn_size; off += sizeof(dyn_t)) {
            dyn_t d;
            memcpy(&d, dyn + off, sizeof(d));
            if (d.tag == DT_NULL) { terminated = 1; break; }
            switch (d.tag) {
            case DT_RELA: rela = (uintptr_t)d.val; break;
            case DT_RELASZ: relasz = (uintptr_t)d.val; break;
            case DT_RELAENT: relaent = (uintptr_t)d.val; break;
            case DT_REL: rel = (uintptr_t)d.val; break;
            case DT_RELSZ: relsz = (uintptr_t)d.val; break;
            case DT_RELENT: relent = (uintptr_t)d.val; break;
            case DT_SYMTAB: symtab = (uintptr_t)d.val; break;
            case DT_SYMENT: syment = (uintptr_t)d.val; break;
            case DT_STRTAB: strtab = (uintptr_t)d.val; break;
            case DT_JMPREL: jmprel = (uintptr_t)d.val; break;
            case DT_PLTRELSZ: pltrelsz = (uintptr_t)d.val; break;
            case DT_PLTREL: pltrel = (uintptr_t)d.val; break;
            }
        }
#define IN_IMAGE(a, n) elf_in_image((uintptr_t)(a), (uintptr_t)(n), lo, span)
        int bad = !terminated;
        char why[96] = "";
#ifdef __x86_64__
        (void)relent;
        if (rel || relsz || (syment && syment != 24) ||
            (rela && relaent && relaent != 24) || (relasz && !rela) ||
            (pltrelsz && (!jmprel || pltrel != DT_RELA))) bad = 1;
        syment = 24;
        uintptr_t tabs[2][2] = { { rela, relasz }, { pltrel == DT_RELA ? jmprel : 0, pltrelsz } };
        for (int t = 0; t < 2 && !bad; t++) {
            if (!tabs[t][0]) continue;
            if (tabs[t][1] % 24 || !IN_IMAGE(tabs[t][0], tabs[t][1])) { bad = 1; break; }
            for (uintptr_t o = 0; o < tabs[t][1]; o += 24) {
                uint64_t r[3];
                memcpy(r, (const void*)(base + tabs[t][0] + o), sizeof(r));
                uint32_t type = (uint32_t)r[1], si = (uint32_t)(r[1] >> 32);
                if (type == R_X86_64_NONE) continue;
                if (!IN_IMAGE(r[0], type == 2 ? 4 : 8)) { bad = 1; break; }
                void* P = (void*)(base + r[0]);
                uint64_t value;
                if (type == R_X86_64_RELATIVE) { value = run + r[2]; memcpy(P, &value, 8); continue; }
                if (!symtab || si > (~(uintptr_t)0 - symtab) / syment) { bad = 1; break; }
                uintptr_t sa = symtab + (uintptr_t)si * syment;
                if (!IN_IMAGE(sa, 24)) { bad = 1; break; }
                const uint8_t* sym = (const uint8_t*)(base + sa);
                uint16_t shndx;
                uint64_t val;
                memcpy(&shndx, sym + 6, 2); memcpy(&val, sym + 8, 8);
                if (!shndx) {
                    uint32_t nm; char name[64];
                    memcpy(&nm, sym, 4); elf_symbol_name(base, lo, span, strtab, nm, name, sizeof(name));
                    ksnprintf(why, sizeof(why), "it needs %s, which Banana OS does not have", name);
                    bad = 2; break;
                }
                uint64_t S = run + val;
                if (type == 1) value = S + r[2];
                else if (type == 6 || type == 7) value = S;
                else if (type == 2) {
                    uint32_t v = (uint32_t)(S + r[2] - (run + r[0]));
                    memcpy(P, &v, 4); continue;
                } else { ksnprintf(why, sizeof(why), "unsupported relocation type %u", type); bad = 2; break; }
                memcpy(P, &value, 8);
            }
        }
#else
        (void)relaent;
        if (rela || relasz || (syment && syment != 16) ||
            (rel && relent && relent != 8) || (relsz && !rel) ||
            (pltrelsz && (!jmprel || pltrel != DT_REL))) bad = 1;
        syment = 16;
        uintptr_t tabs[2][2] = { { rel, relsz }, { pltrel == DT_REL ? jmprel : 0, pltrelsz } };
        for (int t = 0; t < 2 && !bad; t++) {
            if (!tabs[t][0]) continue;
            if (tabs[t][1] % 8 || !IN_IMAGE(tabs[t][0], tabs[t][1])) { bad = 1; break; }
            for (uintptr_t o = 0; o < tabs[t][1]; o += 8) {
                uint32_t r[2];
                memcpy(r, (const void*)(base + tabs[t][0] + o), sizeof(r));
                uint32_t type = r[1] & 0xFF, si = r[1] >> 8;
                if (type == R_386_NONE) continue;
                if (!IN_IMAGE(r[0], 4)) { bad = 1; break; }
                void* P = (void*)(base + r[0]);
                uint32_t value;
                memcpy(&value, P, 4);
                if (type == R_386_RELATIVE) { value += run; memcpy(P, &value, 4); continue; }
                if (!symtab || si > (~(uintptr_t)0 - symtab) / syment) { bad = 1; break; }
                uintptr_t sa = symtab + (uintptr_t)si * syment;
                if (!IN_IMAGE(sa, 16)) { bad = 1; break; }
                const uint8_t* sym = (const uint8_t*)(base + sa);
                uint32_t val; uint16_t shndx;
                memcpy(&val, sym + 4, 4); memcpy(&shndx, sym + 14, 2);
                if (!shndx) {
                    uint32_t nm; char name[64];
                    memcpy(&nm, sym, 4); elf_symbol_name(base, lo, span, strtab, nm, name, sizeof(name));
                    ksnprintf(why, sizeof(why), "it needs %s, which Banana OS does not have", name);
                    bad = 2; break;
                }
                uint32_t S = (uint32_t)run + val;
                if (type == 1) value += S;
                else if (type == 2) value += S - (uint32_t)(run + r[0]);
                else if (type == 6 || type == 7) value = S;
                else { ksnprintf(why, sizeof(why), "unsupported relocation type %u", type); bad = 2; break; }
                memcpy(P, &value, 4);
            }
        }
#endif
#undef IN_IMAGE
        if (bad) {
            if (bad == 1) kstrlcpy(err, "damaged relocations (link it with the SDK or tcc)", (size_t)ecap);
            else kstrlcpy(err, why, (size_t)ecap);
            if (m) umem_free_pages(m, raw, 0); else kfree(raw);
            return -8;
        }
    }
    *image_out = raw;
    *entry = (banana_entry_t)(run + (uintptr_t)eh.entry);
    *base_out = run; *lo_out = run + lo; *hi_out = run + lo + span;
    return 0;
}

/* the same loader for driver modules (kernel/driver.c): the image stays loaded */
int app_load_image(const uint8_t* f, uint32_t size, uint8_t** raw, uintptr_t* entry, char* err, int ecap) {
    banana_entry_t e;
    uintptr_t base, lo, hi;
    if (load_elf(NULL, f, size, raw, &e, &base, &lo, &hi, 0, err, ecap) < 0) return -1;
    *entry = (uintptr_t)e;
    return 0;
}

/* ── running ───────────────────────────────────────────────────────── */

#ifndef __x86_64__
static int trampoline(void* arg) {
    app_proc_t* p = (app_proc_t*)arg;
    return p->entry(&g_api, p->argc, p->argv);
}
#endif

/* before an app's memory goes: every thread has to be out of its code */
static void stop_threads(app_proc_t* p) {
    p->exit_req = 1;
    for (;;) {
        int live = 0;
        for (int t = 0; t < APP_THREADS; t++) if (p->threads[t].used && !p->threads[t].done) live++;
        if (!live) break;
        task_sleep_ms(2);                   /* a busy one is stopped by the timer (app_preempt) */
    }
    for (int t = 0; t < APP_THREADS; t++) {
        if (p->threads[t].used) thread_free(p, &p->threads[t]);
        p->threads[t].used = 0;
    }
}

/* everything the app had goes back (no task runs on its tables any more) */
static void discard(app_proc_t* p) {
    for (int i = 0; i < APP_FDS; i++) fd_close(p, i);
#ifdef __x86_64__
    kstack_free(p->kstack);
    p->kstack = NULL;
#endif
    if(p->web_restricted){
        /* mmap commitments share p->mem's quota: release their pages first. */
        uspace_destroy(p->us);
        umem_destroy(p->mem);
    }else{
        umem_destroy(p->mem);                  /* its code, stacks, memory, argv */
        uspace_destroy(p->us);
    }
    p->mem = NULL;
    p->us = NULL;
}

/* everything the process holds goes (its slot stays: proc_finish decides) */
static void teardown(app_proc_t* p) {
    stop_threads(p);
    for (int i = 0; i < APP_FDS; i++) fd_close(p, i);   /* its descriptors: sockets close (connections end), pipes too */
    gpu_virgl_release(p->id + 1);
    gem_release(p->id + 1);                 /* (its GPU buffers: once the GPU is done with them) */
    appwin_close_owner(p->id);              /* (their pictures are its memory: before it goes) */
    webview_close_owner(p->id);
    discard(p);
    pid_clear(p->pid, p->id);
#ifdef __x86_64__
    umem_destroy(p->old_mem);              /* (an execve that never ran its program) */
    uspace_destroy(p->old_us);
    p->old_mem = NULL;
    p->old_us = NULL;
    kfree(p->fork_frame);
    p->fork_frame = NULL;
#endif
}

static void release(app_proc_t* p) {
    teardown(p);
    p->used = 0;
}


static int run(app_proc_t* p) {
#ifndef __x86_64__
    *(volatile uint32_t*)p->stack = STACK_CANARY;      /* the bottom of the stack: overflow check */
#endif
#ifdef __x86_64__
    p->guarded = 1;                                     /* (its guard pages: stack_map) */
    for (;;) {
        p->running = 1;
        if (p->fork_frame)                              /* a fork's child: from its parent's system call */
            p->exit_code = resume_user(p);
        else
            p->exit_code = enter_user(p, (uintptr_t)p->entry, (uintptr_t)p->uapi, (uintptr_t)p->argc,
                                      (uintptr_t)p->argv, p->stack, APP_STACK, p->kstack, &p->saved_sp);
        p->running = 0;
        if (!p->exec_next) break;
        exec_switch(p);                                 /* execve: the new program now */
    }
#else
    p->running = 1;
    p->exit_code = app_enter(trampoline, p, p->stack + APP_STACK, &p->saved_sp);
#endif
    p->running = 0;
    if (p->crash_msg[0]) {                              /* it crashed (app_fault) */
        klog("app crashed: %s\n", p->crash_msg);
        if (p->has_term) {
            terminal_vt_set_active(p->vt);
            terminal_write_color(p->crash_msg, VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            terminal_putchar('\n');
        }
    }
#ifndef __x86_64__
    if (*(volatile uint32_t*)p->stack != STACK_CANARY)
        klog("app %s: its stack overflowed (256 KiB) - memory may be damaged\n", p->name);
#endif
    return p->exit_code;
}

/* A CPU exception (kernel/idt.c) while an app runs - in its own code, or
 * in a system call it made with a bad pointer: the app is stopped, the
 * rest of the system goes on. Returns only if no app is running here. */
void app_fault(uint32_t vector, uint32_t err, uintptr_t ip, uintptr_t addr) {
    app_proc_t* p = cur();
    if (!p) return;
    athread_t* ft = cur_thread(p);
    if (ft ? !ft->running : !p->running) return;
    if (ft) ft->running = 0;
    else p->running = 0;
    /* We came in through an interrupt gate, so interrupts are off - and the
     * terminal output below may yield to other tasks, which must not run
     * with interrupts off (timers, sleeps and hlt would all stop): turn
     * them on first. This exception handler is never returned from. */
    __asm__ volatile("sti");
    static const char* const what[20] = {
        "division by zero", "debug trap", "NMI", "breakpoint", "overflow", "bound range",
        "invalid instruction", "no FPU", "double fault", "", "invalid TSS", "segment not present",
        "stack fault", "general protection fault", "segmentation fault (bad memory access)", "",
        "floating point error", "alignment check", "machine check", "SIMD floating point error",
    };
    const char* w = vector < 20 && what[vector][0] ? what[vector] : "CPU exception";
    uintptr_t base = p->base;
    char line[160];
    int overflow = p->guarded && addr >= (uintptr_t)p->guard && addr < (uintptr_t)p->guard + APP_GUARD;
    for (int k = 0; k < APP_THREADS; k++) {
        const athread_t* th = &p->threads[k];
        if (p->guarded && th->used && th->stack && addr >= (uintptr_t)th->stack - APP_GUARD && addr < (uintptr_t)th->stack)
            overflow = 1;
    }
    if (vector == 14 && overflow)
        ksnprintf(line, sizeof(line), "%s: stack overflow - it used more than %u KiB of stack (big local arrays? malloc them)",
                  p->name, APP_STACK >> 10);
    else if (vector == 14)
        ksnprintf(line, sizeof(line), "%s: %s at address %p (ip %p, app+%x, error %x)", p->name, w, (void*)addr,
                  (void*)ip, (uint32_t)(ip - base), err);
    else
        ksnprintf(line, sizeof(line), "%s: %s (ip %p, app+%x)", p->name, w, (void*)ip, (uint32_t)(ip - base));
    /* reported by run(), back on the task's own stack: this may be the
     * dedicated fault stack, which must not be in use when we yield */
    kstrlcpy(p->crash_msg, line, sizeof(p->crash_msg));
    p->term_sig = vector == 0 || vector == 16 || vector == 19 ? 8 : vector == 6 ? 4 : vector == 17 ? 7 : 11;
    athread_t* t = cur_thread(p);
    if (t) {                                /* a crash in a thread ends the whole app */
        if (!p->exit_req) { p->exit_req = 1; p->exit_req_code = 139; }
        t->running = 0;
        held_drop(); app_leave(t->saved_sp, 139);
    }
    held_drop(); app_leave(p->saved_sp, 139);
}

#ifdef __x86_64__
/* a stack in the app's mmap region: size bytes, guard bytes under them that
 * fault (an overflow); its lowest address, NULL */
static uint8_t* stack_map(app_proc_t* p, size_t size, size_t guard) {
    uintptr_t a = uvm_map(p->us, p->mem, 0, guard + size, UVM_READ | UVM_WRITE, 0);
    if (!a) return NULL;
    if (guard && uvm_protect(p->us, a, guard, 0) != 0) {
        uvm_unmap(p->us, p->mem, a, guard + size);
        return NULL;
    }
    return (uint8_t*)(a + guard);
}

static void stack_unmap(app_proc_t* p, uint8_t* stack, size_t size, size_t guard) {
    if (stack) uvm_unmap(p->us, p->mem, (uintptr_t)stack - guard, guard + size);
}

/* the bytes an ELF's loadable segments span (whole pages), 0 if it is not one */
static uint64_t elf_span(const uint8_t* f, uint32_t size) {
    uintptr_t lo, hi;
    if (!elf_layout(f, size, &lo, &hi)) return 0;
    return ((uint64_t)(hi - lo) + 4095) & ~4095ull;
}

/* 64-bit: the program in file f, into p's (new, empty) space - everything
 * its code can point at is in its mmap region, so a fork shares all of it
 * copy-on-write: the image (built in kernel scratch memory, relocated for
 * where it runs, copied there), its stack, argv, the call table. 0, or -1
 * with err */
static int load_program(app_proc_t* p, const uint8_t* f, uint32_t size, int argc, char** argv,
                        char** envp, char* err, int ecap) {
    uint64_t span = elf_span(f, size);
    uintptr_t at = span ? uvm_map(p->us, p->mem, 0, span, UVM_READ | UVM_WRITE | UVM_EXEC, 0) : 0x1000;
    if (!at) { kstrlcpy(err, "out of memory", (size_t)ecap); return -12; }
    umem_t* tmp = umem_create(NULL);
    uint8_t* raw = NULL;
    if (!tmp) { kstrlcpy(err, "out of memory", (size_t)ecap); return -12; }
    int load_rc = load_elf(tmp, f, size, &raw, &p->entry, &p->base, &p->code_lo, &p->code_hi, at, err, ecap);
    if (load_rc) {
        umem_destroy(tmp);
        return load_rc;
    }
    int rc = span ? uvm_write(p->us, at, raw, span) : -1;   /* (umem pages: page-aligned already) */
    umem_destroy(tmp);
    if (rc) { kstrlcpy(err, span ? "out of memory" : "no loadable code", (size_t)ecap); return -12; }
    p->stack = stack_map(p, APP_STACK, APP_GUARD);
    p->guard = p->stack ? p->stack - APP_GUARD : NULL;
    /* argv: its pointers, NULL, the environment's (envp: NULL - the system's
     * default), NULL, then the strings - the C library finds environ after argv */
    if (!envp) envp = (char**)g_default_env;
    int envc = 0;
    while (envp[envc]) envc++;
    uint64_t need = (uint64_t)(argc + 1 + envc + 1) * sizeof(char*);
    for (int i = 0; i < argc; i++) need += strlen(argv[i]) + 1;
    for (int i = 0; i < envc; i++) need += strlen(envp[i]) + 1;
    uint8_t* blk = (uint8_t*)kmalloc(need);
    uintptr_t ua = blk ? uvm_map(p->us, p->mem, 0, need, UVM_READ | UVM_WRITE, 0) : 0;
    if (ua) {
        char** ptrs = (char**)blk;
        uint64_t off = (uint64_t)(argc + 1 + envc + 1) * sizeof(char*);
        for (int i = 0; i < argc + 1 + envc; i++) {
            const char* s = i < argc ? argv[i] : i > argc ? envp[i - argc - 1] : NULL;
            if (!s) { ptrs[i] = NULL; continue; }
            size_t l = strlen(s) + 1;
            ptrs[i] = (char*)(ua + off);
            memcpy(blk + off, s, l);
            off += l;
        }
        ptrs[argc + 1 + envc] = NULL;
        if (uvm_write(p->us, ua, blk, need) != 0) ua = 0;
    }
    kfree(blk);
    p->argv = (char**)ua;
    p->argc = argc;
    cmdline_set(p, argc, argv);
    if (!p->kstack) p->kstack = kstack_new();
    if (!p->stack || !ua || !p->kstack || make_uapi(p) != 0) { kstrlcpy(err, "out of memory", (size_t)ecap); return -12; }
    return 0;
}
#endif

/* reads, loads and prepares an app (slot claimed, not started) */
static app_proc_t* prepare(const char* path, int argc, char** argv, char** envp, char* err, int ecap) {
    api_init();
    int fi = fs_find_file(path);
    if (fi < 0) { kstrlcpy(err, "no such program", (size_t)ecap); return NULL; }
    app_proc_t* p = NULL;
    for (int i = 0; i < APP_MAX; i++) if (!g_procs[i].used) { p = &g_procs[i]; p->id = i; break; }
    if (!p) { kstrlcpy(err, "too many apps running", (size_t)ecap); return NULL; }
    int id = p->id;
    memset(p, 0, sizeof(*p));
    p->id = id;
    p->seq = ++g_seq;
    p->pgid = p->sid = p->seq;                          /* (its own group and session; no parent) */
    fs_file_t* f = pin_program(fi);
    if (!f) { kstrlcpy(err, "cannot read it", (size_t)ecap); return NULL; }
#ifdef __x86_64__
    p->us = uspace_create();
    if (!p->us) { fs_unpin(fi); kstrlcpy(err, "out of memory", (size_t)ecap); return NULL; }
#endif
    p->mem = umem_create(p->us);
    if (!p->mem) { fs_unpin(fi); discard(p); kstrlcpy(err, "out of memory", (size_t)ecap); return NULL; }
    if (fd_std(p) != 0) { fs_unpin(fi); discard(p); kstrlcpy(err, "out of memory", (size_t)ecap); return NULL; }
#ifdef __x86_64__
    int bad = load_program(p, (const uint8_t*)f->content, f->size, argc, argv, envp, err, ecap) != 0;
    fs_unpin(fi);
    if (bad) { discard(p); return NULL; }
#else
    (void)envp;
    uint8_t* image;
    int bad = load_elf(p->mem, (const uint8_t*)f->content, f->size, &image, &p->entry, &p->base, &p->code_lo, &p->code_hi, 0, err, ecap) != 0;
    fs_unpin(fi);
    if (bad) { discard(p); return NULL; }
    /* its stack (a canary at its bottom: the 32-bit kernel has no guard pages) */
    p->stack = (uint8_t*)umem_alloc_pages(p->mem, APP_STACK, APP_GUARD);
    p->guard = p->stack ? p->stack - APP_GUARD : NULL;
    /* argv: the pointers, NULL, the default environment's pointers, NULL,
     * then the strings - in one block of its memory */
    int envc = 0;
    while (g_default_env[envc]) envc++;
    uint32_t need = (uint32_t)(argc + 1 + envc + 1) * sizeof(char*);
    for (int i = 0; i < argc; i++) need += (uint32_t)strlen(argv[i]) + 1;
    for (int i = 0; i < envc; i++) need += (uint32_t)strlen(g_default_env[i]) + 1;
    p->argv = (char**)umem_alloc(p->mem, need);
    int ok = p->stack && p->argv;
    if (!ok) {
        discard(p);
        kstrlcpy(err, "out of memory", (size_t)ecap);
        return NULL;
    }
    char* s = (char*)(p->argv + argc + 1 + envc + 1);
    for (int i = 0; i < argc + 1 + envc; i++) {
        const char* src = i < argc ? argv[i] : i > argc ? g_default_env[i - argc - 1] : NULL;
        if (!src) { p->argv[i] = NULL; continue; }
        p->argv[i] = s;
        size_t l = strlen(src) + 1;
        memcpy(s, src, l);
        s += l;
    }
    p->argv[argc + 1 + envc] = NULL;
    p->argc = argc;
    cmdline_set(p, argc, argv);
#endif
    const char* base = strrchr(argc ? argv[0] : path, '/');
    kstrlcpy(p->name, base ? base + 1 : (argc ? argv[0] : path), sizeof(p->name));
    p->used = 1;
    return p;
}


int app_exec(const char* path, int argc, char** argv, char* err, int ecap) {
    app_proc_t* p = prepare(path, argc, argv, NULL, err, ecap);
    if (!p) return -1;
    p->pid = task_current_pid();
    pid_set(p->pid, p->id, -1);
    p->vt = terminal_vt_get_active();
    p->has_term = !task_is_background();
    p->last_breath = timer_ms();
    int rc = run(p);
    terminal_vt_set_active(p->vt);
    terminal_setcolor(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    proc_finish(p);
    return rc;
}

static void spawned_entry(void) {
    app_proc_t* p = cur();
    if (!p) return;
    task_set_background();             /* no terminal: it talks through its windows */
    run(p);
    if(!p->web_restricted)klog("app %s exited (%d)\n", p->name, p->exit_code);
    else if(p->web_stop_reason==1)
        klog("browser quota: CPU %u ms exceeded %u ms (app %d)\n",p->web_stop_cpu,p->web_cpu_limit,p->id);
    else if(p->web_stop_reason==2)
        klog("browser quota: wall deadline exceeded (app %d)\n",p->id);
    else if(p->exit_code&&p->exit_code!=137&&!p->crash_msg[0])
        klog("browser worker exited with code %d (app %d)\n",p->exit_code,p->id);
    proc_finish(p);
}

int app_spawn(const char* path, int argc, char** argv, char* err, int ecap) {
    app_proc_t* p = prepare(path, argc, argv, NULL, err, ecap);
    if (!p) return -1;
    p->pid = -1;
    p->own_task = 1;
    p->has_term = 0;
    p->vt = 0;
    int pid = task_create(p->name, spawned_entry);
    if (pid < 0) { release(p); kstrlcpy(err, "too many tasks", (size_t)ecap); return -1; }
    p->pid = pid;                      /* the new task runs once we yield */
    pid_set(pid, p->id, -1);
    return 0;
}

uint32_t app_web_generation(int id) {
    return id>=0 && id<APP_MAX && g_procs[id].used && g_procs[id].web_restricted ? (uint32_t)g_procs[id].seq : 0;
}
int app_web_alive(int id, uint32_t generation) {
    return id>=0 && id<APP_MAX && g_procs[id].used && !g_procs[id].zombie &&
           g_procs[id].web_restricted && (uint32_t)g_procs[id].seq==generation;
}
uint32_t app_web_memory(int id, uint32_t generation) {
    return app_web_alive(id,generation) ? umem_committed(g_procs[id].mem) : 0;
}
int app_web_stop_reason(int id, uint32_t generation) {
    /* Keep the reason after teardown, but never report a reused slot's status. */
    if(id<0||id>=APP_MAX)return 0;
    app_proc_t* p=&g_procs[id];
    return p->web_restricted&&(uint32_t)p->seq==generation?(int)p->web_stop_reason:0;
}
int app_web_kill(int id, uint32_t generation) {
    return app_web_alive(id,generation) ? app_kill(id) : -1;
}

void app_web_budget(int id, uint32_t generation, uint32_t wall_ms, uint32_t cpu_ms) {
    if (id < 0 || id >= APP_MAX) return;
    app_proc_t* p = &g_procs[id];
    if (!p->used || !p->web_restricted || (uint32_t)p->seq!=generation) return;
    task_query(p->pid, NULL, &p->web_cpu_start, NULL);
    p->web_deadline = wall_ms ? timer_ms() + wall_ms : 0;
    p->web_cpu_limit = cpu_ms;
    p->web_stop_reason=0;p->web_stop_cpu=0;
}

int app_spawn_web_worker(const char* path, char* err, int cap) {
#ifdef __x86_64__
    umem_web_pool_init();
    char* argv[] = { "web-worker" };
    app_proc_t* p = prepare(path, 1, argv, NULL, err, cap);
    if (!p) return -1;
    p->web_restricted = 1;
    p->web_window = timer_ms();
    p->web_deadline = timer_ms() + 90000u;
    p->web_cpu_limit = 30000u;
    if(umem_limit(p->mem,512u<<20)<0){
        release(p);kstrlcpy(err,"browser memory pool is full",(size_t)cap);return -1;
    }
    if(uvm_limit(p->us,p->mem,1024u<<20,512)<0){
        release(p);kstrlcpy(err,"browser loader exceeds memory pool",(size_t)cap);return -1;
    }
    p->pid = -1; p->own_task = 1; p->has_term = 0; p->vt = 0;
    int pid = task_create("web-worker", spawned_entry);
    if (pid < 0) { release(p); return -1; }
    p->pid = pid;
    pid_set(pid, p->id, -1);
    return p->id;
#else
    (void)path;
    kstrlcpy(err, "Isolated browsing requires the 64-bit kernel", (size_t)cap);
    return -1;
#endif
}

#ifdef __x86_64__
/* ── the kernel's own terminals (kernel/vcons.c: the consoles, the serial
 * line) - it holds the master; the program runs on the slave ── */

static void tty_entry(void) {
    app_proc_t* p = cur();
    if (!p) return;
    task_set_background();             /* (its keys come through the pty, not the keyboard) */
    run(p);
    proc_finish(p);
}

/* path started in a task of its own, a session leader with t as its
 * controlling terminal and stdin / stdout / stderr; its pid, or -1 with err */
int app_spawn_tty(const char* path, int argc, char** argv, char** envp, pty_t* t, char* err, int ecap) {
    app_proc_t* p = prepare(path, argc, argv, envp, err, ecap);
    if (!p) return -1;
    p->pid = -1;
    p->own_task = 1;
    p->has_term = 0;
    p->vt = 0;
    for (int fd = 0; fd < 3; fd++) {
        kfile_t* f = kfile_new(KF_PTS, KF_READ | KF_WRITE);
        if (!f) { release(p); kstrlcpy(err, "out of memory", (size_t)ecap); return -1; }
        f->pty = t;
        pty_slave_ref(t, 1);
        kfile_put(p->fdt[fd]);
        p->fdt[fd] = f;
    }
    int pid = task_create(p->name, tty_entry);
    if (pid < 0) { release(p); kstrlcpy(err, "too many tasks", (size_t)ecap); return -1; }
    p->pid = pid;                      /* (it runs once we yield) */
    ctty_set(p, t);
    pty_set_session(t, p->sid);
    pty_set_fg(t, p->pgid);
    pid_set(pid, p->id, -1);
    return p->seq;
}

/* the process pid is there (not ended) */
int app_alive(int pid) {
    app_proc_t* p = proc_by_pid(pid);
    return p && !p->zombie;
}

/* typed into a terminal the kernel holds: its signals (^C...) sent */
long app_pty_type(pty_t* t, const uint8_t* buf, unsigned long n) {
    long r = pty_master_write(t, buf, n);
    pty_dispatch(t);
    return r;
}

/* the kernel lets go of it: SIGHUP to its session */
void app_pty_close(pty_t* t) {
    if (!pty_master_ref(t, -1)) pty_dispatch(t);
}

/* its size changed: SIGWINCH to its foreground */
void app_pty_resize(pty_t* t, int cols, int rows) {
    pty_winsize_t ws = { (uint16_t)rows, (uint16_t)cols, 0, 0 };
    pty_set_winsize(t, &ws);
    pty_dispatch(t);
}
#endif

/* what an app holds: its blocks' memory, and (64-bit) the pages of its
 * mapped memory in RAM - where its malloc heap is now (sdk/lib/malloc.c) */
static uint32_t app_mem_bytes(app_proc_t* p) {
    uint64_t n = umem_used(p->mem);
#ifdef __x86_64__
    n += uvm_resident(p->us);
#endif
    return n > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)n;
}

int app_count(void) {
    int n = 0;
    for (int i = 0; i < APP_MAX; i++) if (g_procs[i].used && !g_procs[i].zombie) n++;
    return n;
}

void app_list(void) {
    char line[96];
    int any = 0;
    for (int i = 0; i < APP_MAX; i++) {
        app_proc_t* p = &g_procs[i];
        if (!p->used || p->zombie) continue;
        uint32_t mem = app_mem_bytes(p);
        ksnprintf(line, sizeof(line), "  %-16s pid %-3d %s  %u KiB allocated", p->name, p->pid,
                  p->own_task ? "desktop " : "terminal", mem / 1024);
        terminal_writeln(line);
        any = 1;
    }
    if (!any) terminal_writeln("  (no app running)");
}

int app_snapshot(app_info_t* out, int max) {
    int n = 0;
    for (int i = 0; i < APP_MAX && n < max; i++) {
        app_proc_t* p = &g_procs[i];
        if (!p->used || p->zombie) continue;
        out[n].id = p->id;
        kstrlcpy(out[n].name, p->name, sizeof(out[n].name));
        out[n].pid = p->pid;
        out[n].desktop = p->own_task;
        out[n].mem = app_mem_bytes(p);
        out[n].threads = 1;
        for (int t = 0; t < APP_THREADS; t++) if (p->threads[t].used && !p->threads[t].done) out[n].threads++;
        n++;
    }
    return n;
}

int app_kill(int id) {
    if (id < 0 || id >= APP_MAX || !g_procs[id].used || g_procs[id].zombie) return -1;
    g_procs[id].kill_req = 1;
    proc_wake(&g_procs[id]);
    return 0;
}

int app_console_focused(void) {
    for (int i = 0; i < APP_MAX; i++)
        if (g_procs[i].used && !g_procs[i].zombie && focused(&g_procs[i])) return 1;
    return 0;
}
