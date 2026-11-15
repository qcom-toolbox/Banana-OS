# SPDX-License-Identifier: GPL-3.0-or-later
APP = webworker
TITLE = Isolated web engine
VERSION = 1.0
TYPE = console
DESCRIPTION = Internal isolated browser renderer
AUTHOR = Banana OS
SRCS = main.c compat.c github.c ../../web/arena.c ../../web/html.c ../../web/css.c        ../../web/layout.c ../../web/page.c ../../web/render.c ../../web/svg.c        ../../web/script.c ../../web/script_es.c ../../web/script_lib.c        ../../web/script_bigint.c ../../web/script_typed.c ../../web/jsdom.c        ../../web/regex.c ../../web/image_decode.c ../../kernel/image.c        ../../kernel/font.c ../../kernel/ttf.c ../../kernel/font8x8.c        ../../web/test/image_stb_host.c        $(wildcard ../../third_party/libwebp/src/dec/*.c ../../third_party/libwebp/src/dsp/*.c ../../third_party/libwebp/src/utils/*.c)
CFLAGS = -DBANANA_MALLOC_REGION=33554432ul -DBANANA_USER_WORKER -DFONT_HOST -DHAVE_CONFIG_H -DNDEBUG          -I../../kernel -I../../web -I../../third_party/libwebp
include ../../sdk/banana.mk
build/fontbin.o: ../../kernel/fontbin.asm $(wildcard ../../fonts/*.ttf)
	@mkdir -p build
	cd ../.. && nasm -f elf64 kernel/fontbin.asm -o apps/webworker/$@
build/app-x86_64: build/fontbin.o

# The SDK tracks SDK headers. The embedded engine also depends on its own
# structure layouts: rebuild its objects when those headers change.
$(OBJS_x86_64): $(wildcard ../../web/*.h ../../kernel/*.h) Makefile
