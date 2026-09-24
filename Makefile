# Banana OS 0.5 Makefile
# Requires: nasm, gcc-multilib, ld, grub-pc-bin, grub-common, xorriso, mtools

CC      = gcc
CFLAGS  = -m32 -ffreestanding -fno-stack-protector -fno-pic \
          -nostdlib -nostdinc -fno-asynchronous-unwind-tables \
          -Wall -Wextra -O2 \
          -mstackrealign -mno-sse -mno-sse2 -mno-mmx -mno-3dnow \
          -I kernel -I shell -I net -I crypto -I usb -MMD -MP
LDFLAGS = -m elf_i386 -nostdlib -T boot/linker.ld
AS      = nasm
ASFLAGS = -f elf32

# 64-bit division/modulo helpers (__udivdi3 & co.) used by the crypto and
# image-decoding code on 32-bit x86.
LIBGCC := $(shell $(CC) -m32 -print-libgcc-file-name)

C_SRCS   = $(wildcard kernel/*.c) $(wildcard shell/*.c) $(wildcard net/*.c) \
           $(wildcard crypto/*.c) $(wildcard usb/*.c) third_party/stb/stb_image_impl.c
ASM_SRCS = boot/boot.asm kernel/isr.asm kernel/task_switch.asm
# boot.o must come first: it carries the Multiboot2 header
OBJS     = $(ASM_SRCS:.asm=.o) $(C_SRCS:.c=.o)
DEPS     = $(C_SRCS:.c=.d)

# QEMU: an Intel e1000 NIC on user-mode (NAT) networking - Banana OS gets
# 10.0.2.15 by DHCP and real Internet access through the host.
QEMU      = qemu-system-i386
QEMU_BASE = -cdrom Banana_OS.iso -m 256 -serial stdio
QEMU_NET  = -nic user,model=e1000

.PHONY: all clean run run-rtl8139 run-headless run-tap

all: Banana_OS.iso

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.asm
	$(AS) $(ASFLAGS) $< -o $@

# the libc routines themselves must never be "optimized" into calls to
# themselves (GCC turns copy/fill loops into memcpy/memset calls)
kernel/kstring.o: CFLAGS += -fno-builtin -fno-tree-loop-distribute-patterns

# vendored image decoder: only the shim headers in third_party/stb/libc
third_party/stb/stb_image_impl.o: CFLAGS += -I third_party/stb/libc -Wno-unused-function \
    -Wno-sign-compare -Wno-unused-parameter -Wno-implicit-fallthrough -Wno-type-limits

kernel.bin: $(OBJS)
	ld $(LDFLAGS) -o $@ $^ $(LIBGCC)

Banana_OS.iso: kernel.bin iso/boot/grub/grub.cfg
	cp kernel.bin iso/boot/kernel.bin
	grub-mkrescue -o Banana_OS.iso iso

run: Banana_OS.iso
	$(QEMU) $(QEMU_BASE) $(QEMU_NET)

# same, with the Realtek RTL8139 NIC instead of the e1000
run-rtl8139: Banana_OS.iso
	$(QEMU) $(QEMU_BASE) -nic user,model=rtl8139

# no window: the shell runs on this terminal through the serial console
run-headless: Banana_OS.iso
	$(QEMU) $(QEMU_BASE) $(QEMU_NET) -display none

# bridged networking on the host's LAN (needs a tap device + root, see README)
run-tap: Banana_OS.iso
	sudo $(QEMU) $(QEMU_BASE) -netdev tap,id=n0,ifname=tap0,script=no,downscript=no \
	    -device e1000,netdev=n0

clean:
	rm -f $(OBJS) $(DEPS) kernel.bin iso/boot/kernel.bin Banana_OS.iso

-include $(DEPS)
