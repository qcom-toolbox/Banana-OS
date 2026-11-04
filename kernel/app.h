/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef APP_H
#define APP_H

#include "types.h"

/*
 * Apps built with the Banana OS SDK (sdk/): position-independent ELF
 * executables that call the system through the table in
 * sdk/include/banana_api.h. kernel/pkg.c installs them from .bpk
 * packages into /apps/<name>/.
 *
 * An app runs on its own 1 MiB stack, with guard pages under it. On the
 * 64-bit kernel it runs in ring 3, in an address space of its own
 * (kernel/uspace.c) where only its memory (kernel/umem.c) is open to it,
 * and calls the system through the syscall instruction (int 0x80 where the
 * processor has none; both end in app_syscall); the 32-bit kernel
 * runs it in ring 0. A CPU exception while it runs stops just the app
 * (app_fault), and everything it allocated, opened or put on screen is
 * released when it exits.
 */

#define APP_MAX 96      /* processes (apps) at once, zombies included */

/* runs the app at path (an ELF file) in the calling task - a shell's -
 * and returns its exit code once it ends; -1 with err if it cannot start */
int  app_exec(const char* path, int argc, char** argv, char* err, int ecap);

/* starts it in a task of its own (desktop apps); 0, or -1 with err */
int  app_spawn(const char* path, int argc, char** argv, char* err, int ecap);

#ifdef __x86_64__
/* the kernel's own terminals (kernel/vcons.c, SSH sessions): path started
 * as a session leader on pty t (envp NULL: the default environment) - its
 * pid, or -1 with err; whether pid still runs; keys typed into t (^C:
 * SIGINT...); t's size changed (SIGWINCH); the master let go (SIGHUP) */
struct pty;
int  app_spawn_tty(const char* path, int argc, char** argv, char** envp, struct pty* t, char* err, int ecap);
int  app_alive(int pid);
int  app_exit_code(int pid);            /* (one that ended lately: its exit code, -1) */
long app_pty_type(struct pty* t, const uint8_t* buf, unsigned long n);
void app_pty_resize(struct pty* t, int cols, int rows);
void app_pty_close(struct pty* t);
#endif

/* Kernel-only restricted browser workers. Return app ID, never a capability
 * accepted from user space. Unsupported on the unprotected 32-bit kernel. */
int app_spawn_web_worker(const char* path, char* err, int cap);
uint32_t app_web_generation(int id);
uint32_t app_web_memory(int id, uint32_t generation);
int app_web_stop_reason(int id, uint32_t generation); /* 1: CPU, 2: wall; 0: unknown */
int app_web_alive(int id, uint32_t generation);
int app_web_kill(int id, uint32_t generation);
void app_web_budget(int id, uint32_t generation, uint32_t wall_ms, uint32_t cpu_ms);

/* running apps (Task Manager) */
typedef struct {
    int      id;
    char     name[32];
    int      pid;
    int      desktop;      /* started from the desktop (its own task) */
    uint32_t mem;          /* bytes it allocated */
    int      threads;      /* its main thread + the ones it started */
} app_info_t;
int  app_snapshot(app_info_t* out, int max);
/* stops app id the next time it calls the system (its windows close) */
int  app_kill(int id);

/* kernel/idt.c: CPU exception `vector` in the current task; if an app is
 * running there it is stopped (this does not return), else it returns */
void app_fault(uint32_t vector, uint32_t err, uintptr_t ip, uintptr_t addr);
/* kernel/idt.c, every page fault: 1 if it was a page of the running app's
 * mmap region touched for the first time (now there: the access goes on) */
int  app_vm_fault(uintptr_t addr, uint32_t err, int from_user, int ints_on);

/* kernel/idt.c: a hardware interrupt came at ip, from ring 3 or not
 * (time-slices app code) */
void app_preempt(uintptr_t ip, int from_user, void* regs);
/* kernel/idt.c, a CPU exception in app code (ring 3): 1 if it went to the
 * app's signal handler (the CPU resumes there), 0 if the app must stop */
int  app_signal_fault(void* regs);

/* memory of app `owner` - what it may write to (its windows' pictures,
 * kernel/appwin.c); kernel memory on neither */
void* app_mem_alloc(int owner, size_t size);
void  app_mem_free(int owner, void* ptr);

/* kernel/task.c: the kernel stack task pid left to run app code (0 if it
 * is not running any) - a fault on another core is handled below it */
uintptr_t app_fault_stack(int pid);

/* running apps, for `pkg ps` */
void app_list(void);
int  app_count(void);
/* a console app runs in the terminal that has the keyboard (it takes the F-keys) */
int  app_console_focused(void);
/* loads a position-independent ELF (an app or a driver module) into the
 * kernel's memory: 0 with *raw (its allocation) and *entry, or -1 with err */
int  app_load_image(const uint8_t* f, uint32_t size, uint8_t** raw, uintptr_t* entry, char* err, int ecap);

/* system calls made on the other cores (sched calls): done there / on the boot core */
typedef struct { const char* name; uint32_t here, home; } app_call_stat_t;
int app_call_stats(app_call_stat_t* out, int max, int reset);

#endif
