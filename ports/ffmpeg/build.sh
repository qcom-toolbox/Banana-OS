#!/bin/sh
# Builds FFmpeg (libavformat, libavcodec, libswscale, libswresample,
# libavutil) as static libraries for Banana OS apps:
#     ports/ffmpeg/build.sh <i686|x86_64> <output dir> [full|audio]
# (audio: only what music needs - sound decoders, and JPEG for covers)
# The source is downloaded once (checked against its SHA-256) into
# ports/ffmpeg/src. Only decoders, demuxers and parsers are built - no
# network, no devices, no encoders.
set -e
ARCH=$1
OUT=$(realpath -m "$2")
PROFILE=${3:-full}
HERE=$(cd "$(dirname "$0")" && pwd)
SDK=$(cd "$HERE/../../sdk" && pwd)
VER=7.1
SHA=40973d44970dbc83ef302b0609f2e74982be2d85916dd2ee7472d30678a7abe6
SRC=$HERE/src/ffmpeg-$VER

if [ ! -d "$SRC" ]; then
    mkdir -p "$HERE/src"
    T=$HERE/src/ffmpeg-$VER.tar.xz
    [ -f "$T" ] || curl -fsSL -o "$T" https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz
    echo "$SHA  $T" | sha256sum -c -
    tar -C "$HERE/src" -xf "$T"
fi

# Banana OS runs apps in ring 0, where an interrupt is taken on the
# stack in use: nothing below the stack pointer survives (no "red zone").
# Two of FFmpeg's assembly files keep variables there on x86_64 - the
# resampler and H.264 deblocking - so they get real stack space instead.
if ! grep -q "Banana OS: no red zone" "$SRC/libswresample/x86/resample.asm"; then
    R="$SRC/libswresample/x86/resample.asm"
    sed -i 's/^cglobal resample_common_%1, 0, 15, 2, ctx, dst, src,/cglobal resample_common_%1, 0, 15, 2, -0x20, ctx, dst, src,/' "$R"
    sed -i 's/^cglobal resample_linear_%1, 0, 15, 5, ctx, dst, phase_mask,/cglobal resample_linear_%1, 0, 15, 5, -0x20, ctx, dst, phase_mask,/' "$R"
    sed -i 's/\[rsp-0x8\]/[rsp+0x18]/; s/\[rsp-0x10\]/[rsp+0x10]/; s/\[rsp-0x14\]/[rsp+0x0c]/; s/\[rsp-0x18\]/[rsp+0x08]/' "$R"
    sed -i '1i ; Banana OS: no red zone (ports/ffmpeg/build.sh)' "$R"
    H="$SRC/libavcodec/x86/h264_deblock.asm"
    sed -i 's/^cglobal deblock_%1_luma_intra_8, 4,6,16,ARCH_X86_64\*0x50-0x50/cglobal deblock_%1_luma_intra_8, 4,6,16,ARCH_X86_64*0x70-0x50/' "$H"
    sed -i 's/%define mask1q \[rsp-24\]/%define mask1q [rsp+0x10]/' "$H"
fi

case $ARCH in
    # x86_64: with the SIMD assembly of FFmpeg (SSE2..AVX2, chosen at run
    # time); i686: plain C (that assembly is not position-independent)
    x86_64) AFLAGS="-m64 -mno-red-zone -mcmodel=small"; FARCH=x86_64; ASM="" ;;
    i686)   AFLAGS="-m32 -march=i686 -mstackrealign"; FARCH=x86_32
            ASM="--disable-x86asm --disable-runtime-cpudetect" ;;
    *) echo "usage: $0 <i686|x86_64> <output dir>"; exit 1 ;;
esac
GCCINC=$(gcc -print-file-name=include)
CFLAGS="$AFLAGS -ffreestanding -fno-stack-protector -fpie -fvisibility=hidden -nostdinc \
 -fno-asynchronous-unwind-tables -fno-math-errno \
 -I$SDK/include -isystem $GCCINC -D__BANANA_OS__"

VDEC=h264,hevc,mpeg4,msmpeg4v1,msmpeg4v2,msmpeg4v3,h263,mpeg1video,mpeg2video,vp8,vp9,mjpeg,theora,flv,wmv1,wmv2
ADEC=aac,aac_latm,mp3,mp3float,mp2,mp2float,mp1,vorbis,opus,flac,alac,ac3,eac3,wmav1,wmav2,pcm_s16le,pcm_s16be,pcm_u8,pcm_s24le,pcm_s32le,pcm_f32le,pcm_alaw,pcm_mulaw,adpcm_ima_wav,adpcm_ms
DEMUX=mov,matroska,avi,mpegps,mpegts,mpegvideo,ogg,flv,mp3,flac,wav,aac,ac3,h264,hevc,m4v,asf,webm_dash_manifest
PARSE=h264,hevc,mpeg4video,mpegvideo,mpegaudio,aac,aac_latm,vp8,vp9,ac3,flac,vorbis,opus,mjpeg,h263
if [ "$PROFILE" = audio ]; then
    VDEC=mjpeg
    DEMUX=mov,matroska,ogg,mp3,flac,wav,aac,ac3,asf
    PARSE=mpegaudio,aac,aac_latm,ac3,flac,vorbis,opus,mjpeg
fi

mkdir -p "$OUT/sdklibc"
cd "$OUT"
# the SDK libc as an archive: configure links its tests against it (so it
# finds what the libc has), and the app links it in the end anyway
LIBC_SRCS="crt0 libc stdio banana math posix"
[ $ARCH = i686 ] && LIBC_SRCS="$LIBC_SRCS divdi3"
for f in $LIBC_SRCS; do
    gcc $CFLAGS -O2 -fno-builtin -fno-tree-loop-distribute-patterns -c "$SDK/lib/$f.c" -o "sdklibc/$f.o"
done
rm -f sdklibc/libc.a
ar rcs sdklibc/libc.a sdklibc/*.o
# configured again only when this script changes (its checksum, not its
# date: a fresh checkout or a restored CI cache keeps the build)
SUM=$(sha256sum "$HERE/build.sh" | cut -d" " -f1)-$PROFILE
if [ ! -f config.h ] || [ "$(cat .configured 2>/dev/null)" != "$SUM" ]; then
    "$SRC/configure" --enable-cross-compile --target-os=none --arch=$FARCH --cc=gcc --ar=ar \
        --prefix="$OUT/install" \
        --disable-everything --disable-programs --disable-doc --disable-network --disable-avdevice \
        --disable-avfilter --disable-autodetect --disable-pthreads --disable-w32threads \
        --disable-os2threads $ASM --disable-debug --disable-iconv \
        --enable-pic \
        --enable-swscale --enable-swresample --enable-protocol=file \
        --enable-decoder=$VDEC,$ADEC --enable-demuxer=$DEMUX --enable-parser=$PARSE \
        --extra-cflags="$CFLAGS" --extra-ldflags="-nostdlib $AFLAGS" --extra-libs="$OUT/sdklibc/libc.a $(gcc $AFLAGS -print-libgcc-file-name)" >configure.log 2>&1 || { tail -30 configure.log; exit 1; }
    echo "$SUM" > .configured
fi
make -j"$(nproc)" libavformat/libavformat.a libavcodec/libavcodec.a libswscale/libswscale.a \
    libswresample/libswresample.a libavutil/libavutil.a
