#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Builds OpenGL 3.3 for Banana OS: Mesa's libOSMesa.so (VirGL when available,
# softpipe on the processor otherwise; 64-bit), which <GL/gl3.h> apps (USE_GL3 = 1)
# load from /lib, /usr/lib, /usr/java/lib (the java package) or /mnt/<disk>/lib.
#     ports/mesa/build.sh [output dir]      (default: ports/mesa/build)
# Out: <output>/libOSMesa.so, gl3test.bpk, glinfo.bpk, gles2.bpk and
# gl3disk.img - a FAT32 disk with lib/libOSMesa.so and the apps for QEMU
# (-drive file=...,if=virtio).
#
# How: Mesa is first built for Linux with the same options (the reference
# build); then every object of libOSMesa.so is compiled again from that
# build's compile_commands.json with the SDK's headers (rebuild.py) and
# the library is linked without the system's C library.
# Needs: gcc, meson, ninja, flex, bison, python3-mako, python3-yaml, mtools,
# sfdisk (in /sbin); with CROCUS=1 or IRIS=1 also libdrm's headers (libdrm-dev);
# with IRIS=1 also LLVM 17, clang, libclc and SPIRV-LLVM-Translator (llvm-17-dev,
# libclang-17-dev, libclang-cpp17-dev, libclc-17-dev, libllvmspirvlib-17-dev,
# spirv-tools, zlib1g-dev, cmake):
# iris's internal shaders are OpenCL, compiled at build time by Mesa's
# intel_clc - built first, on its own, so libOSMesa.so itself needs no LLVM.
set -e
# Non-root SSH sessions on Debian may omit the partitioning tools.
PATH=$PATH:/usr/sbin:/sbin
export PATH
HERE=$(cd "$(dirname "$0")" && pwd)
LLVMPIPE=${LLVMPIPE:-0}
CROCUS=${CROCUS:-0}                      # 1: Mesa's crocus too, for Intel GPUs (gen 4 - 7.5: gpu3d)
IRIS=${IRIS:-0}                          # 1: Mesa's iris too, for Intel GPUs (gen 8 - 11: gpu3d)
DEFAULT_OUT=$HERE/build
[ "$LLVMPIPE" = 0 ] || DEFAULT_OUT=$HERE/build-llvmpipe
[ "$CROCUS" = 0 ] || DEFAULT_OUT=$DEFAULT_OUT-crocus
[ "$IRIS" = 0 ] || DEFAULT_OUT=$DEFAULT_OUT-iris
OUT=$(realpath -m "${1:-$DEFAULT_OUT}")
VER=24.3.4
SHA=e641ae27191d387599219694560d221b7feaa91c900bcec46bf444218ed66025
SRC=${MESA_SRC:-$HERE/src/mesa-$VER}
REF=${MESA_REF:-$OUT/ref}                # (MESA_SRC, MESA_REF: a source / reference build already there)
JOBS=${JOBS:-$(nproc)}
mkdir -p "$HERE/src" "$OUT"

if [ ! -d "$SRC" ]; then
    T=$HERE/src/mesa-$VER.tar.xz
    [ -f "$T" ] || curl -fsSL -o "$T" https://archive.mesa3d.org/mesa-$VER.tar.xz
    echo "$SHA  $T" | sha256sum -c -
    tar -C "$HERE/src" -xJf "$T"
fi

python3 "$HERE/patch.py" "$SRC"

DRIVERS=softpipe,virgl
[ "$CROCUS" = 0 ] || DRIVERS=$DRIVERS,crocus      # (needs libdrm's headers on the build machine)
[ "$IRIS" = 0 ] || DRIVERS=$DRIVERS,iris
LLVM_MODE=disabled
DRAW_LLVM=false
if [ "$LLVMPIPE" != 0 ]; then
    "$HERE/../llvm/build.sh" native
    LLVM_CONFIG="${LLVM_REF:-${LLVM_BUILD:-$HERE/../llvm/build}/ref}/bin/llvm-config"
    export LLVM_CONFIG
    DRIVERS=softpipe,llvmpipe,virgl
    [ "$CROCUS" = 0 ] || DRIVERS=$DRIVERS,crocus
    [ "$IRIS" = 0 ] || DRIVERS=$DRIVERS,iris
    LLVM_MODE=enabled
    DRAW_LLVM=true
fi

CLC_OPT=
if [ "$IRIS" != 0 ]; then
    CLC=$OUT/clc
    if [ ! -x "$CLC/src/intel/compiler/intel_clc" ]; then
        [ -f "$CLC/build.ninja" ] || LLVM_CONFIG=${CLC_LLVM_CONFIG:-/usr/lib/llvm-17/bin/llvm-config} meson setup "$CLC" "$SRC" \
            -Dbuildtype=release -Dgallium-drivers= -Dvulkan-drivers= -Dintel-clc=enabled -Dllvm=enabled -Dshared-llvm=enabled \
            -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dplatforms= -Dgles1=disabled -Dgles2=disabled -Dopengl=false \
            -Dzstd=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dexpat=disabled -Dxmlconfig=disabled -Dbuild-tests=false -Dtools=
        ninja -C "$CLC" -j"$JOBS" src/intel/compiler/intel_clc
    fi
    PATH=$CLC/src/intel/compiler:$PATH
    export PATH
    CLC_OPT=-Dintel-clc=system
fi

# ── the reference Linux build ─────────────────────────────────────────
# a reference build configured otherwise (a cached one from before VirGL or
# LLVMpipe was added) is set up again: else its old drivers would be linked
CONF="drivers=$DRIVERS llvm=$LLVM_MODE draw-llvm=$DRAW_LLVM"
if [ -f "$REF/build.ninja" ] && [ "$(cat "$REF/.banana-conf" 2>/dev/null)" != "$CONF" ]; then
    echo "mesa: the reference build was configured otherwise - setting it up again"
    rm -rf "$REF"
fi
    [ -f "$REF/build.ninja" ] || meson setup "$REF" "$SRC" -Dbuildtype=release -Dosmesa=true $CLC_OPT \
        -Dgallium-drivers="$DRIVERS" -Dc_args="-DBANANA_MESA_OSMESA -include $HERE/banana/host_compat.h" -Dvulkan-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dplatforms= \
        -Dllvm="$LLVM_MODE" -Dcpp_rtti=false -Dgles1=disabled -Dgles2=disabled -Dzstd=disabled -Dvalgrind=disabled \
        -Dlibunwind=disabled -Dshader-cache=disabled -Dlmsensors=disabled -Dgallium-va=disabled \
        -Dgallium-vdpau=disabled -Dgallium-xa=disabled -Dgallium-nine=false -Dgallium-opencl=disabled \
        -Dgallium-rusticl=false -Dandroid-libbacktrace=disabled -Dexpat=disabled -Dxmlconfig=disabled \
        -Ddraw-use-llvm="$DRAW_LLVM" -Dbuild-tests=false -Dtools=
mkdir -p "$REF" && echo "$CONF" > "$REF/.banana-conf"
ninja -C "$REF" -j"$JOBS" src/gallium/targets/osmesa/libOSMesa.so.8.0.0

if [ "$LLVMPIPE" != 0 ]; then
    # Native Mesa determines the exact LLVM symbol closure to cross-compile.
    make -C "$HERE/gl3test"
    "$HERE/../llvm/build.sh" target "$REF/src/gallium/targets/osmesa/libOSMesa.so.8.0.0"
    LLVM_LIBRARY="${LLVM_BUILD:-$HERE/../llvm/build}/libLLVM.so"
    export LLVM_LIBRARY
    cp "$LLVM_LIBRARY" "$OUT/libLLVM.so"
fi

# ── libOSMesa.so for Banana OS ────────────────────────────────────────
KEEP=${KEEP:-1} JOBS=$JOBS python3 "$HERE/rebuild.py" "$REF" "$OUT/banana"
cp "$OUT/banana/libOSMesa.so" "$OUT/libOSMesa.so"
# the Intel drivers must ask banana/drm_shim.c which GPU this is, not
# util/libdrm.h's always-failing stubs (rebuild.py: HAVE_LIBDRM)
if [ "$CROCUS" != 0 ] || [ "$IRIS" != 0 ]; then
    DEVINFO=$(ls "$OUT"/banana/objs/*libintel_dev.a.p_intel_device_info.c.o)
    nm "$DEVINFO" | grep -q ' U drmGetDevice2$' ||
        { echo "intel_device_info.c was built with libdrm stubs: the Intel drivers would refuse every GPU"; exit 1; }
    nm -D "$OUT/libOSMesa.so" | grep -q ' U drmGetDevice2$' &&
        { echo "libOSMesa.so imports drmGetDevice2: banana/drm_shim.c was not linked in"; exit 1; }
fi

# ── the test app and a test disk ──────────────────────────────────────
make -C "$HERE/gl3test"
cp "$HERE/gl3test/gl3test.bpk" "$OUT/"
# EGL and OpenGL ES: glinfo (checks every kind of context) and the gles2 demo
for ex in glinfo gles2; do
    make -C "$HERE/../../sdk/examples/$ex"
    cp "$HERE/../../sdk/examples/$ex/$ex.bpk" "$OUT/"
done
if [ "$LLVMPIPE" != 0 ]; then
    python3 "$HERE/../../tools/check_shared_imports.py" "$OUT/libLLVM.so" "$HERE"/gl3test/build/x86_64/*.o
    python3 "$HERE/../../tools/check_shared_imports.py" "$OUT/libOSMesa.so" "$OUT/libLLVM.so" "$HERE"/gl3test/build/x86_64/*.o
else
    python3 "$HERE/../../tools/check_shared_imports.py" "$OUT/libOSMesa.so" "$HERE"/gl3test/build/x86_64/*.o
fi
IMG=$OUT/gl3disk.img
rm -f "$IMG"
SIZE=64M
[ "$LLVMPIPE" = 0 ] || SIZE=256M
truncate -s "$SIZE" "$IMG"
printf 'label: dos\nstart=2048, type=c\n' | sfdisk -q "$IMG"
mformat -i "$IMG@@1M" -F -v GL3 ::
mmd -i "$IMG@@1M" ::/lib
mcopy -i "$IMG@@1M" "$OUT/libOSMesa.so" ::/lib/
[ "$LLVMPIPE" = 0 ] || mcopy -i "$IMG@@1M" "$OUT/libLLVM.so" ::/lib/
mcopy -i "$IMG@@1M" "$OUT/gl3test.bpk" "$OUT/glinfo.bpk" "$OUT/gles2.bpk" ::/
echo "OpenGL 3.3 for Banana OS: $OUT/libOSMesa.so ($(du -h "$OUT/libOSMesa.so" | cut -f1)), test disk $IMG"
