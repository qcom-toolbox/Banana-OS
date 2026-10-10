#ifndef APP_H
#define APP_H

#include "types.h"

/*
 * Apps built with the Banana OS SDK (sdk/): position-independent ELF
 * executables that call the system through the table in
 * sdk/include/banana_api.h. kernel/pkg.c installs them from .bpk
 * packages into /apps/<name>/.
 *
 * An app runs on its own 1 MiB stack (with guard pages under it on the
 * 64-bit kernel), in ring 0 like the rest of Banana OS. A CPU exception
 * while it runs stops just the app (app_fault), and everything it
 * allocated, opened or put on screen is released when it exits.
 */

/* runs the app at path (an ELF file) in the calling task - a shell's -
 * and returns its exit code once it ends; -1 with err if it cannot start */
int  app_exec(const char* path, int argc, char** argv, char* err, int ecap);

/* starts it in a task of its own (desktop apps); 0, or -1 with err */
int  app_spawn(const char* path, int argc, char** argv, char* err, int ecap);

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

/* kernel/idt.c: a hardware interrupt came at ip (time-slices app code) */
void app_preempt(uintptr_t ip);

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

#endif
