#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Packs Java for Banana OS into one package, java.bpk: the java app, and
# java.zip that `pkg install` unpacks into /usr/java (unpack=, kernel/pkg.c):
#     /usr/java/jdk             OpenJDK 17          (ports/openjdk/build.sh)
#     /usr/java/lib/libOSMesa.so  OpenGL 3.3        (ports/mesa/build.sh; CI: the
#                                 CROCUS=1 IRIS=1 build, Intel GPUs after gpu3d on)
#     /usr/java/lwjgl           LWJGL 3             (ports/lwjgl/build.sh)
# The java app, <GL/gl3.h> apps and the Minecraft launcher look there.
#     ports/openjdk/package.sh [output dir]      (default: ports/openjdk/build)
# Needs those three builds (JDK_OUT, MESA_OUT, LWJGL_OUT: their output
# folders, by default each port's build/), zip.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(realpath -m "${1:-$HERE/build}")
JDK_OUT=$(realpath -m "${JDK_OUT:-$HERE/build}")
MESA_OUT=$(realpath -m "${MESA_OUT:-$HERE/../mesa/build}")
LWJGL_OUT=$(realpath -m "${LWJGL_OUT:-$HERE/../lwjgl/build}")
for need in "$JDK_OUT/jdk/lib/server/libjvm.so" "$MESA_OUT/libOSMesa.so" "$LWJGL_OUT/lwjgl/liblwjgl.so"; do
    [ -f "$need" ] || { echo "missing $need (ports/openjdk, ports/mesa, ports/lwjgl: build.sh)"; exit 1; }
done
mkdir -p "$OUT"

# /usr/java as it will be
S=$OUT/usr-java
rm -rf "$S" && mkdir -p "$S/lib"
cp -r "$JDK_OUT/jdk" "$S/"
cp "$MESA_OUT/libOSMesa.so" "$S/lib/"
cp -r "$LWJGL_OUT/lwjgl" "$S/"
# (the kernel's unzip: no ZIP64 - far below its 4 GB)
rm -f "$OUT/java.zip"
(cd "$S" && zip -qrX "$OUT/java.zip" .)

# the java app with it (its own build folder: the plain java.bpk stays as it is)
make -C "$HERE/launcher" BUILD=build-full DATA="$OUT/java.zip" UNPACK=java.zip:/usr/java java.bpk -B
mv "$HERE/launcher/java.bpk" "$OUT/java.bpk"
rm -rf "$HERE/launcher/build-full"
echo "Java package for Banana OS: $OUT/java.bpk ($(du -h "$OUT/java.bpk" | cut -f1); /usr/java: $(du -sh "$S" | cut -f1))"
