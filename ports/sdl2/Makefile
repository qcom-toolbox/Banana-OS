# SPDX-License-Identifier: GPL-3.0-or-later
CC ?= gcc
AR ?= ar
SDK := $(abspath ../../sdk)
SOURCE := src/sdl2_banana.c
HEADERS := $(wildcard include/SDL2/*.h) $(wildcard $(SDK)/include/*.h)
COMMON := -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-stack-protector \
          -fpie -fvisibility=hidden -nostdlib -nostdinc -fno-asynchronous-unwind-tables \
          -Iinclude -I$(SDK)/include
.PHONY: all clean test
all: build/x86_64/libSDL2.a build/i686/libSDL2.a
build/x86_64/libSDL2.a: $(SOURCE) $(HEADERS) Makefile
	@mkdir -p $(@D)
	$(CC) -m64 -march=x86-64 -mno-red-zone $(COMMON) $(CFLAGS) -c $(SOURCE) -o $(@D)/sdl2_banana.o
	$(AR) rcs $@ $(@D)/sdl2_banana.o
build/i686/libSDL2.a: $(SOURCE) $(HEADERS) Makefile
	@mkdir -p $(@D)
	$(CC) -m32 -march=i686 -mstackrealign $(COMMON) $(CFLAGS) -c $(SOURCE) -o $(@D)/sdl2_banana.o
	$(AR) rcs $@ $(@D)/sdl2_banana.o
test:
	python3 test/host.py
clean:
	rm -rf build
