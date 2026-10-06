# Banana OS 0.5 Makefile
# Requires: nasm, gcc-multilib, ld, python3 (the SDK packager), grub-pc-bin, grub-efi-amd64-bin,
#           grub-common, xorriso, mtools
#
# Builds two kernels from the same sources - kernel.bin (i386, 32-bit) and
# kernel64.bin (x86_64, long mode) - and one ISO that boots on BIOS and
# UEFI machines; its GRUB menu starts the 64-bit kernel when the CPU can.

CC      = gcc
WARN    = -Wall -Wextra
INCS    = -I kernel -I shell -I net -I crypto -I usb -I web
COMMON  = -ffreestanding -fno-stack-protector -fno-pic -nostdlib -nostdinc \
          -fno-asynchronous-unwind-tables $(WARN) -O2 \
          -mno-sse -mno-sse2 -mno-mmx -mno-3dnow $(INCS)

# 32-bit
CFLAGS  = -m32 -mstackrealign $(COMMON) -MMD -MP
LDFLAGS = -m elf_i386 -nostdlib -T boot/linker.ld
# 64-bit: no red zone (interrupts run on the same stack), small code model
# (everything below 2 GiB), no SSE (so interrupt handlers need not save it)
CFLAGS64  = -m64 -mcmodel=small -mno-red-zone -fno-pie $(COMMON) -MMD -MP
LDFLAGS64 = -m elf_x86_64 -nostdlib -z max-page-size=0x1000 -T boot/linker64.ld
AS      = nasm

# 64-bit division/modulo helpers (__udivdi3 & co.) used by the crypto and
# image-decoding code on 32-bit x86.
LIBGCC   := $(shell $(CC) -m32 -print-libgcc-file-name)
LIBGCC64 := $(shell $(CC) -m64 -print-libgcc-file-name)

C_SRCS   = $(wildcard kernel/*.c) $(wildcard shell/*.c) $(wildcard net/*.c) \
           $(wildcard crypto/*.c) $(wildcard usb/*.c) $(wildcard web/*.c) \
           third_party/stb/stb_image_impl.c
ASM_SRCS   = boot/boot.asm kernel/isr.asm kernel/task_switch.asm kernel/appcall.asm kernel/exbin.asm
ASM_SRCS64 = boot/boot64.asm kernel/isr64.asm kernel/task_switch64.asm kernel/appcall64.asm kernel/exbin.asm
# the boot object must come first: it carries the Multiboot2 header
OBJS     = $(ASM_SRCS:.asm=.o) $(C_SRCS:.c=.o)
OBJS64   = $(ASM_SRCS64:.asm=.o64) $(C_SRCS:.c=.o64)
DEPS     = $(C_SRCS:.c=.d) $(C_SRCS:.c=.o64.d)

# QEMU: an Intel e1000 NIC on user-mode (NAT) networking - Banana OS gets
# 10.0.2.15 by DHCP and real Internet access through the host.
# qemu-system-x86_64 runs both kernels (pick 32-bit in the boot menu).
QEMU      = qemu-system-x86_64
QEMU_BASE = -cdrom Banana_OS.iso -m 256 -serial stdio
QEMU_NET  = -nic user,model=e1000
# UEFI firmware for `make run-uefi` (Debian/Ubuntu package: ovmf)
OVMF     ?= $(firstword $(wildcard /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/qemu/OVMF.fd))

.PHONY: all clean run run-uefi run-32 run-rtl8139 run-headless run-tap

all: Banana_OS.iso

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.asm
	$(AS) -f elf32 $< -o $@

%.o64: %.c
	$(CC) $(CFLAGS64) -MF $@.d -c $< -o $@

%.o64: %.asm
	$(AS) -f elf64 $< -o $@

# the libc routines themselves must never be "optimized" into calls to
# themselves (GCC turns copy/fill loops into memcpy/memset calls)
kernel/kstring.o: CFLAGS += -fno-builtin -fno-tree-loop-distribute-patterns
kernel/kstring.o64: CFLAGS64 += -fno-builtin -fno-tree-loop-distribute-patterns

# vendored image decoder: only the shim headers in third_party/stb/libc
STB_FLAGS = -I third_party/stb/libc -Wno-unused-function -Wno-sign-compare \
            -Wno-unused-parameter -Wno-implicit-fallthrough -Wno-type-limits
third_party/stb/stb_image_impl.o: CFLAGS += $(STB_FLAGS)
third_party/stb/stb_image_impl.o64: CFLAGS64 += $(STB_FLAGS)

kernel.bin: $(OBJS)
	ld $(LDFLAGS) -o $@ $^ $(LIBGCC)

kernel64.bin: $(OBJS64)
	ld $(LDFLAGS64) -o $@ $^ $(LIBGCC64)

# grub-mkrescue adds a UEFI boot image when the x86_64-efi GRUB modules
# are installed (grub-efi-amd64-bin) - the ISO then boots BIOS and UEFI
Banana_OS.iso: kernel.bin kernel64.bin iso/boot/grub/grub.cfg
	@test -d /usr/lib/grub/x86_64-efi || echo "warning: grub-efi-amd64-bin missing - the ISO will boot on BIOS only"
	cp kernel.bin iso/boot/kernel.bin
	cp kernel64.bin iso/boot/kernel64.bin
	grub-mkrescue -o Banana_OS.iso iso

run: Banana_OS.iso
	$(QEMU) $(QEMU_BASE) $(QEMU_NET)

# the same ISO on UEFI firmware (OVMF)
run-uefi: Banana_OS.iso
	@test -n "$(OVMF)" || { echo "OVMF not found: apt install ovmf (or make run-uefi OVMF=/path/OVMF.fd)"; exit 1; }
	$(QEMU) -bios $(OVMF) $(QEMU_BASE) $(QEMU_NET)

# a 32-bit-only CPU: the boot menu falls back to the 32-bit kernel
run-32: Banana_OS.iso
	qemu-system-i386 $(QEMU_BASE) $(QEMU_NET)

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
	for e in $(EXAMPLES); do $(MAKE) -s -C sdk/examples/$$e clean; done
	rm -f banana-sdk.tar.gz
	rm -f $(OBJS) $(OBJS64) $(DEPS) kernel.bin kernel64.bin iso/boot/kernel.bin \
	      iso/boot/kernel64.bin Banana_OS.iso


# ── SDK: example apps (embedded in the kernel: ~/Examples) and the tarball ──
EXAMPLES     = hello guess paint clock tones mandel threads webview
EXAMPLE_BPKS = $(foreach e,$(EXAMPLES),sdk/examples/$(e)/$(e).bpk)
SDK_DEPS     = $(wildcard sdk/lib/*.c sdk/include/*.h) sdk/banana.mk sdk/tools/bpkg

define EXAMPLE_RULE
sdk/examples/$(1)/$(1).bpk: $$(wildcard sdk/examples/$(1)/*.c) sdk/examples/$(1)/Makefile $$(SDK_DEPS)
	$$(MAKE) -s -C sdk/examples/$(1)
endef
$(foreach e,$(EXAMPLES),$(eval $(call EXAMPLE_RULE,$(e))))

kernel/exbin.o kernel/exbin.o64: $(EXAMPLE_BPKS)

.PHONY: examples sdk
examples: $(EXAMPLE_BPKS)

# banana-sdk.tar.gz: everything needed to build apps on another Linux machine
sdk: $(EXAMPLE_BPKS)
	tar czf banana-sdk.tar.gz --transform 's,^sdk,banana-sdk,' --exclude=build --exclude='*.bpk' sdk
	@echo "banana-sdk.tar.gz: unpack it anywhere, then see banana-sdk/README.md"
-include $(DEPS)
