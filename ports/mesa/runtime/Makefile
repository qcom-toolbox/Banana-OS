# SPDX-License-Identifier: BSD-2-Clause
APP = mesa
TITLE = Mesa Graphics Runtime
VERSION = 24.3.4
TYPE = console
DESCRIPTION = Intel Crocus/Iris, AMD r600 (Radeon HD 5450-7450), VirGL and software OpenGL; glinfo reports the active renderer
AUTHOR = Mesa contributors and Banana OS
CATEGORY = System
BIN = glinfo
LIB = libOSMesa.so
SRCS = ../../../sdk/examples/glinfo/glinfo.c
USE_EGL = 1
MESA_LIBRARY ?= ../build-crocus-iris-r600/libOSMesa.so
DATA = $(abspath $(MESA_LIBRARY)) ../../../THIRD-PARTY-NOTICES ../src/mesa-24.3.4/docs/license.rst
include ../../../sdk/banana.mk
