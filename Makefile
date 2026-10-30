# SPDX-License-Identifier: GPL-3.0-or-later
# Banana OS 0.6 Makefile
# Requires: nasm, gcc-multilib, ld, objcopy, python3, xorriso, mtools
#
# Builds two kernels from the same sources - kernel.bin (i386, 32-bit) and
# kernel64.bin (x86_64, long mode) - and one ISO that boots on BIOS and
# UEFI machines, from a CD, a USB stick or a hard disk, with Banana OS's own
# boot loader (loader/: Banana Boot); its menu starts the 64-bit kernel
# when the CPU can.

CC      = gcc
.DEFAULT_GOAL := all
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

WEBP_SRCS = $(wildcard third_party/libwebp/src/dec/*.c third_party/libwebp/src/dsp/*.c third_party/libwebp/src/utils/*.c)

C_SRCS   = $(wildcard kernel/*.c) $(wildcard shell/*.c) $(wildcard net/*.c) \
           $(wildcard crypto/*.c) $(wildcard usb/*.c) $(wildcard web/*.c) $(wildcard media/*.c) \
           third_party/stb/stb_image_impl.c $(wildcard third_party/igc/*.c) $(wildcard third_party/rge/*.c) $(wildcard third_party/uacpi/source/*.c)
ASM_SRCS   = boot/boot.asm kernel/isr.asm kernel/task_switch.asm kernel/appcall.asm kernel/exbin.asm kernel/frameworkbin.asm kernel/fontbin.asm kernel/appbin.asm kernel/s3_resume.asm
ASM_SRCS64 = boot/boot64.asm kernel/isr64.asm kernel/task_switch64.asm kernel/appcall64.asm kernel/exbin.asm kernel/frameworkbin.asm kernel/fontbin.asm kernel/landscapebin.asm kernel/appbin.asm kernel/smp_tramp.asm kernel/s3_resume.asm
kernel/landscapebin.o kernel/landscapebin.o64: $(wildcard assets/wallpapers/landscapes/*.jpg)

kernel/frameworkbin.o kernel/frameworkbin.o64: $(wildcard web/fixtures/* web/fixtures/astro/* web/fixtures/astro/_astro/*) $(wildcard web/test/cases/mutation_observer*.html)

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

# Portable AML interpreter, using native kernel callbacks and freestanding types.
UACPI_FLAGS = -I third_party/uacpi/include -I third_party/uacpi/platform \
              -DUACPI_OVERRIDE_TYPES -DUACPI_USE_BUILTIN_STRING \
              -DUACPI_DEFAULT_LOOP_TIMEOUT_SECONDS=2 -DUACPI_DEFAULT_MAX_CALL_STACK_DEPTH=64
$(patsubst %.c,%.o,$(wildcard third_party/uacpi/source/*.c)) kernel/aml_host.o kernel/laptop.o: CFLAGS += $(UACPI_FLAGS)
$(patsubst %.c,%.o64,$(wildcard third_party/uacpi/source/*.c)) kernel/aml_host.o64 kernel/laptop.o64: CFLAGS64 += $(UACPI_FLAGS)

# the libc routines themselves must never be "optimized" into calls to
# themselves (GCC turns copy/fill loops into memcpy/memset calls)
kernel/kstring.o: CFLAGS += -fno-builtin -fno-tree-loop-distribute-patterns
kernel/kstring.o64: CFLAGS64 += -fno-builtin -fno-tree-loop-distribute-patterns

# Decoder-only libwebp: scalar C, no host libc, SIMD or OS threads.
WEBP_FLAGS = -I third_party/libwebp -I third_party/libwebp/libc -DHAVE_CONFIG_H -DNDEBUG
$(WEBP_SRCS:.c=.o): CFLAGS += $(WEBP_FLAGS)
$(WEBP_SRCS:.c=.o64): CFLAGS64 += $(WEBP_FLAGS)
kernel/image.o: CFLAGS += $(WEBP_FLAGS)
kernel/image.o64: CFLAGS64 += $(WEBP_FLAGS)

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
loader/bios.bin: loader/bios.asm loader/bios.c loader/common.h loader/boot_ui.h loader/boot_font.h loader/bios.ld
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
EFI_DEPS = loader/efi.c loader/efi.h loader/common.h loader/boot_ui.h loader/boot_font.h loader/sha256.h loader/kernel_hashes.h

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
	objcopy -j .text -j .reloc -j .got -j .data -j .dynamic -j .rela -j .rel -j .dynsym -O $(2) --subsystem=10 $(1) $@.unsigned
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
isoroot/efi.img: tools/format_efi.py loader/BOOTX64.EFI loader/BOOTIA32.EFI kernel.bin kernel64.bin loader/keys/PK.auth loader/keys/BananaOS.cer
	mkdir -p isoroot
	rm -f $@
	printf 'set banana_medium=live-cd\nset banana_video=auto        \n' > isoroot/medium.cfg
	kb=$$(( ($$(stat -c %s kernel.bin) + $$(stat -c %s kernel64.bin) + 2 * $$(stat -c %s loader/BOOTX64.EFI)) / 1024 + 2048 )); \
	    dd if=/dev/zero of=$@ bs=1024 count=$$kb status=none
	python3 tools/format_efi.py $@
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
	rm -f $(EXAMPLE_BPKZ)
	for a in $(APPS) $(APPS64) terminal webworker; do $(MAKE) -s -C apps/$$a clean; done
	for d in $(DRIVER_EXAMPLES); do $(MAKE) -s -C driver-sdk/examples/$$d clean; done
	rm -f banana-sdk.tar.gz
	rm -f $(OBJS) $(OBJS64) $(DEPS) kernel.bin kernel64.bin Banana_OS.iso build/builtin-meta-*.bin build/libjsworker-x86_64.bpk.z
	rm -rf isoroot
	rm -f loader/*.o loader/*.so loader/*.bin loader/*.EFI loader/*.unsigned loader/kernel_hashes.h \
	      loader/keys/*.esl loader/keys/*.auth loader/keys/.nokey


# ── SDK: example apps (embedded in the kernel: ~/Examples) and the tarball ──
EXAMPLES     = gl3test glinfo gles2 hello languages guess paint clock tones mandel threads webview gears cltest systest posixtest mousetest shelltest
EXAMPLE_BPKS = $(foreach e,$(EXAMPLES),sdk/examples/$(e)/$(e).bpk)
EXAMPLE_BPKZ = $(EXAMPLE_BPKS:.bpk=.bpk.z)
SDK_DEPS     = $(wildcard sdk/lib/*.c sdk/lib/*.h sdk/lib/*.py sdk/include/*.h sdk/include/*/*.h) sdk/banana.mk sdk/tools/bpkg sdk/tools/mkexports

define EXAMPLE_RULE
sdk/examples/$(1)/$(1).bpk: $$(wildcard sdk/examples/$(1)/*.c sdk/examples/$(1)/*.h sdk/examples/$(1)/*.sh sdk/examples/$(1)/tests/*) sdk/examples/$(1)/Makefile $$(SDK_DEPS)
	$$(MAKE) -s -C sdk/examples/$(1)
# (in the kernel compressed, as the apps: 6.7 MB of the kernel's 19 were the
# examples, and the loader reads it through the BIOS)
sdk/examples/$(1)/$(1).bpk.z: sdk/examples/$(1)/$(1).bpk
	python3 -c "import sys, zlib; open(sys.argv[2], 'wb').write(zlib.compress(open(sys.argv[1], 'rb').read(), 9))" $$< $$@
endef
$(foreach e,$(EXAMPLES),$(eval $(call EXAMPLE_RULE,$(e))))

kernel/exbin.o kernel/exbin.o64: $(EXAMPLE_BPKZ)

# ── the apps that come with Banana OS (apps/): FFmpeg-based, built into the
# kernel compressed and installed at boot (kernel/builtin_apps.c). FFmpeg
# itself is built once by ports/ffmpeg/build.sh (the first build takes a while).
APPS     = mediaplayer music amethyst photos code store
APP_DEPS = $(SDK_DEPS) ports/ffmpeg/build.sh
define APP_RULE
# (&: one make of the app makes both - two at once raced in its folder)
apps/$(1)/$(1)-i686.bpk.z apps/$(1)/$(1)-x86_64.bpk.z &: $$(wildcard apps/$(1)/*.c) apps/$(1)/Makefile $$(APP_DEPS)
	$$(MAKE) -C apps/$(1) bundle
endef
$(foreach a,$(APPS),$(eval $(call APP_RULE,$(a))))
kernel/appbin.o: $(foreach a,$(APPS),apps/$(a)/$(a)-i686.bpk.z) build/builtin-meta-i686.bin
kernel/appbin.o64: $(foreach a,$(APPS),apps/$(a)/$(a)-x86_64.bpk.z) build/builtin-meta-x86_64.bin $(PORT_BPKZ) build/ports64.inc

# Unix software (ports/<name>/build.sh, with ports/port.sh), 64-bit only (it
# forks): GNU bash (/bin/bash, /bin/sh), coreutils... - built with the SDK's
# cross compiler (sdk/tools/banana-cc), built into the 64-bit kernel and
# installed at boot, their commands in /bin (bin= in their manifests).
# kernel/appbin.asm and kernel/builtin_apps.c take the list from
# build/ports64.inc and build/ports64.h, made from PORTS64.
PORTS64    = bash coreutils grep sed findutils diffutils gawk ncurses less zsh nano vim vi
# optional ones: built in the same way, but installed only when asked for
# (`apt install neovim`, the App Store: the builtin: source)
OPTPORTS64 = neovim
# Optional Intel Mesa ISO: the same OS plus the user-space GL runtime.
# Empty for the standard ISO; pass the audited runtime BPK for the Mesa ISO.
MESA_BPK ?=
ifneq ($(strip $(MESA_BPK)),)
PORTS64 += mesa
endif
PORT_BPKZ  = $(foreach p,$(PORTS64) $(OPTPORTS64),ports/$(p)/build/$(p)-x86_64.bpk.z)
CROSS_DEPS = sdk/tools/banana-cc sdk/tools/banana-configure sdk/tools/banana-config.site ports/port.sh
define PORT_RULE
ports/$(1)/build/$(1)-x86_64.bpk: $$(filter-out %/src %/build,$$(wildcard ports/$(1)/*)) $$(SDK_DEPS) $$(CROSS_DEPS)
	ports/$(1)/build.sh $$@
ports/$(1)/build/$(1)-x86_64.bpk.z: ports/$(1)/build/$(1)-x86_64.bpk
	python3 -c "import sys, zlib; open(sys.argv[2], 'wb').write(zlib.compress(open(sys.argv[1], 'rb').read(), 9))" $$< $$@
endef
$(foreach p,$(filter-out mesa,$(PORTS64)) $(OPTPORTS64),$(eval $(call PORT_RULE,$(p))))
ifneq ($(strip $(MESA_BPK)),)
.PHONY: mesa-prebuilt-input
mesa-prebuilt-input:
ports/mesa/build/mesa-x86_64.bpk.z: $(MESA_BPK) mesa-prebuilt-input
	mkdir -p $(@D)
	python3 sdk/tools/bpkg check "$(MESA_BPK)"
	python3 -c "import sys,zlib; from pathlib import Path; out=Path(sys.argv[2]); tmp=out.with_suffix('.tmp'); tmp.write_bytes(zlib.compress(Path(sys.argv[1]).read_bytes(),9)); tmp.replace(out)" "$(MESA_BPK)" $@
endif
# The Bash binary/script fault-injection test runs before packaging the port.
ports/bash/build/bash-x86_64.bpk: tools/test_bash_exec.py tools/tests/bash_exec.c
# (less, zsh, nano and vim are linked with ncurses' library)
ports/less/build/less-x86_64.bpk ports/zsh/build/zsh-x86_64.bpk ports/nano/build/nano-x86_64.bpk ports/vim/build/vim-x86_64.bpk: ports/ncurses/build/ncurses-x86_64.bpk
# Banana OS's own Unix programs (apps/<name>, sdk/banana.mk), 64-bit, built
# in the same way: ps & co. (apps/procps)
APPS64      = procps kcmd
APPS64_BPKZ = $(foreach a,$(APPS64),apps/$(a)/$(a)-x86_64.bpk.z)
define APP64_RULE
apps/$(1)/$(1)-x86_64.bpk.z: $$(filter-out %.bpk %.bpk.z %/build,$$(wildcard apps/$(1)/*)) $$(SDK_DEPS)
	$$(MAKE) -s -C apps/$(1) $(1)-x86_64.bpk.z
endef
$(foreach a,$(APPS64),$(eval $(call APP64_RULE,$(a))))
# Re-enter kcmd when its help-derived command list changes.
apps/kcmd/kcmd-x86_64.bpk.z: shell/help.c
kernel/appbin.o64: $(APPS64_BPKZ)
build/builtin-meta-x86_64.bin: $(APPS64_BPKZ)
.PHONY: ports-config-input
ports-config-input:
build/ports64.inc: Makefile ports-config-input
	mkdir -p build
	{ $(foreach p,$(PORTS64) $(OPTPORTS64),printf 'global port_%s, port_%s_end\nalign 16\nport_%s: incbin "ports/%s/build/%s-x86_64.bpk.z"\nport_%s_end:\n' $(p) $(p) $(p) $(p) $(p) $(p);) \
	  $(foreach a,$(APPS64),printf 'global port_%s, port_%s_end\nalign 16\nport_%s: incbin "apps/%s/%s-x86_64.bpk.z"\nport_%s_end:\n' $(a) $(a) $(a) $(a) $(a) $(a);) } > $@.tmp
	cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@
build/ports64.h: Makefile ports-config-input
	mkdir -p build
	{ $(foreach p,$(PORTS64) $(APPS64),echo 'PORT($(p))';) $(foreach p,$(OPTPORTS64),echo 'OPTPORT($(p))';) } > $@.tmp
	cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@
kernel/builtin_apps.o64: build/ports64.h

# the Terminal app (apps/terminal): bash in windows and tabs, 64-bit only too
WEBWORKER_BPKZ = apps/webworker/webworker-x86_64.bpk.z
$(WEBWORKER_BPKZ): $(wildcard apps/webworker/*.c apps/webworker/*.h) apps/webworker/Makefile $(wildcard web/*.c web/*.h) kernel/image.c kernel/image.h kernel/font.c kernel/font.h kernel/fontbin.asm kernel/font8x8.c kernel/ttf.c kernel/ttf.h kernel/types.h $(wildcard fonts/*.ttf third_party/libwebp/src/*/*.c third_party/libwebp/src/*/*.h) web/test/image_stb_host.c $(SDK_DEPS)
	$(MAKE) -C apps/webworker webworker-x86_64.bpk.z
# LibJS stays a user-space renderer; only its compressed install package is
# embedded. CI supplies the audited BPK from the same commit's Debian SDK job.
# Local builds compile it with the pinned Debian 13/GCC 14 toolchain.
LIBJS_WORKER_BPK ?=
LIBJS_WORKER_BPKZ = build/libjsworker-x86_64.bpk.z
ifeq ($(strip $(LIBJS_WORKER_BPK)),)
LIBJS_WORKER_DEPS = $(wildcard ports/libjs/*.py ports/libjs/*.sh ports/libjs/*.c ports/libjs/*.cpp ports/libjs/*.h ports/libjs/*.json) ports/libjs/CMakeLists.txt apps/libjsworker/Makefile $(wildcard apps/libjsworker/*.c apps/libjsworker/*.h apps/webworker/*.c apps/webworker/*.h web/*.c web/*.h kernel/*.h ports/llvm/*.c ports/llvm/include/*.h fonts/*.ttf third_party/libwebp/src/*/*.c third_party/libwebp/src/*/*.h) kernel/image.c kernel/font.c kernel/fontbin.asm kernel/font8x8.c kernel/ttf.c web/test/image_stb_host.c $(SDK_DEPS)
else
LIBJS_WORKER_DEPS = $(LIBJS_WORKER_BPK) libjs-prebuilt-input
.PHONY: libjs-prebuilt-input
libjs-prebuilt-input:
endif
$(LIBJS_WORKER_BPKZ): $(LIBJS_WORKER_DEPS) Makefile
	mkdir -p build
	$(if $(strip $(LIBJS_WORKER_BPK)),python3 sdk/tools/bpkg check "$(LIBJS_WORKER_BPK)",bash ports/libjs/build.sh target)
	python3 -c "import sys, zlib; from pathlib import Path; out=Path(sys.argv[2]); tmp=out.with_suffix('.tmp'); tmp.write_bytes(zlib.compress(Path(sys.argv[1]).read_bytes(), 9)); tmp.replace(out)" "$(if $(strip $(LIBJS_WORKER_BPK)),$(LIBJS_WORKER_BPK),apps/libjsworker/libjsworker-x86_64.bpk)" $@
kernel/appbin.o64 build/builtin-meta-x86_64.bin: $(LIBJS_WORKER_BPKZ)
kernel/appbin.o64: $(WEBWORKER_BPKZ)
build/builtin-meta-x86_64.bin: $(WEBWORKER_BPKZ)
TERMINAL_BPKZ = apps/terminal/terminal-x86_64.bpk.z
build/builtin-meta-x86_64.bin: $(PORT_BPKZ) $(TERMINAL_BPKZ) build/ports64.h
$(TERMINAL_BPKZ): $(wildcard apps/terminal/*.c apps/terminal/*.h) apps/terminal/Makefile apps/terminal/icon.png $(SDK_DEPS)
	$(MAKE) -C apps/terminal terminal-x86_64.bpk.z
kernel/appbin.o64: $(TERMINAL_BPKZ)

# the App Store's facts about those packages and the examples (size, SHA-256,
# manifest): made here, so the boot does not unpack and hash them all again
build/builtin-meta-%.bin: tools/mkbuiltinmeta.py $(foreach a,$(APPS),apps/$(a)/$(a)-%.bpk.z) $(EXAMPLE_BPKZ)
	mkdir -p build
	python3 tools/mkbuiltinmeta.py $@ $(foreach a,$(APPS),$(a).bpk=apps/$(a)/$(a)-$*.bpk.z) \
	    $(if $(filter x86_64,$*),webworker.bpk=$(WEBWORKER_BPKZ) libjsworker.bpk=$(LIBJS_WORKER_BPKZ) terminal.bpk=$(TERMINAL_BPKZ) $(foreach p,$(PORTS64) $(OPTPORTS64),$(p).bpk=ports/$(p)/build/$(p)-x86_64.bpk.z) \
	      $(foreach a,$(APPS64),$(a).bpk=apps/$(a)/$(a)-x86_64.bpk.z)) \
	    $(foreach e,$(EXAMPLES),$(e).bpk=sdk/examples/$(e)/$(e).bpk.z)

# the Driver Kit's examples (driver-sdk/examples): packages, not in the kernel
DRIVER_EXAMPLES = edu bochsfb sdk-probe usb-serial-demo
.PHONY: driver-examples
driver-examples:
	for d in $(DRIVER_EXAMPLES); do $(MAKE) -C driver-sdk/examples/$$d; done
kernel/fontbin.o kernel/fontbin.o64: $(wildcard fonts/*.ttf)

.PHONY: examples sdk driver-sdk
examples: $(EXAMPLE_BPKS)

# banana-sdk.tar.gz: everything needed to build apps on another Linux machine
sdk: $(EXAMPLE_BPKS)
	tar czf banana-sdk.tar.gz --transform 's,^sdk,banana-sdk,' --exclude=build --exclude='*.bpk' sdk
	@echo "banana-sdk.tar.gz: unpack it anywhere, then see banana-sdk/README.md"
-include $(DEPS)

# Standalone hardware kit; intentionally independent from app builds.
driver-sdk:
	python3 tools/export_driver_sdk.py

# Embedded driver notices and firmware are inputs for both architectures.
net/r8125.o net/r8125.o64: third_party/rge/NOTICES

kernel/aml_host.o kernel/aml_host.o64: third_party/uacpi/LICENSE

net/igc.o net/igc.o64: third_party/igc/NOTICES

net/mt7601u.o net/mt7601u.o64: firmware/mediatek/mt7601u.bin firmware/mediatek/NOTICES

net/rtl8188eu.o net/rtl8188eu.o64: firmware/realtek/rtl8188eufw.bin firmware/realtek/NOTICES

net/ar9280.o net/ar9280.o64: third_party/athn/NOTICES
net/ax88179a.o net/ax88179a.o64: third_party/asix/NOTICES
net/aqc107.o net/aqc107.o64: third_party/aq/NOTICES

net/rtl8811cu.o net/rtl8811cu.o64: firmware/realtek/rtw8821c_fw.bin third_party/rtw88/NOTICES
