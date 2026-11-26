/* SPDX-License-Identifier: BSD-2-Clause */
/* Banana OS app startup: the system calls _banana_start() with its call
 * table; it sets up the C library and runs main(). */
#include "banana_api.h"

const banana_api_t* __banana;
extern unsigned long __banana_stack_top;   /* (pthread.c: the main thread's stack) */

/* (a main with argc, argv only, or none, takes this call as well: the C ABI) */
int  main(int argc, char** argv, char** envp);
extern char** environ;
extern char* program_invocation_name;
extern char* program_invocation_short_name;
void __libc_init(void);
void exit(int code) __attribute__((noreturn));

__attribute__((used, visibility("default")))
int _banana_start(const banana_api_t* api, int argc, char** argv) {
    if (!api || api->magic != BANANA_API_MAGIC || api->version < 1) return 127;
    __banana = api;
    /* version 26: the environment follows argv's NULL (execve's envp, or the
     * system's default) */
    if (BANANA_HAS(api, proc_kill) && argv && !argv[argc] && argv[argc + 1]) environ = argv + argc + 1;
    /* (the name error messages start with: GNU's program_invocation_name) */
    if (argv && argc > 0 && argv[0] && argv[0][0]) {
        program_invocation_name = argv[0];
        char* s = argv[0];
        for (char* c = argv[0]; *c; c++) if (*c == '/') s = c + 1;
        program_invocation_short_name = s;
    }
    /* (built for users, API 35: the system need not move /home/banana for
     * it - unless BANANA_LEGACY=1 asks to be treated as an older program) */
    if (api->version >= 35 && BANANA_HAS(api, auth)) {
        int legacy = 0;
        for (char** e = environ; e && *e; e++)
            if (!__builtin_strcmp(*e, "BANANA_LEGACY=1")) legacy = 1;
        if (!legacy) api->auth(BANANA_AUTH_MODERN, 0, 0, 0);
    }
    __banana_stack_top = (unsigned long)__builtin_frame_address(0);
    __libc_init();
    exit(main(argc, argv, environ));
}
