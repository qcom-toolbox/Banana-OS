#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Builds LWJGL 3 (3.3.3, the version Minecraft 1.20.5+ ships) for Banana OS
# (64-bit), for the Java of ports/openjdk:
#     ports/lwjgl/build.sh [output dir]      (default: ports/lwjgl/build)
# Out: <output>/lwjgl - LWJGL's jars (core, OpenGL, GLFW: the official
# ones, from Maven Central) and its native libraries for Banana OS:
#   liblwjgl.so         LWJGL's core (its C sources, with libffi 3.4.6 for
#                       the calls into C and the callbacks back into Java)
#   liblwjgl_opengl.so  the OpenGL binding (GLX left out)
#   liblwjgl_stb.so     stb: images (Minecraft decodes its textures with it),
#                       TrueType, Vorbis, ...
#   libglfw.so          GLFW 3.4's API over Banana OS windows and OpenGL 3.3
#                       (native/glfw.c + sdk/lib/gl3.c: Mesa's libOSMesa.so)
#   libGL.so.1          glXGetProcAddress, for LWJGL's GL.create() (native/libgl.c)
#   libopenal.so        OpenAL 1.1's API over Banana OS's sound (native/openal.c:
#                       a mixer of its own - Minecraft's sound)
#   lwjgl2.jar          LWJGL 2's API over LWJGL 3 (lwjgl2/: Display, Keyboard,
#                       Mouse... and a class loader that rewrites the calls
#                       LWJGL 3 names otherwise) - Minecraft 1.7.2 to 1.12.2
# and <output>/lwjgldisk.img, a FAT32 disk for QEMU (-drive file=...,if=virtio)
# with jdk/, java.bpk, lib/libOSMesa.so, lwjgl/ and the test, which draws a
# triangle in a window with LWJGL:
#     java -Dos.name=Linux -Dorg.lwjgl.librarypath=/mnt/virtio/lwjgl -jar /mnt/virtio/lwjgltest.jar
# (-Dos.name=Linux: LWJGL knows Linux, macOS, Windows, FreeBSD - not "Banana OS")
#
# How: LWJGL's C sources (src/main/c and the generated src/generated/c, both
# in its source tarball) are compiled with the SDK's headers and linked
# without the system's C library: the SDK's dlopen gives them the java
# app's C library. libffi is compiled the same way after a configure for
# Linux (its fficonfig.h).
# Needs: ports/openjdk/build.sh's output (JDK_OUT, default ports/openjdk/build:
# jdk/ and the java app), ports/mesa/build.sh's (MESA_OUT, default
# ports/mesa/build: libOSMesa.so), a JDK 17 for javac and jar (JAVA_BIN, or
# the PATH), gcc, mtools, sfdisk.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(realpath -m "${1:-$HERE/build}")
VER=3.3.3
SRC_SHA=381a386688a5f5109a0edf2395adfee3cc62031929bf0ddbbd39fd356f647447
FFI=3.4.6
FFI_SHA=b0dea9df23c863a7a50e825440f3ebffabd65df1497108e5d437747843895a4e
JAR_SHA="lwjgl:dc9c7b2d48e8396d68895f8902ffa01e46253de44dfe927533ff09457ebfeec4
lwjgl-opengl:5062da750e5f7ec89e27e8b67b7d40fa8c4b16890159e35f828116ce6c8eca1e
lwjgl-glfw:bedc757fd2b1b255146db26b74b9cbd1fcf5644b89b47b104fa721c741604259
lwjgl-stb:0cff7aa46ea9d70fcc20857015293ea80fbf21adcac34600c84da9b81a44ca93
lwjgl-openal:ae0e82767cacf08921f578fba9d84753ddeb00ceada0510a2b4a882be28fbf95"
SDK=$(realpath "$HERE/../../sdk")
JDK_OUT=$(realpath -m "${JDK_OUT:-$HERE/../openjdk/build}")
MESA_OUT=$(realpath -m "${MESA_OUT:-$HERE/../mesa/build}")
JB=${JAVA_BIN:+$JAVA_BIN/}
DL=$HERE/src
SRC=$DL/lwjgl3-$VER
L=$SRC/modules/lwjgl
FS=$DL/libffi-$FFI
W=$OUT/work
LW=$OUT/lwjgl
mkdir -p "$DL" "$W" "$LW"

# ── the sources and the jars ──────────────────────────────────────────
get() {   # get URL FILE SHA256
    [ -f "$2" ] || curl -fsSL -o "$2" "$1"
    echo "$3  $2" | sha256sum -c -
}
if [ ! -d "$SRC" ]; then
    get https://github.com/LWJGL/lwjgl3/archive/refs/tags/$VER.tar.gz "$DL/lwjgl3-$VER.tar.gz" $SRC_SHA
    tar -C "$DL" -xzf "$DL/lwjgl3-$VER.tar.gz"
fi
if [ ! -d "$FS" ]; then
    get https://github.com/libffi/libffi/releases/download/v$FFI/libffi-$FFI.tar.gz "$DL/libffi-$FFI.tar.gz" $FFI_SHA
    tar -C "$DL" -xzf "$DL/libffi-$FFI.tar.gz"
fi
for js in $JAR_SHA; do
    m=${js%%:*}
    get https://repo1.maven.org/maven2/org/lwjgl/$m/$VER/$m-$VER.jar "$DL/$m-$VER.jar" ${js#*:}
    cp "$DL/$m-$VER.jar" "$LW/$m.jar"
done

# ── compiling for Banana OS ───────────────────────────────────────────
# (jni.h of the OpenJDK port alone: its folder also has a libffi ffi.h)
mkdir -p "$W/jni"
cp "$HERE/../openjdk/include/jni.h" "$HERE/../openjdk/include/jni_md.h" "$W/jni/"
BCFLAGS="-nostdinc -I$SDK/include -I$W/jni -isystem $(gcc -print-file-name=include) -D__BANANA_OS__
         -mno-red-zone -fno-stack-protector -O2 -fPIC -fvisibility=hidden -D_GNU_SOURCE"
LIBGCC=$(gcc -print-libgcc-file-name)
cc_all() {   # cc_all OUTDIR flags... -- sources...: compile, list the failures
    d=$1; shift
    fl=
    while [ "$1" != -- ]; do fl="$fl $1"; shift; done
    shift
    mkdir -p "$d"
    bad=
    for f in "$@"; do
        o=$d/$(basename "${f%.*}").o
        [ "$o" -nt "$f" ] && continue
        # shellcheck disable=SC2086
        gcc $fl $BCFLAGS -c "$f" -o "$o" 2>"$o.log" || { bad="$bad $(basename "$f")"; rm -f "$o"; }
        [ -s "$o.log" ] || rm -f "$o.log"
    done
    [ -z "$bad" ] || { echo "failed:$bad (logs: $d/*.log)"; head -n 40 "$d"/*.log; exit 1; }
}
so() {   # so NAME SONAME objects/flags...
    n=$1; s=$2; shift 2
    gcc -shared -nostdlib -Wl,-soname,$s -Wl,--no-undefined-version -Wl,-z,noexecstack -o "$LW/$n" "$@" "$LIBGCC"
}

# libffi (fficonfig.h from its configure for Linux; no static trampolines)
if [ ! -f "$FS/ref/fficonfig.h" ]; then
    mkdir -p "$FS/ref"
    (cd "$FS/ref" && ../configure -q --disable-shared --enable-static --disable-exec-static-tramp --disable-docs)
    # (closures in plain mmap'd memory: the SDK has no memfd_create)
    sed -i 's|^#define HAVE_MEMFD_CREATE 1|/* #undef HAVE_MEMFD_CREATE */|' "$FS/ref/fficonfig.h"
fi
cc_all "$W/ffi" -DHAVE_CONFIG_H -I"$FS/ref" -I"$FS/ref/include" -I"$FS/include" -I"$FS/src/x86" -- \
    "$FS"/src/prep_cif.c "$FS"/src/types.c "$FS"/src/raw_api.c "$FS"/src/java_raw_api.c \
    "$FS"/src/closures.c "$FS"/src/tramp.c "$FS"/src/x86/ffi64.c "$FS"/src/x86/ffiw64.c \
    "$FS"/src/x86/unix64.S "$FS"/src/x86/win64.S

# liblwjgl.so: LWJGL's core (not io_uring, nor FCNTL / UIO's openat, creat,
# preadv, pwritev: the SDK has none of them; LWJGL itself does not use them)
LCF="-std=gnu11 -DNDEBUG -DLWJGL_LINUX -DLWJGL_x64 -D_FILE_OFFSET_BITS=64 -I$L/core/src/main/c -I$L/core/src/main/c/linux"
CORE=$(ls "$L"/core/src/main/c/*.c "$L"/core/src/generated/c/*.c "$L"/core/src/generated/c/linux/*.c |
       grep -v -e liburing -e linux_FCNTL -e linux_UIO)
# shellcheck disable=SC2086
cc_all "$W/core" $LCF -I"$L/core/src/main/c/libffi" -I"$L/core/src/main/c/libffi/x86" -DX86_64 -- $CORE
VS=$SRC/config/linux/version.script
so liblwjgl.so liblwjgl.so -Wl,--version-script="$VS" "$W"/core/*.o "$W"/ffi/*.o

# liblwjgl_opengl.so (not GLX / WGL: no X11 here)
GLSRC=$(ls "$L"/opengl/src/generated/c/*.c | grep -v -e _WGL -e _GLX)
# shellcheck disable=SC2086
cc_all "$W/opengl" $LCF -I"$L/opengl/src/main/c" -- $GLSRC
so liblwjgl_opengl.so liblwjgl_opengl.so -Wl,--version-script="$VS" "$W"/opengl/*.o

# liblwjgl_stb.so (stb's own sources, in the generated files)
# shellcheck disable=SC2086
cc_all "$W/stb" $LCF -isystem "$L/stb/src/main/c" -- "$L"/stb/src/generated/c/*.c
so liblwjgl_stb.so liblwjgl_stb.so -Wl,--version-script="$VS" "$W"/stb/*.o

# libglfw.so (GLFW on bwin + bgl3) and libGL.so.1
cc_all "$W/glfw" -std=gnu11 -Wall -Wextra -- "$HERE/native/glfw.c" "$SDK/lib/gl3.c" "$HERE/native/libgl.c"
so libglfw.so libglfw.so "$W/glfw/glfw.o" "$W/glfw/gl3.o"
so libGL.so.1 libGL.so.1 "$W/glfw/libgl.o" -L"$LW" -l:libglfw.so

# libopenal.so (OpenAL over banana_play)
cc_all "$W/openal" -std=gnu11 -Wall -Wextra -- "$HERE/native/openal.c"
so libopenal.so libopenal.so "$W/openal/openal.o"

# what they (and Mesa) need from the java app: everything must be in its
# export table - a java app built before the SDK had it cannot load them
EXP=$HERE/../openjdk/launcher/build/x86_64/__exports.c
if [ -f "$EXP" ]; then
    for f in "$LW"/*.so* "$MESA_OUT/libOSMesa.so"; do
        [ -f "$f" ] && nm -D --undefined-only "$f" | awk '$1 == "U" {print $2}'
    done | sort -u | while read -r s; do
        grep -q "\"$s\"" "$EXP" || nm -D --defined-only "$LW"/*.so* "$MESA_OUT/libOSMesa.so" 2>/dev/null | grep -q " $s\$" ||
            echo "warning: $s is not in the java app (make -C ports/openjdk/launcher clean all)"
    done
fi

# the jars hold the SHA-1 of LWJGL's Linux libraries, which LWJGL compares
# with the ones it loads ("Incompatible Java and native library versions"):
# ours in our copies of the jars
H=$W/sha1
rm -rf "$H"
for jl in lwjgl:liblwjgl.so lwjgl-opengl:opengl/liblwjgl_opengl.so lwjgl-glfw:glfw/libglfw.so \
          lwjgl-stb:stb/liblwjgl_stb.so lwjgl-openal:openal/libopenal.so; do
    j=${jl%%:*} p=${jl#*:}
    mkdir -p "$(dirname "$H/META-INF/linux/x64/org/lwjgl/$p")"
    printf '%s\r\n' "$(sha1sum "$LW/$(basename "$p")" | cut -d' ' -f1)" > "$H/META-INF/linux/x64/org/lwjgl/$p.sha1"
    "${JB}jar" uf "$LW/$j.jar" -C "$H" "META-INF/linux/x64/org/lwjgl/$p.sha1"
done

# ── LWJGL 2 over LWJGL 3 (lwjgl2/) ────────────────────────────────────
# (no --release: the class loader uses the JDK's own ASM, jdk.internal)
L2=$W/lwjgl2
rm -rf "$L2" && mkdir -p "$L2"
"${JB}javac" -source 17 -target 17 --add-exports java.base/jdk.internal.org.objectweb.asm=ALL-UNNAMED \
    -cp "$LW/lwjgl.jar:$LW/lwjgl-opengl.jar:$LW/lwjgl-glfw.jar:$LW/lwjgl-openal.jar" -d "$L2" $(find "$HERE/lwjgl2/java" -name '*.java')
rm -f "$LW/lwjgl2.jar"
"${JB}jar" cf "$LW/lwjgl2.jar" -C "$L2" .

# ── the test and a test disk ──────────────────────────────────────────
T=$W/test
rm -rf "$T" && mkdir -p "$T/classes"
CP=$LW/lwjgl.jar:$LW/lwjgl-opengl.jar:$LW/lwjgl-glfw.jar
"${JB}javac" --release 17 -cp "$CP" -d "$T/classes" "$HERE"/test/*.java
printf 'Main-Class: LwjglTriangle\nClass-Path: lwjgl/lwjgl.jar lwjgl/lwjgl-opengl.jar lwjgl/lwjgl-glfw.jar\n' > "$T/manifest"
"${JB}jar" cfm "$OUT/lwjgltest.jar" "$T/manifest" -C "$T/classes" .

JAVA_BPK=$JDK_OUT/disk/java.bpk
[ -f "$JAVA_BPK" ] || JAVA_BPK=$HERE/../openjdk/launcher/java.bpk
for need in "$JDK_OUT/jdk/lib/server/libjvm.so" "$JAVA_BPK" "$MESA_OUT/libOSMesa.so"; do
    [ -f "$need" ] || { echo "missing $need (ports/openjdk/build.sh, ports/mesa/build.sh; JDK_OUT, MESA_OUT)"; exit 1; }
done
IMG=$OUT/lwjgldisk.img
rm -f "$IMG"
truncate -s 256M "$IMG"
printf 'label: dos\nstart=2048, type=c\n' | /sbin/sfdisk -q "$IMG"
mformat -i "$IMG@@1M" -F -v LWJGL ::
mcopy -s -i "$IMG@@1M" "$JDK_OUT/jdk" ::/jdk
mcopy -i "$IMG@@1M" "$JAVA_BPK" "$OUT/lwjgltest.jar" ::/
mmd -i "$IMG@@1M" ::/lib
mcopy -i "$IMG@@1M" "$MESA_OUT/libOSMesa.so" ::/lib/
mcopy -s -i "$IMG@@1M" "$LW" ::/lwjgl
echo "LWJGL $VER for Banana OS: $LW ($(du -sh "$LW" | cut -f1)), test disk $IMG"
