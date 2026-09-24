#!/usr/bin/env bash
# Banana OS 0.5 build script
# Run this on Ubuntu/Debian to build the bootable ISO

set -e

echo "🍌  Banana OS 0.5 build script"
echo "========================="

# ── Check dependencies ────────────────────────────────────────────
MISSING=()
for tool in nasm gcc ld grub-mkrescue xorriso mformat; do
    if ! command -v "$tool" &>/dev/null; then
        MISSING+=("$tool")
    fi
done

if [ ${#MISSING[@]} -ne 0 ]; then
    echo "Installing missing dependencies: ${MISSING[*]}"
    sudo apt-get update -qq
    sudo apt-get install -y nasm gcc grub-pc-bin grub-common xorriso mtools
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
echo "  1. New VM  →  Type: Other, Version: Other/Unknown (32-bit)"
echo "  2. RAM: 32 MB minimum (256 MB recommended)"
echo "  3. Network: NAT, Intel PRO/1000 MT Desktop (optional); no hard disk needed"
echo "  4. Settings → Storage → add Banana_OS.iso as optical drive"
echo "  5. Boot!"
echo ""
echo "QEMU quick test:"
echo "  make run     (qemu-system-i386 -cdrom Banana_OS.iso -m 256 -nic user,model=e1000 -serial stdio)"
