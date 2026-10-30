# SPDX-License-Identifier: BSD-2-Clause
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
#     CATEGORY    = Games                 # optional: the App Store's shelf
#     DEPENDS     = libfoo (>= 1.0)       # optional: packages apt installs first
#     ICON        = icon.png              # optional: a 64x64 PNG for Apps and the App Store
#     UNPACK      = java.zip:/usr/java    # optional: a DATA zip pkg install unpacks into a
#                                         # folder of the app's own under /usr (removed with it)
#     LIB         = libfoo.so             # optional: package files linked into /lib
#     BIN         = ps,pgrep              # optional: commands in /bin, links to the program
#                                         # (or to a DATA file of that name)
#     USE_GL      = 1                     # optional: OpenGL 1.x (#include <GL/bgl.h>)
#     USE_GL3     = 1                     # optional: OpenGL 3.3 core (#include <GL/gl3.h>, 64-bit)
#     USE_EGL     = 1                     # optional: EGL + OpenGL ES 1.1 / 2.0 / 3.x (<EGL/egl.h>, <GLES2/gl2.h>, 64-bit)
#     USE_CL      = 1                     # optional: OpenCL (#include <CL/cl.h>)
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
CATEGORY    ?=
DEPENDS     ?=
ICON        ?=
UNPACK      ?=
BIN         ?=
# Shared library basenames exported from /apps/<app> into /lib.
LIB         ?=
USE_GL      ?= 0
USE_GL3     ?= 0
USE_EGL     ?= 0
USE_CL      ?= 0
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

# USE_GL = 1: OpenGL (<GL/gl.h>, <GL/glu.h>, <GL/bgl.h>), built like the app's own code
ifeq ($(USE_GL),1)
SRCS += $(BANANA_SDK)/lib/gl.c
endif

# USE_GL3 = 1: OpenGL 3.3 (<GL/gl3.h>): Mesa, loaded from libOSMesa.so (ports/mesa)
ifeq ($(USE_GL3),1)
SRCS += $(BANANA_SDK)/lib/gl3.c
endif

# USE_EGL = 1: EGL 1.5 with OpenGL ES 1.1 / 2.0 / 3.x (<EGL/egl.h>, <GLES/gl.h>,
# <GLES2/gl2.h>, <GLES3/gl3.h>) and desktop OpenGL: Mesa, as USE_GL3
ifeq ($(USE_EGL),1)
SRCS += $(BANANA_SDK)/lib/egl.c
ifneq ($(USE_GL3),1)
SRCS += $(BANANA_SDK)/lib/gl3.c
endif
endif

# USE_CL = 1: OpenCL (<CL/cl.h>) - the runtime and TinyCC, which compiles the
# kernels; TinyCC's source comes from the Banana OS tree (ports/tcc)
ifeq ($(USE_CL),1)
BANANA_TCC ?= $(BANANA_SDK)/../ports/tcc
CLJIT       = $(BUILD)/cljit
SRCS       += $(BANANA_SDK)/lib/cl.c $(BANANA_SDK)/lib/cljit.c
CFLAGS     += -I$(CLJIT)
.DEFAULT_GOAL := all
$(CLJIT)/config.h: $(BANANA_SDK)/lib/mkcljit.sh $(BANANA_SDK)/lib/patch-cljit.py $(BANANA_SDK)/lib/clvectors-tcc.h
	sh $(BANANA_SDK)/lib/mkcljit.sh $(BANANA_TCC) $(CLJIT)
$(BUILD)/i686/cl.o $(BUILD)/x86_64/cl.o $(BUILD)/i686/cljit.o $(BUILD)/x86_64/cljit.o: $(CLJIT)/config.h
$(BUILD)/i686/cl.o $(BUILD)/x86_64/cl.o: $(wildcard $(BANANA_SDK)/lib/cl*.h)
endif

LIBC_SRCS = $(BANANA_SDK)/lib/crt0.c $(BANANA_SDK)/lib/libc.c $(BANANA_SDK)/lib/malloc.c $(BANANA_SDK)/lib/stdio.c $(BANANA_SDK)/lib/banana.c \
            $(BANANA_SDK)/lib/math.c $(BANANA_SDK)/lib/posix.c $(BANANA_SDK)/lib/setjmp.c \
            $(BANANA_SDK)/lib/mman.c $(BANANA_SDK)/lib/pthread.c $(BANANA_SDK)/lib/socket.c \
            $(BANANA_SDK)/lib/signal.c $(BANANA_SDK)/lib/dl.c $(BANANA_SDK)/lib/cxxrt.c \
            $(BANANA_SDK)/lib/unix.c $(BANANA_SDK)/lib/utf8.c $(BANANA_SDK)/lib/ufd.c $(BANANA_SDK)/lib/math99.c \
            $(BANANA_SDK)/lib/proc.c $(BANANA_SDK)/lib/tty.c $(BANANA_SDK)/lib/ufs.c \
            $(BANANA_SDK)/lib/regex.c
LIBC_i686   = $(LIBC_SRCS) $(BANANA_SDK)/lib/divdi3.c
LIBC_x86_64 = $(LIBC_SRCS)

# Source inputs found through pattern rules are never disposable intermediates.
# GNU make can otherwise remove malloc.c after an incremental -o source build.
.PRECIOUS: $(LIBC_SRCS) $(BANANA_SDK)/lib/divdi3.c

# the libc must not turn its own loops into calls to itself
NOBUILTIN = -fno-builtin -fno-tree-loop-distribute-patterns

OBJS_i686     = $(patsubst %.c,$(BUILD)/i686/%.o,$(notdir $(SRCS)))
OBJS_x86_64   = $(patsubst %.c,$(BUILD)/x86_64/%.o,$(notdir $(SRCS)))
LOBJS_i686    = $(patsubst %.c,$(BUILD)/i686/sdk_%.o,$(notdir $(LIBC_i686)))
LOBJS_x86_64  = $(patsubst %.c,$(BUILD)/x86_64/sdk_%.o,$(notdir $(LIBC_x86_64)))

vpath %.c $(sort $(dir $(SRCS)))

# (the objects are made again when an SDK header changes)
SDK_HEADERS := $(wildcard $(BANANA_SDK)/include/*.h $(BANANA_SDK)/include/*/*.h)

.PHONY: all clean bins
all: $(APP).bpk

bins: $(BUILD)/app-i686 $(BUILD)/app-x86_64

$(BUILD)/i686/%.o: %.c $(SDK_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_i686) $(APP_CFLAGS) -c $< -o $@

$(BUILD)/x86_64/%.o: %.c $(SDK_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_x86_64) $(APP_CFLAGS) -c $< -o $@

$(BUILD)/i686/sdk_%.o: $(BANANA_SDK)/lib/%.c $(SDK_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_i686) $(APP_CFLAGS) $(NOBUILTIN) -c $< -o $@

$(BUILD)/x86_64/sdk_%.o: $(BANANA_SDK)/lib/%.c $(SDK_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_x86_64) $(APP_CFLAGS) $(NOBUILTIN) -c $< -o $@

# the table of the app's own symbols (its objects' and the SDK's), which
# the shared libraries it opens with dlopen may use (sdk/lib/dl.c)
$(BUILD)/i686/__exports.c: $(LOBJS_i686) $(OBJS_i686) $(BANANA_SDK)/tools/mkexports
	$(PYTHON) $(BANANA_SDK)/tools/mkexports $@ $(filter %.o,$^)
$(BUILD)/x86_64/__exports.c: $(LOBJS_x86_64) $(OBJS_x86_64) $(BANANA_SDK)/tools/mkexports
	$(PYTHON) $(BANANA_SDK)/tools/mkexports $@ $(filter %.o,$^)
$(BUILD)/i686/__exports.o: $(BUILD)/i686/__exports.c
	$(CC) $(CFLAGS_i686) $(APP_CFLAGS) -w -c $< -o $@
$(BUILD)/x86_64/__exports.o: $(BUILD)/x86_64/__exports.c
	$(CC) $(CFLAGS_x86_64) $(APP_CFLAGS) -w -c $< -o $@

$(BUILD)/app-i686: $(LOBJS_i686) $(OBJS_i686) $(BUILD)/i686/__exports.o
	$(LD) $(LDFLAGS_i686) $(APP_LDFLAGS) -o $@ $(filter %.o,$^) $(LIBS_i686)

$(BUILD)/app-x86_64: $(LOBJS_x86_64) $(OBJS_x86_64) $(BUILD)/x86_64/__exports.o
	$(LD) $(LDFLAGS_x86_64) $(APP_LDFLAGS) -o $@ $(filter %.o,$^) $(LIBS_x86_64)

# (sq: a value for inside '...' in the shell - an apostrophe in a title or
# description, "the shell's", ended the quote and broke the build)
sq = $(subst ','\'',$(1))

$(BUILD)/manifest: $(MAKEFILE_LIST)
	@mkdir -p $(BUILD)
	@printf 'name=%s\ntitle=%s\nversion=%s\ntype=%s\ndescription=%s\nauthor=%s\ncategory=%s\ndepends=%s\n' \
	    '$(call sq,$(APP))' '$(call sq,$(TITLE))' '$(call sq,$(VERSION))' '$(call sq,$(TYPE))' \
	    '$(call sq,$(DESCRIPTION))' '$(call sq,$(AUTHOR))' '$(call sq,$(CATEGORY))' '$(call sq,$(DEPENDS))' > $@
	@$(if $(UNPACK),printf 'unpack=%s\n' '$(call sq,$(UNPACK))' >> $@,true)
	@$(if $(BIN),printf 'bin=%s\n' '$(call sq,$(BIN))' >> $@,true)
	@$(if $(LIB),printf 'lib=%s\n' '$(call sq,$(LIB))' >> $@,true)

ICON_FILE = $(if $(ICON),icon.png=$(ICON))

$(APP).bpk: $(BUILD)/manifest $(BUILD)/app-i686 $(BUILD)/app-x86_64 $(DATA) $(ICON)
	$(PYTHON) $(BANANA_SDK)/tools/bpkg pack -o $@ $(BUILD)/manifest $(BUILD)/app-i686 $(BUILD)/app-x86_64 $(DATA) $(ICON_FILE)
	@$(PYTHON) $(BANANA_SDK)/tools/bpkg check $@

# one package per CPU, compressed (apps built into Banana OS: the Makefile's APPS)
.PHONY: bundle
bundle: $(APP)-i686.bpk.z $(APP)-x86_64.bpk.z
$(APP)-i686.bpk: $(BUILD)/manifest $(BUILD)/app-i686 $(DATA) $(ICON)
	$(PYTHON) $(BANANA_SDK)/tools/bpkg pack -o $@ $(BUILD)/manifest $(BUILD)/app-i686 $(DATA) $(ICON_FILE)
$(APP)-x86_64.bpk: $(BUILD)/manifest $(BUILD)/app-x86_64 $(DATA) $(ICON)
	$(PYTHON) $(BANANA_SDK)/tools/bpkg pack -o $@ $(BUILD)/manifest $(BUILD)/app-x86_64 $(DATA) $(ICON_FILE)
%.bpk.z: %.bpk
	$(PYTHON) -c "import sys, zlib; open(sys.argv[2], 'wb').write(zlib.compress(open(sys.argv[1], 'rb').read(), 9))" $< $@

clean:
	rm -rf $(BUILD) $(APP).bpk $(APP)-i686.bpk $(APP)-x86_64.bpk $(APP)-*.bpk.z
