#!/bin/sh
# lprun.sh SCRIPT LOG [qemu args...]: the Minecraft / LLVMpipe disk, 64-bit QEMU, 4 CPUs
S=$1; LOG=$2; shift 2
cp ${ISO:-$HOME/Banana-OS/Banana_OS.iso} /tmp/lp.iso && rm -f /tmp/lpqmp.sock
cd ~/bos-tests && exec timeout 7200 python3 vmtest.py --log $LOG --script $S --qmp /tmp/lpqmp.sock --timeout 900 -- -cdrom /tmp/lp.iso -cpu max -m 2G -smp 2 -nic user,model=e1000 -drive file=/home/banana/bos-tests/mc.img,format=raw,if=virtio -boot d -qmp unix:/tmp/lpqmp.sock,server=on,wait=off "$@"
