APP         = mediaplayer
TITLE       = Media Player
VERSION     = 1.0
TYPE        = gui
DESCRIPTION = Videos and music: MP4, MKV, WebM, AVI, MOV, MP3, FLAC, OGG... (FFmpeg)
AUTHOR      = Banana OS
SRCS        = player.c

# FFmpeg, built for both CPUs by ports/ffmpeg/build.sh
FF      = ../../ports/ffmpeg
FFB     = $(FF)/build
FFLIBS  = libavformat/libavformat.a libavcodec/libavcodec.a libswscale/libswscale.a \
          libswresample/libswresample.a libavutil/libavutil.a
CFLAGS  = -I$(FF)/src/ffmpeg-7.1 -I$(FFB)/x86_64 -Wno-unused-parameter
LIBS_i686   = $(addprefix $(FFB)/i686/,$(FFLIBS)) $(shell gcc -m32 -print-libgcc-file-name)
LIBS_x86_64 = $(addprefix $(FFB)/x86_64/,$(FFLIBS)) $(shell gcc -m64 -print-libgcc-file-name)

include ../../sdk/banana.mk

$(FFB)/%/libavutil/libavutil.a:
	$(FF)/build.sh $* $(FFB)/$*

$(BUILD)/i686/player.o $(BUILD)/x86_64/player.o: $(FFB)/x86_64/libavutil/libavutil.a
$(BUILD)/app-i686: $(FFB)/i686/libavutil/libavutil.a
$(BUILD)/app-x86_64: $(FFB)/x86_64/libavutil/libavutil.a
