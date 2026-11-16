# SPDX-License-Identifier: GPL-3.0-or-later
APP = webgl
TITLE = Isolated WebGL renderer
VERSION = 1.0
TYPE = console
DESCRIPTION = Internal GPU process of the browser: plays a page's WebGL commands with OpenGL ES 2.0 or 3.0 (Mesa)
AUTHOR = Banana OS
SRCS = main.c gl.c
CFLAGS = -DBANANA_MALLOC_REGION=33554432ul
USE_EGL = 1
include ../../sdk/banana.mk
