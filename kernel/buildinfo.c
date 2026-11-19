/* SPDX-License-Identifier: GPL-3.0-or-later */
/* the build this kernel is (the Makefile's build/build_info.h): CI's run
 * number - the release's build-N tag - or "dev", and the commit */
#include "buildinfo.h"
#include "../build/build_info.h"

const char* banana_build(void) { return BANANA_BUILD; }
const char* banana_commit(void) { return BANANA_COMMIT; }
