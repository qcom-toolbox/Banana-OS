/* Banana OS app startup: the system calls _banana_start() with its call
 * table; it sets up the C library and runs main(). */
#include "banana_api.h"

const banana_api_t* __banana;

int  main(int argc, char** argv);
void __libc_init(void);
void exit(int code) __attribute__((noreturn));

__attribute__((used, visibility("default")))
int _banana_start(const banana_api_t* api, int argc, char** argv) {
    if (!api || api->magic != BANANA_API_MAGIC || api->version < 1) return 127;
    __banana = api;
    __libc_init();
    exit(main(argc, argv));
}
