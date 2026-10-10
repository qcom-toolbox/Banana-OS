APP         = music
TITLE       = Music
VERSION     = 1.0
TYPE        = gui
DESCRIPTION = Your music library: albums, artists, a queue, an equalizer and a visualizer (after Amethyst)
AUTHOR      = Banana OS
SRCS        = music.c

# FFmpeg's sound-only build, for both CPUs (ports/ffmpeg/build.sh ... audio)
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

$(BUILD)/i686/music.o $(BUILD)/x86_64/music.o: $(FFB)/audio-x86_64/libavutil/libavutil.a
$(BUILD)/app-i686: $(FFB)/audio-i686/libavutil/libavutil.a
$(BUILD)/app-x86_64: $(FFB)/audio-x86_64/libavutil/libavutil.a
