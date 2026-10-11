APP         = store
TITLE       = App Store
VERSION     = 1.0
TYPE        = gui
CATEGORY    = System
DESCRIPTION = Apps from the package repositories: discover, install, update and remove them (the same packages as apt)
AUTHOR      = Banana OS
ICON        = icon.png
SRCS        = store.c

LIBS_i686   = $(shell gcc -m32 -print-libgcc-file-name)
LIBS_x86_64 = $(shell gcc -m64 -print-libgcc-file-name)

include ../../sdk/banana.mk
