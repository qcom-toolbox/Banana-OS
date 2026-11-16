# SPDX-License-Identifier: GPL-3.0-or-later
.DEFAULT_GOAL := prototype
APP = libjsworker
TITLE = Experimental LibJS browser engine
VERSION = 1.0
TYPE = console
DESCRIPTION = Internal isolated browser renderer
AUTHOR = Banana OS
SRCS = ../webworker/main.c worker_compat.c ../webworker/github.c ../../ports/libjs/dom_host.c ../../web/arena.c ../../web/html.c ../../web/css.c        ../../web/layout.c ../../web/page.c ../../web/render.c ../../web/svg.c        ../../web/script.c ../../web/script_es.c ../../web/script_lib.c        ../../web/script_bigint.c ../../web/script_typed.c ../../web/jsdom.c ../../web/canvas.c ../../web/webgl.c        ../../web/regex.c ../../web/image_decode.c ../../kernel/image.c        ../../kernel/font.c ../../kernel/ttf.c ../../kernel/font8x8.c        ../../web/test/image_stb_host.c        $(wildcard ../../third_party/libwebp/src/dec/*.c ../../third_party/libwebp/src/dsp/*.c ../../third_party/libwebp/src/utils/*.c)
CFLAGS = -DBANANA_MALLOC_REGION=33554432ul -DBANANA_USER_WORKER -DBANANA_LIBJS_WORKER -DFONT_HOST -DHAVE_CONFIG_H -DNDEBUG          -I../../kernel -I../../web -I../../third_party/libwebp
DATA = ../../ports/libjs/build/banana/THIRD_PARTY_NOTICES.txt
LIBS_x86_64 = --start-group ../../ports/libjs/build/banana/libjs-prototype.a \
    $(shell g++ -print-file-name=libstdc++.a) $(shell gcc -print-libgcc-file-name) \
    $(shell gcc -print-file-name=libgcc_eh.a) --end-group
LDFLAGS = -Map build/libjs-link.map
include ../../sdk/banana.mk
$(BUILD)/app-x86_64: ../../ports/libjs/build/banana/libjs-prototype.a
$(BUILD)/x86_64/sdk_cxxrt.o: $(BANANA_SDK)/lib/cxxrt.c $(SDK_HEADERS) ../../ports/libjs/localize_sdk_runtime.py
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_x86_64) $(APP_CFLAGS) $(NOBUILTIN) -c $< -o $@
	python3 ../../ports/libjs/localize_sdk_runtime.py $@
.PHONY: prototype
prototype: libjsworker-x86_64.bpk.z
build/fontbin.o: ../../kernel/fontbin.asm $(wildcard ../../fonts/*.ttf)
	@mkdir -p build
	cd ../.. && nasm -f elf64 kernel/fontbin.asm -o apps/libjsworker/$@
build/app-x86_64: build/fontbin.o

# The SDK tracks SDK headers. The embedded engine also depends on its own
# structure layouts: rebuild its objects when those headers change.
$(OBJS_x86_64): $(wildcard ../../web/*.h ../../kernel/*.h) Makefile

$(BUILD)/x86_64/dom_host.o: ../../ports/libjs/dom.h
$(BUILD)/x86_64/worker_compat.o: ../webworker/compat.c
