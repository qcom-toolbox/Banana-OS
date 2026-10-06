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
} app_info_t;
int  app_snapshot(app_info_t* out, int max);
/* stops app id the next time it calls the system (its windows close) */
int  app_kill(int id);

/* kernel/idt.c: CPU exception `vector` in the current task; if an app is
 * running there it is stopped (this does not return), else it returns */
void app_fault(uint32_t vector, uint32_t err, uintptr_t ip, uintptr_t addr);

/* running apps, for `pkg ps` */
void app_list(void);
int  app_count(void);

#endif
