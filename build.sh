#!/usr/bin/env bash
# Banana OS 0.6 build script
# Run this on Ubuntu/Debian to build the bootable ISO

set -e

echo "🍌  Banana OS 0.6 build script"
echo "========================="

# ── Check dependencies ────────────────────────────────────────────
MISSING=()
for tool in nasm gcc ld objcopy xorriso mformat python3; do
    if ! command -v "$tool" &>/dev/null; then
        MISSING+=("$tool")
    fi
done

if [ ${#MISSING[@]} -ne 0 ]; then
    echo "Installing missing dependencies: ${MISSING[*]}"
    sudo apt-get update -qq
    sudo apt-get install -y nasm gcc binutils xorriso mtools python3
fi

# ── gcc multilib check ────────────────────────────────────────────
if ! echo 'int main(){}' | gcc -m32 -x c - -o /dev/null 2>/dev/null; then
    echo "Installing gcc multilib (for -m32 support)..."
    sudo apt-get install -y gcc-multilib
fi

echo ""
echo "✅  All dependencies ready. Building..."
echo ""

make clean 2>/dev/null || true
make

echo ""
echo "🎉  Build complete!  →  Banana_OS.iso"
echo ""
echo "VirtualBox setup:"
echo "  1. New VM  →  Type: Other, Version: Other/Unknown (64-bit)"
echo "  2. RAM: 32 MB minimum (256 MB recommended)"
echo "  3. Network: NAT, Intel PRO/1000 MT Desktop (optional); no hard disk needed"
echo "     (optional) System → Motherboard → Enable EFI to boot through UEFI"
echo "  4. Settings → Storage → add Banana_OS.iso as optical drive"
echo "  5. Boot!"
echo ""
echo "QEMU quick test:"
echo "  make run       (BIOS, 64-bit kernel - QEMU with e1000 networking, serial console here)"
echo "  make run-uefi  (the same ISO on UEFI firmware; needs: apt install ovmf)"
echo "  make run-32    (a 32-bit CPU: the boot menu picks the 32-bit kernel)"
