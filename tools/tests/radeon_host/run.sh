#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Mesa's r600 on the hosted kernel radeon interface (with Linux's command
# checker) and the Radeon HD 7450 / 6670 / 6870 models: an OpenGL 3.3 program through
# OSMesa. Needs the reference build of R600=1 ports/mesa/build.sh.
#   tools/tests/radeon_host/run.sh [frames]
set -e
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
REF=${MESA_REF:-$ROOT/ports/mesa/build-r600/ref}
SRC=$(ls -d "$ROOT"/ports/mesa/src/mesa-*/ | head -1)
OUT=${TMPDIR:-/tmp}/radeon_host
mkdir -p "$OUT"
cd "$ROOT"                                   # (the model's firmware: firmware/amd)
cc -O1 -g -fPIC -shared -fvisibility=hidden -w -I kernel tools/tests/radeon_host/preload.c \
   third_party/radeon/evergreen_gfx.c third_party/radeon/r600_gfx.c third_party/radeon/ni_gfx.c \
   third_party/radeon/evergreen_golden.c third_party/radeon/radeon_cs_check.c kernel/kstring.c \
   -o "$OUT/preload.so" -ldl -lpthread
cc -O1 -w -I "$SRC/include" tools/tests/crocus_host/gltri.c -L "$REF/src/gallium/targets/osmesa" -lOSMesa \
   -Wl,-rpath,"$REF/src/gallium/targets/osmesa" -o "$OUT/gltri"
# the HD 7450 (Caicos), and the HD 6670 (Turks) and HD 6870 (Barts) models
for chip in ok turks barts; do
    echo "== ${chip}"
    MODEL_CHIP=$chip GALLIUM_DRIVER=r600 LD_PRELOAD="$OUT/preload.so" timeout 600 "$OUT/gltri" "${1:-30}" | tee "$OUT/log"
    grep -q '(0 refused)' "$OUT/log" || { echo "r600 on the $chip model: command streams refused by the checker"; exit 1; }
    grep -q ' 0 bad addresses' "$OUT/log" || { echo "r600 on the $chip model: the GPU was sent to bad addresses"; exit 1; }
done
echo "r600 on the models: ran"
