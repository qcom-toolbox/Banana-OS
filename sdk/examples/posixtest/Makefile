# SPDX-License-Identifier: BSD-2-Clause
APP         = posixtest
TITLE       = POSIX test
VERSION     = 1.0
TYPE        = console
DESCRIPTION = Checks the POSIX layer for ported programs: mmap, threads, sockets, signals, dlopen
AUTHOR      = Banana OS SDK
CATEGORY    = Extras
SRCS        = posixtest.c
# shared libraries for its dlopen checks (libdemo2 needs libdemo), per CPU
DATA        = libdemo64.so libdemo2_64.so libdemo32.so libdemo2_32.so

include ../../banana.mk

# a shared library for Banana OS: -fPIC and -shared, as on Linux
SO_CFLAGS = -ffreestanding -fno-stack-protector -fPIC -nostdlib -nostdinc -fno-builtin -O2 -Wall -I$(BANANA_SDK)/include

$(BUILD)/so64/%.o: %.c $(SDK_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) -m64 -mno-red-zone $(SO_CFLAGS) -c $< -o $@
$(BUILD)/so32/%.o: %.c $(SDK_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) -m32 -march=i686 $(SO_CFLAGS) -c $< -o $@

libdemo64.so: $(BUILD)/so64/demo.o
	$(LD) -m elf_x86_64 -shared -soname libdemo64.so -o $@ $^
libdemo2_64.so: $(BUILD)/so64/demo2.o libdemo64.so
	$(LD) -m elf_x86_64 -shared -soname libdemo2_64.so -o $@ $^
libdemo32.so: $(BUILD)/so32/demo.o
	$(LD) -m elf_i386 -shared -soname libdemo32.so -o $@ $^
libdemo2_32.so: $(BUILD)/so32/demo2.o libdemo32.so
	$(LD) -m elf_i386 -shared -soname libdemo2_32.so -o $@ $^
