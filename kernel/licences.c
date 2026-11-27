/* SPDX-License-Identifier: GPL-3.0-or-later */
/* /usr/share/licenses/banana-os: Banana OS's licence (COPYING, GPL 3), the
 * notices of the components it includes (THIRD-PARTY-NOTICES, COPYRIGHT) and
 * the texts of their licences (GPL 2, LGPL, Apache, BSD, MIT...), built into
 * the kernel (tools/mklicences.py) - so an installed system has them too,
 * not only its installation medium (/LICENSES). Written at the start when
 * they changed (an installed disk is not written at every boot). */
#include "licences.h"
#include "fs.h"
#include "kstring.h"
#include "kheap.h"

#define LICENCE(i, name) extern const uint8_t lic_##i##_start[], lic_##i##_end[];
#include "../build/licences.h"
#undef LICENCE

static void put(const char* path, const uint8_t* data, uint32_t n) {
    int idx = fs_find_file(path);
    if (idx >= 0) {
        const fs_file_t* f = fs_file_info(idx);
        if (f && f->size == n) {
            uint8_t* old = (uint8_t*)kmalloc(n + 1);
            int same = old && fs_read(idx, 0, old, n) == (int)n && memcmp(old, data, n) == 0;
            kfree(old);
            if (same) return;
        }
    }
    fs_write_path_sys(path, data, n);
}

void licences_boot(void) {
    fs_mkdir_p_sys("/usr/share/licenses/banana-os");
#define LICENCE(i, name) put("/usr/share/licenses/banana-os/" name, lic_##i##_start, (uint32_t)(lic_##i##_end - lic_##i##_start));
#include "../build/licences.h"
#undef LICENCE
}
