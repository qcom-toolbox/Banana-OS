# SPDX-License-Identifier: BSD-2-Clause
APP         = cltest
TITLE       = OpenCL test
VERSION     = 1.1
TYPE        = console
CATEGORY    = Extras
DESCRIPTION = OpenCL on every core: barriers, shared local arrays, vectors, Mandelbrot, atomics and math
AUTHOR      = Banana OS SDK
SRCS        = cltest.c
USE_CL      = 1
LIBS_i686   = $(shell gcc -m32 -print-libgcc-file-name)
LIBS_x86_64 = $(shell gcc -m64 -print-libgcc-file-name)

include ../../banana.mk

$(BUILD)/i686/cltest.o $(BUILD)/x86_64/cltest.o: collectives.h
