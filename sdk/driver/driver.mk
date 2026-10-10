# Banana OS Driver Kit - build rules for drivers.
#
# A minimal driver Makefile:
#
#     DRIVER      = acme-gpu              # package name: a-z 0-9 - _ (24 max)
#     TITLE       = ACME Graphics 9000
#     VERSION     = 1.0
#     DESCRIPTION = Display driver for the ACME Graphics 9000
#     AUTHOR      = ACME Inc.
#     SRCS        = acme.c
#     include /path/to/banana-sdk/driver/driver.mk
#
# `make` builds the driver for both CPUs Banana OS runs on (i686 and
# x86_64) and packs them into $(DRIVER).bpk (type=driver). On Banana OS,
# `pkg install $(DRIVER).bpk` installs it and loads it; it is loaded at
# every boot from then on. `drivers` shows it.
#
# Needs: gcc with 32-bit support (Debian/Ubuntu: gcc-multilib), binutils,
# python3.

BDK := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
SDK := $(BDK)/..

DRIVER      ?= driver
TITLE       ?= $(DRIVER)
VERSION     ?= 1.0
DESCRIPTION ?=
AUTHOR      ?=
SRCS        ?= driver.c
BUILD       ?= build

CC     ?= gcc
LD     ?= ld
PYTHON ?= python3

# Kernel code: freestanding, position-independent, no floating point or
# SIMD (the kernel does not save those registers for drivers), no red
# zone (interrupt handlers share the stack).
DRV_CFLAGS = -ffreestanding -fno-stack-protector -fpie -fvisibility=hidden -nostdlib -nostdinc \
             -fno-asynchronous-unwind-tables -fno-exceptions -mgeneral-regs-only \
             -fno-tree-loop-distribute-patterns -O2 -Wall -Wextra \
             -I$(BDK) -I$(SDK)/include $(CFLAGS)
CFLAGS_i686   = -m32 -march=i686
CFLAGS_x86_64 = -m64 -mno-red-zone -mcmodel=small
LDFLAGS_i686   = -m elf_i386
LDFLAGS_x86_64 = -m elf_x86_64
DRV_LDFLAGS = -pie --no-dynamic-linker -z noexecstack -z norelro -z max-page-size=4096 \
              -z noseparate-code --hash-style=sysv -e _banana_driver_start -s $(LDFLAGS)

KIT_SRCS = $(BDK)/driver_crt.c $(BDK)/driver_lib.c

OBJS_i686    = $(patsubst %.c,$(BUILD)/i686/%.o,$(notdir $(SRCS)))
OBJS_x86_64  = $(patsubst %.c,$(BUILD)/x86_64/%.o,$(notdir $(SRCS)))
KOBJS_i686   = $(patsubst %.c,$(BUILD)/i686/bdk_%.o,$(notdir $(KIT_SRCS)))
KOBJS_x86_64 = $(patsubst %.c,$(BUILD)/x86_64/bdk_%.o,$(notdir $(KIT_SRCS)))
LIBGCC_i686   = $(shell $(CC) -m32 -print-libgcc-file-name)
LIBGCC_x86_64 = $(shell $(CC) -m64 -print-libgcc-file-name)

vpath %.c $(sort $(dir $(SRCS)))

.PHONY: all clean
all: $(DRIVER).bpk

$(BUILD)/i686/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_i686) $(DRV_CFLAGS) -c $< -o $@
$(BUILD)/x86_64/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_x86_64) $(DRV_CFLAGS) -c $< -o $@
$(BUILD)/i686/bdk_%.o: $(BDK)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_i686) $(DRV_CFLAGS) -fno-builtin -c $< -o $@
$(BUILD)/x86_64/bdk_%.o: $(BDK)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_x86_64) $(DRV_CFLAGS) -fno-builtin -c $< -o $@

$(BUILD)/driver-i686: $(KOBJS_i686) $(OBJS_i686)
	$(LD) $(LDFLAGS_i686) $(DRV_LDFLAGS) -o $@ $^ $(LIBGCC_i686)
$(BUILD)/driver-x86_64: $(KOBJS_x86_64) $(OBJS_x86_64)
	$(LD) $(LDFLAGS_x86_64) $(DRV_LDFLAGS) -o $@ $^ $(LIBGCC_x86_64)

$(BUILD)/manifest: $(MAKEFILE_LIST)
	@mkdir -p $(BUILD)
	@printf 'name=%s\ntitle=%s\nversion=%s\ntype=driver\ndescription=%s\nauthor=%s\n' \
	    '$(DRIVER)' '$(TITLE)' '$(VERSION)' '$(DESCRIPTION)' '$(AUTHOR)' > $@

$(DRIVER).bpk: $(BUILD)/manifest $(BUILD)/driver-i686 $(BUILD)/driver-x86_64
	$(PYTHON) $(SDK)/tools/bpkg pack -o $@ $^
	@$(PYTHON) $(SDK)/tools/bpkg check $@

clean:
	rm -rf $(BUILD) $(DRIVER).bpk
