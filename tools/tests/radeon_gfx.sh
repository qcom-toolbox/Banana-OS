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
    third_party/radeon/ni_gfx.c kernel/kstring.c -o "$out/test"
for mode in ok gddr5 dead cedar real2g fake2g juniper; do
    ASAN_OPTIONS=detect_leaks=0 "$out/test" $mode
done
