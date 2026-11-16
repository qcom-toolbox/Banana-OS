#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# The Radeon TeraScale 2 display path against a synthetic AtomBIOS (tools/tests/radeon_kms.c)
set -eu
cd "$(dirname "$0")/../.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
gcc -O1 -g -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -fsanitize=address,undefined --param asan-globals=0 \
    -Wno-unused-parameter -Wno-sign-compare \
    -I kernel -I shell -I net -I crypto -I usb -I web -I media \
    tools/tests/radeon_kms.c kernel/kstring.c -o "$out/test"
# (the driver keeps the video BIOS and the interpreter for the life of the system)
ASAN_OPTIONS=detect_leaks=0 "$out/test" "$@"
