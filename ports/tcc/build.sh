#!/bin/sh
# TinyCC (LGPL 2.1) for Banana OS: the C compiler inside Banana Code.
#     ports/tcc/build.sh <output dir>
# The source is downloaded once (a fixed commit, checked against its
# SHA-256) into ports/tcc/src, then:
#   src/host     the cross compilers for this build machine (i386-tcc,
#                x86_64-tcc) and their runtime libraries (libtcc1.a)
#   src/banana   the same source set up to run on Banana OS (config.h
#                below); apps/code compiles its libtcc.c into the IDE
#   <out>/<cpu>/libbanana.a   the SDK's C library built by TinyCC
#   <out>/<cpu>/libtcc1.a     TinyCC's helpers (64-bit division, alloca...)
set -e
OUT=$(realpath -m "$1")
HERE=$(cd "$(dirname "$0")" && pwd)
SDK=$(cd "$HERE/../../sdk" && pwd)
REV=43c7708b85681a2fd4451c8a541af4494a8919b2
SHA=8067daee0159f1259463513dac5013fc44c0c631741b60f225c4d52b2e9b8e39
T=$HERE/src/tinycc-$REV.tar.gz

if [ ! -d "$HERE/src/host" ]; then
    mkdir -p "$HERE/src"
    [ -f "$T" ] || curl -fsSL -o "$T" https://github.com/TinyCC/tinycc/archive/$REV.tar.gz
    echo "$SHA  $T" | sha256sum -c -
    rm -rf "$HERE/src/host" "$HERE/src/tinycc-$REV"
    tar -C "$HERE/src" -xzf "$T"
    mv "$HERE/src/tinycc-$REV" "$HERE/src/host"
fi

# the cross compilers (and tccdefs_.h, which both builds embed)
if [ ! -x "$HERE/src/host/x86_64-tcc" ] || [ ! -x "$HERE/src/host/i386-tcc" ]; then
    (cd "$HERE/src/host" && ./configure >/dev/null && make -s cross-x86_64 cross-i386)
fi

# the copy that runs on Banana OS: no -run (it would need mmap), no threads
# lock, no bounds checker; its files are in /apps/code/tcc
rm -rf "$HERE/src/banana"
mkdir -p "$HERE/src/banana"
cp "$HERE"/src/host/*.c "$HERE"/src/host/*.h "$HERE"/src/host/*.def "$HERE/src/banana/"
cp -r "$HERE/src/host/include" "$HERE/src/banana/"
sed -i 's/^#if defined _WIN32 == defined TCC_TARGET_PE/#if !defined CONFIG_TCC_NO_NATIVE \&\& defined _WIN32 == defined TCC_TARGET_PE/' "$HERE/src/banana/tcc.h"
grep -q CONFIG_TCC_NO_NATIVE "$HERE/src/banana/tcc.h"
cp "$HERE/config.h" "$HERE/src/banana/config.h"

# the runtime libraries, per CPU
for CPU in x86_64 i386; do
    TCC=$HERE/src/host/$CPU-tcc
    D=$OUT/$CPU
    mkdir -p "$D/obj"
    for f in crt0 libc stdio banana math posix setjmp; do
        "$TCC" -nostdinc -I"$SDK/include" -D__BANANA_OS__ -c "$SDK/lib/$f.c" -o "$D/obj/$f.o" 2>&1 | grep -v "warning:" || true
        [ -f "$D/obj/$f.o" ]
    done
    rm -f "$D/libbanana.a"
    "$TCC" -ar rcs "$D/libbanana.a" "$D"/obj/*.o
    cp "$HERE/src/host/$CPU-libtcc1.a" "$D/libtcc1.a"
    rm -rf "$D/obj"
done
echo "tcc: built into $OUT"
