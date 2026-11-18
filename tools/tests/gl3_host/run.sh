#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# sdk/lib/gl3.c's pipelined window swaps on the Linux build of libOSMesa:
# softpipe (forced, BGL3_ASYNC=1) and, with the R600=1 reference build,
# r600 on the Radeon HD 7450 model (tools/tests/radeon_host's preload).
#   MESA_REF=ports/mesa/build-r600/ref tools/tests/gl3_host/run.sh
set -e
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
REF=${MESA_REF:-$ROOT/ports/mesa/build-r600/ref}
LIB=$REF/src/gallium/targets/osmesa
OUT=${TMPDIR:-/tmp}/gl3_host
mkdir -p "$OUT"
cc -O1 -g -Wall -Wno-unused-function -I "$ROOT/tools/tests/gl3_host/include" -iquote "$ROOT/sdk/include" -idirafter "$ROOT/sdk/include" \
   "$ROOT/tools/tests/gl3_host/gl3_swap.c" "$ROOT/sdk/lib/gl3.c" -ldl -o "$OUT/gl3_swap"
echo "== softpipe"
GALLIUM_DRIVER=softpipe BGL3_ASYNC=1 BGL3_LIBRARY="$LIB/libOSMesa.so.8" "$OUT/gl3_swap"
if [ -f "$OUT/../radeon_host/preload.so" ] || [ -n "$RADEON_PRELOAD" ]; then
    echo "== r600 on the model (the frames' pixels are the model's: only the path is checked)"
    GALLIUM_DRIVER=r600 MODEL_QUIET=1 GL3_HOST_NO_PIXELS=1 LD_PRELOAD="${RADEON_PRELOAD:-$OUT/../radeon_host/preload.so}" BGL3_LIBRARY="$LIB/libOSMesa.so.8" \
        "$OUT/gl3_swap" > "$OUT/r600.log" 2>&1 || { cat "$OUT/r600.log"; exit 1; }
    cat "$OUT/r600.log"
    grep -q "renderer: AMD CAICOS" "$OUT/r600.log" && grep -q "(0 refused)" "$OUT/r600.log" && grep -q " 0 bad addresses" "$OUT/r600.log" ||
        { echo "r600: the pipelined swaps' command streams were refused or went astray"; exit 1; }
fi
