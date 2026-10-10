APP         = amethyst
TITLE       = Amethyst Music
VERSION     = 1.0
TYPE        = gui
DESCRIPTION = Amethyst Music (music.office-works.ch): stream the catalogue, playlists, recommendations
AUTHOR      = Banana OS
SRCS        = amethyst.c

# FFmpeg's sound-only build (with WebP for the covers), for both CPUs
FF      = ../../ports/ffmpeg
FFB     = $(FF)/build
FFLIBS  = libavformat/libavformat.a libavcodec/libavcodec.a libswscale/libswscale.a \
          libswresample/libswresample.a libavutil/libavutil.a
CFLAGS  = -I$(FF)/src/ffmpeg-7.1 -I$(FFB)/audio-x86_64 -Wno-unused-parameter
LIBS_i686   = $(addprefix $(FFB)/audio-i686/,$(FFLIBS)) $(shell gcc -m32 -print-libgcc-file-name)
LIBS_x86_64 = $(addprefix $(FFB)/audio-x86_64/,$(FFLIBS)) $(shell gcc -m64 -print-libgcc-file-name)

include ../../sdk/banana.mk

$(FFB)/audio-%/libavutil/libavutil.a:
	$(FF)/build.sh $* $(FFB)/audio-$* audio

$(BUILD)/i686/amethyst.o $(BUILD)/x86_64/amethyst.o: $(FFB)/audio-x86_64/libavutil/libavutil.a
$(BUILD)/app-i686: $(FFB)/audio-i686/libavutil/libavutil.a
$(BUILD)/app-x86_64: $(FFB)/audio-x86_64/libavutil/libavutil.a
