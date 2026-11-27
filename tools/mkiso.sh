#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# mkiso.sh OUT.iso KERNELS [EXTRAS_DIR] - one Banana OS image (BIOS and UEFI,
# CD and USB stick / disk) with the kernels named in KERNELS ("32", "64" or
# "32 64": /boot/kernel.bin, /boot/kernel64.bin - the boot menu starts one
# that is there), the licences and notices in /LICENSES, and the packages of
# EXTRAS_DIR in /EXTRAS (the all-in-one image: installed from there,
# kernel/builtin_apps.c). Run by the Makefile, after the kernels and the
# loaders are built.
set -eu
out=$1
kernels=$2
extras=${3:-}
root=build/iso-$(basename "$out" .iso)
rm -rf "$root"
mkdir -p "$root/boot" "$root/LICENSES"

files=""
for k in $kernels; do
    case $k in
        32) cp kernel.bin "$root/boot/"; files="$files boot/kernel.bin" ;;
        64) cp kernel64.bin "$root/boot/"; files="$files boot/kernel64.bin" ;;
        *) echo "mkiso: unknown kernel $k (32 or 64)" >&2; exit 2 ;;
    esac
done
[ -n "$files" ] || { echo "mkiso: no kernel" >&2; exit 2; }
cp loader/bios.bin "$root/boot/"

# the EFI system partition: both UEFI loaders, the same kernels, the medium
# line, Banana OS's certificate and (when signed) its keys
printf 'set banana_medium=live-cd\nset banana_video=auto        \n' > "$root/medium.cfg"
ksize=0
for f in $files; do ksize=$((ksize + $(stat -c %s "$root/$f"))); done
kb=$(( (ksize + 2 * $(stat -c %s loader/BOOTX64.EFI)) / 1024 + 2048 ))
dd if=/dev/zero of="$root/efi.img" bs=1024 count=$kb status=none
python3 tools/format_efi.py "$root/efi.img"
mmd -i "$root/efi.img" ::/EFI ::/EFI/BOOT ::/EFI/BananaOS ::/boot
mcopy -i "$root/efi.img" loader/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$root/efi.img" loader/BOOTIA32.EFI ::/EFI/BOOT/BOOTIA32.EFI
mcopy -i "$root/efi.img" "$root/medium.cfg" loader/keys/BananaOS.cer ::/EFI/BananaOS/
if [ -f loader/keys/PK.auth ]; then
    mcopy -i "$root/efi.img" loader/keys/PK.auth loader/keys/KEK.auth loader/keys/db.auth ::/EFI/BananaOS/
fi
for f in $files; do mcopy -i "$root/efi.img" "$root/$f" ::/boot/; done
rm "$root/medium.cfg"

# what the image is under: Banana OS's licence, the SDK's, the notices of
# every component, and (CI) where the source code of each one is
cp LICENSE "$root/LICENSES/COPYING"
cp LICENSES/* COPYRIGHT THIRD-PARTY-NOTICES "$root/LICENSES/"
b=${BANANA_BUILD:-dev}
{
    echo "Banana OS build $b - where the source code of everything on this image is"
    echo
    if [ "$b" = dev ]; then
        echo "This image was built from a working copy of https://github.com/qcom-toolbox/Banana-OS;"
        echo "its ports/<name>/build.sh scripts fetch each program's upstream source."
    else
        echo "Banana_OS-sources.tar, next to this image in the release:"
        echo "  https://github.com/qcom-toolbox/Banana-OS/releases/tag/build-$b"
        echo "has the upstream source archives of every program on it (as the build"
        echo "scripts fetched them, with their SHA-256 in its SOURCES.txt) and the"
        echo "Banana OS repository at commit ${BANANA_COMMIT:-} (its scripts, patches and"
        echo "Banana OS itself)."
    fi
    echo
    echo "The licences are in this folder; THIRD-PARTY-NOTICES says which applies to what."
} > "$root/LICENSES/SOURCES.txt"

if [ -n "$extras" ]; then
    mkdir -p "$root/EXTRAS"
    for f in "$extras"/*; do
        [ -f "$f" ] || continue
        b=$(basename "$f" | tr 'a-z' 'A-Z')
        cp "$f" "$root/EXTRAS/$b"
        files="$files EXTRAS/$b"
    done
fi

printf '%s %s\n' "${BANANA_BUILD:-dev}" "${BANANA_COMMIT:-unknown}" > "$root/BUILD.TXT"
# (not bios.bin: xorriso writes its boot info table into it)
python3 tools/iso_sums.py "$root" $files efi.img
xorriso -as mkisofs -quiet -o "$out" -R -J -V BANANA_OS \
    -b boot/bios.bin -no-emul-boot -boot-load-size 4 -boot-info-table \
    -eltorito-alt-boot -e efi.img -no-emul-boot "$root"
python3 tools/mkimage.py "$out" loader/mbr.bin
