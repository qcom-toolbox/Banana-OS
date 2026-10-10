/* Banana OS Driver Kit: the entry point the kernel calls (it keeps the
 * table for the driver's code, then runs banana_driver_main) */
#include "banana_driver.h"

const banana_driver_api_t* bdrv;

__attribute__((visibility("default"))) int _banana_driver_start(const banana_driver_api_t* api) {
    if (!api || api->magic != BANANA_DRV_MAGIC) return -1;
    bdrv = api;
    return banana_driver_main(api);
}
