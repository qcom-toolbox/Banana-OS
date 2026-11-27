#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Builds OpenJDK 17 (64-bit) for Banana OS: VARIANT=server (the default:
# HotSpot's template interpreter and its C1 / C2 JIT compilers) or VARIANT=zero
# (the portable C++ interpreter, no JIT):
#     ports/openjdk/build.sh [output dir]      (default: ports/openjdk/build)
# Out: <output>/jdk - a JDK image with the modules Minecraft needs (MODULES below; copy it to /jdk, or a
# disk's jdk folder: the java app finds /jdk, /mnt/usb/jdk, /mnt/virtio/jdk,
# /mnt/sata/jdk, or JAVA_HOME) - and <output>/jdkdisk.img, a FAT32 disk with
# jdk/, java.bpk and the test classes (Hello, NetTest) for QEMU
# (-drive file=...,if=virtio).
#
# How: OpenJDK is first built for Linux (configure + make images, the
# reference build, ~1 h); then every C/C++ object of HotSpot and of
# java.base's native libraries is compiled again from that build's command
# lines with the Banana OS SDK's headers (rebuild.py), and linked into
# shared libraries that the SDK's dlopen loads in the java app. The Java
# classes (lib/modules) are the reference build's: java.base runs on the
# SDK's Linux-like C library as it does on Linux.
# Needs: the reference build's tools (gcc/g++, make, autoconf, zip, the X11,
# CUPS, fontconfig, ALSA headers are not needed: headless), libffi-dev,
# mtools and sfdisk for the disk.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(realpath -m "${1:-$HERE/build}")
VER=17.0.20.1
SRC_SHA=8e5f18f6f75a759fc9584c3c2f8c44a5737692f3ea348cc1f9e63704b9dd81ee
BOOT_SHA=3808d1d15e3ec6bd5b84057fb5d84c33d8a1536a258146bcea2e603fc726e08e
DL=$HERE/src
SRC=${JDK_SRC:-$DL/jdk17u-jdk-$VER-ga}       # (JDK_SRC, BOOT_JDK: ones already there)
BOOT=${BOOT_JDK:-$DL/jdk-$VER+1}
VARIANT=${VARIANT:-server}
CONF=linux-x86_64-$VARIANT-release
REFB=$SRC/build/$CONF
JOBS=${JOBS:-$(nproc)}
mkdir -p "$DL" "$OUT"

# ── the sources and a boot JDK (Temurin) ──────────────────────────────
# (SOURCES_ONLY=1: the source archive only, checked - for the release's
#  corresponding source, tools/collect_sources.py; the boot JDK is a build tool)
if [ "${SOURCES_ONLY:-}" = 1 ]; then
    T=$DL/jdk17u-$VER.tar.gz
    [ -f "$T" ] || curl -fsSL --retry 4 -o "$T" https://github.com/openjdk/jdk17u/archive/refs/tags/jdk-$VER-ga.tar.gz
    echo "$SRC_SHA  $T" | sha256sum -c -
    exit 0
fi
if [ ! -d "$SRC" ]; then
    T=$DL/jdk17u-$VER.tar.gz
    [ -f "$T" ] || curl -fsSL -o "$T" https://github.com/openjdk/jdk17u/archive/refs/tags/jdk-$VER-ga.tar.gz
    echo "$SRC_SHA  $T" | sha256sum -c -
    tar -C "$DL" -xzf "$T"
fi
if [ ! -d "$BOOT" ]; then
    T=$DL/temurin-$VER.tar.gz
    [ -f "$T" ] || curl -fsSL -o "$T" "https://github.com/adoptium/temurin17-binaries/releases/download/jdk-$VER%2B1/OpenJDK17U-jdk_x64_linux_hotspot_${VER}_1.tar.gz"
    echo "$BOOT_SHA  $T" | sha256sum -c -
    tar -C "$DL" -xzf "$T"
fi

# ── the reference Linux build (its command lines and its classes) ─────
# (server: no ZGC / Shenandoah (memory mapped several times), JFR, JVMCI, CDS)
FEATURES=
[ "$VARIANT" = server ] && FEATURES="--with-jvm-features=-zgc,-shenandoahgc,-jfr,-jvmci,-cds,-dtrace --disable-cds-archive"
if [ ! -f "$REFB/images/jdk/lib/modules" ]; then
    (cd "$SRC" && sh configure --with-boot-jdk="$BOOT" --with-jvm-variants=$VARIANT --enable-headless-only \
        --with-debug-level=release --disable-warnings-as-errors --with-native-debug-symbols=none \
        --with-jobs="$JOBS" --with-version-opt=banana --with-conf-name=$CONF $FEATURES)
    make -C "$SRC" CONF=$CONF images
fi

# ── HotSpot and java.base's native code, for Banana OS ────────────────
W=$OUT/work
N=$REFB/support/native/java.base
mkdir -p "$W" "$OUT/lib/server"
JOBS=$JOBS python3 "$HERE/rebuild.py" "$REFB/hotspot/variant-$VARIANT/libjvm/objs" "$W/hotspot-$VARIANT"
for l in libfdlibm libjava libnio libnet libjimage libverify libzip; do
    extra=
    [ $l = libzip ] && extra=-I$SRC/src/java.base/share/native/libzip/zlib
    [ $l = libjimage ] && extra="-fno-exceptions -fno-rtti"       # (the SDK has no C++ unwinder)
    echo "== $l"
    EXTRA="$extra" JOBS=$JOBS python3 "$HERE/rebuild.py" "$N/$l" "$W/$l"
done
# zlib (bundled with OpenJDK; libzip's own copy)
mkdir -p "$W/zlib"
for c in "$SRC"/src/java.base/share/native/libzip/zlib/*.c; do
    gcc -nostdinc -I"$HERE/../../sdk/include" -isystem "$(gcc -print-file-name=include)" -D__BANANA_OS__ \
        -mno-red-zone -fno-stack-protector -O2 -fPIC -fvisibility=hidden -DHAVE_UNISTD_H \
        -c "$c" -o "$W/zlib/$(basename "$c" .c).o"
done
rm -f "$W/libfdlibm.a"
ar rcs "$W/libfdlibm.a" "$W"/libfdlibm/objs/*.o

LIBGCC=$(gcc -print-libgcc-file-name)
L=$OUT/lib
# libjvm: crtbeginS/crtendS give it its own __dso_handle (C++ statics' destructors)
g++ -shared -nostdlib -Wl,-soname,libjvm.so \
    -Wl,--version-script="$REFB/hotspot/variant-$VARIANT/libjvm/mapfile" -o "$L/server/libjvm.so" \
    "$(gcc -print-file-name=crtbeginS.o)" "$W"/hotspot-$VARIANT/objs/*.o /usr/lib/x86_64-linux-gnu/libffi.a "$LIBGCC" \
    "$(gcc -print-file-name=crtendS.o)"
so() {   # so NAME objects/libraries...
    n=$1; shift
    gcc -shared -nostdlib -Wl,-soname,$n.so -Wl,--exclude-libs,ALL -o "$L/$n.so" "$@" -L"$L" -L"$L/server" "$LIBGCC"
}
so libjava "$W"/libjava/objs/*.o "$W/libfdlibm.a" -ljvm
so libnet "$W"/libnet/objs/*.o -ljvm -ljava
so libnio "$W"/libnio/objs/*.o -ljava -lnet
so libjimage "$W"/libjimage/objs/*.o -ljvm
so libverify "$W"/libverify/objs/*.o -ljvm
so libzip "$W"/libzip/objs/*.o "$W"/zlib/*.o -ljvm -ljava
# java.management's (the JVM's threads, memory and GC for java.lang.management):
# kept out of the image if it does not build yet - only those classes fail then
echo "== libmanagement"
if JOBS=$JOBS python3 "$HERE/rebuild.py" "$REFB/support/native/java.management/libmanagement" "$W/libmanagement"; then
    so libmanagement "$W"/libmanagement/objs/*.o -ljvm -ljava
else
    echo "warning: libmanagement did not build (logs: $W/libmanagement/logs) - left out"
fi
# java.desktop's, headless (no X11, no printing): images - ImageIO, BufferedImage
# and drawing into one (Minecraft up to 1.12 loads its textures that way).
# libawt is built HEADLESS (it is that or X11's here); in libawt_headless the
# X11, GLX, CUPS and fontconfig parts do not compile without their headers and
# are left out (rebuild.py reports them) - none of them is used headless
D=$REFB/support/native/java.desktop
for l in libmlib_image libawt libawt_headless liblcms libjavajpeg; do
    extra=
    [ $l = libawt ] && extra=-DHEADLESS
    echo "== $l"
    EXTRA="$extra" JOBS=$JOBS python3 "$HERE/rebuild.py" "$D/$l" "$W/$l" || true
done
so libmlib_image "$W"/libmlib_image/objs/*.o -ljava -ljvm
so libawt "$W"/libawt/objs/*.o -ljvm -ljava
so libawt_headless "$W"/libawt_headless/objs/*.o -lawt -ljvm -ljava
so liblcms "$W"/liblcms/objs/*.o -lawt -ljvm -ljava
so libjavajpeg "$W"/libjavajpeg/objs/*.o -ljava -ljvm

# ── the image ─────────────────────────────────────────────────────────
# java.base; jdk.unsupported (sun.misc.Unsafe: LWJGL); and for Minecraft:
# java.logging, java.management (its MXBeans), java.naming + jdk.naming.dns
# (server address lookups), java.desktop (javax.sound.sampled.AudioFormat
# for its sounds; and, headless, ImageIO and BufferedImage - their native code built above),
# jdk.zipfs (it opens its own jar as a file system: 1.16+), java.scripting
# (log4j 2.8's ScriptManager: 1.13-1.17) and java.sql (gson 2.8's dates).
# Checked with Minecraft 1.13 to 1.20.4 on a desktop JDK jlinked with these.
# jdk.crypto.ec: elliptic curves for TLS (ECDHE) - without it Java's HTTPS
# fails its handshake with most servers (pure Java since JDK 16).
MODULES=java.base,jdk.unsupported,java.logging,java.management,java.naming,jdk.naming.dns,java.desktop
MODULES=$MODULES,jdk.zipfs,java.scripting,java.sql,jdk.crypto.ec
rm -rf "$OUT/jdk"
"$REFB/images/jdk/bin/jlink" --add-modules $MODULES --strip-debug --no-header-files --no-man-pages --output "$OUT/jdk"
rm -rf "$OUT/jdk/bin" "$OUT/jdk/lib/jexec" "$OUT/jdk/lib/jspawnhelper"
# the Linux builds of the native libraries are of no use here (libawt...):
# only the ones built above for Banana OS go in
find "$OUT/jdk/lib" -name '*.so' -delete
cp "$L"/*.so "$OUT/jdk/lib/"
cp "$L/server/libjvm.so" "$OUT/jdk/lib/server/"

# ── the java app and a test disk ──────────────────────────────────────
make -C "$HERE/launcher"
S=$OUT/disk
rm -rf "$S" && mkdir -p "$S"
cp -r "$OUT/jdk" "$S/jdk"
cp "$HERE/launcher/java.bpk" "$S/"
"$BOOT/bin/javac" --release 17 -d "$S" "$HERE"/test/*.java
IMG=$OUT/jdkdisk.img
rm -f "$IMG"
truncate -s 256M "$IMG"
printf 'label: dos\nstart=2048, type=c\n' | /sbin/sfdisk -q "$IMG"
mformat -i "$IMG@@1M" -F -v JDK ::
mcopy -s -i "$IMG@@1M" "$S"/* ::/
echo "OpenJDK for Banana OS: $OUT/jdk ($(du -sh "$OUT/jdk" | cut -f1)), test disk $IMG"
