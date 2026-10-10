# Banana OS SDK - build rules for apps.
#
# A minimal app Makefile:
#
#     APP         = hello                 # package name: a-z 0-9 - _ (24 max)
#     TITLE       = Hello World           # shown in Apps on the desktop
#     VERSION     = 1.0
#     TYPE        = console               # console (runs in a terminal) or gui (opens windows)
#     DESCRIPTION = Says hello
#     AUTHOR      = You
#     SRCS        = main.c                # your C files
#     DATA        = levels.txt            # optional: files shipped with the app
#     include /path/to/banana-sdk/banana.mk
#
# `make` builds the app for both CPUs Banana OS runs on (i686 and x86_64)
# and packs them into $(APP).bpk. Copy that to Banana OS (USB stick,
# download it in the browser, wget, ...) and run `pkg install $(APP).bpk`.
#
# Needs: gcc with 32-bit support (Debian/Ubuntu: gcc-multilib), binutils,
# python3.

BANANA_SDK := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))

APP         ?= app
TITLE       ?= $(APP)
VERSION     ?= 1.0
TYPE        ?= console
DESCRIPTION ?=
AUTHOR      ?=
SRCS        ?= main.c
DATA        ?=
BUILD       ?= build
# extra libraries (.a) linked after the objects, per CPU
LIBS_i686   ?=
LIBS_x86_64 ?=

CC   ?= gcc
LD   ?= ld
PYTHON ?= python3

# Freestanding position-independent code, no red zone (interrupts share
# the app's stack). Floating point is fine: Banana OS saves every task's
# FPU/SSE registers (x86_64 uses SSE, i686 the x87).
APP_CFLAGS = -ffreestanding -fno-stack-protector -fstack-clash-protection -fpie -fvisibility=hidden -nostdlib -nostdinc \
             -fno-asynchronous-unwind-tables -fno-exceptions -fno-builtin-malloc \
             -fno-math-errno -O2 -Wall -Wextra \
             -I$(BANANA_SDK)/include $(CFLAGS)
CFLAGS_i686   = -m32 -march=i686 -mstackrealign
CFLAGS_x86_64 = -m64 -mno-red-zone -mcmodel=small
LDFLAGS_i686   = -m elf_i386
LDFLAGS_x86_64 = -m elf_x86_64
APP_LDFLAGS = -pie --no-dynamic-linker -z noexecstack -z norelro -z max-page-size=4096 \
              -z noseparate-code --hash-style=sysv -e _banana_start -s $(LDFLAGS)

LIBC_SRCS = $(BANANA_SDK)/lib/crt0.c $(BANANA_SDK)/lib/libc.c $(BANANA_SDK)/lib/stdio.c $(BANANA_SDK)/lib/banana.c \
            $(BANANA_SDK)/lib/math.c $(BANANA_SDK)/lib/posix.c
LIBC_i686   = $(LIBC_SRCS) $(BANANA_SDK)/lib/divdi3.c
LIBC_x86_64 = $(LIBC_SRCS)

# the libc must not turn its own loops into calls to itself
NOBUILTIN = -fno-builtin -fno-tree-loop-distribute-patterns

OBJS_i686     = $(patsubst %.c,$(BUILD)/i686/%.o,$(notdir $(SRCS)))
OBJS_x86_64   = $(patsubst %.c,$(BUILD)/x86_64/%.o,$(notdir $(SRCS)))
LOBJS_i686    = $(patsubst %.c,$(BUILD)/i686/sdk_%.o,$(notdir $(LIBC_i686)))
LOBJS_x86_64  = $(patsubst %.c,$(BUILD)/x86_64/sdk_%.o,$(notdir $(LIBC_x86_64)))

vpath %.c $(sort $(dir $(SRCS)))

.PHONY: all clean bins
all: $(APP).bpk

bins: $(BUILD)/app-i686 $(BUILD)/app-x86_64

$(BUILD)/i686/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_i686) $(APP_CFLAGS) -c $< -o $@

$(BUILD)/x86_64/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_x86_64) $(APP_CFLAGS) -c $< -o $@

$(BUILD)/i686/sdk_%.o: $(BANANA_SDK)/lib/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_i686) $(APP_CFLAGS) $(NOBUILTIN) -c $< -o $@

$(BUILD)/x86_64/sdk_%.o: $(BANANA_SDK)/lib/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_x86_64) $(APP_CFLAGS) $(NOBUILTIN) -c $< -o $@

$(BUILD)/app-i686: $(LOBJS_i686) $(OBJS_i686)
	$(LD) $(LDFLAGS_i686) $(APP_LDFLAGS) -o $@ $(filter %.o,$^) $(LIBS_i686)

$(BUILD)/app-x86_64: $(LOBJS_x86_64) $(OBJS_x86_64)
	$(LD) $(LDFLAGS_x86_64) $(APP_LDFLAGS) -o $@ $(filter %.o,$^) $(LIBS_x86_64)

$(BUILD)/manifest: $(MAKEFILE_LIST)
	@mkdir -p $(BUILD)
	@printf 'name=%s\ntitle=%s\nversion=%s\ntype=%s\ndescription=%s\nauthor=%s\n' \
	    '$(APP)' '$(TITLE)' '$(VERSION)' '$(TYPE)' '$(DESCRIPTION)' '$(AUTHOR)' > $@

$(APP).bpk: $(BUILD)/manifest $(BUILD)/app-i686 $(BUILD)/app-x86_64 $(DATA)
	$(PYTHON) $(BANANA_SDK)/tools/bpkg pack -o $@ $(BUILD)/manifest $(BUILD)/app-i686 $(BUILD)/app-x86_64 $(DATA)
	@$(PYTHON) $(BANANA_SDK)/tools/bpkg check $@

# one package per CPU, compressed (apps built into Banana OS: the Makefile's APPS)
.PHONY: bundle
bundle: $(APP)-i686.bpk.z $(APP)-x86_64.bpk.z
$(APP)-i686.bpk: $(BUILD)/manifest $(BUILD)/app-i686 $(DATA)
	$(PYTHON) $(BANANA_SDK)/tools/bpkg pack -o $@ $^
$(APP)-x86_64.bpk: $(BUILD)/manifest $(BUILD)/app-x86_64 $(DATA)
	$(PYTHON) $(BANANA_SDK)/tools/bpkg pack -o $@ $^
%.bpk.z: %.bpk
	$(PYTHON) -c "import sys, zlib; open(sys.argv[2], 'wb').write(zlib.compress(open(sys.argv[1], 'rb').read(), 9))" $< $@

clean:
	rm -rf $(BUILD) $(APP).bpk $(APP)-i686.bpk $(APP)-x86_64.bpk $(APP)-*.bpk.z
