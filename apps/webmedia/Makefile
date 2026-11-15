# SPDX-License-Identifier: GPL-3.0-or-later
APP = webmedia
TITLE = Isolated media decoder
VERSION = 1.0
TYPE = console
DESCRIPTION = Internal decoder of web page audio and video (FFmpeg), one restricted process per stream
AUTHOR = Banana OS
SRCS = main.c media.c
# FFmpeg, built by ports/ffmpeg/build.sh
FF      = ../../ports/ffmpeg
FFB     = $(FF)/build
FFLIBS  = libavformat/libavformat.a libavcodec/libavcodec.a libswscale/libswscale.a \
          libswresample/libswresample.a libavutil/libavutil.a
CFLAGS  = -DBANANA_MALLOC_REGION=33554432ul -I$(FF)/src/ffmpeg-7.1 -I$(FFB)/x86_64 -Wno-unused-parameter
LIBS_x86_64 = $(addprefix $(FFB)/x86_64/,$(FFLIBS)) $(shell gcc -m64 -print-libgcc-file-name)
include ../../sdk/banana.mk
$(FFB)/x86_64/libavutil/libavutil.a:
	$(FF)/build.sh x86_64 $(FFB)/x86_64
$(BUILD)/x86_64/media.o: $(FFB)/x86_64/libavutil/libavutil.a
$(BUILD)/app-x86_64: $(FFB)/x86_64/libavutil/libavutil.a
