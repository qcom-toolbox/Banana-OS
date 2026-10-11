# Banana OS 0.6 Makefile
# Requires: nasm, gcc-multilib, ld, objcopy, python3, xorriso, mtools
#
# Builds two kernels from the same sources - kernel.bin (i386, 32-bit) and
# kernel64.bin (x86_64, long mode) - and one ISO that boots on BIOS and
# UEFI machines, from a CD, a USB stick or a hard disk, with Banana OS's own
# boot loader (loader/: Banana Boot); its menu starts the 64-bit kernel
# when the CPU can.

CC      = gcc
WARN    = -Wall -Wextra
INCS    = -I kernel -I shell -I net -I crypto -I usb -I web -I media
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
           $(wildcard crypto/*.c) $(wildcard usb/*.c) $(wildcard web/*.c) $(wildcard media/*.c) \
           third_party/stb/stb_image_impl.c
ASM_SRCS   = boot/boot.asm kernel/isr.asm kernel/task_switch.asm kernel/appcall.asm kernel/exbin.asm kernel/fontbin.asm kernel/appbin.asm
ASM_SRCS64 = boot/boot64.asm kernel/isr64.asm kernel/task_switch64.asm kernel/appcall64.asm kernel/exbin.asm kernel/fontbin.asm kernel/appbin.asm kernel/smp_tramp.asm
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
# ── Banana Boot (loader/): the boot loader ──────────────────────────────
# the MBR (USB sticks, hard disks)
loader/mbr.bin: loader/mbr.asm
	$(AS) -f bin $< -o $@

# the BIOS loader: 16-bit entries + 32-bit C, one flat file loaded at 0x8000
LOADER_CFLAGS32 = -m32 -march=i686 -Os -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
                  -fno-asynchronous-unwind-tables -mgeneral-regs-only -fno-delete-null-pointer-checks \
                  --param=min-pagesize=0 $(WARN)
loader/bios.bin: loader/bios.asm loader/bios.c loader/common.h loader/bios.ld
	$(AS) -f elf32 loader/bios.asm -o loader/bios_asm.o
	$(CC) $(LOADER_CFLAGS32) -c loader/bios.c -o loader/bios_c.o
	ld -m elf_i386 -T loader/bios.ld -o $@ loader/bios_asm.o loader/bios_c.o

# the UEFI loaders: position-independent C turned into PE programs - for
# 64-bit (BOOTX64.EFI) and 32-bit (BOOTIA32.EFI) firmware. They must have no
# relocations (they run wherever the firmware puts them), must not hold the
# "set banana_medium" line (`install` patches that, and the Secure Boot
# signature covers every byte: it is in EFI/BananaOS/medium.cfg), and they
# hold the SHA-256 of both kernels (under Secure Boot they start no other).
LOADER_CFLAGS_EFI = -O2 -ffreestanding -fpie -fno-stack-protector -fno-stack-check -fshort-wchar \
                    -mgeneral-regs-only -fno-asynchronous-unwind-tables -fvisibility=hidden $(WARN)
EFI_DEPS = loader/efi.c loader/efi.h loader/common.h loader/sha256.h loader/kernel_hashes.h

loader/kernel_hashes.h: kernel.bin kernel64.bin
	{ echo "/* made by the Makefile: the kernels this loader starts under Secure Boot */"; \
	  echo "static const u8 KERNEL32_SHA256[32] = { $$(sha256sum kernel.bin | cut -c1-64 | sed 's/../0x&,/g') };"; \
	  echo "static const u8 KERNEL64_SHA256[32] = { $$(sha256sum kernel64.bin | cut -c1-64 | sed 's/../0x&,/g') };"; } > $@

# Secure Boot: signed with Banana OS's key when it is here (CI: a secret);
# without it the loaders are built unsigned and boot with Secure Boot off
SB_KEY  ?= $(HOME)/.banana-secureboot/banana-sb.key
SB_CERT  = loader/keys/banana-sb.crt
define efi_finish
	python3 tools/efi_relocs.py $(1)
	objcopy -j .text -j .reloc -j .got -j .data -j .dynamic -j .rela -j .rel -j .dynsym --target $(2) --subsystem=10 $(1) $@.unsigned
	@if grep -q "set banana_medium" $@.unsigned; then echo "$@ holds the medium line: install would break its signature"; exit 1; fi
	@if [ -f "$(SB_KEY)" ]; then sbsign --key "$(SB_KEY)" --cert $(SB_CERT) --output $@ $@.unsigned; \
	 else cp $@.unsigned $@; echo "note: no Secure Boot key at $(SB_KEY) - $@ is not signed"; fi
endef

loader/BOOTX64.EFI: $(EFI_DEPS) loader/tramp.S loader/efi.lds
	$(CC) -m64 -mno-red-zone $(LOADER_CFLAGS_EFI) -c loader/efi.c -o loader/efi64.o
	$(CC) -m64 -c loader/tramp.S -o loader/tramp64.o
	ld -m elf_x86_64 -nostdlib -pie --no-dynamic-linker -z nocombreloc -T loader/efi.lds -o loader/bootx64.so loader/efi64.o loader/tramp64.o
	$(call efi_finish,loader/bootx64.so,efi-app-x86_64)

loader/BOOTIA32.EFI: $(EFI_DEPS) loader/tramp32.S loader/efi32.lds
	$(CC) -m32 -malign-double $(LOADER_CFLAGS_EFI) -c loader/efi.c -o loader/efi32.o
	$(CC) -m32 -c loader/tramp32.S -o loader/tramp32.o
	ld -m elf_i386 -nostdlib -pie --no-dynamic-linker -z nocombreloc -T loader/efi32.lds -o loader/bootia32.so loader/efi32.o loader/tramp32.o
	$(call efi_finish,loader/bootia32.so,efi-app-ia32)

# Banana OS's keys as signed UEFI variables (efitools), for the menu's
# "Enroll" in Secure Boot's setup mode: db and KEK appended to, then PK
SB_OWNER = 9a5f3e1c-62b4-4d2a-b6f1-ba5a5a0b0057
loader/keys/PK.auth: $(SB_CERT)
	@if [ -f "$(SB_KEY)" ]; then \
	    cert-to-efi-sig-list -g $(SB_OWNER) $(SB_CERT) loader/keys/banana.esl && \
	    sign-efi-sig-list -a -k "$(SB_KEY)" -c $(SB_CERT) db  loader/keys/banana.esl loader/keys/db.auth && \
	    sign-efi-sig-list -a -k "$(SB_KEY)" -c $(SB_CERT) KEK loader/keys/banana.esl loader/keys/KEK.auth && \
	    sign-efi-sig-list    -k "$(SB_KEY)" -c $(SB_CERT) PK  loader/keys/banana.esl $@; \
	 else rm -f $@; touch loader/keys/.nokey; fi

# the EFI system partition: a FAT image with both UEFI loaders, the kernels,
# the medium line, Banana OS's certificate (to enroll it by hand in the
# firmware's settings) and its signed keys (to enroll from the menu)
isoroot/efi.img: loader/BOOTX64.EFI loader/BOOTIA32.EFI kernel.bin kernel64.bin loader/keys/PK.auth loader/keys/BananaOS.cer
	mkdir -p isoroot
	rm -f $@
	printf 'set banana_medium=live-cd\nset banana_video=auto        \n' > isoroot/medium.cfg
	kb=$$(( ($$(stat -c %s kernel.bin) + $$(stat -c %s kernel64.bin) + 2 * $$(stat -c %s loader/BOOTX64.EFI)) / 1024 + 2048 )); \
	    dd if=/dev/zero of=$@ bs=1024 count=$$kb status=none
	mformat -i $@ -v BANANA_EFI ::
	mmd -i $@ ::/EFI ::/EFI/BOOT ::/EFI/BananaOS ::/boot
	mcopy -i $@ loader/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
	mcopy -i $@ loader/BOOTIA32.EFI ::/EFI/BOOT/BOOTIA32.EFI
	mcopy -i $@ isoroot/medium.cfg loader/keys/BananaOS.cer ::/EFI/BananaOS/
	if [ -f loader/keys/PK.auth ]; then mcopy -i $@ loader/keys/PK.auth loader/keys/KEK.auth loader/keys/db.auth ::/EFI/BananaOS/; fi
	mcopy -i $@ kernel.bin kernel64.bin ::/boot/

# the image: an ISO9660 CD (El Torito: the BIOS loader, and the EFI
# partition for UEFI) that tools/mkimage.py makes a disk too (MBR)
Banana_OS.iso: kernel.bin kernel64.bin loader/mbr.bin loader/bios.bin isoroot/efi.img tools/mkimage.py
	mkdir -p isoroot/boot
	cp kernel.bin kernel64.bin loader/bios.bin isoroot/boot/
	xorriso -as mkisofs -quiet -o $@ -R -J -V BANANA_OS \
	    -b boot/bios.bin -no-emul-boot -boot-load-size 4 -boot-info-table \
	    -eltorito-alt-boot -e efi.img -no-emul-boot isoroot
	python3 tools/mkimage.py $@ loader/mbr.bin

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
	for a in $(APPS); do $(MAKE) -s -C apps/$$a clean; done
	for d in $(DRIVER_EXAMPLES); do $(MAKE) -s -C sdk/driver/examples/$$d clean; done
	rm -f banana-sdk.tar.gz
	rm -f $(OBJS) $(OBJS64) $(DEPS) kernel.bin kernel64.bin Banana_OS.iso
	rm -rf isoroot
	rm -f loader/*.o loader/*.so loader/*.bin loader/*.EFI loader/*.unsigned loader/kernel_hashes.h \
	      loader/keys/*.esl loader/keys/*.auth loader/keys/.nokey


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

# ── the apps that come with Banana OS (apps/): FFmpeg-based, built into the
# kernel compressed and installed at boot (kernel/builtin_apps.c). FFmpeg
# itself is built once by ports/ffmpeg/build.sh (the first build takes a while).
APPS     = mediaplayer music amethyst photos code store
APP_DEPS = $(SDK_DEPS) ports/ffmpeg/build.sh
define APP_RULE
apps/$(1)/$(1)-i686.bpk.z apps/$(1)/$(1)-x86_64.bpk.z: $$(wildcard apps/$(1)/*.c) apps/$(1)/Makefile $$(APP_DEPS)
	$$(MAKE) -C apps/$(1) bundle
endef
$(foreach a,$(APPS),$(eval $(call APP_RULE,$(a))))
kernel/appbin.o: $(foreach a,$(APPS),apps/$(a)/$(a)-i686.bpk.z)
kernel/appbin.o64: $(foreach a,$(APPS),apps/$(a)/$(a)-x86_64.bpk.z)

# the Driver Kit's examples (sdk/driver/examples): packages, not in the kernel
DRIVER_EXAMPLES = edu bochsfb
.PHONY: driver-examples
driver-examples:
	for d in $(DRIVER_EXAMPLES); do $(MAKE) -C sdk/driver/examples/$$d; done
kernel/fontbin.o kernel/fontbin.o64: $(wildcard fonts/*.ttf)

.PHONY: examples sdk
examples: $(EXAMPLE_BPKS)

# banana-sdk.tar.gz: everything needed to build apps on another Linux machine
sdk: $(EXAMPLE_BPKS)
	tar czf banana-sdk.tar.gz --transform 's,^sdk,banana-sdk,' --exclude=build --exclude='*.bpk' sdk
	@echo "banana-sdk.tar.gz: unpack it anywhere, then see banana-sdk/README.md"
-include $(DEPS)
