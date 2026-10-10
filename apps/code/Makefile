APP         = code
TITLE       = Banana Code
VERSION     = 1.0
TYPE        = gui
DESCRIPTION = Write, build and port apps: a code editor with a C compiler (TinyCC) and extensions
AUTHOR      = Banana OS
SRCS        = main.c editor.c build.c ext.c json.c tcclib.c

# TinyCC (ports/tcc/build.sh): its source for libtcc, and the libraries
# programs are linked with on Banana OS
TCC     = ../../ports/tcc
TCCB    = $(TCC)/build
CFLAGS  = -I$(TCC)/src/banana -Wno-unused-parameter -Wno-sign-compare
LIBS_i686   = $(shell gcc -m32 -print-libgcc-file-name)
LIBS_x86_64 = $(shell gcc -m64 -print-libgcc-file-name)

include ../../sdk/banana.mk

$(TCCB)/x86_64/libbanana.a $(TCCB)/i386/libbanana.a $(TCC)/src/banana/libtcc.c &: $(TCC)/build.sh $(TCC)/config.h $(wildcard ../../sdk/lib/*.c) $(wildcard ../../sdk/include/*.h)
	$(TCC)/build.sh $(TCCB)

$(BUILD)/i686/tcclib.o $(BUILD)/x86_64/tcclib.o $(BUILD)/i686/build.o $(BUILD)/x86_64/build.o: $(TCC)/src/banana/libtcc.c

# what the package holds besides the program: the SDK's headers, TinyCC's
# libraries for that CPU (in /apps/code/tcc), the built-in extensions
SDKINC   = ../../sdk/include
HEADERS  = $(wildcard $(SDKINC)/*.h) $(wildcard $(SDKINC)/sys/*.h)
HDR_ARGS = $(foreach h,$(HEADERS),tcc/include/$(patsubst $(SDKINC)/%,%,$(h))=$(h))
EXTFILES = $(wildcard extensions/*/*)
EXT_ARGS = $(foreach f,$(EXTFILES),$(f)=$(f))
LIB_ARGS_i686   = tcc/i386/libbanana.a=$(TCCB)/i386/libbanana.a tcc/i386/libtcc1.a=$(TCCB)/i386/libtcc1.a
LIB_ARGS_x86_64 = tcc/x86_64/libbanana.a=$(TCCB)/x86_64/libbanana.a tcc/x86_64/libtcc1.a=$(TCCB)/x86_64/libtcc1.a
PKG_DEPS = $(HEADERS) $(EXTFILES) $(TCCB)/x86_64/libbanana.a $(TCCB)/i386/libbanana.a

$(APP).bpk: $(BUILD)/manifest $(BUILD)/app-i686 $(BUILD)/app-x86_64 $(PKG_DEPS)
	$(PYTHON) $(BANANA_SDK)/tools/bpkg pack -o $@ $(BUILD)/manifest $(BUILD)/app-i686 $(BUILD)/app-x86_64 \
	    $(HDR_ARGS) $(EXT_ARGS) $(LIB_ARGS_i686) $(LIB_ARGS_x86_64)
$(APP)-i686.bpk: $(BUILD)/manifest $(BUILD)/app-i686 $(PKG_DEPS)
	$(PYTHON) $(BANANA_SDK)/tools/bpkg pack -o $@ $(BUILD)/manifest $(BUILD)/app-i686 $(HDR_ARGS) $(EXT_ARGS) $(LIB_ARGS_i686)
$(APP)-x86_64.bpk: $(BUILD)/manifest $(BUILD)/app-x86_64 $(PKG_DEPS)
	$(PYTHON) $(BANANA_SDK)/tools/bpkg pack -o $@ $(BUILD)/manifest $(BUILD)/app-x86_64 $(HDR_ARGS) $(EXT_ARGS) $(LIB_ARGS_x86_64)
