/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef EXAMPLES_H
#define EXAMPLES_H

#include "types.h"

/* The SDK's example and test apps (embedded in the kernel) as packages in
 * EXAMPLES_DIR - `pkg install /usr/share/extras/paint.bpk`; the App Store
 * lists them under Extras. At every start (only rewritten when the kernel's
 * examples changed); an older system's ~/Examples goes if it holds nothing
 * else. */
#define EXAMPLES_DIR "/usr/share/extras"
void examples_seed(void);
/* the examples one by one ("paint.bpk" and its zlib-compressed bytes), for the App Store */
int examples_count(void);
const char* examples_file(int i);
const uint8_t* examples_data(int i, uint32_t* len);

#endif
