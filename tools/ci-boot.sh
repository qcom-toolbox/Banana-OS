#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Boot the already-built ISO; never rebuild a different artifact in validation.
set -euo pipefail
cd "$(dirname "$0")/.."

firmware="${1:?usage: bash tools/ci-boot.sh bios|uefi}"
logs="build/logs/boot-$firmware"
mkdir -p "$logs"
memory=512M
if [ "${MESA_ISO:-0}" = 1 ]; then memory=1G; fi
args=(-accel tcg -cdrom Banana_OS.iso -boot d -m "$memory" -smp 4 -nic none)
case "$firmware" in
    bios) ;;
    uefi)
        # Ubuntu 24.04 / Debian ovmf: separate read-only code and writable vars.
        code="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
        vars="${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}"
        test -r "$code"
        cp "$vars" "$logs/OVMF_VARS.fd"
        args+=(-drive "if=pflash,format=raw,readonly=on,file=$code"
               -drive "if=pflash,format=raw,file=$logs/OVMF_VARS.fd")
        ;;
    *) echo "Unknown firmware: $firmware" >&2; exit 2 ;;
esac

{
    printf 'commit=%s\nfirmware=%s\n' "$(git rev-parse HEAD)" "$firmware"
    sha256sum Banana_OS.iso
    qemu-system-x86_64 --version
    printf 'QEMU arguments:'
    printf ' %q' "${args[@]}"
    printf '\n'
} > "$logs/provenance.txt"

mesa_checks=()
if [ "${MESA_ISO:-0}" = 1 ]; then
    mesa_checks=(--cmd 'glinfo ## glinfo: every check passed'
                 --cmd 'gpu3d on'
                 --cmd 'glinfo ## glinfo: every check passed'
                 --cmd 'gpu3d off'
                 --cmd 'readlink /lib/libOSMesa.so ## /apps/mesa/libOSMesa.so'
                 --cmd 'pkg remove mesa ## removed Mesa Graphics Runtime'
                 --cmd 'test ! -L /lib/libOSMesa.so && echo mesa-link-removed ## mesa-link-removed'
                 --cmd 'apt install mesa'
                 --cmd 'glinfo ## glinfo: every check passed')
fi

# Reaching a prompt and completing commands catches hangs that a timed boot
# with nonempty serial output would otherwise treat as success. No Internet.
python3 tools/serialtest.py --log "$logs/serial.log" \
    --boot-timeout 180 --timeout 180 \
    "${mesa_checks[@]}" \
    --cmd 'laptop ## ACPI AML: ready' \
    --cmd 'usbc ## no ready ACPI UCSI interface' \
    --cmd 'wifi ## no supported radio detected' \
    --cmd 'cat /usr/share/licenses/rtl8811cu/NOTICES ## Realtek Corporation' \
    --cmd 'cat /usr/share/licenses/rtl8188eu/NOTICES ## Copyright (c) 2010, Realtek' \
    --cmd 'cat /usr/share/licenses/mt7601u/NOTICES ## Copyright (c) 2013, Ralink' \
    --cmd 'cat /usr/share/licenses/igc/NOTICES ## Copyright 2021 Intel Corp' \
    --cmd 'cat /usr/share/licenses/ar9380/NOTICES ## Copyright (c) 2010 Atheros' \
    --cmd 'cat /usr/share/licenses/ar9280/NOTICES ## Copyright (c) 2008-2009 Atheros' \
    --cmd 'cat /usr/share/licenses/ax88179a/NOTICES ## Copyright (c) 2022    ASIX' \
    --cmd 'cat /usr/share/licenses/aqc107/NOTICES ## Copyright (C) 2014-2017 aQuantia' \
    --cmd 'cat /usr/share/licenses/r8125/NOTICES ## Copyright (c) 2026 Realtek' \
    --cmd 'selftest boot ## 0 failed' \
    --cmd 'browser --engine ## browser engine: LibJS' \
    --cmd 'selftest browser libjsdom ## 30 passed, 0 failed' \
    --cmd 'pkg remove libjsworker ## integrated OS component' \
    --cmd 'selftest browser isolation ## 0 failed' \
    --cmd 'selftest ram ## 0 failed' \
    --cmd 'pkg install /home/banana/Examples/shelltest.bpk' \
    --cmd 'apt install neovim' \
    --cmd 'apt install git' \
    --cmd 'cd /tmp && git init -q gitci && cd gitci && echo a > a && git add a && git -c user.name=ci -c user.email=ci@ci.invalid commit -qm gitci && git log --oneline ## gitci' \
    --cmd 'cd /tmp && git clone -q gitci gitci2 && git -C gitci2 log --oneline ## gitci' \
    --cmd 'pkg remove vi ## removed vi (Neatvi)' \
    --cmd 'test -x /bin/vim && test ! -e /bin/vi && echo separate-editors ## separate-editors' \
    --cmd 'apt install vi' \
    --cmd 'shelltest -e ## shelltest: all passed (0 failed)' -- "${args[@]}" \
    2>&1 | tee "$logs/results.log"
