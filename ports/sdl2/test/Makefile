# SPDX-License-Identifier: GPL-3.0-or-later
APP = sdl2_demo
TITLE = SDL2 subset experiment
VERSION = 0.1
TYPE = console
DESCRIPTION = Optional SDL-style software window and surface smoke test
SRCS = sdl2_demo.c
HERE := $(abspath .)
CFLAGS += -I$(HERE)/../include
LIBS_i686 = $(HERE)/../build/i686/libSDL2.a
LIBS_x86_64 = $(HERE)/../build/x86_64/libSDL2.a
include ../../../sdk/banana.mk
$(BUILD)/app-i686: $(LIBS_i686)
$(BUILD)/app-x86_64: $(LIBS_x86_64)
SDL_DEPS = ../src/sdl2_banana.c $(wildcard ../include/SDL2/*.h) ../Makefile $(wildcard ../../../sdk/include/*.h)
$(LIBS_i686): $(SDL_DEPS)
	$(MAKE) -C .. build/i686/libSDL2.a
$(LIBS_x86_64): $(SDL_DEPS)
	$(MAKE) -C .. build/x86_64/libSDL2.a
