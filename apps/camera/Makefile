# SPDX-License-Identifier: GPL-3.0-or-later
APP         = camera
TITLE       = Camera
VERSION     = 1.0
TYPE        = gui
DESCRIPTION = The webcam live, photos of it, and voice memos from the microphone
AUTHOR      = Banana OS
SRCS        = camera.c
ICON        = icon.png

LIBS_i686   = $(shell gcc -m32 -print-libgcc-file-name)
LIBS_x86_64 = $(shell gcc -m64 -print-libgcc-file-name)

include ../../sdk/banana.mk
