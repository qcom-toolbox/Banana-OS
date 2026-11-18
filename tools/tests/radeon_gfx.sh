#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# The Radeon HD 7450's 3D engine bring-up on a model command processor (tools/tests/radeon_gfx.c)
set -eu
cd "$(dirname "$0")/../.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
gcc -O1 -g -fsanitize=address,undefined -fno-sanitize=shift --param asan-globals=0 \
    -Wall -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function \
    -I kernel tools/tests/radeon_gfx.c third_party/radeon/evergreen_gfx.c third_party/radeon/r600_gfx.c \
    third_party/radeon/ni_gfx.c third_party/radeon/evergreen_golden.c third_party/radeon/radeon_cs_check.c kernel/kstring.c -o "$out/test"
for mode in ok gddr5 dead cedar real2g fake2g juniper wideap drm turks barts; do
    ASAN_OPTIONS=detect_leaks=0 "$out/test" $mode
done
# the display's watermarks against the 3D engine's memory traffic (Linux's arithmetic)
gcc -O1 -g -fsanitize=address,undefined -Wall -I kernel tools/tests/radeon_wm.c third_party/radeon/evergreen_wm.c -o "$out/wm"
"$out/wm"
